# Acceptance test — one flash, verify everything

A single-flash checklist that exercises the whole feature set: boot
animation, physical volume control + LED feedback, a full voice turn,
streaming playback, and stock wake-word regression. "It compiles" is not
"it works" — this is the bridge from a green build to an actual verified
device.

## 0. Setup

1. Flash (OTA once you have a working base image):
   ```bash
   cd firmware/esphome
   esphome run hoshi-voice-pe.yaml
   ```
2. **Let it boot undisturbed for ~60 seconds** (see RUNBOOK.md's safe-mode
   warning — don't touch it).
3. Watch logs over WiFi (USB serial dies on any crash):
   ```bash
   esphome logs hoshi-voice-pe.yaml --device hoshi-voice-pe.local
   ```

## 1. Boot / connecting animation (LED)

| Step | Expected |
|---|---|
| After boot, before WiFi is up | LED ring breathes blue — a few seconds |
| Once WiFi/network is up (IP assigned) | short green pulse (~1s, "ready") → LED off (idle) |

**Pass:** blue breathing is brief and ends as soon as WiFi connects (not at
first speech), then one green pulse. **Endless blue breathing → WiFi isn't
coming up** (check secrets/signal) — useful diagnostic, not a bug in the
audio path. Note: the connection to your Hoshi server opens lazily on the
*first* wake word, by design — the LED tracks WiFi status, not the backend
connection, so it doesn't sit there breathing until the first interaction.

## 2. Volume dial + LED feedback

| Step | Expected |
|---|---|
| Turn the dial | ~1.2s level overlay: color shifts dim-blue→green→amber, brightness tracks level. Log: `volume set to X.XX` |
| Turn down, then ask something | audibly quieter response |
| Turn up (to the ceiling), ask something | audibly louder response |
| Reboot, ask something | volume setting persisted |

Check rotation direction matches your expectation (clockwise = louder); if
inverted, swap `pin_a`/`pin_b` in the YAML (one line).

## 3. Full voice turn (regression)

Wake word → a real question.

| Phase | LED |
|---|---|
| Listening | cyan |
| Thinking/STT | amber |
| Speaking | green |

**Pass:** audible response, no crash, no cut-off audio. Log shows STT
result, TTS audio start, speaker volume assertion, speaking state, and no
"bus busy" errors.

## 4. Streaming for long responses

Ask something that produces a long, multi-sentence answer.

| Watch (log) | Expected with real streaming |
|---|---|
| `llm_audio` frames | multiple frames, increasing `seq` — not one giant block |
| Time from TTS-start to first audible sound | short (first sentence plays while the rest is still being synthesized) |
| Playback | continuous, no cuts/gaps between sentences, no underrun |
| LED | stays green through the whole response |

Whether responses stream sentence-by-sentence is a **backend** decision — the
device can play back incrementally (verified), but if you only ever see one
big `llm_audio` frame, that's a backend-side gap, not a firmware one.

## 5. Stock wake word regression

Say the wake word repeatedly from a few meters away — should trigger
reliably, no false triggers during silence/ambient noise.

*(A custom trained wake word is a separate, later step — see
[`../tools/wake/README.md`](../tools/wake/README.md).)*

## 6. What to capture per run

For each of 1–5: pass/fail, plus for streaming (4) the `llm_audio seq …` log
excerpt if it fails. That's what you need to tune (dial direction, VAD,
LED) or to know whether the gap is on the device side or the backend side.
