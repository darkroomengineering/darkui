# darkUI

darkUI is a Darkroom-themed fork of [MinUI](https://github.com/shauninman/MinUI), Shaun Inman's focused custom launcher and libretro frontend — stripped down to exactly the two devices it runs on:

- **Anbernic RG35XX** (original, 2022 — Actions ATM7039S), platform `rg35xx`
- **Anbernic RG35XXSP** (Allwinner H700), platform `rg35xxplus`

All launcher and frontend design and engineering is [Shaun Inman](https://github.com/shauninman)'s work; this fork rebrands the boot logos, install screens, and launcher chrome, and prunes the other ten platforms upstream supported. Upstream is archived, so this fork also serves as a pinned, buildable snapshot with both devices sharing one SD-card release.

<img src="github/minui-main.png" width=320 /> <img src="github/minui-menu-gbc.png" width=320 />

## Features

- Simple launcher, simple SD card — one card works in both devices
- No settings or configuration
- No boxart, themes, or distractions
- Consistent in-emulator menu with quick access to save states, disc changing, and emulator options
- Automatic sleep, power-off, and resume (lid-aware on the SP)
- Streamlined emulator frontend (minarch + libretro cores)
- Darkroom boot logo and branding

## Building

Each platform builds inside its own Docker toolchain (x86_64 buildroot — on Apple Silicon build and run the images with `--platform linux/amd64`):

- [`union-rg35xx-toolchain`](https://github.com/darkroomengineering/union-rg35xx-toolchain) → image `rg35xx-toolchain`
- [`union-rg35xxplus-toolchain`](https://github.com/darkroomengineering/union-rg35xxplus-toolchain) → image `rg35xxplus-toolchain`

```sh
make PLATFORM=rg35xx build      # builds inside the toolchain container
make PLATFORM=rg35xxplus build  # rg35xx must be built first (shares its cores)
make                            # everything: both platforms + package
```

Release zips land in `./releases`. The payload zip keeps the internal name `MinUI.zip` so existing MinUI chainloaders pick it up as an update.

## Installing

See `skeleton/BASE/README.txt` (shipped as `README.txt` in the base release zip).
