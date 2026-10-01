# Screenshot provenance

`gpu-plasma.png` and `gpu-gears.png` are original captures from the real GamePup
framebuffer on a PocketBeagle 2 running Armbian (July 26, 2026), taken from the
earlier 128x160 ST7735R panel without upscaling or post-processing.

Every other image is a native 320x240 render of the LVGL launcher
(`emulator/gamepup-ui`) in headless mode, produced from sample data under
`GAMEPUP_ROOT` (made-up game names, empty ROM files, no real music or voice
recordings). The renders use the exact UI code that runs on the device; only the
framebuffer and button drivers are replaced by an in-memory buffer. Regenerate
them with:

```sh
make -C emulator gamepup-ui
GAMEPUP_ROOT=/path/to/sample-tree ./emulator/gamepup-ui --headless OUTDIR \
    --script "right*7;enter;shot:settings"
```

The gallery intentionally contains only original GamePup UI and GPU benchmark
scenes. It contains no ROM imagery, commercial game screenshot, or Doom data.
The screenshots are licensed with the project under GPL-2.0-only.
