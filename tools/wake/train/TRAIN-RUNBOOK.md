# TRAIN-RUNBOOK — custom wake-word microWakeWord training

> **Scope of this document:** the pipeline from *"augmented dataset ->
> training -> `.tflite` + JSON"*. **No training happens on this machine** —
> training runs on an **external GPU / Colab / Docker environment**, not on
> a RAM-constrained laptop with no CUDA. This runbook is the exact,
> reviewed procedure for that external step.

---

## 0) TL;DR — the pipeline once data exists

```
[1] Generate data locally    ->  ../generate-samples.sh   (turnkey, local machine)
[2] Upload the dataset       ->  positives/ + negatives/  to the GPU box
[3] Trainer + external sets  ->  micro-wake-word + RIR/background + HF negatives
[4] Build spectrograms        ->  ragged mmaps (positives/hard_negatives/generic)
[5] Train + export            ->  model_train_eval -> stream_state_internal_quant.tflite
[6] Finalize the manifest     ->  ../models/hey_hoshi.json (values from training)
[7] Back into the firmware    ->  hey_hoshi.tflite + .json -> ESPHome (a real hardware step)
```

The two steps that need a human hands-on decision: providing real
recordings (step 1) and flashing at the end (step 7). Everything in between
is mechanical and detailed below.

---

## 1) Input data (produced locally, local-first)

Produced by `../generate-samples.sh` (turnkey, already proven to work):

```
generated-samples/dataset-v1/
  positives/   pos_*.wav     16 kHz mono PCM16   (synthetic wake-phrase samples)
  negatives/   neg_*_*.wav   16 kHz mono PCM16   (hard negatives from hard-negatives.txt)
```

**Plus gold-standard data (highest priority):** real household recordings
from `../recordings/` — recorded per a `recordings/README.md` you write for
your own setup, then resampled into `positives/` (real negative recordings
similarly into `negatives/`). `generate-samples.sh` has a `TODO[REAL]` block
for this — uncomment it once you have recordings.

> **Volume guidance:** aim for 1000-5000 synthetic positives + 50-500 real
> ones. Roughly 20-40 hard negatives per phrase. Generic negatives come from
> an external dataset (step 3).

---

## 2) Target environment (a GPU box: Colab / Docker / Linux+CUDA)

**Why not a laptop:** `micro-wake-word`/TensorFlow effectively need CUDA and
substantial RAM for augmentation and spectrogram generation across
thousands of clips. A modest Apple Silicon machine is enough for
*synthesis* (step 1, already proven) but not for training.

**Recommended:** the official training notebook (Google Colab, GPU runtime)
or a CUDA Docker container. Requirements:

```bash
# Python 3.10-3.11 (TF-compatible). On the GPU box, not locally.
git clone https://github.com/OHF-Voice/micro-wake-word   # (the old kahrendt URL redirects here)
pip install -e ./micro-wake-word
pip install audiomentations mmap-ninja tensorflow
```

**Download external datasets (large — deliberately not fetched locally,
gigabyte range):**

| Set | Source | Purpose |
|---|---|---|
| Generic negatives | `huggingface.co/datasets/kahrendt/microwakeword` -> `speech.zip`, `no_speech.zip`, `dinner_party.zip` (+ `*_eval`) | negative class (speech/silence/party noise) |
| RIR (room impulse responses) | the MIT RIR set (`mit_rirs`) | `RIR` augmentation |
| Background audio | `fma_16k`, `audioset_16k` (16 kHz) | `AddBackgroundNoise` augmentation |

> These are several GB — check licensing (FMA/AudioSet) before use. Only
> after downloading, fill in the real paths in `training_parameters.yaml`
> (`impulse_paths`/`background_paths`).

---

## 3) Dataset layout on the GPU box

```
work/
  clips/
    positives/          <- pos_*.wav + real_*.wav (16 kHz mono)
    hard_negatives/      <- neg_*_*.wav (16 kHz mono)
  external/
    generic_negatives/   <- unpacked from speech.zip / no_speech.zip / dinner_party.zip
    mit_rirs/            <- RIR WAVs
    fma_16k/  audioset_16k/   <- background WAVs
  features/              <- (step 4) ragged mmaps land here
  trained_models/        <- (step 5) checkpoints + export
  training_parameters.yaml   <- copied from this train/ folder, paths adjusted
```

---

## 4) Generating spectrograms (ragged mmap, per class)

Run once for EACH class folder (`positives`, `hard_negatives`,
`generic_negatives`) — produces the `features/*_mmap` that
`training_parameters.yaml` references. (Augmentation is applied HERE; the
values come from the YAML.)

```python
from microwakeword.audio.clips import Clips
from microwakeword.audio.augmentation import Augmentation
from microwakeword.audio.spectrograms import SpectrogramGeneration
from mmap_ninja.ragged import RaggedMmap

clips = Clips(input_directory='clips/positives',   # adjust per class
              file_pattern='*.wav', remove_silence=False, split_count=0.1)

augmenter = Augmentation(
    augmentation_duration_s=3.2,
    augmentation_probabilities={
        "SevenBandParametricEQ": 0.1, "TanhDistortion": 0.1, "PitchShift": 0.1,
        "BandStopFilter": 0.1, "AddColorNoise": 0.1,
        "AddBackgroundNoise": 0.75, "Gain": 1.0, "RIR": 0.5,
    },
    impulse_paths=['external/mit_rirs'],
    background_paths=['external/fma_16k', 'external/audioset_16k'],
    background_min_snr_db=-5, background_max_snr_db=10,
    min_jitter_s=0.195, max_jitter_s=0.205)

spectrograms = SpectrogramGeneration(clips=clips, augmenter=augmenter,
                                     slide_frames=10, step_ms=10)
RaggedMmap.from_generator(
    out_dir='features/positives_mmap',             # adjust per class
    sample_generator=spectrograms.spectrogram_generator(split='train'),
    batch_size=100, verbose=True)
```

> `step_ms=10` MUST match `window_step_ms` (in the YAML) and
> `feature_step_size` (in the JSON manifest) — all three = **10**. Load
> generic negatives without augmentation (`augmenter=None`).

---

## 5) Training + exporting TFLite

```bash
python -m microwakeword.model_train_eval \
  --training_config='training_parameters.yaml' \
  --train 1 \
  --test_tflite_streaming_quantized 1 \
  --use_weights "best_weights" \
  mixednet --pointwise_filters "64,64,64,64" --stride 3
```

Export artifact:

```
trained_models/hey_hoshi/tflite_stream_state_internal_quant/stream_state_internal_quant.tflite
```

-> rename to **`hey_hoshi.tflite`**.

> **Set expectations (per upstream):** this is explicitly an "early
> release ... requires a lot of experimentation" framework. Budget for
> multiple rounds; iterate on architecture/steps/class weights. The metric
> that ultimately matters is measured **in the room** (target FAR < 0.5/h,
> FRR < 5%), not the validation split.

---

## 6) Finalizing the JSON manifest

Template: **`../models/hey_hoshi.json`** (format matches the stock
`okay_nabu` model). After training, overwrite the placeholders with real
values:

- `probability_cutoff` / `sliding_window_size` -> start from the YAML
  values; tune for real in your own room afterward.
- `tensor_arena_size` -> the value reported by the export step (reference
  for `okay_nabu`: 26080).
- `feature_step_size` -> 10 (must match training).
- `trained_languages` -> whatever languages your training data covers.
- `minimum_esphome_version` -> the ESPHome version you're flashing against.

`hey_hoshi.tflite` and `hey_hoshi.json` MUST sit next to each other (the
manifest references the `.tflite` via a relative `"model"` field).

---

## 7) Back into the firmware

Put `hey_hoshi.tflite` + `hey_hoshi.json` wherever your ESPHome config
expects local model files (e.g. `/config/models/`) or under
`../../firmware/esphome/` in this repo. **YAML entry:** see
`../models/NOTE-yaml-einbau.md` (switches from a GitHub URL to a local
file; `probability_cutoff`/`sliding_window_size` become on-device tuning
knobs).

**Flashing needs your hands physically at the hardware** — this runbook
doesn't touch the YAML and doesn't flash anything itself.

---

## Appendix — what's already done vs. what stays external

| Step | Status | Where |
|---|---|---|
| Synthesis pipeline (positives + hard negatives + 16k resample) | done, smoke-tested | locally, `../generate-samples.sh` |
| Training config | template ready | `./training_parameters.yaml` |
| JSON manifest format | template ready | `../models/hey_hoshi.json` |
| YAML integration doc | ready | `../models/NOTE-yaml-einbau.md` |
| Real recordings | pending — needs you | `../recordings/README.md` (create your own) |
| Generic negatives + RIR + background (GB-scale) | pending — external download | step 2 (GPU box) |
| Training + export | pending — external GPU | steps 4-5 (not local) |
| Flash | pending — needs you at the hardware | step 7 |
