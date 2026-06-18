# PsyGuardVoiceKeyboard

Voice Keyboard-compatible XIAO nRF52840 Sense firmware.

This sketch is the currently tested BLE Capture Path firmware for the
Voice Keyboard Engine. It keeps the same Nordic UART UUIDs used by PsyGuard,
captures the XIAO Sense onboard PDM microphone at 16 kHz, down-samples to
8 kHz for BLE throughput, and sends 16-bit little-endian mono PCM over TX
notifications.

## Current Tested Settings

- Board: `Seeed XIAO BLE Sense - nRF52840`
- Arduino board package: `Seeed nRF52 mbed-enabled Boards`
- BLE local name: `PsyGuard-XIAO`
- Control command: `0x01` starts recording, `0x00` stops recording
- PDM gain: `60`
- Output audio: 8 kHz, 16-bit PCM mono

## Voice Keyboard Notes

Voice Keyboard currently uses `audio.device: xiao_ble` and upsamples this
firmware's 8 kHz BLE stream to 16 kHz before sending it to STT.

The best observed speaking distance is roughly 30-60 cm in a quiet room. One
meter can work, but the signal is near the lower edge and may be less stable.
