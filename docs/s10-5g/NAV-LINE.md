# Navigatsiya chizig'i (iPhone uslubi) — reja

Yangi sessiya nav bo'yicha shu fayldan boshlanadi. Kod: `ztmetro/ztmetro.c` (`NAV_LINE`, `glass_draw`,
`nav_line_end`, `LINE_*` konstantalari), `ztmetro/settings/ztsettings.c` (`ACT_NAV_STYLE`).
Telefonda yig'ish `~/m3/ztmetro` da; o'rnatish namunasi `~/m3/u-navline.sh` (greetd restart + 3 daqiqa rollback).

## Holat (2026-11-10)

✅ 1-bosqich o'rnatildi va egasi sinadi («yaxshi yoqdi», lekin «silliq emas»):
- Sozlamalar → navigatsiya paneli → "Navigatsiya usuli": Tugmalar / Chiziq (`nav_style = line`), SIGHUP bilan almashadi.
- 20 pt shisha tasma (yuqoridagi 28 pt dan blur + tus), chiziq fonga qarab oq/qora.
- Yuqoriga surish = Start; yo'lda to'xtash (220 ms, 70 pt dan baland) = switcher; chiziq bo'ylab = keyingi ilova;
  o'ngdagi ⌨ = klaviatura; chap chetdan = orqaga (chiziq rejimida yopish emas).
- Chiziq rejimida display plane o'chiq (shisha kadrdan yasaladi) — batareya biroz ko'proq.

✅ 1-bosqich silliqligi (branch `nav-smooth`, 10.11, egasi sinadi: «yaxshi ishladi», main'ga qo'shildi):
- Surayotganda ilova barmoq ortidan kichrayadi (switcher kartochkasi kabi, barmoq kartochkaning pastki chetida),
  yon tomonga ham ergashadi (`line_t`, `line_dx`, `line_place`, `line_follow`, `line_settle`).
- Qo'yib yuborish: yetarli ko'tarilsa yoki tez silkitsa (`LINE_FLICK` 0.6 pt/ms, 12 pt dan ko'p) — Start,
  ilova turnstilesiz kichrayib so'nadi; yetmasa — ilova 280 ms da joyiga qaytadi.
- To'xtab turish → switcher shu kartochkadan uzluksiz davom etadi (`switch_t`, `switch_x` shu joydan).
- Chiziq bo'ylab: keyingi ilova barmoq kelgan tomondan siljib kiradi (eskisi hozircha chizilmaydi — qora bo'shliq).
- O'rnatish: `sudo sh ~/m3/u-navsmooth.sh` (faqat ztmetro, greetd restart, 3 daq. crash-rollback;
  yoqmasa qaytarish: `/usr/local/bin/ztmetro.bak-navsmooth`).

✅ 2-bosqich — faqat chiziq (branch `line-only`, 10.11, egasi sinadi: «yaxshi ishladi», ⌨ xalaqit bermaydi; main):
- Chiziq rejimida `nav_h = 0`: ilova pastki chetgacha; `NAV_LINE` endi `nav_h == 0` bilan, DotMatrix uslubi tugmalarni saqlaydi.
- Pill (`glass_draw`) ilova ustida, ostidagi bo'lak o'rtacha yorqinligiga qarab qora/oq (shader faqat pill atrofidagi qutini o'qiydi).
- Pastki `LINE_ZONE` = 16 pt: barmoq siljisa — imo-ishora; joyida qo'yib yuborilsa yoki 300 ms ushlab turilsa
  (`LINE_HOLD_MS`) — `line_replay` xuddi shu joyga ilovaga/klaviaturaga/Start'ga qayta tushiradi.
  Switcher va panel ustida qayta yuborilmaydi.
- ⌨ — o'ng pastda kichik suzuvchi yumaloq tugma (`KBD_BTN` 34 pt, `kbd_key_place`), klaviatura ochiq bo'lsa uning ustida
  (egasining tanlovi). Faqat ilova ochiq va tinch turganda ko'rinadi.
- Ma'lum: terminal oxirgi qatori va klaviaturaning pastki qatori pill ostida qisman qoladi (iPhone kabi) — egasi ko'rsin.
- O'rnatish: `sudo sh ~/m3/u-lineonly.sh`; qaytarish `/usr/local/bin/ztmetro.bak-lineonly`.

✅ 3-bosqich — chiziq bo'ylab ikki tomonga (branch `line-sides`, 10.11, egasi sinadi: «yaxshi zo'r», main):
- O'ngga = oldingi (eskiroq) ilova, chapga = qaytish (yangirog'i). Tartib iPhone kabi: birinchi surishdagi
  ro'yxat (`line_order`, eng yangisi birinchi) `LINE_ORDER_MS` = 5 s ichida yana surilsa saqlanadi, keyin qayta olinadi.
- Surayotganda qo'shni ilova yonida chiziladi (`line_other`, `line_side`); qo'yib yuborilganda ikkalasi yonma-yon
  siljiydi — qora bo'shliq yo'q. O'sha tomonda ilova bo'lmasa, ilova `LINE_EDGE` = 0.3 qadar ergashadi va qaytadi.
- Tez yon silkitish (16 pt dan ko'p, `vx` > `LINE_FLICK`) ham hisoblanadi. Ilova yopilsa `line_forget` tartibni tozalaydi.
- O'rnatish: `sudo sh ~/m3/u-linesides.sh`; qaytarish `/usr/local/bin/ztmetro.bak-linesides`.

✅ 5b — klaviatura ochiqda chiziq yo'q (branch `line-osk`, 10.11, egasi sinadi: «yaxshi ishladi», main):
- Pill `osk_slide` bilan birga so'nadi (shaderda `fade`), klaviatura to'liq ochiq bo'lsa umuman chizilmaydi.
- `osk_visible` bo'lsa pastki `LINE_ZONE` chiziqniki emas — teginish darhol klaviaturaga. Yopish: ⌨ suzuvchi tugma.
- O'rnatish: `sudo sh ~/m3/u-oskline.sh`; qaytarish `/usr/local/bin/ztmetro.bak-oskline`.

✅ Switcher tezligi, 1-qadam — o'lchash (branch `switcher-perf`, 10.11, o'rnatildi, main):
- `perflog()` endi ishlab turganda ham yoqiladi: `touch /run/user/1000/ztmetro-perflog` (o'chirish: `rm`),
  soniyada bir tekshiriladi. Shunda "animation: N frames … draw/finish/commit … screens" va "slow frame" yozuvlari chiqadi.
- `switcher: N apps (titles drawn in X ms)` — kartochka nomlarini chizish vaqti.
- Switcher qorasi to'liq bo'lganda ostidagi Start plitkalari chizilmaydi (Start'dan ochilganda keraksiz ish edi).
- O'rnatish: `sudo sh ~/m3/u-swperf.sh`; qaytarish `/usr/local/bin/ztmetro.bak-swperf`.
- Keyin: perflog yoqib, egasi switcher'ni 5–6 marta ochib-yopadi, log o'qiladi, qimmat qismi tuzatiladi.

✅ Switcher tezligi, 2-qadam — topildi va tuzatildi (branch `switcher-perf`, 10.11, egasi: «ancha yaxshilandi», main):
- O'lchov (perflog): animatsiya o'zi 60 fps, kadr 6–8 ms (draw ~2 + finish ~4) — GPU emas. Nomlar 1.5–2.5 ms.
- `strace -p` ko'rsatdi: "rested" → "switcher" orasidagi 36–56 ms — `hap()` → `haptics_init()`: vibrator yo'q
  (yadroda hali drayver yo'q), shuning uchun har 3 s da `/dev/input/event*` ni O_RDWR ochib-yopadi; touchscreen
  (event1) va event0 ning `close()` i 15–30 ms dan. Bu har qanday teginishdagi tebranishda bo'lardi (libinput "lagging").
- Tuzatish: qurilma topilmasa `/dev/input` mtime eslab qolinadi, u o'zgarmaguncha qayta qidirilmaydi.
- O'rnatish: `sudo sh ~/m3/u-haprescan.sh`; qaytarish `/usr/local/bin/ztmetro.bak-haprescan`.
- Yadro uchun (kompyuterda): S10 5G vibratori mainline'da yo'q — bor bo'lsa bu muammo ham bo'lmasdi.

✅ 5, 6 va klaviatura ustidan surish (branch `line-rest`, 10.11, o'rnatildi, egasi ishlatdi — logda uchala yo'l bor; main):
- Eslatma: Nullgram ham `request_fullscreen` yuboradi (log 10.11: "asks for the whole screen", ehtimol rasm/video
  ko'ruvchida) — unda ham chiziq 3 s dan keyin so'nadi; egasi bilan tekshirish, kerak bo'lsa `requested.fullscreen=false` ham kuzatiladi.
- **Qidiruv:** Start eng tepada turganda pastga `SEARCH_PULL` = 80 pt tortilsa — Hyperscan (`hyper_open`).
  Faqat chiziq rejimida, plitkalar sahifasida, tahrir paytida emas.
- **Avtomatik yashirish:** ilova `xdg_toplevel.request_fullscreen` so'rasa (`wants_full`; joyi o'zgarmaydi,
  status bar qoladi), `LINE_IDLE_MS` = 3 s tegilmasa pill va ⌨ 600 ms da so'nadi (`line_dim`, `line_idle_timer`);
  har qanday teginish yoki boshqa ilova — qaytadi (`line_wake`). So'nganda pill chizilmaydi (GPU nusxasi ham yo'q).
- **Klaviatura ochiq, pastdan yuqoriga (egasi, 10.11):** pastki `LINE_ZONE` da boshlanib `LINE_KEY_RISE` = 24 pt dan
  ko'p tik ko'tarilgan ROLE_KEY teginishi tugmani yozmaydi, ROLE_NAV ga aylanadi — oddiy chiziq imo-ishorasi
  (Start / switcher / yon). Yetmasa — klaviatura qaytadi (`line_osk_back`).
- O'rnatish: `sudo sh ~/m3/u-linerest.sh`; qaytarish `/usr/local/bin/ztmetro.bak-linerest`.

## (10.11, 20:15) chap chetdagi «orqaga» belgisi — TUGADI, egasi: «yaxshi zo'r chiqdi», main'da. Yangi vazifani egasidan so'ra.

Egasi so'radi: chapdan tortganda Android'dagi animatsiya va ‹ tugmasi. Branch `back-sign`: `draw_back` — chetdan
chiqadigan qora «do'mboq» (bezier), ichida ‹; barmoq balandligida (`back_y`), tortish bilan kattalashadi
(`back_pull` = tortish / SWIPE); 1 dan oshsa ‹ to'liq oq va qalinroq (qo'yib yuborsa — orqaga). Yetmasa 180 ms da
chetga qaytadi; orqaga bo'lsa joyida so'nadi (220 ms). Faqat chiziq rejimida, ilova ochiq va qulf yo'q paytda.
1-2-sinov: egasi «hech narsa chiqmayapti». SABAB (gdb bilan topildi): `zt_buffer_show` oxirida `gl_end` — kadr o'rtasida chizilgan belgi GL kontekstini qo'yib yuborardi, keyingi chizishlar yo'qolardi (o'ng chetdagi ⌨ belgisida ham). Tuzatish: chizgach `gl_begin`, belgi faqat tortish o'zgarsa qayta chiziladi; kulrang rang. O'rnatish `sudo sh ~/m3/u-backsign3.sh` (asl zaxira `ztmetro.bak-backsign`). Sinash: ctl `down/move/up` + gdb `render_frame+off` da qiymatlarni o'qish usuli ishladi.

## (eski) 10.11, 19:40: NAV-LINE 3-band TO'LIQ TUGADI — Nullgram oynalari ham (egasi: «yaxshi ishladi», nullgram-dev main). Yangi vazifani egasidan so'ra.

nullgram-dev branch `sheets-safe` (push qilingan, main'ga HALI EMAS — egasi ko'rgach): AttachSheet (ro'yxat oxiri +
«Yuborish» tugmasi), ForwardSheet (`bottomMargin`), PlayerSheet (balandlik + «saqlash»/pleyer tugmalari), GiftSheet
(oxiri, 2-varaq va «Hadya yuborish» `max(osk.h, safe.bottom)`), PhotoViewer (video paneli gradienti pastgacha,
tugmalar yuqorida). ~/nullgram/qml ga ko'chirildi (zaxira ~/nullgram.bak-sheets/, 5 fayl). Sinash: 📎, xabarni uzatish,
musiqa pleyeri, sovg'a, video ko'rish. Yoqsa: nullgram-dev main'ga merge.

## (eski) 10.11, 19:15: 3-band TUGADI (main) — keyingisi Nullgram oynalari

**3-band HOLATI (branch `apps-safe`):** umumiy kutubxonada (zrnmetrocore) safe area — har C ilova bitta qator bilan oladi:
- `struct zmc_app` ga `.safe_area = true` → ilova ishga tushganda ctl ga `safe area APP_ID` yozadi; `zmc.safe_bottom`
  (`$XDG_RUNTIME_DIR/ztos-safe`, bo'lmasa `ZTOS_SAFE_BOTTOM`; har configure'da qayta o'qiladi). Oynadan uzun sahifa
  `safe_bottom` qadar ko'proq suriladi (oxirgi qator chiziq ostida qolmaydi). `--render` ham shu faylni o'qiydi.
- Shell: `safe area` joylari 4 → `SAFE_AREA_MAX` 16.
- ztphone: `.safe_area = true`; terish sahifasi (qo'ng'iroq/saqlash tugmalari) va qo'ng'iroq ekrani (javob/rad/tugatish)
  `zmc.safe_bottom` qadar yuqorida. Render: ~/m3/render-safe/{keypad,call,history}.png.
- O'RNATILMAGAN: `sudo sh ~/m3/u-appsafe.sh` (ztmetro + ztphone, greetd restart, 3 daq. crash-rollback;
  qaytarish `*.bak-appsafe`). Sinash: Telefon ilovasi fon pastgacha, tugmalar chiziqdan yuqorida; kiruvchi qo'ng'iroq ekrani.
- ✅ ztphone O'RNATILDI (u-appsafe.sh, ok), egasi: «telefon ilovasi yaxshi».
- ztmessages (shu branch): pastki panel (`footer_height` + safe_bottom) rangi chetgacha, tugmalar/maydon chiziqdan
  yuqorida; chiziq ostidagi bosish e'tiborsiz; klaviatura `zmc_osk_box(height - safe)` (maydon klaviatura ustiga yopishadi).
  Render ~/m3/render-safe/m-*.png. O'rnatish: `sudo sh ~/m3/u-msgsafe.sh` (shell restart YO'Q, ilovani qayta ochish;
  qaytarish `ztmessages.bak-msgsafe`). Sinash: suhbatlar ro'yxati, suhbat, yangi xabar, klaviatura ochilganda maydon.
- ztmessages o'rnatildi (u-msgsafe.sh). Egasi (10.11): «ş» so'zlar — yangi o'zbek alifbosi, QOLADI; qolgan ilovalarni
  BITTADA qil (bittalab ko'p vaqt).
- **Qolganlari bittada (shu branch):** `.safe_area = true` — ztcontacts, ztdevices, ztfiles, ztosial, ztpower, ztsettings,
  ztsound (ro'yxatlar: aylantirishni core hal qiladi); ztcamera (kadr chetgacha, tugmalar `foot()` = height - safe).
  Core: menyu (popup) ham chiziqdan yuqorida. Superscan va ztstore o'rnatilmagan — tegilmadi.
  O'rnatish: `sudo sh ~/m3/u-allsafe.sh` (10 ta ilova, shell restart YO'Q; qaytarish `--undo`, zaxira `*.bak-allsafe`).
- ✅ O'RNATILDI (u-allsafe.sh), egasi: «juda ham zo'r va go'zal chiqibdi», main'ga qo'shildi. Keyingisi: Nullgram'ning qolgan oynalari (AttachSheet, GiftSheet, PlayerSheet,
  ForwardSheet, PhotoViewer).
- Eslatma: ztlang-uz.h da ba'zi so'zlar «ş» bilan («teriş», «saqlaş», «qöňğiroq») — egasidan so'rash, xato bo'lsa tuzatish.

## (eski) YANGI SESSIYA (10.11, 18:45): 3-band — o'z ilovalarimiz safe area bilan

1 va 2-band TUGADI, o'rnatilgan (u-oskpull.sh, 18:43), egasi: «rang yoqdi, sozlamalar zo'r», main'da.
Klaviatura: ⌄ yo'q, faqat pastga surib yopiladi; keng space. Sozlamalar → Navigatsiya: so'nish vaqti va chiziq rangi.
Keyingisi — pastdagi 3-band (ztphone birinchi, `--render` + egasining ko'zi, bittalab).

## (eski) YANGI SESSIYA (10.11, 18:20): egasining 3 ta istagi, tartib bilan

**1-band HOLATI (10.11, branch `osk-row`, 769b630):** yozildi, yig'ildi, O'RNATILMAGAN — `sudo sh ~/m3/u-oskrow.sh`
(greetd restart; qaytarish `/usr/local/bin/ztmetro.bak-oskrow`). Harflar: &123 1.1, 🙂 0.8, «,» 0.8, space 4.4, «.» 0.8,
⌄ 0.8, ↵ 1.3 (10 ustun); belgilar: abc 1.1, 🙂 0.8, qo'y 1, «,» 0.7, space 3.6, «.» 0.7, ⌄ 0.8, ↵ 1.3. Render:
~/m3/render-kb/keyboard.png. O'RNATILDI 10.11 18:20, egasi sinayapti.

**2-band HOLATI (10.11, branch `nav-settings` = osk-row + dee3b19):** yozildi, yig'ildi, O'RNATILMAGAN —
`sudo sh ~/m3/u-navset.sh` (ztmetro + ztsettings; greetd restart; qaytarish `*.bak-navset`). Config kalitlari:
`nav_line_hide` = never|3|5|10 (shell: `line_idle_ms`, 0 = so'nmaydi), `nav_line_color` = auto|white|black|accent
(shell: `line_ink`, shader uniform `ink_set`; rang tanlansa ostidagi nusxa olinmaydi). SIGHUP'da `line_wake`.
Render: ~/m3/render-kb/nav.png. Sinash: har tanlovni Sozlamalarda bosib, ilova ustida chiziqqa qarash. Keyin 3-band.

**⌄ ko'chirildi (egasi, 10.11: «klaviatura ustiga, o'chirish tugmasining tepasiga»; shu `nav-settings` da, u-navset.sh bilan
birga o'rnatiladi):** ⌄ endi klaviatura ustki chetida, ⌫ ustunida turgan qora «quloq» (`OSK_TAB_W` 52 × `OSK_TAB_H` 30 pt,
`osk_tab_key`, o'z qatlami `osk_tab_layer`, `osk_tab_hit`). Lock ekranda va trackpad paytida yo'q. Pastki qatorda ⌄ yo'q:
harflar &123 1.2, 🙂 0.9, «,» 0.9, space 4.7, «.» 0.9, ↵ 1.4. Rasm: ~/m3/render-kb/preview-tab.png. Ilovaning o'ng pastki
burchagini 52×30 pt yopadi — egasiga yoqmasa, 2-variant: «a…l» qatori oxirida ⌫ ustida.
**Keyin (egasi, 10.11): rang va sozlamalar «zo'r» (u-navset.sh o'rnatilgan). ⌄ butunlay olib tashlandi** — klaviatura
faqat pastga surib yopiladi (iOS kabi). Quloqcha kodi `OSK_TAB 0` ortida qoldi (1 = qaytadi). O'RNATILMAGAN:
`sudo sh ~/m3/u-oskpull.sh`. Keyin: main'ga (osk-row + nav-settings), so'ng 3-band.

Holat: tasma, safe area (Nullgram) va klaviaturani yopish (⌄ + pastga tortish) — o'rnatilgan, egasi: «yaxshi ishladi», main
(30fceef). Nullgram `nullgram-dev` main = ccd93f8 (safe area, egasi «ha», telefonda o'rnatilgan).
**Egasining qoidasi: bundan keyin har qanday UI o'zgarishi chiziqqa (nav line) qarab qilinadi** — safe area, chiziq ostida tugma qolmasin.

1. **Klaviatura pastki qatori (birinchi, kichik ish):** egasi space o'rniga «.» ni bosib yuboryapti (ko'p nuqta) —
   space tor. &123 va 🙂 ni kichikroq, space'ni kengroq, o'ng tomonni (. ⌄ ↵) biroz suring. `osk_layout` dagi
   `bottom_letters` og'irliklari (hozir 1,1,1,3,1,1,1 = 9 ustun). Taklif (10 ustun): &123 1.1, 🙂 0.8, «,» 0.8,
   space 4.4, «.» 0.8, ⌄ 0.8, ↵ 1.3 — `osk_grid_row` kasr og'irliklarni qabul qiladi, `--render` bilan ko'rib, egasiga ko'rsatish.
   ⌄ ni boshqa joyga ko'chirish ham mumkin (masalan 3-qatorda backspace yonida emas — egasi «yoki boshqa joyga» dedi): so'rash.
2. **Sozlamalar → Navigatsiya bo'limi** (`settings/ztsettings.c`, `ACT_NAV_STYLE` picker ~3263, config o'qish ~450, yozish ~695;
   shell `load_config` ~3428 `nav_style`):
   - chiziq necha soniyadan keyin so'nadi: Doim yoniq / 3 / 5 / 10 s (yangi kalit, masalan `nav_line_hide`; hozir qattiq
     `LINE_IDLE_MS` 3000, ztmetro.c ~7980/7988 — o'zgaruvchiga aylantirish, 0 = so'nmaydi);
   - chiziq rangi: Avto (fonga qarab, hozirgisi) / Oq / Qora / Aksent rangi (`glass_draw` shaderi, yorqinlik tanlovi).
   SIGHUP bilan qo'llanadi (nav_style kabi). `ztsettings --render` bilan sahifani ko'rish.
3. **O'z ilovalarimiz Nullgram kabi** (safe area protokoli: ctl ga `safe area APP_ID`, pastdan `ZTOS_SAFE_BOTTOM` /
   `$XDG_RUNTIME_DIR/ztos-safe`): ztphone (phone/), ztmessages (messages/), ztcontacts (contacts/), ztsettings, keyin
   ztfiles, ztsound, ztpower, ztdevices. Har birida: fon pastgacha, pastki tugmalar/panel `bottom` qadar yuqorida;
   bittalab, har biridan keyin egasi ko'radi. Qo'ng'iroq ekrani (ztphone --call-screen) alohida tekshirilsin.
   Keyin: Nullgram'ning qolgan oynalari (AttachSheet, GiftSheet, PlayerSheet, ForwardSheet, PhotoViewer).

## (eski) YANGI SESSIYA (10.11, 17:00): CHIZIQ UCHUN JOY — egasi tasdiqlagan reja, tartib bilan

Holat: 0, A, B, C, prujina (0,5 s), 3 s so'nish, tezlik bo'yicha switcher — hammasi o'rnatilgan, egasi: «endi yaxshi».
Upstream taklifi `upstream-navline-draft.md` da, egasi «qoldirib tur» dedi.

**Muammo:** iOS'da ilova fon bilan pastgacha boradi, lekin tizim unga ~34 pt «safe area» beradi va tugmalar
yuqoriroqda turadi. Wayland'da bunday standart yo'q, begona ilovalar (foot, chromium) pastki qatorni chiziq ostiga
qo'yadi. Egasi «UI chiroyliroq bo'lsin» deydi.

**1-qadam HOLATI (10.11 kech, branch `line-strip`, cf3b30d):** yozildi va yig'ildi, O'RNATILMAGAN — egasi
`sudo sh ~/m3/u-linestrip.sh` ishga tushiradi (greetd restart; qaytarish `/usr/local/bin/ztmetro.bak-linestrip`).
Qilingani: `LINE_SAFE` = 18, `app_room()` (initial size, `app_configure`, popup qutisi; klaviatura terminalga joy
bersa tasma yo'q), `app_strip()`/`app_draw_all()` — ilova asosiy surface'ining oxirgi qatori (uv_scale.y = 0)
ilova bilan birga cho'zib chiziladi: kartochka, surish, yon ilova, qo'ng'iroq ekrani ham. Chiziq ochiq fonda 0.16
kulrang. Keyin: egasi foot / chromium / Telefon'da ko'radi; yoqsa main'ga, so'ng 2-qadam.

**1-qadam ✅ (10.11, 17:20):** o'rnatildi, egasi: «ko'plab ilovalarda juda zo'r», faqat Nullgram'da g'alati
(ostida ro'yxat qatori cho'zilib chiziladi) — u 2-qadamda. main'ga qo'shildi (907e1c5).

**2-qadam HOLATI (10.11, branch `safe-area`):** O'RNATILDI 10.11 (Nullgram ochiladi), egasi ko'rishi kerak —
`sudo sh ~/m3/u-safearea.sh` (greetd restart; qaytarish `/usr/local/bin/ztmetro.bak-safearea`).
- Protokol (`osk room` kabi, ilova o'zi aytadi): ilova `ztmetro.ctl` ga `safe area APP_ID` yozadi → shell unga
  `LINE_SAFE` siz to'liq balandlik beradi (tasma yo'q). Qancha joy qoldirish: `$XDG_RUNTIME_DIR/ztos-safe`
  («bottom 18», chiziq bo'lmasa 0; har config yuklanganda qayta yoziladi) va launch'da env `ZTOS_SAFE_BOTTOM`.
  Eski shell bilan ilova o'zgarmaydi (fayl yo'q → 0).
- Nullgram (repo github urinbayevoff/nullgram-dev → `~/projects/nullgram-dev`, branch `safe-area` = main + safe area; telefonda o.rnatilgani `ae98262` + safe area; zaxira `~/nullgram.bak-safe`, diff
  `nullgram-safe-area.patch`; ChatPage da `lift` band — bizniki `barLift`): `Safe` obyekti (`safe.bottom`);
  tab bar chiziqdan `bottom + 4` yuqorida; chatda maydon/kanal/tanlash/qidiruv panellari `lift = max(kb, bottom)`
  ga ko'tariladi, `foot` ga `bottom` qo'shildi; qidiruv paneli foni pastgacha.
- Keyin: egasi Nullgram'ni ko'radi (chatlar ro'yxati, chat, kanal, qidiruv); so'ng varaqlar (AttachSheet, GiftSheet,
  PlayerSheet, ForwardSheet, PhotoViewer), keyin Telefon/Xabarlar/Sozlamalar (C ilovalar: env o'qib, ctl ga yozadi).

**Klaviaturani yopish (10.11 kech, branch `osk-close`, egasi so'radi: o'ng chetda 2 s ushlab yopish jonga tegdi):**
yozildi, yig'ildi, O'RNATILMAGAN — `sudo sh ~/m3/u-oskclose.sh` (qaytarish `/usr/local/bin/ztmetro.bak-oskclose`).
- Pastki qatorda «⌄» tugma (`ACT_HIDE`, ikon `kbd-down`; harflarda space 4→3, belgilarda 3→2, emoji'da ham).
- Klaviatura ustida pastga tortish (`ROLE_OSKPULL`): tugma `OSK_PULL` = 28 pt tik pastga siljisa — klaviatura barmoq
  bilan tushadi; 1/3 dan ko'p yoki `OSK_PULL_FLICK` 0.5 pt/ms dan tez — yopiladi, yo'qsa qaytadi.
- O'ng chetdagi 2 s ushlash endi faqat klaviatura YOPIQ bo'lsa (ochish uchun; maydoni yo'q ilovalarda boshqa yo'l yo'q).
- Ma'lum: tortish paytida terminal hali kichraygan holda (ostida fon ko'rinadi), qo'yib yuborilganda o'z joyiga keladi.

**1-qadam — barcha ilovalar uchun «xameleon» tasma (ztMETRO'ning o'zida):**
- Ilovaga pastdan ~18 pt joy (yangi `LINE_SAFE`; `app_height()` ztmetro.c:~7598 va `NAV_H` ishlatilgan joylar:
  `server->height - STATUS_H - NAV_H` ~10 ta joy — bitta yordamchiga (masalan `bottom_room(server)`) yig'ish).
  `nav_h = 0` qoladi (chiziq rejimi belgisi `NAV_LINE` unga bog'liq!), yangi qiymat alohida bo'lsin.
- Tasma rangi = ilovaning eng pastki 1–2 qatori (glass_draw allaqachon pastdan glCopyTexSubImage qiladi;
  tasmani ilova kadrining oxirgi qatorini cho'zib chizish yoki shader'da `tex` ning yuqori qatoridan olish).
  Ilova rangi o'zgarsa birga o'zgaradi — chegara ko'rinmasin.
- Chiziq rangi fonga qarab: ochiq fonda to'q kulrang, to'q fonda oq (yorqinlik ostidagi pikseldan; glass shader).
- Chiziq 3 s da so'nganda tasma qoladi (ilova o'lchami o'zgarmaydi — sakrash xunuk).
- Start, switcher, Hyperscan, lock: tasma yo'q. Klaviatura ochiq: tasma klaviatura ostida yo'q (klaviatura pastgacha).
- Sinash: foot (oxirgi qator ko'rinadimi), chromium, Telefon ilovasi tugmalari; `--render` + egasining ko'zi.
- Energiya: qo'shimcha nusxa yo'q, o'sha glass bo'lagi.

**2-qadam — o'zimizning ilovalar iOS usulida (tasmasiz, fon pastgacha):**
- ztMETRO ilovaga «safe area» ni aytadi: env `ZTOS_SAFE_BOTTOM=18` (ilova ishga tushganda, `setenv` ztmetro.c:~8176
  yonida) va shu ilovalar ro'yxati (app_id) uchun tasma berilmaydi — butun balandlik.
- Ilovalar (Telefon, Xabarlar, Kontaktlar, Sozlamalar, Nullgram, SDK) pastki tugmalarini shu qadar ko'taradi,
  foni pastgacha. Bittalab, har biridan keyin ko'rish.
- Keyin: STATUS.md qatori va upstream qoralamasiga qo'shish.

## >>> YANGI SESSIYA SHU YERDAN (10.11, kechqurun): 1–6 bajarildi. Egasining yangi 3 ta istagi (tartib bilan):

**10.11 kech, 6: egasi: «tepaga surib ushlab tursam switcher kech ochiladi».** Sabab: soat har 3 pt qimirlashda
qayta boshlanardi (qo'l hech qachon qotmaydi). Endi iPhone kabi tezlikka qarab: 70 pt dan baland va silliqlangan
tezlik < 0,15 pt/ms (LINE_REST_V) bo'lsa soat qayta boshlanmaydi, 110 ms (LINE_PAUSE_MS, avval 220) dan keyin
switcher. HALI O'RNATILMAGAN. Sinash: ochilish tezligi, sekin surib Start'ga chiqish hali ishlaydimi.

**10.11 kech, 5: switcher o'lchandi (perf log, egasi 20+ marta ochdi-yopdi): barcha animatsiyalar 60 fps
(16,8–17 ms/kadr, chizish ~1 ms, finish ~4 ms), nomlar 1,3–3 ms — switcher o'zi og'ir emas. Kechikkan kadrlar
(26–41 ms) faqat terminalda chiziq klaviatura ustidan boshlanganda: keraksiz draw_osk (yopilayotgan klaviaturani
qayta chizish) va osk_show'dagi draw_nav olib tashlandi. ⌨ tugmasidan qolgan kod tozalandi, fayl boshidagi
tavsif yangilandi; STATUS.md ga qator. HALI O'RNATILMAGAN (kichik o'zgarish). Keyingisi: upstream'ga taklif matni
(egasi «yuboramiz» dedi; faqat matn, fayl/raqam/repo havolasisiz). Qoralama: docs/s10-5g/upstream-navline-draft.md — egasi: «qoldirib tur», hali yuborilmagan.

**10.11 kech, 4 (O'RNATILDI, egasi: «yaxshi zo'r ishladi»): egasi aniqladi — «Nullgram'dagidek» = ilova ochilganda chiziq ko'rinmay qolsin.** Nullgram
to'liq ekran so'raydi (wants_full) va chiziq 3 s tegilmasa so'nardi; endi bu BARCHA ilovalarda (LINE_IDLE_MS,
line_wake/line_idle_fired dan wants_full sharti olindi). Teginsa chiziq qaytadi, 3 s dan keyin yana so'nadi;
so'ngan chiziq chizilmaydi (glass nusxasi yo'q — kam energiya). Imo-ishoralar ko'rinmas chiziqda ham ishlaydi.

**10.11 kech, 3: B/C o'rnatildi. Egasi: switcherga o'tishda chiziq yo'qolib, switcher ochilganda qaytib paydo
bo'lib yo'qoladi, hammasi juda tez; «Nullgram'dagidek qilaylik».** Qilingani (HALI O'RNATILMAGAN):
chiziq harakatlari (line_t, line_dx, switch_t, switch_x, pill, line_vis kirishi) endi Nullgram sahifalaridagi iOS 26
prujinasida, 0,5 s (`animate_ios`, `SPRING_MS`; zt_anim.ios: 1-(1+x)e^-x, kritik so'nish, sakrashsiz; Metro
plitkalari o'z egri chiziqlarida qoladi). Chiziq dam olib switcher ochilganda `line_vis` darhol 0 (miltillamaydi).
pill_home sakrashsiz. Sinash: switcher, Start'ga surish, yon tomonga — tezlik ma'qulmi.

**10.11 kech, 2: 0 va A O'RNATILDI, egasi sinadi — ishladi. B va C kodda bajarildi, yig'ildi, HALI O'RNATILMAGAN**
(`sudo sh ~/m3/u-navline.sh`). iOS bo'yicha topilgani: Notification Center pastdan yuqoriga surib yopiladi
(o'sha ekranda qolasiz); uy ekranida home indicator yo'q; imo-ishorada indikator barmoqqa ergashadi, qo'yilganda
prujina bilan qaytadi (WWDC23 «Animate with springs»: tezlik saqlanadi). Qilingani:
B — panel tushgan sari chiziq so'nadi; panel ochiq paytda pastki zona panelning o'z tutqichi, yuqoriga surish panelni yopadi.
C — chiziq barmoq bilan yon tomonga (±72 pt, tanh bilan sekinlashib) va yuqoriga (18 pt gacha) yuradi, ilova
kartochkaga aylangan sari so'nadi, qo'yilganda biroz o'tib prujina bilan qaytadi (`pill_home`); Start'da,
switcher'da va Hyperscan'da chiziq yo'q, ilova ochilganda 120 ms dan keyin so'nib paydo bo'ladi (`line_vis`).
Sinash: panelni tushirish → chiziq yo'q, pastdan surish → panel yopiladi; chiziqni yonga/yuqoriga sudrash.
Qolgani: switcher «og'irligi»ni o'lchash (pastda), 7 (egasidan so'rash).

**10.11 kech (telefon sessiyasi): 0 va A kodda bajarildi va yig'ildi (~/m3/ztmetro/ztmetro), HALI O'RNATILMAGAN** —
o'rnatish: `sudo sh ~/m3/u-navline.sh` (greetd qayta ishga tushadi). O'rnatgach sinash:
(0) Start → pastga tortib Hyperscan → chiziqni yuqoriga surish = Hyperscan yopiladi;
(A) ilova ustida o'ng chetdan ichkariga ~20 pt surib, 2 s ushlash = klaviatura ochiladi/yopiladi (barmoq ustida
halqa to'lib boradigan ⌨ belgisi). ⌨ suzuvchi tugma olib tashlandi. Eslatma: o'ng chetdagi 18 pt endi ilovaga
bormaydi (chap chet kabi). Keyingisi: B.

0. **XATO (egasi, 10.11): Start'dan tortib ochilgan Hyperscan'dan chiziq bilan chiqib bo'lmaydi.** Log: hyper ochilganda
   klaviatura ham ochiladi ("keyboard shown" → "hyperscan: open"), pastki zona klaviaturaniki; ROLE_KEY → ROLE_NAV
   o'tishi esa `server->shown && !server->hyper` shartida — Start'da (shown = NULL) va hyper ochiqda ishlamaydi.
   Tuzatish: shu shartdan `server->shown` va `!server->hyper` ni olib tashlash (`line_follow` o'zi shown/hyper'da
   ergashmaydi; `nav_line_end` → `nav_activate(NAV_START)` hyper'ni yopadi). Yo'lda to'xtash (switcher) hyper ustida
   ochilmasin — `long_press_fired` da `!server->hyper` sharti. Birinchi shuni qilish, kichik.
A. **⌨ suzuvchi tugmani butunlay olib tashlash** — ba'zan ilovadagi narsalarni yopib qo'yadi. O'rniga:
   **o'ng chetdan (ekran ichiga) surib, ~2 soniya ushlab turish** = klaviaturani ochish/yopish (egasining g'oyasi,
   «yaxshi fikr»). Chap chet = orqaga, unga tegmaslik. Ushlab turishda kichik ko'rsatkich/tebranish (vibrator yo'q —
   vizual belgi kerak). `KBD_BTN`, `kbd_key_shown`, `kbd_key_place`, `draw_nav` dagi tugma, touch_begin'dagi ⌨ shoxi.
B. **Bildirishnoma paneli (shade/Action Center) tushirilganda chiziq ko'rinib turibdi — yashirilsin.**
   Avval iOS'da qanday ishlashini manbalardan aniqlash (Notification Center / Control Center ochiqda home indicator
   bormi, yuqoriga surish panelni yopadimi), keyin shunga moslash (`glass_draw` da `shade_h` ga qarab fade,
   pastki zona shade ochiqda nima qiladi).
C. **Chiziq animatsiyalari — iOS kabi.** Manbalardan izlash: surayotganda home indicator barmoq bilan
   ko'tariladimi/siljiydimi, qo'yib yuborilganda qanday qaytadi, ilova ochilganda/yopilganda paydo bo'lishi va h.k.
   Topilganini qisqa yozib, keyin qilish.
Undan keyin: 7 (STATUS.md, upstream taklifi — egasidan so'rash).

**Egasi, 10.11:** switcher kartochkasini yuqoriga otib yopish ishlaydi («yoqdi»), lekin switcher hali «og'irroq».
Logdagi izlar (yangi sessiyada o'lchash, 3-banddan oldin yoki birga):
- `switcher_open` 25–30 ms oladi (log: "rested on the way up" → "switcher: N apps") — har ochilishda kartochka
  nomlari cairo bilan qayta chiziladi; oldindan/keshlab chizish mumkin.
- `output: only the scanout buffer is free; the frame waits` (14354) va libinput "lagging behind by 24ms" — kadr kechikadi.
- Har kadr barcha kartochkalar to'liq o'lchamli teksturadan chiziladi + pill uchun `glCopyTexSubImage2D`;
  kadr vaqtini o'lchab, qaysi biri qimmatligini aniqlash. G3D/MIF chastotasi switcher paytida pastmi — tekshirish.

## Keyingi qadamlar (tartib bilan)

1. **Silliqlik (egasining birinchi istagi).** Hozir harakat faqat barmoq qo'yilganda bajariladi, shuning uchun keskin.
   - Surayotganda ilova barmoq ortidan kichrayib, yuqoriga ko'tarilsin (`app_lift`/`app_draw.scale` kabi qiymatlar
     bilan, har `touch_update` da), qo'yib yuborilganda prujina animatsiyasi bilan Start'ga yoki joyiga qaytsin.
   - Tezlikni hisobga olish: qisqa, lekin tez silkitish ham Start (`p->vy`).
   - Switcher'ga o'tish uzluksiz bo'lsin: kichrayib borayotgan ilova kartochkaga aylanadi (hozir to'satdan ochiladi).
   - Chiziq bo'ylab o'tishda ilovalar yonma-yon siljisin (hozir birdan almashadi).
   - Logdagi har kadr vaqtini (`frame_quads`/animatsiya xulosasi) o'lchab, shisha shaderining narxini tekshirish.
2. **20 pt tasmani olib tashlash — faqat chiziq qolsin (egasining istagi).**
   - `nav_h = 0` (ilova butun pastgacha), chiziq ilova ustiga chiziladi; rang ostidagi pikselga qarab (glass_draw'ni
     chiziq atrofidagi kichik bo'lakka qisqartirish).
   - Muammo: pastki ~16 pt dagi teginishlar ilovaga ham kerak. Yechim: pastdan boshlangan teginishni bir necha ms/pt
     ushlab turish; yuqoriga ketsa — imo-ishora, aks holda ilovaga o'tgan vaqti bilan qayta yuborish
     (terminal tutqichlaridagi `selhandle_grab` usuli).
   - ⌨ tugmasi uchun yangi joy (masalan, chiziqni bosib turish yoki kichik suzuvchi tugma).
   - Terminal oxirgi qatori va Telefon ilovasi tugmalari chiziq ostida qolmasligini tekshirish.
3. **Chiziq bo'ylab ikki tomonga:** o'ngga = oldingi ilova, chapga = qaytish (hozir ikkalasi ham "keyingi").
   Ilovalar tartibi ro'yxati kerak.
4. **Switcher'da kartochkani yuqoriga otib yopish** (iPhone kabi), mavjud switcher o'qlarini tekshirish.
5. **Chiziq rejimida qidiruv (Hyperscan):** Start ekranda o'rtadan pastga surish.
5b. **Klaviatura ochiq paytda chiziq yo'qolsin (egasi, 10.11):** `osk_visible` bo'lsa pill chizilmaydi va pastki
   `LINE_ZONE` imo-ishora emas — teginish to'g'ridan-to'g'ri klaviaturaga (pastki qator to'liq ishlaydi, kechikishsiz).
   Klaviatura ⌨ suzuvchi tugma bilan yopiladi, yopilgach chiziq so'nib qaytadi (`osk_slide` bilan birga).
6. **Avtomatik yashirish:** to'liq ekranli video/o'yinda chiziq bir necha soniyadan keyin xiralashadi;
   shu holatda display plane qaytadan yoqilishi mumkin (batareya).
7. Tayyor bo'lgach: STATUS.md ga qator, upstream'ga taklif qilish yoki yo'qligini egasidan so'rash.
