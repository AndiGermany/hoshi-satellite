# No-flash test bridge — HA Voice PE <-> Hoshi (`napi_bridge.py`)

> **Status: unverified end-to-end.** Syntax, audio helpers, and API call
> shapes are checked against `aioesphomeapi==45.3.1`. A full round trip
> against a real device has not been exercised as part of this public
> release — treat this as a well-researched starting point, not a proven
> path. See [`../../docs/PROTOCOL.md`](../../docs/PROTOCOL.md) for what
> *is* proven (the wire protocol to your Hoshi backend itself).

This is the **alternative to flashing custom firmware**: it lets a
**stock**, unmodified HA Voice PE (no flash, no Home Assistant needed at
runtime) talk to a Hoshi-style backend by acting as an ESPHome Native API
client — the role Home Assistant normally plays. Stock wake word ("Okay
Nabu") wakes the device -> device mic audio streams to the bridge over the
ESPHome Native API -> bridge relays it to your backend's `/ws/audio` ->
returned audio streams back to the device's speaker.

```
 HA Voice PE (stock firmware)                          your Hoshi backend
   "Okay Nabu" wake (on-device)         ESPHome Native API :6053
   Mic PCM16/16k  --VoiceAssistantAudio-->  napi_bridge --wss /ws/audio--> STT->LLM->TTS
   Speaker        <--send_voice_assistant_audio-- (24k->16k)  <-- llm_audio (WAV PCM16/24k)
```

If you'd rather not flash anything, this is the lower-risk path — at the
cost of an extra process in the loop and being bound to ESPHome's own wire
protocol on the device side instead of talking your backend's protocol
directly. See [`../../docs/DECISIONS.md`](../../docs/DECISIONS.md) (D1) for
the trade-off against the custom-firmware route this repo's `firmware/`
took instead.

## Prerequisites

1. **Device on the same network**, ESPHome Native API reachable on
   `:6053`. Find its IP/hostname (`ping <device>.local` or your router's
   DHCP client list).
2. **API encryption key (`noise_psk`)**, if the device requires one — the
   biggest unknown here (see "Known risks" below). Try keyless first; if
   the connection fails with `RequiresEncryptionAPIError` or similar,
   you'll need the device's key.
3. **Your Hoshi-style backend running**, reachable over `wss://` with a
   self-signed leaf certificate you control. Point `--cert` at that leaf
   (see the flag table below).
4. **A token, only if your backend's ingress auth is enabled.** Most
   default setups run without one — see
   [`../../docs/PROTOCOL.md` §2](../../docs/PROTOCOL.md).

## Setup

```bash
cd tools/bridge
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements.txt
```

> **VAD = Silero (ONNX), not RMS energy.** The bridge detects end-of-speech
> with the neural Silero VAD (more noise/TV-robust than raw energy
> thresholding). It depends only on **onnxruntime** (~18 MB), not torch —
> the `silero-vad` PyPI package pulls in torch+torchaudio, so instead the
> Silero ONNX model ships directly under `models/silero_vad_16k_op15.onnx`
> (~1.3 MB, extracted from the `silero-vad` wheel) and loads once at bridge
> start.

## Running it

Keyless (try this first on a fresh stock device):

```bash
.venv/bin/python napi_bridge.py --device-host <voice-pe-ip-or-.local>
```

With an API key and an explicit backend target:

```bash
.venv/bin/python napi_bridge.py \
  --device-host voice-pe.local \
  --api-encryption-key "BASE64_NOISE_PSK==" \
  --hoshi-ws-url wss://<hoshi-server-ip>:8081/ws/audio \
  --cert /path/to/your/leaf-cert.pem
```

Then say **"Okay Nabu"** at the device and speak.

### Options / env

| Flag | Env | Default | Purpose |
|---|---|---|---|
| `--device-host` | — | (required) | Voice PE IP / `.local` hostname |
| `--api-port` | — | `6053` | ESPHome API port |
| `--api-encryption-key` | `ESPHOME_API_KEY` | — | `noise_psk` (base64); empty = keyless |
| `--api-password` | `ESPHOME_API_PASSWORD` | `""` | Legacy password auth |
| `--hoshi-ws-url` | — | `wss://<hoshi-server-ip>:8081/ws/audio` | your backend's target URL — **must be set** |
| `--cert` | `HOSHI_CERT` | `certs/hoshi-server-leaf.pem` (if present) | wss trust anchor (your server's leaf cert) |
| `--token-file` | `HOSHI_API_TOKEN` | — | backend token, **only if the auth gate is ON** |
| `--room` / `--satellite-id` | — | — | optional identity fields sent in `start` |
| `--uplink-format` | — | `wav` | binary uplink container; only `wav` is implemented (`webm` is a TODO, falls back to wav) |
| `--silence-ms` | — | `900` | trailing silence (Silero prob<neg) that ends the turn |
| `--vad-threshold` | — | `0.8` | Silero speech probability >= this counts as speech (0..1, TV-hardened default) |
| `--vad-neg-threshold` | — | `thr-0.15` | hysteresis low watermark; prob<this counts as silence |
| `--max-utterance-ms` | — | `12000` | hard cap from speech-start -> force stop |
| `--min-speech-ms` | — | `50` | min voiced run before "speech start" (start guard) |
| `--silero-model` | — | `models/silero_vad_16k_op15.onnx` | path to the Silero ONNX model |
| `--vad-rms-threshold` | — | — | **deprecated** no-op (VAD is now Silero, not RMS) |
| `-v` | — | — | debug logging (roughly 1/s of the Silero probability) |

> **Tuning note:** the silence threshold is the single most impactful
> parameter and needs to be tuned against real recordings in your actual
> room, in small (~0.05 or 100 ms) steps. The defaults are a reasonable
> starting point, not a guarantee.

## The handshake this bridge implements (we play the "server"/HA role)

1. `APIClient.connect(login=True)` -> `subscribe_voice_assistant(handle_start,
   handle_stop, handle_audio=...)`. Setting `handle_audio` flips the
   `VOICE_ASSISTANT_SUBSCRIBE_API_AUDIO` subscribe flag -> the device streams
   mic audio **over the API** (no separate UDP socket).
2. The device wakes on "Okay Nabu" -> sends `VoiceAssistantRequest(start=true,
   conversation_id, flags, audio_settings, wake_word_phrase)`. `aioesphomeapi`
   calls our `handle_start(...)`.
3. `handle_start` returns **`0`** (not `None`) -> the library replies
   `VoiceAssistantResponse(port=0)`, meaning "use API audio" (`None`/an
   exception -> `error=true`, device aborts). This is where we open the wss
   session and send `{type:start, mimeType:"audio/wav", turnId}`.
4. The device streams `VoiceAssistantAudio` frames -> our `handle_audio(data,
   data2)`. `data` is the primary mic channel (PCM16/16k mono) and is
   **accumulated per turn in a buffer** (not streamed raw — the reference
   backend's STT rejects raw PCM on the binary uplink; it maps `mimeType` to
   a file extension and only recognizes webm/ogg/wav/mp4). `data2` (the
   second channel / AEC reference) is discarded (half-duplex; no barge-in
   here).
5. We drive the device's own state machine via `send_voice_assistant_event(...)`:
   `RUN_START -> STT_START -> STT_END{text} -> INTENT_START -> INTENT_END ->
   TTS_START{text} -> TTS_STREAM_START -> [send_voice_assistant_audio(chunks)] ->
   TTS_STREAM_END -> TTS_END -> RUN_END`, triggered by the backend's downlink
   frames (`transcript`, `tts_audio_start`, `llm_audio`, `llm_done`, `llm_error`).
6. End of utterance (bridge-side VAD silence/max-utterance, a watchdog, or a
   device-sent `stop`): we build **one valid WAV** from the buffer (RIFF/WAVE,
   PCM16 LE, 16000 Hz, mono, 44-byte header), send it as **one** binary frame,
   then `{type:"stop"}`. The reference backend buffers binary frames per
   session and transcribes once at `stop` — one complete WAV blob is exactly
   right (start -> one WAV blob -> stop, idempotent). Then we wait for
   `transcript` + `llm_audio` + `llm_done`.

### Backend frame mapping (`/ws/audio`)

| Backend downlink | Bridge action |
|---|---|
| `transcript {text}` | `STT_END{text}`, then `INTENT_START` |
| `no_input` | end the turn (`ERROR`+`RUN_END`) |
| `tts_audio_start` | `INTENT_END`, `TTS_START`, `TTS_STREAM_START` |
| `llm_audio {seq, data:base64-WAV-24k}` | WAV->PCM16, resample 24k->16k, `send_voice_assistant_audio` in 1 KB chunks |
| `llm_done {ttsHandled}` | `TTS_STREAM_END`, `TTS_END`, `RUN_END` |
| `llm_error {stage,message}` | `ERROR`+`RUN_END` |
| `turn_aborted {turnId}` | abort the turn |

Uplink to the backend: `{type:start, mimeType:"audio/wav",...}` -> **one**
binary WAV blob (PCM16/16k mono, the whole utterance) -> `{type:stop}`.
(`abort` for barge-in is scaffolded in the code, but this no-flash bridge
doesn't use it — half-duplex only.)

## Known risks / open questions

- **The exact event handshake ordering is unverified against real
  hardware.** Which `VoiceAssistantEvent` sequence a stock device strictly
  requires to open its mic *and* play TTS back cleanly before returning to
  idle isn't hardware-confirmed here — the sequence above is the most
  plausible reading of ESPHome's `voice_assistant.cpp on_event()`. If
  debugging, that ordering is the first thing to question.
- **API encryption key on a fresh stock device.** Whether an out-of-the-box
  Voice PE accepts the API keyless, or forces a `noise_psk`, is open — try
  keyless first. Note that onboarding through the Home Assistant companion
  app may provision a key on the device, which would make "stock = keyless"
  no longer true for that specific device.
- **TTS sample rate, 24k<->16k.** This device's speaker expects **16k**;
  the backend returns **24k** WAV, so the bridge resamples 24k->16k in
  Python. `audioop` was removed from the stdlib in Python 3.13+, so a
  pure-Python linear resampler is used as a fallback — its audio quality is
  unverified. For cleaner sound, add back an `audioop`-compatible package or
  a numpy-based resampler, or have your backend emit 16k directly if it
  supports that.
- **The second mic channel (`data2`) / AEC reference is discarded.** No
  barge-in support here (half-duplex only, matching the custom-firmware
  route).
- **wss pinning.** The `--cert` you pass must match the certificate your
  backend actually serves, and you must connect by whichever identity (IP
  or hostname) that certificate's SAN covers — a mismatch fails the
  handshake by design (see
  [`../../docs/PROTOCOL.md` §1](../../docs/PROTOCOL.md)).
- **Believed solid:** the API call shapes
  (`subscribe_voice_assistant`/`send_voice_assistant_event`/`_audio`/announce),
  the `API_AUDIO` flag logic (setting `handle_audio` -> flag, returning `0`
  -> API audio mode), WAV decoding, resample length math, and CLI/config —
  these have been checked locally against the installed library version,
  independent of a live device test.

## Sources

- `aioesphomeapi/api.proto` (VoiceAssistant messages,
  `VOICE_ASSISTANT_SUBSCRIBE_API_AUDIO`)
- ESPHome's `voice_assistant.cpp` (`request_start`/`on_event`/`on_audio`,
  `SAMPLE_RATE_HZ=16000`)
- [`esphome/home-assistant-voice-pe#477`][hvpe-477] — exactly this use case;
  the handshake ordering there is still an open question upstream too.
- [`../../docs/PROTOCOL.md`](../../docs/PROTOCOL.md) — the `/ws/audio` wire
  contract this bridge speaks on the backend side.

[hvpe-477]: https://github.com/esphome/home-assistant-voice-pe/issues/477
