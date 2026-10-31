#!/usr/bin/env python3
"""Renders the "classic-wood" sound set (assets/sounds/classic-wood/*.wav) by modal synthesis.

A strike is a short raised-cosine contact pulse (plus a little filtered noise) driving a bank of
exponentially damped sinusoids; the contact time sets brightness, the mode table sets the material.
Wood: inharmonic modes, low Q, higher modes dying faster. Chimes use marimba bar ratios (1, 4, 10).
Deterministic (fixed seed). Needs only numpy. Output is CC0 (made for opcan).

usage: python3 tools/gen_wood_sounds.py [out_dir]
"""
import os
import sys
import wave

import numpy as np

SR = 48000
rng = np.random.default_rng(7)


def db(x):
    return 10 ** (x / 20)


def strike(modes, contact_ms, noise=0.0, pitch=1.0, length=0.4, jitter=0.015):
    """modes: [(freq Hz, decay tau ms, amp)]. Returns the struck response, peak-normalised to 1."""
    n = int(length * SR)
    t = np.arange(n) / SR
    h = np.zeros(n)
    for f, tau, a in modes:
        f *= pitch * (1 + rng.uniform(-jitter, jitter))
        a *= 1 + rng.uniform(-0.1, 0.1)
        h += a * np.exp(-t / (tau / 1000)) * np.sin(2 * np.pi * f * t)
    # contact force: raised cosine of contact_ms (shorter = brighter, a low-pass on the modes)
    m = max(2, int(contact_ms / 1000 * SR))
    pulse = 0.5 - 0.5 * np.cos(2 * np.pi * np.arange(m) / m)
    y = np.fft.irfft(np.fft.rfft(h, 2 * n) * np.fft.rfft(pulse, 2 * n), 2 * n)[:n]
    if noise:
        # the "tick" of the contact itself: ~3 ms of band-limited noise (first difference = high-pass)
        k = int(0.003 * SR)
        w = rng.standard_normal(k + 1)
        w = np.diff(w) * np.exp(-np.arange(k) / (0.0008 * SR))
        w = np.convolve(w, np.ones(3) / 3, "same")  # tame the very top
        y[:k] += noise * np.abs(y).max() * w / np.abs(w).max()
    return y / np.abs(y).max()


# boxwood piece with a felt-less base landing on a wooden board
TOCK = [
    (210, 9, 0.30),     # board body
    (640, 16, 0.45),
    (1180, 22, 1.00),   # main "tock"
    (1720, 13, 0.45),
    (2610, 8, 0.42),
    (3870, 5, 0.25),
    (5400, 3, 0.10),
]
# a temple-block "tick" for check: fewer, longer-ringing modes, higher
BLOCK = [(1480, 42, 1.0), (3550, 14, 0.30), (5200, 6, 0.10)]


def tock(pitch=1.0, contact=0.5, noise=0.25):
    return strike(TOCK, contact, noise, pitch, length=0.25)


def marimba(freq, length=1.3, mallet=1.0):
    return strike([(freq, 260 * (600 / freq) ** 0.6, 1.0), (freq * 3.98, 70, 0.22), (freq * 9.9, 18, 0.06)],
                  mallet, 0.0, length=length, jitter=0.002)


def mix(parts, length):
    out = np.zeros(int(length * SR))
    for at_ms, gain, x in parts:
        i = int(at_ms / 1000 * SR)
        x = x[: len(out) - i]
        out[i:i + len(x)] += gain * x
    return out


def finish(x, peak_db, floor_db=-60, fade_ms=5):
    # one-pole high-pass at 40 Hz: no DC offset or sub-bass thump
    c = np.exp(-2 * np.pi * 40 / SR)
    y, prev_x, prev_y = np.empty_like(x), 0.0, 0.0
    for i, v in enumerate(x):
        prev_y = c * (prev_y + v - prev_x)
        prev_x = v
        y[i] = prev_y
    x = y / np.abs(y).max()
    # trim the tail where it falls below floor_db for good, with a 5 ms fade
    idx = np.where(np.abs(x) > db(floor_db))[0]
    end = min(len(x), idx[-1] + int(0.005 * SR))
    x = x[:end]
    fade = int(fade_ms / 1000 * SR)
    x[-fade:] *= np.linspace(1, 0, fade)
    return x * db(peak_db)


def write(path, x):
    pcm = np.clip(np.round(x * 32767), -32768, 32767).astype("<i2")
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(pcm.tobytes())


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "assets", "sounds", "classic-wood")
    os.makedirs(out, exist_ok=True)
    sounds = {
        # one soft tock
        "Move": finish(tock(), -3),
        # piece knocks piece (higher, brighter), then lands
        "Capture": finish(mix([(0, 0.75, tock(1.22, 0.3, 0.45)), (34, 1.0, tock(0.97, 0.45, 0.3))], 0.3), -3),
        # rook then king
        "Castle": finish(mix([(0, 0.85, tock(0.93)), (105, 1.0, tock(1.0))], 0.4), -3),
        # the move, then a light temple-block tick
        "Check": finish(mix([(0, 1.0, tock()), (85, 0.62, strike(BLOCK, 0.45, 0.15, length=0.3))], 0.45), -3),
        # last move, then a calm low fifth
        "Checkmate": finish(mix([(0, 1.0, tock()), (130, 0.55, marimba(392.0, 1.5)), (195, 0.45, marimba(587.33, 1.4))], 1.6), -8, fade_ms=150),
        # two rising marimba notes
        "GenericNotify": finish(mix([(0, 0.8, marimba(659.25)), (115, 0.9, marimba(880.0, 1.2))], 1.3), -8, fade_ms=150),
        # retry solved: a small major arpeggio, soft mallet
        "Success": finish(mix([(0, 0.6, marimba(1046.5, 0.8, 0.55)), (75, 0.6, marimba(1318.5, 0.8, 0.55)),
                                (150, 0.75, marimba(1568.0, 0.8, 0.55))], 0.95), -9, fade_ms=150),
    }
    for name, x in sounds.items():
        write(os.path.join(out, name + ".wav"), x)
        print(f"{name}.wav  {len(x) / SR * 1000:.0f} ms")


if __name__ == "__main__":
    main()
