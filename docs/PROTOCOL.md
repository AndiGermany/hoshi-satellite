# PROTOCOL.md — `/ws/audio` wire spec for `hoshi_ws_audio`

The exact wire protocol the firmware's `hoshi_ws_audio` component speaks to a
Hoshi backend. This spec was reverse-engineered and proven against a real
backend using a Python reference client
([`tools/bridge/napi_bridge.py`](../tools/bridge/napi_bridge.py)) *before* the
C++ firmware component was written — the firmware is a 1:1 port of the same
wire behavior, running on the ESP32 instead of over a Python bridge.

**Confidence legend**, used throughout:
- **[proven]** — exercised end-to-end against a real backend by the Python
  reference client; transcript and LLM audio actually came back.
- **[documented]** — implemented/expected server-side (or firmware-side) but
  not exercised by the reference client in that exact form.
- **[open]** — not yet implemented anywhere in this repo; a plausible
  design, to be built and measured on real hardware.

Trust what's measured, not what merely sounds right — items not marked
**[proven]** are unverified until you've watched them work.

---

## 0) Topology, one line

```
[Voice PE: micro_wake_word + mic]  --wss(/ws/audio)-->  your Hoshi server
   {start} -> WAV chunks(<=16KB) -> {stop}                -> STT
   speaker.play  <-- llm_audio{base64-WAV} / tts_audio_* --  -> LLM
   Half-duplex (no AEC): mic muted during playback           -> TTS
```

---

## 1) TLS — wss with a pinned leaf certificate **[proven]**

- **Connect by IP**, not hostname. The reference deployment's server is a
  static LAN host; the leaf cert's SAN covers exactly that IP. If your leaf
  cert's SAN covers a hostname instead, connect by hostname — just be
  consistent between how the cert was issued and how the client connects.
- **Trust anchor = the self-signed leaf itself** (Trust-On-First-Use
  pinning — exactly one certificate, no CA chain). The Python reference
  client loads it via `ssl_ctx.load_verify_locations(cert_path)` with
  verification and hostname-checking left **on**.
- **ESP-IDF mapping:** embed your server's leaf PEM as the `esp-tls`
  `cert_pem` field (the `hoshi_ws_audio` component's WS client uses this as
  its root trust anchor — see `firmware/esphome/components/hoshi_ws_audio/hoshi_ws_audio.cpp`,
  `SERVER_LEAF_PEM`, which ships as a placeholder you must replace with your
  own).
- **Generating and checking your own leaf cert:**
  ```bash
  # fetch what your server currently serves:
  echo | openssl s_client -connect <your-hoshi-server-ip>:<port> 2>/dev/null | openssl x509 -outform PEM
  # verify the fingerprint before you trust it:
  openssl x509 -noout -fingerprint -sha256 -in leaf.pem
  ```
  A self-signed leaf with a ~10-year validity window means you won't need to
  rotate it or re-flash routinely — only if the key is compromised or the
  server's IP changes (new SAN → new cert → re-flash).
- **Plain `ws://` is a hard no.** Audio should never travel unencrypted on
  the LAN.

---

## 2) Auth — token, only when your backend requires it **[proven: tokenless case]**

- The reference client can connect **without** a token when the backend's
  ingress auth is disabled, and this was the configuration under which the
  wire protocol below was originally proven end-to-end.
- **If your backend requires auth**, offer the token both ways (the
  reference client does; your server picks whichever it reads):
  1. Query param: `wss://<host>:<port>/ws/audio?token=<your-token>`
  2. Header: `Authorization: Bearer <your-token>`

  Prefer the `Authorization: Bearer` header if your ESP-side WS client
  supports custom headers — a token in a query string can end up in proxy
  access logs. **[documented]**
- **Send the token byte-exact.** If your server does a constant-time
  comparison (recommended, to avoid timing side-channels), it will very
  likely compare byte-for-byte with no trimming — don't append trailing
  whitespace/newlines. **[documented]**
- **Origin header:** a real ESP/non-browser client sends no `Origin` header
  at all. Don't try to set one artificially — a backend doing browser-style
  origin checks should treat "no Origin" as "not a browser" and allow it.
  **[documented]**
- **Provisioning order matters:** get your server's auth gate working and
  tested *before* you provision a real token onto the device and flip the
  gate to required — flipping the gate before the device (or your frontend,
  if you have one) can talk to it correctly just locks everyone out.

---

## 3) Uplink (device → server) — start → WAV chunks → stop **[proven]**

The exact, live-proven sequence per turn:

### 3.1 `start` (text/JSON frame) **[proven]**

On wake, send one JSON text frame:
```json
{ "type": "start", "mimeType": "audio/wav", "turnId": "<uuid-v4>" }
```
- `type`: constant `"start"`.
- `mimeType`: **`"audio/wav"`** — required, see gotcha (1) below.
- `turnId`: a UUID per utterance. A well-behaved server echoes it back on
  every turn event (`llm_*`, `tts_*`, `turn_aborted`) so the client can
  discard late frames from an aborted turn. **[proven]** that the reference
  client sets it; echo behavior is **[documented]**.
- **Optional, additive:** `"room": "<id>"`, `"satelliteId": "<id>"` — a
  server that understands them can use them for per-device
  settings/routing; a server that doesn't should just ignore them.

### 3.2 Binary audio — one complete WAV blob, in ≤16 KB chunks **[proven]**

- Mic delivers PCM16 mono 16 kHz.
- The reference backend buffers all binary frames of a session and
  transcribes once, at `stop` — so send exactly **one** complete WAV
  container per turn: `[44-byte RIFF/WAVE header] + [PCM16-LE samples]`.
- **Chunking:** split the WAV blob into binary frames of **≤ 16384 bytes**
  each and send them in order. (The reference backend truncates oversized
  single frames — see gotcha (2).)
- WAV header, exact layout (16-bit PCM, 44-byte header total):
  `RIFF <riff_size=36+data> WAVE | fmt 16 fmt=1 ch=1 rate=16000 byteRate=32000 blockAlign=2 bits=16 | data <data_size> <pcm>`.
- **Streaming variant (firmware-side, [open]):** since the server
  concatenates all binary frames up to `stop` anyway, a firmware
  implementation is free to send the 44-byte header as the first frame and
  then stream raw PCM chunks live while the user is still speaking, instead
  of buffering the whole turn in RAM first — as long as `header ++ pcm ++
  ... ++ stop` arrives on the wire in order, it's wire-identical to the
  proven single-blob approach. This needs to be measured on real hardware
  (RAM headroom, timing) — the reference Python client sent one blob at the
  end, not a live stream.

### 3.3 `stop` (text/JSON frame) **[proven]**

After the last audio chunk, send exactly one frame:
```json
{ "type": "stop" }
```
Exactly once per turn (keep it idempotent on your side regardless). This is
what triggers STT → LLM → TTS server-side. **The server does not do its own
VAD or timeout the turn on its own** — see gotcha (3): without `stop`, the
server may just wait forever.

### 3.4 `abort` (text/JSON frame) — barge-in **[documented]**

```json
{ "type": "abort", "turnId": "<uuid>" }
```
Aborts the in-flight turn (barge-in via a physical button, in this
firmware's case). Omit `turnId` to mean "the current turn." A server
implementing this should respond with `turn_aborted` + `llm_done`.

---

## 4) Proven gotchas — hit these as real bugs, fixed once, documented so you don't repeat them

1. **Raw PCM does not get transcribed — WAV container is mandatory.**
   A backend that maps `mimeType` to a file extension for its STT step (only
   recognizing e.g. `webm/ogg/wav/mp4`) will treat raw PCM as garbage and
   return an empty transcript. Always send a proper 44-byte WAV header
   (PCM16/16k/mono) with `mimeType:"audio/wav"`.
2. **Oversized single frames get truncated — cap every binary frame at ≤16 KB.**
   A single WS frame that's too large can get cut off by the server (EOF,
   empty transcript). Chunk your binary uplink at **≤ 16384 bytes** per
   frame — proven reliable at this size.
3. **No server-side VAD — an explicit `{type:"stop"}` is mandatory (on-device endpointing).**
   The server does not end a turn on its own; without `stop` it can wait
   indefinitely. The client (device or bridge) must detect end-of-speech
   itself and send exactly one `stop`.
   - The Python reference client uses a neural VAD (Silero, via ONNX) on
     16 kHz frames with hysteresis (roughly: a speech-probability threshold
     around 0.8, a lower "back to silence" threshold about 0.15 below that,
     ~900 ms of trailing silence to end a turn, a minimum speech duration
     guard around 50 ms, and a hard cap around 12 seconds from speech
     start).
   - **A known trap with Silero-style streaming VAD models:** they can
     require a small amount of context from the *previous* frame prepended
     to each new frame — feeding bare, unprefixed frames can make the model
     read near-zero speech probability even during clear speech, which
     silently defeats end-of-turn detection and runs straight into a
     timeout. If you port a VAD, verify this against your model's docs.
   - **On the ESP32 firmware side**, a full neural VAD is not practical in
     the real-time audio path, so this firmware uses a much simpler
     **energy/RMS VAD** instead — see
     [`ARCHITECTURE.md`](ARCHITECTURE.md) and the component README for why,
     and the RUNBOOK for how to tune it on real hardware. What matters at
     the wire level is unchanged: exactly one `stop` after the audio.
4. **STT/LLM pipelines commonly have a timeout on the order of ~15 seconds — keep utterances short and cap the turn.**
   A clip that runs into your backend's STT timeout comes back as an empty
   transcript. The reference client uses an **absolute wall-clock cap from
   wake** (not from speech-start) of about 10 seconds, forcing a `stop` even
   if VAD never detects speech at all — a safety net against a
   misconfigured or stuck audio path.
5. **Half-duplex (no AEC on this hardware).**
   Exactly one turn at a time; mic capture pauses while the speaker plays. A
   wake trigger during an active turn should be dropped or treated as
   barge-in, not a second concurrent turn.

---

## 5) Downlink (server → device) — frames the client needs to read

All downlink frames are **JSON text frames** — even audio (`llm_audio` is
base64-encoded WAV *inside* JSON, never a raw binary frame). A client should
ignore any stray binary frame on the downlink.

| `type` | Fields | Meaning |
|---|---|---|
| `transcribing_started` | — | STT is running (informational) |
| `transcript` | `text` | Recognized text. STT is done. **[proven]** |
| `no_input` | — | No speech recognized → end the turn. **[proven]** |
| `llm_start` | `provider`, `model`, `emotion?` | LLM is starting (informational) |
| `llm_delta` | `text` | Streaming text token (incremental). **[proven]** |
| `tts_audio_start` | `provider`, `estimatedMs?` | TTS audio is about to start |
| `llm_audio` | `seq` (int), `data` (base64 **WAV PCM16/24k/mono**) | One audio segment. Play it. **[proven]** |
| `tts_audio_end` | `actualMs` | TTS audio finished (informational) |
| `llm_done` | `ttsHandled` (bool) | Turn is done. **[proven]** |
| `llm_error` | `stage` (`STT`\|`LLM`\|`TTS`\|other), `message` | Error → end the turn. **[proven, error path]** |
| `turn_aborted` | `turnId` | Turn was aborted (barge-in ack) |
| `session_meta` | `room`, `satelliteId` | Ack of optional identity fields sent in `start` |

A server may add optional extensions beyond this core set (e.g. a push for
UI dimming/night-mode, a countdown for an in-progress timer, a recognized
speaker's identity for accent color) — a client should simply ignore JSON
frame types it doesn't recognize rather than erroring out. This firmware
happens to have optional handlers for a few such extensions; treat them as
opt-in decoration, not part of the required core contract.

### 5.1 Playing `llm_audio` — required details **[proven]**

- `data` decodes (base64) to a **WAV container, PCM16, 24 kHz, mono**.
- If your speaker hardware runs at a different native rate (this board runs
  16 kHz on the uplink/wake path, resampled up to 48 kHz for the actual i2s
  sink), resample accordingly before playback.
- Feed the speaker in small chunks (around 1 KB at a time worked well for
  the reference client) so its buffer keeps up.
- Multiple `llm_audio` segments arrive with increasing `seq` — play them in
  `seq` order.

### 5.2 `llm_done.ttsHandled`

- `ttsHandled=true` means the server already played the audio itself
  somewhere (e.g. a server-side speaker path) — the client should **not**
  re-synthesize or play anything, just close the turn. In a satellite
  deployment where the device is the only audio output, expect this to
  normally be `false`, meaning the device is responsible for playback.

---

## 6) Device-side state machine (reference mapping, not a required wire sequence)

This is how the local turn lifecycle maps to wire events — useful as a
reference if you're implementing your own client, not something you need to
send:

```
IDLE -> LISTENING (wake + start sent)
     -> AWAITING_STT (stop sent, waiting for transcript)
     -> THINKING (transcript received, waiting for first audio)
     -> SPEAKING (audio playing)
     -> IDLE (llm_done, or no_input / llm_error / ws-close)
abort: -> {abort, turnId} -> turn_aborted{turnId} + llm_done -> IDLE
```

---

## 7) Reference constants (from the proven Python client)

| Constant | Value |
|---|---|
| Mic format | PCM16, 16000 Hz, mono |
| Uplink container | WAV (44-byte header), `mimeType:"audio/wav"` |
| Uplink chunk limit | 16384 bytes/frame |
| TTS downlink format | base64 WAV, PCM16, 24000 Hz, mono |
| Speaker resample | 24000 → device native rate |
| Speaker chunk | ~1024 bytes/frame |
| Silence endpoint (reference VAD) | ~900 ms |
| VAD threshold / hysteresis-low (reference VAD) | ~0.80 / ~0.65 |
| Max utterance from speech-start (reference VAD) | ~12000 ms |
| **Max turn from wake, absolute** | ~10000 ms |
| Min-speech guard | ~50 ms |
| Duplex | Half-duplex (one turn at a time) |

---

## 8) What's proven vs. what's still open

**Proven** (by the Python reference client, against a real backend —
transcript and LLM audio actually came back):
- Tokenless wss connection with leaf-pinning to the endpoint.
- `{start, mimeType:audio/wav, turnId}` → WAV blob in ≤16 KB chunks → `{stop}`.
- Downlink `transcript`, `llm_delta`, `llm_audio` (base64 WAV 24k), `llm_done`.
- All 5 gotchas above (WAV requirement, 16 KB cap, explicit stop, ~10s
  wall-clock cap, half-duplex).

**Documented but not exercised by the reference client:** the token path
when auth is required, `abort`/`turn_aborted` barge-in, optional identity
fields, `llm_error`/`no_input` paths end-to-end, `ttsHandled=true` behavior.

**Open — firmware-side, still to build/measure on real hardware:**
- On-device endpointing (wake → VAD → exactly one `stop`) using a
  lightweight VAD suitable for a microcontroller, since the reference VAD
  (neural, ONNX) doesn't fit the real-time budget on this hardware.
- Live-streaming the uplink (header + PCM chunks while still speaking)
  instead of buffering the whole turn.
- Barge-in via a physical control.
- A device-side state machine/LED feedback with no Home Assistant API
  involved.
- Provisioning a real token + first live test against your own backend.

---

## 9) References

- Proven reference client: [`tools/bridge/napi_bridge.py`](../tools/bridge/napi_bridge.py)
- Firmware component implementing this spec:
  [`firmware/esphome/components/hoshi_ws_audio/`](../firmware/esphome/components/hoshi_ws_audio/)
- [`ARCHITECTURE.md`](ARCHITECTURE.md) — why the firmware never blocks the
  main loop on network I/O
- [`../firmware/RUNBOOK.md`](../firmware/RUNBOOK.md) — build/flash recipe and gates
