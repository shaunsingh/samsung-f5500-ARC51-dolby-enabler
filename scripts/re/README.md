# Firmware notes

`redis.py`, `emu.py`, `emu_preset.py`, and `q1.py` through `q33.py` were used to read a local copy of `exeTV` while the color model and the audio path were being worked out. They are not part of the build, and `exeTV` is not in this repository. Several of them still refer to the scratch directory they were written in.

`sonos_discover.py` sends one SSDP search for a ZonePlayer. It does not change the TV.
