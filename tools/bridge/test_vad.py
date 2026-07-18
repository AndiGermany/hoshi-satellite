#!/usr/bin/env python3
"""Offline unit tests for the bridge-side Silero VAD (no device, no Hoshi).

Run: .venv/bin/python -m pytest test_vad.py -q   (pytest optional)
 or: .venv/bin/python test_vad.py                 (stdlib unittest fallback)

The Silero hysteresis state machine (napi_bridge.SpeechVad) is exercised with a
MOCKED probability function (prob_fn): synthetic sine tones do NOT read as speech
to the real Silero model (it is trained on real speech spectra), so a sine-vs-zero
test would be meaningless against the model. Instead we feed deterministic prob
sequences and assert the machine fires exactly once with the right reason. A
separate test loads the REAL ONNX model (if present + onnxruntime installed) and
only asserts it returns a float in [0, 1] — the actual mic prob distribution and
the right threshold MUST be tuned on the real device/room (see research doc).

Time is derived from the 512-sample @16k frame count, so tests are deterministic.
"""
from __future__ import annotations

import array
import math
import os
import unittest

from napi_bridge import (
    DEVICE_SAMPLE_RATE,
    SILERO_FRAME_SAMPLES,
    SileroModel,
    SpeechVad,
    _default_silero_model,
    pcm16_rms,
)

FRAME_MS = SILERO_FRAME_SAMPLES * 1000.0 / DEVICE_SAMPLE_RATE  # 32 ms @16k
FRAME_BYTES = SILERO_FRAME_SAMPLES * 2  # PCM16


def _pcm_frame(amplitude: int, n: int = SILERO_FRAME_SAMPLES) -> bytes:
    """One PCM16 mono frame: a 440 Hz sine at `amplitude` (0 => silence)."""
    a = array.array("h", [0] * n)
    if amplitude > 0:
        for i in range(n):
            a[i] = int(amplitude * math.sin(2 * math.pi * 440 * i / DEVICE_SAMPLE_RATE))
    return a.tobytes()


def _scripted_prob(values):
    """A prob_fn that returns the next value each call, holding the last forever."""
    box = {"i": 0}

    def fn(_frame):
        i = box["i"]
        v = values[i] if i < len(values) else values[-1]
        box["i"] = i + 1
        return v

    return fn


def _vad(prob_fn, **kw) -> SpeechVad:
    base = dict(
        threshold=0.8,
        neg_threshold=0.65,
        silence_ms=900,
        max_utterance_ms=12000,
        min_speech_ms=50,
        sample_rate=DEVICE_SAMPLE_RATE,
        prob_fn=prob_fn,
    )
    base.update(kw)
    return SpeechVad(**base)


def _feed_n(v: SpeechVad, n_frames: int):
    """Feed n exactly-512-sample silence frames; return the first reason or None."""
    silent = _pcm_frame(0)
    out = None
    for _ in range(n_frames):
        r = v.feed(silent)
        if r and out is None:
            out = r
    return out


class TestRms(unittest.TestCase):
    def test_loud_above_quiet(self):
        self.assertGreater(pcm16_rms(_pcm_frame(8000)), 500)
        self.assertEqual(pcm16_rms(_pcm_frame(0)), 0.0)

    def test_empty(self):
        self.assertEqual(pcm16_rms(b""), 0.0)


class TestSileroStateMachine(unittest.TestCase):
    """State-machine tests with a MOCKED prob_fn (model not involved)."""

    def test_speech_start_after_min_speech(self):
        # 50 ms min_speech => ceil(50/32) = 2 frames of high prob to latch start.
        v = _vad(_scripted_prob([0.95]))  # always speech
        f = _pcm_frame(8000)
        self.assertIsNone(v.feed(f))      # 32 ms voiced -> below 50 ms guard
        self.assertFalse(v.speech_started)
        self.assertIsNone(v.feed(f))      # 64 ms voiced -> >= 50 ms -> start
        self.assertTrue(v.speech_started)

    def test_silence_ends_after_speech(self):
        # speech for a while (prob 0.95) then silence (prob 0.05).
        # ~150 ms speech then enough silence frames for 900 ms.
        probs = [0.95] * 5 + [0.05] * 100
        v = _vad(_scripted_prob(probs))
        f = _pcm_frame(8000)
        for _ in range(5):
            v.feed(f)
        self.assertTrue(v.speech_started)
        fired = _feed_n(v, math.ceil(900 / FRAME_MS) + 2)
        self.assertEqual(fired, "silence")

    def test_fires_exactly_once(self):
        probs = [0.95] * 5 + [0.0] * 200
        v = _vad(_scripted_prob(probs))
        f = _pcm_frame(8000)
        for _ in range(5):
            v.feed(f)
        first = None
        extra = []
        sil = _pcm_frame(0)
        for _ in range(int(2000 / FRAME_MS)):
            r = v.feed(sil)
            if r and first is None:
                first = r
            elif r:
                extra.append(r)
        self.assertEqual(first, "silence")
        self.assertEqual(extra, [], "VAD must fire only once per turn")

    def test_brief_blip_does_not_start_speech(self):
        # a single 32 ms high-prob frame (< 50 ms guard) then silence: no start.
        v = _vad(_scripted_prob([0.95] + [0.0] * 100))
        v.feed(_pcm_frame(8000))
        self.assertFalse(v.speech_started)
        self.assertIsNone(_feed_n(v, 50))
        self.assertFalse(v.speech_started)

    def test_hysteresis_band_holds_silence_run(self):
        # prob in [neg, thr) must neither extend nor reset the silence run.
        # Start speech, then sit in the band: no fire even after a long time.
        probs = [0.95] * 5 + [0.7] * 200  # 0.7 is in [0.65, 0.8)
        v = _vad(_scripted_prob(probs))
        f = _pcm_frame(8000)
        for _ in range(5):
            v.feed(f)
        self.assertTrue(v.speech_started)
        fired = _feed_n(v, math.ceil(900 / FRAME_MS) + 5)
        self.assertIsNone(fired, "band-prob must not accumulate silence")

    def test_short_silence_gap_then_speech_resets(self):
        # 500 ms silence (< 900) then speech again resets the silence run.
        probs = [0.95] * 5            # initial speech
        probs += [0.05] * 16          # ~512 ms silence (< 900)
        probs += [0.95] * 8           # speech again
        probs += [0.05] * 60          # fresh silence -> should fire
        v = _vad(_scripted_prob(probs))
        f = _pcm_frame(8000)
        sil = _pcm_frame(0)
        for _ in range(5):
            v.feed(f)
        for _ in range(16):
            self.assertIsNone(v.feed(sil))
        for _ in range(8):
            self.assertIsNone(v.feed(f))
        fired = None
        for _ in range(int(900 / FRAME_MS) + 5):
            r = v.feed(sil)
            if r:
                fired = r
                break
        self.assertEqual(fired, "silence")

    def test_max_utterance_cap_fires(self):
        # always-speech, silence disabled => only the cap can fire.
        v = _vad(_scripted_prob([0.95]), silence_ms=10_000_000, max_utterance_ms=2000)
        f = _pcm_frame(8000)
        fired = None
        for _ in range(int(3000 / FRAME_MS)):
            r = v.feed(f)
            if r:
                fired = r
                break
        self.assertEqual(fired, "max_utterance")

    def test_check_timeout_watchdog(self):
        v = _vad(_scripted_prob([0.95]), max_utterance_ms=5000)
        f = _pcm_frame(8000)
        for _ in range(5):
            v.feed(f)
        self.assertTrue(v.speech_started)
        self.assertFalse(v.fired)
        self.assertIsNone(v.check_timeout(v._start_wall + 1.0))      # 1s < 5s
        self.assertEqual(v.check_timeout(v._start_wall + 6.0), "max_utterance")
        self.assertIsNone(v.check_timeout(v._start_wall + 7.0))      # idempotent

    def test_frame_slicing_and_carry(self):
        # Feed odd-sized PCM (not a 512-multiple): the VAD must slice into exact
        # 512-sample frames and carry the remainder. Count prob() invocations.
        calls = {"n": 0}

        def counting(_):
            calls["n"] += 1
            return 0.0

        v = _vad(counting)
        # 512 + 100 samples => 1 full frame now, 100 carried.
        v.feed(_pcm_frame(0, n=SILERO_FRAME_SAMPLES + 100))
        self.assertEqual(calls["n"], 1)
        # add 412 more samples => carry (100) + 412 = 512 => one more frame.
        v.feed(_pcm_frame(0, n=412))
        self.assertEqual(calls["n"], 2)

    def test_reset_states_clears_carry(self):
        v = _vad(_scripted_prob([0.0]))
        v.feed(_pcm_frame(0, n=100))   # 100 samples carried (< 512)
        self.assertTrue(len(v._tail) > 0)
        v.reset_states()
        self.assertEqual(len(v._tail), 0)


class TestSileroModelReal(unittest.TestCase):
    """Loads the REAL ONNX model if available; only checks the prob contract.

    The real speech threshold cannot be asserted offline with synthetic audio;
    it must be tuned on the device/room. Here we only prove the model loads
    offline and returns a float in [0, 1] for a valid 512-sample frame.
    """

    def test_real_model_prob_in_range(self):
        path = _default_silero_model()
        if not path:
            self.skipTest("Silero model not bundled (bridge/models/) -> skip")
        try:
            import numpy as np  # noqa: F401
            import onnxruntime  # noqa: F401
        except Exception as exc:  # pragma: no cover
            self.skipTest(f"onnxruntime/numpy missing: {exc}")
        import numpy as np

        m = SileroModel(path)
        m.load()
        st = m.new_state()
        # silence frame
        p0, st = m.prob(np.zeros(SILERO_FRAME_SAMPLES, dtype=np.float32), st)
        self.assertTrue(0.0 <= p0 <= 1.0)
        # a louder frame (still synthetic; we only check the range/contract)
        t = np.arange(SILERO_FRAME_SAMPLES) / DEVICE_SAMPLE_RATE
        sig = (0.5 * np.sin(2 * np.pi * 200 * t)).astype(np.float32)
        p1, st = m.prob(sig, st)
        self.assertTrue(0.0 <= p1 <= 1.0)

    def test_vad_with_real_model_does_not_crash(self):
        path = _default_silero_model()
        if not path:
            self.skipTest("Silero model not bundled -> skip")
        try:
            import onnxruntime  # noqa: F401
            import numpy  # noqa: F401
        except Exception as exc:  # pragma: no cover
            self.skipTest(f"onnxruntime/numpy missing: {exc}")
        m = SileroModel(path)
        m.load()
        v = SpeechVad(
            threshold=0.8, neg_threshold=0.65, silence_ms=900,
            max_utterance_ms=12000, min_speech_ms=50,
            sample_rate=DEVICE_SAMPLE_RATE, model=m,
        )
        # Feed real frames through the model: with synthetic audio it likely never
        # starts speech, but the path must run without raising and stay in range.
        for _ in range(20):
            v.feed(_pcm_frame(6000))
        self.assertTrue(0.0 <= v.last_prob <= 1.0)
        v.reset_states()  # must not raise


if __name__ == "__main__":
    unittest.main(verbosity=2)
