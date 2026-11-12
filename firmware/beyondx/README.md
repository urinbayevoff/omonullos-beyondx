# Galaxy S10 5G (SM-G977N) Wi-Fi / Bluetooth firmware

From this phone's stock VENDOR partition (G977NKSU6HWD3, `/vendor/firmware/`),
taken out of the AP tar with debugfs, renamed the way brcmfmac asks for it:

| here | stock | note |
|---|---|---|
| `brcmfmac4375-pcie.bin` | `bcmdhd_sta.bin_b1` | 4375b1 FW 18.41.117, FWID 01-1b4772fd |
| `brcmfmac4375-pcie.txt` | `nvram.txt_CS01_semco_b1` | the only B1 nvram (SEMCO E43 R03), CRs stripped |
| `brcmfmac4375-pcie.clm_blob` | `bcmdhd_clm.blob` | |
| `bcm4375B1_*.hcd` | same | Bluetooth patch RAM, for later; the module vendor (semco/murata) decides which |

The other nvrams in VENDOR (`*_b0`, `1rh_es1x`) are for B0 engineering samples.
The nvram's `macaddr` is a placeholder (00:90:4c:30:50:01); the phone's own
MAC is in EFS (`/efs/wifi/.mac.info`, read only, never written).
