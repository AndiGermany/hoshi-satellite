# DECISIONS.md — why this firmware is shaped the way it is

Short rationale for the architectural choices that aren't obvious from the
code alone. Written as decision/consequence pairs rather than a chronological
log — the "why" tends to outlive the "when."

---

## D1: Flash custom firmware instead of routing through Home Assistant

**Decision:** the device runs custom ESPHome firmware and talks directly to
a Hoshi-style backend's `/ws/audio` endpoint. Home Assistant is not in the
audio path at all.

**Why:** the stock HA Voice PE firmware binds the device to Home Assistant's
`voice_assistant` + `api` components, which in turn requires HA to act as
the conversation agent and forward audio onward. That's an extra hop, an
extra dependency, and an extra point of protocol drift for a project whose
backend already exposes its own WebSocket audio endpoint. Talking to that
endpoint directly is architecturally simpler, not just more direct.

**Trade-off accepted:** flashing custom firmware carries real (if low) brick
risk and up-front effort that a no-flash approach (treating the device as an
ESPHome Native API client, port 6053) would have avoided. A no-flash bridge
was evaluated and works as a bridge (see
[`../tools/bridge/README.md`](../tools/bridge/README.md)) — it's the lower-risk
path if you'd rather not flash at all, at the cost of an extra process in the
loop and being bound to ESPHome's own wire protocol instead of your
backend's native one. Custom firmware was chosen deliberately for the
long-term shape (this project's own protocol end to end, no proxy, headroom
for streaming-uplink and always-on-stream designs later) — see
[Phase 0 / recovery discipline](../firmware/recovery/README.md) before
following that path yourself.

**Consequence:** `voice_kit`/XMOS stays byte-for-byte identical to upstream
(don't touch the far-field audio front-end); `voice_assistant` + `api` are
removed; `micro_wake_word` + a custom WebSocket client
(`hoshi_ws_audio`) replace them.

---

## D2: Half-duplex, not full-duplex

**Decision:** the mic is muted while the speaker is playing. There is no
barge-in via speaking over the device — only via a physical button (which
sends an explicit `abort`).

**Why:** this hardware has no acoustic echo cancellation reference channel
reaching the wake/VAD path, so the mic would otherwise pick up the device's
own TTS output and misinterpret it as user speech (or re-trigger the wake
word on it). Half-duplex is the correct choice for this specific hardware,
not a general preference — a board with a real AEC reference channel could
support true voice barge-in.

**Consequence:** `hoshi_ws_audio` gates mic capture on the turn's playback
state (`mic_should_capture_()`); `micro_wake_word` is explicitly stopped
while `SPEAKING` and restarted afterward.

---

## D3: TLS via Trust-On-First-Use leaf-pinning, not a CA chain

**Decision:** the firmware embeds one specific server's self-signed leaf
certificate directly as its `esp-tls` trust anchor — no certificate
authority, no chain validation, just "this exact certificate is trusted."

**Why:** for a single fixed backend on a private LAN, standing up and
maintaining a full CA is disproportionate. Pinning the one leaf certificate
you actually expect to see gives the same practical guarantee (nobody else
can impersonate your server without your private key) with far less
infrastructure, at the cost of needing a re-flash if the server's IP or key
ever changes. Connect by IP, not hostname, and keep the certificate's SAN
and the connection method consistent with each other.

**Consequence:** `wss://` only — plain `ws://` is intentionally unsupported.
Every deployment must generate its own leaf certificate and swap it into
`hoshi_ws_audio.cpp` before building; the certificate shipped in this repo is
a placeholder that will not connect to anyone's server. See
[`PROTOCOL.md` §1](PROTOCOL.md) for the exact TLS contract.

---

## D4: Energy/RMS VAD on-device, not the neural VAD used for reference/testing

See [`ARCHITECTURE.md` §3](ARCHITECTURE.md) for the full rationale — in
short, the neural (Silero/ONNX) VAD used by the Python reference client
isn't feasible in the ESP32-S3's real-time audio path, so the firmware uses
a simpler, hysteresis-based RMS energy detector with a room-relative ambient
floor. This is a deliberate accuracy/footprint trade-off, and the resulting
thresholds are config keys precisely because they need per-room tuning.

---

## D5: Wake word — start from the stock model, defer a custom one

**Decision:** ship with ESPHome's stock `micro_wake_word` model
(`okay_nabu`) rather than blocking the whole firmware on training a custom
wake word first.

**Why:** proving the rest of the pipeline (turn lifecycle, TLS, audio
round-trip, half-duplex, VAD tuning) doesn't depend on having a trained
custom wake model, and it's the more expensive, more data-hungry piece of
work. Decoupling it means the firmware can be verified end-to-end sooner.

**Consequence:** a custom wake word (this project trained one, "Hey Hoshi")
is a separate, later step — see
[`../tools/wake/README.md`](../tools/wake/README.md) and
[`../tools/wake/train/TRAIN-RUNBOOK.md`](../tools/wake/train/TRAIN-RUNBOOK.md).
Swapping in a trained model later is a one-line change in the
`micro_wake_word:` block plus a re-flash.

---

## D6: LED feedback and the optional downlink extensions (night mode, speaker accent, timers)

**Decision:** the LED ring reflects local turn state (listening/thinking/
speaking/error) purely from device-side state transitions — no Home
Assistant, no external LED controller. A few *optional* JSON downlink frame
types (`night_mode`, `speaker`, `timer_state`) let a backend that implements
them push extra context (a global dim factor, a recognized-speaker accent
color, a countdown arc) without being part of the required wire contract.

**Why:** a client should never *require* extensions it can't get from every
backend — the core turn lifecycle (`start`/`stop`/`transcript`/`llm_audio`/
`llm_done`) works with zero backend-side extra work, and everything else
degrades gracefully (a client that never sends `night_mode` just means the
ring never dims). See [`PROTOCOL.md` §5](PROTOCOL.md) for the full frame
list and which parts are proven vs. optional.

**Consequence:** any backend can implement none, some, or all of the
optional extensions. The device-side accent-color mapping for `speaker` is
a small example table meant to be edited per household — see the comment
above it in `hoshi_ws_audio.cpp`.

---

## Open questions

- **Streaming uplink.** The wire protocol already supports sending the
  44-byte WAV header as the first frame and then streaming raw PCM chunks
  live (see [`PROTOCOL.md` §3.2](PROTOCOL.md)) instead of buffering a whole
  utterance in RAM first — not yet measured on real hardware (RAM headroom,
  timing).
- **True voice barge-in.** Would need a real AEC reference path on this
  hardware, which doesn't currently exist — see D2.
- **A second independent backend/device pairing.** This firmware has been
  verified against one reference deployment and one physical unit; it has
  not been cross-verified against an unrelated backend implementation or a
  second device.
