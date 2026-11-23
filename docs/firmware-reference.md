# Firmware Reference

These notes apply to the development **PN60F5500AF** running **T-MST12AKUC-2225.0**. They are implementation references, not installation instructions.

## HDMI EDID handling

`libDDCRAM_EDID` uses MStar functions exported by `libUTOPIA.so`:

| Function | Purpose |
|---|---|
| `MDrv_HDMI_READ_DDCRAM` | Read the 256-byte EDID currently served by an HDMI input. |
| `MDrv_HDMI_PROG_DDCRAM` | Program that EDID into the receiver's DDC RAM. |
| `MApi_XC_Sys_SetHPD` | Pulse hot-plug detect so the source reads the changed EDID. |

The library verifies every write by reading it back. It pulses HPD for 0.8 seconds only on changed ports and rejects malformed EDIDs or checksums. DDC RAM survives standby but is repopulated with stock EDIDs after mains power is removed, so the boot script reapplies the patch.

`libFakeEDID` is not equivalent on this chipset. Its path reaches `SdCommon_SetEDID` and SPI-flash writes, while the HDMI receiver answers sources from DDC RAM.

## Custom color-space model

`PrimePictureShare::m_PrimePicture_Set_ColorMapping` programs a 27-node RGB cube with three output channels, for 81 XC bank `0x14` words.

- Neutral is `512`.
- Near neutral, an own-channel slider moves a word by 10 counts per slider unit; an off-channel slider moves it by 5.
- The six 2D Custom calls use factory IDs `0x2b0`, `0x2b3`, `0x2b6`, `0x2b9`, `0x2bc`, and `0x2bf`.
- The corresponding 3D IDs are `0x3a9` through `0x3b8`.
- Calls are stateful because some later nodes average values written by earlier calls.
- `TPTRGB` is three packed `uint8_t` values, not three integers.

The selector values differ by layer:

```text
TCScreenImpl / OSD Custom = 3  ->  TD Custom = 2  ->  PrimePicture Custom = 4
```

On this firmware, `TCScreenImpl::SetColorSpace(..., 3, 0)` selects Custom consistently. Seek `1` can write the Custom LUT while leaving the OSD selector on Native.

### Factory color tables

Values read through `DBFactoryBase::GetUniqueItemData`:

| Color | Auto RGB (`0x2b0`–`0x2c1`) | Native RGB (`0x2c2`–`0x2e5`) |
|---|---|---|
| Red | 59, 29, 30 | 50, 0, 0 |
| Green | 86, 54, 28 | 0, 60, 5 |
| Blue | 13, 29, 53 | 0, 0, 50 |
| Yellow | 50, 53, 39 | 52, 48, 0 |
| Cyan | 85, 48, 52 | 0, 48, 52 |
| Magenta | 44, 39, 52 | 50, 0, 50 |

Native B and C tables are identical. Both `Auto` (`0x2b0`–`0x2c1`) and `Custom` (`0x3a9`–`0x3b8`) disable WCE (`0x280 = 0`) and branch into the identical Bank `0x14` 3D LUT path (`on = 10*(x-50) + 512`, `off = 5*x + 512`); only `Native` (`0x2c2`–`0x2e5`) enables WCE.

**Hardware LUT scale audit:** `scripts/gamut/model.py` reproduces all 81 integer Bank `0x14` register writes, but assumed full scale `FS = 500` (`w_on = x/50`, `w_off = x/100`). Checking Samsung's own factory `Auto` table (`G = [86, 54, 28]`, `C = [85, 48, 52]`) disproves `FS = 500`—under `FS = 500`, `Auto` would map Green to yellow-brown `(0.40, 0.51)` and Cyan to gray `(0.28, 0.28)`. Fitting `Auto` against published PN60F5500 primaries puts Bank `0x14`'s physical full scale near `FS ≈ 1024–1536` (~2–3× finer per slider unit). For strict Rec.709 without a meter, Samsung's factory **`Auto`** table is closer than the legacy `BT709` preset; to keep near-native phosphor saturation without WCE distortion, `libCMSApply WIDE` (`R:47,0,20  G:55,55,0  B:12,0,50  Y:47,55,0  C:25,52,52  M:44,0,52`) preserves ~103.6% saturation with zero third-channel bleed while aligning all six hue angles to `~1.4°` across `FS = 1024–1536`.

## Plasma registers

| Register | Meaning and observed values |
|---|---|
| `0x806f` | 2D drive mode: `0x05` for Movie/Relax, `0x11` for Dynamic/Standard/Natural. |
| `0x809c` | Black Optimizer DeepBlack: `0` or `3` in firmware paths. |
| `0x8063` | Black Optimizer PeakStretch: `0` or `1` in firmware paths. |
| `0x80bf` | `LogicFHD::t_Logic_SetDeepBlackMode(bool)`: `0` or `1` (enabled only for 50 Hz PAL in stock firmware). |
| `0x8064` | `LogicFHD::t_Logic_SetPowerSaveMode(bool)`: `1` (on) or `0`/`3` (off). |
| `0x89bf` | `LogicFHD::t_Logic_SetAPCSpeed(bool)`: `0xc1ff` (on/fast) or `0x41ff` (off/slow). |
| `0x89c0` | APC offset (`0..511`, `(1024 - final_duty) >> 1`). Duty boosts above `128` apply only while active APC offset is `0` (either naturally or via `APC_OFFSET:0`). |
| `0x89ee` | `LogicFHD::t_Logic_SetSAPCTimer(bool)`: `1` (on) or `0` (off, disables Static APC time-based dimming). |
| `0x89da` | Second duty value (8-bit `0..255` via `uxtb` in `t_Logic_SetDutyData`). Computed by `PDPDimmingPanel::m_SetFinalDutyPDP` as `79 + final_duty / 21` (`final_duty` in `0..1024`), so `128` is Samsung's stock ceiling. |
| `0x8921`–`0x8926` | `LogicFHD::t_Logic_SetWhiteBalance` (6 words: R/G/B gain and offset). |
| `0x8927` | Sub Contrast, stored as the service-menu value times 16 (`val << 4`). Movie uses `128`; Standard uses `132`. |
| `0x8114` | LogicFHD hardware test pattern (`t_Logic_SetTestPattern`): `0x0000` off, `0x0100` black, `0x0101` full white, `0x0102`–`0x0104` R/G/B, `0x0106` 9-point white boxes, `0x011f` moving scroll bar. |

## 24p Cinema Smooth (`libUTOPIA.so` FRC table)

`mbrg_outputsource_setfilmmode(uint8_t)` (`0x42b72ba0` in `libUTOPIA.so`) controls the MStar X12 frame-rate conversion table (`0x42cf7550`, 5 entries). Its default entry for `16.0–24.5 Hz` is `0x21` (2:1 → 48 Hz, doubled to 96 Hz by the plasma driver), while `49.0+ Hz` is `0x11` (1:1 passthrough). When `setfilmmode(0)` is called (the F5500 stock state), entry 1 is overwritten to `0x52` (5:2 → 60 Hz 3:2 pulldown). Calling `mbrg_outputsource_setfilmmode(1)` restores 2:1 48 Hz output for 24p sources without altering 50/60 Hz timings. Live output frequency is read via `MApi_XC_GetOutputVFreqX100`.

Samsung's Black Optimizer writes DeepBlack and PeakStretch as `0/0` for Off, `3/0` for Dark Room, and `0/1` for Bright Room. The development panel powers on with `3/1`; its `BLACK_OPTIMIZER = 3` NVRAM state skips the normal writes. `DEEPBLACK:ON` and `PEAKSTRETCH:ON` therefore preserve this set's observed startup state. Their exact visual behavior is undocumented.

The duty cap of `255` in `libPDPTune` is the full 8-bit hardware register ceiling, not a Samsung-supported operating point. Values above `128` are experimental and are inhibited whenever active `APC_OFFSET` (`0x89c0`) is non-zero (unless `APC_OFFSET:0` is explicitly passed).

## Gamma implementation

`GAMMA:*` hooks `MDrv_PNL_SetGammaTbl` in `libUTOPIA.so` and composes a 12-bit curve onto the firmware LUT. The call path is:

```text
PrimePictureShare::m_PrimePicture_SetGamma / SetGamma10PWB
  -> SdDisplay_SetPictureQuality
  -> g_IPanel->SetGammaTbl
  -> MDrv_PNL_SetGammaTbl
  -> Hal_PNL_Set12BitGammaPerChannel
```

The hook preserves 2-point and 10-point white balance, keeps black at zero and peak unclipped, and initializes the 257th guard entry left unset by `SdDisplay_SetPictureQuality`. On first activation it replays the real firmware LUT through `SsScreenBase::SetGammaMode`.

At OSD Gamma `0`, the observed firmware LUT is approximately linear. Curve selection assumes approximately 2.2 native panel response above the black floor. `2.4` is the zero-black limit of BT.1886; `TOE` changes only the low end. These assumptions require optical verification.

## SPDIF / ARC audio format and the remote key

`libArcToggle` targets `exeAPP` directly (not `exeTV`).

Both remote paths converge on `CRemoteControlInput::ProcessRemoteControlEvent(unsigned long type, uint16_t keycode, RemoteControlParam *param)` (`0x01ed2720`) in `exeAPP`:

- **Bluetooth Smart Touch remote:** `bsa_server` → `BluetoothInputDev::SendRemoteControlInputEvent` (`0x012eec48`) → `CInputManager::GetRemoteControlInput(id)->ProcessRemoteControlEvent(...)`
- **IR remote:** MICOM UART (`0x10` packet, NOT `0x50`) → `exeTV` `KeyInputBase` → `KeyCommon::SendKeyPressInput` → vdbinder IPC → `exeAPP` `APPRemoteControlInputNative::SendRemoteControlPressKey` (`0x01cdebb8`) / `ReleaseKey` (`0x01cdf05c`) → `IRemoteControlInput::ProcessRemoteControlEvent(...)`

All callers invoke `ProcessRemoteControlEvent` virtually through `_ZTV19CRemoteControlInput` (`0x04996838`), either via the primary slot (`+0x30`, `0x04996868` → `0x01ed2720`) or the `IRemoteControlInput` subobject thunk slot (`+0x44`, `0x0499687c` → `0x01ed2718`, `sub r0, r0, #20; b 0x01ed2720`). Replacing those two vtable pointers atomically requires no code patching and avoids ARM's ±32 MiB branch-range limit across `exeAPP`'s 121 MiB `.text` segment.

`KEY_DVR` is Samsung keycode `0x0072` (`114`). When unhandled, `SMInput::CallRemoteControlEventDelegator` delivers it to `CTvApplicationManager::m_OnKeyDirectMBR` (`0x02274e6c`), which checks `0x72`, `0x10f`, `0x112`, and `0x115` and launches `APP_MBR` (`0x25`, `0x534`) displaying *"This button will only work after the universal remote control has been set up. Do you want to set it up now?"* (string `0x05f1076c` in `g_MBRWStringEngUS`). Swallowing `0x72` press, repeat, and release inside `ProcessRemoteControlEvent` prevents `m_OnKeyDirectMBR` from ever running.

Audio format switching uses `exeAPP`'s own `TCSpeakerProxy::GetInstance(0)` (`0x0111d4e8`), `GetExternalOutMode` (`0x0111bd30`), and `SetExternalOutMode` (`0x011185e0`) over vdbinder to `exeTV`'s `TCSpeakerImpl::SetExternalOutMode` → `ExternalOutControl::SetAudioMode` with query `0` (`0` = PCM, `1` = Dolby Digital, `2` = DTS, `3` = DTS Neo:6).

*(Earlier attempts hooked `TDcPrimeMicomInterface::t_ParsePacket` in `exeTV` on packet byte `0x50` and patched `RVUCKey::CRVUCNotAvailableKey::ShowNAPopUpMessage` in `exeAPP`. In `t_ParsePacket`, `0x50` is a generic MICOM ACK/status frame that arrives periodically while idle, and `RVUCKey` belongs to the DirecTV RVU client rather than MBR universal remote control.)*

## Audio capture and content-aware routing

`libSDAL.so` exports `SdSound_InstallCallbackPCMCapture` and related capture functions. `AU_PcmCapture_Init` configures two 16-bit capture channels and two `0x1000`-byte buffers. `MApi_AUDIO_SST_GetLevel` likewise exposes left/right levels, not six decoded channels.

The firmware contains AC-3 decoder and encoder support, but the exposed capture and level interfaces do not provide per-channel activity for a 5.1 stream. Consequently, they cannot distinguish stereo material carried in a DD 5.1 container from genuine surround content.

The Beam Gen 1 `HTControl` UPnP service exposes IR, LED, and TOSLink controls only. It has no input-codec or stereo-upmix override.

## Probe safety

Do not use `MApi_XC_Get_Pixel_RGB` for live color readback on this set. That probe crashed `exeTV` and rebooted the TV during development. Factory-table and XC-register reads were the safe validation path.

## BT.709 source measurement

The chromaticity model uses a 2014 i1D2/HCFR measurement of another PN60F5500:

| Primary | x | y | Y |
|---|---:|---:|---:|
| Red | 0.6481 | 0.3402 | 31.37 |
| Green | 0.2839 | 0.6194 | 93.78 |
| Blue | 0.1453 | 0.0635 | 9.03 |

Measured white XYZ was `117.357, 122.189, 131.293`. The old grayscale was modelled as approximately gamma 2.2 and recalculated for the current gamma-2.4 pipeline. The gamma-encoded LUT interpretation is a modelling decision. Native secondaries were excluded from the gamut-boundary estimate because plasma brightness limiting distorts them.
