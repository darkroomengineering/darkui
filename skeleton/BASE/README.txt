darkUI is a minimal launcher for the Anbernic RG35XX (original) and RG35XXSP--both from the same SD card.

A Darkroom-themed fork of Shaun Inman's MinUI.

Source:
https://github.com/darkroomengineering/darkui

Upstream:
https://github.com/shauninman/MinUI

----------------------------------------
Installing

PREFACE

darkUI has two essential parts: an installer/updater zip archive named "MinUI.zip" (the name is kept from upstream MinUI so existing MinUI installs pick darkUI up as a regular update) and a per-device bootstrap file named "dmenu.bin".

On these devices "TF1" refers to the card that goes into slot one of the device. All other instances of "SD card" or "primary card" refer to the card that goes into the second slot. To use darkUI from a single SD card on both devices you must install it on the second card.

The primary card should be a reputable brand and freshly formatted as FAT32 (MBR).

CAVEATS

While darkUI can be updated from any device once installed, both devices require (minor) changes to NAND or TF1 (via the aforementioned bootstrap file) and therefore need to be installed from the specific device before using. The same is true when trying to use an existing card in a new device of the same type. When in doubt, follow the installation instructions; if all the necessary bits are already installed, the installer will just act as an updater instead.

Installing darkUI over an existing MinUI install is supported and treated as a fresh install: your existing boot logo is backed up and the Darkroom boot logo is installed.

ALL

Preload the "Bios" and "Roms" folders then copy both to the root of your primary card.

RG35XX (original)

darkUI is meant to be installed over a fresh copy of the stock Anbernic firmware (or an existing MinUI install). You can use the stock TF1 card, reports of its poor quality are greatly exaggerated and, as long as you are using the recommended two card setup, no userdata is stored on it.

Copy "/rg35xx/dmenu.bin" (just the file) to the root of the MISC partition of the TF1 card. Copy "MinUI.zip" (without unzipping) to the root of the TF2 card.

RG35XXSP

darkUI is meant to be installed over a fresh copy of the stock Anbernic firmware (or an existing MinUI install). You can use the stock TF1 card. (Note that the PLUS/H/2024/SP stock TF1 is not compatible with other Anbernic families.)

Copy "/rg35xxplus/dmenu.bin" (just the file) to the root of the "NO NAME" partition (FAT32 with an "anbernic" folder) of the TF1 card. Copy "MinUI.zip" (without unzipping) to the root of the TF2 card.

----------------------------------------
Updating

Copy "MinUI.zip" (without unzipping) to the root of the SD card containing your Roms.

----------------------------------------
Shortcuts

  Brightness: MENU + VOLUME UP
                  or VOLUME DOWN

  Sleep: POWER
  Wake: POWER

On the RG35XXSP closing the lid sleeps, opening it wakes.

----------------------------------------
Quicksave & auto-resume

darkUI will create a quicksave when powering off in-game. The next time you power on the device it will automatically resume from where you left off. A quicksave is created when powering off manually or automatically after a short sleep.

----------------------------------------
Roms

Included in this zip is a "Roms" folder containing folders for each console darkUI currently supports. You can rename these folders but you must keep the uppercase tag name in parentheses in order to retain the mapping to the correct emulator (eg. "Nintendo Entertainment System (FC)" could be renamed to "Nintendo (FC)", "NES (FC)", or "Famicom (FC)").

When one or more folder share the same display name (eg. "Game Boy Advance (GBA)" and "Game Boy Advance (MGBA)") they will be combined into a single menu item containing the roms from both folders (continuing the previous example, "Game Boy Advance"). This allows opening specific roms with an alternate pak.

----------------------------------------
Bios

Some emulators require or perform much better with official bios. darkUI is strictly BYOB. Place the bios for each system in a folder that matches the tag in the corresponding "Roms" folder name (eg. bios for "Sony PlayStation (PS)" roms goes in "/Bios/PS/"),

Bios file names are case-sensitive:

   FC: disksys.rom
   GB: gb_bios.bin
  GBA: gba_bios.bin
  GBC: gbc_bios.bin
   MD: bios_CD_E.bin
       bios_CD_J.bin
       bios_CD_U.bin
   PS: psxonpsp660.bin

----------------------------------------
Disc-based games

To streamline launching multi-file disc-based games with darkUI place your bin/cue (and/or iso/wav files) in a folder with the same name as the cue file. darkUI will automatically launch the cue file instead of navigating into the folder when selected, eg.

  Harmful Park (English v1.0)/
    Harmful Park (English v1.0).bin
    Harmful Park (English v1.0).cue

For multi-disc games, put all the files for all the discs in a single folder. Then create an m3u file in that folder (just a text file containing the relative path to each disc's cue file on a separate line) with the same name as the folder. Instead of showing the entire messy contents of the folder, darkUI will launch the appropriate cue file, eg. For a "Policenauts" folder structured like this:

  Policenauts (English v1.0)/
    Policenauts (English v1.0).m3u
    Policenauts (Japan) (Disc 1).bin
    Policenauts (Japan) (Disc 1).cue
    Policenauts (Japan) (Disc 2).bin
    Policenauts (Japan) (Disc 2).cue

The m3u file would contain just:

  Policenauts (Japan) (Disc 1).cue
  Policenauts (Japan) (Disc 2).cue

When a multi-disc game is detected the in-game menu's Continue item will also show the current disc. Press left or right to switch between discs.

darkUI also supports chd files and official pbp files (multi-disc pbp files larger than 2GB are not supported). Regardless of the multi-disc file format used, every disc of the same game share the same memory card and save state slots.

----------------------------------------
Collections

A collection is just a text file containing an ordered list of full paths to rom, cue, or m3u files. These text files live in the "Collections" folder at the root of your SD card, eg. "/Collections/Metroid series.txt" might look like this:

  /Roms/GBA/Metroid Zero Mission.gba
  /Roms/GB/Metroid II.gb
  /Roms/SNES (SFC)/Super Metroid.sfc
  /Roms/GBA/Metroid Fusion.gba

----------------------------------------

Display names

Certain (unsupported arcade) cores require roms to use arcane file names. You can override the display name used throughout darkUI by creating a map.txt in the same folder as the files you want to rename. One line per file, `rom.ext` followed by a single tab followed by `Display Name`. You can hide a file by adding a `.` at the beginning of the display name. eg.

  neogeo.zip	.Neo Geo Bios
  mslug.zip	Metal Slug
  sf2.zip	Street Fighter II

----------------------------------------
Simple mode

Not simple enough for you (or maybe your kids)? darkUI has a simple mode that hides the Tools folder and replaces Options in the in-game menu with Reset. Perfect for handing off to littles (and olds too I guess). Just create an empty file named "enable-simple-mode" (no extension) in "/.userdata/shared/".

----------------------------------------
Advanced

darkUI can automatically run a user-authored shell script on boot. Just place a file named "auto.sh" in "/.userdata/<DEVICE>/". If you're on Windows, make sure your text editor uses Unix line-endings (eg. `\n`), these devices usually choke on Windows line-endings (eg. `\r\n`).

----------------------------------------
Thanks

darkUI is a fork of MinUI by Shaun Inman--all launcher and frontend design and engineering is his work. The original thanks relevant to these devices follow.

To eggs, for his NEON scalers, years of top-notch example code, and patience in the face of my endless questions.

Check out eggs' releases (includes source code):

  RG35XX https://www.dropbox.com/sh/3av70t99ffdgzk1/AAAKPQ4y0kBtTsO3e_Xlrhqha

To neonloop, for putting together the original Trimui toolchain from which I learned everything I know about docker and buildroot and is the basis for every toolchain I've put together since, and for picoarch, the inspiration for minarch.

Check out neonloop's repos:

  https://git.crowdedwood.com

To adixial and acmeplus and the entire muOS community, for sharing their discoveries for the h700 family of Anbernic devices.

Check out muOS and Knulli:

	https://muos.dev
	https://knulli.org

To Steward, for maintaining exhaustive documentation on a plethora of devices:

	https://steward-fu.github.io/website/

To BlackSeraph, for introducing me to chroot.

Check out the GarlicOS repos:

	https://github.com/GarlicOS

To Jim Gray, for commiserating during development, for early alpha testing, and for providing the soundtrack for much of MinUI's development.

Check out Jim's music:

  https://ourghosts.bandcamp.com/music
  https://www.patreon.com/ourghosts/
