# Calibration files from the development PN60F5500

Picture files are 312-byte copies of the active picture record.

| File | What it is |
|---|---|
| `PicSettings_HDMI3_2_pre709.bin` | HDMI3 Movie before the Custom preset. MD5 `579966ff4187163f683e6ba3be1cd2a9`. |
| `PicSettings_HDMI3_2_BT709.bin` | Saved after `libCMSApply BT709`. The color-space selector is `3` (OSD Custom). MD5 `4edc7e37c7f68ff0c1c3d6d2eb42f269`. This is the state that was kept. |
| `PicSettings_HDMI3_2_selector4.bin` | The same RGB values with selector `4`. This draft was not the applied state. |
| `PicSettings_HDMI3_2_final.bin` | An earlier dump (07:38 UTC, before the pre-709 and BT.709 saves). Sixteen bytes differ from the pre-709 file. |
| `pic_orig.bin`, `pic_clean.bin` | Snapshots from the day before the Custom preset. |

`edid/DDCRAM_port0.bin` through `edid/DDCRAM_port3.bin` are the stock DDC RAM dumps. `edid/FakeEDIDdump_HDMI1.bin` through `edid/FakeEDIDdump_HDMI4.bin` are those dumps after `scripts/patch_edid_51.py`: audio block `290907071507503d07c0`, speaker block `830f0000`.
