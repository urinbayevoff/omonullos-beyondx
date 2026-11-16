#!/usr/bin/env python3
# ed-trial helper: once 1-0048 is bound to zrn-s6sy761, a touch on its
# touchscreen input ("s6sy761", not "s6sy761 ear detect") makes $1/touch-ok.
import os, sys, time, glob, select
S = sys.argv[1]
D = '/sys/bus/i2c/devices/1-0048'
t = time.time()
while time.time() - t < 150:
    try:
        if os.path.basename(os.readlink(D + '/driver')) == 'zrn-s6sy761':
            break
    except OSError:
        pass
    time.sleep(0.2)
else:
    print('driver never bound'); sys.exit(1)
time.sleep(0.5)
ev = [e for e in glob.glob(D + '/input/input*/event*')
      if open(os.path.dirname(e) + '/name').read().strip() == 's6sy761']
print('watching', ev, flush=True)
fd = os.open('/dev/input/' + os.path.basename(ev[0]), os.O_RDONLY)
while time.time() - t < 150:
    r, _, _ = select.select([fd], [], [], 1)
    if r and len(os.read(fd, 24 * 64)) > 0:
        open(S + '/touch-ok', 'w').close(); print('touch seen', flush=True); break
