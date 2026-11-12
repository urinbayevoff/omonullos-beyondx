#!/usr/bin/env python3
"""OmonullOS's own UI sounds for ztos-sounds (system/*.wav).

Upstream plays the Windows Phone sound kit there, which is Microsoft's audio
and not in git. These are synthesised here instead: short sine partials
with a fast attack and an exponential decay, 48 kHz 16-bit mono.

The S10 5G's speaker has no protection DSP yet (audio work is paused), so
every sound stays moderate (peak -5 dBFS) and above 600 Hz.

    os/sounds/make-system-sounds.py [OUT_DIR]

OUT_DIR defaults to os/overlay/usr/local/share/sounds/ztos/system.
"""
import math
import os
import struct
import sys
import wave

RATE = 48000
PEAK = 10 ** (-5 / 20)	# -5 dBFS (was -10: too quiet on the phone)


def tone(freq, length, decay, partials=((1.0, 1.0),), attack=0.003, start=0.0):
	"""One struck note: (ratio, level) partials of freq, decaying with time constant `decay`."""
	n = int(length * RATE)
	first = int(start * RATE)
	out = [0.0] * (first + n)
	for i in range(n):
		t = i / RATE
		env = min(1.0, t / attack) * math.exp(-t / decay)
		v = sum(level * math.sin(2 * math.pi * freq * ratio * t) for ratio, level in partials)
		out[first + i] = env * v
	return out


def mix(*tracks):
	n = max(len(t) for t in tracks)
	return [sum(t[i] for t in tracks if i < len(t)) for i in range(n)]


def write(path, samples, gain=1.0):
	top = max(abs(s) for s in samples) or 1.0
	scale = PEAK * gain / top
	# 4 ms fade at the end, so nothing clicks
	fade = int(0.004 * RATE)
	for i in range(fade):
		samples[-1 - i] *= i / fade
	with wave.open(path, "wb") as w:
		w.setnchannels(1)
		w.setsampwidth(2)
		w.setframerate(RATE)
		w.writeframes(b"".join(struct.pack("<h", int(s * scale * 32767)) for s in samples))


BELL = ((1.0, 1.0), (2.0, 0.35), (3.01, 0.12), (4.2, 0.05))
SOFT = ((1.0, 1.0), (2.0, 0.2))

SOUNDS = {
	# the chime as the phone finishes starting, and for notifications: two bell notes, a fifth apart
	"notify": lambda: mix(tone(1046.5, 0.9, 0.28, BELL), tone(1568.0, 0.9, 0.32, BELL, start=0.12)),
	# locking: one low, dry knock
	"lock": lambda: tone(720, 0.09, 0.022, SOFT),
	# unlocking: two quick rising blips
	"unlock": lambda: mix(tone(880, 0.07, 0.02, SOFT), tone(1320, 0.09, 0.025, SOFT, start=0.055)),
	# copy: a light tick
	"copy": lambda: tone(1760, 0.05, 0.012, SOFT),
	# a menu opening: a soft pop
	"menu": lambda: tone(1180, 0.07, 0.018, SOFT),
	# battery low: two falling notes
	"battery-low": lambda: mix(tone(988, 0.35, 0.12, BELL), tone(740, 0.45, 0.15, BELL, start=0.2)),
	# pip: the smallest blip (volume steps)
	"pip": lambda: tone(2093, 0.04, 0.01, SOFT),
}
GAIN = {"copy": 0.7, "pip": 0.6, "menu": 0.8}

if __name__ == "__main__":
	root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.realpath(__file__))))
	out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, "os/overlay/usr/local/share/sounds/ztos/system")
	os.makedirs(out, exist_ok=True)
	for name, make in SOUNDS.items():
		path = os.path.join(out, name + ".wav")
		write(path, make(), GAIN.get(name, 1.0))
		print(path)
