# zboot's /etc/zboot/splash for the Galaxy S10 5G (beyondx): sourced by /init
# first thing, ~0.15 s after the kernel starts.
#
# Added for OmonullOS, November 2026. The boot screen from here on, in the
# way flicker-free boots do it (Fedora: the firmware's logo stays, the kernel
# draws nothing, Plymouth carries on; postmarketOS: pbsplash draws from the
# initramfs): the kernel defers fbcon's take-over until there is text
# (FRAMEBUFFER_CONSOLE_DEFERRED_TAKEOVER) and prints none (loglevel=1,
# INITRAMFS_FORCE), this puts the first picture of zsplash on simpledrm's
# framebuffer, and zsplash, started by systemd from the root, draws the same
# picture and goes on from it. Nothing changes on the screen between them.
#
# splash.raw.gz: zsplash's first frame, 1440x3040 BGRX, made on the phone:
#   zsplash --video 2 1440 | head -c 17510400 | gzip -9 -n > splash.raw.gz
# so it changes with zsplash's look (the name, the foot).
#
# Not with zboot.rescue=1 or zboot.splash=0, and only on the framebuffer it
# was made for. Then /init's own output goes to its log, so a stray message
# from a tool cannot bring fbcon in; the rescue shell writes to /dev/tty0 and
# does (its text is then on the screen, as it should be).
splash_fb=/sys/class/graphics/fb0
if [ "$(arg zboot.rescue)" != 1 ] && [ "$(arg zboot.splash)" != 0 ] &&
   [ -r /etc/zboot/splash.raw.gz ] && [ -c /dev/fb0 ] &&
   [ "$(cat $splash_fb/virtual_size 2>/dev/null)" = 1440,3040 ] &&
   [ "$(cat $splash_fb/stride 2>/dev/null)" = 5760 ] &&
   [ "$(cat $splash_fb/bits_per_pixel 2>/dev/null)" = 32 ]; then
	if zcat /etc/zboot/splash.raw.gz > /dev/fb0 2>/dev/null; then
		log "splash: on $(cat $splash_fb/name 2>/dev/null)"
		exec >> "$LOG" 2>&1 < /dev/null
	else
		log "splash: writing /dev/fb0 failed"
	fi
else
	log "splash: none (rescue, zboot.splash=0 or fb0 $(cat $splash_fb/virtual_size 2>/dev/null))"
fi
unset splash_fb
