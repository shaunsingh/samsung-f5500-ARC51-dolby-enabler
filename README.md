# Samsung F5500 ARC 5.1 and plasma tuning

[SamyGO](https://forum.samygo.tv/) payloads for a 2013 Samsung F-series plasma, developed on a **PN60F5500AF** running firmware **T-MST12AKUC-2225.0**. Payloads are injected into the running `exeTV` (`libDDCRAM_EDID`, `libPDPTune`) or `exeAPP` (`libArcToggle`) process from a SamyGO USB stick. Nothing is written to the TV's flash: without the stick, the TV is stock.

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
scripts/        patch_edid_51.py, gamma_curves.py, optical_sweep.py, gamut/
test/           fake_exetv.c and fake_exeapp.c, QEMU stand-ins for all three boot payloads
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

`libArcToggle` runs in **`exeAPP`** and swaps **PCM** (`0`) and **Dolby Digital** (`1`) via `TCSpeakerProxy::SetExternalOutMode(..., 0)`, the same Binder IPC path used by **Sound → Additional Settings → SPDIF Output → Audio Format**. A different process or firmware build is logged and left unchanged.

Both the IR remote (via `exeTV` `KeyCommon` → Binder → `APPRemoteControlInputNative`) and the Bluetooth Smart Touch remote (via `bsa_server` → `BluetoothInputDev`) dispatch through `CRemoteControlInput::ProcessRemoteControlEvent` (`_ZTV19CRemoteControlInput` slots `+0x30` and `+0x44`), upstream of `SMInput` and `CTvApplicationManager::m_OnKeyDirectMBR`. Swapping those two vtable pointers atomically intercepts and swallows press, repeat, and release events for the armed key — so `KEY:DVR` (`0x72` / `114`) toggles PCM ↔ Dolby Digital cleanly, never triggers on idle MICOM frames, and never raises the universal-remote setup prompt.

### Installation

Copy `prebuilt/libArcToggle.so` to `/mnt/opt/privateer/usr/libso/` and `init/99_93_ArcToggle.init` to `/mnt/etc/init.d/` (`chmod 755`).

With no words, the boot script arms `KEY:DVR` (`0x72`). `/mnt/opt/privateer/usr/libso/libArcToggle.conf` replaces that default. One or more words per line. An empty `STOP_ARCTOGGLE` or `STOP` in the root of the stick skips the script.

### Options

| Option | What it does |
|---|---|
| *(none)* / `KEY:DVR` | Hooks `CRemoteControlInput` in `exeAPP` and toggles PCM ↔ Dolby Digital on `KEY_DVR` (`0x72` / `114`). |
| `KEY:N` | Toggles on any 16-bit Samsung keycode `1`–`65535` (`0x` hex or decimal) or `DVR`. |
| `LOG` | Hooks `CRemoteControlInput` and appends each keycode and event type to `/dtv/ArcToggle.log`. Does not toggle unless `KEY:` is also given. |
| `PCM` / `DD` / `TOGGLE` | Sets PCM, sets Dolby Digital, or swaps once on a worker thread. `DOLBY` is the same as `DD`. |
| `OFF` | Restores both vtable slots and disarms the hook. Accepts no other word. |

A bad word is reported and the whole injection is ignored. Injecting again updates the armed key and log flag without installing a second hook.

### Test or rebind a button

```sh
. /dtv/SGO.env
samyGOso -n exeAPP -B -r -l /mnt/opt/privateer/usr/libso/libArcToggle.so LOG
# press a button once
cat /dtv/ArcToggle.log
samyGOso -n exeAPP -B -r -l /mnt/opt/privateer/usr/libso/libArcToggle.so KEY:DVR
cat /dtv/ArcToggle.log
```

To test the switch immediately without pressing a button:

```sh
samyGOso -n exeAPP -B -r -l /mnt/opt/privateer/usr/libso/libArcToggle.so TOGGLE
cat /dtv/ArcToggle.log
```

The log is `/dtv/ArcToggle.log`. A line like `PCM -> Dolby set=1 now=Dolby 12ms` confirms the Binder call succeeded.

## Plasma picture tuning

`libPDPTune` checks nine `exeTV` symbols and the `LogicFHD` panel driver before it hooks anything. A different firmware is logged and left unchanged. Each injection is the whole configuration: a word that is left out goes back to stock.

### Installation

Copy `prebuilt/libPDPTune.so` to `/mnt/opt/privateer/usr/libso/` and `init/99_92_PDPTune.init` to `/mnt/etc/init.d/` (`chmod 755`).

The boot script's default is `UNLOCK DRIVE:STANDARD`. The file `/mnt/opt/privateer/usr/libso/libPDPTune.conf` replaces that default. One or more words per line. An empty `STOP_PDPTUNE` or `STOP` in the root of the stick skips the script.

The configuration on this TV is:

```text
UNLOCK DRIVE:STANDARD DEEPBLACK:ON PEAKSTRETCH:ON DEEPBLACK_MODE:ON POWERSAVE:OFF APCSPEED:OFF SAPC:OFF SUBCONTRAST:132 DUTY2:168 OSD_GAMMA:0 GAMMA:2.4
```

### Options

| Option | On this TV | What it does |
|---|---|---|
| `UNLOCK` | **Enabled** | Shows 10-point White Balance, Color Space → Custom, and Expert Pattern. |
| `DRIVE:STANDARD` | **Enabled** | In 2D, Movie and Relax normally use drive mode `0x05` and Dynamic, Standard, and Natural use `0x11` (register `0x806f`). This sends `0x11` wherever the firmware asks for `0x05`. |
| `DEEPBLACK:ON` / `OFF` | **Enabled** (`ON`) | Holds the Black Optimizer Deep Black register `0x809c` at `3` (`OFF` holds `0`). |
| `PEAKSTRETCH:ON` / `OFF` | **Enabled** (`ON`) | Holds the Black Optimizer Peak Stretch register `0x8063` at `1` (`OFF` holds `0`). |
| `DEEPBLACK_MODE:ON` / `OFF` | **Enabled** (`ON`) | Holds `LogicFHD::t_Logic_SetDeepBlackMode` register `0x80bf` at `1` or `0`. Stock firmware only enables `0x80bf` for 50 Hz PAL signals. |
| `POWERSAVE:ON` / `OFF` | **Enabled** (`OFF`) | Holds `LogicFHD::t_Logic_SetPowerSaveMode` register `0x8064` at `1` or `0`. |
| `APCSPEED:ON` / `OFF` | **Enabled** (`OFF`) | Holds `LogicFHD::t_Logic_SetAPCSpeed` register `0x89bf` at `0xc1ff` (fast/on) or `0x41ff` (slow/off). |
| `SAPC:ON` / `OFF` | **Enabled** (`OFF`) | Holds `LogicFHD::t_Logic_SetSAPCTimer` register `0x89ee` at `1` or `0`. `OFF` disables Static APC time-based dimming (on by default in stock Movie mode). |
| `APC_OFFSET:<0..511>` | **Not set** (guard kept) | Overrides `APC_OFFSET` (`0x89c0`). `0` locks full Cell Light 20 and bypasses thermal/Eco/Movie-PWM APC throttling while keeping `DUTY2` boosts active. |
| `SUBCONTRAST:<100..220>` | **132** | Sets Sub Contrast (register `0x8927`, value × 16) apart from the Contrast menu. `132` is factory Standard; `128` is Movie. Values past white-clipping will clip highlights. |
| `DUTY2:<79..255>` | **168**, measured peak | Writes the full 8-bit second panel duty word (`0x89da`, stock formula `79 + final_duty / 21`). `A → B → A` bracketed optical sweep (`NULL` controls `99.6–100.8%`) pinpointed `168` at `+42.8% ± 1.0%` full-field luminance above `128` before fold-back. See [Duty](#duty). |
| `GAMMA:TOE` / `2.3` / `2.4` / `BT1886` | **`2.4`** | Composes a 12-bit curve onto `exeTV`'s gamma LUT and keeps white balance. Not hooked unless one of these words is present. |
| `OSD_GAMMA:<-3..3>` | **0** | Sets the OSD Gamma slider the curve is built on. `0` is the neutral baseline. |
| `CINEMA_SMOOTH:ON` / `OFF` | **Not forced** (native 24p) | Hooks `mbrg_outputsource_setfilmmode` in `libUTOPIA.so` to force MStar X12's 24p → 48 Hz (2:1) FRC table on or off across source/mode changes. Left off by default because this set already has `24Px4 Support = ON` natively for 24 Hz inputs, and forcing Cinema Smooth globally on 60 Hz content can introduce cadence hiccups. |
| `PATTERN:OFF\|BLACK\|WHITE\|RED\|GREEN\|BLUE\|BOX9\|SCROLL` | **Not left on** | Drives LogicFHD hardware test patterns (`0x8114`), including `WHITE` / `BOX9` for optical sweeps and `SCROLL` for burn-in wipe. Reverts to `OFF` (`0x0000`) when omitted or on `OFF`. |
| `DUMP` | **Not left on** | Logs hook state, all managed and diagnostic LogicFHD registers, OSD Gamma, panel temperature, Cinema Smooth state, and live `OutputVFreq`. Changes nothing. |
| `OFF` | **Not in use** | Removes the hooks and restores the firmware's register, Cinema Smooth, and gamma values. |

The firmware writes Deep Black and Peak Stretch as a pair from Black Optimizer: `0`/`0` for Off, `3`/`0` for Dark Room, and `0`/`1` for Bright Room. This set powers on at `3`/`1` and the firmware leaves that pair alone, which is why both overrides are enabled.

### Duty

`DUTY2` is the second duty word the logic board is given while the panel is lit (register `0x89da`). Samsung sends a constant. It is not a percentage of brightness, and it is not the Cell Light control.

| Value | What that maximum means |
|---|---|
| **128** | The highest duty value this firmware sends to any panel type. That is Samsung's ceiling. |
| **139** | Baseline boosted value on this TV, 11 counts above Samsung's ceiling. |
| **255** | The full 8-bit hardware register ceiling (`uxtb` in `t_Logic_SetDutyData`). Not a Samsung rating; step up incrementally while checking panel temperature and image stability. |

A value above `128` is written only while active `APC_OFFSET` (`0x89c0`) is `0`, which is Cell Light 20 with Eco and thermal dimming off — or continuously if `APC_OFFSET:0` is explicitly passed. More light means more power, heat, and wear. Nothing here claims a measured increase in brightness or a known lifetime cost.

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
    UNLOCK DRIVE:STANDARD DEEPBLACK:ON PEAKSTRETCH:ON DEEPBLACK_MODE:ON POWERSAVE:OFF APCSPEED:OFF SAPC:OFF SUBCONTRAST:132 DUTY2:168 OSD_GAMMA:0 GAMMA:2.4 DUMP
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

`make test` runs `libPDPTune` and `libDDCRAM_EDID` against `test/fake_exetv.c` and `libArcToggle` against `test/fake_exeapp.c` under QEMU. It checks arguments, hooks, overrides, gamma tables, vtable interception, and EDID rules. It does not check ARC playback or the picture on a panel.

## Disable or revert

- `STOP_EDID` skips the EDID boot script. `STOP_PDPTUNE` skips the picture boot script. `STOP_ARCTOGGLE` skips the audio-key boot script. `STOP` skips all three.
- `STOP_SAMYGO`, or booting with no stick, starts the TV stock.
- `samyGOso -T -B -r -l /mnt/opt/privateer/usr/libso/libPDPTune.so OFF` removes the picture hooks on a running TV.
- `samyGOso -T -B -r -l /mnt/opt/privateer/usr/libso/libArcToggle.so OFF` removes the audio-key hook on a running TV.
- Without the EDID boot script, the stereo EDIDs return at the next power-on.

## License

MIT. See [LICENSE](LICENSE).
