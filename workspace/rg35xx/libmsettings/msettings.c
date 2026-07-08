#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <errno.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <string.h>

#include "msettings.h"
#include "msettings_core.h"

///////////////////////////////////////

#define SETTINGS_VERSION 2
typedef struct Settings {
	int version; // future proofing
	int brightness;
	int headphones;
	int speaker;
	int unused[2]; // for future use
	// NOTE: doesn't really need to be persisted but still needs to be shared
	int jack; 
} Settings;
static Settings DefaultSettings = {
	.version = SETTINGS_VERSION,
	.brightness = 2,
	.headphones = 4,
	.speaker = 8,
	.jack = 0,
};
static Settings* settings;

static int is_host = 0;
static int shm_size = sizeof(Settings);

#define BACKLIGHT_PATH "/sys/class/backlight/backlight.2/bl_power"
#define BRIGHTNESS_PATH "/sys/class/backlight/backlight.2/brightness"
#define VOLUME_PATH "/sys/class/volume/value"

void InitSettings(void) {
	// shm-open/mmap/read-defaults dance lives in msettings_core.c, shared
	// across platforms -- it only moves shm_size bytes around and never
	// looks at the Settings layout, so it's agnostic to rg35xxplus's extra
	// hdmi field.
	settings = InitSettingsCore(&DefaultSettings, shm_size, &is_host);
	// these shouldn't be persisted
	// settings->jack = 0;

	printf("brightness: %i\nspeaker: %i \n", settings->brightness, settings->speaker);

	SetVolume(GetVolume());
	SetBrightness(GetBrightness());
}
void QuitSettings(void) {
	QuitSettingsCore(settings, shm_size, is_host);
}
static inline void SaveSettings(void) {
	SaveSettingsCore(settings, shm_size);
}

int GetBrightness(void) { // 0-10
	return settings->brightness;
}
void SetBrightness(int value) {
	int raw;
	switch (value) {
		case 0: raw=16; break; 		//   0
		case 1: raw=24; break; 		//   8
		case 2: raw=40; break; 		//  16
		case 3: raw=64; break; 		//  24
		case 4: raw=128; break;		//	64
		case 5: raw=192; break;		//  64
		case 6: raw=256; break;		//  64
		case 7: raw=384; break;		// 128
		case 8: raw=512; break;		// 128
		case 9: raw=768; break;		// 256
		case 10: raw=1024; break;	// 256
	}
	SetRawBrightness(raw);
	settings->brightness = value;
	SaveSettings();
}

int GetVolume(void) { // 0-20
	return settings->jack ? settings->headphones : settings->speaker;
}
void SetVolume(int value) {
	if (settings->jack) settings->headphones = value;
	else settings->speaker = value;
	
	int raw = value * 2;
	SetRawVolume(raw);
	SaveSettings();
}

void SetRawBrightness(int val) { // 0 - 1024
	// printf("SetRawBrightness(%i)\n", val); fflush(stdout);
	int fd = open(BRIGHTNESS_PATH, O_WRONLY);
	if (fd>=0) {
		dprintf(fd,"%d",val);
		close(fd);
	}
}
void SetRawVolume(int val) { // 0 - 40
	int fd = open(VOLUME_PATH, O_WRONLY);
	if (fd>=0) {
		dprintf(fd,"%d",val);
		close(fd);
	}
}

// monitored and set by thread in keymon
int GetJack(void) {
	return settings->jack;
}
void SetJack(int value) {
	// printf("SetJack(%i)\n", value); fflush(stdout);
	
	settings->jack = value;
	SetVolume(GetVolume());
}

int GetHDMI(void) {
	return 0;
}
void SetHDMI(int value) {
	// buh
}

int GetMute(void) { return 0; }
void SetMute(int value) {}
