/*
  PdmBleAudioOptimized - Seeed Studio XIAO nRF52840 Sense

  Optimized reusable BLE microphone firmware for speech-input prototypes.

  Audio:
    - PDM microphone, 16 kHz, 16-bit PCM, mono
    - Ring buffer between PDM ISR and BLE sender
    - Optional DC blocker, noise gate, soft-knee AGC, and limiter
    - Runtime gain / processing control over BLE

  BLE:
    - Nordic UART compatible service
    - TX 6E400003... notifies raw PCM audio, 244-byte chunks
    - RX 6E400002... accepts control commands
    - METRICS 6E400004... notifies JSON stats every second

  Board package:
    Seeed nRF52 mbed-enabled Boards -> Seeed XIAO BLE Sense - nRF52840
*/

#include <ArduinoBLE.h>
#include <PDM.h>
#include <math.h>

#define NUS_SERVICE_UUID   "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_TX_UUID        "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_UUID        "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_METRICS_UUID   "6E400004-B5A3-F393-E0A9-E50E24DCCA9E"

static const uint32_t SAMPLE_RATE = 16000;
static const uint16_t BLE_CHUNK_BYTES = 244;
static const uint16_t PDM_SAMPLES = 256;
static const uint16_t RING_SAMPLES = 4096;  // 256 ms at 16 kHz.

BLEService nusService(NUS_SERVICE_UUID);
BLECharacteristic txChar(NUS_TX_UUID, BLERead | BLENotify, BLE_CHUNK_BYTES);
BLECharacteristic rxChar(NUS_RX_UUID, BLERead | BLEWrite, 64);
BLECharacteristic metricsChar(NUS_METRICS_UUID, BLERead | BLENotify, 180);

short pdmBuffer[PDM_SAMPLES];
volatile uint16_t pdmSamplesRead = 0;
volatile bool pdmOverflow = false;

int16_t ringBuffer[RING_SAMPLES];
volatile uint16_t ringWrite = 0;
volatile uint16_t ringRead = 0;
volatile uint32_t ringOverflowCount = 0;

volatile bool isRecording = false;
bool metricsEnabled = true;
bool processingEnabled = true;
bool dcBlockEnabled = true;
bool noiseGateEnabled = false;
bool agcEnabled = true;

uint8_t pdmGain = 35;        // Seeed PDM gain, runtime tunable.
float digitalGain = 1.8f;    // Post-PDM multiplier before limiter.
float agcTargetRms = 2400.0f;
float agcMaxGain = 8.0f;
int16_t noiseGateThreshold = 80;

float dcPrevInput = 0.0f;
float dcPrevOutput = 0.0f;
float agcGain = 1.0f;

uint32_t bytesSent = 0;
uint32_t packetsSent = 0;
uint32_t droppedPackets = 0;
uint32_t samplesProcessed = 0;
uint32_t clippedSamples = 0;
uint32_t silentGateSamples = 0;
int16_t peakAbs = 0;
double sumSquares = 0.0;

uint32_t lastMetricsMs = 0;
uint32_t lastBytesSent = 0;
uint32_t lastSamplesProcessed = 0;
uint32_t lastDroppedPackets = 0;
uint32_t lastRingOverflow = 0;

uint8_t bleChunk[BLE_CHUNK_BYTES];
uint16_t bleChunkLen = 0;

static int16_t clamp16(float v) {
  if (v > 32767.0f) return 32767;
  if (v < -32768.0f) return -32768;
  return (int16_t)v;
}

static void recordOutputMetrics(int16_t sample) {
  int16_t absOut = sample == -32768 ? 32767 : abs(sample);
  if (absOut > peakAbs) peakAbs = absOut;
  if (absOut >= 32760) clippedSamples++;
  sumSquares += (double)sample * (double)sample;
  samplesProcessed++;
}

static int16_t processSample(int16_t raw) {
  float x = (float)raw;

  if (dcBlockEnabled) {
    // First-order DC blocker: y[n] = x[n] - x[n-1] + R*y[n-1].
    const float R = 0.995f;
    float y = x - dcPrevInput + R * dcPrevOutput;
    dcPrevInput = x;
    dcPrevOutput = y;
    x = y;
  }

  if (noiseGateEnabled && fabsf(x) < noiseGateThreshold) {
    x = 0.0f;
    silentGateSamples++;
  }

  x *= digitalGain;

  if (agcEnabled) {
    // Lightweight envelope follower. It raises quiet speech and releases fast
    // enough to avoid staying over-amplified after loud syllables.
    float absx = fabsf(x);
    float desired = agcTargetRms / max(absx, 200.0f);
    desired = constrain(desired, 0.65f, agcMaxGain);
    const float attack = 0.0007f;
    const float release = 0.00012f;
    float coeff = desired < agcGain ? attack : release;
    agcGain += (desired - agcGain) * coeff;
    agcGain = constrain(agcGain, 0.65f, agcMaxGain);
    x *= agcGain;
  }

  // Soft limiter before hard clamp. This is intentionally cheap and stable.
  const float limit = 26000.0f;
  float ax = fabsf(x);
  if (ax > limit) {
    float sign = x < 0 ? -1.0f : 1.0f;
    x = sign * (limit + (32767.0f - limit) * (1.0f - expf(-(ax - limit) / 6000.0f)));
  }

  int16_t out = clamp16(x);
  recordOutputMetrics(out);
  return out;
}

static inline uint16_t ringNext(uint16_t idx) {
  return (idx + 1) % RING_SAMPLES;
}

static bool ringPush(int16_t sample) {
  uint16_t next = ringNext(ringWrite);
  if (next == ringRead) {
    ringOverflowCount++;
    return false;
  }
  ringBuffer[ringWrite] = sample;
  ringWrite = next;
  return true;
}

static bool ringPop(int16_t* sample) {
  if (ringRead == ringWrite) return false;
  *sample = ringBuffer[ringRead];
  ringRead = ringNext(ringRead);
  return true;
}

static void resetAudioState() {
  noInterrupts();
  ringRead = 0;
  ringWrite = 0;
  pdmSamplesRead = 0;
  pdmOverflow = false;
  ringOverflowCount = 0;
  interrupts();

  bleChunkLen = 0;
  dcPrevInput = 0.0f;
  dcPrevOutput = 0.0f;
  agcGain = 1.0f;
  bytesSent = 0;
  packetsSent = 0;
  droppedPackets = 0;
  samplesProcessed = 0;
  clippedSamples = 0;
  silentGateSamples = 0;
  peakAbs = 0;
  sumSquares = 0.0;
  lastBytesSent = 0;
  lastSamplesProcessed = 0;
  lastDroppedPackets = 0;
  lastRingOverflow = 0;
}

void onPDMdata() {
  int bytesAvailable = PDM.available();
  int toRead = min(bytesAvailable, (int)sizeof(pdmBuffer));
  PDM.read(pdmBuffer, toRead);
  pdmSamplesRead = toRead / 2;

  if (!isRecording) return;

  for (uint16_t i = 0; i < pdmSamplesRead; i++) {
    // Keep the PDM callback short: only move raw samples into the ring.
    if (!ringPush(pdmBuffer[i])) {
      pdmOverflow = true;
    }
  }
}

static void flushBleChunk() {
  if (bleChunkLen == 0) return;
  if (txChar.writeValue(bleChunk, bleChunkLen)) {
    bytesSent += bleChunkLen;
    packetsSent++;
  } else {
    droppedPackets++;
  }
  bleChunkLen = 0;
}

static void sendAvailableAudio() {
  int16_t sample;
  while (ringPop(&sample)) {
    int16_t out = processingEnabled ? processSample(sample) : sample;
    if (!processingEnabled) {
      recordOutputMetrics(out);
    }
    bleChunk[bleChunkLen++] = (uint8_t)(out & 0xFF);
    bleChunk[bleChunkLen++] = (uint8_t)((out >> 8) & 0xFF);
    if (bleChunkLen >= BLE_CHUNK_BYTES) {
      flushBleChunk();
    }
  }
}

static void sendMetrics(bool force = false) {
  if (!metricsEnabled || (!metricsChar.subscribed() && !force)) return;

  uint32_t now = millis();
  if (!force && now - lastMetricsMs < 1000) return;
  uint32_t elapsed = max((uint32_t)1, now - lastMetricsMs);
  lastMetricsMs = now;

  uint32_t sampleDelta = samplesProcessed - lastSamplesProcessed;
  uint32_t byteDelta = bytesSent - lastBytesSent;
  uint32_t dropDelta = droppedPackets - lastDroppedPackets;
  uint32_t overflowDelta = ringOverflowCount - lastRingOverflow;

  float rms = 0.0f;
  if (sampleDelta > 0) {
    rms = sqrt(sumSquares / (double)sampleDelta);
  }

  char json[180];
  snprintf(json, sizeof(json),
           "{\"sr\":16000,\"gain\":%u,\"dg\":%.2f,\"agc\":%d,\"ag\":%.2f,"
           "\"rms\":%.0f,\"peak\":%d,\"clip\":%lu,\"gate\":%lu,"
           "\"bps\":%lu,\"drop\":%lu,\"ovf\":%lu}",
           pdmGain, digitalGain, agcEnabled ? 1 : 0, agcGain,
           rms, peakAbs, (unsigned long)clippedSamples,
           (unsigned long)silentGateSamples,
           (unsigned long)(byteDelta * 1000UL / elapsed),
           (unsigned long)dropDelta, (unsigned long)overflowDelta);

  metricsChar.writeValue((const uint8_t*)json, strlen(json));

  lastBytesSent = bytesSent;
  lastSamplesProcessed = samplesProcessed;
  lastDroppedPackets = droppedPackets;
  lastRingOverflow = ringOverflowCount;
  sumSquares = 0.0;
  peakAbs = 0;
  clippedSamples = 0;
  silentGateSamples = 0;
}

static void setRecording(bool enabled) {
  if (enabled == isRecording) return;
  if (enabled) {
    resetAudioState();
    lastMetricsMs = millis();
    isRecording = true;
  } else {
    isRecording = false;
    sendAvailableAudio();
    flushBleChunk();
  }
  digitalWrite(LED_RED, isRecording ? LOW : HIGH);
}

static void handleTextCommand(String cmd) {
  cmd.trim();
  cmd.toUpperCase();

  if (cmd == "START") {
    setRecording(true);
  } else if (cmd == "STOP") {
    setRecording(false);
  } else if (cmd.startsWith("GAIN=")) {
    uint8_t value = constrain(cmd.substring(5).toInt(), 0, 80);
    pdmGain = value;
    PDM.setGain(pdmGain);
  } else if (cmd.startsWith("DGAIN=")) {
    float value = cmd.substring(6).toFloat();
    digitalGain = constrain(value, 0.25f, 12.0f);
  } else if (cmd.startsWith("AGC=")) {
    agcEnabled = cmd.substring(4).toInt() != 0;
  } else if (cmd.startsWith("TARGET=")) {
    float value = cmd.substring(7).toFloat();
    agcTargetRms = constrain(value, 400.0f, 8000.0f);
  } else if (cmd.startsWith("MAXGAIN=")) {
    float value = cmd.substring(8).toFloat();
    agcMaxGain = constrain(value, 1.0f, 16.0f);
  } else if (cmd.startsWith("DC=")) {
    dcBlockEnabled = cmd.substring(3).toInt() != 0;
  } else if (cmd.startsWith("GATE=")) {
    noiseGateEnabled = cmd.substring(5).toInt() != 0;
  } else if (cmd.startsWith("GATETH=")) {
    noiseGateThreshold = constrain(cmd.substring(7).toInt(), 0, 2000);
  } else if (cmd.startsWith("PROC=")) {
    processingEnabled = cmd.substring(5).toInt() != 0;
  } else if (cmd == "METRICS") {
    sendMetrics(true);
  }
}

static void handleRxCommand() {
  int len = rxChar.valueLength();
  if (len <= 0) return;

  uint8_t buf[64];
  rxChar.readValue(buf, min(len, (int)sizeof(buf)));

  if (len == 1) {
    if (buf[0] == 0x01) {
      setRecording(true);
      return;
    }
    if (buf[0] == 0x00) {
      setRecording(false);
      return;
    }
  }

  String cmd;
  for (int i = 0; i < len && i < (int)sizeof(buf); i++) {
    if (buf[i] == 0) break;
    cmd += (char)buf[i];
  }
  handleTextCommand(cmd);
}

void setup() {
  Serial.begin(115200);

  pinMode(LED_RED, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_BLUE, OUTPUT);
  digitalWrite(LED_RED, HIGH);
  digitalWrite(LED_GREEN, HIGH);
  digitalWrite(LED_BLUE, HIGH);

  if (!BLE.begin()) {
    Serial.println("BLE init failed");
    while (1) yield();
  }

  BLE.setLocalName("nRF52840-MicLab");
  BLE.setDeviceName("nRF52840-MicLab");
  BLE.setAdvertisedService(nusService);
  nusService.addCharacteristic(txChar);
  nusService.addCharacteristic(rxChar);
  nusService.addCharacteristic(metricsChar);
  BLE.addService(nusService);
  rxChar.writeValue((byte)0);
  BLE.advertise();
  digitalWrite(LED_BLUE, LOW);

  PDM.onReceive(onPDMdata);
  PDM.setGain(pdmGain);
  if (!PDM.begin(1, SAMPLE_RATE)) {
    Serial.println("PDM init failed");
    while (1) yield();
  }

  Serial.println("nRF52840-MicLab ready");
}

void loop() {
  BLEDevice central = BLE.central();

  if (central) {
    Serial.print("Connected: ");
    Serial.println(central.address());
    digitalWrite(LED_BLUE, HIGH);
    digitalWrite(LED_GREEN, LOW);
    lastMetricsMs = millis();

    while (central.connected()) {
      BLE.poll();

      if (rxChar.written()) {
        handleRxCommand();
      }

      if (isRecording) {
        sendAvailableAudio();
      }

      sendMetrics();
    }

    setRecording(false);
    resetAudioState();
    Serial.println("Disconnected");
    digitalWrite(LED_GREEN, HIGH);
    digitalWrite(LED_RED, HIGH);
    digitalWrite(LED_BLUE, LOW);
  }
}
