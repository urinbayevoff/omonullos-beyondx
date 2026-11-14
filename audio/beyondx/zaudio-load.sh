#!/bin/sh
# zaudio-load.sh: the sound stack for the Galaxy S10 5G at boot (zaudio.service,
# or zaudio-vss.sh with ZAUDIO_VSS=1): the ALSA core modules built for
# 7.1.0-zboot, then the ABOX/CS35L40 driver (audio/beyondx/snd-zrn-abox-bx.c).
# Arguments go to the driver as module parameters.
# Added for OmonullOS, November 2026 (the phone's /root/tools copy since October).
cd /lib/modules/7.1.0-zboot/extra/snd
for m in soundcore snd snd-timer snd-pcm i2c-algo-bit; do insmod $m.ko 2>/dev/null; done
sync
insmod /root/tools/snd-zrn-abox-bx.ko vss_cp=${ZAUDIO_VSS:-0} "$@"
