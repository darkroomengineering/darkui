#ifndef __API_INTERNAL_H__
#define __API_INTERNAL_H__

// Internal declarations shared only among the workspace/all/common/*.c
// translation units (gfx.c, pwr.c). Nothing here is part of the public
// api.h surface -- do not include this from platform code or from
// minarch.c/minui.c/etc.
//
// This header exists solely because, in the original monolithic api.c,
// GFX_* and PWR_* read and write each other's "static" context structs
// directly:
//   - PWR_initOverlay(), PWR_powerOff(), PWR_enterSleep(), PWR_fauxSleep()
//     read/write gfx.screen and gfx.assets
//   - GFX_blitBattery(), GFX_blitHardwareGroup() read pwr.is_charging
//     and pwr.charge
// Splitting the file into per-subsystem translation units means those
// two structs can no longer be file-static in either gfx.c or pwr.c;
// they're promoted here (not into api.h, which stays public-surface-only)
// so gfx.c and pwr.c can still share them.

#include <pthread.h>

#include "api.h"

struct GFX_Context {
	SDL_Surface* screen;
	SDL_Surface* assets;

	int mode;
	int vsync;
};
extern struct GFX_Context gfx; // defined in gfx.c; also read/written by pwr.c

struct PWR_Context {
	int initialized;

	int can_sleep;
	int can_poweroff;
	int can_autosleep;
	int requested_sleep;
	int requested_wake;

	pthread_t battery_pt;
	int is_charging;
	int charge;
	int should_warn;

	SDL_Surface* overlay;
};
extern struct PWR_Context pwr; // defined in pwr.c; also read by gfx.c

#endif
