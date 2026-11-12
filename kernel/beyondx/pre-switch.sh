# Galaxy S10 5G: initramfs's last step before switch_root. Sourced by
# initramfs/init as /etc/zboot/pre-switch (build-boot.sh --device beyondx
# --pet).
#
# Its loops (the watchdog pet from wdt-pet.sh, zthermal, zchg) exec busybox
# applets from this root, which is gone after switch_root: they would spin
# on failed execs. So they stop here, the watchdog is reloaded one last
# time, and the root system's zboot-guards.service starts them again within
# the 30 s that gives (it runs at sysinit).
#
# Added for OmonullOS, October 2026.
[ -n "$WDT_PET_PID" ] && kill "$WDT_PET_PID" 2>/dev/null
killall zchg zthermal 2>/dev/null
dat=$(devmem 0x10050004 32)
[ $((dat)) -gt 0 ] && devmem 0x10050008 32 "$dat"
log "pre-switch: loops stopped, watchdog reloaded ($dat), zboot-guards takes over"
