# RUNBOOK — flashing the HA Voice PE with this firmware

> **A clean way back, before a single custom byte goes on the device.**
> This is the exact flash sequence for `esphome/hoshi-voice-pe.yaml`, with the
> gotchas that cost real hours to find, written down so you don't have to
> rediscover them.

Every hardware/flash step below needs you physically present at the device —
none of it should run unattended.

---

## Phase 0 — Recovery first (before any flash)

> A bricked Voice PE is the one thing you can't easily undo.

1. Back up the current stock firmware — see
   [`recovery/README.md`](recovery/README.md) for the exact `esptool
   read-flash` command and what to verify (size + sha256) before trusting
   the backup.
2. Also grab the current official `home-assistant-voice.factory.bin` (+
   `.sha256`) for your installed version from
   [`esphome/home-assistant-voice-pe/releases`](https://github.com/esphome/home-assistant-voice-pe/releases)
   as a second line of defense.
3. **Rehearse the recovery path once**, for real: unplug power → hold the
   center button (GPIO0) → plug in USB-C → hold briefly → release
   (bootloader mode). Use a USB **data** cable, not charge-only; if the
   device isn't detected, check the internal USB-select switch.
4. **Never** enable Secure Boot / Flash Encryption / `DIS_DOWNLOAD_MODE` —
   the one real hard-brick path on this board.

**Gate:** don't proceed to Phase 4 (first flash) until you've done this at
least once and know your recovery path works.

---

## Phase 1 — Firmware finalized offline (no device touched)

This phase doesn't need the hardware at all.

1. `esphome/hoshi-voice-pe.yaml` keeps the upstream audio stack (pins,
   voice_kit/XMOS, dual-I2S, LED ring, I2C) and swaps `voice_assistant`+`api`
   for `micro_wake_word` + the custom `hoshi_ws_audio` component. Stock wake
   model `okay_nabu` by default (a trained custom wake word is a later,
   separate step — see [`../tools/wake/README.md`](../tools/wake/README.md)).
2. **Check the `voice_kit.firmware:` block against your actual device's
   installed XMOS firmware version** before your first flash. Either match
   it exactly or comment the block out — a version mismatch triggers an XMOS
   I2C DFU with real corruption risk.
3. Replace the placeholder TLS leaf certificate in
   `components/hoshi_ws_audio/hoshi_ws_audio.cpp` (`SERVER_LEAF_PEM`) with
   your own Hoshi server's leaf certificate, and set `hoshi_host` /
   `hoshi_port` / `hoshi_satellite_id` / `hoshi_room` in the YAML's
   `substitutions:` block.

---

## Phase 2 — Build environment

```bash
# isolated venv recommended
python3 -m venv ~/.esphome-venv
source ~/.esphome-venv/bin/activate
pip install esphome
esphome version
```

Copy `esphome/secrets.yaml.example` → `esphome/secrets.yaml` and fill in your
real WiFi SSID/password (**2.4 GHz only**, no 5 GHz), your Hoshi API token,
and optionally an OTA password. **`secrets.yaml` is gitignored — never commit
it.**

---

## Phase 3 — Compile & config-validate (no flash)

```bash
# from firmware/esphome/
esphome config hoshi-voice-pe.yaml     # schema/substitution check
esphome compile hoshi-voice-pe.yaml    # real build
```

First-build gotchas, roughly in the order you'll hit them:

1. The `esp_websocket_client` managed component needs network access on the
   first build (it's pulled from the esp-idf component registry).
2. The `okay_nabu`/`stop` wake models are pulled from GitHub release URLs on
   first build — also needs network access.
3. `voice_kit.firmware:` md5/version only matters at flash time, not compile
   time.

**Gate:** don't flash until `esphome compile` is clean. Fix and re-run —
don't treat a red compile as "done."

---

## Phase 4 — USB first flash

> Only after recovery is rehearsed (Phase 0) and compile is green (Phase 3).

```bash
esphome run hoshi-voice-pe.yaml        # first time: USB flash, pick the port when asked
```

- Pick USB flash, not OTA — there's no running firmware to OTA onto yet.
- If `voice_kit.firmware:` is pinned and the version doesn't match the
  device, it triggers an XMOS DFU. When in doubt, comment the `firmware:`
  block out before this flash.

### ⚠️ After ANY flash: let the device boot undisturbed for ~60 seconds

Don't touch it, don't power-cycle it, don't open a serial monitor that resets
it. ESPHome's `safe_mode` counts fast resets; disturbing an early boot can
push it into a boot loop that looks like a dead device (app never starts,
nothing responds, no wake, no logs) but is actually just safe-mode being
overly cautious. The log line **"Boot seems successful; resetting boot loop
counter"** is your confirmation that it's stable. If you get stuck in this
state, a clean power cycle and another 60s of not touching it usually
resolves it before you reach for recovery.

### ⚠️ USB-Serial/JTAG dies on crash — use WiFi logs

The ESP32-S3's USB-Serial/JTAG interface goes away the moment the firmware
crashes, hangs, or resets — so a USB serial monitor is useless for debugging
exactly the moments you need it most. Once the device is on WiFi, use:

```bash
esphome logs hoshi-voice-pe.yaml --device hoshi-voice-pe.local
```

This reconnects automatically across resets and reliably catches crash
loops. A backtrace from `esp32.crash` shows up on the *next* clean boot, not
the crashing one.

### If the flash port disappears

Enter download mode manually: unplug USB → hold the center button (GPIO0) →
plug in USB-C → hold ~3 seconds. A charge-only cable or a flaky USB port on
your machine can eat a lot of time here — if in doubt, try a different cable
or a reboot of your flashing machine before assuming the device is at fault.

---

## Phase 5 — OTA (after a successful USB first flash)

```bash
esphome run hoshi-voice-pe.yaml        # finds the device over WiFi/mDNS -> OTA
```

**2.4 GHz only** from here on. OTA uses `ota_password` from secrets if set.

---

## Phase 6 — Live verification (needs your real backend running)

Your backend (STT/LLM/TTS pipeline behind the `wss://.../ws/audio` endpoint)
needs to be up and reachable. Verify, in order:

1. **Auth works:** the endpoint rejects a connection without a valid token
   and accepts one with it.
2. **Wake → first audible response**, timed over ~10 turns (median + P95).
3. **End-to-end:** wake → STT → LLM → TTS → audible response, sample rate
   correct (pitch should sound normal, not "chipmunked" — a sign of a
   sample-rate mismatch somewhere in the resample chain).
4. **Half-duplex:** mic muted during playback (confirm via logs, not just by
   ear).
5. **Barge-in:** center button mid-response aborts cleanly.
6. **Hardware mute switch** hard-gates the uplink.
7. Your own language, real sentences — does it actually sound usable, not
   just "technically works"?

---

## Mic-gain / AGC tuning — the most common cause of empty transcripts

**Symptom:** wake word triggers fine (LED goes to listening), then the turn
comes back empty / "no input", even though you clearly spoke.

**Root cause (found and fixed during development of this firmware):** the
wake-word detector has its own gain knob
(`micro_wake_word.microphone.gain_factor`) that only affects *its* audio
path. The uplink path that feeds your STT is a **separate** consumer of the
same raw mic stream and, unless explicitly configured, sees the mic at a
different (often much lower) effective level — so the wake detector "hears"
fine while your speech recognizer gets a signal too quiet to transcribe.

**The fix, already wired into this firmware:** `hoshi_ws_audio.mic_gain` (a
YAML config key) applies a separate, clip-safe digital gain to the uplink
path independent of the wake detector's gain. Default is a conservative
starting point — you will very likely need to tune it for your specific
room/device.

**Tuning procedure (do this on flash day, with your backend's logs open):**

1. Flash with the default `mic_gain`.
2. Run a few turns, watch the RMS level your backend logs per turn (or add
   logging on your firmware side if your backend doesn't expose this).
3. Aim for a healthy mid-range RMS — audible clear speech, no clipping. Too
   quiet → raise `mic_gain`. Clipping/distorted audio → lower it. Re-flash
   over OTA and repeat.
4. You're done when your speech recognizer reliably returns real transcripts
   instead of empty results, with no audible clipping.

**Digital gain has a ceiling:** `mic_gain` amplifies noise floor along with
signal. If your room is loud/reverberant enough that digital gain alone
can't get a clean SNR, the deeper lever is the XMOS front-end's own AGC
pipeline (`voice_kit` firmware) — a separate, more involved tuning path.
Exhaust `mic_gain` first.

---

## Rollback, any time a flash goes wrong

```bash
# Bootloader mode: unplug power -> hold center button -> plug in USB-C -> hold briefly
esptool --port /dev/cu.usbmodemXXXX write-flash 0x0 ~/voice-pe-stock-16MB.bin
```

---

## Gate checklist (short form)

| Gate | Condition |
|---|---|
| 0 | Stock backup verified (size+hash) + recovery rehearsed for real |
| 1 | YAML + component compile |
| 2 | `esphome` installed, `secrets.yaml` filled in (never committed) |
| 3 | `esphome compile` green |
| 4 | USB first flash done, leaf cert verified against your server |
| 5 | OTA works (2.4 GHz) |
| 6 | Live end-to-end verified against your real backend |
| 6m | `mic_gain` tuned — reliable non-empty transcripts, no clipping |

> **Golden rule:** until recovery is proven and rehearsed, nothing custom
> touches the device. A small clean step beats a bricked Voice PE.

## References

- [`README.md`](README.md) — build recipe, phases, target picture
- [`recovery/README.md`](recovery/README.md) — backup + recovery truth source
- `esphome/hoshi-voice-pe.yaml` · `esphome/components/hoshi_ws_audio/`
- [`../docs/PROTOCOL.md`](../docs/PROTOCOL.md) · [`../docs/ARCHITECTURE.md`](../docs/ARCHITECTURE.md)
- Upstream: `github.com/esphome/home-assistant-voice-pe@dev`, `home-assistant-voice.yaml`
