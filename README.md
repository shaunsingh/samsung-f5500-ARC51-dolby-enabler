# Samsung F5500 ARC 5.1 and plasma tuning

[SamyGO](https://forum.samygo.tv/) payloads for a 2013 Samsung F-series plasma, developed on a **PN60F5500AF** running firmware **T-MST12AKUC-2225.0**. These payloads are injected into the running `exeTV` process from a SamyGO USB stick. Nothing is written to the TV's flash: without the stick, the TV is stock.

| Component | What it does | Works on |
|---|---|---|
| **`libDDCRAM_EDID`** | Makes HDMI sources offer **Dolby Digital 5.1** and **DTS 5.1**, which the TV passes to an HDMI-ARC or optical receiver. No EDID emulator or audio extractor is required. | F-series `T-MST12` firmware that exports the MStar functions in [the firmware notes](docs/firmware-reference.md) |
| **`libPDPTune`** | Unlocks **10-point White Balance**, **Color Space → Custom**, and **Expert Pattern**, and can override plasma drive, Deep Black, Peak Stretch, duty, Sub Contrast, and the 12-bit gamma curve (`BT1886`, `2.4`, `2.3`, `TOE`). | Only the PN60F5500 with exactly `T-MST12AKUC-2225.0`. Any other firmware is left alone. |
| **`libArcToggle`** | One remote key swaps **PCM** and **Dolby Digital** on the SPDIF/ARC output, the same setting as Sound → Audio Format. | Only the PN60F5500 with exactly `T-MST12AKUC-2225.0`. Any other firmware is left alone. |

## Repository layout

```text
src/            libDDCRAM_EDID.c, libPDPTune.c, libArcToggle.c, libCMSApply.c,
                libCMProbe.c, libCMFactory.c, libHPD.c, libAudioProbe.c,
                gamma_tables.h, samygo.h
init/           boot scripts for the EDID patch, libPDPTune, and libArcToggle
scripts/        patch_edid_51.py, gamma_curves.py, gamut/, re/
test/           fake_exetv.c, the QEMU stand-in for the two boot payloads
prebuilt/       the built libraries
calibration/    picture-state files and this set's EDIDs
docs/           firmware-reference.md, validation.md
```

`libCMSApply` writes the BT.709 Custom preset. `libCMProbe` and `libCMFactory` only read the color tables. `libHPD` pulses hot-plug on HDMI3. `libAudioProbe` reads the external-output audio mode. The boot scripts do not load those four.

## Reference

Longer notes live in [`docs/`](docs/), not in this file:

- [Firmware, registers, gamma, and the color model](docs/firmware-reference.md)
- [What was checked on the TV, including the BT.709 backup files](docs/validation.md)

## Requirements

- A TV rooted with `SamyGO-F-SM-MST-Fix`, which provides `samyGOso`.
- A SamyGO USB stick. These paths are the stick as the TV mounts it: `/mnt/opt/privateer/usr/libso/` for libraries and `/mnt/etc/init.d/` for boot scripts.
- For audio passthrough, SamyGO [`libHDMIpassthru`](https://forum.samygo.tv/) started with `OPTIONS="AUTO PCM DTS"`.
- A shell on the TV. See [Root shell access](#root-shell-access).

## Dolby Digital 5.1 over ARC

The stock EDIDs advertise only stereo LPCM, so a source never offers Dolby Digital or DTS. `libDDCRAM_EDID` writes the HDMI receiver's DDC RAM, which is the copy a source actually reads. The patched EDID keeps stereo LPCM and adds:

- Dolby Digital 5.1 at 32/44.1/48 kHz, up to 640 kbps.
- DTS 5.1 at 32/44.1/48 kHz, up to 1536 kbps.
- A 5.1 speaker-allocation block.

Video timings and each port's HDMI physical address stay as they were. Multichannel LPCM is not advertised, because ARC cannot carry it. DDC RAM survives standby and is filled with the stereo EDIDs again after mains power is removed, so the boot script loads the patch at every start.

### Installation

1. Copy `prebuilt/libDDCRAM_EDID.so` to `/mnt/opt/privateer/usr/libso/`.

2. Dump this TV's EDIDs. Do not use a dump from another set.

   ```sh
   . /dtv/SGO.env
   samyGOso -T -B -l /mnt/opt/privateer/usr/libso/libDDCRAM_EDID.so DUMP
   cat /dtv/DDCRAM_EDID.log
   ```

   Keep `/dtv/DDCRAM_port0.bin` through `/dtv/DDCRAM_port3.bin`.

3. Patch them on a computer:

   ```sh
   python3 scripts/patch_edid_51.py ./dumped ./patched
   ```

   Copy the patched files into `/mnt/opt/privateer/usr/libso/` on the stick.

4. Load them and pulse hot-plug detect so connected sources read the new EDID:

   ```sh
   samyGOso -T -B -l /mnt/opt/privateer/usr/libso/libDDCRAM_EDID.so LOAD HPD
   cat /dtv/DDCRAM_EDID.log
   ```

   Each port in use should report `LOAD-OK` or `LOAD-ALREADY-OK`, with:

   ```text
   ADB=290907071507503d07c0 SADB=830f0000
   ```

   `LOAD-MISMATCH` means the read-back differed. `LOAD-SKIP` means the file is missing or not a valid EDID.

5. Copy `init/99_91_DDCRAM_EDID.init` to `/mnt/etc/init.d/` and `chmod 755` it. An empty `STOP_EDID` or `STOP` file in the root of the stick skips the script.

### What to set

Set the format in three places. The port a device is plugged into does not matter.

| Where | Setting |
|---|---|
| **Source** | HDMI audio to **Dolby Digital**, or to **DTS** when the receiver decodes DTS. This is one output format for the whole source, not a per-app switch. Many consoles have no separate "bitstream out" item; the format list is the switch. A Blu-ray player's **Let my receiver decode audio** option is a separate path. |
| **TV** | **Sound → Additional Settings → SPDIF Output → Audio Format → Dolby Digital**. If that list is greyed out, open **DTS** once, then choose **Dolby Digital**. |
| **Passthrough** | `libHDMIpassthru` with `OPTIONS="AUTO PCM DTS"`. |
| **Receiver** | The HDMI-ARC input, or optical. Confirm the format on the receiver. The TV's Audio Format line can keep saying **PCM** while a Dolby Digital or DTS bitstream is going out. |

Leave the source in its normal limited RGB range, with the TV's HDMI black level on **Low**. A full/PC RGB mode crushes the codes just above black.

### Audio limitations

- Content-aware automatic switching is not implemented. Beam has no input-format override, and Samsung exposes only stereo PCM capture and stereo level reporting, not the six decoded channels needed to detect surround activity inside a DD 5.1 container. A clean implementation would require a new multichannel decoder/DSP tap and a hysteretic PCM/bitstream router. The remote toggle below is the manual switch.
- Because the source format is global, stereo programmes are re-encoded into Dolby Digital when the source is set to Dolby Digital. They are not passed as stereo PCM for the receiver to upmix.
- DTS 5.1 is advertised and selected the same way. Playback was not checked on a receiver that only decodes Dolby Digital.
- This path does not carry multichannel LPCM, Dolby Digital Plus, TrueHD/Atmos, DTS-HD, or eARC.

The checks behind those limits are in [validation](docs/validation.md).

## Remote PCM / Dolby Digital toggle

`libArcToggle` calls the same function as **Sound → Additional Settings → SPDIF Output → Audio Format**. PCM is `0` and Dolby Digital is `1`. The call uses query `0`, which is the path that programs the output. A different firmware is logged and left unchanged.

The live command is the first byte of the buffer passed to `TDcPrimeMicomInterface::t_ParsePacket`. On this set that byte is `0x50` for the DVR button. The same command also arrives when the button is not pressed, and arming it flips PCM and Dolby during playback, so the boot configuration does not arm a key. With no words the payload only logs presses. `KEY:` arms the toggle. A held key repeats. The payload toggles once per press and swallows every repeat of the armed key, so a hold does not also open a menu.

Playback of the switch on a receiver has not been checked. The log shows the menu value before and after the call. If that value changes and the bitstream does not, the output refresh is a separate step.

### Installation

Copy `prebuilt/libArcToggle.so` to `/mnt/opt/privateer/usr/libso/` and `init/99_93_ArcToggle.init` to `/mnt/etc/init.d/` (`chmod 755`).

With no words, the boot script logs commands and does not toggle. `/mnt/opt/privateer/usr/libso/libArcToggle.conf` replaces that. One or more words per line. On this set the DVR button is `KEY:0x50`. An empty `STOP_ARCTOGGLE` or `STOP` in the root of the stick skips the script.

### Options

| Option | What it does |
|---|---|
| *(none)* / `LOG` | Hooks the remote and appends each key id to the log. Does not toggle. |
| `KEY:N` | Toggles on that key id. `0x` is hex; a bare number is decimal. `1`–`255`. |
| `PCM` / `DD` / `TOGGLE` | Sets PCM, sets Dolby Digital, or swaps once. Does not hook the remote unless a hook option is present. `DOLBY` is the same as `DD`. |
| `OFF` | Removes the hook. It accepts no other word. |

A bad word is reported and the whole injection is ignored. Injecting again changes the key and the log switch without installing a second hook.

### Find the button, then arm it

```sh
. /dtv/SGO.env
samyGOso -T -B -r -l /mnt/opt/privateer/usr/libso/libArcToggle.so LOG
# press the button once
cat /dtv/ArcToggle.log
samyGOso -T -B -r -l /mnt/opt/privateer/usr/libso/libArcToggle.so KEY:111
cat /dtv/ArcToggle.log
```

On this set the DVR button is command `0x50`. Use the command `LOG` printed. To try the switch before a button is known:

```sh
samyGOso -T -B -l /mnt/opt/privateer/usr/libso/libArcToggle.so TOGGLE
cat /dtv/ArcToggle.log
```

The log is `/dtv/ArcToggle.log`. A line like `PCM -> Dolby set=1 now=Dolby` means the menu value changed. On this firmware the call returns 1. `set=` is that return code.

## Plasma picture tuning

`libPDPTune` checks nine `exeTV` symbols and the `LogicFHD` panel driver before it hooks anything. A different firmware is logged and left unchanged. Each injection is the whole configuration: a word that is left out goes back to stock.

### Installation

Copy `prebuilt/libPDPTune.so` to `/mnt/opt/privateer/usr/libso/` and `init/99_92_PDPTune.init` to `/mnt/etc/init.d/` (`chmod 755`).

The boot script's default is `UNLOCK DRIVE:STANDARD`. The file `/mnt/opt/privateer/usr/libso/libPDPTune.conf` replaces that default. One or more words per line. An empty `STOP_PDPTUNE` or `STOP` in the root of the stick skips the script.

The configuration on this TV is:

```text
UNLOCK DRIVE:STANDARD DEEPBLACK:ON PEAKSTRETCH:ON SUBCONTRAST:132 DUTY2:139 OSD_GAMMA:0 GAMMA:2.4
```

### Options

| Option | On this TV | What it does |
|---|---|---|
| `UNLOCK` | **Enabled** | Shows 10-point White Balance, Color Space → Custom, and Expert Pattern. |
| `DRIVE:STANDARD` | **Enabled** | In 2D, Movie and Relax normally use drive mode `0x05` and Dynamic, Standard, and Natural use `0x11` (register `0x806f`). This sends `0x11` wherever the firmware asks for `0x05`. |
| `DEEPBLACK:ON` / `OFF` | **Enabled** (`ON`) | Holds the Deep Black register `0x809c` at `3`. `OFF` holds it at `0`. `ON` is the value this panel already has at startup. What the logic board does with it is not documented. |
| `PEAKSTRETCH:ON` / `OFF` | **Enabled** (`ON`) | Holds the Peak Stretch register `0x8063` at `1`. `OFF` holds it at `0`. `ON` is the value this panel already has at startup. What the logic board does with it is not documented. |
| `SUBCONTRAST:<100..160>` | **132** | Sets Sub Contrast (register `0x8927`, the service value times 16) apart from the Contrast menu. `132` is the factory Standard value. Movie's factory value is `128`. This is a level, not a switch. |
| `DUTY2:<79..160>` | **139, experimental** | Writes the second panel duty word. See [Duty](#duty). |
| `GAMMA:TOE` / `2.3` / `2.4` / `BT1886` | **`2.4`** | Composes a 12-bit curve onto `exeTV`'s gamma LUT and keeps white balance. Not hooked unless one of these words is present. |
| `OSD_GAMMA:<-3..3>` | **0** | Sets the OSD Gamma slider the curve is built on. `0` is the neutral baseline. |
| `DUMP` | **Not left on** | Logs hook state, registers, OSD Gamma, and panel temperature. Changes nothing. |
| `OFF` | **Not in use** | Removes the hooks and restores the firmware's register and gamma values. |

The firmware writes Deep Black and Peak Stretch as a pair from Black Optimizer: `0`/`0` for Off, `3`/`0` for Dark Room, and `0`/`1` for Bright Room. This set powers on at `3`/`1` and the firmware leaves that pair alone, which is why both overrides are enabled.

### Duty

`DUTY2` is the second duty word the logic board is given while the panel is lit (register `0x89da`). Samsung sends a constant. It is not a percentage of brightness, and it is not the Cell Light control.

| Value | What that maximum means |
|---|---|
| **128** | The highest duty value this firmware sends to any panel type. That is Samsung's ceiling. |
| **139** | The value on this TV, 11 counts above that ceiling. |
| **160** | The highest number `libPDPTune` will accept. This is an input limit in the tool, not a Samsung rating and not a point that has been checked as safe. |

A value above `128` is written only while `APC_OFFSET` (`0x89c0`) is `0`, which is Cell Light 20 with Eco and thermal dimming off. While automatic power control is active, the override stays at Samsung's ceiling. More light means more power, heat, and wear. Nothing here claims a measured increase in brightness or a known lifetime cost.

### Picture menu

These are the menu settings used with the configuration above:

| Control | Setting |
|---|---|
| Picture mode | Movie |
| Cell Light | 20 |
| Contrast | 100 |
| Brightness | 50 |
| Color Space | Custom, with the BT.709 values below |
| Gamma | 0 on the OSD slider (`OSD_GAMMA:0`), curve `GAMMA:2.4` |
| Source range | Standard / limited RGB, HDMI black level **Low** |

`2.4` is the curve BT.1886 becomes when the display's own black is treated as zero. Full-range RGB on the source crushes the codes just above black into that toe. Brightness has to sit on the black point: on a near-black PLUGE, in the source's own range, 50 is the value at which the bars above black are still just visible on this set.

### BT.709

Color Space → Custom, used with `GAMMA:2.4`:

| Colour | R | G | B | Estimated dE00 |
|---|---:|---:|---:|---:|
| Red | 46 | 0 | 17 | 0.56 |
| Green | 34 | 57 | 16 | 0.29 |
| Blue | 20 | 0 | 49 | 1.99 |
| Yellow | 48 | 56 | 21 | 0.53 |
| Cyan | 37 | 56 | 51 | 0.74 |
| Magenta | 46 | 0 | 50 | 1.03 |

These dE00 figures are calculated from a 2014 measurement of a different PN60F5500, not from a meter on this panel. Blue and magenta cannot reach a BT.709 primary inside that measured native gamut. `libCMSApply` applies this preset: `BT709` selects Custom, writes these six triples, and saves Movie. With no argument it returns the picture to Native. The model is in `scripts/gamut/`. The saved picture state is [calibration/PicSettings_HDMI3_2_BT709.bin](calibration/PicSettings_HDMI3_2_BT709.bin). The other picture files, and how they were restored, are in [validation](docs/validation.md) and [calibration](calibration/README.md).

### Apply a configuration

```sh
. /dtv/SGO.env
samyGOso -T -B -r -l /mnt/opt/privateer/usr/libso/libPDPTune.so \
    UNLOCK DRIVE:STANDARD DEEPBLACK:ON PEAKSTRETCH:ON SUBCONTRAST:132 DUTY2:139 OSD_GAMMA:0 GAMMA:2.4 DUMP
cat /dtv/PDPTune.log
```

A bad word is reported and the whole injection is ignored. Drive, duty, Sub Contrast, and gamma are experiments: change one at a time. None of them outlasts a restart unless the boot script loads them again.

## Root shell access

A stock SamyGO stick opens root FTP (21), telnet (23), and a raw root shell (4023) with no password. `init/run1.sh` and `init/01_02_telnet.init` bring those up only while the root of the stick contains an empty file named `ENABLE_SHELL`.

## Build and test

The TV is Linux 3.0.33, ARMv7, soft-float, glibc 2.14.1.

```sh
sudo apt install gcc-arm-linux-gnueabi qemu-user shellcheck
make
make test
make lint
make prebuilt
```

`make test` runs both libraries against `test/fake_exetv.c` under QEMU. It checks arguments, hooks, overrides, gamma tables, and EDID rules. It does not check ARC playback or the picture on a panel.

## Disable or revert

- `STOP_EDID` skips the EDID boot script. `STOP_PDPTUNE` skips the picture boot script. `STOP_ARCTOGGLE` skips the audio-key boot script. `STOP` skips all three.
- `STOP_SAMYGO`, or booting with no stick, starts the TV stock.
- `samyGOso -T -B -r -l /mnt/opt/privateer/usr/libso/libPDPTune.so OFF` removes the picture hooks on a running TV.
- `samyGOso -T -B -r -l /mnt/opt/privateer/usr/libso/libArcToggle.so OFF` removes the audio-key hook on a running TV.
- Without the EDID boot script, the stereo EDIDs return at the next power-on.

## License

MIT. See [LICENSE](LICENSE).
