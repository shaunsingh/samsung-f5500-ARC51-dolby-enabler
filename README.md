# samsung-f5500-ARC51-dolby-enabler

Enable **Dolby Digital 5.1 (AC-3)** and **DTS 5.1** passthrough from HDMI sources (Xbox, Apple TV, Shield, consoles, Blu-ray players) to an **HDMI-ARC** (or optical) soundbar/AVR on **2013 Samsung F-series MStar (`T-MST12`) TVs** (tested on **PN60F5500AF**, firmware `T-MST12AKUC-2225.0`, feeding a Sonos Beam Gen 1 over HDMI-ARC)—**100% in software on the TV + SamyGO USB stick, with no external EDID emulator or audio extractor.**

---

## Why stock F-series TVs and `libFakeEDID` fail

1. **Stock firmware restricts HDMI EDID to 2.0 Stereo PCM:**
   Out of the box, Samsung F-series TVs advertise only 2-channel LPCM (`ADB = 23 09 07 07`) and a 2.0 Speaker Allocation block (`SADB = 83 01 00 00`), so sources like Xbox refuse to offer Dolby Digital or DTS over HDMI.
2. **Why SamyGO's `libFakeEDID` never worked on F-MST (`T-MST12`):**
   Disassembly of `exeTV`, `libSDAL.so`, and `libUTOPIA.so` from `T-MST12AKUC` shows:
   - `libFakeEDID` calls `TDsEepromStore::t_WriteDDC` → `SdCommon_SetEDID`, which erases and writes **SPI serial flash** sectors `0x10000`–`0x14000` (`MDrv_SERFLASH_Write`).
   - However, the MStar MST12 HDMI receiver serves EDIDs to connected sources from **on-chip internal HDMI DDCRAM**, not directly from those SPI flash sectors. Even after a full AC wall-plug power cycle, live hardware DDCRAM still holds the stock 2.0 EDID.
3. **Why HDMI-ARC 5.1 works on F-MST once the source sends 5.1:**
   Disassembly confirms:
   - `TDsPrimeHdmiSwitch::SetAudioReturnChannel` → `VenderAudio_ArcControl` → `MApi_AUDIO_EnableOutput(7, enable)` → `HAL_AUDIO_ARC_HWEN` simply muxes the **MStar SPDIF hardware output stream** onto the HDMI-ARC pin.
   - `libHDMIpassthru` (`VenderAudio_SetHDMI_AC3_Support(1)`) sets `g_EnableHdmiAc3Decode = 1` in `libSDAL.so` (used by `AUDIO_task` and `SdSound_Connect`), unlocking Dolby Digital 5.1 and DTS 5.1 passthrough for **both** optical and HDMI-ARC.

---

## How `libDDCRAM_EDID` fixes it

`libDDCRAM_EDID.so` is an ARMv7 soft-float `samyGOso` payload that calls MStar's exported `libUTOPIA.so` driver functions directly inside `exeTV`:

- **`MDrv_HDMI_READ_DDCRAM(XC_DDCRAM_PROG_INFO *info, 0)`** — reads the live 256-byte on-chip DDCRAM for HDMI ports `0..3`.
- **`MDrv_HDMI_PROG_DDCRAM(XC_DDCRAM_PROG_INFO *info, 0)`** — writes a validated 256-byte 5.1 EDID directly into live on-chip DDCRAM and verifies byte-for-byte on read-back.
- **`MApi_XC_Sys_SetHPD(enable, 0x50 + port)`** — pulses Hot-Plug Detect (800 ms low → high) **only** for ports whose DDCRAM actually changed, so connected devices re-read the new 5.1 EDID immediately on cold boot with zero flicker on subsequent runs.
- **Zero internal flash writes:** Stock SPI flash stays untouched. Removing the USB stick (or placing `STOP_EDID` / `STOP` on USB root) reverts everything cleanly.

```c
typedef struct {
    unsigned char *pu8EDID; // pointer to 256-byte EDID
    unsigned short u16Size; // 256
    unsigned int   ePort;   // 0..3 (on PN60F5500: 0=HDMI1/4.0.0.0, 1=HDMI2/3.0.0.0, 2=HDMI3/1.0.0.0, 3=HDMI4/2.0.0.0)
} XC_DDCRAM_PROG_INFO;
```

---

## Prerequisites

- A rooted Samsung F-series MStar (`T-MST12`) TV running **SamyGO** (`SamyGO-F-SM-MST-Fix` on USB) with **`samyGOso`** installed.
- **`libHDMIpassthru`** (`99_90_HDMIpassthru.init` with `OPTIONS="AUTO PCM DTS"`) installed from the SamyGO forums to unlock `VenderAudio_SetHDMI_AC3_Support(1)`.
- Optional: disable OTN firmware updates in the Service Menu (or via SamyGO `libFactory`).

---

## Installation

### 1. Copy `libDDCRAM_EDID.so` to the TV

Copy `prebuilt/libDDCRAM_EDID.so` (or your own build from `make`) onto the TV at:

```text
/mnt/opt/privateer/usr/libso/libDDCRAM_EDID.so
```

### 2. Dump your TV's native on-chip DDCRAM EDIDs

Always patch **your own TV's** dumped EDIDs so your native panel timings, monitor name, and per-port CEC/HDMI physical addresses stay intact:

```sh
rm -f /dtv/DDCRAM_EDID.log /dtv/DDCRAM_port*.bin
samyGOso -T -B -l /mnt/opt/privateer/usr/libso/libDDCRAM_EDID.so DUMP
cat /dtv/DDCRAM_EDID.log
```

Expected log output (stock 2.0 EDID):

```text
[DUMP] port=0 valid=1 phys=4.0.0.0 hdr=00ffffff ADB=23090707 SADB=83010000
[DUMP] port=1 valid=1 phys=3.0.0.0 hdr=00ffffff ADB=23090707 SADB=83010000
[DUMP] port=2 valid=1 phys=1.0.0.0 hdr=00ffffff ADB=23090707 SADB=83010000
[DUMP] port=3 valid=1 phys=2.0.0.0 hdr=00ffffff ADB=23090707 SADB=83010000
```

Download `/dtv/DDCRAM_port0.bin` through `/dtv/DDCRAM_port3.bin` to your computer and keep a backup copy.

### 3. Patch the dumped EDIDs for 5.1 surround

Run the included CEA-861 patcher on your dumped files:

```sh
python3 scripts/patch_edid_51.py ./stock_ddcram ./patched_ddcram
```

This surgically modifies each 256-byte EDID to:
- Append **AC-3 (Dolby Digital) 5.1** (`15 07 50`: 6ch, 32/44.1/48 kHz, up to 640 kbps)
- Append **DTS 5.1** (`3d 07 c0`: 6ch, 32/44.1/48 kHz, up to 1536 kbps)
- Keep **LPCM 2.0** (`09 07 07`) so sources do not attempt 5.1 uncompressed LPCM over ARC/optical
- Update **Speaker Allocation Data Block** from `83 01 00 00` (2.0) to `83 0f 00 00` (5.1: FL/FR + LFE + FC + RL/RR)
- Shift the CEA-861 DTD offset by +6 bytes without losing any active Detailed Timing Descriptors, and recalculate the Block 1 checksum.

Upload the patched `DDCRAM_port0.bin` .. `DDCRAM_port3.bin` to:

```text
/mnt/opt/privateer/usr/libso/DDCRAM_port0.bin
/mnt/opt/privateer/usr/libso/DDCRAM_port1.bin
/mnt/opt/privateer/usr/libso/DDCRAM_port2.bin
/mnt/opt/privateer/usr/libso/DDCRAM_port3.bin
```

### 4. Test live injection + HPD pulse

```sh
rm -f /dtv/DDCRAM_EDID.log
samyGOso -T -B -l /mnt/opt/privateer/usr/libso/libDDCRAM_EDID.so LOAD HPD
cat /dtv/DDCRAM_EDID.log
```

Expected output:

```text
[LOAD-OK] port=0 valid=1 phys=4.0.0.0 hdr=00ffffff ADB=290907071507503d07c0 SADB=830f0000
[LOAD-OK] port=1 valid=1 phys=3.0.0.0 hdr=00ffffff ADB=290907071507503d07c0 SADB=830f0000
[LOAD-OK] port=2 valid=1 phys=1.0.0.0 hdr=00ffffff ADB=290907071507503d07c0 SADB=830f0000
[LOAD-OK] port=3 valid=1 phys=2.0.0.0 hdr=00ffffff ADB=290907071507503d07c0 SADB=830f0000
[HPD] pulsing port=0 (0x50) LOW
...
[HPD] pulsing port=3 (0x53) HIGH
```

Your HDMI source (e.g., Xbox **Settings → General → Volume & audio output → HDMI audio**) will immediately offer **Dolby Digital** and **DTS Digital Surround**. On the TV, make sure **Sound → Additional Settings → SPDIF Output → Audio Format** is set to **Dolby Digital** (or **DTS**; `libHDMIpassthru` `AUTO` handles switching automatically).

### 5. Persist on every TV boot

Copy `init/99_91_DDCRAM_EDID.init` to `/mnt/etc/init.d/99_91_DDCRAM_EDID.init` and mark it executable:

```sh
chmod 755 /mnt/etc/init.d/99_91_DDCRAM_EDID.init
```

Because `libDDCRAM_EDID.so` checks existing DDCRAM contents before writing, it only programs and pulses HPD when a port's DDCRAM does not already match (e.g., after a cold boot).

---

## Optional: Lock down root ports behind a USB `ENABLE_SHELL` switch

By default, SamyGO leaves unauthenticated root FTP (`:21`), Telnet (`:23`), and netcat (`:4023`) open on the LAN.

The templates in `init/run1.sh` (for `SamyGO_data/run1.sh` on USB) and `init/01_02_telnet.init` (for `/mnt/etc/init.d/01_02_telnet.init`) keep `SRS_module.ko` loaded (for `mount --bind`) while closing ports `21`, `23`, and `4023` during normal use. Whenever you need shell/FTP access again, plug the USB stick into any computer, create an empty file named `ENABLE_SHELL` in the USB root directory, and reboot the TV.

---

## Building from source

The TV runs Linux 3.0.33 (`armv7l`, soft-float EABI, `glibc 2.14.1`). To guarantee only `GLIBC_2.4` symbols are referenced when compiling with a modern cross-compiler (`arm-linux-gnueabi-gcc`), copy three libraries from your TV's `/mtd_uncompexe/lib/` into `./syslib/`:

- `/mtd_uncompexe/lib/libc-2.14.1.so` → `syslib/libc.so.6`
- `/mtd_uncompexe/lib/libdl-2.14.1.so` → `syslib/libdl.so.2`
- `/mtd_uncompexe/lib/libgcc_s.so.1` → `syslib/libgcc_s.so.1`

Then run:

```sh
make
readelf -V build/libDDCRAM_EDID.so   # verify only GLIBC_2.4 is required
```

---

## Safety & Revert

- Create an empty `STOP_EDID` file on the USB stick root to skip `99_91_DDCRAM_EDID.init`.
- Create an empty `STOP_SAMYGO` file on the USB stick root (or simply unplug the USB stick) to boot the TV 100% stock.
- `libDDCRAM_EDID.so` refuses to program any EDID file that is not exactly 256 bytes, lacks the `00 ff ff ff ff ff ff 00` header, or fails either 128-byte checksum.

## License

MIT — see [LICENSE](LICENSE).
