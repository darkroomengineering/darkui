CORES = fceumm gambatte gpsp pcsx_rearmed picodrive snes9x2005_plus
CORES+= mednafen_pce_fast mednafen_vb fake-08 mednafen_supafaust mgba pokemini race

###############################

# optional core vars
# *_REPO=
# *_HASH=
# *_CORE=
# *_FLAGS=
# *_MAKEFILE=
# *_BUILD_PATH=

mednafen_pce_fast_REPO = https://github.com/libretro/beetle-pce-fast-libretro
mednafen_vb_REPO = https://github.com/libretro/beetle-vb-libretro

fake-08_REPO = https://github.com/jtothebell/fake-08
fake-08_CORE = fake08_libretro.so
fake-08_BUILD_PATH = fake-08/platform/libretro

fceumm_REPO = https://github.com/libretro/libretro-fceumm

gambatte_REPO = https://github.com/libretro/gambatte-libretro

mednafen_supafaust_REPO = https://github.com/libretro/supafaust

pcsx_rearmed_MAKEFILE = Makefile.libretro

picodrive_REPO = https://github.com/irixxxx/picodrive
picodrive_MAKEFILE = Makefile.libretro
picodrive_PROCS = -j1 # cyclone generator breaks the make jobserver under -j

pokemini_REPO = https://github.com/libretro/PokeMini
pokemini_MAKEFILE = Makefile.libretro

snes9x2005_plus_REPO = https://github.com/libretro/snes9x2005
snes9x2005_plus_FLAGS = USE_BLARGG_APU=1


###############################
# darkUI: pin every core to its last commit before upstream MinUI froze
# (2025-11-27) so builds are reproducible and MinUI's patches keep applying

fceumm_HASH = 407051425
gambatte_HASH = 6924c76ba
gpsp_HASH = 6250979f0
pcsx_rearmed_HASH = 905948569
picodrive_HASH = ec68ce858
snes9x2005_plus_HASH = 74d871db9
mednafen_pce_fast_HASH = 7e9b257b8
mednafen_vb_HASH = 8f837ebc0
fake-08_HASH = 288268d44
mednafen_supafaust_HASH = e25f66765
mgba_HASH = c758314a6
pokemini_HASH = 78656d461 # pre-2025-03-28: later PokeMini.c change breaks the resume-audio patch
race_HASH = 171950ea6

###############################

include ../../all/cores/makefile
