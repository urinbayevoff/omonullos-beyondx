#!/bin/sh
# zaudio.service's ExecStart (drop-in zaudio-vss.conf): the ABOX DSP with the
# CP's VSS region (snd-zrn-abox-bx vss_cp=1, VSS_CP in /etc/zaudio.env, 1 by
# default) unless the last boot that tried it did not last two minutes; then
# the zeroed stand-in. zaudio-vss-check.sh clears the mark and logs the DSP.
G=/var/lib/zaudio/vss-try
mkdir -p /var/lib/zaudio
ZAUDIO_VSS=${VSS_CP:-1}
if [ "$ZAUDIO_VSS" = 1 ] && [ -e $G ]; then
	ZAUDIO_VSS=0; echo "zaudio: the last boot with the CP's VSS did not last: stand-in"
fi
[ "$ZAUDIO_VSS" = 1 ] && date > $G && sync
export ZAUDIO_VSS
systemd-run -q --on-active=120 --unit=zaudio-vss-check /root/tools/zaudio-vss-check.sh || true
exec /root/tools/zaudio-load.sh
