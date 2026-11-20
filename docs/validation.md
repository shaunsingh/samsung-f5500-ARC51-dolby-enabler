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
