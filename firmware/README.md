# firmware/ — HA Voice PE custom ESPHome firmware

Custom ESPHome firmware that turns a Home Assistant Voice Preview Edition
into a **direct** Hoshi client: no Home Assistant in the path, wake word and
audio streaming go straight to your own Hoshi server over `wss://.../ws/audio`.

## Why flash instead of using the device through Home Assistant

The HA Voice PE is a great piece of hardware (ESP32-S3 + XMOS XU316 far-field
audio front-end) built around Home Assistant's `voice_assistant` + `api`
components. Those bind the device to HA as the conversation agent. This
firmware removes that layer and replaces it with a small custom ESPHome
external component (`components/hoshi_ws_audio/`) that speaks Hoshi's own
WebSocket protocol directly — see [`../docs/PROTOCOL.md`](../docs/PROTOCOL.md)
for the exact wire spec, and
[`../docs/DECISIONS.md`](../docs/DECISIONS.md) for why this route was chosen
over a no-flash (ESPHome Native API bridge) alternative.

## Target picture

```
[ Voice PE — custom ESPHome firmware ]              [ your Hoshi server ]
  XMOS XU316 (voice_kit, UNCHANGED)                    wss://<host>:<port>/ws/audio
   -> cleaned mono stream via I2S                         -> STT
  micro_wake_word ("okay_nabu" stock model,   --wss-->    -> LLM
   on-device)                                             -> TTS
  microphone.on_data -> custom WS client      <-- WAV PCM16/24k/mono (llm_audio)
  speaker.play <---------------------------------------------┘
  Half-duplex (no AEC on this board): mic muted during playback
```

## Build recipe, phase by phase

### Phase 0 — Recovery first (before any flash)

1. Back up the current `factory.bin` + checksum for your installed HA Voice
   PE firmware version from
   [`esphome/home-assistant-voice-pe/releases`](https://github.com/esphome/home-assistant-voice-pe/releases),
   and take your own full flash dump. See [`recovery/README.md`](recovery/README.md).
2. Rehearse the recovery path once: power off → hold the center button
   (GPIO0) → plug in USB-C → hold briefly. Use a USB **data** cable (not
   charge-only); if the device isn't detected, check the internal USB-select
   switch.
3. **Never** enable Secure Boot / Flash Encryption / `DIS_DOWNLOAD_MODE` — the
   one real hard-brick path on this board.

### Phase 1 — Custom firmware (prepared offline)

4. This repo's `esphome/hoshi-voice-pe.yaml` is forked conceptually from
   `esphome/home-assistant-voice-pe` (branch `dev`) — the upstream audio
   stack (voice_kit/XMOS, dual-I2S, mic, speaker, LED ring) is kept
   byte-for-byte where possible; only `voice_assistant` + `api` are removed.
5. **Leave `voice_kit` (XMOS) unchanged.** Either omit the `firmware:` block
   entirely or pin it *exactly* to your device's installed XMOS version —
   a mismatch triggers an XMOS I2C DFU (real corruption risk). The ESP32
   resets and re-initializes the XMOS chip over I2C on every boot;
   `voice_kit` does that, keep it.
6. `voice_assistant` + `api` are removed (they bind to Home Assistant).
   Instead: `micro_wake_word` (on-device wake, stock `okay_nabu` model by
   default), `microphone` (`i2s_audio`, feeds the custom component), and the
   custom `hoshi_ws_audio` external component (bidirectional WS client to
   your Hoshi server's `/ws/audio`) plus `speaker` (`i2s_audio`, plays the
   returned audio).
7. **Half-duplex** is load-bearing: mic capture pauses while the speaker
   plays (there's no acoustic echo cancellation on this board). Interrupt via
   the center button, which sends `{type:"abort", turnId}`.
8. **TLS**: `wss` to your self-signed server needs a trust decision — this
   firmware uses Trust-On-First-Use leaf-pinning (embed your server's leaf
   cert directly in the component). See
   [`esphome/components/hoshi_ws_audio/README.md`](esphome/components/hoshi_ws_audio/README.md).

### Phase 2 — First flash + pilot

9. `esphome run hoshi-voice-pe.yaml` over USB-C for the first flash; OTA over
   WiFi afterwards (**2.4 GHz only**, no 5 GHz).
10. Measure: wake → first audible response (10 turns, median + P95), false
    wake rate, speech-recognition quality in your own language. See
    [`../docs/MEASUREMENTS.md`](../docs/MEASUREMENTS.md).

## Every hardware/flash step needs your hands

Putting the device into flash mode, the actual `esphome run`/OTA, provisioning
secrets onto the device, and the first live test against your real backend
are all steps that need you physically at the hardware — none of that is
something to automate blindly. See [`RUNBOOK.md`](RUNBOOK.md) for the exact,
gated sequence with all the hard-won gotchas.

## Layout

```
firmware/
  README.md            <- you are here (build recipe, phases)
  RUNBOOK.md            <- exact flash sequence + the 5 solved bugs + gotchas
  recovery/README.md    <- stock-firmware backup + recovery procedure
  esphome/
    hoshi-voice-pe.yaml       <- the firmware config
    secrets.yaml.example      <- copy to secrets.yaml, fill in your values
    components/hoshi_ws_audio/  <- the custom WS-audio component (C++/Python)
```

## References

- [`../docs/PROTOCOL.md`](../docs/PROTOCOL.md) — exact `/ws/audio` wire spec
- [`../docs/ARCHITECTURE.md`](../docs/ARCHITECTURE.md) — the 4-context
  concurrency redesign that made the firmware stable
- [`../docs/DECISIONS.md`](../docs/DECISIONS.md) — why flashing was chosen
  over a no-flash bridge
- Upstream: `github.com/esphome/home-assistant-voice-pe` (`dev` branch),
  `home-assistant-voice.yaml`
