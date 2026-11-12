# Shriftlar: Segoe UI Symbol va boshqa Microsoft shriftlari

## Muammo (2026-11-12)

Tizim ilovalarining belgilari (Telefon, Xabarlar, Kontaktlar, Kamera, Musiqa, Fayllar, Do'kon, Ovoz,
Qurilmalar, Quvvat) **Segoe UI Symbol** (`seguisym.ttf`) shriftidan chiziladi
(`ztmetro/apps/ztapps.c` `system_glyph`, Start plitkalari `ztmetro.c` Pango orqali, fallback o'chiq).

Telefonda shrift o'rnatilmagan edi (`os/overlay/usr/local/share/fonts/segoe/` hech qachon
`/usr/local/share/fonts/` ga tushmagan). fontconfig "Segoe UI Symbol" o'rniga Noto Sans berdi, unda
bu belgilar (U+E1xx) yo'q, va 10 ta ilovaning hammasi bir xil "belgi topilmadi" to'rtburchagi bilan
chizilib, `~/.cache/ztmetro/icons/segoe-v1-*.a8` keshida qolib ketdi (kesh hech qachon yangilanmaydi).

Telefonda tuzatildi: shrift `/usr/local/share/fonts/segoe/seguisym.ttf` ga o'rnatildi, eski kesh
`~/.cache/ztmetro/icons-tofu-bak-1112/` ga olindi, belgilar qayta chizildi.

## Litsenziya

Segoe UI, Segoe UI Symbol, Segoe MDL2 Assets — **Microsoft'ning proprietary shriftlari**, ularni
qayta tarqatish ruxsat etilmagan. Upstream ztOS ochiq repo'sida ular bor
(`os/overlay/usr/local/share/fonts/segoe/*.ttf`, `apps/music/SegMDL2.ttf`) — bu upstream qarori.
`apple/` shriftlari (Apple Color Emoji va boshq.) ham proprietary.
Selawik (`os/fonts/selawik/`) — Microsoft'ning SIL OFL ochiq shrifti, tarqatsa bo'ladi.

## Yechim: foydalanuvchi o'z nusxasini o'rnatadi

1. **Zaxira yo'l** (`ztapps.c` `symbol_font()`): fontconfig "Segoe UI Symbol"ni o'zini topmasa,
   tizim ilovalari to'rtburchak o'rniga o'z mavzu ikonkalarini (`Icon=ztphone` va h.k.) oladi.
   Har chaqiruvda `FcInitBringUptoDate()` — o'rnatilgandan keyin darrov ko'radi.
2. **Sozlamalar → mavzu → Belgilar şrifti**: shrift bor-yo'qligini ko'rsatadi; yo'q bo'lsa
   «şriftni örnatiş» tugmasi `seguisym.ttf` ni (har qanday harf kattaligida) qidiradi:
   `~/Downloads` (2 daraja chuqur), uy papkasi, `/run/media/$USER/*` (fleşka). Topsa:
   `~/.local/share/fonts/omonullos/seguisym.ttf` ga ko'chiradi (parolsiz), `fc-cache`,
   `segoe-v1-*.a8` keshini o'chiradi, `ztapps --once`. Start plitkalari telefon qayta yoqilganda
   yangilanadi.

Foydalanuvchi uchun qisqa yo'riqnoma: Windows kompyuterda `C:\Windows\Fonts` dan `seguisym.ttf`
faylini telefonning Yuklanmalar (Downloads) papkasiga ko'chiring → Sozlamalar → mavzu →
«fayldan örnatiş» → telefonni qayta yoqing.

## Eng oson yo'l: ztOS'dan yuklab olish (2026-11-12)

`os/overlay/usr/local/bin/ztos-fonts` 7 ta Segoe shriftini (~7 MB) upstream ztOS'ning ochiq repo'sidan
**qotirilgan commit** (`714765708075`) bo'yicha yuklaydi, har birini sha256 bilan tekshiradi va
`~/.local/share/fonts/ztos-segoe/` ga o'rnatadi (parolsiz), keyin belgilarni qayta chizdiradi.
Oxirgi qatori `ok` yoki `error: ...`. Sozlamalar → mavzu → «ztOSʼdan yuklab oliş» uni fonda ishga
tushiradi; «fayldan örnatiş» — internetsiz yo'l. Biz shriftni tarqatmaymiz: repo va image toza.
Telefonda sinaldi: Redmi hotspot orqali 78 s, hammasi tekshiruvdan o'tdi.

## Apple Color Emoji (2026-11-12)

Apple'ning rasmlari, litsenziyasi faqat Apple qurilmalariga: omonullOS bilan **kelmaydi** (standart —
Google Noto Color Emoji, ochiq). Private repo'dagi `os/fonts/apple-emoji/` (installer har rootfs'ga
qo'yadi) ochiq repo/image'ga **kirmaydi** — eksportda chiqarib tashlanadi, installer ham o'tkazib yuboradi.

`os/overlay/usr/local/bin/ztos-emoji` uni samuelngs/apple-emoji-ttf release'idan
(`macos-26-20260722-484daf4e`, `AppleColorEmoji-Linux.ttf`, 116 MB) yuklab, sha256 (`e37c7af6…`) bilan
tekshirib, `~/.local/share/fonts/apple-emoji/` ga o'rnatadi. Sozlamalar → mavzu → Emoji → Apple,
shrift yo'q bo'lsa «yuklab oliş (116 MB)» tugmasi (Wi-Fi ogohlantirishi bilan). Sinov: mahalliy fayldan
(file://) — o'rnatildi; buzuq fayl — `error: wrong file`, qoldiq o'chirildi. Release havolasi 200 qaytaradi.

## ztOS fon rasmlari va qo'ng'iroq ohanglari (2026-11-12)

Upstream `prebuilts/ztos-media` (23 bo'lak, 184 MB): Nokia/Lumia fon rasmlari, Windows Phone ohanglari va
klaviatura tovushlari — Microsoft/Nokia'niki. Ochiq repo/image'ga **kirmaydi** (eksportda chiqariladi;
installer media'siz ham ishlaydi, faqat ogohlantiradi). Egasi 11-12: emoji kabi yuklab olinadigan bo'lsin.

- `os/overlay/usr/local/libexec/ztos-media-fetch` (root, `ztos-media.service` dan): ztOS commit
  `b3bff4734727` dagi 23 bo'lakni yuklaydi, har birini sha256 bilan tekshiradi, faqat
  `usr/local/share/backgrounds` va `usr/local/share/sounds`ni `--skip-old-files` bilan ochadi (bizning fonlar
  va `omonullos-ring.wav` ustiga yozilmaydi), `backgrounds/ztos/.ztos-media` belgisini qo'yadi. Holat:
  `/run/ztos-media.status` (`busy`/`ok`/`error: ...`). 450 MB bo'sh joy talab qiladi.
- `ztos-media` (foydalanuvchi): `systemctl start ztos-media.service`, natijani chiqaradi.
- Polkit: alohida `os/overlay/etc/polkit-1/rules.d/50-ztos-media.rules` (faqat shu unit, faqat start, wheel, parolsiz).
- Sozlamalar → mavzu → fon rasmi ostida «yuklab oliş (184 MB)» (belgi yo'q bo'lsa).
- 11-12 18:09 telefonda sinov (egasi «ha»): Sozlamalar yo'li (`ztos-media` → polkit parolsiz → servis) 5 daqiqada
  `ok`, 21 fon papkasi, belgi qo'yildi, bizning fayllar o'zgarmadi. Topildi va tuzatildi: fon tanlagich 16 papka /
  400 rasm bilan cheklangan edi (Gullar, Lumia 925... yo'qolardi) → 40/1000; uzun nom soniga chiqib ketardi → «…».
- Qo'ng'iroq ohanglari (da0894f, 11-12 18:20 o'rnatildi): kiruvchi qo'ng'iroqni faqat `zcell.py` chaladi —
  Sozlamalarda tanlangan nom (`~/.config/ztmetro/ringtone`), avval `sounds/ztos/phone/`, keyin `sounds/ztos/ring/`,
  bo'lmasa `omonullos-ring.wav` (`zt_ringtone_file()` zrnvibe.h bilan bir qoida). ztphone beyondx'da o'z ohangini,
  qo'ng'iroq oxiri ovozlarini va suhbatdagi tugma ohangini chalmaydi. Ro'yxatda birinchi «omonullOS».
  **Sinov 11-12:** 18:25 qo'ng'iroq — faqat `[zcell] omonullos-ring.wav`; 18:27 Nokia-Tune tanlangach — faqat
  `[zcell] Nokia-Tune.wav`, egasi: «ishladi, alohida chalindi». Ikkalasida qo'ng'iroq oxirida ovoz yo'q.

## Ochiq qolgan

- Start plitkalari (`ztmetro.c`, Pango fallback o'chiq) shrift bo'lmasa hali ham to'rtburchak
  chizadi — ularga ham shu zaxira yo'l kerak (ztmetro qayta ishga tushishi kerak, keyingi UI
  to'plamida).
- Ochiq repo/image (`omonullos-beyondx`): Microsoft shriftlari qo'shilmaydi (egasi 11-12: ztOS'dan
  yuklab olish). Birinchi yoqishda Wi-Fi ulanganda o'zi yuklashi — keyinroq.
