# Test Notes

## 2026-06-18 Voice Keyboard Bring-up

Hardware:

- Seeed Studio XIAO nRF52840 Sense
- Onboard PDM microphone

Firmware:

- `firmware/PsyGuardVoiceKeyboard/PsyGuardVoiceKeyboard.ino`
- BLE local name set to `PsyGuard-XIAO`
- PDM gain set to `60`
- 16 kHz capture down-sampled to 8 kHz PCM over Nordic UART TX

Voice Keyboard configuration:

- `audio.device: xiao_ble`
- STT provider: `glm_asr_2512`

Observed results:

- Very close speaking can clip.
- 20-40 cm works well with no clipping.
- 30-60 cm is the practical sweet spot.
- Around 1 m works in a quiet room, but signal is near the weak edge and may skip or lose stability.

Representative logs from Voice Keyboard:

```text
rms=2675 max=27466 silent=26.9% clipped=0.000% -> 你好你好，能听得到我说话吗？
rms=310  max=1931  silent=74.5% clipped=0.000% -> 现在离了大概有20到30厘米。
rms=125  max=839   silent=91.7% clipped=0.000% -> 现在大概是一米的距离，看看效果怎么样？
```
