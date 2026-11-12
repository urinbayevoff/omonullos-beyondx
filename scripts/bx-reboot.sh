#!/bin/sh
# reboot the phone into bx (Arch): reboot goes to TWRP, adb reboot from there.
# --wdt: stop petting the watchdog instead (TWRP in ~30 s), for when a
# shutdown would wait on a stuck driver (cbd in BOOT_OFF waits up to 300 s)
K="-i $HOME/.ssh/omonullos_ed25519 -o ConnectTimeout=3"
if [ "$1" = --wdt ]; then
	ssh $K root@172.16.42.1 'sync; touch /run/zboot.nopet' 2>/dev/null
else
	ssh $K root@172.16.42.1 'sync; reboot' 2>/dev/null
fi
for i in $(seq 1 60); do sleep 3; adb devices 2>/dev/null | grep -q "recovery$" && break; done
adb reboot; sleep 10
for i in $(seq 1 40); do sleep 3; timeout 4 ssh $K root@172.16.42.1 true 2>/dev/null && break; done
ssh $K root@172.16.42.1 "date -u -s @$(date +%s) >/dev/null; zacpm rtc set; uptime"
