# recordings/ — real wake-phrase recordings (gold-standard training data)

This folder is where you put **your own** real recordings of your wake
phrase, said by the people who'll actually use the device, in the actual
room(s) it'll live in. It's intentionally empty in this repo — these
recordings are personal audio data and shouldn't be committed to a shared
repo. Treat this directory as gitignored/local-only.

Synthetic samples (see [`../README.md`](../README.md) §2) give you volume
and speaker diversity for free. Real recordings give you something
synthesis can't: your specific microphone hardware, your room's acoustics
and reflections, and your household's real background noise. Per
[`../README.md`](../README.md) §3, real recordings are the single biggest
lever for lowering false-reject rate in your actual room — they don't
replace the synthetic pool, they enrich it.

## What to record

- **The wake phrase itself**, spoken naturally — not shouted, not
  over-enunciated. Include the tone you'd actually use ("hey, quick
  question" energy, not "reading it off a card" energy).
- **Multiple speakers**, if more than one person will use the device.
- **Multiple distances** from the device: close (~1m), normal
  conversational distance (~2-3m), and far (~4-5m+, another room if your
  layout allows).
- **Multiple angles** relative to the device's mic array, not just
  directly facing it.
- **Multiple background conditions**: silence, TV/music playing, normal
  household noise (conversation, kitchen, etc.) — this matters more for
  the *negative* recordings (see below) but varying it for positives too
  makes the model more robust.
- **A handful of near-miss negatives**, if you can — real recordings of
  the phrases in [`../hard-negatives.txt`](../hard-negatives.txt), or
  anything you've noticed the wake word actually mis-triggering on.
  Real false-trigger audio is the most valuable negative data there is.

## How many

There's no hard minimum, but as a rough guide: even 20-50 real positive
recordings per speaker, spread across the distance/angle/background
variations above, meaningfully improve in-room false-reject rate over
synthetic-only data. More is better; quality (real variation) matters more
than raw count once you're past a baseline.

## Format and naming

Match what [`../generate-samples.sh`](../generate-samples.sh) expects:

- **16 kHz mono, 16-bit PCM WAV.** If your recording device captures at a
  different rate, resample first, e.g.:
  ```bash
  ffmpeg -i raw_recording.wav -ar 16000 -ac 1 -sample_fmt s16 clean_recording.wav
  ```
- Put positive (wake-phrase) recordings directly in this directory as
  `.wav` files — `generate-samples.sh`'s `TODO[REAL]` block (see that
  script) picks up anything matching `*.wav` here and resamples/copies it
  into the training pool.
- Put negative recordings (near-misses, false triggers) in a `negatives/`
  subdirectory here, following the same format.
- Naming doesn't matter mechanically (the script just enumerates `*.wav`),
  but something like `<speaker>_<distance>_<take>.wav` (e.g.
  `speaker1_near_03.wav`) makes it easy to track coverage as you go.

## Privacy note

These are voice recordings of real people in your home. Keep them local,
don't commit them to any shared repo (this directory should stay
gitignored), and only use them for training your own wake-word model.
