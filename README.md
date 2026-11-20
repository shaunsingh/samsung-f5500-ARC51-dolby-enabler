# samsung-f5500-ARC51-dolby-enabler

Two [SamyGO](https://forum.samygo.tv/) payloads for 2013 Samsung F-series TVs with the MStar chipset (`T-MST12`), developed on a **PN60F5500AF** plasma running firmware `T-MST12AKUC-2225.0`:

| | What it does | Works on |
|---|---|---|
| **`libDDCRAM_EDID`** | Makes HDMI sources (Xbox, Apple TV, Shield, Blu-ray players) send **Dolby Digital 5.1** and **DTS 5.1**, which the TV then passes to an **HDMI-ARC** or optical soundbar or receiver. No EDID emulator or audio extractor is needed. | F-series `T-MST12` firmware that exports the MStar functions named below |
| **`libPDPTune`** | Picture options for the plasma panel: the hidden **10p White Balance** menu, and control over a few registers of the panel's logic board. | Only the PN60F5500 with exactly `T-MST12AKUC-2225.0`; it leaves any other firmware alone |

Both are injected into the running `exeTV` process by `samyGOso` from a SamyGO USB stick. **Nothing is written to the TV's flash**: without the stick the TV is stock.

```text
src/       libDDCRAM_EDID.c, libPDPTune.c, samygo.h
init/      boot scripts for the stick
scripts/   patch_edid_51.py, which adds 5.1 audio to dumped EDIDs
test/      a stand-in exeTV that the libraries are tested against under qemu
prebuilt/  the two libraries, built from this tree
```

---

## 5.1 audio over HDMI-ARC

### Why the stock TV and `libFakeEDID` do not get there

1. **The TV tells sources it is a stereo device.** Its HDMI EDIDs list only 2-channel LPCM (Audio Data Block `23 09 07 07`) and two speakers (Speaker Allocation Data Block `83 01 00 00`), so a source never offers Dolby Digital or DTS.
2. **`libFakeEDID` changes the wrong copy.** On this chipset it ends in `SdCommon_SetEDID`, which erases and rewrites sectors of the SPI flash (`MDrv_SERFLASH_SectorErase`, `MDrv_SERFLASH_Write`). The HDMI receiver, however, answers a source's EDID read from **on-chip DDC RAM**, and on this TV that RAM still held the stereo EDIDs afterwards, even after pulling the mains plug.
3. **The audio path itself is fine.** With `libHDMIpassthru` enabling bitstream passthrough, the TV forwards a 5.1 bitstream to ARC and optical as soon as a source sends one.

### What `libDDCRAM_EDID` does

It calls three functions that MStar's `libUTOPIA.so` exports inside `exeTV`:

- `MDrv_HDMI_READ_DDCRAM` reads the 256-byte EDID a port currently serves.
- `MDrv_HDMI_PROG_DDCRAM` writes a new one into the DDC RAM. The library reads it back and compares.
- `MApi_XC_Sys_SetHPD` pulls hot-plug detect low for 0.8 s on the ports that changed, so the sources there read the new EDID.

Before writing, it compares, so a port that already holds the wanted EDID is neither rewritten nor pulsed. It refuses any file that is not exactly 256 bytes, lacks the EDID header, or fails either block checksum. The DDC RAM keeps its contents through standby and gets the stereo EDIDs back when the TV has been without mains power, which is why the boot script repeats the load at every start.

Command line words: `DUMP` (the default) saves each port's EDID to `/dtv/DDCRAM_port<n>.bin`; `LOAD` programs the `DDCRAM_port<n>.bin` files that lie next to the library; `HPD` adds the hot-plug pulse. The log is `/dtv/DDCRAM_EDID.log`.

### Prerequisites

- The TV rooted with SamyGO (`SamyGO-F-SM-MST-Fix` on a USB stick), which provides `samyGOso`.
- [`libHDMIpassthru`](https://forum.samygo.tv/) from the SamyGO forum, started with `OPTIONS="AUTO PCM DTS"`.
- A shell on the TV; see [Root shell only on request](#root-shell-only-on-request).

### Installation

The paths below are those of the SamyGO stick as the TV mounts it: `/mnt/opt/privateer/usr/libso/` for libraries and `/mnt/etc/init.d/` for boot scripts.

**1. Copy `prebuilt/libDDCRAM_EDID.so`** to `/mnt/opt/privateer/usr/libso/`.

**2. Dump the EDIDs of your own TV.** They carry its timings and each port's HDMI physical address, so do not use someone else's.

```sh
. /dtv/SGO.env
samyGOso -T -B -l /mnt/opt/privateer/usr/libso/libDDCRAM_EDID.so DUMP
cat /dtv/DDCRAM_EDID.log
```

```text
[DUMP] port=0 valid=1 phys=4.0.0.0 ADB=23090707 SADB=83010000
[DUMP] port=1 valid=1 phys=3.0.0.0 ADB=23090707 SADB=83010000
[DUMP] port=2 valid=1 phys=1.0.0.0 ADB=23090707 SADB=83010000
[DUMP] port=3 valid=1 phys=2.0.0.0 ADB=23090707 SADB=83010000
```

Fetch `/dtv/DDCRAM_port0.bin` to `/dtv/DDCRAM_port3.bin` and keep a copy.

**3. Patch them.**

```sh
python3 scripts/patch_edid_51.py ./dumped ./patched
```

In the CEA-861 extension block of each EDID this

- adds a Dolby Digital descriptor (`15 07 50`: 6 channels, 32/44.1/48 kHz, 640 kbps) and a DTS descriptor (`3d 07 c0`: 6 channels, 32/44.1/48 kHz, 1536 kbps) to the Audio Data Block, and keeps LPCM at 2 channels so that no source tries multichannel LPCM, which ARC cannot carry;
- sets the Speaker Allocation Data Block to 5.1 (`83 0f 00 00`);
- moves the detailed timings up by the 6 bytes this takes, refusing if the block's padding is too short, and recomputes the checksum.

Copy the patched files to `/mnt/opt/privateer/usr/libso/`.

**4. Try it.**

```sh
rm -f /dtv/DDCRAM_EDID.log
samyGOso -T -B -l /mnt/opt/privateer/usr/libso/libDDCRAM_EDID.so LOAD HPD
cat /dtv/DDCRAM_EDID.log
```

```text
[LOAD-OK] port=0 valid=1 phys=4.0.0.0 ADB=290907071507503d07c0 SADB=830f0000
[LOAD-OK] port=1 valid=1 phys=3.0.0.0 ADB=290907071507503d07c0 SADB=830f0000
[LOAD-OK] port=2 valid=1 phys=1.0.0.0 ADB=290907071507503d07c0 SADB=830f0000
[LOAD-OK] port=3 valid=1 phys=2.0.0.0 ADB=290907071507503d07c0 SADB=830f0000
[HPD] port=0 pulsed
[HPD] port=1 pulsed
[HPD] port=2 pulsed
[HPD] port=3 pulsed
```

The source now offers Dolby Digital and DTS (Xbox: *Settings → General → Volume & audio output → HDMI audio*). On the TV, set *Sound → Additional Settings → SPDIF Output → Audio Format* to the bitstream format.

Other lines the log can show: `LOAD-ALREADY-OK` (nothing to do), `LOAD-MISMATCH` (the read-back differs), `LOAD-SKIP` (the file is missing or not a valid EDID).

**5. Make it stick.** Copy `init/99_91_DDCRAM_EDID.init` to `/mnt/etc/init.d/` and `chmod 755` it. An empty file `STOP_EDID` or `STOP` in the root of the stick skips it.

---

## Picture: `libPDPTune`

`libPDPTune` first checks that six symbols of `exeTV` are at the addresses they have in `T-MST12AKUC-2225.0` and that the panel's logic board is driven by the firmware's `LogicFHD` class. If not, it logs that and does nothing.

The words of one injection are the whole configuration. What is not named goes back to stock, so the configuration can be changed while the TV runs.

| Word | Effect |
|---|---|
| `UNLOCK` | Offers **10p White Balance** under *Picture → Advanced Settings* in Movie mode. The firmware has the feature and hides it on this model: `PrdInfoManager::CheckSupportOption(0x4a)` answers no. |
| `DRIVE:STANDARD` | In 2D, the firmware gives the logic board driving mode `0x05` for the Movie and Relax picture modes and `0x11` for Dynamic, Standard and Natural (register `0x806f`). This sends `0x11` wherever the firmware asks for `0x05`. Nothing else about Movie mode changes. |
| `DEEPBLACK:ON` / `OFF` | Holds the logic board's DeepBlack register (`0x809c`) at `3` or at `0`. |
| `PEAKSTRETCH:ON` / `OFF` | Holds its PeakStretch register (`0x8063`) at `1` or at `0`. |
| `DUTY2:<79..160>` | Replaces the constant `128` this panel gets in register `0x89da` whenever it is lit. |
| `SUBCONTRAST:<100..160>` | Replaces the factory *Sub Contrast* (`128` here; register `0x8927` holds it times 16). |
| `OFF` | Nothing: the registers get the firmware's values back and the hooks are removed. |
| `DUMP` | Logs the registers involved and the panel temperature. Changes nothing and can be combined with the others. |

The boot script's default is `UNLOCK DRIVE:STANDARD`.

**What is known and what is not.** The register addresses, the values the firmware sends and when it sends them were read from the disassembly of `exeTV`. What the logic board does with them is not documented, and nothing here has been measured with a meter. In particular:

- The firmware's *Black Optimizer* code writes DeepBlack and PeakStretch as a pair: `0` and `0` for Off, `3` and `0` for Dark Room, `0` and `1` for Bright Room. On the test set, though, the logic board comes up with DeepBlack `3` and PeakStretch `1` at every start and the firmware leaves them there. `ON` is therefore what this set already does, and `OFF` is the experiment.
- `128` is the highest `duty_2` the firmware sends to any panel type, and `128` is the factory *Sub Contrast*. Higher values are outside what Samsung shipped.

Treat `DEEPBLACK`, `PEAKSTRETCH`, `DUTY2` and `SUBCONTRAST` as experiments: change one at a time, compare by eye or with a meter, and keep what holds up. More light means more power, heat and wear on a plasma panel. None of this outlasts a restart of the TV.

The library does not touch the video processor: no gamma tables, no sharpening, no colour processing.

### Installation

Copy `prebuilt/libPDPTune.so` to `/mnt/opt/privateer/usr/libso/` and `init/99_92_PDPTune.init` to `/mnt/etc/init.d/` (`chmod 755`).

To boot with other options than the default, put them in `/mnt/opt/privateer/usr/libso/libPDPTune.conf`:

```text
# one or more words per line
UNLOCK DRIVE:STANDARD
DEEPBLACK:ON
```

An empty file `STOP_PDPTUNE` or `STOP` in the root of the stick skips the script.

To change the configuration on the running TV:

```sh
. /dtv/SGO.env
samyGOso -T -B -r -l /mnt/opt/privateer/usr/libso/libPDPTune.so UNLOCK DRIVE:STANDARD DEEPBLACK:ON DUMP
cat /dtv/PDPTune.log
```

```text
libPDPTune 2.0.0: UNLOCK DRIVE:STANDARD DEEPBLACK:ON DUMP
LogicFHD::m_Logic_WriteI2C hooked
DrivingMode 0x806f: firmware 0x0005, panel 0x0011
DeepBlack   0x809c: firmware 0x0003, panel 0x0003
PeakStretch 0x8063: firmware 0x0001, panel 0x0001
duty_2      0x89da: firmware 0x0080, panel 0x0080
SubContrast 0x8927: firmware 0x0800, panel 0x0800
PrdInfoManager::CheckSupportOption hooked
dump LogicFHD::m_Logic_WriteI2C is hooked
dump PrdInfoManager::CheckSupportOption is hooked
dump DrivingMode 0x806f = 0x0111
dump DeepBlack   0x809c = 0x0003
dump PeakStretch 0x8063 = 0x0001
dump duty_2      0x89da = 0x0080
dump SubContrast 0x8927 = 0x0800
dump APC offset  0x89c0 = 0x0000
dump SubBright   0x8926 = 0x0450
dump panel temperature 43 C
done
```

The first block gives, per register, what the firmware last asked for (or, before it has asked, what the logic board held) and what the logic board has now. The `dump` lines are raw read-backs; the logic board reports status in the high byte of DrivingMode. A bad word is reported and the whole injection is ignored.

### How it works

- **Finding things.** `exeTV` is not position independent and exports its C++ symbols, so the library looks each one up with `dlsym` and requires the address it expects. The logic board object comes from the exported `TDcLogic::m_poLogicIC`, and its vtable pointer must be that of `LogicFHD`.
- **Hooks.** The first instruction of `LogicFHD::m_Logic_WriteI2C` and of `PrdInfoManager::CheckSupportOption` is replaced by a branch. That is one aligned word, so a thread entering the function sees either the old instruction or the new one, and hooks can be set and removed while `exeTV` runs. A branch reaches 32 MiB and libraries are mapped much further away, so it lands on a small stub in a page mapped into the gap between the segments of `exeTV`, which jumps on to the library. Before patching or restoring, the library checks that the word it is about to replace is the one it expects.
- **Overrides.** The firmware rewrites these registers whenever the picture mode, *Black Optimizer* or *Cell Light* changes, so the override sits in the write path. The library remembers what the firmware last asked for, which is what it restores when an override is dropped.
- **Threads.** `samyGOso` borrows a thread of `exeTV` wherever that thread happens to be, possibly inside a firmware lock. The library therefore does its work on a thread of its own.

---

## Root shell only on request

A stock SamyGO stick leaves root FTP (21), telnet (23) and a raw root shell (4023) open to the network without a password. With `init/run1.sh` (for `SamyGO_data/run1.sh` on the stick) and `init/01_02_telnet.init` (for `/mnt/etc/init.d/`) they come up only while the root of the stick holds an empty file `ENABLE_SHELL`. Create the file on a computer, boot the TV with the stick, do the work, and delete the file again.

---

## Building

The TV runs Linux 3.0.33 on ARMv7 with the soft-float ABI and glibc 2.14.1. Any `arm-linux-gnueabi` GCC will do:

```sh
sudo apt install gcc-arm-linux-gnueabi qemu-user shellcheck
make            # build/libDDCRAM_EDID.so, build/libPDPTune.so
make test       # run both against test/fake_exetv.c under qemu-arm
make lint       # shellcheck the boot scripts
make prebuilt   # copy the libraries to prebuilt/
```

The libraries are linked without naming any library of their own; inside `exeTV` their imports bind to the libc already loaded there. The build fails if a library imports a symbol that is not on the list in the `Makefile`, all of which the TV's libc has. This catches the redirections newer libc headers apply, such as `strtol` to `__isoc23_strtol`.

`test/fake_exetv.c` puts stand-ins for the firmware's functions and objects at the addresses they have in the real `exeTV`, with the real first instructions, and loads the libraries the way `samyGOso` does. It checks argument handling, hooking and unhooking under load, that overrides follow the firmware and are undone, and the EDID rules.

---

## Reverting

- `STOP_PDPTUNE`, `STOP_EDID` or `STOP` in the root of the stick: the respective boot script does nothing.
- `STOP_SAMYGO` there, or no stick: the TV boots stock.
- On the running TV, `samyGOso -T -B -r -l …/libPDPTune.so OFF` restores the registers and removes the hooks. The EDIDs return to stock at the next power-on without the boot script.

## License

MIT, see [LICENSE](LICENSE).
