**Title:** Proposal: optional iPhone-style gesture line instead of the three nav buttons (`nav_style = line`)

> 2026-11-10: taklif egasining roziligi bilan Ometh'ga EMAIL orqali yuborildi (u issue emas, Gmail o'qiydi). Javob kelsa — «xarita» (kod + ishlash tushuntirishi) va patch/PR tayyorlanadi.

Hi! I run ztOS (ported to a Galaxy S10 5G) as my daily phone and added an optional navigation mode to ztMETRO. It has worked well for me, and I would like to offer it upstream if you are interested.

**What it is.** A new config key `nav_style = line` (default stays `buttons`, nothing changes unless it is set). The nav bar is replaced by a thin pill-shaped line over the app, drawn on a small strip of frosted glass:

- swipe up from the bottom: Start
- swipe up and rest on the way: the app switcher (the app shrinks into its card as the finger rises); flick a card up to close it
- swipe along the line: previous / next app, the two apps sliding side by side; the order is kept while you go back and forth, as on iOS
- left edge swipe: back
- pull in from the right edge and hold 2 s: show / hide the on-screen keyboard (a small ring fills while holding)
- pull Start down: Hyperscan
- swipe up over the keyboard's bottom edge still works as the line (the line itself is hidden while the keyboard is up)

**Motion.** The line follows the finger and springs back; all its motions use the iOS 26 spring (critically damped, stiffness 555, damping 47.1, 0.5 s). The line is hidden on Start, in the switcher, over Hyperscan and over the pulled-down Action Center (a swipe up from the bottom closes the Action Center there), and it fades out over any app left untouched for 3 s, so it costs nothing to draw most of the time.

**Cost.** Measured with the frame log: the switcher and all line animations run at 60 fps on the Exynos 9820 / Mali-G76 (about 1 ms draw per frame). The settings app has a matching option.

**Code.** It is all in `ztmetro.c` (behind a `NAV_LINE` check, so the button mode is untouched) plus one option in `ztsettings.c`, GPLv3 like the rest. I can prepare a pull request or a patch against current main, whichever you prefer, and adjust it to your style (for example, drop the S10-specific bits).

Thanks for ztOS!
