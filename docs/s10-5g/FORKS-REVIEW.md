# Other ztOS ports: what is worth taking (reviewed 2026-11-14)

Source list: devices.ztos.online (data `https://ztos.online/devices/index.json` + `<id>/device.json`),
10 phones. Read only (gh api / shallow clone in scratch), nothing run. Upstream ztOS itself has
nothing new for us: our merge base is BETA-1.1 (94b35c2), after it upstream only added device
entries. The store (ztOS-apprepo) lists only `zrn-example-hello` and `zrn-whatsapp`.

## Worth taking, in order

1. **Tube — YouTube without an account or ads, Metro style** (GPL-3.0).
   `madesh-ops/ztos-tube` (standalone) and `madesh-ops/ztOS` branch `m20lte-port`, `apps/tube/`
   (PySide6 + yt-dlp + mpv; ~135 KB of Python). Our phone already has pyside6 6.12, mpv 0.41,
   yt-dlp 2025.12.08 in pacman. What's hot / subscriptions (no Google account) / search /
   history / downloads, background play, up to 1080p60, H.264/VP9 (never AV1).
   Comes with two commits it relies on:
   - `b5918a6` ztMETRO: an app asking for full screen (xdg set_fullscreen) gets the whole
     screen on its side, no status/nav bar; upright gives them back (ztmetro.c +95-13).
   - `5aa04c2` SDK: a pivot's slide keeps its app bar, renders its tabs whole
     (sdk/runtime/ztos/__init__.py +31-1).
   Need: check its SDK runtime calls against ours (`sdk/runtime/ztos`), Uzbek strings.

2. **Settings shows the S9's hardware on the S10.** ztsettings.c still prints «Exynos 9810»,
   «Mali-G72 MP18», «3000 mAh», «3.5 GB RAM» (lines ~1357-1363, 3316, 3334, 3417).
   `Davitekk/ztOS` branch `starqltechn-port` has `device_fact()` reading
   `/vendor/ztos/manifest.toml [device]`, RAM from sysinfo(), CPU range from cpufreq,
   core count from sysconf. Same idea in `VachaganGrigoryan/ztos-ports`
   `patches/ztsettings-device-info.diff`. S10 5G: Exynos 9820, Mali-G76 MP12, 8 GB, 4500 mAh.

3. **ztMETRO aborts on a requested restart** (display size, style): backend listeners not
   removed before `wlr_backend_destroy`, wlroots asserts, greetd restarts it. Found by
   `cyber-dream/ztOS-miatoll` (README «Known issues»). Upstream's bug, likely ours too.

4. **Audio output switch** (volume panel): skip PipeWire smart filter sinks (`filter.smart`,
   Davitekk starqltechn-port ztmetro.c) and outputs whose port is «not available»
   (miatoll note: it got pinned to Headphones with nothing plugged in).

5. Small ones:
   - ztmetro.c Wi-Fi bars: pick an interface with `/sys/class/net/X/wireless`, not any name
     starting with `w` (waydroid0 is always up) — Davitekk starqltechn-port.
   - ztOSIAL generic.py: skip `tcpm-source-psy-*` as a charger; `"auto"` = no node
     (miatoll 7925adb/upstream-fixes); gyroscope (`in_anglvel_*`), `in_proximity0_raw`,
     `in_illuminance0_input` (Davitekk talkman-port).
   - ztoscsf: a device layer's `/vendor/ztos/csf/components` list, sensor hub watched only
     when listed (miatoll c4ddf8d) — matters while the S10's sensorhub is not up.
   - Waydroid boot/recover patches (VachaganGrigoryan/ztos-ports bonito) if we use Waydroid.

## Looked at, nothing for us

- `flaherxd/ztOS` port/oneplus-fajita: torch sysfs paths, libcamera/GStreamer camera (SDM845).
- `Davitekk/ztOS` talkman-port: CAMSS camera source, perfd without cpufreq (Lumia 950).
- `cyber-dream/ztOS-miatoll`: Snapdragon device layer; TAS2562 volume byte-order kernel fix.
- `Noxyntious/ztOS` (daisy): README only; work is in its ztconflo fork.
- `emir34-34/ubuntu-touch-poco-x8-pro`: Ubuntu Touch, not ztOS.
- `madesh-ops/ios-m20lte` (iOS on Galaxy S7), `madesh-ops/earbods` (Windows app): unrelated.
