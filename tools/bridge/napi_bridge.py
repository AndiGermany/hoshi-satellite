#!/usr/bin/env python3
"""No-Flash Test-Bridge: HA Voice PE (ESPHome Native API) <-> Hoshi /ws/audio.

Hoshi speaks the ESPHome Native API of the Voice PE device AS A CLIENT (the role
Home Assistant normally plays). The device keeps its STOCK firmware and its stock
"Okay Nabu" wake word. On wake, the bridge:

  device mic (PCM16/16k)  --APIClient handle_audio-->  bridge  --wss binary-->  Hoshi /ws/audio
  Hoshi llm_audio (WAV PCM16/24k) --resample 24k->16k--> send_voice_assistant_audio --> device speaker

No Home Assistant. No flash. Half-duplex (one turn at a time).

STATUS: UNVERIFIED against a real device. The ESPHome Voice Assistant API surface
is verified against installed aioesphomeapi 45.x; the END-TO-END run order
(esphome/home-assistant-voice-pe#477) is best-effort and marked TODO/UNVERIFIED
where the device-side behaviour cannot be confirmed without hardware.

Protocol references (read 2026-06-15):
  - aioesphomeapi api.proto: VoiceAssistant{Request,Response,EventResponse,Audio,
    AudioSettings,AnnounceRequest}, VoiceAssistantSubscribeFlag.API_AUDIO.
  - ESPHome firmware voice_assistant.cpp request_start()/on_event()/on_audio()
    (device SAMPLE_RATE_HZ = 16000).
  - esphome/home-assistant-voice-pe#477 (this exact use case; handshake still open).
  - Hoshi contract: see ../../docs/PROTOCOL.md in this repo.

HANDSHAKE WE IMPLEMENT (client = us, "server"/HA role):
  1. APIClient.connect(login=True) then subscribe_voice_assistant(handle_start,
     handle_stop, handle_audio=...). Passing handle_audio sets the API_AUDIO
     subscribe flag => device streams mic over the API connection (not UDP).
  2. Device wakes on "Okay Nabu" -> sends VoiceAssistantRequest(start=True,
     conversation_id, flags, audio_settings, wake_word_phrase). aioesphomeapi
     invokes our handle_start(conversation_id, flags, audio_settings, wake_word).
  3. handle_start returns 0 (NOT None) -> library replies VoiceAssistantResponse(
     port=0) => "use API audio". Returning None/raising -> response(error=True).
  4. Device streams VoiceAssistantAudio frames -> our handle_audio(data, data2).
     We forward `data` (mono PCM16/16k) to Hoshi as binary ws frames.
  5. We DRIVE the device's pipeline UI/state by sending events back:
     RUN_START -> STT_START -> STT_END(text) -> INTENT_START -> INTENT_END ->
     TTS_START(text) -> TTS_STREAM_START -> [send_voice_assistant_audio chunks]
     -> TTS_STREAM_END -> TTS_END -> RUN_END. (UNVERIFIED ordering for #477.)
  6. End of utterance: device VAD ends; aioesphomeapi calls handle_stop(...).
     We send {type:"stop"} to Hoshi, await transcript + llm_audio, play it back.
"""

from __future__ import annotations

import argparse
import asyncio
import base64
import io
import json
import logging
import math
import os
import ssl
import struct
import sys
import time
import uuid
import wave
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

import websockets

try:  # audioop removed from stdlib in Python 3.13+; use it if present, else pure-python.
    import audioop  # type: ignore
except ImportError:  # pragma: no cover
    audioop = None  # type: ignore

from aioesphomeapi import (
    APIClient,
    VoiceAssistantAudioSettings,
    VoiceAssistantEventType,
)

LOG = logging.getLogger("napi_bridge")

DEVICE_SAMPLE_RATE = 16000  # ESPHome voice_assistant.cpp SAMPLE_RATE_HZ (mic + speaker)
HOSHI_TTS_SAMPLE_RATE = 24000  # contract: WS audio normalised to WAV PCM16 mono 24k
SPEAKER_CHUNK = 1024  # bytes per send_voice_assistant_audio frame (PCM16 => 512 samples)
UPLINK_CHUNK = 16384  # bytes per /ws/audio binary frame; Hoshi cuts off oversized single frames (EOF, no transcript)

# -- Bridge-side End-of-Speech (VAD) defaults -------------------------------
# In ESPHome API_AUDIO mode the device does NOT signal end-of-speech (no
# VoiceAssistantRequest(start=false) => handle_stop is never called), so the
# bridge must detect silence itself and emit a single {type:"stop"} to Hoshi.
#
# VAD = Silero (ONNX): a neural speech-probability model is far more
# noise/TV-robust than raw RMS energy. Silero runs on EXACTLY 512-sample frames
# @16k (32 ms); a hysteresis state machine on its probability decides speech/
# silence, and `silence_ms` of contiguous quiet ends the utterance.
DEFAULT_SILENCE_MS = 900        # trailing silence that ends the utterance
DEFAULT_VAD_THRESHOLD = 0.8     # Silero prob >= this == "speech" (TV-hardened)
DEFAULT_VAD_NEG_OFFSET = 0.15   # neg_threshold = threshold - this (hysteresis)
DEFAULT_MAX_UTTERANCE_MS = 12000  # hard cap from speech start -> force stop
DEFAULT_MIN_SPEECH_MS = 50      # min voiced run before "speech start" (start guard)
# ABSOLUTE wall-clock cap from WAKE/stream-start (NOT speech-start). Safety net:
# even if the VAD never detects speech (e.g. mis-fed audio), this forces a stop
# so a multi-second clip can NEVER be flushed to Whisper (which times out at
# ~15 s -> empty transcript). Distinct from max_utterance_ms, which only counts
# AFTER speech starts and so does nothing when speech is never detected.
DEFAULT_MAX_TURN_MS = 10000     # absolute cap from wake -> force stop

# Silero VAD ONNX. The 16k-only op15 model is small (~1.3 MB) and matches the
# bridge's fixed 16 kHz mic. Loaded ONCE at bridge start (not per turn). The
# model file is shipped under bridge/models/ (extracted from the silero-vad
# wheel; we depend only on onnxruntime, NOT torch).
SILERO_FRAME_SAMPLES = 512      # Silero requires exactly 512 samples @16k (32 ms)
# Silero v5/v6 needs the LAST 64 samples of the PREVIOUS frame prepended to the
# current 512-sample frame (model input = 64 + 512 = 576 samples). Feeding a bare
# 512-sample frame makes the model read real speech as prob~0.00 (LIVE BUG: VAD
# never fired -> ~100 s clip -> Whisper timeout). The context is per-stream state,
# carried with the LSTM state across frames and zeroed per turn.
SILERO_CONTEXT_SAMPLES = 64
SILERO_MODEL_FILENAME = "silero_vad_16k_op15.onnx"


def _default_silero_model() -> Optional[str]:
    """Path to the bundled Silero ONNX model (bridge/models/...), if present."""
    here = Path(__file__).resolve()
    p = here.parent / "models" / SILERO_MODEL_FILENAME
    return str(p) if p.is_file() else None


# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------
@dataclass
class Config:
    device_host: str
    api_port: int = 6053
    encryption_key: Optional[str] = None  # ESPHome API "noise_psk" (base64); None = keyless
    api_password: str = ""  # legacy password auth (stock devices usually empty)
    hoshi_ws_url: str = "wss://<hoshi-server-ip>:8081/ws/audio"
    cert_path: Optional[str] = None  # PEM trust anchor for wss leaf-pinning
    token: Optional[str] = None  # Hoshi ingress token; None unless auth gate is ON
    room: Optional[str] = None
    satellite_id: Optional[str] = None
    # Hoshi binary uplink container. Only "wav" is implemented (raw PCM is not
    # accepted by Hoshi STT). "webm" is a TODO placeholder (needs an encoder).
    uplink_format: str = "wav"
    # Bridge-side VAD (end-of-speech) tunables. VAD = Silero (ONNX).
    silence_ms: int = DEFAULT_SILENCE_MS
    vad_threshold: float = DEFAULT_VAD_THRESHOLD
    # neg_threshold defaults to threshold - DEFAULT_VAD_NEG_OFFSET; None => derive.
    vad_neg_threshold: Optional[float] = None
    max_utterance_ms: int = DEFAULT_MAX_UTTERANCE_MS
    min_speech_ms: int = DEFAULT_MIN_SPEECH_MS
    # Absolute wall-clock cap from wake/stream-start (safety net; not speech-gated).
    max_turn_ms: int = DEFAULT_MAX_TURN_MS
    silero_model_path: Optional[str] = None  # None => bundled bridge/models/...
    # Digital mic-gain compensation for a too-quiet device (low Silero prob ->
    # empty STT -> no_input). 1.0 = off. Real fix = device AGC at Route-B flash.
    mic_gain: float = 1.0

    @classmethod
    def from_args(cls, ns: argparse.Namespace) -> "Config":
        token = None
        if ns.token_file:
            p = Path(ns.token_file).expanduser()
            if p.is_file():
                token = p.read_text(encoding="utf-8").strip()
                if not token:
                    token = None
                    LOG.warning("token file %s is empty -> running tokenless", p)
            else:
                LOG.warning("token file %s not found -> running tokenless", p)
        elif os.environ.get("HOSHI_API_TOKEN"):
            token = os.environ["HOSHI_API_TOKEN"].strip() or None

        return cls(
            device_host=ns.device_host,
            api_port=ns.api_port,
            encryption_key=ns.api_encryption_key
            or os.environ.get("ESPHOME_API_KEY")
            or None,
            api_password=ns.api_password or os.environ.get("ESPHOME_API_PASSWORD", ""),
            hoshi_ws_url=ns.hoshi_ws_url,
            cert_path=ns.cert or os.environ.get("HOSHI_CERT") or None,
            token=token,
            room=ns.room,
            satellite_id=ns.satellite_id,
            uplink_format=ns.uplink_format,
            silence_ms=ns.silence_ms,
            vad_threshold=ns.vad_threshold,
            vad_neg_threshold=ns.vad_neg_threshold,
            max_utterance_ms=ns.max_utterance_ms,
            min_speech_ms=ns.min_speech_ms,
            max_turn_ms=ns.max_turn_ms,
            silero_model_path=ns.silero_model or None,
            mic_gain=ns.mic_gain,
        )

    @property
    def effective_neg_threshold(self) -> float:
        """Hysteresis low watermark: explicit --vad-neg-threshold or threshold-offset."""
        if self.vad_neg_threshold is not None:
            return self.vad_neg_threshold
        return max(0.0, self.vad_threshold - DEFAULT_VAD_NEG_OFFSET)


# ---------------------------------------------------------------------------
# Audio helpers
# ---------------------------------------------------------------------------
def pcm16_to_wav(pcm: bytes, sample_rate: int = DEVICE_SAMPLE_RATE, channels: int = 1) -> bytes:
    """Wrap raw PCM16 LE samples in a canonical 44-byte RIFF/WAVE header.

    Pure-Python (struct only), so the bridge needs no numpy/ffmpeg. Hoshi's
    `/ws/audio` binary uplink does NOT accept raw PCM: WhisperSttClient maps the
    mimeType to a file extension (only webm/ogg/wav/mp4, else -> webm fallback)
    and hoshi-stt-mlx `_convert_to_wav` passes `wav` bytes through unchanged. So
    we send ONE complete WAV per turn with mimeType "audio/wav".

    Header layout (16-bit PCM, fmt chunk = 16 bytes => 44-byte header total):
      RIFF <riff_size> WAVE
      fmt  16  audio_format=1 channels rate byte_rate block_align bits=16
      data <data_size> <pcm...>
    where riff_size = 36 + data_size, byte_rate = rate*channels*2,
    block_align = channels*2.
    """
    bits_per_sample = 16
    bytes_per_sample = bits_per_sample // 8
    data_size = len(pcm)
    byte_rate = sample_rate * channels * bytes_per_sample
    block_align = channels * bytes_per_sample
    riff_size = 36 + data_size
    header = struct.pack(
        "<4sI4s4sIHHIIHH4sI",
        b"RIFF",
        riff_size,
        b"WAVE",
        b"fmt ",
        16,                 # fmt chunk size (PCM)
        1,                  # audio_format = 1 (PCM)
        channels,
        sample_rate,
        byte_rate,
        block_align,
        bits_per_sample,
        b"data",
        data_size,
    )
    return header + pcm


def wav_to_pcm16_mono(data: bytes) -> tuple[bytes, int, int]:
    """Decode a WAV container -> (raw PCM16 bytes, sample_rate, channels)."""
    with wave.open(io.BytesIO(data), "rb") as wf:
        sr = wf.getframerate()
        ch = wf.getnchannels()
        sw = wf.getsampwidth()
        pcm = wf.readframes(wf.getnframes())
    if sw != 2:
        # contract guarantees 16-bit; if not, bail loudly rather than play noise.
        raise ValueError(f"expected 16-bit WAV, got sampwidth={sw}")
    if ch == 2:
        pcm = _stereo_to_mono(pcm)
        ch = 1
    return pcm, sr, ch


def _stereo_to_mono(pcm: bytes) -> bytes:
    if audioop is not None:
        return audioop.tomono(pcm, 2, 0.5, 0.5)
    # pure-python fallback (audioop absent on Python 3.13+)
    import array

    a = array.array("h")
    a.frombytes(pcm)
    out = array.array("h", [0] * (len(a) // 2))
    for i in range(len(out)):
        out[i] = (a[2 * i] + a[2 * i + 1]) // 2
    return out.tobytes()


def resample_pcm16(pcm: bytes, src_rate: int, dst_rate: int, state=None):
    """Resample mono PCM16. Returns (pcm, new_state). Uses audioop.ratecv if present.

    24k->16k is a 2:3 ratio. Pure-python fallback is a crude linear resampler and
    is marked UNVERIFIED for audio quality -- prefer a venv with audioop or numpy.
    """
    if src_rate == dst_rate:
        return pcm, state
    if audioop is not None:
        out, new_state = audioop.ratecv(pcm, 2, 1, src_rate, dst_rate, state)
        return out, new_state
    return _linear_resample(pcm, src_rate, dst_rate), None


def _linear_resample(pcm: bytes, src_rate: int, dst_rate: int) -> bytes:
    import array

    a = array.array("h")
    a.frombytes(pcm)
    n_in = len(a)
    if n_in == 0:
        return b""
    n_out = max(1, int(n_in * dst_rate / src_rate))
    out = array.array("h", [0] * n_out)
    for i in range(n_out):
        pos = i * src_rate / dst_rate
        lo = int(pos)
        hi = min(lo + 1, n_in - 1)
        frac = pos - lo
        out[i] = int(a[lo] * (1 - frac) + a[hi] * frac)
    return out.tobytes()


# ---------------------------------------------------------------------------
# VAD (end-of-speech detection) — Silero (ONNX). pcm16_rms kept as a small,
# torch-free energy helper (no longer drives endpointing; handy for diagnostics).
# ---------------------------------------------------------------------------
def pcm16_rms(pcm: bytes) -> float:
    """Root-mean-square amplitude of mono PCM16 little-endian samples (0..32767).

    Uses audioop.rms when present (fast C path); otherwise a pure-python
    fallback (audioop is gone from stdlib on Python 3.13+). An odd trailing
    byte (truncated sample) is ignored.
    """
    n = len(pcm) - (len(pcm) & 1)
    if n <= 0:
        return 0.0
    if audioop is not None:
        return float(audioop.rms(pcm[:n], 2))
    import array

    a = array.array("h")
    a.frombytes(pcm[:n])
    if not a:
        return 0.0
    acc = 0
    for s in a:
        acc += s * s
    return math.sqrt(acc / len(a))


def pcm16_apply_gain(pcm: bytes, gain: float) -> bytes:
    """Scale mono PCM16 little-endian samples by `gain`, hard-clipping to int16.

    Digital compensation for a too-quiet device mic (low Silero prob -> empty STT
    -> no_input -> red light). gain==1.0 is a no-op. Needs NO reflash; the real
    fix is the Voice-PE's analog gain/AGC, changeable only at Route-B flash time.
    Digital gain also lifts the noise floor and can clip, so keep it modest
    (~2-4x) and watch the per-WAV rms log in _send_wav.
    """
    if gain == 1.0:
        return pcm
    n = len(pcm) - (len(pcm) & 1)
    if n <= 0:
        return pcm
    if audioop is not None:
        return audioop.mul(pcm[:n], 2, gain)  # clips to int16 automatically
    import array

    a = array.array("h")
    a.frombytes(pcm[:n])
    for i in range(len(a)):
        v = int(a[i] * gain)
        a[i] = 32767 if v > 32767 else (-32768 if v < -32768 else v)
    return a.tobytes()


class SileroModel:
    """Thin onnxruntime wrapper around the Silero VAD ONNX model.

    Loaded ONCE at bridge start and SHARED across turns (the model is stateless;
    per-stream LSTM state lives in SpeechVad). Each call evaluates one 512-sample
    @16k frame and returns the speech probability in [0, 1].

    Silero v5/v6 ONNX interface (verified against silero_vad_16k_op15.onnx):
      inputs : input (float [batch, samples]), state (float [2, batch, 128]),
               sr (int64 scalar)
      outputs: output (prob [batch, 1]), stateN (next state)

    CONTEXT WINDOW (verified live): the model does NOT take a bare 512-sample
    frame. It expects the LAST 64 samples of the PREVIOUS frame prepended, so the
    `input` tensor is 64 + 512 = 576 samples. Feeding only 512 makes real speech
    read as prob~0.003 (the LIVE BUG); with the 64-sample context the same voiced
    frame reads prob~0.90. The context is per-stream and is carried alongside the
    LSTM state (see `new_state`/`prob`), zeroed at the start of each turn.

    onnxruntime is the ONLY heavy dep (no torch). If onnxruntime or the model
    file is missing, `load()` raises -> the bridge logs and refuses to arm,
    rather than silently degrading to a worse VAD.
    """

    def __init__(self, model_path: str, sample_rate: int = DEVICE_SAMPLE_RATE):
        self.model_path = model_path
        self.sample_rate = sample_rate
        self._sess = None
        self._np = None
        self._sr_tensor = None

    def load(self) -> None:
        import numpy as np  # local import: numpy is only needed with Silero
        import onnxruntime as ort

        opts = ort.SessionOptions()
        # Single-threaded: per-frame inference is sub-millisecond; threads only
        # add overhead and contend with the asyncio loop.
        opts.inter_op_num_threads = 1
        opts.intra_op_num_threads = 1
        self._sess = ort.InferenceSession(
            self.model_path, sess_options=opts, providers=["CPUExecutionProvider"]
        )
        self._np = np
        self._sr_tensor = np.array(self.sample_rate, dtype=np.int64)
        LOG.info("Silero VAD loaded: %s (sr=%d, frame=%d samples)",
                 self.model_path, self.sample_rate, SILERO_FRAME_SAMPLES)

    def new_state(self):
        """Fresh per-stream state for a turn: (lstm_state, context).

        `context` is the 64-sample carry the Silero v5/v6 model prepends to each
        512-sample frame; it starts as zeros (silence) and is updated per call.
        Returned as a tuple so the context lifecycle exactly tracks the LSTM
        state (both reset together at turn start).
        """
        return (
            self._np.zeros((2, 1, 128), dtype=self._np.float32),
            self._np.zeros(SILERO_CONTEXT_SAMPLES, dtype=self._np.float32),
        )

    def prob(self, frame_f32, state):
        """Run one frame -> (probability float, next_state). `frame_f32` is a
        float32 numpy array of exactly SILERO_FRAME_SAMPLES samples in [-1, 1].

        `state` is the (lstm_state, context) tuple from new_state(). The model
        input is `context (64) + frame (512) = 576` samples; the new context is
        the last 64 samples of THIS frame, carried to the next call. (Feeding a
        bare 512 frame is the LIVE BUG that made speech read as prob~0.)"""
        lstm_state, context = state
        model_input = self._np.concatenate((context, frame_f32)).reshape(1, -1)
        out, lstm_state_n = self._sess.run(
            None,
            {
                "input": model_input,
                "state": lstm_state,
                "sr": self._sr_tensor,
            },
        )
        next_context = frame_f32[-SILERO_CONTEXT_SAMPLES:]
        return float(out[0, 0]), (lstm_state_n, next_context)


class SpeechVad:
    """Silero-VAD silence endpointing for one utterance, fed mono PCM16/16k.

    The bridge slices the incoming PCM stream into EXACTLY 512-sample @16k frames
    (32 ms; the only size Silero accepts) and evaluates each with the shared
    Silero model. A leftover partial frame at end-of-stream is zero-padded. The
    LSTM state is reset per turn (a fresh SpeechVad per turn does this).

    Hysteresis state machine on the Silero probability:
      1. Speech START: prob >= `threshold` accumulates voiced time; once
         `min_speech_ms` of cumulative voiced audio is seen -> speech started
         (a debounce guard against a single noisy frame).
      2. After start: prob < `neg_threshold` accumulates trailing silence; prob
         >= `threshold` resets it. The gap between threshold and neg_threshold
         is the hysteresis band that stops chattering near the boundary.
      3. `silence_ms` of contiguous silence -> end-of-speech ("silence").
      4. Hard cap: `max_utterance_ms` of audio from speech start -> force
         end-of-speech ("max_utterance"). Keeps a never-ending TV from latching.

    `feed(pcm)` returns the reason string ("silence" | "max_utterance") exactly
    ONCE, the first time end-of-speech is reached; afterwards it returns None.
    Speech/silence time is derived from the frame count, so the state machine is
    deterministic and unit-testable offline (mock the prob via `prob_fn`).
    `check_timeout(now)` lets a watchdog force the cap when no frames arrive.

    For tests / model-less fallback, pass `prob_fn(frame_f32) -> float` to bypass
    the ONNX model; otherwise `model` (a loaded SileroModel) is used.
    """

    def __init__(
        self,
        threshold: float,
        neg_threshold: float,
        silence_ms: int,
        max_utterance_ms: int,
        min_speech_ms: int,
        sample_rate: int = DEVICE_SAMPLE_RATE,
        model: Optional["SileroModel"] = None,
        prob_fn=None,
    ):
        self.threshold = threshold
        self.neg_threshold = neg_threshold
        self.silence_ms = silence_ms
        self.max_utterance_ms = max_utterance_ms
        self.min_speech_ms = min_speech_ms
        self.sample_rate = sample_rate
        self.model = model
        self._prob_fn = prob_fn
        self._state = model.new_state() if (model is not None and prob_fn is None) else None
        self.speech_started = False
        self.fired = False
        self.last_prob = 0.0        # most recent Silero prob (for DEBUG logging)
        self._frame_ms = SILERO_FRAME_SAMPLES * 1000.0 / sample_rate  # 32 ms @16k
        self._voiced_ms = 0.0       # cumulative voiced audio (start guard)
        self._silence_ms_run = 0.0  # contiguous trailing silence
        self._utterance_ms = 0.0    # audio elapsed since speech start
        self._start_wall: Optional[float] = None  # wall-clock at speech start
        self._tail = bytearray()    # carry leftover PCM (< 1 frame) between feeds

    def reset_states(self) -> None:
        """Reset Silero LSTM state + carry buffer (call per turn/stream)."""
        if self.model is not None and self._prob_fn is None:
            self._state = self.model.new_state()
        self._tail = bytearray()

    def _frame_prob(self, frame_f32) -> float:
        if self._prob_fn is not None:
            return float(self._prob_fn(frame_f32))
        p, self._state = self.model.prob(frame_f32, self._state)
        return p

    def _feed_frame(self, frame_f32) -> Optional[str]:
        """Process ONE 512-sample frame through the hysteresis machine."""
        prob = self._frame_prob(frame_f32)
        self.last_prob = prob
        dur_ms = self._frame_ms

        if not self.speech_started:
            if prob >= self.threshold:
                self._voiced_ms += dur_ms
                if self._voiced_ms >= self.min_speech_ms:
                    self.speech_started = True
                    self._start_wall = time.monotonic()
                    self._utterance_ms = self._voiced_ms
                    self._silence_ms_run = 0.0
            else:
                # reset the start guard on a gap so a stray frame can't latch
                self._voiced_ms = 0.0
            return None

        # speech in progress
        self._utterance_ms += dur_ms
        if prob >= self.threshold:
            self._silence_ms_run = 0.0
        elif prob < self.neg_threshold:
            self._silence_ms_run += dur_ms
            if self._silence_ms_run >= self.silence_ms:
                self.fired = True
                return "silence"
        # prob in the hysteresis band [neg_threshold, threshold): hold state,
        # neither extend nor reset the silence run.
        if self._utterance_ms >= self.max_utterance_ms:
            self.fired = True
            return "max_utterance"
        return None

    def feed(self, pcm: bytes) -> Optional[str]:
        """Slice incoming PCM16/16k into 512-sample frames and run each.

        Leftover (< 512 samples) is carried to the next feed; on a short final
        frame at stream end the carry stays buffered (the silence_ms / cap path
        ends the turn regardless). Returns the end-of-speech reason exactly once.
        """
        if self.fired:
            return None
        import numpy as np
        self._tail += pcm
        frame_bytes = SILERO_FRAME_SAMPLES * 2  # 16-bit
        reason: Optional[str] = None
        while len(self._tail) >= frame_bytes:
            chunk = bytes(self._tail[:frame_bytes])
            del self._tail[:frame_bytes]
            samples = np.frombuffer(chunk, dtype="<i2").astype(np.float32) / 32768.0
            r = self._feed_frame(samples)
            if r is not None:
                reason = r
                break
        return reason

    def flush(self) -> Optional[str]:
        """Zero-pad and run a final partial frame (end-of-stream). Optional; the
        silence/cap paths normally end the turn before this matters."""
        if self.fired or not self._tail:
            return None
        import numpy as np
        n = len(self._tail) // 2
        if n == 0:
            return None
        samples = np.frombuffer(bytes(self._tail[: n * 2]), dtype="<i2").astype(np.float32) / 32768.0
        self._tail = bytearray()
        if n < SILERO_FRAME_SAMPLES:
            samples = np.pad(samples, (0, SILERO_FRAME_SAMPLES - n))
        return self._feed_frame(samples)

    def check_timeout(self, now: float) -> Optional[str]:
        """Wall-clock fallback for the hard cap (watchdog, no frames arriving)."""
        if self.fired or not self.speech_started or self._start_wall is None:
            return None
        if (now - self._start_wall) * 1000.0 >= self.max_utterance_ms:
            self.fired = True
            return "max_utterance"
        return None


# ---------------------------------------------------------------------------
# Turn state
# ---------------------------------------------------------------------------
@dataclass
class Turn:
    turn_id: str
    ws: object  # websockets client connection
    conversation_id: str = ""
    transcript: str = ""
    # ordered list of (seq, pcm16_24k) chunks received from Hoshi
    tts_chunks: list = field(default_factory=list)
    done: asyncio.Event = field(default_factory=asyncio.Event)
    aborted: bool = False
    error: Optional[str] = None
    # bridge-side end-of-speech detection
    vad: Optional["SpeechVad"] = None
    stop_sent: bool = False             # idempotent: exactly one {type:stop} per turn
    # monotonic wall-clock at wake/stream-start (NOT speech-start); drives the
    # absolute max-turn-ms safety cap even when the VAD never detects speech.
    started_wall: float = 0.0
    _last_prob_log: float = 0.0         # monotonic ts of last DEBUG Silero-prob log
    # WAV uplink: accumulate mic PCM16/16k for this turn, flush as ONE WAV blob
    # at stop. bytearray (mutable, cheap append). Reset per turn.
    mic_pcm: bytearray = field(default_factory=bytearray)
    audio_sent: bool = False            # idempotent: exactly one WAV blob per turn


# ---------------------------------------------------------------------------
# Bridge
# ---------------------------------------------------------------------------
class Bridge:
    def __init__(self, cfg: Config):
        self.cfg = cfg
        self.client: Optional[APIClient] = None
        self.loop = asyncio.get_event_loop()
        self._turn: Optional[Turn] = None
        self._resample_state = None
        self.silero: Optional[SileroModel] = None  # loaded ONCE in run()

    # ---- ESPHome API lifecycle ----------------------------------------
    async def run(self) -> None:
        self.client = APIClient(
            address=self.cfg.device_host,
            port=self.cfg.api_port,
            password=self.cfg.api_password,
            noise_psk=self.cfg.encryption_key,  # None => keyless (try first on stock)
            client_info="hoshi-napi-bridge",
        )
        LOG.info(
            "connecting to ESPHome device %s:%s (keyless=%s)",
            self.cfg.device_host,
            self.cfg.api_port,
            self.cfg.encryption_key is None,
        )
        # Load the Silero VAD model ONCE before going live (shared across turns).
        self._load_silero()

        await self.client.connect(login=True)
        info = await self.client.device_info()
        LOG.info("connected: %s (esphome %s)", info.name, info.esphome_version)

        # Passing handle_audio sets VOICE_ASSISTANT_SUBSCRIBE_API_AUDIO ->
        # device streams mic over the API connection rather than UDP.
        self.client.subscribe_voice_assistant(
            handle_start=self._handle_start,
            handle_stop=self._handle_stop,
            handle_audio=self._handle_audio,
            handle_announcement_finished=self._handle_announce_finished,
        )
        LOG.info("subscribed to voice assistant (API_AUDIO mode). Waiting for wake...")

        # Stay alive until disconnected.
        stop = asyncio.Event()
        try:
            await stop.wait()
        finally:
            await self.client.disconnect()

    def _load_silero(self) -> None:
        """Load the Silero VAD ONNX model once at startup (shared across turns).

        Fatal if missing: a silently-degraded VAD is worse than a clear error,
        and the bridge's whole endpointing now depends on Silero.
        """
        path = self.cfg.silero_model_path or _default_silero_model()
        if not path:
            raise FileNotFoundError(
                f"Silero VAD model not found (bridge/models/{SILERO_MODEL_FILENAME}); "
                "pass --silero-model PATH or extract the model from the silero-vad wheel."
            )
        model = SileroModel(path, sample_rate=DEVICE_SAMPLE_RATE)
        model.load()  # raises if onnxruntime/model is unusable
        self.silero = model

    # ---- Wake / start -------------------------------------------------
    async def _handle_start(
        self,
        conversation_id: str,
        flags: int,
        audio_settings: VoiceAssistantAudioSettings,
        wake_word_phrase: Optional[str],
    ) -> Optional[int]:
        """Device woke ("Okay Nabu"). Open a Hoshi /ws/audio session for this turn.

        Returning 0 => use API audio (device streams VoiceAssistantAudio to us).
        Returning None / raising => library sends VoiceAssistantResponse(error=True).
        """
        if self._turn is not None:
            LOG.warning("wake while a turn is active -> ignoring (half-duplex)")
            return None  # refuse -> error response, device aborts the new run

        turn_id = str(uuid.uuid4())
        LOG.info(
            "WAKE conv=%s wake_word=%r flags=%s -> new turn %s",
            conversation_id,
            wake_word_phrase,
            flags,
            turn_id,
        )
        try:
            ws = await self._open_hoshi_ws()
        except Exception as exc:  # noqa: BLE001
            LOG.exception("failed to open Hoshi ws: %s", exc)
            return None  # -> error response

        neg = self.cfg.effective_neg_threshold
        turn = Turn(
            turn_id=turn_id,
            ws=ws,
            conversation_id=conversation_id,
            vad=SpeechVad(
                threshold=self.cfg.vad_threshold,
                neg_threshold=neg,
                silence_ms=self.cfg.silence_ms,
                max_utterance_ms=self.cfg.max_utterance_ms,
                min_speech_ms=self.cfg.min_speech_ms,
                sample_rate=DEVICE_SAMPLE_RATE,
                model=self.silero,  # shared, pre-loaded; per-turn LSTM state inside
            ),
            started_wall=time.monotonic(),  # absolute cap counts from wake
        )
        self._turn = turn
        self._resample_state = None
        LOG.info(
            "VAD armed (Silero): thr=%.2f neg=%.2f, silence=%dms, min_speech=%dms, "
            "max_utt=%dms, max_turn=%dms (absolute, from wake)",
            self.cfg.vad_threshold,
            neg,
            self.cfg.silence_ms,
            self.cfg.min_speech_ms,
            self.cfg.max_utterance_ms,
            self.cfg.max_turn_ms,
        )

        # Hoshi uplink: announce the turn. Hoshi STT does NOT accept raw PCM on
        # the binary uplink (WhisperSttClient maps mimeType -> file extension,
        # only webm/ogg/wav/mp4; the backend buffers all binary frames per
        # session and transcribes ONCE on {type:stop}). So we accumulate mic
        # PCM this turn and flush a single complete WAV blob at stop.
        start_msg = {
            "type": "start",
            "mimeType": "audio/wav",
            "turnId": turn_id,
        }
        if self.cfg.room:
            start_msg["room"] = self.cfg.room
        if self.cfg.satellite_id:
            start_msg["satelliteId"] = self.cfg.satellite_id
        await ws.send(json.dumps(start_msg))
        LOG.info("ws -> start(mimeType=audio/wav) turn=%s", turn_id)

        # Drive the device UI state machine: pipeline + STT started.
        self._event(VoiceAssistantEventType.VOICE_ASSISTANT_RUN_START)
        self._event(VoiceAssistantEventType.VOICE_ASSISTANT_STT_START)

        # Background task: pump Hoshi downlink -> device.
        asyncio.create_task(self._consume_hoshi(turn))
        # Watchdog: force the hard cap even if the device stops sending frames
        # (API_AUDIO gives no stop, and silence-as-no-frames wouldn't be fed).
        asyncio.create_task(self._vad_watchdog(turn))

        return 0  # API audio mode

    async def _open_hoshi_ws(self):
        ssl_ctx = None
        if self.cfg.hoshi_ws_url.startswith("wss://"):
            ssl_ctx = ssl.create_default_context()
            if self.cfg.cert_path:
                ssl_ctx.load_verify_locations(self.cfg.cert_path)
            # The reference deployment's leaf is self-signed with a SAN covering
            # its server's LAN IP. Pinning the cert as the trust anchor validates
            # the chain; hostname check still applies and the SAN covers the IP,
            # so keep verification ON.
        url = self.cfg.hoshi_ws_url
        if self.cfg.token:
            sep = "&" if "?" in url else "?"
            url = f"{url}{sep}token={self.cfg.token}"
        extra_headers = {}
        if self.cfg.token:
            extra_headers["Authorization"] = f"Bearer {self.cfg.token}"
        LOG.info("opening Hoshi ws %s (token=%s)", self.cfg.hoshi_ws_url, bool(self.cfg.token))
        # websockets>=14 uses additional_headers; older used extra_headers.
        try:
            return await websockets.connect(
                url, ssl=ssl_ctx, additional_headers=extra_headers or None, max_size=None
            )
        except TypeError:
            return await websockets.connect(
                url, ssl=ssl_ctx, extra_headers=extra_headers or None, max_size=None
            )

    # ---- Mic audio: device -> Hoshi -----------------------------------
    async def _handle_audio(self, data: bytes, data2: Optional[bytes]) -> None:
        """VoiceAssistantAudio frame from the device (PCM16 mono 16k in `data`).

        `data2` is the optional second mic channel (reference/AEC). We keep only
        the primary channel; Hoshi STT is mono. (data2 handling = TODO if AEC wanted.)

        We do NOT stream raw PCM to Hoshi (it is not accepted on the binary
        uplink). Instead we ACCUMULATE the mic PCM16/16k for this turn and flush
        a single complete WAV blob at stop (see _send_stop).

        In API_AUDIO mode the device never signals end-of-speech, so we run a
        bridge-side Silero VAD over these frames and emit a single {type:"stop"}
        once speech is followed by `silence_ms` of quiet (Silero prob below the
        hysteresis low watermark) or the `max_utterance_ms` hard cap is hit.
        """
        turn = self._turn
        if turn is None or turn.aborted:
            return
        # Accumulate first; VAD must not block buffering. After stop_sent we drop
        # trailing frames (the WAV blob is already flushed) and skip VAD work.
        if turn.stop_sent:
            return
        # Digital mic-gain compensation (clip-safe) BEFORE buffering + VAD, so a
        # too-quiet mic lifts BOTH the Silero prob and the STT WAV. 1.0 = no-op.
        if self.cfg.mic_gain != 1.0:
            data = pcm16_apply_gain(data, self.cfg.mic_gain)
        turn.mic_pcm += data

        if turn.vad is None:
            return
        was_speaking = turn.vad.speech_started
        reason = turn.vad.feed(data)
        if not was_speaking and turn.vad.speech_started:
            LOG.info("VAD: speech start (turn=%s, prob=%.2f)", turn.turn_id, turn.vad.last_prob)
        # Occasional Silero prob on DEBUG (~1/s), never per frame.
        now = time.monotonic()
        if LOG.isEnabledFor(logging.DEBUG) and now - turn._last_prob_log >= 1.0:
            turn._last_prob_log = now
            LOG.debug(
                "VAD prob=%.2f thr=%.2f/neg=%.2f speaking=%s sil=%.0fms",
                turn.vad.last_prob,
                turn.vad.threshold,
                turn.vad.neg_threshold,
                turn.vad.speech_started,
                turn.vad._silence_ms_run,
            )
        # Absolute wall-clock safety net from wake (NOT speech-start): fires even
        # if the VAD never detected speech, so a multi-second clip can never reach
        # Whisper's timeout. Checked before the VAD reasons so it always wins.
        if (now - turn.started_wall) * 1000.0 >= self.cfg.max_turn_ms:
            LOG.info("VAD: max turn %dms (absolute, from wake) -> forcing stop",
                     self.cfg.max_turn_ms)
            await self._send_stop(turn, source="max-turn")
            return
        if reason == "silence":
            LOG.info("VAD: silence %dms -> sending stop", self.cfg.silence_ms)
            await self._send_stop(turn, source="vad-silence")
        elif reason == "max_utterance":
            LOG.info("VAD: max utterance %dms -> forcing stop", self.cfg.max_utterance_ms)
            await self._send_stop(turn, source="vad-max")

    async def _vad_watchdog(self, turn: Turn) -> None:
        """Force a stop if the device stops delivering frames.

        Two caps run here on wall-clock (frames may stop arriving entirely):
          1. ABSOLUTE max_turn_ms from wake/stream-start -- the safety net that
             fires even if the VAD never saw speech (the LIVE-BUG scenario: a long
             clip with no detected speech must never reach Whisper).
          2. max_utterance_ms from speech start (only once speech was detected),
             for the "silence as no-frames mid-utterance" case.
        """
        try:
            while True:
                await asyncio.sleep(0.25)
                if turn is not self._turn or turn.stop_sent or turn.aborted:
                    return
                if turn.vad is None:
                    return
                now = time.monotonic()
                if (now - turn.started_wall) * 1000.0 >= self.cfg.max_turn_ms:
                    LOG.info(
                        "VAD watchdog: max turn %dms (absolute, from wake) -> forcing stop",
                        self.cfg.max_turn_ms,
                    )
                    turn.vad.fired = True
                    await self._send_stop(turn, source="max-turn-watchdog")
                    return
                if turn.vad.check_timeout(now) == "max_utterance":
                    LOG.info(
                        "VAD watchdog: max utterance %dms (no frames) -> forcing stop",
                        self.cfg.max_utterance_ms,
                    )
                    await self._send_stop(turn, source="vad-watchdog")
                    return
        except asyncio.CancelledError:  # pragma: no cover
            raise
        except Exception as exc:  # noqa: BLE001  # pragma: no cover
            LOG.debug("vad watchdog error: %s", exc)

    async def _send_stop(self, turn: Turn, source: str) -> None:
        """Flush one WAV blob, then send exactly one {type:"stop"} (idempotent).

        Hoshi buffers binary frames per session and transcribes once on stop, so
        a single complete WAV is exactly right: start -> 1 WAV blob -> stop.
        """
        if turn.stop_sent or turn.aborted:
            return
        turn.stop_sent = True
        if turn.vad is not None:
            turn.vad.fired = True
        self._event(VoiceAssistantEventType.VOICE_ASSISTANT_STT_VAD_END)

        # Build + send the turn's audio as ONE binary WAV blob before stop.
        await self._send_wav(turn)

        try:
            await turn.ws.send(json.dumps({"type": "stop"}))
            LOG.info("ws -> stop (source=%s, turn=%s); awaiting transcript/llm/tts",
                     source, turn.turn_id)
        except Exception as exc:  # noqa: BLE001
            LOG.warning("failed to send stop: %s", exc)

    async def _send_wav(self, turn: Turn) -> None:
        """Flush the accumulated mic PCM as exactly one binary WAV frame.

        Idempotent: at most one WAV blob per turn. Only `wav` is implemented;
        other --uplink-format values fall back to wav (webm = TODO, needs an
        encoder).
        """
        if turn.audio_sent:
            return
        turn.audio_sent = True
        pcm = bytes(turn.mic_pcm)
        if not pcm:
            LOG.warning("no mic audio buffered for turn %s -> sending empty WAV",
                        turn.turn_id)
        if self.cfg.uplink_format != "wav":
            LOG.warning("--uplink-format=%s not implemented -> using wav",
                        self.cfg.uplink_format)
        wav_bytes = pcm16_to_wav(pcm, sample_rate=DEVICE_SAMPLE_RATE, channels=1)
        audio_ms = (len(pcm) // 2) * 1000 // DEVICE_SAMPLE_RATE
        # Per-WAV RMS diagnostic: a too-quiet mic (low Silero prob) shows up here
        # as a low RMS and explains empty STT / no_input. ~<300 (of 32767) is
        # suspiciously quiet for near-field speech -> raise --mic-gain / device AGC.
        rms = pcm16_rms(pcm)
        rms_note = " LOW(empty-STT-risk)" if rms < 300 else ""
        try:
            n_chunks = 0
            for off in range(0, len(wav_bytes), UPLINK_CHUNK):
                await turn.ws.send(wav_bytes[off:off + UPLINK_CHUNK])
                n_chunks += 1
            LOG.info("ws -> WAV %d bytes in %d chunk(s) (%d ms audio) rms=%.0f%s "
                     "gain=%.1f turn=%s",
                     len(wav_bytes), n_chunks, audio_ms, rms, rms_note,
                     self.cfg.mic_gain, turn.turn_id)
        except Exception as exc:  # noqa: BLE001
            LOG.warning("failed to send WAV blob: %s", exc)

    # ---- End of utterance: device VAD stop (if the device ever sends it) ----
    async def _handle_stop(self, server_side: bool) -> None:
        """Device finished capturing (VAD end). Flush -> Hoshi processes the turn.

        Stock devices in API_AUDIO mode generally do NOT call this (no
        VoiceAssistantRequest(start=false)); our bridge-side VAD handles the
        common case. If the device DOES signal stop, honour it here -- still
        exactly one {type:"stop"} thanks to the idempotent _send_stop.
        """
        turn = self._turn
        if turn is None:
            return
        LOG.info("STOP from device (server_side=%s) turn=%s", server_side, turn.turn_id)
        await self._send_stop(turn, source="device")

    # ---- Hoshi downlink -> device -------------------------------------
    async def _consume_hoshi(self, turn: Turn) -> None:
        """Read Hoshi /ws/audio frames and drive device events + TTS playback."""
        tts_started = False
        try:
            async for raw in turn.ws:
                if isinstance(raw, (bytes, bytearray)):
                    # Contract: llm_audio is JSON text, not binary. Ignore stray binary.
                    LOG.debug("ignoring unexpected binary downlink frame")
                    continue
                msg = json.loads(raw)
                mtype = msg.get("type")

                if mtype == "transcript":
                    turn.transcript = msg.get("text", "")
                    LOG.info("ws <- transcript: %r", turn.transcript)
                    # STT done -> tell device the recognized text, advance to intent.
                    self._event(
                        VoiceAssistantEventType.VOICE_ASSISTANT_STT_END,
                        {"text": turn.transcript},
                    )
                    self._event(VoiceAssistantEventType.VOICE_ASSISTANT_INTENT_START)

                elif mtype == "no_input":
                    # Empty STT: end the turn QUIETLY (no ERROR event = no red
                    # light). The device wakes, hears nothing usable, and should
                    # fall back to idle, not flash an error.
                    LOG.info("no_input -> soft idle (no error event)")
                    self._end_turn_idle(turn, reason="no_input")
                    return

                elif mtype == "llm_start":
                    LOG.info("llm_start %s", {k: msg.get(k) for k in ("provider", "model")})

                elif mtype == "llm_delta":
                    # Streaming text; could be surfaced via INTENT_PROGRESS (UNVERIFIED).
                    pass

                elif mtype == "tts_audio_start":
                    if not tts_started:
                        self._event(VoiceAssistantEventType.VOICE_ASSISTANT_INTENT_END)
                        self._event(
                            VoiceAssistantEventType.VOICE_ASSISTANT_TTS_START,
                            {"text": turn.transcript},
                        )
                        self._event(VoiceAssistantEventType.VOICE_ASSISTANT_TTS_STREAM_START)
                        tts_started = True

                elif mtype == "llm_audio":
                    LOG.info("ws <- llm_audio seq=%s", msg.get("seq"))
                    if not tts_started:
                        # Some paths emit llm_audio without a tts_audio_start; open anyway.
                        self._event(VoiceAssistantEventType.VOICE_ASSISTANT_INTENT_END)
                        self._event(
                            VoiceAssistantEventType.VOICE_ASSISTANT_TTS_START,
                            {"text": turn.transcript},
                        )
                        self._event(VoiceAssistantEventType.VOICE_ASSISTANT_TTS_STREAM_START)
                        tts_started = True
                    await self._play_llm_audio(turn, msg)

                elif mtype == "tts_audio_end":
                    LOG.info("tts_audio_end actualMs=%s", msg.get("actualMs"))

                elif mtype == "llm_done":
                    LOG.info("ws <- llm_done ttsHandled=%s", msg.get("ttsHandled"))
                    if tts_started:
                        self._event(VoiceAssistantEventType.VOICE_ASSISTANT_TTS_STREAM_END)
                        self._event(VoiceAssistantEventType.VOICE_ASSISTANT_TTS_END)
                    self._event(VoiceAssistantEventType.VOICE_ASSISTANT_RUN_END)
                    turn.done.set()
                    return

                elif mtype == "llm_error":
                    stage = msg.get("stage")
                    err = msg.get("message", "")
                    LOG.error("llm_error stage=%s: %s", stage, err)
                    self._fail_turn(turn, f"{stage}: {err}")
                    return

                elif mtype == "turn_aborted":
                    LOG.info("turn_aborted %s", msg.get("turnId"))
                    self._fail_turn(turn, "aborted")
                    return

                elif mtype in ("transcribing_started", "llm_thinking", "session_meta",
                               "speaker", "sidecar_alarm"):
                    LOG.debug("downlink %s: %s", mtype, msg)
                else:
                    LOG.debug("unhandled downlink type %s", mtype)
        except websockets.ConnectionClosed:
            LOG.info("Hoshi ws closed for turn %s", turn.turn_id)
            if not turn.done.is_set():
                self._event(VoiceAssistantEventType.VOICE_ASSISTANT_RUN_END)
        except Exception as exc:  # noqa: BLE001
            LOG.exception("downlink loop error: %s", exc)
            self._fail_turn(turn, str(exc))
        finally:
            await self._close_turn(turn)

    async def _play_llm_audio(self, turn: Turn, msg: dict) -> None:
        """Decode base64 WAV (PCM16/24k), resample to 16k, stream to device speaker."""
        b64 = msg.get("data")
        if not b64:
            return
        try:
            wav_bytes = base64.b64decode(b64)
            pcm, sr, ch = wav_to_pcm16_mono(wav_bytes)
        except Exception as exc:  # noqa: BLE001
            LOG.warning("failed to decode llm_audio seq=%s: %s", msg.get("seq"), exc)
            return
        if sr != DEVICE_SAMPLE_RATE:
            pcm, self._resample_state = resample_pcm16(
                pcm, sr, DEVICE_SAMPLE_RATE, self._resample_state
            )
        # Pace writes to real time (device plays at 16k). Without pacing the whole
        # utterance bursts out in ~20ms and TTS_STREAM_END/TTS_END/RUN_END fire ~1ms
        # later -> the device's voice-assistant state machine returns to idle and cuts
        # playback before the speaker buffer has drained, so nothing is heard even
        # though the turn "completes". We keep ~one chunk of lead and clock the rest
        # against an absolute deadline (no cumulative drift, bounded buffer fill).
        loop = asyncio.get_running_loop()
        start = loop.time()
        lead_s = (SPEAKER_CHUNK // 2) / DEVICE_SAMPLE_RATE  # one chunk of buffer lead
        sent_samples = 0
        for off in range(0, len(pcm), SPEAKER_CHUNK):
            chunk = pcm[off : off + SPEAKER_CHUNK]
            if not chunk:
                break
            try:
                self.client.send_voice_assistant_audio(chunk)
            except Exception as exc:  # noqa: BLE001
                LOG.warning("send_voice_assistant_audio failed: %s", exc)
                return
            sent_samples += len(chunk) // 2  # PCM16 mono
            delay = start + (sent_samples / DEVICE_SAMPLE_RATE) - lead_s - loop.time()
            if delay > 0:
                await asyncio.sleep(delay)

    async def _handle_announce_finished(self, finished) -> None:
        LOG.info("announce finished success=%s", getattr(finished, "success", None))

    # ---- helpers ------------------------------------------------------
    def _event(self, event_type: VoiceAssistantEventType, data: Optional[dict] = None) -> None:
        if self.client is None:
            return
        try:
            self.client.send_voice_assistant_event(event_type, data)
            LOG.debug("-> event %s %s", event_type.name, data or "")
        except Exception as exc:  # noqa: BLE001
            LOG.warning("send event %s failed: %s", event_type, exc)

    def _end_turn_idle(self, turn: Turn, reason: str) -> None:
        """End a turn WITHOUT a device error event (no red light).

        For empty STT / no_input: emit a benign STT_END(text="") so the device's
        voice-assistant state machine advances out of LISTENING, then RUN_END ->
        the device returns to idle quietly instead of flashing the error feedback.
        (UNVERIFIED on real hardware; verify the LED behaviour live on your own device.)
        """
        turn.error = None
        LOG.info("ending turn idle (reason=%s, no error) turn=%s", reason, turn.turn_id)
        self._event(
            VoiceAssistantEventType.VOICE_ASSISTANT_STT_END,
            {"text": ""},
        )
        self._event(VoiceAssistantEventType.VOICE_ASSISTANT_RUN_END)
        turn.done.set()

    def _fail_turn(self, turn: Turn, reason: str) -> None:
        turn.error = reason
        # ERROR event lets the device drop back to idle.
        self._event(
            VoiceAssistantEventType.VOICE_ASSISTANT_ERROR,
            {"code": "bridge_error", "message": reason},
        )
        self._event(VoiceAssistantEventType.VOICE_ASSISTANT_RUN_END)
        turn.done.set()

    async def _close_turn(self, turn: Turn) -> None:
        try:
            await turn.ws.close()
        except Exception:  # noqa: BLE001
            pass
        if self._turn is turn:
            self._turn = None
        LOG.info("turn %s closed (transcript=%r error=%s)",
                 turn.turn_id, turn.transcript, turn.error)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="No-Flash bridge: HA Voice PE (ESPHome API) <-> Hoshi /ws/audio",
    )
    p.add_argument("--device-host", required=True,
                   help="Voice PE IP or .local hostname (ESPHome native API).")
    p.add_argument("--api-port", type=int, default=6053, help="ESPHome API port (default 6053).")
    p.add_argument("--api-encryption-key", default=None,
                   help="ESPHome API encryption key (noise_psk, base64). "
                        "Omit to try keyless first; env ESPHOME_API_KEY also works.")
    p.add_argument("--api-password", default=None,
                   help="Legacy ESPHome API password (env ESPHOME_API_PASSWORD).")
    p.add_argument("--hoshi-ws-url", default="wss://<hoshi-server-ip>:8081/ws/audio",
                   help="Hoshi /ws/audio URL. Required: set this to your own server.")
    p.add_argument("--cert", default=None,
                   help="PEM trust anchor for wss (default: certs/hoshi-server-leaf.pem "
                        "next to this repo, if present). env HOSHI_CERT also works.")
    p.add_argument("--token-file", default=None,
                   help="File with Hoshi ingress token (only if auth gate is ON). "
                        "env HOSHI_API_TOKEN also works.")
    p.add_argument("--room", default=None, help="Optional room id (optional start-frame identity field).")
    p.add_argument("--satellite-id", default=None, help="Optional satellite id (optional start-frame identity field).")
    p.add_argument("--uplink-format", choices=("wav", "webm"), default="wav",
                   help="Hoshi binary uplink container. Only 'wav' is implemented "
                        "(raw PCM is rejected by Hoshi STT); 'webm' is a TODO "
                        "placeholder and falls back to wav. (default: wav)")
    # Bridge-side end-of-speech (VAD) tunables. Needed because the device in
    # API_AUDIO mode never signals stop; the bridge detects silence itself.
    # VAD = Silero (ONNX); the silence threshold is the single most impactful
    # parameter and must be tuned against real recordings (research note).
    p.add_argument("--silence-ms", type=int, default=DEFAULT_SILENCE_MS,
                   help=f"Trailing silence (Silero prob<neg) that ends the utterance "
                        f"(default {DEFAULT_SILENCE_MS}).")
    p.add_argument("--vad-threshold", type=float, default=DEFAULT_VAD_THRESHOLD,
                   help=f"Silero speech probability >= this counts as speech, 0..1 "
                        f"(TV-hardened default {DEFAULT_VAD_THRESHOLD}).")
    p.add_argument("--vad-neg-threshold", type=float, default=None,
                   help=f"Hysteresis low watermark; prob<this counts as silence. "
                        f"Default = threshold - {DEFAULT_VAD_NEG_OFFSET}.")
    p.add_argument("--max-utterance-ms", type=int, default=DEFAULT_MAX_UTTERANCE_MS,
                   help=f"Hard cap from speech start -> force stop "
                        f"(default {DEFAULT_MAX_UTTERANCE_MS}).")
    p.add_argument("--max-turn-ms", type=int, default=DEFAULT_MAX_TURN_MS,
                   help=f"ABSOLUTE cap from wake/stream-start -> force stop, even "
                        f"if the VAD never detects speech (safety net so a long "
                        f"clip never reaches Whisper's timeout) "
                        f"(default {DEFAULT_MAX_TURN_MS}).")
    p.add_argument("--min-speech-ms", type=int, default=DEFAULT_MIN_SPEECH_MS,
                   help=f"Min voiced run before 'speech start' (start guard) "
                        f"(default {DEFAULT_MIN_SPEECH_MS}).")
    p.add_argument("--silero-model", default=None,
                   help=f"Path to the Silero VAD ONNX model "
                        f"(default: bridge/models/{SILERO_MODEL_FILENAME}).")
    p.add_argument("--mic-gain", type=float, default=1.0,
                   help="Digital gain on device mic PCM before VAD+STT "
                        "(clip-safe). Compensates a too-quiet mic (low Silero "
                        "prob -> empty STT -> no_input -> red). 1.0=off; try 2-4 "
                        "if the per-WAV rms log reads low. Real fix is device AGC "
                        "at Route-B flash. (default: 1.0)")
    # Deprecated no-op: kept so old invocations don't crash; ignored (VAD is now
    # Silero, not RMS). Will be removed in a later release.
    p.add_argument("--vad-rms-threshold", type=int, default=None,
                   help="DEPRECATED no-op (VAD is now Silero, not RMS energy).")
    p.add_argument("-v", "--verbose", action="store_true", help="DEBUG logging.")
    return p


def _default_cert() -> Optional[str]:
    # Convenience default: a leaf cert dropped in certs/hoshi-server-leaf.pem next
    # to the repo root (gitignored). Not present in a fresh checkout — pass --cert
    # explicitly (or env HOSHI_CERT) until you've placed your own server's leaf here.
    here = Path(__file__).resolve()
    cert = here.parents[2] / "certs" / "hoshi-server-leaf.pem"
    return str(cert) if cert.is_file() else None


async def amain(cfg: Config) -> None:
    bridge = Bridge(cfg)
    await bridge.run()


def main(argv: Optional[list[str]] = None) -> int:
    ns = build_parser().parse_args(argv)
    logging.basicConfig(
        level=logging.DEBUG if ns.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    if ns.vad_rms_threshold is not None:
        LOG.warning("--vad-rms-threshold is DEPRECATED and ignored (VAD is now Silero). "
                    "Use --vad-threshold (0..1).")
    cfg = Config.from_args(ns)
    if cfg.cert_path is None and cfg.hoshi_ws_url.startswith("wss://"):
        cfg.cert_path = _default_cert()
        if cfg.cert_path:
            LOG.info("using default cert %s", cfg.cert_path)
        else:
            LOG.warning("no cert for wss -> default trust store (self-signed will FAIL)")
    try:
        asyncio.run(amain(cfg))
    except KeyboardInterrupt:
        LOG.info("interrupted")
    return 0


if __name__ == "__main__":
    sys.exit(main())
