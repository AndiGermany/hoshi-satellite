# SETUP-LOG — wake-word sample generation setup notes (Piper)

> Worked example of getting `piper-sample-generator` installed and producing
> its first batch on an Apple Silicon Mac (should translate directly to
> Linux/x86 too — the pitfalls below are Python packaging issues, not
> platform-specific). Scope: only `tools/wake/`. No device, no flash, no
> training, nothing else touched.
>
> Result: the pipeline works end to end — install succeeded, a 25-sample
> test batch generated and resampled to 16 kHz.

---

## Result (TL;DR)

- venv: `wake/.venv` (Python 3.14, recent pip) — roughly 1 GB.
- piper-sample-generator: `wake/piper-sample-generator/` (shallow clone,
  `master` HEAD, pyproject version 3.2.0).
- Generator model: `wake/piper-sample-generator/models/en_US-libritts_r-medium.pt`
  (204,089,915 bytes, ~208 MB).
- Test batch: 25 WAV files in `wake/generated-samples/test-v0/` (22050 Hz
  mono 16-bit, 0.56-0.77s each).
- 16 kHz variant (resampled): 25 WAV files in
  `wake/generated-samples/test-v0-16k/` (16000 Hz mono 16-bit).
- Generation time: roughly 6.5s for 25 samples on Apple Silicon's MPS GPU
  backend. Negligible RAM impact.

---

## Versions (as installed in the venv at the time)

| Package | Version |
|---|---|
| python | 3.14.5 |
| pip | 26.1.2 |
| piper-sample-generator | 3.2.0 (`master` HEAD) |
| torch | 2.12.0 (arm64 wheel, MPS available) |
| torchaudio | 2.11.0 |
| piper-tts | 1.3.0 |
| onnxruntime | 1.26.0 |
| audiomentations | 0.33.0 |
| numpy | 2.4.6 |
| webrtcvad | 2.0.10 (built from sdist — needs a C compiler) |
| librosa / numba / scipy / scikit-learn | 0.10.2.post1 / 0.65.1 / 1.17.1 / 1.9.0 |

Dependencies per `pyproject.toml`: `audiomentations==0.33.0`,
`piper-tts==1.3.0`, `numpy>=2,<3`, `torch>=2,<3`, `torchaudio`, `webrtcvad`.

---

## Exact working commands

### 1. venv + pip
```bash
cd tools/wake
python3 -m venv .venv
.venv/bin/python -m pip install --upgrade pip
```

### 2. Clone the repo + install into the venv
```bash
git clone --depth 1 https://github.com/rhasspy/piper-sample-generator.git piper-sample-generator
.venv/bin/pip install ./piper-sample-generator
```

### 3. Download the generator model (204 MB)
URL: `https://github.com/rhasspy/piper-sample-generator/releases/download/v2.0.0/en_US-libritts_r-medium.pt`
```bash
cd tools/wake/piper-sample-generator
curl -fL -o models/en_US-libritts_r-medium.pt \
  'https://github.com/rhasspy/piper-sample-generator/releases/download/v2.0.0/en_US-libritts_r-medium.pt'
# REQUIRED CHECK: size MUST be exactly 204089915 bytes.
test "$(stat -f%z models/en_US-libritts_r-medium.pt)" -eq 204089915 && echo "SIZE OK" || echo "TRUNCATED"
```
The matching config `models/en_US-libritts_r-medium.pt.json` ships with the repo.

### 4. Generate a test batch (~25 samples)
```bash
cd tools/wake/piper-sample-generator
mkdir -p ../generated-samples/test-v0
../.venv/bin/python -m piper_sample_generator 'Hey Hoshi' \
  --model models/en_US-libritts_r-medium.pt \
  --max-samples 25 \
  --batch-size 5 \
  --max-speakers 700 \
  --length-scales 0.9 1.0 1.1 \
  --slerp-weights 0.0 0.25 0.5 0.75 1.0 \
  --output-dir ../generated-samples/test-v0/
```

### 5. Resample to 16 kHz mono (microWakeWord's requirement)
The generator produces 22050 Hz (no sample-rate flag). microWakeWord needs
16 kHz. The bundled `augment` module resamples:
```bash
cd tools/wake/piper-sample-generator
../.venv/bin/python -m piper_sample_generator.augment --sample-rate 16000 \
  ../generated-samples/test-v0 ../generated-samples/test-v0-16k
```

---

## Two pitfalls worth documenting (both solved)

1. **`python -m piper_sample_generator` MUST run from the repo root**
   (`.../wake/piper-sample-generator/`), not an arbitrary cwd. Reason:
   `pip install ./piper-sample-generator` only packages
   `piper_sample_generator*`, not the sibling package `piper_train` (VITS)
   that it also needs. Running from the repo directory puts `piper_train/`
   on the import path, so the import succeeds. Otherwise you'll hit
   `ModuleNotFoundError: No module named 'piper_train'`. (An alternative
   would be `pip install -e` plus a `PYTHONPATH` pointing at the repo —
   running from the repo directory is the simpler fix.)

2. **The model download truncates easily.** The first attempt here produced
   only a 17 MB file (a valid zip/PK header, but
   `RuntimeError: PytorchStreamReader failed ... failed finding central
   directory`, i.e. cut off mid-download). Also: never run two `curl`
   downloads in parallel against the same target file — they'll overwrite
   each other and corrupt the result. Always do one clean download and
   **verify the size is exactly 204089915 bytes** before your first
   generation run.

---

## Scaling up to a full batch

Same command as step 4, just raise `--max-samples` (1000-5000 is a
reasonable target per `generate-samples.sh`):
```bash
cd tools/wake/piper-sample-generator
../.venv/bin/python -m piper_sample_generator 'Hey Hoshi' \
  --model models/en_US-libritts_r-medium.pt \
  --max-samples 2000 \
  --batch-size 25 \
  --max-speakers 700 \
  --length-scales 0.8 0.9 1.0 1.1 1.2 \
  --slerp-weights 0.0 0.25 0.5 0.75 1.0 \
  --noise-scales 0.667 \
  --noise-scale-ws 0.8 \
  --output-dir ../generated-samples/full-v0/
# then resample:
../.venv/bin/python -m piper_sample_generator.augment --sample-rate 16000 \
  ../generated-samples/full-v0 ../generated-samples/full-v0-16k
```
Notes:
- Keep `--batch-size` moderate on Apple Silicon MPS (RAM budget is tight).
  5-25 is fine; lower it if you hit OOM.
- To mix in pronunciation for another language: run additional passes with
  real Piper voices via repeated `--model voiceX.onnx` (per-language voices
  from `huggingface.co/rhasspy/piper-voices`). The LibriTTS-R generator
  itself is English-only.
- Real recordings from `recordings/` remain the most valuable source (don't
  let synthesis replace them).
- Background-noise/RIR/gain augmentation belongs in the microWakeWord
  trainer (out of scope here) — see `wake/README.md` +
  `wake/generate-samples.sh`.

---

## Cloud-TTS refinement hook

- Requires `OPENAI_API_KEY` (or a key file — see `openai-gen.sh`) to be
  available; not needed for the local pipeline above.
- Doctrine per `generate-samples.sh`: cloud TTS is NOT a volume source
  (only a handful of voices -> overfit risk). Synthesis volume comes
  locally/for-free from Piper; cloud TTS at most for a small, targeted
  quality/accent refinement pass on a subset of samples.
- See `openai-gen.sh` for a minimal example script that re-speaks a small
  sample of phrases through a cloud TTS API and resamples them to 16 kHz
  mono the same way.

---

## Resource notes (for planning your own run)

- RAM footprint during generation + resample was negligible (25 samples in
  ~6.5s).
- Disk: the venv is roughly 1 GB, the model ~208 MB. A full batch (thousands
  of WAVs at ~30 KB each) stays in the low hundreds of MB.
- Recent `torch` releases ship arm64 wheels (CPU/MPS), so a recent Python on
  Apple Silicon works out of the box, with MPS actively used.
- No training, no flash — this only exercises the local synthesis step.
