# IDEAS.md — edge/firmware roadmap

Feature ideas for this firmware, roughly ordered by priority. Tags: 🔌 = a
pure device-side feature (no backend changes needed) · 🤝 = needs a
matching backend extension · ✅ = built · ⏳ = in progress/planned · ⏸ =
deliberately deferred · ❌ = considered and dropped.

## 1. ⏳🤝 Timers & alarms (top priority)

- **Device executes, backend sets:** the device counts down locally
  (timer) or compares against NTP wall-clock time (alarm), rings locally
  (tone + LED), and **persists the state to NVS** — an alarm has to fire
  even if the backend or WiFi is down when it's due.
- Needs on-device: SNTP time sync, NVS persistence, a local alarm tone,
  stop/snooze via the center button.
- Backend side: recognize the intent + send a control frame
  (`timer_set`/`alarm_set`/`cancel`) over `/ws/audio`. This is an optional
  downlink extension, not part of the core wire contract — see
  [`PROTOCOL.md` §5](PROTOCOL.md).
- LED idea: a running timer as a **shrinking ring** (visual countdown); a
  ringing alarm as urgent pulsing.

## 2. 🔌 Instant local audio feedback

- A short confirmation "blip" right on wake detection, before STT/the
  backend has responded — makes the device feel far more responsive.
- Timer/alarm tone, a short error tone, a subtle confirmation tone. All
  local, no network dependency.

## 3. 🔌 Richer LED feedback — package 1 built, package 2 (night mode) built

- ✅ **Speaking VU meter** — the ring breathes with speech energy (RMS tap
  on the playback drain).
- ✅ **Thinking swirl** — a rotating, dimmed amber comet instead of a static
  yellow, for perceived-latency cover.
- ✅ **Stop acknowledgment** — a red sweep as confirmation after
  `abort_turn()` (center button or a "stop" wake trigger).
- ✅ **Wake spark** — a white spark blooms into listening-cyan over ~240 ms.
- ✅ **Mute indicator** — a subtle red pixel while the hardware mute switch
  is engaged, shown only at idle.
- ✅ **OTA flash progress** — a green fill arc driven directly from the OTA
  progress callback (paints and flushes itself; the main loop is blocked
  during the flash).
- ✅ **Timer countdown arc (device side)** — waits for a `timer_state` frame
  from a backend that implements it.
- ✅ **Speaker-accent shimmer (device side)** — waits for an optional
  `speaker` downlink frame naming a recognized speaker; see the example
  accent-color table in `hoshi_ws_audio.cpp` (customize per household).
- ❌ **Idle decoration (an ambient shooting-star effect) — dropped.** Field
  testing found it distracting at night; idle stays dark except for
  timer/mute info glows.
- ⏸ **Alarm "sunrise" glow, opt-in only** — needs the alarm frame to exist
  first; deliberately not a default.
- Open: subtly indicating connection loss (WiFi/WS down).
- ✅ **Night mode built:** a `night_mode` handler (downlink -> atomics), a
  global dim factor applied across every LED branch (boot comet, volume
  bar, OTA ring, etc.), a visibility floor (~0.12) for interaction feedback
  so it stays perceptible even at night, pure info glows allowed to reach 0
  (fully off), NVS persistence (loaded in `setup()`, saved from `loop()` via
  a dirty flag) so a middle-of-the-night reboot boots dim, and a repaint
  trigger on every night-mode flip.
  Alongside this: `satelliteId`/`room` are actively sent in the `start`
  frame — the per-device routing key for backends that implement
  device-specific settings.
- ✅ Boot animation (comet) + a volume-level bar — built and verified
  on-device.

## 4. 🔌 Physical controls

- **Center-button gestures:** short press = barge-in/stop (already
  implemented) · long press = stop an alarm/timer, or "repeat the last
  response" · double press = free for future use (e.g. a do-not-disturb
  toggle).
- Stop/snooze for a ringing alarm (see §1).

## 5. 🔌🤝 Robustness / graceful degradation

- A **local fallback announcement** when the backend is unreachable ("I
  can't reach the server right now") instead of a silent failure or just a
  red LED.
- Polish the WS-connect-vs-WiFi-up ordering (a known minor reconnect
  hiccup right at boot).

## 6. 🤝 Voice comfort

- **"Stop" as a spoken barge-in** — a stop wake-word model can already be
  loaded; use it to interrupt an in-progress spoken response by voice
  instead of only via the button.
- ⏳ **A custom wake word** (the offline training pipeline exists — see
  [`../tools/wake/README.md`](../tools/wake/README.md) — but needs real
  household recordings and a training run before it's usable).

## 7. 🤝 Multi-room / identity (later)

- Set `room`/`satelliteId` per device (the wire contract already has these
  fields) — only matters once you have more than one satellite device.

---

**Next concrete steps:** (1) flash and verify the boot animation on real
hardware (built, needs verification). (2) Nail down a timer/alarm frame
contract with whatever backend you're targeting, then build the device-side
engine. (3) Low-effort, high-perceived-value edge polish (mute LED, wake
blip).
