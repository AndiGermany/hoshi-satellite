# wake/ — training a custom on-device wake word

> **Goal:** an on-device `micro_wake_word` TFLite model for a custom wake
> phrase (this project used "Hey Hoshi") that runs on the Voice PE
> (ESP32-S3) and wakes the custom firmware in
> [`../../firmware/esphome/`](../../firmware/esphome/).
>
> **Doctrine:** local-first, free, diverse. Synthetic samples are the
> volume booster; **real recordings in your own room are the gold
> standard**, and the metric that matters is measured **in the room**, not
> on a validation split. Trust the microphone in your living room, not a
> pretty validation-accuracy number.

This is entirely optional groundwork — the firmware ships with (and
defaults to) the stock `micro_wake_word` model (`okay_nabu`); see
[`../../docs/DECISIONS.md`](../../docs/DECISIONS.md) (D5) for why a custom
wake word is a deliberately separate, later step.

---

## 0) TL;DR — the order of operations

1. **Pick your phrase.** A 3+ syllable phrase (e.g. "Hey Hoshi") beats a
   2-syllable one for false-accept rate — more distinct acoustic content to
   key on.
2. **Bulk synthetic positives (local, free):** `piper-sample-generator` ->
   hundreds to thousands of diverse synthetic speakers.
3. **Real recordings from your own household** — the single most valuable
   dataset — see [`recordings/README.md`](recordings/README.md) (not
   included in this repo; create your own).
4. **Augmentation:** background noise + room impulse responses (RIR) +
   gain/pitch/tempo jitter.
5. **Hard negatives:** words/phrases that sound close to your wake phrase,
   plus everyday household utterances — see
   [`hard-negatives.txt`](hard-negatives.txt).
6. **Train** (microWakeWord, not run as part of this repo, needs a GPU) ->
   tune the detection threshold + an optional 2nd-stage VAD.
7. **Measure in the room:** target false-accept rate < 0.5/hour and
   false-reject rate < 5%, measured from the real Voice PE microphone.
8. **Export:** `.tflite` + a JSON manifest -> ESPHome's `micro_wake_word` in
   [`../../firmware/esphome/`](../../firmware/esphome/).

---

## 1) Tools and sources

| Tool | Repo | Purpose |
|---|---|---|
| **microWakeWord** | `github.com/kahrendt/microWakeWord` -> moved to **`github.com/OHF-Voice/micro-wake-word`** | training framework -> TFLite-micro model for ESPHome |
| **piper-sample-generator** | `github.com/rhasspy/piper-sample-generator` | synthetic, diverse-speaker samples per phrase |
| **ESPHome `micro_wake_word`** | `esphome.io/components/micro_wake_word/` | on-device inference on the ESP32-S3 |
| **Model/negative data** | `huggingface.co/datasets/kahrendt/microwakeword` · `github.com/esphome/micro-wake-word-models` | pre-generated negative/ambient spectrograms, reference models |

> **Version note:** `kahrendt/microWakeWord` now redirects to the
> `OHF-Voice` org (`OHF-Voice/micro-wake-word`). Old git URLs still work via
> redirect, but prefer the OHF-Voice URL for anything new. The framework is
> explicitly an early release upstream — expect to need multiple training
> passes to land on a usable model.

---

## 2) Bulk synthetic positives with piper-sample-generator

**Why synthesis first?** Hundreds to thousands of distinct synthetic voices
cost nothing, run offline, and cover a wide range of pitch/tempo/timbre —
a cheap volume foundation before real recordings.

### One-time model download

The LibriTTS-R multi-speaker generator (speaker-embedding mixing) —
**English-only**:
```sh
wget -O models/en_US-libritts_r-medium.pt \
  'https://github.com/rhasspy/piper-sample-generator/releases/download/v2.0.0/en_US-libritts_r-medium.pt'
```
> **Language note:** the speaker-mixing generator (`.pt`, `--max-speakers`
> up to 904, `--slerp-weights`) is English-only. A short wake phrase is
> usually phonetically neutral enough that English multi-speaker synthesis
> still provides a useful volume base regardless of your target language.
> For non-English pronunciation, additionally use regular Piper voices
> (`.onnx` + `.onnx.json`) from that language's voice catalog (rotate
> through several `--model` values). piper-sample-generator is currently at
> v3.1.0 (installable via `pip install piper-sample-generator`).

### Generating diverse positives (see `generate-samples.sh`)

Per the microWakeWord training notebook (`generate_samples.py`):
```sh
python3 piper-sample-generator/generate_samples.py "Hey Hoshi" \
  --max-samples 2000 \
  --batch-size 100 \
  --output-dir generated_samples/
```
Diversity knobs:
- `--length-scales` -> speaking tempo (rotate across batches)
- `--slerp-weights` / `--max-speakers` (<904) -> speaker blending/count (English generator)
- multiple `--model` values -> mix in other-language voices

> **Pronunciation tip** (from the upstream notebook): if the TTS
> pronunciation of your phrase is off, a phonetic spelling of the phrase
> can help (the notebook's own example uses a phonetic respelling for
> "computer"). Try a few spelling variants of your phrase and pick whichever
> the TTS pronounces most naturally.

**Volume:** the reference notebook uses 1000+ synthetic positives; aim for
1000-5000 for a reasonably robust model of your own, then enrich with real
recordings (below).

---

## 3) Real household recordings — the most valuable dataset

Synthesis covers voices, but not **your room**: your device's mic frontend
(XMOS XU316 on this hardware), your walls' reflections, your background
noise (dishwasher, TV, kids). **Real recordings of your wake phrase from
your own household are the only thing that meaningfully lowers in-room
false-reject rate.** Each one is worth many times a synthetic sample.

Recording guidance, volume targets, and file naming belong in a
`recordings/README.md` you create for your own setup (not included here —
this is inherently personal/household-specific content that doesn't belong
in a shared public repo). Aim for 16 kHz mono, varied distance/angle/room/
background noise.

These real clips feed the same `Clips` loader as the synthetic ones (same
`--output-dir` pattern, or an additional `input_directory`) — see the "REAL
RECORDINGS" section in `generate-samples.sh`.

---

## 4) Augmentation — room, noise, gain/pitch/tempo

microWakeWord augments clips during training. Example `Augmentation` config
from the upstream training notebook:

```python
from microwakeword.audio.clips import Clips
from microwakeword.audio.augmentation import Augmentation
from microwakeword.audio.spectrograms import SpectrogramGeneration

clips = Clips(input_directory='generated_samples',
              file_pattern='*.wav',
              remove_silence=False,
              split_count=0.1)

augmenter = Augmentation(
    augmentation_duration_s=3.2,
    augmentation_probabilities={
        "SevenBandParametricEQ": 0.1,
        "TanhDistortion":        0.1,
        "PitchShift":            0.1,
        "BandStopFilter":        0.1,
        "AddColorNoise":         0.1,
        "AddBackgroundNoise":    0.75,  # background noise — the most important lever
        "Gain":                  1.0,   # gain — always on
        "RIR":                   0.5,   # room impulse response — room reverb
    },
    impulse_paths=['mit_rirs'],                    # RIR set (MIT) — fetch separately
    background_paths=['fma_16k', 'audioset_16k'],  # background noise sets — fetch separately
    background_min_snr_db=-5,
    background_max_snr_db=10,
    min_jitter_s=0.195,
    max_jitter_s=0.205)
```
The training/spectrogram step additionally applies SpecAugment (masking
time/frequency bands) and the micro-frontend's own noise suppression +
auto-gain-control.

> Tempo is mostly covered at synthesis time via `--length-scales`; pitch,
> gain, background noise, and RIR come from augmentation — between the two,
> all of the usual diversity axes are covered.

**Generating spectrograms** (a ragged mmap for fast loading):
```python
from mmap_ninja.ragged import RaggedMmap
spectrograms = SpectrogramGeneration(clips=clips, augmenter=augmenter, slide_frames=10, step_ms=10)
RaggedMmap.from_generator(
    out_dir='generated_features/wakeword_mmap',
    sample_generator=spectrograms.spectrogram_generator(split='train'),
    batch_size=100, verbose=True)
```

---

## 5) Hard negatives — targeting false triggers deliberately

Generic negatives (speech/music/silence) come from the
`kahrendt/microwakeword` HF dataset (`speech.zip`, `no_speech.zip`,
`dinner_party.zip`, plus `*_eval`). Hard negatives are the extra layer:
words/phrases that *sound* close to your wake phrase, or that come up often
in everyday household conversation. They force the model to draw a sharp
boundary around the real phrase.

See [`hard-negatives.txt`](hard-negatives.txt) for a starter list. Synthesize
these the same way as positives, and — better — record them for real in
your household too, then feed them in as the negative class. A high
`negative_class_weight` in the training config (a value around 20 is a
reasonable reference point) makes false triggers expensive during training.

---

## 6) Tunable threshold + optional 2nd-stage VAD

On-device robustness isn't only about the model — two ESPHome-side knobs
matter as much:

- **`probability_cutoff`** (in the JSON manifest, see §8): the detection
  threshold. Higher -> fewer false accepts, more false rejects. This is
  your primary tuning knob after in-room measurement.
- **`sliding_window_size`**: how many windows the probability is averaged
  over (higher -> more sluggish, more robust).
- **2nd-stage VAD** (the `vad:` block in ESPHome's `micro_wake_word`): its
  own `probability_cutoff` + `sliding_window_size`, filters out non-speech
  and noticeably lowers false-accept rate without having to make the wake
  model itself more conservative.

```yaml
# Sketch for ../../firmware/esphome/ — the actual block lives there
micro_wake_word:
  vad:                       # 2nd-stage voice-activity detection
  models:
    - model: /config/models/hey_hoshi.json   # your custom manifest (see §8)
      id: hey_hoshi_model
      probability_cutoff: 0.97               # tune in-room
      sliding_window_size: 5
```

---

## 7) Metrics — measured in the room, not on the split

| Metric | Target | How measured |
|---|---|---|
| **FAR** (false-accept rate) | < 0.5/hour | device runs passively in a lived-in room (TV/conversation/kitchen), count false wakes/hour |
| **FRR** (false-reject rate) | < 5% | say your wake phrase N times across distance/angle/background, count misses |

- Measure with the **real device microphone** (this board's XMOS frontend),
  in **your own** room — not on the training validation split. A 99%
  validation score means nothing if the dishwasher triggers it.
- Keep a record of the measurement runs (median + P95, >=10 trials) —
  mirrors the wake-to-first-response measurement described in
  [`../../docs/MEASUREMENTS.md`](../../docs/MEASUREMENTS.md).
- Tuning loop: measure -> adjust `probability_cutoff`/VAD -> measure again.
  A live listening test by an actual person is the final gate, not a
  validation-split number.

---

## 8) Export -> ESPHome `micro_wake_word`

Training produces a streamed, quantized TFLite model:
```python
# Training/export call (run in the training notebook, not in this repo)
!python -m microwakeword.model_train_eval \
  --training_config='training_parameters.yaml' \
  --train 1 --test_tflite_streaming_quantized 1 --use_weights "best_weights" \
  mixednet --pointwise_filters "64,64,64,64" --stride 3

# Export artifact:
#   trained_models/wakeword/tflite_stream_state_internal_quant/stream_state_internal_quant.tflite
```
Alongside it, a JSON manifest that the ESPHome component reads. Required
fields (per `esphome.io/components/micro_wake_word/` and the
`esphome/micro-wake-word-models` format):

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
    "probability_cutoff": 0.97,
    "sliding_window_size": 5,
    "feature_step_size": 10,
    "tensor_arena_size": 30000,
    "minimum_esphome_version": "2025.5.0"
  }
}
```
> Set `probability_cutoff`, `sliding_window_size`, `tensor_arena_size`, and
> `minimum_esphome_version` after your own training/measurement — the
> values above are placeholders. `feature_step_size` must match the
> `step_ms`/`window_step_ms` used during training (10 is the reference
> value here).

**Placement:** put `hey_hoshi.tflite` + `hey_hoshi.json` wherever your
ESPHome config expects local files (e.g. `/config/models/`) or under
[`../../firmware/esphome/`](../../firmware/esphome/) in this repo, and
reference it in the YAML (`model: /config/models/hey_hoshi.json`). Flashing
a new wake model is a real hardware step — see
[`../../firmware/README.md`](../../firmware/README.md).

---

## 9) Cloud TTS voices — at most a small optional add-on, never the base

- A cloud TTS API typically offers only a handful of voices — using it as
  the *primary* source risks overfitting the model to too few speaker
  timbres.
- It also works against the local-first, free doctrine this tooling
  otherwise follows.
- Use it, if at all, as a *small* percentage mixed in for extra timbre
  variety, and only if the added cost/latency of the API calls is
  acceptable to you. The foundation should remain: piper-sample-generator
  (volume) + real household recordings (quality).

`openai-gen.sh` in this directory is a small example script for generating
a handful of such samples via the OpenAI TTS API, if you choose to use it —
it requires your own API key and is entirely optional.

---

## 10) Directory layout

```
wake/
  README.md              <- you are here
  generate-samples.sh    <- sketch: synthesize positives (review before running)
  openai-gen.sh           <- optional: small cloud-TTS sample mix-in
  hard-negatives.txt      <- starter list of close-sounding/common phrases to reject
  models/                 <- exported .tflite + .json manifest go here
  train/                  <- training runbook + parameters (GPU/cloud, not run locally)
```

## Sources

- microWakeWord / micro-wake-word: `github.com/kahrendt/microWakeWord` -> `github.com/OHF-Voice/micro-wake-word`
- piper-sample-generator (v3.1.0): `github.com/rhasspy/piper-sample-generator`
- ESPHome `micro_wake_word`: `esphome.io/components/micro_wake_word/`
- Negative/model data: `huggingface.co/datasets/kahrendt/microwakeword` · `github.com/esphome/micro-wake-word-models`
