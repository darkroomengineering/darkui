#include <stdio.h>
#include <stdlib.h>

#include <unistd.h>
#include <pthread.h>

#include <stdbool.h>
#include <stdint.h>

#include <msettings.h>

#include "defines.h"
#include "api.h"
#include "utils.h"
#include "api_internal.h"

///////////////////////////////

struct PWR_Context pwr = {0}; // type in api_internal.h; also read by gfx.c

static void PWR_initOverlay(void) {
	// setup surface
	pwr.overlay = PLAT_initOverlay();

	// draw battery
	SDLX_SetAlpha(gfx.assets, 0,0);
	GFX_blitAsset(ASSET_BLACK_PILL, NULL, pwr.overlay, NULL);
	SDLX_SetAlpha(gfx.assets, SDL_SRCALPHA,0);
	GFX_blitBattery(pwr.overlay, NULL);
}

static void PWR_updateBatteryStatus(void) {
	PLAT_getBatteryStatus(&pwr.is_charging, &pwr.charge);
	PLAT_enableOverlay(pwr.should_warn && pwr.charge<=PWR_LOW_CHARGE);
}

static void* PWR_monitorBattery(void *arg) {
	while(1) {
		// TODO: the frequency of checking could depend on whether
		// we're in game (less frequent) or menu (more frequent)
		sleep(5);
		PWR_updateBatteryStatus();
	}
	return NULL;
}

void PWR_init(void) {
	pwr.can_sleep = 1;
	pwr.can_poweroff = 1;
	pwr.can_autosleep = 1;

	pwr.requested_sleep = 0;
	pwr.requested_wake = 0;

	pwr.should_warn = 0;
	pwr.charge = PWR_LOW_CHARGE;

	PWR_initOverlay();

	PWR_updateBatteryStatus();
	pthread_create(&pwr.battery_pt, NULL, &PWR_monitorBattery, NULL);
	pwr.initialized = 1;
}
void PWR_quit(void) {
	if (!pwr.initialized) return;

	// cancel/join the battery thread first -- otherwise it can wake and
	// call PLAT_enableOverlay() during (or after) overlay teardown
	pthread_cancel(pwr.battery_pt);
	pthread_join(pwr.battery_pt, NULL);

	PLAT_quitOverlay();
}
void PWR_warn(int enable) {
	pwr.should_warn = enable;
	PLAT_enableOverlay(pwr.should_warn && pwr.charge<=PWR_LOW_CHARGE);
}

int PWR_ignoreSettingInput(int btn, int show_setting) {
	return show_setting && (btn==BTN_MOD_PLUS || btn==BTN_MOD_MINUS);
}

void PWR_update(int* _dirty, int* _show_setting, PWR_callback_t before_sleep, PWR_callback_t after_sleep) {
	int dirty = _dirty ? *_dirty : 0;
	int show_setting = _show_setting ? *_show_setting : 0;

	static uint32_t last_input_at = 0; // timestamp of last input (autosleep)
	static uint32_t checked_charge_at = 0; // timestamp of last time checking charge
	static uint32_t setting_shown_at = 0; // timestamp when settings started being shown
	static uint32_t power_pressed_at = 0; // timestamp when power button was just pressed
	static uint32_t mod_unpressed_at = 0; // timestamp of last time settings modifier key was NOT down
	static uint32_t was_muted = -1;
	if (was_muted==-1) was_muted = GetMute();

	static int was_charging = -1;
	if (was_charging==-1) was_charging = pwr.is_charging;

	uint32_t now = SDL_GetTicks();
	if (was_charging || PAD_anyPressed() || last_input_at==0) last_input_at = now;

	#define CHARGE_DELAY 1000
	if (dirty || now-checked_charge_at>=CHARGE_DELAY) {
		int is_charging = pwr.is_charging;
		if (was_charging!=is_charging) {
			was_charging = is_charging;
			dirty = 1;
		}
		checked_charge_at = now;
	}

	if (PAD_justReleased(BTN_POWEROFF) || (power_pressed_at && now-power_pressed_at>=1000)) {
		if (before_sleep) before_sleep();
		PWR_powerOff();
	}

	if (PAD_justPressed(BTN_POWER)) {
		power_pressed_at = now;
	}

	#define SLEEP_DELAY 30000 // 30 seconds
	if (now-last_input_at>=SLEEP_DELAY && PWR_preventAutosleep()) last_input_at = now;

	if (
		pwr.requested_sleep || // hardware requested sleep
		now-last_input_at>=SLEEP_DELAY || // autosleep
		(pwr.can_sleep && PAD_justReleased(BTN_SLEEP)) // manual sleep
	) {
		pwr.requested_sleep = 0;
		if (before_sleep) before_sleep();
		PWR_fauxSleep();
		if (after_sleep) after_sleep();

		last_input_at = now = SDL_GetTicks();
		power_pressed_at = 0;
		dirty = 1;
	}

	int was_dirty = dirty; // dirty list (not including settings/battery)

	// TODO: only delay hiding setting changes if that setting didn't require a modifier button be held, otherwise release as soon as modifier is released

	int delay_settings = BTN_MOD_BRIGHTNESS==BTN_MENU; // when both volume and brighness require a modifier hide settings as soon as it is released
	#define SETTING_DELAY 500
	if (show_setting && (now-setting_shown_at>=SETTING_DELAY || !delay_settings) && !PAD_isPressed(BTN_MOD_VOLUME) && !PAD_isPressed(BTN_MOD_BRIGHTNESS)) {
		show_setting = 0;
		dirty = 1;
	}

	if (!show_setting && !PAD_isPressed(BTN_MOD_VOLUME) && !PAD_isPressed(BTN_MOD_BRIGHTNESS)) {
		mod_unpressed_at = now; // this feels backwards but is correct
	}

	#define MOD_DELAY 250
	if (
		(
			(PAD_isPressed(BTN_MOD_VOLUME) || PAD_isPressed(BTN_MOD_BRIGHTNESS)) &&
			(!delay_settings || now-mod_unpressed_at>=MOD_DELAY)
		) ||
		((!BTN_MOD_VOLUME || !BTN_MOD_BRIGHTNESS) && (PAD_justRepeated(BTN_MOD_PLUS) || PAD_justRepeated(BTN_MOD_MINUS)))
	) {
		setting_shown_at = now;
		if (PAD_isPressed(BTN_MOD_BRIGHTNESS)) {
			show_setting = 1;
		}
		else {
			show_setting = 2;
		}
	}

	int muted = GetMute();
	if (muted!=was_muted) {
		was_muted = muted;
		show_setting = 2;
		setting_shown_at = now;
	}

	if (show_setting) dirty = 1; // shm is slow or keymon is catching input on the next frame
	if (_dirty) *_dirty = dirty;
	if (_show_setting) *_show_setting = show_setting;
}

// TODO: this isn't whether it can sleep but more if it should sleep in response to the sleep button
void PWR_disableSleep(void) {
	pwr.can_sleep = 0;
}
void PWR_enableSleep(void) {
	pwr.can_sleep = 1;
}

void PWR_disablePowerOff(void) {
	pwr.can_poweroff = 0;
}
void PWR_powerOff(void) {
	if (pwr.can_poweroff) {

		int w = FIXED_WIDTH;
		int h = FIXED_HEIGHT;
		int p = FIXED_PITCH;
		if (GetHDMI()) {
			w = HDMI_WIDTH;
			h = HDMI_HEIGHT;
			p = HDMI_PITCH;
		}
		gfx.screen = GFX_resize(w,h,p);

		char* msg;
		if (HAS_POWER_BUTTON || HAS_POWEROFF_BUTTON) msg = exists(AUTO_RESUME_PATH) ? "Quicksave created,\npowering off" : "Powering off";
		else msg = exists(AUTO_RESUME_PATH) ? "Quicksave created,\npower off now" : "Power off now";

		// LOG_info("PWR_powerOff %s (%ix%i)\n", gfx.screen, gfx.screen->w, gfx.screen->h);

		// TODO: for some reason screen's dimensions end up being 0x0 in GFX_blitMessage...
		PLAT_clearVideo(gfx.screen);
		GFX_blitMessage(font.large, msg, gfx.screen,&(SDL_Rect){0,0,gfx.screen->w,gfx.screen->h}); //, NULL);
		GFX_flip(gfx.screen);
		PLAT_powerOff();
	}
}

static void PWR_enterSleep(void) {
	SDL_PauseAudio(1);
	if (GetHDMI()) {
		PLAT_clearVideo(gfx.screen);
		PLAT_flip(gfx.screen, 0);
	}
	else {
		SetRawVolume(MUTE_VOLUME_RAW);
		PLAT_enableBacklight(0);
	}
	system("killall -STOP keymon.elf");

	sync();
}
static void PWR_exitSleep(void) {
	system("killall -CONT keymon.elf");
	if (GetHDMI()) {
		// buh
	}
	else {
		PLAT_enableBacklight(1);
		SetVolume(GetVolume());
	}
	SDL_PauseAudio(0);

	sync();
}

static void PWR_waitForWake(void) {
	uint32_t sleep_ticks = SDL_GetTicks();
	while (!PAD_wake()) {
		if (pwr.requested_wake) {
			pwr.requested_wake = 0;
			break;
		}
		SDL_Delay(200);
		if (pwr.can_poweroff && SDL_GetTicks()-sleep_ticks>=120000) { // increased to two minutes
			if (pwr.is_charging) sleep_ticks += 60000; // check again in a minute
			else PWR_powerOff();
		}
	}

	return;
}
void PWR_fauxSleep(void) {
	GFX_clear(gfx.screen);
	PAD_reset();
	PWR_enterSleep();
	PWR_waitForWake();
	PWR_exitSleep();
	PAD_reset();
}

void PWR_disableAutosleep(void) {
	pwr.can_autosleep = 0;
}
void PWR_enableAutosleep(void) {
	pwr.can_autosleep = 1;
}
int PWR_preventAutosleep(void) {
	return pwr.is_charging || !pwr.can_autosleep || GetHDMI();
}

// updated by PWR_updateBatteryStatus()
int PWR_isCharging(void) {
	return pwr.is_charging;
}
int PWR_getBattery(void) { // 10-100 in 10-20% fragments
	return pwr.charge;
}

///////////////////////////////

// TODO: tmp? move to individual platforms or allow overriding like PAD_poll/PAD_wake?
int PLAT_setDateTime(int y, int m, int d, int h, int i, int s) {
	char cmd[512];
	sprintf(cmd, "date -s '%d-%d-%d %d:%d:%d'; hwclock --utc -w", y,m,d,h,i,s);
	system(cmd);
	return 0; // why does this return an int?
}
