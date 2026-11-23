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

## Plasma register tuning (`libPDPTune` 2.5.1)

Stock Movie-mode 60 Hz readback on HDMI3 at 38 °C showed `0x806f = 0x0105`, `0x80bf = 0x0000` (DeepBlackMode off on 60 Hz), `0x89ee = 0x0001` (Static APC timer active), `0x89c0 = 0x0000` (APC offset 0), `0x89da = 0x0080` (`128`), `0x8927 = 0x0800` (`128`), `CinemaSmooth = 0`, and `OutputVFreq = 60.00 Hz`.

### Optical sweep (`scripts/optical_sweep.py`, Full-Field White `0x8114 = 0x0101`)

Symmetric `A → B → A` bracketed differential photometry (~0.85 s bracket width, 80%×80% spatial integration, 3× brackets per step) cancels both the 0.06 Hz beat between 59.94 Hz plasma and 30 Hz phone video and slow camera AE drift. Panel steady at 44 °C, `APCOffset = 0x0000`:

#### Coarse `DUTY2` sweep (vs `128`, `DRIVE:STANDARD`)

| Step | Relative Luminance | `±SD` | Notes |
|---|---:|---:|---|
| `NULL (128 → 128)` start | `100.38%` | `±0.13%` | Beat/drift control |
| `DUTY2:139` | `103.89%` | `±0.30%` | Small ramp start |
| `DUTY2:155` | `118.13%` | `±4.82%` | Climbing smoothly |
| `DUTY2:175` | `139.05%` | `±1.76%` | Peak region |
| `DUTY2:200` | `135.98%` | `±1.75%` | Just past peak |
| `DUTY2:225` | `123.99%` | `±4.81%` | Power-limit fold-back |
| `DUTY2:255` | `118.19%` | `±1.18%` | Folded back to `155` level |
| `NULL (128 → 128)` end | `100.77%` | `±0.96%` | Beat/drift control |

#### Fine peak zoom (`160..195` vs `128`) & mode totals (vs Stock Movie `128`)

| Step | Relative Luminance | `±SD` | Notes |
|---|---:|---:|---|
| `NULL (128 → 128)` start | `99.61%` | `±0.80%` | Control |
| `DUTY2:160` | `126.92%` | `±5.79%` | Rising |
| **`DUTY2:168`** | **`142.79%`** | **`±0.98%`** | **Absolute peak (3/3 runs `141.4–143.7%`, balanced RGB)** |
| `DUTY2:175` | `131.27%` | `±4.57%` | Rolling off |
| `DUTY2:182` | `123.15%` | `±3.25%` | Rolling off |
| `DUTY2:188` | `119.72%` | `±1.38%` | Rolling off |
| `NULL (128 → 128)` end | `99.82%` | `±0.74%` | Control |
| `DRIVE:STANDARD` (`128`, `DBM:OFF`) vs Stock Movie | `114.48%` | — | `+14.5%` |
| `DRIVE:STANDARD` + `DEEPBLACK_MODE:ON` (`128`) vs Stock Movie | `127.28%` | — | `+12.8%` over `DBM:OFF`, flicker-free at 60 Hz |
| Full Tuned (`STD + DBM:ON + DUTY2:175–182`) vs Stock Movie | `176.7–181.1%` | — | **~+80% total full-field luminance over stock Movie** |

Final persisted configuration (`UNLOCK DRIVE:STANDARD DEEPBLACK:ON PEAKSTRETCH:ON DEEPBLACK_MODE:ON POWERSAVE:OFF APCSPEED:OFF SAPC:OFF SUBCONTRAST:132 DUTY2:168 OSD_GAMMA:0 GAMMA:2.4`) verified live: `0x806f = 0x0111`, `0x809c = 0x0003`, `0x8063 = 0x0001`, `0x80bf = 0x0001`, `0x8064 = 0x0000`, `0x89bf = 0x41ff`, `0x89c0 = 0x0000`, `0x89ee = 0x0000`, `0x89da = 0x00a8` (`168`), `0x8927 = 0x0840` (`132`), `0x8114 = 0x0000`, `OutputVFreq = 60.00 Hz`, 44 °C. `CINEMA_SMOOTH:ON` was tested live (`mbrg_outputsource_setfilmmode` hooked, `0 -> 1`, `ok=1`) and left opt-in rather than forced globally because `Factory.log` shows `24Px4 Support = 1=ON` natively on 24 Hz inputs and forcing Cinema Smooth on 60 Hz sources can cause cadence stutter on F-series plasmas.

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
