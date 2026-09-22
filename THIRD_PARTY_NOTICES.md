# Third-party notices

GamePup PB2 does not bundle ROMs, BIOS images, commercial game data, libretro
core binaries, Linux kernel binaries, or TI PowerVR packages. Installation
and build helpers obtain some components from their upstream projects or the
operating-system package manager. Those components remain subject to their
own licenses.

This file is a practical inventory, not legal advice. When redistributing a
built image or precompiled package, review and ship the license and source
materials required by every included dependency.

## Source lineage included in this repository

The PocketBeagle 2 device-tree overlay is a port of the BeagleBoard.org
[`BBORG_GAMEPUP-00A2.dts`](https://github.com/beagleboard/bb.org-overlays/blob/master/src/arm/BBORG_GAMEPUP-00A2.dts)
GamePup overlay. The upstream file is copyright Texas Instruments Incorporated
and licensed under GNU GPL version 2. Its notice is retained in the port.

To the extent copyright or similar rights apply, the generated bezel art, OLED
animation, screenshots, original application code, scripts, and documentation
in this repository are offered under the repository's `GPL-2.0-only` license
unless a file says otherwise. Generation notes for the artwork are retained
beside the assets.

## Components obtained at build or install time

| Component | How it is used | Upstream license/status |
|---|---|---|
| [Linux](https://github.com/torvalds/linux) `drm_mipi_dbi` and `ili9341` | `install.sh` downloads matching source and builds modules on the device | GPL-2.0-only; follow Linux's module/source redistribution requirements |
| [libretro API](https://github.com/libretro/RetroArch/tree/master/libretro-common/include) | Header used to build the frontend | MIT |
| [Nestopia](https://github.com/libretro/nestopia) | NES libretro core | GPL-2.0 family; see the upstream repository for component-specific notices |
| [Gambatte](https://github.com/libretro/gambatte-libretro) | Game Boy/Game Boy Color libretro core | GPL-2.0 |
| [Mupen64Plus-Next](https://github.com/libretro/mupen64plus-libretro-nx) | Nintendo 64 libretro core, built by `build-n64-docker.sh` | GPL-2.0 |
| [PrBoom](https://github.com/libretro/libretro-prboom) | Doom libretro core, built by `install-doom.sh` | GPL-2.0 |
| [giflib](https://giflib.sourceforge.net/) | Decodes optional OLED GIF animations | MIT-style license; supplied by the OS, not bundled |
| [Armbian build](https://github.com/armbian/build) | Source of the TI package-priority configuration referenced by `install-gpu.sh` | GPL-2.0 |
| [TI Debian packages](https://github.com/TexasInstruments/ti-debpkgs) | AM62 PowerVR kernel driver, userspace, tools, firmware, and Mesa integration | Not bundled. Package-specific TI, Imagination, MIT, and/or GPL terms apply; inspect the installed package copyright files and TI repository before redistribution |
| Ubuntu/Debian `doom-wad-shareware` | Optional `doom1.wad` installed by `install-doom.sh` | Copyright id Software; governed by the Doom shareware data license, not this project's GPL |

The scripts identify pinned core commits where reproducibility matters, but a
pin is not a license grant. Preserve upstream notices when distributing the
resulting binaries.

The one-command installer is a convenience wrapper around the readable scripts
in this repository. It downloads source over HTTPS and executes package and
system configuration steps as root; users should inspect it before use.

## Games, names, and artwork

No ROM or BIOS file is included. Users are responsible for supplying game
files they are legally entitled to use. The USB importer deliberately stores
them outside the repository under `/opt/gamepup`.

Nintendo, Nintendo Entertainment System, Game Boy, and Nintendo 64 are
trademarks of Nintendo. Doom is a trademark of ZeniMax Media or its
affiliates. PocketBeagle and BeagleBoard are marks of BeagleBoard.org. Other
names belong to their respective owners. This independent project is not
endorsed by those owners.
