# OmonullOS's ringtone: a marimba-like phrase (two partials, 1x and 3.93x,
# with their own decays), 48 kHz 16-bit stereo, played in a loop by zcell.
#   python3 make-ring.py omonullos-ring.wav
import math, struct, sys, wave
RATE = 48000
NOTES = [(0.00, 76), (0.18, 79), (0.36, 83), (0.54, 88), (0.90, 83), (1.08, 88),
	(1.62, 76), (1.80, 79), (1.98, 83), (2.16, 88), (2.52, 91)]	# (s, MIDI)
LENGTH = 3.6
buf = [0.0] * int(RATE * LENGTH)
for at, midi in NOTES:
	f = 440 * 2 ** ((midi - 69) / 12)
	start = int(at * RATE)
	for i in range(int(1.0 * RATE)):
		if start + i >= len(buf):
			break
		t = i / RATE
		attack = min(1.0, t / 0.003)
		v = math.sin(2 * math.pi * f * t) * math.exp(-t * 4.5) \
			+ 0.35 * math.sin(2 * math.pi * f * 3.93 * t) * math.exp(-t * 18)
		buf[start + i] += attack * v
peak = max(abs(x) for x in buf)
scale = 0.7 * 32767 / peak	# -3 dBFS
with wave.open(sys.argv[1], 'wb') as w:
	w.setnchannels(2); w.setsampwidth(2); w.setframerate(RATE)
	w.writeframes(b''.join(struct.pack('<hh', int(x * scale), int(x * scale)) for x in buf))
