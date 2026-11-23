# Live Validation

Validation was performed on the development **PN60F5500AF**, firmware **T-MST12AKUC-2225.0**, with an Xbox One X on HDMI3 and a Sonos Beam Gen 1 on HDMI2 ARC.

## Audio

- All four source-facing EDIDs read back with `ADB=290907071507503d07c0` and `SADB=830f0000`.
- `libHDMIpassthru` was resident with `OPTIONS="AUTO PCM DTS"` after reboot.
- With Xbox and Samsung both set to Dolby Digital, known 5.1 content produced `HTAudioInCode 84934713` on the Beam: **Dolby Digital 5.1**.
- The Beam simultaneously reported its `x-sonos-htastream:…:spdif` input as `PLAYING`.
- Before the Samsung router was set to Dolby Digital, the same path produced `33554434`: **stereo PCM 2.0**.
- Stereo Opus content in Jellyfin still produced `84934713`. This established that Xbox Dolby Digital mode re-encodes stereo rather than providing automatic stereo PCM.
- DTS playback on Beam Gen 1 was not tested.

The Beam's status endpoint is `http://BEAM_IP:1400/status/zp`. Samsung's OSD may display PCM regardless of the actual ARC bitstream.

### Remote ARC toggle (`libArcToggle` 2.0.0)

- Injected resident into `exeAPP` with `KEY:DVR` (`0x0072` / `114`).
- Seven consecutive presses of the remote **DVR** button logged clean press (`type=11`) and release (`type=12`) pairs and swapped the HDMI2 ARC output in 475–506 ms per press.
- Beam `HTAudioInCode` tracked every toggle live between `84934713` (Dolby Digital 5.1) and `33554434` (Stereo PCM 2.0).
- Zero universal-remote setup prompts appeared, and zero idle events were logged.

## Plasma register tuning (`libPDPTune` 2.5.0)

Stock Movie-mode 60 Hz readback on HDMI3 at 38 °C showed `0x806f = 0x0105`, `0x80bf = 0x0000` (DeepBlackMode off on 60 Hz), `0x89ee = 0x0001` (Static APC timer active), `0x89c0 = 0x0000` (APC offset 0), `0x89da = 0x0080` (`128`), `0x8927 = 0x0800` (`128`), `CinemaSmooth = 0`, and `OutputVFreq = 60.00 Hz`.

### Optical sweep (`scripts/optical_sweep.py`, Full-Field White `0x8114 = 0x0101`)

Unclipped median-filtered differential photometry (2× pass average per step, panel steady at 44 °C, `APCOffset = 0x0000`):

| Sweep / Setting | Relative Luminance | Spread | Notes |
|---|---:|---:|---|
| `DUTY2:128` (stock baseline) | `100.0%` | — | Reference (`DRIVE:STANDARD`) |
| `DUTY2:139` | `99.3%` | `±2.3%` | No measurable full-field gain |
| **`DUTY2:155`** | **`130.5%`** | **`±0.9%`** | **Reproducible peak before fold-back; cleanest RGB balance** |
| `DUTY2:175` | `122.8%` | `±1.4%` | Fold-back begins |
| `DUTY2:195` | `111.1%` | `±1.2%` | Trough (`-15%` vs `155`) |
| `DUTY2:215` | `110.3%` | `±4.0%` | Trough |
| `DUTY2:235` | `123.4%` | `±17.6%` | High subfield rolling-shutter variance |
| `DUTY2:255` | `132.8%` | `±7.2%` | Same ceiling as `155` with higher variance |
| Stock Movie (`128`, `SubC:128`) | `100.0%` | — | Mode A/B baseline |
| `DRIVE:STANDARD` + `DEEPBLACK_MODE:OFF` (`200`) | `149.9%` | `±3.9%` | |
| **`DRIVE:STANDARD` + `DEEPBLACK_MODE:ON` (`200`)** | **`163.7%`** | **`±3.5%`** | **+13.8% brighter than `DBM:OFF`; flicker-free at 60 Hz** |
| `DRIVE:STANDARD` + `SUBCONTRAST:150` (`200`) | `120.6%` | `±2.6%` | High SubContrast triggered ABL (`-26%` vs `132`) |

Final persisted configuration (`UNLOCK DRIVE:STANDARD DEEPBLACK:ON PEAKSTRETCH:ON DEEPBLACK_MODE:ON POWERSAVE:OFF APCSPEED:OFF SAPC:OFF SUBCONTRAST:132 DUTY2:155 OSD_GAMMA:0 GAMMA:2.4 CINEMA_SMOOTH:ON`) verified live: `0x806f = 0x0111`, `0x809c = 0x0003`, `0x8063 = 0x0001`, `0x80bf = 0x0001`, `0x8064 = 0x0000`, `0x89bf = 0x41ff`, `0x89c0 = 0x0000`, `0x89ee = 0x0000`, `0x89da = 0x009b` (`155`), `0x8927 = 0x0840` (`132`), `0x8114 = 0x0000`, `CinemaSmooth = 1`, `OutputVFreq = 60.00 Hz`, 44 °C.

## Picture menu used with this configuration

Movie, Cell Light 20, Contrast 100, Brightness 50, Color Space Custom, OSD Gamma 0, `GAMMA:2.4`. The source stays in limited RGB with the TV's HDMI black level on Low. Full-range RGB crushed the codes just above black.

## BT.709 Custom preset

- The firmware model reproduced all 81 stock Native color-cube words exactly.
- The computed Custom preset matched all 81 expected words after application.
- `libBackPicSet` reported Custom selector `3` and the six expected RGB triples.
- A Native → load-BT.709-file restore test restored Custom mode and the exact register set.
- A full TV reboot preserved Custom mode, all six triples, and all 81 register values.
- The pre-BT.709 and final active-state files differ in only 17 bytes: the selector and changed RGB bytes. All unrelated picture fields remained unchanged.

These checks verify settings persistence and the register-transfer model. They do not replace optical calibration.

The files on the development set are under `/dtv/usb/sda1/picsettings_final/`:

| File | MD5 | Purpose |
|---|---|---|
| `PicSettings_HDMI3_2_pre709.bin` | `579966ff4187163f683e6ba3be1cd2a9` | HDMI3 Movie settings before the Custom preset |
| `PicSettings_HDMI3_2_BT709.bin` | `4edc7e37c7f68ff0c1c3d6d2eb42f269` | HDMI3 Movie settings with the BT.709 Custom preset |

Restore either file with SamyGO `libBackPicSet`:

```sh
. /dtv/SGO.env
samyGOso -T -B -l /mnt/opt/privateer/usr/libso/libBackPicSet.so \
    LOAD:/dtv/usb/sda1/picsettings_final/PicSettings_HDMI3_2_BT709.bin
```

## Content-aware stereo/surround switching

The requested behavior was investigated: sustained front-left/right-only activity would select stereo PCM, while sustained center/surround/LFE activity would select the original DD 5.1 bitstream, with hysteresis.

Content-aware automatic switching is not implemented. Beam has no input-format override, and Samsung exposes only stereo PCM capture and stereo level reporting, not the six decoded channels needed to detect surround activity inside a DD 5.1 container. A clean implementation would require a new multichannel decoder/DSP tap and a hysteretic PCM/bitstream router.

## Optical limits

No meter was available for the development panel. BT.709 dE00 figures are model estimates based on another PN60F5500 measurement, not measurements of this unit. Blue and magenta cannot reach ideal BT.709 within the measured native gamut. Plasma drive and duty changes remain experimental.
