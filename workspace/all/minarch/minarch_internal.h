#ifndef MINARCH_INTERNAL_H
#define MINARCH_INTERNAL_H

// shared types, globals, and functions between minarch.c and menu.c
// NOTE: keep in sync with minarch.c -- types here were moved verbatim,
// globals/functions here were promoted from static to external linkage
// solely so menu.c can reach them (see minarch.c for the actual definitions)

#include <pthread.h>

#include "libretro.h"
#include "defines.h"
#include "api.h"
#include "utils.h"

///////////////////////////////////////

enum {
	SCALE_NATIVE,
	SCALE_ASPECT,
	SCALE_FULLSCREEN,
	SCALE_CROPPED,
	SCALE_COUNT,
};

///////////////////////////////////////

struct Core {
	int initialized;
	int need_fullpath;

	const char tag[8]; // eg. GBC
	const char name[128]; // eg. gambatte
	const char version[128]; // eg. Gambatte (v0.5.0-netlink 7e02df6)
	const char extensions[128]; // eg. gb|gbc|dmg

	const char config_dir[MAX_PATH]; // eg. /mnt/sdcard/.userdata/rg35xx/GB-gambatte
	const char states_dir[MAX_PATH]; // eg. /mnt/sdcard/.userdata/arm-480/GB-gambatte
	const char saves_dir[MAX_PATH]; // eg. /mnt/sdcard/Saves/GB
	const char bios_dir[MAX_PATH]; // eg. /mnt/sdcard/Bios/GB

	double fps;
	double sample_rate;
	double aspect_ratio;

	void* handle;
	void (*init)(void);
	void (*deinit)(void);

	void (*get_system_info)(struct retro_system_info *info);
	void (*get_system_av_info)(struct retro_system_av_info *info);
	void (*set_controller_port_device)(unsigned port, unsigned device);

	void (*reset)(void);
	void (*run)(void);
	size_t (*serialize_size)(void);
	bool (*serialize)(void *data, size_t size);
	bool (*unserialize)(const void *data, size_t size);
	bool (*load_game)(const struct retro_game_info *game);
	bool (*load_game_special)(unsigned game_type, const struct retro_game_info *info, size_t num_info);
	void (*unload_game)(void);
	unsigned (*get_region)(void);
	void *(*get_memory_data)(unsigned id);
	size_t (*get_memory_size)(unsigned id);

	// retro_audio_buffer_status_callback_t audio_buffer_status;
};
extern struct Core core;

///////////////////////////////////////

struct Game {
	char path[MAX_PATH];
	char name[MAX_PATH]; // TODO: rename to basename?
	char m3u_path[MAX_PATH];
	char tmp_path[MAX_PATH]; // location of unzipped file
	void* data;
	size_t size;
	int is_open;
};
extern struct Game game;

///////////////////////////////

typedef struct Option {
	char* key;
	char* name; // desc
	char* desc; // info, truncated
	char* full; // info, longer but possibly still truncated
	char* var;
	int default_value;
	int value;
	int count; // TODO: drop this?
	int lock;
	char** values;
	char** labels;
} Option;
typedef struct OptionList {
	int count;
	int changed;
	Option* options;

	int enabled_count;
	Option** enabled_options;
	// OptionList_callback_t on_set;
} OptionList;

///////////////////////////////

enum {
	SHORTCUT_SAVE_STATE,
	SHORTCUT_LOAD_STATE,
	SHORTCUT_RESET_GAME,
	SHORTCUT_SAVE_QUIT,
	SHORTCUT_CYCLE_SCALE,
	SHORTCUT_CYCLE_EFFECT,
	SHORTCUT_TOGGLE_FF,
	SHORTCUT_HOLD_FF,
	SHORTCUT_COUNT,
};

#define LOCAL_BUTTON_COUNT 16 // depends on device
#define RETRO_BUTTON_COUNT 16 // allow L3/R3 to be remapped by user if desired, eg. Virtual Boy uses extra buttons for right d-pad

typedef struct ButtonMapping {
	char* name;
	int retro;
	int local; // TODO: dislike this name...
	int mod;
	int default_;
	int ignore;
} ButtonMapping;

///////////////////////////////

enum {
	CONFIG_NONE,
	CONFIG_CONSOLE,
	CONFIG_GAME,
};

struct Config {
	char* system_cfg; // system.cfg based on system limitations
	char* default_cfg; // pak.cfg based on platform limitations
	char* user_cfg; // minarch.cfg or game.cfg based on user preference
	char* device_tag;
	OptionList frontend;
	OptionList core;
	ButtonMapping* controls;
	ButtonMapping* shortcuts;
	int loaded;
	int initialized;
};
extern struct Config config;

enum {
	CONFIG_WRITE_ALL,
	CONFIG_WRITE_GAME,
};

///////////////////////////////////////
// shared frontend state (definitions remain in minarch.c)

extern SDL_Surface* screen;
extern GFX_Renderer renderer;

extern int quit;
extern int show_menu;
extern int simple_mode;
extern int thread_video;
extern int should_run_core;
extern pthread_mutex_t core_mx;

extern int DEVICE_WIDTH;
extern int DEVICE_HEIGHT;
extern int DEVICE_PITCH;

extern int screen_scaling;
extern int screen_effect;
extern int prevent_tearing;
extern int overclock;
extern int has_custom_controllers;
extern int gamepad_type;
extern int state_slot;

extern char* button_labels[];
extern char* gamepad_labels[];
extern char* gamepad_values[];

///////////////////////////////////////
// shared functions (definitions remain in minarch.c)

void SRAM_write(void);
void RTC_write(void);
void State_getPath(char* filename);
void State_read(void);
int State_write(void);
void State_autosave(void);

void setOverclock(int i);
void Game_changeDisc(char* path);

void selectScaler(int src_w, int src_h, int src_p);
void video_refresh_callback(const void *data, unsigned width, unsigned height, size_t pitch);

void hdmimon(void);

void Config_syncFrontend(char* key, int value);
void Config_write(int override);
void Config_restore(void);

Option* OptionList_getOption(OptionList* list, const char* key);
void OptionList_setOptionRawValue(OptionList* list, const char* key, int value);

///////////////////////////////////////
// menu (definitions in menu.c, called from minarch.c)
// MenuList/MenuItem are otherwise menu.c-internal; exposed here only
// because minarch.c's main() touches options_menu.items[1].desc directly.

typedef struct MenuList MenuList;
typedef struct MenuItem MenuItem;
typedef int (*MenuList_callback_t)(MenuList* list, int i);
typedef struct MenuItem {
	char* name;
	char* desc;
	char** values;
	char* key; // optional, used by options
	int id; // optional, used by bindings
	int value;
	MenuList* submenu;
	MenuList_callback_t on_confirm;
	MenuList_callback_t on_change;
} MenuItem;
typedef struct MenuList {
	int type;
	int max_width; // cached on first draw
	char* desc;
	MenuItem* items;
	MenuList_callback_t on_confirm;
	MenuList_callback_t on_change;
} MenuList;

extern MenuList options_menu;

void Menu_init(void);
void Menu_quit(void);
void Menu_beforeSleep(void);
void Menu_afterSleep(void);
void Menu_saveState(void);
void Menu_loadState(void);
void Menu_initState(void);
void Menu_loop(void);

#endif
