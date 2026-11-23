# Calibration files from the development PN60F5500

Picture files are 312-byte copies of the active picture record saved with SamyGO `libBackPicSet`.

| File | What it is |
|---|---|
| `PicSettings_HDMI3_2_pre709.bin` | HDMI3 Movie before the Custom preset. MD5 `579966ff4187163f683e6ba3be1cd2a9`. |
| `PicSettings_HDMI3_2_BT709.bin` | Saved after `libCMSApply BT709`. Color-space selector `3` (OSD Custom). MD5 `4edc7e37c7f68ff0c1c3d6d2eb42f269`. |

`edid/DDCRAM_port0.bin` through `edid/DDCRAM_port3.bin` are the stock DDC RAM dumps. `edid/FakeEDIDdump_HDMI1.bin` through `edid/FakeEDIDdump_HDMI4.bin` are those dumps after `scripts/patch_edid_51.py` (audio block `290907071507503d07c0`, speaker block `830f0000`).
