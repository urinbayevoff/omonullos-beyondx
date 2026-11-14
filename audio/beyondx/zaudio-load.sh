#!/bin/sh
# zaudio-load.sh: the sound stack for the Galaxy S10 5G at boot (zaudio.service,
# or zaudio-vss.sh with ZAUDIO_VSS=1): the ALSA core modules built for
# 7.1.0-zboot, then the ABOX/CS35L40 driver (audio/beyondx/snd-zrn-abox-bx.c).
# Arguments go to the driver as module parameters.
# Added for OmonullOS, November 2026 (the phone's /root/tools copy since October).
cd /lib/modules/7.1.0-zboot/extra/snd
for m in soundcore snd snd-timer snd-pcm i2c-algo-bit; do insmod $m.ko 2>/dev/null; done
sync
# insmod reads no /etc/modprobe.d: take the driver's "options snd_zrn_abox_bx ..."
# lines from there by hand (dsp_cal_r, the measured RDC); if they do not load,
# the driver loads without them as before
OPTS=$(sed -n 's/^[[:space:]]*options[[:space:]]\+snd[-_]zrn[-_]abox[-_]bx[[:space:]]\+//p' \
	/etc/modprobe.d/snd-zrn-abox-bx.conf 2>/dev/null | tr '\n' ' ')
# shellcheck disable=SC2086
insmod /root/tools/snd-zrn-abox-bx.ko vss_cp=${ZAUDIO_VSS:-0} $OPTS "$@" ||
	{ [ -n "$OPTS" ] && echo "zaudio-load: options '$OPTS' refused, loading without" >&2 &&
	  insmod /root/tools/snd-zrn-abox-bx.ko vss_cp=${ZAUDIO_VSS:-0} "$@"; }
