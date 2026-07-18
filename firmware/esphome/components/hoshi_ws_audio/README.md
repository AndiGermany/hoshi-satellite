# hoshi_ws_audio — ESPHome external component

A bidirectional **wss** client that turns the HA Voice PE into a direct
[`/ws/audio`](../../../../docs/PROTOCOL.md) client for a Hoshi backend —
replacing ESPHome's `voice_assistant` + `api` (which bind the device to Home
Assistant).

> **Honest status:** this component builds and runs against the reference
> deployment it was written for (5 crash/hang bugs found and fixed on real
> hardware — see [`../../../../docs/DECISIONS.md`](../../../../docs/DECISIONS.md) and
> [`../../../../docs/ARCHITECTURE.md`](../../../../docs/ARCHITECTURE.md)). It has **not**
> been verified against a second, independent Hoshi backend or a second
> physical device. Treat config defaults (VAD thresholds, gain, timings) as a
> starting point to tune against your own room and hardware, not gospel.

## What it does

On-device flow implemented in C++ (`hoshi_ws_audio.cpp`):

1. **wss connect** via esp-idf `esp_websocket_client` with an embedded TLS
   leaf certificate (TLS leaf-pinning — see [Build / use](#build--use) below,
   **you must swap in your own server's certificate**, contract §A of
   [`docs/PROTOCOL.md`](../../../../docs/PROTOCOL.md)).
2. On **`micro_wake_word`** trigger → `start_turn()` opens a turn and sends
   `{type:"start", mimeType:"audio/wav", turnId, room?, satelliteId?}`.
3. **Mic PCM16/16k accumulation**: the XMOS-cleaned mic (32-bit stereo) is
   downmixed to PCM16 mono (channel 0, top 16 bits) and buffered for the
   whole utterance — *not* streamed raw (the reference backend's STT rejects
   raw PCM on the binary uplink; it maps `mimeType` → file extension and only
   accepts `webm/ogg/wav/mp4`, transcribing the buffered binary once on
   `{stop}`).
4. **On-device endpointing** via a simple **energy/RMS silence VAD** with
   hysteresis + a wall-clock cap (config: `vad_rms_threshold` / `silence_ms` /
   `min_speech_ms` / `max_turn_ms`). On end-of-speech: build a canonical
   44-byte-header WAV from the buffered PCM → send it in ≤16 KB binary frames
   → `{type:"stop"}`.
5. **Downlink**: parse the JSON frames (`transcript`, `llm_audio{base64-WAV-24k}`,
   `tts_audio_start/end`, `llm_done`, `llm_error`, `no_input`, `turn_aborted`, …),
   base64-decode the return audio, resample 24k→16k, and `speaker->play(...)`
   (the YAML resampler then upsamples 16k→48k for the i2s sink).
6. **Half-duplex**: mic capture is muted while the speaker is active
   (`mic_should_capture_()`), plus a hard gate on the physical mute switch
   (mute mid-listen aborts the turn).
7. **Local turn lifecycle**: `IDLE → LISTENING → AWAITING_STT → THINKING →
   SPEAKING → IDLE`. There is no Home Assistant in this path, so what would be
   `VoiceAssistant` `RUN_START..RUN_END` events on a stock device become
   **local state transitions** (they drive the LED ring from the YAML on the
   same wake/state hooks).

### Why energy VAD instead of a neural VAD

A companion Python test bridge in [`tools/bridge/`](../../../../tools/bridge/) uses
**Silero ONNX** for endpointing (onnxruntime + a ~1.3 MB model + per-frame
float matmul). That is not feasible inside the ESP32-S3 audio hot path, so
the firmware substitutes a **simple RMS-energy silence detector**: per mic
block it computes RMS, runs a start-guard (`min_speech_ms`), then counts
trailing silence below a hysteresis low watermark until `silence_ms` ends the
turn; `max_turn_ms` is an absolute wall-clock safety net from wake. This is a
**deliberate downgrade** vs. a neural VAD (worse in TV/noise) and is the
single most important thing to tune on real hardware. All VAD thresholds are
config keys.

## Why a custom component

ESPHome ships `voice_assistant` (HA-bound) + `api`, but no native generic
WebSocket client, so this project writes one. The WS path is locked to
esp-idf [`esp_websocket_client`](https://components.espressif.com/component/espressif/esp_websocket_client)
(managed IDF component `espressif/esp_websocket_client`, pinned `^1.7.0`),
pulled via `esp32.add_idf_component` in `__init__.py` (and mirrored in
`hoshi-voice-pe.yaml`'s `framework: components:`). Its esp-tls `cert_pem`
field is exactly the leaf-pin slot — that is the whole reason this client was
chosen over hand-rolling on `esphome::socket`.

Precedent for a custom socket/streaming audio component alongside the i2s
speaker stack: `sendspin` (upstream Voice-PE `media_player`) and
`gnumpi/esphome_audio` (duplex I2S + custom transports).

## Contract map (what this component does)

Authority: [`docs/PROTOCOL.md`](../../../../docs/PROTOCOL.md).

| Direction | Frame | Notes |
|---|---|---|
| → out | `{type:"start", mimeType:"audio/wav", turnId}` | optional `room/satelliteId` |
| → out | binary **WAV** (44-byte header + PCM16/16k/mono), in ≤16 KB frames | ONE blob per turn, sent at end-of-speech (`send_bin`) |
| → out | `{type:"stop"}` | flush → STT → LLM → TTS |
| → out | `{type:"abort", turnId}` | barge-in (center button GPIO0) — uplink only |
| ← in | `transcript{text}`, `no_input`, `transcribing_started`, `llm_thinking`, `llm_start`, `llm_delta` | state/LED feedback |
| ← in | `llm_audio{seq, data:<base64 WAV PCM16/24k/mono>}` (JSON text frame, **not** binary) | decode → resample 24k→16k → play |
| ← in | `tts_audio_start`, `tts_audio_end`, `llm_done{ttsHandled}` | playback lifecycle / half-duplex gate |
| ← in | `llm_error{stage,message}`, `sidecar_alarm{...}`, `turn_aborted{turnId}` | error / abort-ack → back to IDLE |
| ← in | `night_mode{active,dim}`, `timer_state{remainingS,totalS}`, `speaker{speakerId}` | optional extensions — LED dimming, timer arc, speaker-accent color |

> `turnId` is echoed on outbound events → the client drops frames whose
> `turnId` doesn't match the current turn (stale-turn guard after a barge-in).
> `abort` is never received — it is a client→server frame; `turn_aborted` is
> the server's ACK of an abort.

- **Auth**: `?token=<your-api-token>` as a query param on the WS upgrade
  (the reference backend reads the token only from the query, not from an
  `Authorization` header — check your own backend's handshake). The token is
  never logged (`connect_` logs the token-free path only). Set it via
  `!secret` in `secrets.yaml`, never commit it.
- **TLS**: leaf-pinning (Trust-On-First-Use). The self-signed leaf **is its
  own trust anchor**, embedded as `SERVER_LEAF_PEM` in `hoshi_ws_audio.cpp` —
  **replace the placeholder with your own server's leaf certificate** before
  building (see the comment right above it). Plain `ws://` is intentionally
  not supported: audio should never travel unencrypted on the LAN.
- **Half-duplex v1**: acoustic echo cancellation is not available on this
  board (no loopback reference channel reaches the wake/VAD path) → mute mic
  while the speaker is active, plus a hard gate on the physical mute switch.

## Config keys (`__init__.py`)

`microphone`, `speaker`, `host/port/path`, `auth_token/auth_mode`,
`cacert_pem` (override only — default = embedded leaf),
`uplink_sample_rate` (16000), `return_sample_rate` (24000), `half_duplex`,
`mic_gain` (uplink/STT amplification, clip-safe in the `on_mic_data_`
downmix — the wake path has its own separate gain), `room` / `satellite_id`
(optional device identity sent in the start frame), and the energy-VAD knobs
`vad_rms_threshold` / `silence_ms` / `min_speech_ms` / `max_turn_ms`. Plus
`esp32.add_idf_component("espressif/esp_websocket_client", "^1.7.0")`.

## Files

- `__init__.py` — config schema + codegen.
- `hoshi_ws_audio.h` — class, turn state machine, energy-VAD + WAV-uplink +
  downlink reassembly members, embedded leaf-cert placeholder, opaque `ws_`
  handle.
- `hoshi_ws_audio.cpp` — connect/event handler, mic downmix+accumulate,
  energy VAD, WAV builder (44-byte header), chunked `send_bin`, JSON frame
  parse (tiny in-house extractor — no ArduinoJson under esp-idf), base64
  decode, 24k→16k linear resample, speaker play, half-duplex gate, LED state
  machine.

## Build / use

```bash
# from firmware/esphome/  (validate only — does NOT flash)
esphome config hoshi-voice-pe.yaml
# first flash over USB-C (recovery rehearsed first — see ../../RUNBOOK.md Phase 0)
esphome run hoshi-voice-pe.yaml
# later: OTA over WiFi (2.4 GHz ONLY)
```

Before any of this compiles/works against your own backend:

1. Copy `secrets.yaml.example` → `secrets.yaml`, fill in your WiFi + API
   token. Never commit `secrets.yaml`.
2. Replace `SERVER_LEAF_PEM` in `hoshi_ws_audio.cpp` with your own server's
   leaf certificate (see the comment above the placeholder).
3. Set `hoshi_host` / `hoshi_port` / `hoshi_satellite_id` / `hoshi_room` in
   the `substitutions:` block of `hoshi-voice-pe.yaml` to your own values.

The component is referenced from `hoshi-voice-pe.yaml` via
`external_components: { source: { type: local, path: components }, components: [hoshi_ws_audio] }`
and configured under the top-level `hoshi_ws_audio:` key.
