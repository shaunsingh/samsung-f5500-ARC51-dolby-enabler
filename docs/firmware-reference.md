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

Native B and C tables are identical. The transfer model reproduced all 81 Native words exactly before it was used to generate the BT.709 Custom preset.

## Plasma registers

| Register | Meaning and observed values |
|---|---|
| `0x806f` | 2D drive mode: `0x05` for Movie/Relax, `0x11` for Dynamic/Standard/Natural. |
| `0x809c` | DeepBlack: `0` or `3` in firmware paths. |
| `0x8063` | PeakStretch: `0` or `1` in firmware paths. |
| `0x89da` | Second duty value. `128` is the highest value Samsung sends to any panel type. |
| `0x89c0` | APC offset. Duty boosts above `128` are allowed only while this is zero. |
| `0x8927` | Sub Contrast, stored as the service-menu value times 16. Movie uses `128`; Standard uses `132`. |

Samsung's Black Optimizer writes DeepBlack and PeakStretch as `0/0` for Off, `3/0` for Dark Room, and `0/1` for Bright Room. The development panel powers on with `3/1`; its `BLACK_OPTIMIZER = 3` NVRAM state skips the normal writes. `DEEPBLACK:ON` and `PEAKSTRETCH:ON` therefore preserve this set's observed startup state. Their exact visual behavior is undocumented.

The duty cap of `160` in `libPDPTune` is an input limit, not a Samsung-supported operating point. Values above `128` are experimental and are inhibited whenever APC is active.

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

## SPDIF audio format and the remote key

`libArcToggle` uses the menu setter, not a speaker-object call. `TCSpeakerImpl::SetExternalOutMode` (`_ZN13TCSpeakerImpl18SetExternalOutModeEii`, `0x00af9a1c`) ignores its receiver, takes `TCExternalOutInfo` for output 0, and calls virtual `+0x10`. The read is `GetExternalOutMode` at `0x00af9a74`. `TPCExternalOut::EAudioType` is 0 PCM, 1 Dolby Digital, 2 DTS, 3 DTS Neo:6.

That path reaches `ExternalOutControl::SetAudioMode` (`0x008c0e98`). The third argument is `TPCTv::EQueryType`. Query 0 programs the processor. Any other query returns without writing the hardware, so the hotkey passes 0.

`SpeakerControl::SetSPDIFAudioFormat` (`0x0096aa44`) is a different object. `TPCSpeaker::Instance` returns a 0x14-byte `TPASpeaker`, whose vtable has delay and support flags and does not have that function. Calling it there would crash `exeTV`. The payload does not.

`KeyInputBase::t_ProcessKeyValue` (`0x009038bc`), `KeyCommon::SendKeyPressInput` (`0x009004b0`), and `CInputHandler::t_OnEvent` (`0x016a75ec`) are not called when this remote is pressed. `t_ProcessNormalBuf` carries the brightness sensor (`da 14`, `da 15`, `da 16`). The remote command is the first byte of the buffer passed to `TDcPrimeMicomInterface::t_ParsePacket` (`0x011106b4`). On this set the DVR button is command `0x50`. That command also arrives while the button is idle. An armed toggle therefore changes the format during playback, with no press. Swallowing `t_ParsePacket` does not stop the universal-remote setup dialog; that text is drawn in `exeAPP`.

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
