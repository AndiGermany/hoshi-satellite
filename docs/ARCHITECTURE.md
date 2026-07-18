# ARCHITECTURE.md — why the firmware never blocks the main loop on network I/O

This documents the concurrency redesign behind `hoshi_ws_audio` (see
[`../firmware/esphome/components/hoshi_ws_audio/`](../firmware/esphome/components/hoshi_ws_audio/)).
An early version of this firmware had a hard, reproducible boot hang; this is
the confirmed root cause and the fix, kept here so nobody has to rediscover
either. "Compiles clean" is not "works" — verify on real hardware before you
trust a change here.

---

## 1. The bug this design fixes

The wake trigger (`micro_wake_word`'s `wake_word_detected_trigger`) fires on
the **ESPHome main loop**. The original implementation reacted to it by
calling `start_turn()` straight from that callback, which did a *blocking*
`esp_websocket_client_send_text()` (2 s timeout) — often before the TLS
handshake had even finished connecting.

A main loop blocked for up to 2 seconds starves everything else that depends
on it running: USB-CDC (serial goes silent), the ESPHome API port (times
out), the LED (freezes on whatever it was last painting), and the watchdog.
Those four symptoms — dead serial, dead API, frozen LED, eventual
watchdog reset — are exactly what a blocked main loop looks like from the
outside, and they're easy to mistake for a hardware or WiFi problem instead
of what they actually are: synchronous network I/O on a loop that has to stay
responsive.

**The fix: no networking of any kind runs on the ESPHome main loop, ever.**

## 2. Four strictly separated contexts

State is shared only through a single-producer/single-consumer
`esphome::ring_buffer::RingBuffer` per direction, plus a handful of
`std::atomic` flags — no locks on the hot path.

1. **Mic callback** (runs on the i2s mic task). `micro_wake_word` owns an
   **active** `MicrophoneSource` that powers the i2s mic; this component
   attaches a **passive** `MicrophoneSource` to the same `i2s_mics` block, so
   it receives frames that are already channel-selected, converted from
   32-bit to 16-bit, and gain-applied — no hand-rolled downmix in this
   component. The callback does exactly three things: write PCM16 into
   `uplink_rb_`, run the RMS VAD, and set the atomic `eos_requested_` flag on
   end-of-speech. No socket access, no per-frame heap allocation.

2. **`loop()`** (ESPHome main loop). Owns the turn state machine. Drains
   `play_rb_` into the speaker via non-blocking `play(..., ticks=0)` with
   backpressure, tracks watchdog deadlines per state
   (`AWAITING_STT`/`THINKING`/`SPEAKING` → back to idle + error LED on
   timeout), and drives the LED on every state transition. A turn only ends
   once the play queue is empty, the speaker has no buffered data left, and
   it isn't currently running — then `speaker.finish()` is called exactly
   once. **Never does blocking I/O.**

3. **A dedicated FreeRTOS task, `hoshi_ws_tx`** (pinned to a different core
   than the IDF WS task). This is the **only** caller of
   `send_text`/`send_bin`. It waits for the connection to be up, sends
   `start`, accumulates the turn's WAV data, and on end-of-speech sends it in
   ≤16 KB binary frames followed by `stop`. The blocking send / 2-second
   timeout only ever affects this task — never the main loop.

4. **The IDF `esp_websocket_client` task.** Handles TLS, receive, and
   dispatch. On `CONNECTED` it flips the `connected_` atomic and notifies the
   tx task. On `DATA` it reassembles fragmented frames and decodes
   `llm_audio` (base64 → PCM16 @ 24 kHz) straight into `play_rb_`. It
   **never** calls send from here — doing so risks a recursive-lock deadlock
   inside the WS client.

### Audio path, end to end

**Uplink:** passive mic source (16 kHz / mono / 16-bit) → `uplink_rb_` → tx
task → chunked WAV frames over the wire.

**Downlink:** `llm_audio` (base64 WAV, 24 kHz) → decode → `play_rb_` → `loop()`
drains it into the speaker with `set_audio_stream_info(16, 1, 24000)` set
*before* first playback → a resampler steps 24 kHz → 48 kHz for the i2s DAC.

The WebSocket connection itself opens lazily — off the main loop, not in
`setup()` — on the *first* wake word, not at boot. This avoids an
early-boot regression where opening the connection in `setup()` interfered
with wake-word detection coming up cleanly.

## 3. Why an energy/RMS VAD instead of a neural one

The project's Python reference client
([`tools/bridge/napi_bridge.py`](../tools/bridge/napi_bridge.py)) uses a
neural VAD (Silero, via ONNX: onnxruntime + a ~1.3 MB model + a per-frame
matrix multiply). That's not something you can run inside the ESP32-S3's
real-time audio hot path, so the firmware substitutes a much simpler
**RMS-energy silence detector** with hysteresis: track RMS per mic block,
require a minimum speech duration before "speech started" counts (debounces
a single noisy block), then count trailing silence below a hysteresis-low
watermark until a `silence_ms` threshold ends the turn. An absolute
wall-clock cap (`max_turn_ms`) is a safety net independent of the VAD.

This is a deliberate downgrade relative to a neural VAD — noisier
environments (a TV playing, for instance) will fool a pure energy threshold
more easily — which is why the detector also tracks a slow-moving ambient
noise floor at idle and treats "silence" as relative to that floor rather
than an absolute number (see the `vad_feed_` implementation and the comments
around `vad_ambient_margin`/`vad_silence_floor_max` in
`hoshi_ws_audio.cpp`/`.h`). All of the VAD's thresholds are exposed as YAML
config keys specifically because they need per-room, per-device tuning — see
[`../firmware/RUNBOOK.md`](../firmware/RUNBOOK.md) for the tuning procedure.

## 4. Hard-won invariants — read before touching this code

1. **`set_audio_stream_info` ordering.** Must be set on the speaker before
   its first `start()`/`play()` call, or the resampler latches onto the
   wrong rate (16 kHz input read as 24 kHz plays back ~1.5x too fast — the
   "chipmunk" bug). Re-asserted defensively in `enter_speaking_()`.
2. **No shared `std::vector` across tasks.** The play buffer is a
   single-producer/single-consumer `RingBuffer`, not a `std::vector` touched
   from two tasks — the latter is a heap-corruption bug waiting to happen.
   `to_idle_()` doesn't reset until the producer side is confirmed quiesced,
   so a straggler `llm_audio` frame arriving right after `llm_done` can't
   corrupt state.
3. **Uplink ring must be large enough to drain continuously** while
   `LISTENING` (≥ 64 KB, roughly 2 seconds of audio) — undersizing it drops
   mid-utterance audio and truncates STT input silently. Log the high-water
   mark during tuning.
4. **TLS/WS hardening.** Verify your server's certificate fingerprint before
   flashing. If your leaf certificate's SAN is an IP address, some esp-tls
   builds refuse to validate it against the connection host string — that's
   what `skip_cert_common_name_check` is for (it still pins the exact leaf
   bytes via `cert_pem`, so it is not a security downgrade). Log
   `CONNECTED`/`ERROR` distinctly so a TLS failure is never confused with a
   hang. Only call `start_turn()` while actually `connected_`; otherwise show
   the error LED.
5. **`turnId` guard must be tolerant.** If your server echoes `turnId` back
   on every event and your device-side guard treats a missing/mismatched
   `turnId` as fatal, it will silently drop *every* downlink frame — which
   looks exactly like a hang. Either make the guard tolerant of a
   missing/differing `turnId`, or confirm your server's echo behavior for
   real before relying on strict matching.
6. **VAD/gain/channel are hardware-calibration knobs, not constants.** An
   XMOS AGC'd stream behaves differently from a raw mic; start `mic_gain`
   low (1–2, not the wake path's higher default) and verify which channel is
   the cleaned one on your actual device. Tune `silence_ms`/`min_speech_ms`
   against real speech, not assumptions. The gate for "it's tuned" is a
   consistently non-empty transcript, not a vibe.
7. **Stop the wake-word detector while speaking.** There's no acoustic echo
   cancellation on this hardware, so without this the device can trigger on
   its own TTS output. `micro_wake_word` is stopped on entering `SPEAKING`
   and restarted on returning to idle.
8. **First real compile surfaces unknowns.** Expect to hit
   `microphone_source_schema` binding details, `AUTO_LOAD` requirements
   (`ring_buffer`), `esp_websocket_client` header/field-name specifics
   (`payload_len`/`offset`), and `AudioStreamInfo` constructor shape. Budget
   time for a compile-fix pass on first bring-up against a new ESPHome
   version.
9. **`voice_kit` (XMOS) firmware version mismatches are a real brick risk.**
   Confirm your device's installed XMOS firmware version before flashing a
   `firmware:` block that pins a specific version, or omit the block
   entirely.
10. **Verify the wake-word regression independently.** Don't assume the
    off-main-loop refactor automatically fixes an unrelated wake-detection
    issue — after any refactor, flash with debug logging and confirm wake
    still fires reliably; bisect (API on/off, boot-delay, wake-start
    placement) if it doesn't.

## 5. Verification order (what to check, in sequence)

1. `esphome config` + `esphome compile` clean.
2. Certificate fingerprint matches your server (item 4 above).
3. **Loop-liveness check:** flash, trigger wake, and confirm the device
   stays reachable (network port, debug log) *during* a turn — proof the
   main loop never stalls and USB/serial survives.
4. Mic channel/gain calibration → consistently non-empty transcripts.
5. Full STT → LLM → TTS round trip in your own language.
6. **Audible playback** — correct pitch (proves the 24 kHz→48 kHz resample
   chain is right), no cut-off tail.
7. VAD endpointing — silence actually ends the turn; a stuck turn hits the
   watchdog instead of hanging.
8. Barge-in (button → abort) and the hardware mute switch.
9. Soak test: 20+ wake/reply cycles, no heap growth, no watchdog resets, and
   a dropped WS connection reconnects cleanly.

## References

- [`../firmware/esphome/components/hoshi_ws_audio/`](../firmware/esphome/components/hoshi_ws_audio/) — the implementation
- [`../firmware/esphome/components/hoshi_ws_audio/README.md`](../firmware/esphome/components/hoshi_ws_audio/README.md) — component contract map
- [`PROTOCOL.md`](PROTOCOL.md) — the wire spec this firmware implements
- [`../firmware/RUNBOOK.md`](../firmware/RUNBOOK.md) — flash sequence, gates, and VAD/gain tuning procedure
- [`DECISIONS.md`](DECISIONS.md) — why this design was chosen over the alternatives
