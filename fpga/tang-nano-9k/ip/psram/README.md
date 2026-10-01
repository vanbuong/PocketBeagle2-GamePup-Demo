# Gowin PSRAM Memory Interface HS IP goes here

The Gowin IP is encrypted and licensed with Gowin EDA, so it is not in this repo.
Generate it once and copy the output into this directory (`ip/psram/`):

1. Gowin EDA -> Tools -> IP Core Generator -> *PSRAM Memory Interface HS* (version 1,
   single channel), device GW1NR-LV9QN88PC6/I5.
2. Settings: **burst length 16**, everything else default. Module name
   `PSRAM_Memory_Interface_HS_Top` (the generator's default), user clock 74.25 MHz
   from a 148.5 MHz memory clock.
3. Copy the generated `*.v` / `*.ipc` here (sub-directory is fine).

`gw_sh build.tcl psram` adds everything under `ip/psram/` automatically.
Then check the assumptions listed in `../../README.md` ("PSRAM build").
