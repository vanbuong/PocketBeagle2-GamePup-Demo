# GamePup PocketBeagle 2 overlays

Three overlays match the split layout used on current PocketBeagle 2 images
(`k3-am62-pocketbeagle2.dtb`):

| Overlay | Role |
|---|---|
| `k3-am62-pocketbeagle2-spi0-ili9341.dts` | ILI9341 LCD on **SPI0** (P2), ECAP2 backlight |
| `k3-am62-pocketbeagle2-gamepup-audio.dts` | Buttons, eyes, buzzer, encoder, MAX98357 on McASP2 |
| `k3-am62-pocketbeagle2-spi2-eth-wiz-click.dts` | W5500 Eth Wiz Click on **SPI2** (mikroBUS) |

Pin cells are numeric so `dtc -@` works without kernel `dt-bindings` headers.

Suggested `fdtoverlays` order:

```text
fdtoverlays /overlays/k3-am62-pocketbeagle2-spi0-ili9341.dtbo \
  /overlays/k3-am62-pocketbeagle2-gamepup-audio.dtbo \
  /overlays/k3-am62-pocketbeagle2-spi2-eth-wiz-click.dtbo
```

(Adjust the `/overlays/` prefix if your image keeps dtbos under `/dtb/ti/`.)

The older monolithic `k3-am6232-pocketbeagle2-gamepup-a4.dts` (LCD on SPI2)
is deprecated for this board layout.
