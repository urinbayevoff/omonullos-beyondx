#!/bin/sh
# Added for OmonullOS, October 2026. Run on the phone (systemd-run
# --unit=zdecon-test /root/tools/zdecon-test.sh) while someone watches the panel.
# DECON flip test: IDMA G0 (0x19071000, dpp0's first reg) scanout base alternates between the framebuffer and
# 600 lines further in (inside the 20 MB reservation), 10 s each, for 8 min.
G=0x19071000 D=0x19030000 FB=0xca000000 SHIFT=$((0xca000000 + 600 * 5760))
flip() {
	devmem $((G + 0x40)) 32 $1
	devmem $((D + 0x60)) 32 $(( $(devmem $((D + 0x60)) 32) | 0x20 ))
}
trap 'flip $FB; echo "zboot| decon test: stopped, base restored" > /dev/kmsg; exit 0' INT TERM
echo "zboot| decon test: start" > /dev/kmsg
end=$(( $(date +%s) + 480 ))
while [ $(date +%s) -lt $end ]; do
	echo "screen on" > /run/user/1000/ztmetro.ctl
	flip $SHIFT; echo "zboot| decon test: shifted" > /dev/kmsg; sleep 10
	flip $FB; echo "zboot| decon test: normal" > /dev/kmsg; sleep 10
done
flip $FB
echo "zboot| decon test: done, base restored" > /dev/kmsg
