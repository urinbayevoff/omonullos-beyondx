#!/bin/sh
# two minutes after zaudio: the boot lasted, so the VSS try mark goes (only
# when this boot made it), and what the DSP said goes to the field logs
P=/sys/module/snd_zrn_abox_bx/parameters
[ "$(cat $P/vss_cp 2>/dev/null)" = 1 ] && rm -f /var/lib/zaudio/vss-try && sync
L=/var/lib/zfield/repo/logs/vss-$(date +%m%d-%H%M).log
{ echo "vss_cp=$(cat $P/vss_cp 2>&1) mark=$(cat /var/lib/zaudio/vss-try 2>/dev/null)"
  dmesg | grep -i -E 'abox|vss|sysmmu|iommu|fault' | tail -40
  echo == dsplog; cat $P/dsplog 2>&1 | tail -80; } > $L 2>&1
