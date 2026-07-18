#!/usr/bin/env python3
"""Offline Silero-VAD diagnostic on a REAL speech WAV (no device, no Hoshi).

Loads a real 16k mono PCM16 speech clip, strips the container down to raw int16
PCM (exactly what the device delivers on `_handle_audio`), and pushes it through
the EXACT live code path: SileroModel (the shared ONNX model) driven by
SpeechVad.feed() (the same 512-sample slicing + int16->float32 normalisation the
bridge runs per turn). It prints the max + mean Silero speech probability over
the whole clip, plus whether the hysteresis machine ever reached "speech start".

WHY: the live bug was Silero logging prob=0.00 the whole time while the user
clearly spoke -> "speech start" never fired -> no {stop} -> a ~100 s clip got
flushed to Whisper (15 s timeout, empty transcript). If the feed/normalisation
is wrong (e.g. int16 not scaled to [-1,1]), real speech reads as prob~0. This
tool is the offline proof: REAL speech MUST reach prob>0.5 somewhere.

Run (numpy + onnxruntime live in the bridge venv):
  .venv/bin/python diag_vad.py
  .venv/bin/python diag_vad.py --wav /path/to/other.wav --threshold 0.5

Exit code 0 if max-prob > 0.5 (normalisation OK), 1 otherwise (feed is the bug).
"""
from __future__ import annotations

import argparse
import io
import sys
import wave
from pathlib import Path

# Default real-speech clip: 16k mono PCM16, says "Hey Hoshi".
DEFAULT_WAV = (
    Path(__file__).resolve().parents[1]
    / "wake" / "generated-samples" / "openai-v0" / "positives" / "pos_nova_neutral.wav"
)

from napi_bridge import (  # noqa: E402  (import after module docstring/consts)
    DEVICE_SAMPLE_RATE,
    SILERO_FRAME_SAMPLES,
    SileroModel,
    SpeechVad,
    _default_silero_model,
)


def load_wav_pcm16(path: Path) -> bytes:
    """Decode a WAV container -> raw int16 PCM16 LE bytes (mono 16k expected).

    Uses the stdlib `wave` module so non-data chunks (LIST/INFO etc.) are skipped
    correctly -- a naive 44-byte header strip would corrupt files that carry a
    LIST chunk before `data` (this very sample does).
    """
    with wave.open(io.BytesIO(path.read_bytes()), "rb") as wf:
        sr = wf.getframerate()
        ch = wf.getnchannels()
        sw = wf.getsampwidth()
        pcm = wf.readframes(wf.getnframes())
    if sw != 2:
        raise ValueError(f"expected 16-bit PCM, got sampwidth={sw}")
    if ch != 1:
        raise ValueError(f"expected mono, got channels={ch}")
    if sr != DEVICE_SAMPLE_RATE:
        raise ValueError(f"expected {DEVICE_SAMPLE_RATE} Hz, got {sr}")
    return pcm


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Offline Silero-VAD diagnostic on a real WAV.")
    ap.add_argument("--wav", default=str(DEFAULT_WAV), help="16k mono PCM16 speech WAV.")
    ap.add_argument("--silero-model", default=None,
                    help="Silero ONNX path (default: bundled bridge/models/...).")
    ap.add_argument("--threshold", type=float, default=0.5,
                    help="Pass/fail bar for max-prob (default 0.5).")
    ns = ap.parse_args(argv)

    wav_path = Path(ns.wav).expanduser()
    if not wav_path.is_file():
        print(f"FAIL: WAV not found: {wav_path}", file=sys.stderr)
        return 2
    model_path = ns.silero_model or _default_silero_model()
    if not model_path:
        print("FAIL: Silero model not found (bridge/models/) -> pass --silero-model",
              file=sys.stderr)
        return 2

    pcm = load_wav_pcm16(wav_path)
    n_samples = len(pcm) // 2
    clip_ms = n_samples * 1000 // DEVICE_SAMPLE_RATE
    print(f"WAV: {wav_path}")
    print(f"raw int16 PCM: {len(pcm)} bytes = {n_samples} samples = {clip_ms} ms @16k mono")

    model = SileroModel(model_path, sample_rate=DEVICE_SAMPLE_RATE)
    model.load()

    # Tap every per-frame prob WITHOUT bypassing the live path: SpeechVad.feed()
    # still does the int16->float32 normalisation + 512-sample slicing exactly as
    # the bridge does live; we only wrap model.prob to record each result.
    probs: list[float] = []
    real_prob = model.prob

    def recording_prob(frame_f32, state):
        p, state_n = real_prob(frame_f32, state)
        probs.append(p)
        return p, state_n

    model.prob = recording_prob  # type: ignore[method-assign]

    vad = SpeechVad(
        threshold=0.8, neg_threshold=0.65, silence_ms=900,
        max_utterance_ms=12000, min_speech_ms=50,
        sample_rate=DEVICE_SAMPLE_RATE, model=model,
    )

    # Feed the whole clip through the live path (chunked like real frames arrive).
    reason = None
    chunk_bytes = SILERO_FRAME_SAMPLES * 2  # one 512-sample frame at a time
    for off in range(0, len(pcm), chunk_bytes):
        r = vad.feed(pcm[off:off + chunk_bytes])
        if r and reason is None:
            reason = r
    vad.flush()

    if not probs:
        print("FAIL: no frames evaluated (clip shorter than one 512-sample frame?)",
              file=sys.stderr)
        return 1

    max_p = max(probs)
    mean_p = sum(probs) / len(probs)
    argmax = max(range(len(probs)), key=lambda i: probs[i])
    at_ms = int(argmax * SILERO_FRAME_SAMPLES * 1000 / DEVICE_SAMPLE_RATE)

    print(f"frames evaluated : {len(probs)}")
    print(f"max  Silero prob : {max_p:.3f}  (at ~{at_ms} ms)")
    print(f"mean Silero prob : {mean_p:.3f}")
    print(f"speech_started   : {vad.speech_started}")
    print(f"vad reason       : {reason!r}")

    ok = max_p > ns.threshold
    print(f"\nRESULT: {'PASS' if ok else 'FAIL'} "
          f"(max-prob {max_p:.3f} {'>' if ok else '<='} {ns.threshold} threshold)")
    if not ok:
        print("  -> real speech reads near-zero: feed/normalisation is the bug.")
    else:
        print("  -> real speech reaches a high prob: normalisation path is correct.")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
