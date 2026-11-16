# Karnay himoyasi (Cirrus spk-prot) — Samsung balandligiga chiqish

2026-11-14, telefondagi sessiya. **Faqat o'qildi**, hech narsa yozilmagan. Egasi: «bugungi test tugagandan
keyin B dan boshlaymiz» (B = Samsung'ning himoya firmware'ini amp'larga yuklash).

## Nega past

| | Biz (14.11) | Stock (`/mnt/vendor/etc/mixer_gains.xml`, `mixer_paths.xml`) |
|---|---|---|
| Raqamli ovoz (AMP_DIG_VOL) | VOL 624 = **-24 dB** (drayver chegarasi -6 dB, `AMP_VOL_MAX`) | «Digital PCM Volume» 817 = **0 dB** (0x4CF+817 = 0x800 → 0) |
| AMP PCM Gain | 20 | **18** (tuning fayllari ham «18dB») |
| Signal yo'li | `DAC_PCM1_SRC = ASPRX1` (DSP'siz) | `PCM Source = DSP`, `DSP1 Firmware = Protection`, Preload 1, Boost Enabled |
| Media karnaylari | faqat pastki (`amp_mask=1`) | `gain-media-speaker` (Right=pastki); `dual-speaker` yo'li ham bor (stereo) |

Cho'qqida taxminan **22 dB** farq. Stock 0 dB'da tura oladi, chunki amp ichidagi HALO DSP'da CSPL (Cirrus
Speaker Protection) ishlaydi: karnay qarshiligi/haroratini VMON/IMON orqali kuzatib, kerak bo'lsa kuchni kesadi.
Bizda u yo'q — shuning uchun -6 dB chegara va hozirgi -24 dB.

## Stock fayllar (VENDOR, faqat o'qiladi)

`/mnt/vendor/firmware/` (bot = pastki karnay 0x40, rcv = quloq dinamigi 0x41):

| fayl | hajm | sha256 (boshi) | ichida |
|---|---|---|---|
| cs35l40-{bot,rcv}-dsp1-spk-prot.wmfw | 28380 | 0b04dd0a… (ikkalasi bir xil) | HALO (core 4), wmfw ver 3; alg 0x400a4 FIRMWARE_HALO_CSPL, alg **0xcd CSPL** (29 control) |
| cs35l40-bot-dsp1-spk-prot.bin | 4224 | 00b22633… | alg 0xcd koeffitsiyentlari, «BYX_18dB_SPKPV2_PPP_20190131» |
| cs35l40-rcv-dsp1-spk-prot.bin | 4224 | 3bff9e34… | «BYX_18dB_RCVPV1_PPP_20190131» |
| cs35l40-{bot,rcv}-dsp1-spk-prot-calib.bin | 1076 | 252b1de9… / d48a9107… | kalibrovka o'lchovi uchun koeffitsiyentlar («…_calib.bin») |

Tahlil: `audio/beyondx/cirrus-fw-info.py FAYL...`. Muhim CSPL control'lari (XM, alg 0xcd): CSPL_ENABLE,
CSPL_COMMAND, CSPL_STATE (0 = running, 1 = error), CSPL_TEMPERATURE, **CAL_R, CAL_AMBIENT, CAL_STATUS,
CAL_CHECKSUM, CAL_SET_STATUS**, BDLOG_MAX_TEMP / MAX_EXC / OVER_TEMP_COUNT / OVER_EXC_COUNT / ABNORMAL_MUTE
(kuzatish uchun juda qulay), REDUCE_POWER, CSPL_SPK_OUTPUT_POWER. HALO_HEARTBEAT — DSP tirikligi.

## Stock qanday qiladi (vk9820: `sound/soc/codecs/cs35l41.c`, `drivers/mfd/cirrus-cal.c`)

1. «DSP1 Preloader» PRE/POST_PMU: `wm_halo_early_event` + `wm_halo_event` (wmfw + `.bin` yuklanadi, bir marta,
   `halo_booted`), so'ng `cirrus_cal_apply_{left,right}()`.
2. Kalibrovka: `/efs/cirrus/rdc_cal` (chap), `rdc_cal_r` (o'ng), `temp_cal` dan o'qiladi; yo'q bo'lsa
   **standart RDC 8580, ambient 23**. Yoziladi: CAL_RDC 0x02800224, CAL_AMBIENT …228, CAL_STATUS …22C = 1,
   CAL_CHECKSUM …230 = status + rdc (bu manzillar shu firmware'ning XM'idagi CAL_* control'lari).
   Zavod kalibrovkasi `-calib.bin` bilan jim ijroda o'lchanadi va EFS'ga yoziladi — **biz EFS'ga hech qachon
   yozmaymiz**; o'zimiz o'lchasak natija `/var/lib/...` ga.
3. Amp yoqilishi (`cs35l41_main_amp_event` POST_PMU): pup patch → GLOBAL_EN=1 → DSP yo'naltirilgan bo'lsa
   `CSPL_MBOX_CMD_RESUME`, aks holda PAUSE + `DAC_PCM1_SRC = ASPRX1`. O'chishda: mbox PAUSE → GLOBAL_EN=0 → pdn patch.
   `cirrus_pwr_start/stop` — qo'shimcha quvvat/harorat kuzatuvchi (keyinroq, shart emas).
4. Stock `dev-speaker` (o'ng = pastki): ASPTX1 = DSPTX1, ASPTX2 = ASPRX1, `PCM Source = DSP`, Boost Enabled.

## Mainline'da tayyor namuna

- `sound/hda/codecs/side-codecs/cs35l41_hda.c` — ASoC'siz, `cs_dsp` bilan aynan shu CSPL firmware'ni
  yuklaydi (laptoplar): `cs35l41_init_dsp` (cs_dsp_halo_init + `cs35l41_configure_cs_dsp` + `cs_dsp_power_up`),
  DSP1_RX1..6 manbalari (ASPRX1, VMON, IMON, ERRVOL, CLASSH_TGT/VBSTMON), `cs_dsp_run`,
  `cs35l41_set_cspl_mbox_cmd(RESUME/PAUSE)`. Bizning holatga eng yaqin.
- `drivers/firmware/cirrus/cs_dsp.c` — bizning yadroda **CONFIG_CS_DSP yo'q** → `cs35l41-lib.c` kabi
  modulimizga ichidan qo'shiladi (`audio/beyondx/Kbuild`, `build.sh` nusxalaydi).
- `sound/soc/codecs/cs-amp-lib.c` `cs_amp_write_cal_coeffs` — CAL_R/AMBIENT/STATUS/CHECKSUM'ni control nomi
  bilan yozadi (stock'ning qattiq manzillari o'rniga).

## Xavf va e'tibor

- **I2C bit-bang 100 kHz** (`bb_data.udelay = 5`): ~26 KB DSP xotirasi + 4 KB koeffitsiyent har amp'ga
  ≈ 2-3 s, ikkalasiga ~5 s. Boot'da muammo emas (fonda yuklansin).
- **Uyqudan keyin** amp'lar quvvatsiz qoladi (gph1 CON_PDN, amps_rescue ece1b62) → DSP'ni har resume'da
  qayta yuklash kerak (~5 s). Qo'ng'iroq kelganda jiringlash kechikmasin: DSP tayyor bo'lguncha ASPRX1
  bypass + past ovoz (hozirgidek), keyin DSP'ga o'tish.
- Tuning **18 dB gain** uchun → DSP yoqilganda `amp_gain = 18`.
- Qo'ng'iroq echo reference'i hozir ASPTX1 = ASPRX1, ASPTX2 = VMON (11.11 tuzatish) — DSP yo'lida stock
  ASPTX1 = DSPTX1. Qo'ng'iroq (echo, kechikish) qayta sinaladi.
- Amp registrlariga yozish → **har qadamda egasining «ha»si**. Modulni almashtirish = reboot (rmmod yo'q).
- Noto'g'ri tuning karnayni kuydirishi mumkin: faqat stock fayllar, boshqa (laptop) `cirrus/` fayllari emas.

## Reja (B)

1. Egasidan: EFS'dagi `/efs/cirrus/{rdc_cal,rdc_cal_r,temp_cal}` ni **faqat o'qishga** ruxsat (yo'q bo'lsa
   standart 8580/23). Fayllar `/lib/firmware/cirrus/` ga emas, o'zimizning yo'lga (`/usr/lib/firmware/omonullos/`)
   nusxa, sha256 bilan.
2. Kod (mic-codec): `cs_dsp.c` modulga; `amp_dsp=0|1` parametri (standart 0 = hozirgi yo'l). DSP yo'li: halo init →
   wmfw+bin → kalibrovka → DSP1_RX manbalari → `DAC_PCM1_SRC = DSP1TX1` → run → mbox RESUME/PAUSE ijro bilan;
   dmesg'ga CSPL_STATE, HEARTBEAT, CAL_SET_STATUS. Avval faqat pastki amp.
3. Yig'ish telefonda (`~/omonullos-src`), o'rnatish «ha» bilan → **reboot 1**: `amp_dsp=0` — hech narsa o'zgarmagani.
4. `amp_dsp=1` (jonli yoki modprobe parametri) → **reboot 2** kerak bo'lishi mumkin; -24 dB'da sinov: ovoz bor,
   CSPL_STATE 0, HEARTBEAT o'sadi, BDLOG nol.
5. Bosqichma-bosqich -18 → -12 → -6 → 0 dB (egasi tinglaydi, BDLOG_MAX_TEMP/OVER_* kuzatiladi), `amp_gain 18`.
6. Uyqu + resume (DSP qayta yuklanishi, jiringlash), qo'ng'iroq (echo), so'ng quloq dinamigi (rcv) va stereo.

## Holat

- **14.11 11:20** EFS'da `cirrus/` yo'q (egasi faqat o'qishga ruxsat berdi, faqat `ls` qilindi): zavod kalibrovkasi
  saqlanmagan → stock ham standart RDC 8580 / 23 C yozadi; bizda ham shu (`dsp_cal_r`, `dsp_cal_ambient`).
- **14.11 11:40** kod: `snd-zrn-abox-bx` `amp_dsp` (0644, bit 0 pastki, bit 1 quloq; standart 0 = avvalgidek),
  `dsp_gain` 18, `dsp_state` (o'qish). Yuklash fonda (`dsp_load_work`), ishga tushirish birinchi ijroda (bit clock
  bilan), PAUSE/RESUME amp bilan, resume'dan keyin qayta yuklash. `cs_dsp.c` modul ichida (Kbuild, build.sh).
  Firmware `/usr/lib/firmware/cs35l40-{bot,rcv}-dsp1-spk-prot.{wmfw,bin}` (zvendor-firmware ro'yxatida ham).
  Telefonda yig'ildi, vermagic mos, barcha belgilar yadroda bor. **O'rnatilmagan.**
- Sinov tartibi: o'rnatish («ha») + reboot → `amp_dsp=0` da ovoz avvalgidek → `echo 1 > .../amp_dsp`
  («ha», amp DSP'siga yozadi) → `dsp_state` «bot:loaded» → ijro → «running», ovoz -24 dB'da.
- **14.11 12:12** e611fa8: `amp_dsp=1` → `bot:loaded` (BYX_18dB_SPKPV2_PPP_20190131) → ijro → **«speaker protection
  running, cal R 8580 ambient 23»**, `bot:running`. Birinchi marta ishladi; egasi: ovoz yaxshi, oldingidek (-24 dB).
- **14.11 12:20** keyingi kod (o'rnatilmagan): `dsp_vol_boost` (0644, 0.125 dB qadam, standart 0) — faqat DSP orqali
  o'ynayotgan amp'ning ovoziga qo'shiladi, jami 0 dB gacha; boshqalari (quloq dinamigi) AMP_VOL_MAX -6 dB'da qoladi.
  ALSA «Speaker Playback Volume» oralig'i o'zgarmagan (0..768, zaudio.env 624). `dsp_diag` (o'qish): HEARTBEAT,
  CSPL_STATE, TEMPERATURE, CAL_SET_STATUS, BDLOG_* , power/ipeak/vpeak (xom qiymatlar).
  Sinov: o'rnatish + reboot → `amp_dsp=1` → boost 48 (-18 dB) → 96 (-12) → 144 (-6) → 168 (-3) → 192 (0), har birida
  egasi tinglaydi va `dsp_diag` (overtemp/overexc/mute = 0, state = 0).
- **14.11 12:28** 7829d07 o'rnatilgan, reboot toza (CS35L40 x2). `amp_dsp=1` → loaded → running (cal R 8580, 23 C).
  `dsp_vol_boost` 48 (-18 dB) → 96 (-12) → 144 (-6 dB): qo'ng'iroq ohangi 6 s har birida, egasi: «ovoz yaxshi».
  `dsp_diag` har safar state=0, overtemp/overexc/mute=0, temp o'zgarmadi (376832), maxexc 204472→204474,
  power 0→8→93. Keyingisi: 168 (-3) / 192 (0 dB) — alohida «ha».
- **14.11 12:33** 168 (-3 dB): ohang toza; YouTube qo'shiq ~3 daqiqa: uzilish yo'q, egasi «bass biroz kam».
  Lekin `dsp_diag`: BDLOG_MAX_TEMP 2473983 = **151.0 C** (Q14, 151<<14 - 1 = to'yinish), OVER_TEMP_COUNT 0→71
  (musiqa davomida ~1/2 s), CSPL_TEMPERATURE odatda 23.0 (bir marta 151), ipeak/vpeak doim 0. Ya'ni coil modeli
  to'g'ri kirish olmayapti → «qizib ketdi» deb cheklagan (bass kam shundan bo'lishi mumkin). Boost 144 (-6) ga qaytarildi.
  Sabab (stock cs35l41.c bilan solishtirish): stock DSP1 RX5 VPMON, **RX6 CLASSH, RX7 TEMPMON** va DSP1 widget
  VMON/IMON/**VPMON/VBSTMON/TEMPMON** ADC'larini yoqadi (PWR_CTRL2 8,9,10,12,13); bizda RX6 VBSTMON (mainline hda),
  faqat VMON/IMON yoqilgan edi. Tuzatish: stock kabi; dsp_diag'ga gain (CSPL_OVERSIGHT_GAIN), reduce, rx=, pwr2=.

### 11-14 12:45 — 58f32c7 sinovi va HALO rate topilmasi

- 58f32c7, amp_dsp=1, boost 144 (-6 dB), YouTube: rx=08,08,18,19,28,21,3a, pwr2=3721 (stock bilan bir xil),
  lekin ipeak=vpeak=0, temp 23.00 C da qotgan (376832 Q14), maxtemp keyin 151 C ga sakradi, overtemp=1.
  Boost darhol 0 ga qaytarildi.
- i2c o'qish (faqat o'qish): 0x4100 VIMON_SPKMON_RESYNC=1, 0x4310=1, 0x4400=0, DSP1 RX/TX RATE hammasi =1.
  Stock beyondx DT'da `cirrus,use-fsync-errata` yo'q -> wm_adsp RUN'da rate'larni 0 yozadi; mainline
  fs errata 1 yozadi (u resync=0 bilan juft). Bizda aralash holat: rates 1 + resync 1.
- Tuzatish: `dsp_fs_errata` param (0644, standart 0 = stock, rates hammasi 0; 1 = mainline). dsp_diag'da
  `rate=` (RX1,RX3,RX4) va `resync=` ham ko'rinadi.

### 11-14 12:55–13:05 — 5f9a21c sinovi (stock rate'lar 0)

- 5f9a21c (sha 48256fe5) toza yondi. amp_dsp=1 → loaded → running, cal R 8580 / 23.
- Boost 0, YouTube ~3 daq: rate=0,0,0; **151 C sakrash yo'q, overtemp 0, mute 0**; maxexc 5887→204472
  (siljish modeli audio ko'ryapti), gain 1.0..1.84 (Q20) o'zgarib turadi; power 0.
- Boost 144 (-6 dB), ~1 daq: power 56..254, overtemp/mute 0, egasi ovozdan shikoyat qilmadi.
  **Lekin ipeak=vpeak=0 va temp/maxtemp roppa-rosa 376832 (23.00 C) — qimirlamaydi.**
  Xulosa: rates 1 da (oldin) model I≈0 ko'rib Re→∞ (151 C) hisoblagan; rates 0 da harorat
  bahosi umuman yangilanmayapti. IV hali DSP'ga to'g'ri yetmayapti deb hisoblaymiz.
- `dsp_diag` `resync=ff` — regmap 0x4100 ni o'qiy olmaydi (readable emas); i2c bilan o'qish kerak.
- i2c faqat o'qish (0x40, ijrosiz): 3854=05180240, 400c=0, 410c=0, VI_VOL_POL 4000=08000800 (stock
  default bilan bir xil), 4100=1, 4310=1, **VPVBST_FS_SEL 4400=0** (stock default 1, stock A0 errata
  unga tegmaydi; mainline A0 errata 0 yozadi), ASP_CONTROL4 4854=01000000, AMP_GAIN 6c04=240 (18 dB).
- Stock A0 errata (`cs35l41_reva0_errata_patch`, vk9820) mainline'nikidan farqli: VIMON_SPKMON_RESYNC,
  0x4310, VPVBST_FS_SEL, PWR_CTRL2, AMP_GAIN_CTRL, ASP_TX3/4_SRC, ACCEL_PL0 ni YOZMAYDI. Bular stock'da
  faqat `cirrus,use-fsync-errata` patch'ida (beyondx'da yo'q).
- KEYINGI (taklif): (1) IV'ni to'g'ridan-to'g'ri tekshirish = stock kalibrovka: wmfw +
  `/mnt/vendor/firmware/cs35l40-bot-dsp1-spk-prot-calib.bin` → run → ~1.25 s jimjitlik → CAL_STATUS/CAL_R.
  R ~5-8 Ohm va status OK bo'lsa IV ishlaydi (va haqiqiy RDC olinadi). (2) VPVBST_FS_SEL ni stock 1 ga
  qaytarish (mainline A0 errata o'rniga stock'niki) — alohida sinov.

### 2026-11-14 13:25 — 939be5f kalibrovka sinovi (egasi «ha»)
- amp_dsp=1 → bot:loaded (~5 s); jimjitlik ijrosi paytida `dsp_calibrate=23` ikki marta (2-sida 3 s
  amp oldindan yoniq): ikkalasida `status 2 r 0 checksum 2 (ok) cspl_state 0`. Stock bo'yicha
  2 = `CS35L41_CSPL_STATUS_INCOMPLETE` (3 = out of range). Ketma-ketlik stock cirrus-cal bilan bir xil.
- **Yangilik:** kalibrovkadan keyin `ipeak/vpeak` birinchi marta ≠ 0 (2340/989, keyin 2168/1164;
  har kalibrovkada yangilanadi — kalibrovka signali tokni o'lchayapti) va jimjitlikda `temp`
  qimirlaydi (377033..377259 ≈ 23.01-23.03 C; oldin roppa-rosa 376832). Ya'ni REINIT'dan keyin IV
  DSP'ga yetadi. Gumon: birinchi RUN'da (REINIT'siz) IV/harorat modeli ulanmaydi.
- Fix (build): `dsp_cal_ms` param (default 1250) + CAL_STATUS har 100 ms kuzatiladi, natijada
  `trace status@ms`. Sinov: dsp_cal_ms=8000 → status 2 dan qachon chiqadi yoki umuman chiqmaydimi.
- Keyingi alohida sinov: kalibrovkasiz, oddiy RUN'dan keyin bitta REINIT qilib musiqa → ipeak/vpeak.
- 13:33 qo'shiq sinovi (eski 939be5f modul, kalibrovka REINIT'laridan keyin, boost 0, 20 s pianino):
  temp silliq ko'tarildi 377304→378743 (23.03→23.12 C), overtemp 0, ipeak/vpeak ushlangan qiymatda.
  Oldin -6 dB da ham 23.00 qotib turardi → **REINIT'dan keyin harorat modeli ishlaydi**. Reja: spk_run'da
  birinchi RUN'dan keyin bitta STOP_PRE_REINIT/REINIT. dd813f2 o'rnatildi (sha 37047541), reboot 13:35.

### 2026-11-14 13:36-13:55 — dd813f2 boot: amps never power up (-110)
- From the first sound of this boot (33 s, amp_dsp=0) every `bottom speaker on` fails:
  `Enable(1) failed: -110` (no PUP_DONE within 100 ms), IRQ1_STATUS1 00400000. None of the 6
  earlier boots had it (journal). dd813f2 does not touch amp_power/probe.
- amp_dsp=1 loads/runs the DSP, but dsp_calibrate is refused (amp not on). Not calibrated.
- `amp_rescue=1` (nRESET low/high + full re-probe, ret 0) → still -110. The **earpiece** amp
  (amp_mask=2, silence) → also -110 → common cause, not one amp / not stale DSP state.
- Read-only after a fresh reset + one attempt (i2c, 0x40): IRQ1 S1 00400000, S2 00300000,
  S3 000000be (bit1 PLL_LOCK plus bits 2-5,7; vendor CS35L41_PLL_UNLOCK 0x10), S4 80000003,
  PWR_CTRL1 1 (GLOBAL_EN left asserted after the failure — driver bug: should de-assert),
  PWR_CTRL2 3721, PLL_CLK_CTRL 510, GLOBAL_CLK_CTRL 3, SP_FORMAT 20200040 (same as the
  earpiece amp).
- ABOX side same as good runs: uaif1=01000017, qch 8. Battery 4.29 V, charging (also charging
  on boot -3 which worked).
- Suspect: bit clock frequency wrong/unstable (amp PLL locks then unlocks) — e.g. PLL_AUD or the
  CMU_AUD dividers not as S-Boot usually leaves them (driver assumes 1179.648 MHz). No way to read
  CMU here without /dev/mem. Next: plain reboot; if it stays, add a read-only diag param (CMU_AUD
  PLL_CON0/DIV_AUDIF/DIV_UAIF1/MUX_UAIF1 + amp IRQ1 S1-S4) and de-assert GLOBAL_EN on a failed
  enable.

### 11-14 14:20 — ed124b1 sinovi va keyingi tuzatish

- ed124b1 boot: amp_dsp=1 → run, so'ng ~46 ms keyin REINIT → **CSPL_STATE 1 (error)** butun run davomida;
  ipeak/vpeak 0, harorat 23.00 da qotgan; dsp_calibrate «tuning refused (CSPL_STATE 1)».
- dsp_run_reinit=0 + amp_rescue=1 (DSP qayta yuklandi, kirishlar run'dan oldin) → CSPL_STATE 0.
  Demak aybdor juda erta REINIT, spk_inputs-before-run emas. IV hali 0 (kalibrovkasiz).
- Shu holatda dsp_calibrate 23 → status 1, **R 8786 (6.2817 Ω)**, checksum ok, kept, 948 ms.
  Shundan keyin ipeak/vpeak 686/3054, harorat jimlikda sekin ko'tariladi; amp off/on'dan keyin ham.
- Tuzatish: dsp_run_reinit default 0; 1 bo'lsa REINIT dsp_reinit_ms (1000) dan keyin delayed work'da,
  faqat CSPL_STATE 0 bo'lsa, keyin holat logga; qo'lda sinov uchun `dsp_reinit` (write 1).
  Sinov: amp_dsp=1 → jimlik → dsp_reinit=1 → state 0 qoladimi, ipeak/vpeak ≠ 0 bo'ladimi.
  Agar oddiy REINIT IV bermasa: kalibrovka yo'lidagi farq (calib .bin, RX1/2 mute, 0 dB) aybdor.

### 11-14 14:30–14:44 — 814b55b va 551a284 sinovi
- 814b55b: qo'lda dsp_reinit → CSPL_STATE 1, IV 0 (ed124b1 bilan bir xil) → vaqt emas, REINIT oldidan
  playback tuning (spk-prot.bin) qayta yuklanmagani aybdor edi. 551a284: spk_first_reinit = inputs 0,
  STOP_PRE_REINIT, spk-prot.bin, cal, REINIT, inputs qayta (kalibrovkaning qaytish yo'li kabi).
- 551a284 (sha 41953a20) boot toza; amp_dsp=1 → jimlik → dsp_reinit=1 → «REINIT with the amp on,
  cal R 8786, CSPL_STATE 0». Harorat jimlikda ham qimirlaydi (23.00 → 23.03 C), calset 1→2.
- Qo'shiq, boost 0 (14:43:56–14:44:15): state 0, harorat ravon ko'tarildi 23.12 → 23.18 C (tebranib),
  overtemp/mute 0, power 0 (−24 dB da kutilgan), ipeak/vpeak 0 (oldin faqat kalibrovka tonidan keyin
  ≠ 0 edi — ehtimol yuqori signalda yangilanadi). Coil model ishlaydi — 13:33 sinovi bilan bir xil.
- Keyingi: /etc/modprobe.d conf'ga amp_dsp=1 dsp_run_reinit=1 → reboot → avtomatik REINIT logi.
- **14.11 16:42 (799e511 o'rnatilgan, conf'dan amp_dsp=1):** egasi «ha» → `dsp_vol_boost=144` (-6 dB, ya'ni
  oldingi -24 dB'dan +18 dB). YouTube qo'shig'i ~90 s: state 0, harorat 23.0 → eng ko'pi 32.3 C (528475/16384),
  power 24..271, qo'shiq pasayganda 27 C ga tushdi; overtemp/overexc/mute/reduce 0. Egasi: «ovoz yaxshi».
- **14.11 16:47:** egasi «ha» → `/etc/modprobe.d/snd-zrn-abox-bx.conf` ga `dsp_vol_boost=144` qo'shildi
  (zaxira `.bak-1114-1647`): `options snd_zrn_abox_bx dsp_cal_r=8786 amp_dsp=1 dsp_run_reinit=1 dsp_vol_boost=144`.
  Keyingi reboot'da avtomatik -6 dB. Keyingi: -3 dB (168) qayta sinov (oldin «bass biroz kam» — coil model
  o'shanda ishlamagan edi), rcv amp DSP.
- **14.11 16:49 -3 dB qayta sinov (boost 168, coil model endi ishlaydi):** qo'shiq ~2 daqiqa: state 0, maxtemp
  41.8 C (684275), temp qo'shiq pasayganda ~27 C ga qaytadi, power ≤763 (-6 dB'da ≤271), overtemp/overexc/mute/
  reduce 0, gain 1.0. Egasi: «ovoz yaxshi». **16:53** egasi «ha» → conf `dsp_vol_boost=168` (zaxira
  `.bak-1114-1653` = 144). Ehtiyot: dsp_diag ipeak/vpeak hali 0 (temp model esa qimirlaydi); 0 dB (192) qilinmaydi.

### 2026-11-15 — stereo (ikkala amp himoya bilan), quloq dinamigi kalibrovkasi

- Eski modul bilan jonli sinov (egasi «ha»): `amp_dsp=3 amp_mask=3`, boost 96 (-12 dB) va 144 (-6 dB): ikkala
  karnaydan toza ovoz, state 0, eng ko'pi 31.9 C (bot) / 29.1 C (rcv), overtemp/mute 0.
- **Topilma:** `dsp_vol_boost` himoya orqali chalayotgan har qanday amp'ga qo'shilardi — quloq dinamigi qo'ng'iroqda
  ham (+18 dB quloqda); stereo → qo'ng'iroqda amp yoniq qolib, eski yo'l saqlanardi. Darhol mono -3 dB'ga qaytarildi.
- 40d0829 (`spk-stereo`): qo'ng'iroqda quloq dinamigi hech qachon DSP orqali emas (ASPRX1, boost yo'q); call on/off va
  amp_mask o'zgarganda amp'lar avval o'chadi; `dsp_cal_r_rcv` (8580), `dsp_calibrate_rcv`. Telefonda yig'ildi
  (taskset 0-3, -j1: -j8 yig'ish 85 C → issiqlik himoyasi CPU offline → osilish, TWRP).
- O'rnatildi (`/root/tools/snd-zrn-abox-bx.ko` sha e6170f7c, eskisi `.bak-1115`) + reboot. Qo'ng'iroq (karnay tugmasi
  bilan): quloq dinamigi DSP'si to'xtab turdi (hb qotdi), ovoz oddiy; keyin stereo tiklandi.
- `cs35l40-rcv-dsp1-spk-prot-calib.bin` VENDOR'dan `/usr/lib/firmware` ga (sha d48a9107). `dsp_calibrate_rcv=23`:
  **status 1, R 7874 = 5.6297 Ω**, 940 ms, checksum ok. (8786 bilan model quloq dinamigini sovuqroq ko'rgan bo'lardi.)
- Stereo -3 dB (168), qo'shiq ~3 daq: state 0, eng ko'pi 35.5 C (bot) / 37.1 C (rcv), power ≤1135 / ≤357,
  overtemp/overexc/mute/reduce 0; egasi: «ikkalasidan, toza, balandroq».
- Doimiy (egasi «ha»): `/etc/modprobe.d/snd-zrn-abox-bx.conf` = `dsp_cal_r=8786 dsp_cal_r_rcv=7874 amp_dsp=3
  amp_mask=3 dsp_run_reinit=1 dsp_vol_boost=168` (eskisi `.bak-1115-stereo`). 0 dB qilinmaydi.
- Ochiq: ipeak/vpeak hali 0 (temp modeli ishlaydi); zspk-calibrate faqat pastki karnayni o'lchaydi; drayver OTA'da yo'q.
