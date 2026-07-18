# HARDWARE.md — the target device

This firmware targets the **Home Assistant Voice Preview Edition** ("Voice
PE"), Home Assistant's official ESP32-S3 voice satellite hardware. Nothing
below is invented — it's read off the actual ESPHome config in this repo
(`firmware/esphome/hoshi-voice-pe.yaml`) plus the upstream hardware it's
forked from.

## Core hardware

- **MCU:** ESP32-S3 (`board: esp32-s3-devkitc-1`, `variant: esp32s3`).
- **Flash:** 16 MB (`flash_size: 16MB`).
- **PSRAM:** present, configured octal mode @ 80 MHz (required for the audio
  buffers and TLS heap this firmware needs).
- **Far-field audio front-end:** an XMOS XU316 DSP (ESPHome's `voice_kit`
  component), doing mic array cleanup/AGC before audio ever reaches the
  ESP32. This firmware keeps that chip and its firmware **completely
  unmodified** — see below.
- **DAC:** a TI AIC3204 (`audio_dac: platform: aic3204`) driving the
  speaker through an external Class-D amplifier.
- **Connectivity:** WiFi only (2.4 GHz — this board does not do 5 GHz), USB
  for flashing/serial.
- **Physical controls:** a center button (barge-in/abort), a rotary
  volume dial, a hardware mute switch, and an addressable LED ring (12
  WS2812 LEDs) for status feedback.

## Pin map (from `hoshi-voice-pe.yaml`, upstream-inherited unless noted)

| Signal | Pin | Notes |
|---|---|---|
| Internal I2C (XMOS + DAC) | SDA `GPIO5`, SCL `GPIO6` | 400 kHz |
| XMOS (`voice_kit`) reset | `GPIO4` | must reset+reload the XMOS pipeline every boot |
| I2S output bus (speaker/DAC) | LRCLK `GPIO7`, BCLK `GPIO8` | |
| I2S input bus (mic, from XMOS) | LRCLK `GPIO14`, BCLK `GPIO13` | |
| Mic data in | `GPIO15` | 16 kHz, 32-bit, stereo (XMOS delivers cleaned + reference channel; this firmware downmixes) |
| Speaker data out | `GPIO10` | 48 kHz, external DAC |
| External Class-D amp enable | `GPIO47` | `restore_mode: ALWAYS_OFF` |
| Center button (barge-in) | `GPIO0` | inverted; also the bootloader-entry button |
| Hardware mute switch | `GPIO3` | must hard-gate the mic uplink |
| Volume rotary encoder | `GPIO16` (A), `GPIO18` (B) | |
| LED ring (WS2812, 12 LEDs) | `GPIO21` | |

## What this firmware changes vs. stock

- **Removed:** `voice_assistant` + `api`-as-a-conversation-path — the
  components that bind the device to Home Assistant as a client.
- **Added:** `micro_wake_word` (on-device wake detection) +
  `hoshi_ws_audio` (this repo's custom external component — a bidirectional
  `wss` client that replaces the Home Assistant link with a direct
  connection to a Hoshi-style backend). See
  [`../firmware/esphome/components/hoshi_ws_audio/`](../firmware/esphome/components/hoshi_ws_audio/).
- **Unchanged:** everything about the XMOS audio front-end (`voice_kit`),
  the I2S buses, the DAC, and the LED hardware — this firmware reuses the
  upstream audio stack byte-for-byte where possible. See
  [`DECISIONS.md`](DECISIONS.md) (D1) for why.

## The one real hard-brick risk on this board

The `voice_kit:` block's `firmware:` sub-block can push a new firmware
image to the XMOS chip over I2C. **A version mismatch between what you pin
in the YAML and what's actually installed on your device risks XMOS
corruption.** Before your first flash: either confirm your device's
installed XMOS firmware version matches what's pinned in
`hoshi-voice-pe.yaml`, or comment the `firmware:` sub-block out entirely.
See [`../firmware/RUNBOOK.md`](../firmware/RUNBOOK.md) Phase 1.

Separately: **never** enable Secure Boot, Flash Encryption, or
`DIS_DOWNLOAD_MODE` on the ESP32-S3 itself — that is the one truly
unrecoverable brick path on this hardware, independent of the XMOS. See
[`../firmware/recovery/README.md`](../firmware/recovery/README.md).

## References

- Upstream hardware/firmware: `github.com/esphome/home-assistant-voice-pe`
  (`dev` branch), `home-assistant-voice.yaml`.
- [`../firmware/README.md`](../firmware/README.md) — build recipe.
- [`../firmware/RUNBOOK.md`](../firmware/RUNBOOK.md) — flash sequence,
  gates, and the XMOS version-pinning gotcha in detail.
- [`../firmware/recovery/README.md`](../firmware/recovery/README.md) —
  backup/recovery before you flash anything.
- [`ARCHITECTURE.md`](ARCHITECTURE.md) — the firmware-side concurrency
  design running on top of this hardware.
