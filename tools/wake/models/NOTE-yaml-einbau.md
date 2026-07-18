# NOTE — wiring `hey_hoshi.json` into the ESPHome firmware

> **Documentation only.** This note does not change the firmware YAML and
> does not flash anything — that's a real hardware step you do by hand.
> This just documents *what the integration looks like*.

---

## Files (format verified against `esphome/micro-wake-word-models` / `okay_nabu`)

| File | Purpose |
|---|---|
| `hey_hoshi.json` | model manifest (this directory, a template). `"model"` points relatively at the `.tflite`. |
| `hey_hoshi.tflite` | the trained, streamed, quantized model (produced by `../train/TRAIN-RUNBOOK.md`, not checked into this repo). |

Both files MUST sit **next to each other**. ESPHome loads the JSON; the
JSON references the `.tflite` via a relative `"model"` field.

---

## Manifest fields (what to set after training)

```json
{
  "type": "micro",
  "wake_word": "Hey Hoshi",
  "author": "your name / project",
  "website": "",
  "model": "hey_hoshi.tflite",
  "trained_languages": ["en"],
  "version": 2,
  "micro": {
    "probability_cutoff": 0.97,      // PLACEHOLDER — tune in your own room (higher -> fewer false accepts, more false rejects)
    "feature_step_size": 10,         // MUST match window_step_ms/step_ms used in training (10)
    "sliding_window_size": 5,        // PLACEHOLDER — more windows -> more sluggish/robust
    "tensor_arena_size": 26080,      // PLACEHOLDER — use the value reported by the export step (okay_nabu reference: 26080)
    "minimum_esphome_version": "2025.5.0"
  }
}
```

> Placeholder values initially come from `../train/training_parameters.yaml`;
> the final values get tuned after in-room measurement (target FAR < 0.5/h,
> FRR < 5%) — see `../README.md` §6/§7.

---

## YAML entry (a local file instead of a GitHub URL)

By default `../../firmware/esphome/hoshi-voice-pe.yaml` uses the stock
`okay_nabu` model via a GitHub URL. For a custom wake word, the **`model:`
entry** switches to a **local file** — the rest of the `micro_wake_word:`
block (microphone, vad, on_wake_word_... handlers) stays byte-identical.

**Stock, today (trimmed):**
```yaml
micro_wake_word:
  id: mww
  microphone:
    microphone: i2s_mics
    channels: 1
    gain_factor: 4
  stop_after_detection: false
  models:
    - model: https://github.com/kahrendt/microWakeWord/releases/download/okay_nabu_20241226.3/okay_nabu.json
      id: okay_nabu
    - model: https://github.com/kahrendt/microWakeWord/releases/download/stop/stop.json
      id: stop
      internal: true
  vad:
```

**With a custom wake word (local file — this is what the entry would look like):**
```yaml
micro_wake_word:
  id: mww
  microphone:
    microphone: i2s_mics
    channels: 1
    gain_factor: 4
  stop_after_detection: false
  models:
    # Custom wake model — local file instead of a GitHub URL. hey_hoshi.tflite
    # must sit next to hey_hoshi.json in the same directory (e.g. /config/models/).
    - model: /config/models/hey_hoshi.json
      id: hey_hoshi
      probability_cutoff: 0.97      # on-device tuning knob (overrides the manifest default)
      sliding_window_size: 5        # on-device tuning knob (overrides the manifest default)
    # Keep the "stop" model (voice-cancel of an in-progress turn) — unchanged.
    - model: https://github.com/kahrendt/microWakeWord/releases/download/stop/stop.json
      id: stop
      internal: true
  vad:                              # keep the 2nd-stage VAD (lowers false-accept rate) — unchanged
```

### Notes
- **`probability_cutoff` / `sliding_window_size` set directly on the model
  entry** override the manifest defaults and are the fastest on-device
  tuning knobs (no model reflash needed, just a YAML change + OTA). Measure
  in your own room first, then adjust (see `../README.md` §6).
- **Path:** `/config/models/` is the conventional location on the
  Home-Assistant/ESPHome side. Within this firmware repo, the file could
  instead live under `../../firmware/esphome/` — adjust the `model:` path
  (relative or absolute) accordingly.
- **Keep the `stop` model**, or voice-cancel of an in-progress turn stops
  working.
- The rest of the block (`on_wake_word_detected:` -> `hoshi_ws.start_turn(...)`,
  LED handling, `vad:`) stays **untouched** — only the one `model:` entry
  changes.
