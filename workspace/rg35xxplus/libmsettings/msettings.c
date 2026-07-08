#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
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
	int hdmi; 
} Settings;
static Settings DefaultSettings = {
	.version = SETTINGS_VERSION,
	.brightness = 2,
	.headphones = 4,
	.speaker = 8,
	.jack = 0,
	.hdmi = 0,
};
static Settings* settings;

static int is_host = 0;
static int shm_size = sizeof(Settings);

#define JACK_STATE_PATH "/sys/module/snd_soc_sunxi_component_jack/parameters/jack_state" // TODO: doesn't change, always 0
#define HDMI_STATE_PATH "/sys/class/switch/hdmi/cable.0/state"

static int getInt(char* path) {
	int i = 0;
	FILE *file = fopen(path, "r");
	if (file!=NULL) {
		fscanf(file, "%i", &i);
		fclose(file);
	}
	return i;
}

void InitSettings(void) {
	// shm-open/mmap/read-defaults dance lives in msettings_core.c, shared
	// across platforms -- it only moves shm_size bytes around and never
	// looks at the Settings layout, so it's agnostic to this platform's
	// extra hdmi field.
	settings = InitSettingsCore(&DefaultSettings, shm_size, &is_host);
	// these shouldn't be persisted
	// settings->jack = 0;
	// settings->hdmi = 0;

	int jack = getInt(JACK_STATE_PATH);
	int hdmi = getInt(HDMI_STATE_PATH);
	printf("brightness: %i (hdmi: %i)\nspeaker: %i (jack: %i)\n", settings->brightness, hdmi, settings->speaker, jack); fflush(stdout);
	
	// both of these set volume
	SetJack(jack);
	SetHDMI(hdmi);
	
	SetBrightness(GetBrightness());
	// system("echo $(< " BRIGHTNESS_PATH ")");
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
	if (settings->hdmi) return;
	
	int raw;
	switch (value) {
		case  0: raw=  4; break;	//  0
		case  1: raw=  6; break;	//  2
		case  2: raw= 10; break;	//  4
		case  3: raw= 16; break;	//  6
		case  4: raw= 32; break;	// 16
		case  5: raw= 48; break;	// 16
		case  6: raw= 64; break;	// 16
		case  7: raw= 96; break;	// 32
		case  8: raw=128; break;	// 32
		case  9: raw=192; break;	// 64
		case 10: raw=255; break;	// 64
	}
	
	SetRawBrightness(raw);
	settings->brightness = value;
	SaveSettings();
}

int GetVolume(void) { // 0-20
	return settings->jack ? settings->headphones : settings->speaker;
}
void SetVolume(int value) {
	if (settings->hdmi) return;
	
	if (settings->jack) settings->headphones = value;
	else settings->speaker = value;
	
	int raw = value * 5;
	SetRawVolume(raw);
	SaveSettings();
}

#define DISP_LCD_SET_BRIGHTNESS  0x102
void SetRawBrightness(int val) { // 0 - 255
	if (settings->hdmi) return;
	
	printf("SetRawBrightness(%i)\n", val); fflush(stdout);
    int fd = open("/dev/disp", O_RDWR);
	if (fd) {
	    unsigned long param[4]={0,val,0,0};
		ioctl(fd, DISP_LCD_SET_BRIGHTNESS, &param);
		close(fd);
	}
}
void SetRawVolume(int val) { // 0 - 100
	printf("SetRawVolume(%i)\n", val); fflush(stdout);
	char cmd[256];
	sprintf(cmd, "amixer sset 'lineout volume' %i%% > /dev/null 2>&1", val);
	// // puts(cmd); fflush(stdout);
	system(cmd);
}

// monitored and set by thread in keymon
int GetJack(void) {
	return settings->jack;
}
void SetJack(int value) {
	// printf("SetJack(%i)\n", value); fflush(stdout);
	
	// char cmd[256];
	// sprintf(cmd, "amixer cset name='Playback Path' '%s' &> /dev/null", value?"HP":"SPK");
	// system(cmd);
	
	settings->jack = value;
	SetVolume(GetVolume());
}

int GetHDMI(void) {	
	// printf("GetHDMI() %i\n", settings->hdmi); fflush(stdout);
	return settings->hdmi;
}
void SetHDMI(int value) {
	// printf("SetHDMI(%i)\n", value); fflush(stdout);
	
	settings->hdmi = value;
	if (value) SetRawVolume(100); // max
	else SetVolume(GetVolume()); // restore
}

int GetMute(void) { return 0; }
void SetMute(int value) {}
