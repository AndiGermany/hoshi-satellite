#!/usr/bin/env python3
"""Offline unit tests for the WAV uplink builder (napi_bridge.pcm16_to_wav).

Pure-Python, no device/Hoshi/network. Verifies that pcm16_to_wav produces a
canonical 44-byte RIFF/WAVE header with correct size/length fields for known
PCM payloads, and that Python's stdlib `wave` module reads it back as a valid
PCM16 mono 16 kHz stream with the exact sample bytes round-tripping.

Run:
  .venv/bin/python -m pytest test_wav_builder.py -q
  # or without pytest:
  .venv/bin/python test_wav_builder.py
"""
from __future__ import annotations

import io
import struct
import wave

from napi_bridge import DEVICE_SAMPLE_RATE, pcm16_to_wav


def _check(pcm: bytes, sample_rate: int = DEVICE_SAMPLE_RATE, channels: int = 1) -> None:
    wav = pcm16_to_wav(pcm, sample_rate=sample_rate, channels=channels)
    data_size = len(pcm)

    # 1. 44-byte header (16-byte PCM fmt chunk) + payload.
    assert len(wav) == 44 + data_size, f"len {len(wav)} != {44 + data_size}"

    # 2. Unpack the header and check every field by hand.
    (riff, riff_size, wave_tag, fmt_tag, fmt_size, audio_fmt, ch, rate,
     byte_rate, block_align, bits, data_tag, ds) = struct.unpack(
        "<4sI4s4sIHHIIHH4sI", wav[:44]
    )
    assert riff == b"RIFF"
    assert wave_tag == b"WAVE"
    assert fmt_tag == b"fmt "
    assert data_tag == b"data"
    assert fmt_size == 16
    assert audio_fmt == 1                      # PCM
    assert ch == channels
    assert rate == sample_rate
    assert bits == 16
    assert block_align == channels * 2
    assert byte_rate == sample_rate * channels * 2
    assert ds == data_size                     # data chunk size
    assert riff_size == 36 + data_size         # RIFF size = 36 + data
    assert wav[44:] == pcm                      # payload follows header verbatim

    # 3. stdlib `wave` must accept it and round-trip the exact PCM bytes.
    with wave.open(io.BytesIO(wav), "rb") as wf:
        assert wf.getnchannels() == channels
        assert wf.getsampwidth() == 2
        assert wf.getframerate() == sample_rate
        assert wf.getnframes() == data_size // (2 * channels)
        assert wf.readframes(wf.getnframes()) == pcm


def test_empty() -> None:
    _check(b"")


def test_small_known() -> None:
    # 4 samples of PCM16 mono => 8 bytes => header says data=8, riff=44.
    pcm = struct.pack("<4h", 0, 1000, -1000, 32767)
    _check(pcm)
    wav = pcm16_to_wav(pcm)
    assert struct.unpack_from("<I", wav, 4)[0] == 36 + 8   # riff_size
    assert struct.unpack_from("<I", wav, 40)[0] == 8       # data_size


def test_one_second() -> None:
    # 1 s @ 16 kHz mono PCM16 = 16000 samples = 32000 bytes.
    pcm = b"\x00\x10" * DEVICE_SAMPLE_RATE
    _check(pcm)
    wav = pcm16_to_wav(pcm)
    assert len(wav) == 44 + 32000
    with wave.open(io.BytesIO(wav), "rb") as wf:
        assert wf.getnframes() == DEVICE_SAMPLE_RATE  # 1.0 s


def test_odd_length_byte_count() -> None:
    # Builder must not assume even length: 5 bytes still produce a valid header
    # with data_size=5 (wave may reject a partial sample on read, so only the
    # header math is asserted here).
    pcm = b"\x01\x02\x03\x04\x05"
    wav = pcm16_to_wav(pcm)
    assert len(wav) == 44 + 5
    assert struct.unpack_from("<I", wav, 40)[0] == 5
    assert struct.unpack_from("<I", wav, 4)[0] == 36 + 5


if __name__ == "__main__":
    test_empty()
    test_small_known()
    test_one_second()
    test_odd_length_byte_count()
    print("all WAV-builder tests passed")
