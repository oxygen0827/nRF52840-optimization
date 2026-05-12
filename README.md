# nRF52840 Microphone Optimization Lab

这个目录是一套可复用的 XIAO nRF52840 Sense 收音优化与测试工具。

目标不是一次性做出最终产品结构，而是在只有一块 nRF52840 开发板的情况下，把硬件链路调到尽量可靠、可复测、可比较：

- 优化版 PDM + BLE 固件
- Web Bluetooth 测试页面
- WAV 原录音保存
- 可配置 ASR Endpoint 的转录精度测试
- RMS / Peak / BLE 吞吐 / 丢包 / ring overflow 指标

## 目录

```text
firmware/PdmBleAudioOptimized/PdmBleAudioOptimized.ino
web/index.html
```

## 固件功能

硬件：Seeed Studio XIAO nRF52840 Sense。

Arduino IDE 选择：

- Board: `Seeed XIAO BLE Sense - nRF52840`
- Boards package: `Seeed nRF52 mbed-enabled Boards`

BLE UUID：

| UUID 后缀 | 方向 | 内容 |
| --- | --- | --- |
| `...0001` | service | Nordic UART compatible service |
| `...0003` | device -> web/app | PCM 16 kHz, 16-bit, mono, little-endian |
| `...0002` | web/app -> device | control command |
| `...0004` | device -> web/app | JSON metrics |

兼容旧控制命令：

- `0x01` start
- `0x00` stop

新增文本命令：

```text
START
STOP
GAIN=35
DGAIN=1.80
AGC=1
TARGET=2400
MAXGAIN=8
DC=1
GATE=0
GATETH=80
PROC=1
METRICS
```

## Web 测试页

打开 `web/index.html`。

注意：Web Bluetooth 需要 Chrome / Edge，并且页面需要在 HTTPS 或 localhost 环境运行。最简单方式：

```bash
cd web
python -m http.server 8000
```

然后打开：

```text
http://localhost:8000
```

页面能力：

- 连接 `nRF52840-MicLab`
- 调 PDM gain、digital gain、AGC target
- 开关前处理、DC blocker、AGC、noise gate
- 查看实时 RMS、Peak、BLE bytes/sec、drop、overflow
- 保存原始 WAV
- 配置 ASR Endpoint 后上传 WAV 转录
- 填入标准文本后计算粗略相似度

## 推荐测试流程

每次改硬件或固件后，固定同一段文本，录三组：

1. 贴近嘴边 3-5 cm，低声说
2. 贴近嘴边 3-5 cm，正常说
3. 胸前佩戴距离，正常说

每组都记录：

- 固件参数：PDM gain / digital gain / AGC target / AGC on/off
- RMS
- Peak
- BLE B/s
- drop / overflow
- 保存的 WAV
- ASR 转录文本和相似度

经验指标：

- BLE B/s 应接近 `32000`，明显低于这个说明传输链路可能丢音频
- drop 和 overflow 应长期为 `0`
- 正常说话 Peak 建议落在 `8000-20000`
- Peak 经常接近 `32767` 说明增益过大或 limiter 压得太重
- 低声说话 RMS 如果长期低于 `200`，ASR 通常会不稳定

## ASR Endpoint 约定

Web 页面支持两种请求：

1. `multipart/form-data`

字段名：

```text
file=audio.wav
```

返回可以是纯文本，也可以是 JSON：

```json
{"text":"转录结果"}
```

2. raw WAV body

`Content-Type: audio/wav`

返回同上。

可以先用你们现有 voice-keyboard 的 STTClient 包一个本地 HTTP 服务，或者接任意云端 ASR 代理。

## 调参建议

初始参数：

```text
GAIN=35
DGAIN=1.80
AGC=1
TARGET=2400
DC=1
GATE=0
PROC=1
```

如果声音太小：

- 先增加 `GAIN` 到 40 或 45
- 再增加 `DGAIN`
- 最后再提高 `TARGET`

如果声音爆裂或失真：

- 先降低 `GAIN`
- 再降低 `DGAIN`
- 看 Peak 是否仍然频繁接近 32767

如果安静环境底噪明显：

- 保持 `DC=1`
- 尝试 `GATE=1`
- 逐步调整 `GATETH=80` 到 `150-300`

如果 VAD/ASR 会吞字：

- 优先检查 BLE B/s、drop、overflow
- 再检查 WAV 是否有断续
- 最后再调 gain/AGC

## 设计取舍

这份固件仍然使用 XIAO Sense 板载 PDM 麦克风。它不能替代最终产品的近讲麦克风和声学结构，但它能让当前开发板达到比较稳定的可测试状态。

真正要接近 Insta360 / DJI 这类无线麦体验，后续仍建议换成更好的近讲麦克风与结构。但在现阶段，这套工具能帮助团队用数据而不是体感判断每次硬件优化是否真的变好了。
