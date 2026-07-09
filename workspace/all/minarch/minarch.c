#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <msettings.h>

#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <libgen.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>
#include <zlib.h>
#include <pthread.h>

#include "libretro.h"
#include "defines.h"
#include "api.h"
#include "utils.h"
#include "scaler.h"

#include "minarch_internal.h"

///////////////////////////////////////

SDL_Surface* screen;
int quit = 0;
int show_menu = 0;
int simple_mode = 0;
int thread_video = 0;
static int was_threaded = 0;
int should_run_core = 1; // used by threaded video

static pthread_t		core_pt;
pthread_mutex_t	core_mx;
static pthread_cond_t	core_rq; // not sure this is required
static SDL_Surface*	backbuffer = NULL;
static volatile int	frame_ready = 0; // guards core_rq against lost wakeups
static void* coreThread(void *arg);

// default frontend options
int screen_scaling = SCALE_ASPECT;
static int screen_sharpness = SHARPNESS_SOFT;
int screen_effect = EFFECT_NONE;
int prevent_tearing = 1; // lenient
static int show_debug = 0;
static int max_ff_speed = 3; // 4x
static int fast_forward = 0;
int overclock = 1; // normal
int has_custom_controllers = 0;
int gamepad_type = 0; // index in gamepad_labels/gamepad_values

// these are no longer constants as of the RG CubeXX (even though they look like it)
int DEVICE_WIDTH = 0; // FIXED_WIDTH;
int DEVICE_HEIGHT = 0; // FIXED_HEIGHT;
int DEVICE_PITCH = 0; // FIXED_PITCH;

GFX_Renderer renderer;

///////////////////////////////////////

struct Core core;

///////////////////////////////////////
// based on picoarch/unzip.c

#define ZIP_HEADER_SIZE 30
#define ZIP_CHUNK_SIZE 65536
#define ZIP_LE_READ16(buf) ((uint16_t)(((uint8_t *)(buf))[1] << 8 | ((uint8_t *)(buf))[0]))
#define ZIP_LE_READ32(buf) ((uint32_t)(((uint8_t *)(buf))[3] << 24 | ((uint8_t *)(buf))[2] << 16 | ((uint8_t *)(buf))[1] << 8 | ((uint8_t *)(buf))[0]))
typedef int (*Zip_extract_t)(FILE* zip, FILE* dst, size_t size);

static int Zip_copy(FILE* zip, FILE* dst, size_t size) { // uncompressed?
	uint8_t buffer[ZIP_CHUNK_SIZE];
	while (size) {
		size_t sz = MIN(size, ZIP_CHUNK_SIZE);
		if (sz!= fread(buffer, 1, sz, zip)) return -1;
		if (sz!=fwrite(buffer, 1, sz, dst)) return -1;
		size -= sz;
	}
	return 0;
}
static int Zip_inflate(FILE* zip, FILE* dst, size_t size) { // compressed
	z_stream stream = {0};
	size_t have = 0;
	uint8_t  in[ZIP_CHUNK_SIZE];
	uint8_t out[ZIP_CHUNK_SIZE];
	int ret = -1;

	ret = inflateInit2(&stream, -MAX_WBITS);
	if (ret != Z_OK)
		return ret;

	do {
		size_t insize = MIN(size, ZIP_CHUNK_SIZE);

		stream.avail_in = fread(in, 1, insize, zip);
		if (ferror(zip)) {
			(void)inflateEnd(&stream);
			return Z_ERRNO;
		}

		if (!stream.avail_in)
			break;
		stream.next_in = in;

		do {
			stream.avail_out = ZIP_CHUNK_SIZE;
			stream.next_out = out;

			ret = inflate(&stream, Z_NO_FLUSH);
			switch(ret) {
				case Z_NEED_DICT:
					ret = Z_DATA_ERROR;
				case Z_DATA_ERROR:
				case Z_MEM_ERROR:
					(void)inflateEnd(&stream);
					return ret;
			}

			have = ZIP_CHUNK_SIZE - stream.avail_out;
			if (fwrite(out, 1, have, dst) != have || ferror(dst)) {
				(void)inflateEnd(&stream);
				return Z_ERRNO;
			}
		} while (stream.avail_out == 0);

		size -= insize;
	} while (size && ret != Z_STREAM_END);

	(void)inflateEnd(&stream);

	if (!size || ret == Z_STREAM_END) {
		return Z_OK;
	} else {
		return Z_DATA_ERROR;
	}
}

///////////////////////////////////////

struct Game game;
static void Game_open(char* path) {
	LOG_info("Game_open\n");
	memset(&game, 0, sizeof(game));

	strcpy((char*)game.path, path);
	strcpy((char*)game.name, strrchr(path, '/')+1);

	// if we have a zip file
	if (suffixMatch(".zip", game.path)) {
		LOG_info("is zip file\n");
		int supports_zip = 0;
		int i = 0;
		char* ext;
		char exts[128];
		char* extensions[32];
		strcpy(exts,core.extensions);
		while ((ext=strtok(i?NULL:exts,"|"))) {
			extensions[i++] = ext;
			if (!strcmp("zip", ext)) {
				supports_zip = 1;
				break;
			}
		}
		extensions[i] = NULL;

		// if the core doesn't support zip files natively
		if (!supports_zip) {
			FILE *zip = fopen(game.path, "r");
			if (zip==NULL) {
				LOG_error("Error opening archive: %s\n\t%s\n", game.path, strerror(errno));
				return;
			}

			// extract a known file format
			uint8_t header[ZIP_HEADER_SIZE];
			uint32_t next = 0;
			uint16_t len = 0;
			char filename[MAX_PATH];
			uint32_t compressed_size = 0;
			char extension[8];
			while (1) {
				if (next) fseek(zip, next, SEEK_CUR);

				if (ZIP_HEADER_SIZE!=fread(header, 1, ZIP_HEADER_SIZE, zip)) break;

				if ((uint16_t)(header[6]) & 0x0008) break;

				len = ZIP_LE_READ16(&header[26]);
				if (len>=MAX_PATH) break;

				if (len!=fread(filename,1,len,zip)) break;
				filename[len] = '\0';
				LOG_info("filename: %s\n", filename);

				compressed_size = ZIP_LE_READ32(&header[18]);

				fseek(zip, ZIP_LE_READ16(&header[28]), SEEK_CUR);
				next = compressed_size;

				int found = 0;
				for (i=0; extensions[i]; i++) {
					sprintf(extension, ".%s", extensions[i]);
					if (suffixMatch(extension, filename)) {

						found = 1;
						break;
					}
				}
				if (!found) continue;

				char tmp_template[MAX_PATH];
				strcpy(tmp_template, "/tmp/minarch-XXXXXX");
				char* tmp_dirname = mkdtemp(tmp_template);
				// LOG_info("tmp_dirname: %s\n", tmp_dirname);
				sprintf(game.tmp_path, "%s/%s", tmp_dirname, basename(filename));

				// TODO: we need to clear game.tmp_path if anything below this point fails!

				FILE* dst = fopen(game.tmp_path, "w");
				if (dst==NULL) {
					game.tmp_path[0] = '\0';
					LOG_error("Error extracting file: %s\n\t%s\n", filename, strerror(errno));
					return;
				}

				Zip_extract_t extract = NULL;
				switch (ZIP_LE_READ16(&header[8])) {
					case 0: extract = Zip_copy; break;
					case 8: extract = Zip_inflate; break;
				}

				if (!extract || extract(zip,dst,compressed_size)) {
					game.tmp_path[0] = '\0';
					LOG_error("Error extracting file: %s\n\t%s\n", filename, strerror(errno));
					return;
				}

				fclose(dst);

				break;
			}

			fclose(zip);
		}
	}

	// some cores handle opening files themselves, eg. pcsx_rearmed
	// if the frontend tries to load a 500MB file itself bad things happen
	if (!core.need_fullpath) {
		path = game.tmp_path[0]=='\0'?game.path:game.tmp_path;

		FILE *file = fopen(path, "r");
		if (file==NULL) {
			LOG_error("Error opening game: %s\n\t%s\n", path, strerror(errno));
			return;
		}

		fseek(file, 0, SEEK_END);
		game.size = ftell(file);

		rewind(file);
		game.data = malloc(game.size);
		if (game.data==NULL) {
			LOG_error("Couldn't allocate memory for file: %s\n", path);
			return;
		}

		fread(game.data, sizeof(uint8_t), game.size, file);

		fclose(file);
	}

	// m3u-based?
	char* tmp;
	char m3u_path[MAX_PATH];
	char base_path[MAX_PATH];
	char dir_name[MAX_PATH];

	strcpy(m3u_path, game.path);
	tmp = strrchr(m3u_path, '/') + 1;
	tmp[0] = '\0';

	strcpy(base_path, m3u_path);

	tmp = strrchr(m3u_path, '/');
	tmp[0] = '\0';

	tmp = strrchr(m3u_path, '/');
	strcpy(dir_name, tmp);

	tmp = m3u_path + strlen(m3u_path);
	strcpy(tmp, dir_name);

	tmp = m3u_path + strlen(m3u_path);
	strcpy(tmp, ".m3u");

	if (exists(m3u_path)) {
		strcpy(game.m3u_path, m3u_path);
		strcpy((char*)game.name, strrchr(m3u_path, '/')+1);
	}

	game.is_open = 1;
}
static void Game_close(void) {
	if (game.data) free(game.data);
	if (game.tmp_path[0]) remove(game.tmp_path);
	game.is_open = 0;
	VIB_setStrength(0); // just in case
}

static struct retro_disk_control_ext_callback disk_control_ext;
void Game_changeDisc(char* path) {

	if (exactMatch(game.path, path) || !exists(path)) return;

	Game_close();
	Game_open(path);

	struct retro_game_info game_info = {};
	game_info.path = game.path;
	game_info.data = game.data;
	game_info.size = game.size;

	if (!disk_control_ext.replace_image_index) return;
	disk_control_ext.replace_image_index(0, &game_info);
	putFile(CHANGE_DISC_PATH, path); // MinUI still needs to know this to update recents.txt
}

///////////////////////////////////////

static void SRAM_getPath(char* filename) {
	sprintf(filename, "%s/%s.sav", core.saves_dir, game.name);
}
static void SRAM_read(void) {
	size_t sram_size = core.get_memory_size(RETRO_MEMORY_SAVE_RAM);
	if (!sram_size) return;

	char filename[MAX_PATH];
	SRAM_getPath(filename);
	printf("sav path (read): %s\n", filename);

	FILE *sram_file = fopen(filename, "r");
	if (!sram_file) return;

	void* sram = core.get_memory_data(RETRO_MEMORY_SAVE_RAM);

	if (!sram || !fread(sram, 1, sram_size, sram_file)) {
		LOG_error("Error reading SRAM data\n");
	}

	fclose(sram_file);
}
void SRAM_write(void) {
	size_t sram_size = core.get_memory_size(RETRO_MEMORY_SAVE_RAM);
	if (!sram_size) return;

	char filename[MAX_PATH];
	SRAM_getPath(filename);
	printf("sav path (write): %s\n", filename);

	char tmp_filename[MAX_PATH];
	snprintf(tmp_filename, sizeof(tmp_filename), "%s.tmp", filename);

	FILE *sram_file = fopen(tmp_filename, "w");
	if (!sram_file) {
		LOG_error("Error opening SRAM file: %s\n", strerror(errno));
		return;
	}

	void *sram = core.get_memory_data(RETRO_MEMORY_SAVE_RAM);

	int write_ok = sram && sram_size == fwrite(sram, 1, sram_size, sram_file);
	if (!write_ok) {
		LOG_error("Error writing SRAM data to file\n");
	}

	fclose(sram_file);

	if (write_ok) rename(tmp_filename, filename);
	else remove(tmp_filename);

	sync();
}

///////////////////////////////////////

static void RTC_getPath(char* filename) {
	sprintf(filename, "%s/%s.rtc", core.saves_dir, game.name);
}
static void RTC_read(void) {
	size_t rtc_size = core.get_memory_size(RETRO_MEMORY_RTC);
	if (!rtc_size) return;

	char filename[MAX_PATH];
	RTC_getPath(filename);
	printf("rtc path (read): %s\n", filename);

	FILE *rtc_file = fopen(filename, "r");
	if (!rtc_file) return;

	void* rtc = core.get_memory_data(RETRO_MEMORY_RTC);

	if (!rtc || !fread(rtc, 1, rtc_size, rtc_file)) {
		LOG_error("Error reading RTC data\n");
	}

	fclose(rtc_file);
}
void RTC_write(void) {
	size_t rtc_size = core.get_memory_size(RETRO_MEMORY_RTC);
	if (!rtc_size) return;

	char filename[MAX_PATH];
	RTC_getPath(filename);
	printf("rtc path (write) size(%u): %s\n", rtc_size, filename);

	char tmp_filename[MAX_PATH];
	snprintf(tmp_filename, sizeof(tmp_filename), "%s.tmp", filename);

	FILE *rtc_file = fopen(tmp_filename, "w");
	if (!rtc_file) {
		LOG_error("Error opening RTC file: %s\n", strerror(errno));
		return;
	}

	void *rtc = core.get_memory_data(RETRO_MEMORY_RTC);

	int write_ok = rtc && rtc_size == fwrite(rtc, 1, rtc_size, rtc_file);
	if (!write_ok) {
		LOG_error("Error writing RTC data to file\n");
	}

	fclose(rtc_file);

	if (write_ok) rename(tmp_filename, filename);
	else remove(tmp_filename);

	sync();
}

///////////////////////////////////////

int state_slot = 0;
void State_getPath(char* filename) {
	sprintf(filename, "%s/%s.st%i", core.states_dir, game.name, state_slot);
}
void State_read(void) { // from picoarch
	size_t state_size = core.serialize_size();
	if (!state_size) return;

	int was_ff = fast_forward;
	fast_forward = 0;

	void *state = calloc(1, state_size);
	if (!state) {
		LOG_error("Couldn't allocate memory for state\n");
		goto error;
	}

	char filename[MAX_PATH];
	State_getPath(filename);

	FILE *state_file = fopen(filename, "r");
	if (!state_file) {
		if (state_slot!=8) { // st8 is a default state in MiniUI and may not exist, that's okay
			LOG_error("Error opening state file: %s (%s)\n", filename, strerror(errno));
		}
		goto error;
	}

	// some cores report the wrong serialize size initially for some games, eg. mgba: Wario Land 4
	// so we allow a size mismatch as long as the actual size fits in the buffer we've allocated
	if (state_size < fread(state, 1, state_size, state_file)) {
		LOG_error("Error reading state data from file: %s (%s)\n", filename, strerror(errno));
		goto error;
	}

	if (!core.unserialize(state, state_size)) {
		LOG_error("Error restoring save state: %s (%s)\n", filename, strerror(errno));
		goto error;
	}

error:
	if (state) free(state);
	if (state_file) fclose(state_file);

	fast_forward = was_ff;
}
int State_write(void) { // from picoarch
	size_t state_size = core.serialize_size();
	if (!state_size) return 0;

	int was_ff = fast_forward;
	fast_forward = 0;

	int success = 0;
	void *state = NULL;
	FILE *state_file = NULL;
	char filename[MAX_PATH] = "";
	char tmp_filename[MAX_PATH] = "";

	state = calloc(1, state_size);
	if (!state) {
		LOG_error("Couldn't allocate memory for state\n");
		goto error;
	}

	State_getPath(filename);
	snprintf(tmp_filename, sizeof(tmp_filename), "%s.tmp", filename);

	state_file = fopen(tmp_filename, "w");
	if (!state_file) {
		LOG_error("Error opening state file: %s (%s)\n", tmp_filename, strerror(errno));
		goto error;
	}

	if (!core.serialize(state, state_size)) {
		LOG_error("Error creating save state: %s (%s)\n", filename, strerror(errno));
		goto error;
	}

	if (state_size != fwrite(state, 1, state_size, state_file)) {
		LOG_error("Error writing state data to file: %s (%s)\n", filename, strerror(errno));
		goto error;
	}

	fclose(state_file);
	state_file = NULL;

	if (rename(tmp_filename, filename) != 0) {
		LOG_error("Error renaming state file: %s (%s)\n", filename, strerror(errno));
		goto error;
	}

	success = 1;

error:
	if (state) free(state);
	if (state_file) fclose(state_file);
	if (!success && tmp_filename[0]) remove(tmp_filename);

	sync();

	fast_forward = was_ff;
	return success;
}
void State_autosave(void) {
	int last_state_slot = state_slot;
	state_slot = AUTO_RESUME_SLOT;
	State_write();
	state_slot = last_state_slot;
}
static void State_resume(void) {
	if (!exists(RESUME_SLOT_PATH)) return;

	int last_state_slot = state_slot;
	state_slot = getInt(RESUME_SLOT_PATH);
	unlink(RESUME_SLOT_PATH);
	State_read();
	state_slot = last_state_slot;
}

///////////////////////////////

static char* onoff_labels[] = {
	"Off",
	"On",
	NULL
};
static char* scaling_labels[] = {
	"Native",
	"Aspect",
	"Fullscreen",
	"Cropped",
	NULL
};
static char* effect_labels[] = {
	"None",
	"Line",
	"Grid",
	NULL
};
static char* sharpness_labels[] = {
	"Sharp",
	"Crisp",
	"Soft",
	NULL
};
static char* tearing_labels[] = {
	"Off",
	"Lenient",
	"Strict",
	NULL
};
static char* max_ff_labels[] = {
	"None",
	"2x",
	"3x",
	"4x",
	"5x",
	"6x",
	"7x",
	"8x",
	NULL,
};

///////////////////////////////

enum {
	FE_OPT_SCALING,
	FE_OPT_EFFECT,
	FE_OPT_SHARPNESS,
	FE_OPT_TEARING,
	FE_OPT_OVERCLOCK,
	FE_OPT_THREAD,
	FE_OPT_DEBUG,
	FE_OPT_MAXFF,
	FE_OPT_COUNT,
};

static ButtonMapping default_button_mapping[] = { // used if pak.cfg doesn't exist or doesn't have bindings
	{"Up",			RETRO_DEVICE_ID_JOYPAD_UP,		BTN_ID_DPAD_UP},
	{"Down",		RETRO_DEVICE_ID_JOYPAD_DOWN,	BTN_ID_DPAD_DOWN},
	{"Left",		RETRO_DEVICE_ID_JOYPAD_LEFT,	BTN_ID_DPAD_LEFT},
	{"Right",		RETRO_DEVICE_ID_JOYPAD_RIGHT,	BTN_ID_DPAD_RIGHT},
	{"A Button",	RETRO_DEVICE_ID_JOYPAD_A,		BTN_ID_A},
	{"B Button",	RETRO_DEVICE_ID_JOYPAD_B,		BTN_ID_B},
	{"X Button",	RETRO_DEVICE_ID_JOYPAD_X,		BTN_ID_X},
	{"Y Button",	RETRO_DEVICE_ID_JOYPAD_Y,		BTN_ID_Y},
	{"Start",		RETRO_DEVICE_ID_JOYPAD_START,	BTN_ID_START},
	{"Select",		RETRO_DEVICE_ID_JOYPAD_SELECT,	BTN_ID_SELECT},
	{"L1 Button",	RETRO_DEVICE_ID_JOYPAD_L,		BTN_ID_L1},
	{"R1 Button",	RETRO_DEVICE_ID_JOYPAD_R,		BTN_ID_R1},
	{"L2 Button",	RETRO_DEVICE_ID_JOYPAD_L2,		BTN_ID_L2},
	{"R2 Button",	RETRO_DEVICE_ID_JOYPAD_R2,		BTN_ID_R2},
	{"L3 Button",	RETRO_DEVICE_ID_JOYPAD_L3,		BTN_ID_L3},
	{"R3 Button",	RETRO_DEVICE_ID_JOYPAD_R3,		BTN_ID_R3},
	{NULL,0,0}
};
static ButtonMapping button_label_mapping[] = { // used to lookup the retro_id and local btn_id from button name
	{"NONE",	-1,								BTN_ID_NONE},
	{"UP",		RETRO_DEVICE_ID_JOYPAD_UP,		BTN_ID_DPAD_UP},
	{"DOWN",	RETRO_DEVICE_ID_JOYPAD_DOWN,	BTN_ID_DPAD_DOWN},
	{"LEFT",	RETRO_DEVICE_ID_JOYPAD_LEFT,	BTN_ID_DPAD_LEFT},
	{"RIGHT",	RETRO_DEVICE_ID_JOYPAD_RIGHT,	BTN_ID_DPAD_RIGHT},
	{"A",		RETRO_DEVICE_ID_JOYPAD_A,		BTN_ID_A},
	{"B",		RETRO_DEVICE_ID_JOYPAD_B,		BTN_ID_B},
	{"X",		RETRO_DEVICE_ID_JOYPAD_X,		BTN_ID_X},
	{"Y",		RETRO_DEVICE_ID_JOYPAD_Y,		BTN_ID_Y},
	{"START",	RETRO_DEVICE_ID_JOYPAD_START,	BTN_ID_START},
	{"SELECT",	RETRO_DEVICE_ID_JOYPAD_SELECT,	BTN_ID_SELECT},
	{"L1",		RETRO_DEVICE_ID_JOYPAD_L,		BTN_ID_L1},
	{"R1",		RETRO_DEVICE_ID_JOYPAD_R,		BTN_ID_R1},
	{"L2",		RETRO_DEVICE_ID_JOYPAD_L2,		BTN_ID_L2},
	{"R2",		RETRO_DEVICE_ID_JOYPAD_R2,		BTN_ID_R2},
	{"L3",		RETRO_DEVICE_ID_JOYPAD_L3,		BTN_ID_L3},
	{"R3",		RETRO_DEVICE_ID_JOYPAD_R3,		BTN_ID_R3},
	{NULL,0,0}
};
static ButtonMapping core_button_mapping[RETRO_BUTTON_COUNT+1] = {0};

static const char* device_button_names[LOCAL_BUTTON_COUNT] = {
	[BTN_ID_DPAD_UP]	= "UP",
	[BTN_ID_DPAD_DOWN]	= "DOWN",
	[BTN_ID_DPAD_LEFT]	= "LEFT",
	[BTN_ID_DPAD_RIGHT]	= "RIGHT",
	[BTN_ID_SELECT]		= "SELECT",
	[BTN_ID_START]		= "START",
	[BTN_ID_Y]			= "Y",
	[BTN_ID_X]			= "X",
	[BTN_ID_B]			= "B",
	[BTN_ID_A]			= "A",
	[BTN_ID_L1]			= "L1",
	[BTN_ID_R1]			= "R1",
	[BTN_ID_L2]			= "L2",
	[BTN_ID_R2]			= "R2",
	[BTN_ID_L3]			= "L3",
	[BTN_ID_R3]			= "R3",
};


// NOTE: these must be in BTN_ID_ order also off by 1 because of NONE (which is -1 in BTN_ID_ land)
char* button_labels[] = {
	"NONE", // displayed by default
	"UP",
	"DOWN",
	"LEFT",
	"RIGHT",
	"A",
	"B",
	"X",
	"Y",
	"START",
	"SELECT",
	"L1",
	"R1",
	"L2",
	"R2",
	"L3",
	"R3",
	"MENU+UP",
	"MENU+DOWN",
	"MENU+LEFT",
	"MENU+RIGHT",
	"MENU+A",
	"MENU+B",
	"MENU+X",
	"MENU+Y",
	"MENU+START",
	"MENU+SELECT",
	"MENU+L1",
	"MENU+R1",
	"MENU+L2",
	"MENU+R2",
	"MENU+L3",
	"MENU+R3",
	NULL,
};
static char* overclock_labels[] = {
	"Powersave",
	"Normal",
	"Performance",
	NULL,
};

// TODO: this should be provided by the core
char* gamepad_labels[] = {
	"Standard",
	"DualShock",
	NULL,
};
char* gamepad_values[] = {
	"1",
	"517",
	NULL,
};

static inline char* getScreenScalingDesc(void) {
	if (GFX_supportsOverscan()) {
		return "Native uses integer scaling. Aspect uses core\nreported aspect ratio. Fullscreen has non-square\npixels. Cropped is integer scaled then cropped.";
	}
	else {
		return "Native uses integer scaling.\nAspect uses core reported aspect ratio.\nFullscreen has non-square pixels.";
	}
}
static inline int getScreenScalingCount(void) {
	return GFX_supportsOverscan() ? 4 : 3;
}


struct Config config = {
	.frontend = { // (OptionList)
		.count = FE_OPT_COUNT,
		.options = (Option[]){
			[FE_OPT_SCALING] = {
				.key	= "minarch_screen_scaling",
				.name	= "Screen Scaling",
				.desc	= NULL, // will call getScreenScalingDesc()
				.default_value = 1,
				.value = 1,
				.count = 3, // will call getScreenScalingCount()
				.values = scaling_labels,
				.labels = scaling_labels,
			},
			[FE_OPT_EFFECT] = {
				.key	= "minarch_screen_effect",
				.name	= "Screen Effect",
				.desc	= "Grid simulates an LCD grid.\nLine simulates CRT scanlines.\nEffects usually look best at native scaling.",
				.default_value = 0,
				.value = 0,
				.count = 3,
				.values = effect_labels,
				.labels = effect_labels,
			},
			[FE_OPT_SHARPNESS] = {
				.key	= "minarch_screen_sharpness",
				.name	= "Screen Sharpness",
				.desc	= "Sharp uses nearest neighbor sampling.\nCrisp integer upscales before linear sampling.\nSoft uses linear sampling.",
				.default_value = 2,
				.value = 2,
				.count = 3,
				.values = sharpness_labels,
				.labels = sharpness_labels,
			},
			[FE_OPT_TEARING] = {
				.key	= "minarch_prevent_tearing",
				.name	= "Prevent Tearing",
				.desc	= "Wait for vsync before drawing the next frame.\nLenient only waits when within frame budget.\nStrict always waits.",
				.default_value = VSYNC_LENIENT,
				.value = VSYNC_LENIENT,
				.count = 3,
				.values = tearing_labels,
				.labels = tearing_labels,
			},
			[FE_OPT_OVERCLOCK] = {
				.key	= "minarch_cpu_speed",
				.name	= "CPU Speed",
				.desc	= "Over- or underclock the CPU to prioritize\npure performance or power savings.",
				.default_value = 1,
				.value = 1,
				.count = 3,
				.values = overclock_labels,
				.labels = overclock_labels,
			},
			[FE_OPT_THREAD] = {
				.key	= "minarch_thread_video",
				.name	= "Prioritize Audio",
				.desc	= "Can eliminate crackle but\nmay cause dropped frames.\nOnly turn on if necessary.",
				.default_value = 0,
				.value = 0,
				.count = 2,
				.values = onoff_labels,
				.labels = onoff_labels,
			},
			[FE_OPT_DEBUG] = {
				.key	= "minarch_debug_hud",
				.name	= "Debug HUD",
				.desc	= "Show frames per second, cpu load,\nresolution, and scaler information.",
				.default_value = 0,
				.value = 0,
				.count = 2,
				.values = onoff_labels,
				.labels = onoff_labels,
			},
			[FE_OPT_MAXFF] = {
				.key	= "minarch_max_ff_speed",
				.name	= "Max FF Speed",
				.desc	= "Fast forward will not exceed the\nselected speed (but may be less\ndepending on game and emulator).",
				.default_value = 3, // 4x
				.value = 3, // 4x
				.count = 8,
				.values = max_ff_labels,
				.labels = max_ff_labels,
			},
			[FE_OPT_COUNT] = {NULL}
		}
	},
	.core = { // (OptionList)
		.count = 0,
		.options = (Option[]){
			{NULL},
		},
	},
	.controls = default_button_mapping,
	.shortcuts = (ButtonMapping[]){
		[SHORTCUT_SAVE_STATE]			= {"Save State",		-1, BTN_ID_NONE, 0},
		[SHORTCUT_LOAD_STATE]			= {"Load State",		-1, BTN_ID_NONE, 0},
		[SHORTCUT_RESET_GAME]			= {"Reset Game",		-1, BTN_ID_NONE, 0},
		[SHORTCUT_SAVE_QUIT]			= {"Save & Quit",		-1, BTN_ID_NONE, 0},
		[SHORTCUT_CYCLE_SCALE]			= {"Cycle Scaling",		-1, BTN_ID_NONE, 0},
		[SHORTCUT_CYCLE_EFFECT]			= {"Cycle Effect",		-1, BTN_ID_NONE, 0},
		[SHORTCUT_TOGGLE_FF]			= {"Toggle FF",			-1, BTN_ID_NONE, 0},
		[SHORTCUT_HOLD_FF]				= {"Hold FF",			-1, BTN_ID_NONE, 0},
		{NULL}
	},
};
static int Config_getValue(char* cfg, const char* key, char* out_value, int* lock) { // gets value from string
	char* tmp = cfg;
	while ((tmp = strstr(tmp, key))) {
		if (lock!=NULL && tmp>cfg && *(tmp-1)=='-') *lock = 1; // prefixed with a `-` means lock
		tmp += strlen(key);
		if (!strncmp(tmp, " = ", 3)) break; // matched
	};
	if (!tmp) return 0;
	tmp += 3;

	strncpy(out_value, tmp, 256);
	out_value[256 - 1] = '\0';
	tmp = strchr(out_value, '\n');
	if (!tmp) tmp = strchr(out_value, '\r');
	if (tmp) *tmp = '\0';

	// LOG_info("\t%s = %s (%s)\n", key, out_value, (lock && *lock) ? "hidden":"shown");
	return 1;
}

void setOverclock(int i) {
	overclock = i;
	switch (i) {
		case 0: PWR_setCPUSpeed(CPU_SPEED_POWERSAVE); break;
		case 1: PWR_setCPUSpeed(CPU_SPEED_NORMAL); break;
		case 2: PWR_setCPUSpeed(CPU_SPEED_PERFORMANCE); break;
	}
}
static int toggle_thread = 0;
void Config_syncFrontend(char* key, int value) {
	int i = -1;
	if (exactMatch(key,config.frontend.options[FE_OPT_SCALING].key)) {
		screen_scaling 	= value;

		if (screen_scaling==SCALE_NATIVE) GFX_setSharpness(SHARPNESS_SHARP);
		else GFX_setSharpness(screen_sharpness);

		renderer.dst_p = 0;
		i = FE_OPT_SCALING;
	}
	else if (exactMatch(key,config.frontend.options[FE_OPT_EFFECT].key)) {
		screen_effect = value;
		GFX_setEffect(value);
		renderer.dst_p = 0;
		i = FE_OPT_EFFECT;
	}
	else if (exactMatch(key,config.frontend.options[FE_OPT_SHARPNESS].key)) {
		screen_sharpness = value;

		if (screen_scaling==SCALE_NATIVE) GFX_setSharpness(SHARPNESS_SHARP);
		else GFX_setSharpness(screen_sharpness);

		renderer.dst_p = 0;
		i = FE_OPT_SHARPNESS;
	}
	else if (exactMatch(key,config.frontend.options[FE_OPT_TEARING].key)) {
		prevent_tearing = value;
		i = FE_OPT_TEARING;
	}
	else if (exactMatch(key,config.frontend.options[FE_OPT_THREAD].key)) {
		int old_value = thread_video || was_threaded;
		toggle_thread = old_value!=value;
		i = FE_OPT_THREAD;
	}
	else if (exactMatch(key,config.frontend.options[FE_OPT_OVERCLOCK].key)) {
		overclock = value;
		i = FE_OPT_OVERCLOCK;
	}
	else if (exactMatch(key,config.frontend.options[FE_OPT_DEBUG].key)) {
		show_debug = value;
		i = FE_OPT_DEBUG;
	}
	else if (exactMatch(key,config.frontend.options[FE_OPT_MAXFF].key)) {
		max_ff_speed = value;
		i = FE_OPT_MAXFF;
	}
	if (i==-1) return;
	Option* option = &config.frontend.options[i];
	option->value = value;
}
static void OptionList_setOptionValue(OptionList* list, const char* key, const char* value);
static void Config_getPath(char* filename, int override) {
	char device_tag[64] = {0};
	if (config.device_tag) snprintf(device_tag, sizeof(device_tag), "-%s", config.device_tag);
	if (override) sprintf(filename, "%s/%s%s.cfg", core.config_dir, game.name, device_tag);
	else sprintf(filename, "%s/minarch%s.cfg", core.config_dir, device_tag);
	LOG_info("Config_getPath %s\n", filename);
}
static void Config_init(void) {
	if (!config.default_cfg || config.initialized) return;

	LOG_info("Config_init\n");
	char* tmp = config.default_cfg;
	char* tmp2;
	char* key;

	char button_name[128];
	char button_id[128];
	int i = 0;
	while ((tmp = strstr(tmp, "bind "))) {
		tmp += 5; // tmp now points to the button name (plus the rest of the line)
		key = tmp;
		tmp = strstr(tmp, " = ");
		if (!tmp) break;

		int len = tmp-key;
		strncpy(button_name, key, len);
		button_name[len] = '\0';

		tmp += 3;
		strncpy(button_id, tmp, 128);
		tmp2 = strchr(button_id, '\n');
		if (!tmp2) tmp2 = strchr(button_id, '\r');
		if (tmp2) *tmp2 = '\0';

		int retro_id = -1;
		int local_id = -1;

		tmp2 = strrchr(button_id, ':');
		int remap = 0;
		if (tmp2) {
			for (int j=0; button_label_mapping[j].name; j++) {
				ButtonMapping* button = &button_label_mapping[j];
				if (!strcmp(tmp2+1,button->name)) {
					retro_id = button->retro;
					break;
				}
			}
			*tmp2 = '\0';
		}
		for (int j=0; button_label_mapping[j].name; j++) {
			ButtonMapping* button = &button_label_mapping[j];
			if (!strcmp(button_id,button->name)) {
				local_id = button->local;
				if (retro_id==-1) retro_id = button->retro;
				break;
			}
		}

		tmp += strlen(button_id); // prepare to continue search

		LOG_info("\tbind %s (%s) %i:%i\n", button_name, button_id, local_id, retro_id);

		// TODO: test this without a final line return
		tmp2 = calloc(strlen(button_name)+1, sizeof(char));
		strcpy(tmp2, button_name);
		ButtonMapping* button = &core_button_mapping[i++];
		button->name = tmp2;
		button->retro = retro_id;
		button->local = local_id;
	};

	config.initialized = 1;
}
static void Config_quit(void) {
	if (!config.initialized) return;
	for (int i=0; core_button_mapping[i].name; i++) {
		free(core_button_mapping[i].name);
	}
}
static void Config_readOptionsString(char* cfg) {
	if (!cfg) return;

	LOG_info("Config_readOptions\n");
	char key[256];
	char value[256];
	for (int i=0; config.frontend.options[i].key; i++) {
		Option* option = &config.frontend.options[i];
		if (!Config_getValue(cfg, option->key, value, &option->lock)) continue;
		OptionList_setOptionValue(&config.frontend, option->key, value);
		Config_syncFrontend(option->key, option->value);
	}

	if (has_custom_controllers && Config_getValue(cfg,"minarch_gamepad_type",value,NULL)) {
		gamepad_type = strtol(value, NULL, 0);
		int device = strtol(gamepad_values[gamepad_type], NULL, 0);
		core.set_controller_port_device(0, device);
	}

	for (int i=0; config.core.options[i].key; i++) {
		Option* option = &config.core.options[i];
		if (!Config_getValue(cfg, option->key, value, &option->lock)) continue;
		OptionList_setOptionValue(&config.core, option->key, value);
	}
}
static void Config_readControlsString(char* cfg) {
	if (!cfg) return;

	LOG_info("Config_readControlsString\n");

	char key[256];
	char value[256];
	char* tmp;
	for (int i=0; config.controls[i].name; i++) {
		ButtonMapping* mapping = &config.controls[i];
		sprintf(key, "bind %s", mapping->name);
		sprintf(value, "NONE");

		if (!Config_getValue(cfg, key, value, NULL)) continue;
		if ((tmp = strrchr(value, ':'))) *tmp = '\0'; // this is a binding artifact in default.cfg, ignore

		int id = -1;
		for (int j=0; button_labels[j]; j++) {
			if (!strcmp(button_labels[j],value)) {
				id = j - 1;
				break;
			}
		}
		// LOG_info("\t%s (%i)\n", value, id);

		int mod = 0;
		if (id>=LOCAL_BUTTON_COUNT) {
			id -= LOCAL_BUTTON_COUNT;
			mod = 1;
		}

		mapping->local = id;
		mapping->mod = mod;
	}

	for (int i=0; config.shortcuts[i].name; i++) {
		ButtonMapping* mapping = &config.shortcuts[i];
		sprintf(key, "bind %s", mapping->name);
		sprintf(value, "NONE");

		if (!Config_getValue(cfg, key, value, NULL)) continue;

		int id = -1;
		for (int j=0; button_labels[j]; j++) {
			if (!strcmp(button_labels[j],value)) {
				id = j - 1;
				break;
			}
		}

		int mod = 0;
		if (id>=LOCAL_BUTTON_COUNT) {
			id -= LOCAL_BUTTON_COUNT;
			mod = 1;
		}
		// LOG_info("shortcut %s:%s (%i:%i)\n", key,value, id, mod);

		mapping->local = id;
		mapping->mod = mod;
	}
}
static void Config_load(void) {
	LOG_info("Config_load\n");

	config.device_tag = getenv("DEVICE");
	LOG_info("config.device_tag %s\n", config.device_tag);

	// update for crop overscan support
	Option* scaling_option = &config.frontend.options[FE_OPT_SCALING];
	scaling_option->desc = getScreenScalingDesc();
	scaling_option->count = getScreenScalingCount();
	if (!GFX_supportsOverscan()) {
		scaling_labels[3] = NULL;
	}

	char* system_path = SYSTEM_PATH "/system.cfg";

	char device_system_path[MAX_PATH] = {0};
	if (config.device_tag) sprintf(device_system_path, SYSTEM_PATH "/system-%s.cfg", config.device_tag);

	if (config.device_tag && exists(device_system_path)) {
		LOG_info("usng device_system_path: %s\n", device_system_path);
		config.system_cfg = allocFile(device_system_path);
	}
	else if (exists(system_path)) config.system_cfg = allocFile(system_path);
	else config.system_cfg = NULL;

	// LOG_info("config.system_cfg: %s\n", config.system_cfg);

	char default_path[MAX_PATH];
	getEmuPath((char *)core.tag, default_path);
	char* tmp = strrchr(default_path, '/');
	strcpy(tmp,"/default.cfg");

	char device_default_path[MAX_PATH] = {0};
	if (config.device_tag) {
		getEmuPath((char *)core.tag, device_default_path);
		tmp = strrchr(device_default_path, '/');
		char filename[64];
		snprintf(filename, sizeof(filename), "/default-%s.cfg", config.device_tag);
		strcpy(tmp,filename);
	}

	if (config.device_tag && exists(device_default_path)) {
		LOG_info("usng device_default_path: %s\n", device_default_path);
		config.default_cfg = allocFile(device_default_path);
	}
	else if (exists(default_path)) config.default_cfg = allocFile(default_path);
	else config.default_cfg = NULL;

	// LOG_info("config.default_cfg: %s\n", config.default_cfg);

	char path[MAX_PATH];
	config.loaded = CONFIG_NONE;
	int override = 0;
	Config_getPath(path, CONFIG_WRITE_GAME);
	if (exists(path)) override = 1;
	if (!override) Config_getPath(path, CONFIG_WRITE_ALL);

	config.user_cfg = allocFile(path);
	if (!config.user_cfg) return;

	LOG_info("using user config: %s\n", path);

	config.loaded = override ? CONFIG_GAME : CONFIG_CONSOLE;
}
static void Config_free(void) {
	if (config.system_cfg) free(config.system_cfg);
	if (config.default_cfg) free(config.default_cfg);
	if (config.user_cfg) free(config.user_cfg);
}
static void Config_readOptions(void) {
	Config_readOptionsString(config.system_cfg);
	Config_readOptionsString(config.default_cfg);
	Config_readOptionsString(config.user_cfg);

	// screen_scaling = SCALE_NATIVE; // TODO: tmp
}
static void Config_readControls(void) {
	Config_readControlsString(config.default_cfg);
	Config_readControlsString(config.user_cfg);
}
void Config_write(int override) {
	char path[MAX_PATH];
	// sprintf(path, "%s/%s.cfg", core.config_dir, game.name);
	Config_getPath(path, CONFIG_WRITE_GAME);

	if (!override) {
		if (config.loaded==CONFIG_GAME) unlink(path);
		Config_getPath(path, CONFIG_WRITE_ALL);
	}
	config.loaded = override ? CONFIG_GAME : CONFIG_CONSOLE;

	FILE *file = fopen(path, "wb");
	if (!file) return;

	for (int i=0; config.frontend.options[i].key; i++) {
		Option* option = &config.frontend.options[i];
		fprintf(file, "%s = %s\n", option->key, option->values[option->value]);
	}
	for (int i=0; config.core.options[i].key; i++) {
		Option* option = &config.core.options[i];
		fprintf(file, "%s = %s\n", option->key, option->values[option->value]);
	}

	if (has_custom_controllers) fprintf(file, "%s = %i\n", "minarch_gamepad_type", gamepad_type);

	for (int i=0; config.controls[i].name; i++) {
		ButtonMapping* mapping = &config.controls[i];
		int j = mapping->local + 1;
		if (mapping->mod) j += LOCAL_BUTTON_COUNT;
		fprintf(file, "bind %s = %s\n", mapping->name, button_labels[j]);
	}
	for (int i=0; config.shortcuts[i].name; i++) {
		ButtonMapping* mapping = &config.shortcuts[i];
		int j = mapping->local + 1;
		if (mapping->mod) j += LOCAL_BUTTON_COUNT;
		fprintf(file, "bind %s = %s\n", mapping->name, button_labels[j]);
	}

	fclose(file);
	sync();
}
void Config_restore(void) {
	char path[MAX_PATH];
	if (config.loaded==CONFIG_GAME) {
		Config_getPath(path, CONFIG_WRITE_GAME);
		unlink(path);
		LOG_info("deleted game config: %s\n", path);
	}
	else if (config.loaded==CONFIG_CONSOLE) {
		Config_getPath(path, CONFIG_WRITE_ALL);
		unlink(path);
		LOG_info("deleted console config: %s\n", path);
	}
	config.loaded = CONFIG_NONE;

	for (int i=0; config.frontend.options[i].key; i++) {
		Option* option = &config.frontend.options[i];
		option->value = option->default_value;
		Config_syncFrontend(option->key, option->value);
	}
	for (int i=0; config.core.options[i].key; i++) {
		Option* option = &config.core.options[i];
		option->value = option->default_value;
	}
	config.core.changed = 1; // let the core know

	if (has_custom_controllers) {
		gamepad_type = 0;
		core.set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
	}

	for (int i=0; config.controls[i].name; i++) {
		ButtonMapping* mapping = &config.controls[i];
		mapping->local = mapping->default_;
		mapping->mod = 0;
	}
	for (int i=0; config.shortcuts[i].name; i++) {
		ButtonMapping* mapping = &config.shortcuts[i];
		mapping->local = BTN_ID_NONE;
		mapping->mod = 0;
	}

	Config_load();
	Config_readOptions();
	Config_readControls();
	Config_free();

	renderer.dst_p = 0;
}

///////////////////////////////
static struct Special {
	int palette_updated;
} special;
static void Special_updatedDMGPalette(int frames) {
	// LOG_info("Special_updatedDMGPalette(%i)\n", frames);
	special.palette_updated = frames; // must wait a few frames
}
static void Special_refreshDMGPalette(void) {
	special.palette_updated -= 1;
	if (special.palette_updated>0) return;

	int rgb = getInt("/tmp/dmg_grid_color");
	GFX_setEffectColor(rgb);
}
static void Special_init(void) {
	if (special.palette_updated>1) special.palette_updated = 1;
	// else if (exactMatch((char*)core.tag, "GBC"))  {
	// 	putInt("/tmp/dmg_grid_color",0xF79E);
	// 	special.palette_updated = 1;
	// }
}
static void Special_render(void) {
	if (special.palette_updated) Special_refreshDMGPalette();
}
static void Special_quit(void) {
	system("rm -f /tmp/dmg_grid_color");
}
///////////////////////////////

static  int Option_getValueIndex(Option* item, const char* value) {
	if (!value) return 0;
	for (int i=0; i<item->count; i++) {
		if (!strcmp(item->values[i], value)) return i;
	}
	return 0;
}
static void Option_setValue(Option* item, const char* value) {
	// TODO: store previous value?
	item->value = Option_getValueIndex(item, value);
}

// TODO: does this also need to be applied to OptionList_vars()?
static const char* option_key_name[] = {
	"pcsx_rearmed_analog_combo", "DualShock Toggle Combo",
	NULL
};
static const char* getOptionNameFromKey(const char* key, const char* name) {
	char* _key = NULL;
	for (int i=0; (_key = (char*)option_key_name[i]); i+=2) {
		if (exactMatch((char*)key,_key)) return option_key_name[i+1];
	}
	return name;
}

// the following 3 functions always touch config.core, the rest can operate on arbitrary OptionLists
static void OptionList_init(const struct retro_core_option_definition *defs) {
	LOG_info("OptionList_init\n");
	int count;
	for (count=0; defs[count].key; count++);

	// LOG_info("count: %i\n", count);

	// TODO: add frontend options to this? so the can use the same override method? eg. minarch_*

	config.core.count = count;
	if (count) {
		config.core.options = calloc(count+1, sizeof(Option));

		for (int i=0; i<config.core.count; i++) {
			int len;
			const struct retro_core_option_definition *def = &defs[i];
			Option* item = &config.core.options[i];
			len = strlen(def->key) + 1;

			item->key = calloc(len, sizeof(char));
			strcpy(item->key, def->key);

			len = strlen(def->desc) + 1;
			item->name = calloc(len, sizeof(char));
			strcpy(item->name, getOptionNameFromKey(def->key,def->desc));

			if (def->info) {
				len = strlen(def->info) + 1;
				item->desc = calloc(len, sizeof(char));
				strncpy(item->desc, def->info, len);

				item->full = calloc(len, sizeof(char));
				strncpy(item->full, item->desc, len);
				// item->desc[len-1] = '\0';

				// these magic numbers are more about chars per line than pixel width
				// so it's not going to be relative to the screen size, only the scale
				// what does that even mean?
				GFX_wrapText(font.tiny, item->desc, SCALE1(240), 2); // TODO magic number!
				GFX_wrapText(font.medium, item->full, SCALE1(240), 7); // TODO: magic number!
			}

			for (count=0; def->values[count].value; count++);

			item->count = count;
			item->values = calloc(count+1, sizeof(char*));
			item->labels = calloc(count+1, sizeof(char*));

			for (int j=0; j<count; j++) {
				const char* value = def->values[j].value;
				const char* label = def->values[j].label;

				len = strlen(value) + 1;
				item->values[j] = calloc(len, sizeof(char));
				strcpy(item->values[j], value);

				if (label) {
					len = strlen(label) + 1;
					item->labels[j] = calloc(len, sizeof(char));
					strcpy(item->labels[j], label);
				}
				else {
					item->labels[j] = item->values[j];
				}
				// printf("\t%s\n", item->labels[j]);
			}

			item->value = Option_getValueIndex(item, def->default_value);
			item->default_value = item->value;

			// LOG_info("\tINIT %s (%s) TO %s (%s)\n", item->name, item->key, item->labels[item->value], item->values[item->value]);
		}
	}
	// fflush(stdout);
}
static void OptionList_vars(const struct retro_variable *vars) {
	LOG_info("OptionList_vars\n");
	int count;
	for (count=0; vars[count].key; count++);

	config.core.count = count;
	if (count) {
		config.core.options = calloc(count+1, sizeof(Option));

		for (int i=0; i<config.core.count; i++) {
			int len;
			const struct retro_variable *var = &vars[i];
			Option* item = &config.core.options[i];

			len = strlen(var->key) + 1;
			item->key = calloc(len, sizeof(char));
			strcpy(item->key, var->key);

			len = strlen(var->value) + 1;
			item->var = calloc(len, sizeof(char));
			strcpy(item->var, var->value);

			char* tmp = strchr(item->var, ';');
			if (tmp && *(tmp+1)==' ') {
				*tmp = '\0';
				item->name = item->var;
				tmp += 2;
			}

			char* opt = tmp;
			for (count=0; (tmp=strchr(tmp, '|')); tmp++, count++);
			count += 1; // last entry after final '|'

			item->count = count;
			item->values = calloc(count+1, sizeof(char*));
			item->labels = calloc(count+1, sizeof(char*));

			tmp = opt;
			int j;
			for (j=0; (tmp=strchr(tmp, '|')); j++) {
				item->values[j] = opt;
				item->labels[j] = opt;
				*tmp = '\0';
				tmp += 1;
				opt = tmp;
			}
			item->values[j] = opt;
			item->labels[j] = opt;

			// no native default_value support for retro vars
			item->value = 0;
			item->default_value = item->value;
			// printf("SET %s to %s (%i)\n", item->key, default_value, item->value); fflush(stdout);
		}
	}
	// fflush(stdout);
}
static void OptionList_reset(void) {
	if (!config.core.count) return;

	for (int i=0; i<config.core.count; i++) {
		Option* item = &config.core.options[i];
		if (item->var) {
			// values/labels are all points to var
			// so no need to free individually
			free(item->var);
		}
		else {
			if (item->desc) free(item->desc);
			if (item->full) free(item->full);
			for (int j=0; j<item->count; j++) {
				char* value = item->values[j];
				char* label = item->labels[j];
				if (label!=value) free(label);
				free(value);
			}
		}
		free(item->values);
		free(item->labels);
		free(item->key);
		free(item->name);
	}
	if (config.core.enabled_options) free(config.core.enabled_options);
	config.core.enabled_count = 0;
	free(config.core.options);
}

Option* OptionList_getOption(OptionList* list, const char* key) {
	for (int i=0; i<list->count; i++) {
		Option* item = &list->options[i];
		if (!strcmp(item->key, key)) return item;
	}
	return NULL;
}
static char* OptionList_getOptionValue(OptionList* list, const char* key) {
	Option* item = OptionList_getOption(list, key);
	// if (item) LOG_info("\tGET %s (%s) = %s (%s)\n", item->name, item->key, item->labels[item->value], item->values[item->value]);

	if (item) return item->values[item->value];
	else LOG_warn("unknown option %s \n", key);
	return NULL;
}
void OptionList_setOptionRawValue(OptionList* list, const char* key, int value) {
	Option* item = OptionList_getOption(list, key);
	if (item) {
		item->value = value;
		list->changed = 1;
		// LOG_info("\tRAW SET %s (%s) TO %s (%s)\n", item->name, item->key, item->labels[item->value], item->values[item->value]);
		// if (list->on_set) list->on_set(list, key);

		if (exactMatch((char*)core.tag, "GB") && containsString(item->key, "palette")) Special_updatedDMGPalette(3); // from options
	}
	else LOG_info("unknown option %s \n", key);
}
static void OptionList_setOptionValue(OptionList* list, const char* key, const char* value) {
	Option* item = OptionList_getOption(list, key);
	if (item) {
		Option_setValue(item, value);
		list->changed = 1;
		// LOG_info("\tSET %s (%s) TO %s (%s)\n", item->name, item->key, item->labels[item->value], item->values[item->value]);
		// if (list->on_set) list->on_set(list, key);

		if (exactMatch((char*)core.tag, "GB") && containsString(item->key, "palette")) Special_updatedDMGPalette(2); // from core
	}
	else LOG_info("unknown option %s \n", key);
}
// static void OptionList_setOptionVisibility(OptionList* list, const char* key, int visible) {
// 	Option* item = OptionList_getOption(list, key);
// 	if (item) item->visible = visible;
// 	else printf("unknown option %s \n", key); fflush(stdout);
// }

///////////////////////////////

static int setFastForward(int enable) {
	if (!fast_forward && enable && thread_video) {
		// LOG_info("entered fast forward with threaded core...\n");
		was_threaded = 1;
		toggle_thread = 1;
	}
	else if (fast_forward && !enable && !thread_video && was_threaded) {
		// LOG_info("exited fast forward with previously threaded core...\n");
		was_threaded = 0;
		toggle_thread = 1;
	}
	fast_forward = enable;
	return enable;
}

static uint32_t buttons = 0; // RETRO_DEVICE_ID_JOYPAD_* buttons
static int ignore_menu = 0;
static int show_setting = 0; // 1=brightness, 2=volume; drawn over the game frame in video_refresh_callback_main
static int overlay_clear = 0; // frames left to fully clear after the overlay disappears (rg35xx letterbox cleanup)
static void input_poll_callback(void) {
	PAD_poll();

	PWR_update(NULL, &show_setting, Menu_beforeSleep, Menu_afterSleep);

	// I _think_ this can stay as is...
	if (PAD_justPressed(BTN_MENU)) {
		ignore_menu = 0;
	}
	if (PAD_isPressed(BTN_MENU) && (PAD_isPressed(BTN_PLUS) || PAD_isPressed(BTN_MINUS))) {
		ignore_menu = 1;
	}

	if (PAD_justPressed(BTN_POWER)) {
		if (thread_video) {
			// LOG_info("pressed power with threaded core...\n");
			was_threaded = 1;
			toggle_thread = 1;
		}
	}
	else if (PAD_justReleased(BTN_POWER)) {
		if (!thread_video && was_threaded) {
			// LOG_info("released power with previously threaded core before power off...\n");
			was_threaded = 0;
			toggle_thread = 1;
		}
	}

	static int toggled_ff_on = 0; // this logic only works because TOGGLE_FF is before HOLD_FF in the menu...
	for (int i=0; i<SHORTCUT_COUNT; i++) {
		ButtonMapping* mapping = &config.shortcuts[i];
		int btn = 1 << mapping->local;
		if (btn==BTN_NONE) continue; // not bound
		if (!mapping->mod || PAD_isPressed(BTN_MENU)) {
			if (i==SHORTCUT_TOGGLE_FF) {
				if (PAD_justPressed(btn)) {
					toggled_ff_on = setFastForward(!fast_forward);
					if (mapping->mod) ignore_menu = 1;
					break;
				}
				else if (PAD_justReleased(btn)) {
					if (mapping->mod) ignore_menu = 1;
					break;
				}
			}
			else if (i==SHORTCUT_HOLD_FF) {
				// don't allow turn off fast_forward with a release of the hold button
				// if it was initially turned on with the toggle button
				if (PAD_justPressed(btn) || (!toggled_ff_on && PAD_justReleased(btn))) {
					fast_forward = setFastForward(PAD_isPressed(btn));
					if (mapping->mod) ignore_menu = 1; // very unlikely but just in case
				}
			}
			else if (PAD_justPressed(btn)) {
				switch (i) {
					case SHORTCUT_SAVE_STATE: Menu_saveState(); break;
					case SHORTCUT_LOAD_STATE: Menu_loadState(); break;
					case SHORTCUT_RESET_GAME: core.reset(); break;
					case SHORTCUT_SAVE_QUIT:
						Menu_saveState();
						quit = 1;
						break;
					case SHORTCUT_CYCLE_SCALE:
						screen_scaling += 1;
						int count = config.frontend.options[FE_OPT_SCALING].count;
						if (screen_scaling>=count) screen_scaling -= count;
						Config_syncFrontend(config.frontend.options[FE_OPT_SCALING].key, screen_scaling);
						break;
					case SHORTCUT_CYCLE_EFFECT:
						screen_effect += 1;
						if (screen_effect>=EFFECT_COUNT) screen_effect -= EFFECT_COUNT;
						Config_syncFrontend(config.frontend.options[FE_OPT_EFFECT].key, screen_effect);
						break;
					default: break;
				}

				if (mapping->mod) ignore_menu = 1;
			}
		}
	}

	if (!ignore_menu && PAD_justReleased(BTN_MENU)) {
		show_menu = 1;

		if (thread_video) {
			pthread_mutex_lock(&core_mx);
			should_run_core = 0;
			pthread_mutex_unlock(&core_mx);
		}
	}

	// TODO: figure out how to ignore button when MENU+button is handled first
	// TODO: array size of LOCAL_ whatever that macro is
	// TODO: then split it into two loops
	// TODO: first check for MENU+button
	// TODO: when found mark button the array
	// TODO: then check for button
	// TODO: only modify if absent from array
	// TODO: the shortcuts loop above should also contribute to the array

	buttons = 0;
	for (int i=0; config.controls[i].name; i++) {
		ButtonMapping* mapping = &config.controls[i];
		int btn = 1 << mapping->local;
		if (btn==BTN_NONE) continue; // present buttons can still be unbound
		if (gamepad_type==0) {
			switch(btn) {
				case BTN_DPAD_UP: 		btn = BTN_UP; break;
				case BTN_DPAD_DOWN: 	btn = BTN_DOWN; break;
				case BTN_DPAD_LEFT: 	btn = BTN_LEFT; break;
				case BTN_DPAD_RIGHT: 	btn = BTN_RIGHT; break;
			}
		}
		if (PAD_isPressed(btn) && (!mapping->mod || PAD_isPressed(BTN_MENU))) {
			buttons |= 1 << mapping->retro;
			if (mapping->mod) ignore_menu = 1;
		}
		//  && !PWR_ignoreSettingInput(btn, show_setting)
	}

	// if (buttons) LOG_info("buttons: %i\n", buttons);
}
static int16_t input_state_callback(unsigned port, unsigned device, unsigned index, unsigned id) {
	if (port==0 && device==RETRO_DEVICE_JOYPAD && index==0) {
		if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return buttons;
		return (buttons >> id) & 1;
	}
	else if (port==0 && device==RETRO_DEVICE_ANALOG) {
		if (index==RETRO_DEVICE_INDEX_ANALOG_LEFT) {
			if (id==RETRO_DEVICE_ID_ANALOG_X) return pad.laxis.x;
			else if (id==RETRO_DEVICE_ID_ANALOG_Y) return pad.laxis.y;
		}
		else if (index==RETRO_DEVICE_INDEX_ANALOG_RIGHT) {
			if (id==RETRO_DEVICE_ID_ANALOG_X) return pad.raxis.x;
			else if (id==RETRO_DEVICE_ID_ANALOG_Y) return pad.raxis.y;
		}
	}
	return 0;
}
///////////////////////////////

static void Input_init(const struct retro_input_descriptor *vars) {
	static int input_initialized = 0;
	if (input_initialized) return;

	LOG_info("Input_init\n");

	config.controls = core_button_mapping[0].name ? core_button_mapping : default_button_mapping;

	puts("---------------------------------");

	const char* core_button_names[RETRO_BUTTON_COUNT] = {0};
	int present[RETRO_BUTTON_COUNT];
	int core_mapped = 0;
	if (vars) {
		core_mapped = 1;
		// identify buttons available in this core
		for (int i=0; vars[i].description; i++) {
			const struct retro_input_descriptor* var = &vars[i];
			if (var->port!=0 || var->device!=RETRO_DEVICE_JOYPAD || var->index!=0) continue;

			// TODO: don't ignore unavailable buttons, just override them to BTN_ID_NONE!
			if (var->id>=RETRO_BUTTON_COUNT) {
				printf("UNAVAILABLE: %s\n", var->description); fflush(stdout);
				continue;
			}
			else {
				printf("PRESENT    : %s\n", var->description); fflush(stdout);
			}
			present[var->id] = 1;
			core_button_names[var->id] = var->description;
		}
	}

	puts("---------------------------------");

	for (int i=0;default_button_mapping[i].name; i++) {
		ButtonMapping* mapping = &default_button_mapping[i];
		LOG_info("DEFAULT %s (%s): <%s>\n", core_button_names[mapping->retro], mapping->name, (mapping->local==BTN_ID_NONE ? "NONE" : device_button_names[mapping->local]));
		if (core_button_names[mapping->retro]) mapping->name = (char*)core_button_names[mapping->retro];
	}

	puts("---------------------------------");

	for (int i=0; config.controls[i].name; i++) {
		ButtonMapping* mapping = &config.controls[i];
		mapping->default_ = mapping->local;

		// ignore mappings that aren't available in this core
		if (core_mapped && !present[mapping->retro]) {
			mapping->ignore = 1;
			continue;
		}
		LOG_info("%s: <%s> (%i:%i)\n", mapping->name, (mapping->local==BTN_ID_NONE ? "NONE" : device_button_names[mapping->local]), mapping->local, mapping->retro);
	}

	puts("---------------------------------");
	input_initialized = 1;
}

static bool set_rumble_state(unsigned port, enum retro_rumble_effect effect, uint16_t strength) {
	// TODO: handle other args? not sure I can
	VIB_setStrength(strength);
	return 1;
}
static bool environment_callback(unsigned cmd, void *data) { // copied from picoarch initially
	// LOG_info("environment_callback: %i\n", cmd);

	switch(cmd) {
	// case RETRO_ENVIRONMENT_SET_ROTATION: { /* 1 */
	// 	LOG_info("RETRO_ENVIRONMENT_SET_ROTATION %i\n", *(int *)data); // core requests frontend to handle rotation
	// 	break;
	// }
	case RETRO_ENVIRONMENT_GET_OVERSCAN: { /* 2 */
		bool *out = (bool *)data;
		if (out)
			*out = true;
		break;
	}
	case RETRO_ENVIRONMENT_GET_CAN_DUPE: { /* 3 */
		bool *out = (bool *)data;
		if (out)
			*out = true;
		break;
	}
	case RETRO_ENVIRONMENT_SET_MESSAGE: { /* 6 */
		const struct retro_message *message = (const struct retro_message*)data;
		if (message) LOG_info("%s\n", message->msg);
		break;
	}
	case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL: { /* 8 */
		// puts("RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL");
		// TODO: used by fceumm at least
		break;
	}
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: { /* 9 */
		const char **out = (const char **)data;
		if (out) {
			*out = core.bios_dir;
		}
		break;
	}
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: { /* 10 */
		const enum retro_pixel_format *format = (enum retro_pixel_format *)data;

		if (*format != RETRO_PIXEL_FORMAT_RGB565) { // TODO: pull from platform.h?
			/* 565 is only supported format */
			return false;
		}
		break;
	}
	case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS: { /* 11 */
		// puts("RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS\n");
		Input_init((const struct retro_input_descriptor *)data);
		return false;
	} break;
	case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE: { /* 13 */
		const struct retro_disk_control_callback *var =
			(const struct retro_disk_control_callback *)data;

		if (var) {
			memset(&disk_control_ext, 0, sizeof(struct retro_disk_control_ext_callback));
			memcpy(&disk_control_ext, var, sizeof(struct retro_disk_control_callback));
		}
		break;
	}

	// TODO: this is called whether using variables or options
	case RETRO_ENVIRONMENT_GET_VARIABLE: { /* 15 */
		// puts("RETRO_ENVIRONMENT_GET_VARIABLE ");
		struct retro_variable *var = (struct retro_variable *)data;
		if (var && var->key) {
			var->value = OptionList_getOptionValue(&config.core, var->key);
			// printf("\t%s = %s\n", var->key, var->value);
		}
		// fflush(stdout);
		break;
	}
	// TODO: I think this is where the core reports its variables (the precursor to options)
	// TODO: this is called if RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION sets out to 0
	// TODO: not used by anything yet
	case RETRO_ENVIRONMENT_SET_VARIABLES: { /* 16 */
		// puts("RETRO_ENVIRONMENT_SET_VARIABLES");
		const struct retro_variable *vars = (const struct retro_variable *)data;
		if (vars) {
			OptionList_reset();
			OptionList_vars(vars);
		}
		break;
	}
	case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME: { /* 18 */
		bool flag = *(bool*)data;
		// LOG_info("%i: RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME: %i\n", cmd, flag);
		break;
	}
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: { /* 17 */
		bool *out = (bool *)data;
		if (out) {
			*out = config.core.changed;
			config.core.changed = 0;
		}
		break;
	}
	case RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK: { /* 21 */
		// LOG_info("%i: RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK\n", cmd);
		break;
	}
	case RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK: { /* 22 */
		// LOG_info("%i: RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK\n", cmd);
		break;
	}
	case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE: { /* 23 */
	        struct retro_rumble_interface *iface = (struct retro_rumble_interface*)data;

	        // LOG_info("Setup rumble interface.\n");
	        iface->set_rumble_state = set_rumble_state;
		break;
	}
	case RETRO_ENVIRONMENT_GET_INPUT_DEVICE_CAPABILITIES: {
		unsigned *out = (unsigned *)data;
		if (out)
			*out = (1 << RETRO_DEVICE_JOYPAD) | (1 << RETRO_DEVICE_ANALOG);
		break;
	}
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: { /* 27 */
		struct retro_log_callback *log_cb = (struct retro_log_callback *)data;
		if (log_cb)
			log_cb->log = (void (*)(enum retro_log_level, const char*, ...))LOG_note; // same difference
		break;
	}
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: { /* 31 */
		const char **out = (const char **)data;
		if (out)
			*out = core.saves_dir; // save_dir;
		break;
	}
	case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO: { /* 35 */
		// LOG_info("RETRO_ENVIRONMENT_SET_CONTROLLER_INFO\n");
		const struct retro_controller_info *infos = (const struct retro_controller_info *)data;
		if (infos) {
			// TODO: store to gamepad_values/gamepad_labels for gamepad_device
			const struct retro_controller_info *info = &infos[0];
			for (int i=0; i<info->num_types; i++) {
				const struct retro_controller_description *type = &info->types[i];
				if (exactMatch((char*)type->desc,"dualshock")) { // currently only enabled for PlayStation
					has_custom_controllers = 1;
					break;
				}
				// printf("\t%i: %s\n", type->id, type->desc);
			}
		}
		fflush(stdout);
		return false; // TODO: tmp
		break;
	}
	// RETRO_ENVIRONMENT_SET_MEMORY_MAPS (36 | RETRO_ENVIRONMENT_EXPERIMENTAL)
	// RETRO_ENVIRONMENT_GET_LANGUAGE 39
	case RETRO_ENVIRONMENT_GET_CURRENT_SOFTWARE_FRAMEBUFFER: { /* (40 | RETRO_ENVIRONMENT_EXPERIMENTAL) */
		// puts("RETRO_ENVIRONMENT_GET_CURRENT_SOFTWARE_FRAMEBUFFER");
		break;
	}

	case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE: {
		// fixes fbneo save state graphics corruption
		// puts("RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE");
		int *out_p = (int *)data;
		if (out_p) {
			int out = 0;
			out |= RETRO_AV_ENABLE_VIDEO;
			out |= RETRO_AV_ENABLE_AUDIO;
			*out_p = out;
		}
		break;
	}

	// RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS (42 | RETRO_ENVIRONMENT_EXPERIMENTAL)
	// RETRO_ENVIRONMENT_GET_VFS_INTERFACE (45 | RETRO_ENVIRONMENT_EXPERIMENTAL)
	// RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE (47 | RETRO_ENVIRONMENT_EXPERIMENTAL)
	// RETRO_ENVIRONMENT_GET_INPUT_BITMASKS (51 | RETRO_ENVIRONMENT_EXPERIMENTAL)
	case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS: { /* 51 | RETRO_ENVIRONMENT_EXPERIMENTAL */
		bool *out = (bool *)data;
		if (out)
			*out = true;
		break;
	}
	case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION: { /* 52 */
		// puts("RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION");
		unsigned *out = (unsigned *)data;
		if (out)
			*out = 1;
		break;
	}
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS: { /* 53 */
		// puts("RETRO_ENVIRONMENT_SET_CORE_OPTIONS");
		if (data) {
			OptionList_reset();
			OptionList_init((const struct retro_core_option_definition *)data);
		}
		break;
	}
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL: { /* 54 */
		// puts("RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL");
		const struct retro_core_options_intl *options = (const struct retro_core_options_intl *)data;
		if (options && options->us) {
			OptionList_reset();
			OptionList_init(options->us);
		}
		break;
	}
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY: { /* 55 */
		// puts("RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY");
		// const struct retro_core_option_display *display = (const struct retro_core_option_display *)data;
	// 	if (display) OptionList_setOptionVisibility(&config.core, display->key, display->visible);
		break;
	}
	case RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION: { /* 57 */
		unsigned *out =	(unsigned *)data;
		if (out) *out = 1;
		break;
	}
	case RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE: { /* 58 */
		const struct retro_disk_control_ext_callback *var =
			(const struct retro_disk_control_ext_callback *)data;

		if (var) {
			memcpy(&disk_control_ext, var, sizeof(struct retro_disk_control_ext_callback));
		}
		break;
	}
	// TODO: RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION 59
	// TODO: used by mgba, (but only during frameskip?)
	// case RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK: { /* 62 */
	// 	LOG_info("RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK\n");
	// 	const struct retro_audio_buffer_status_callback *cb = (const struct retro_audio_buffer_status_callback *)data;
	// 	if (cb) {
	// 		LOG_info("has audo_buffer_status callback\n");
	// 		core.audio_buffer_status = cb->callback;
	// 	} else {
	// 		LOG_info("no audo_buffer_status callback\n");
	// 		core.audio_buffer_status = NULL;
	// 	}
	// 	break;
	// }
	// TODO: used by mgba, (but only during frameskip?)
	// case RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY: { /* 63 */
	// 	LOG_info("RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY\n");
	//
	// 	const unsigned *latency_ms = (const unsigned *)data;
	// 	if (latency_ms) {
	// 		unsigned frames = *latency_ms * core.fps / 1000;
	// 		if (frames < 30)
	// 			// audio_buffer_size_override = frames;
	// 			LOG_info("audio_buffer_size_override = %i (unused?)\n", frames);
	// 		else
	// 			LOG_info("Audio buffer change out of range (%d), ignored\n", frames);
	// 	}
	// 	break;
	// }

	// TODO: RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE 64
	case RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE: { /* 65 */
		// const struct retro_system_content_info_override* info = (const struct retro_system_content_info_override* )data;
		// if (info) LOG_info("has overrides");
		break;
	}
	// RETRO_ENVIRONMENT_GET_GAME_INFO_EXT 66
	// TODO: RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK 69
	// used by fceumm
	// TODO: used by gambatte for L/R palette switching (seems like it needs to return true even if data is NULL to indicate support)
	case RETRO_ENVIRONMENT_SET_VARIABLE: {
		// puts("RETRO_ENVIRONMENT_SET_VARIABLE");
		const struct retro_variable *var = (const struct retro_variable *)data;
		if (var && var->key) {
			// printf("\t%s = %s\n", var->key, var->value);
			OptionList_setOptionValue(&config.core, var->key, var->value);
			break;
		}

		int *out = (int *)data;
		if (out) *out = 1;

		break;
	}

	// unused
	// case RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK: {
	// 	puts("RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK"); fflush(stdout);
	// 	break;
	// }
	// case RETRO_ENVIRONMENT_GET_THROTTLE_STATE: {
	// 	puts("RETRO_ENVIRONMENT_GET_THROTTLE_STATE"); fflush(stdout);
	// 	break;
	// }
	// case RETRO_ENVIRONMENT_GET_FASTFORWARDING: {
	// 	puts("RETRO_ENVIRONMENT_GET_FASTFORWARDING"); fflush(stdout);
	// 	break;
	// };

	default:
		// LOG_debug("Unsupported environment cmd: %u\n", cmd);
		return false;
	}
	return true;
}

///////////////////////////////

void hdmimon(void) {
	// handle HDMI change
	static int had_hdmi = -1;
	int has_hdmi = GetHDMI();
	if (had_hdmi==-1) had_hdmi = has_hdmi;
	if (has_hdmi!=had_hdmi) {
		had_hdmi = has_hdmi;

		LOG_info("restarting after HDMI change...\n");
		Menu_beforeSleep();
		sleep(4);
		show_menu = 0;
		quit = 1;
	}
}

///////////////////////////////

// TODO: this is a dumb API
SDL_Surface* digits;
#define DIGIT_WIDTH 9
#define DIGIT_HEIGHT 8
#define DIGIT_TRACKING -2
enum {
	DIGIT_SLASH = 10,
	DIGIT_DOT,
	DIGIT_PERCENT,
	DIGIT_X,
	DIGIT_OP, // (
	DIGIT_CP, // )
	DIGIT_COUNT,
};
#define DIGIT_SPACE DIGIT_COUNT
static void MSG_init(void) {
	digits = SDL_CreateRGBSurface(SDL_SWSURFACE,SCALE2(DIGIT_WIDTH*DIGIT_COUNT,DIGIT_HEIGHT),FIXED_DEPTH, 0,0,0,0);
	SDL_FillRect(digits, NULL, RGB_BLACK);

	SDL_Surface* digit;
	char* chars[] = { "0","1","2","3","4","5","6","7","8","9","/",".","%","x","(",")", NULL };
	char* c;
	int i = 0;
	while ((c = chars[i])) {
		digit = TTF_RenderUTF8_Blended(font.tiny, c, COLOR_WHITE);
		SDL_BlitSurface(digit, NULL, digits, &(SDL_Rect){ (i * SCALE1(DIGIT_WIDTH)) + (SCALE1(DIGIT_WIDTH) - digit->w)/2, (SCALE1(DIGIT_HEIGHT) - digit->h)/2});
		SDL_FreeSurface(digit);
		i += 1;
	}
}
static int MSG_blitChar(int n, int x, int y) {
	if (n!=DIGIT_SPACE) SDL_BlitSurface(digits, &(SDL_Rect){n*SCALE1(DIGIT_WIDTH),0,SCALE2(DIGIT_WIDTH,DIGIT_HEIGHT)}, screen, &(SDL_Rect){x,y});
	return x + SCALE1(DIGIT_WIDTH + DIGIT_TRACKING);
}
static int MSG_blitInt(int num, int x, int y) {
	int i = num;
	int n;

	if (i > 999) {
		n = i / 1000;
		i -= n * 1000;
		x = MSG_blitChar(n,x,y);
	}
	if (i > 99) {
		n = i / 100;
		i -= n * 100;
		x = MSG_blitChar(n,x,y);
	}
	else if (num>99) {
		x = MSG_blitChar(0,x,y);
	}
	if (i > 9) {
		n = i / 10;
		i -= n * 10;
		x = MSG_blitChar(n,x,y);
	}
	else if (num>9) {
		x = MSG_blitChar(0,x,y);
	}

	n = i;
	x = MSG_blitChar(n,x,y);

	return x;
}
static int MSG_blitDouble(double num, int x, int y) {
	int i = num;
	int r = (num-i) * 10;
	int n;

	x = MSG_blitInt(i, x,y);

	n = DIGIT_DOT;
	x = MSG_blitChar(n,x,y);

	n = r;
	x = MSG_blitChar(n,x,y);
	return x;
}
static void MSG_quit(void) {
	SDL_FreeSurface(digits);
}

///////////////////////////////

static const char* bitmap_font[] = {
	['0'] =
		" 111 "
		"1   1"
		"1   1"
		"1  11"
		"1 1 1"
		"11  1"
		"1   1"
		"1   1"
		" 111 ",
	['1'] =
		"   1 "
		" 111 "
		"   1 "
		"   1 "
		"   1 "
		"   1 "
		"   1 "
		"   1 "
		"   1 ",
	['2'] =
		" 111 "
		"1   1"
		"    1"
		"   1 "
		"  1  "
		" 1   "
		"1    "
		"1    "
		"11111",
	['3'] =
		" 111 "
		"1   1"
		"    1"
		"    1"
		" 111 "
		"    1"
		"    1"
		"1   1"
		" 111 ",
	['4'] =
		"1   1"
		"1   1"
		"1   1"
		"1   1"
		"1   1"
		"1   1"
		"11111"
		"    1"
		"    1",
	['5'] =
		"11111"
		"1    "
		"1    "
		"1111 "
		"    1"
		"    1"
		"    1"
		"1   1"
		" 111 ",
	['6'] =
		" 111 "
		"1    "
		"1    "
		"1111 "
		"1   1"
		"1   1"
		"1   1"
		"1   1"
		" 111 ",
	['7'] =
		"11111"
		"    1"
		"    1"
		"   1 "
		"  1  "
		"  1  "
		"  1  "
		"  1  "
		"  1  ",
	['8'] =
		" 111 "
		"1   1"
		"1   1"
		"1   1"
		" 111 "
		"1   1"
		"1   1"
		"1   1"
		" 111 ",
	['9'] =
		" 111 "
		"1   1"
		"1   1"
		"1   1"
		"1   1"
		" 1111"
		"    1"
		"    1"
		" 111 ",
	['.'] =
		"     "
		"     "
		"     "
		"     "
		"     "
		"     "
		"     "
		" 11  "
		" 11  ",
	[','] =
		"     "
		"     "
		"     "
		"     "
		"     "
		"     "
		"  1  "
		"  1  "
		" 1   ",
	[' '] =
		"     "
		"     "
		"     "
		"     "
		"     "
		"     "
		"     "
		"     "
		"     ",
	['('] =
		"   1 "
		"  1  "
		" 1   "
		" 1   "
		" 1   "
		" 1   "
		" 1   "
		"  1  "
		"   1 ",
	[')'] =
		" 1   "
		"  1  "
		"   1 "
		"   1 "
		"   1 "
		"   1 "
		"   1 "
		"  1  "
		" 1   ",
	['/'] =
		"   1 "
		"   1 "
		"   1 "
		"  1  "
		"  1  "
		"  1  "
		" 1   "
		" 1   "
		" 1   ",
	['x'] =
		"     "
		"     "
		"1   1"
		"1   1"
		" 1 1 "
		"  1  "
		" 1 1 "
		"1   1"
		"1   1",
	['%'] =
		" 1   "
		"1 1  "
		"1 1 1"
		" 1 1 "
		"  1  "
		" 1 1 "
		"1 1 1"
		"  1 1"
		"   1 ",
	['-'] =
		"     "
		"     "
		"     "
		"     "
		" 111 "
		"     "
		"     "
		"     "
		"     ",
	};
static void blitBitmapText(char* text, int ox, int oy, uint16_t* data, int stride, int width, int height) {
	#define CHAR_WIDTH 5
	#define CHAR_HEIGHT 9
	#define LETTERSPACING 1

	int len = strlen(text);
	int w = ((CHAR_WIDTH+LETTERSPACING)*len)-1;
	int h = CHAR_HEIGHT;

	if (ox<0) ox = width-w+ox;
	if (oy<0) oy = height-h+oy;

	data += oy * stride + ox;
	uint16_t* row = data - stride; // TODO: this will crash and burn if ox,oy==0,0 but is fine as used currently :sweat_smile:
	memset(row-1, 0, (w+2)*2);
	for (int y=0; y<CHAR_HEIGHT; y++) {
		row = data + y * stride;
		memset(row-1, 0, (w+2)*2);
		for (int i=0; i<len; i++) {
			const char* c = bitmap_font[text[i]];
			for (int x=0; x<CHAR_WIDTH; x++) {
				int j = y * CHAR_WIDTH + x;
				if (c[j]=='1') *row = 0xffff;
				row++;
			}
			row += LETTERSPACING;
		}
	}
	row = data + CHAR_HEIGHT * stride;
	memset(row-1, 0, (w+2)*2);
}

///////////////////////////////

static int cpu_ticks = 0;
static int fps_ticks = 0;
static int use_ticks = 0;
static double fps_double = 0;
static double cpu_double = 0;
static double use_double = 0;
static uint32_t sec_start = 0;

void selectScaler(int src_w, int src_h, int src_p) {
	LOG_info("selectScaler\n");

	int src_x,src_y,dst_x,dst_y,dst_w,dst_h,dst_p,scale;
	double aspect;

	int aspect_w = src_w;
	int aspect_h = CEIL_DIV(aspect_w, core.aspect_ratio);

	// TODO: make sure this doesn't break fit==1 devices
	if (aspect_h<src_h) {
		aspect_h = src_h;
		aspect_w = aspect_h * core.aspect_ratio;
		aspect_w += aspect_w % 2;
	}

	char scaler_name[16];

	src_x = 0;
	src_y = 0;
	dst_x = 0;
	dst_y = 0;

	// unmodified by crop
	renderer.true_w = src_w;
	renderer.true_h = src_h;

	// TODO: this is saving non-rgb30 devices from themselves...or rather, me
	int scaling = screen_scaling;
	if (scaling==SCALE_CROPPED && DEVICE_WIDTH==HDMI_WIDTH) {
		scaling = SCALE_NATIVE;
	}

	if (scaling==SCALE_NATIVE || scaling==SCALE_CROPPED) {
		// this is the same whether fit or oversized
		scale = MIN(DEVICE_WIDTH/src_w, DEVICE_HEIGHT/src_h);
		if (!scale) {
			sprintf(scaler_name, "forced crop");
			dst_w = DEVICE_WIDTH;
			dst_h = DEVICE_HEIGHT;
			dst_p = DEVICE_PITCH;

			int ox = (DEVICE_WIDTH  - src_w) / 2; // may be negative
			int oy = (DEVICE_HEIGHT - src_h) / 2; // may be negative

			if (ox<0) src_x = -ox;
			else dst_x = ox;

			if (oy<0) src_y = -oy;
			else dst_y = oy;
		}
		// TODO: this is all kinds of messed up
		// TODO: is this blowing up because the smart has to rotate before scaling?
		// TODO: or is it just that I'm trying to cram 4 logical rects into 2 rect arguments
		// TODO: eg. src.size + src.clip + dst.size + dst.clip
		else if (scaling==SCALE_CROPPED) {
			int scale_x = CEIL_DIV(DEVICE_WIDTH, src_w);
			int scale_y = CEIL_DIV(DEVICE_HEIGHT, src_h);
			scale = MIN(scale_x, scale_y);

			sprintf(scaler_name, "cropped");
			dst_w = DEVICE_WIDTH;
			dst_h = DEVICE_HEIGHT;
			dst_p = DEVICE_PITCH;

			int scaled_w = src_w * scale;
			int scaled_h = src_h * scale;

			int ox = (DEVICE_WIDTH  - scaled_w) / 2; // may be negative
			int oy = (DEVICE_HEIGHT - scaled_h) / 2; // may be negative

			if (ox<0) {
				src_x = -ox / scale;
				src_w -= src_x * 2;
			}
			else {
				dst_x = ox;
				// dst_w -= ox * 2;
			}

			if (oy<0) {
				src_y = -oy / scale;
				src_h -= src_y * 2;
			}
			else {
				dst_y = oy;
				// dst_h -= oy * 2;
			}
		}
		else {
			sprintf(scaler_name, "integer");
			int scaled_w = src_w * scale;
			int scaled_h = src_h * scale;
			dst_w = DEVICE_WIDTH;
			dst_h = DEVICE_HEIGHT;
			dst_p = DEVICE_PITCH;
			dst_x = (DEVICE_WIDTH  - scaled_w) / 2; // should always be positive
			dst_y = (DEVICE_HEIGHT - scaled_h) / 2; // should always be positive
		}
	}
	else {
		int scale_x = CEIL_DIV(DEVICE_WIDTH, src_w);
		int scale_y = CEIL_DIV(DEVICE_HEIGHT,src_h);

		// odd resolutions (eg. PS1 Rayman: 320x239) is throwing this off, need to snap to eights
		int r = (DEVICE_HEIGHT-src_h)%8;
		if (r && r<8) scale_y -= 1;

		scale = MAX(scale_x, scale_y);
		// if (scale>4) scale = 4;
		// if (scale>2) scale = 4; // TODO: restore, requires sanity checking

		int scaled_w = src_w * scale;
		int scaled_h = src_h * scale;

		if (scaling==SCALE_FULLSCREEN) {
			sprintf(scaler_name, "full%i", scale);
			// type = 'full (oversized)';
			dst_w = scaled_w;
			dst_h = scaled_h;
			dst_p = dst_w * FIXED_BPP;
		}
		else {
			double src_aspect_ratio = ((double)src_w) / src_h;
			// double core_aspect_ratio
			double fixed_aspect_ratio = ((double)DEVICE_WIDTH) / DEVICE_HEIGHT;
			int core_aspect = core.aspect_ratio * 1000;
			int fixed_aspect = fixed_aspect_ratio * 1000;

			// still having trouble with FC's 1.306 (13/10? wtf) on 4:3 devices
			// specifically I think it has trouble when src, core, and fixed
			// ratios don't match

			// it handles src and core matching but fixed not, eg. GB and GBA
			// or core and fixed matching but not src, eg. odd PS resolutions

			// we need to transform the src size to core aspect
			// then to fixed aspect

			if (core_aspect>fixed_aspect) {
				sprintf(scaler_name, "aspect%iL", scale);
				// letterbox
				// dst_w = scaled_w;
				// dst_h = scaled_w / fixed_aspect_ratio;
				// dst_h += dst_h%2;
				int aspect_h = DEVICE_WIDTH / core.aspect_ratio;
				double aspect_hr = ((double)aspect_h) / DEVICE_HEIGHT;
				dst_w = scaled_w;
				dst_h = scaled_h / aspect_hr;

				dst_y = (dst_h - scaled_h) / 2;
			}
			else if (core_aspect<fixed_aspect) {
				sprintf(scaler_name, "aspect%iP", scale);
				// pillarbox
				// dst_w = scaled_h * fixed_aspect_ratio;
				// dst_w += dst_w%2;
				// dst_h = scaled_h;
				aspect_w = DEVICE_HEIGHT * core.aspect_ratio;
				double aspect_wr = ((double)aspect_w) / DEVICE_WIDTH;
				dst_w = scaled_w / aspect_wr;
				dst_h = scaled_h;

				dst_w = (dst_w/8)*8;
				dst_x = (dst_w - scaled_w) / 2;
			}
			else {
				sprintf(scaler_name, "aspect%iM", scale);
				// perfect match
				dst_w = scaled_w;
				dst_h = scaled_h;
			}
			dst_p = dst_w * FIXED_BPP;
		}
	}

	// TODO: need to sanity check scale and demands on the buffer

	// LOG_info("aspect: %ix%i (%f)\n", aspect_w,aspect_h,core.aspect_ratio);

	renderer.src_x = src_x;
	renderer.src_y = src_y;
	renderer.src_w = src_w;
	renderer.src_h = src_h;
	renderer.src_p = src_p;
	renderer.dst_x = dst_x;
	renderer.dst_y = dst_y;
	renderer.dst_w = dst_w;
	renderer.dst_h = dst_h;
	renderer.dst_p = dst_p;
	renderer.scale = scale;
	renderer.aspect = (scaling==SCALE_NATIVE||scaling==SCALE_CROPPED)?0:(scaling==SCALE_FULLSCREEN?-1:core.aspect_ratio);
	LOG_info("aspect: %f\n", renderer.aspect);
	renderer.blit = GFX_getScaler(&renderer);

	// LOG_info("coreAR:%0.3f fixedAR:%0.3f srcAR: %0.3f\nname:%s\nfit:%i scale:%i\nsrc_x:%i src_y:%i src_w:%i src_h:%i src_p:%i\ndst_x:%i dst_y:%i dst_w:%i dst_h:%i dst_p:%i\naspect_w:%i aspect_h:%i\n",
	// 	core.aspect_ratio, ((double)DEVICE_WIDTH) / DEVICE_HEIGHT, ((double)src_w) / src_h,
	// 	scaler_name,
	// 	fit,scale,
	// 	src_x,src_y,src_w,src_h,src_p,
	// 	dst_x,dst_y,dst_w,dst_h,dst_p,
	// 	aspect_w,aspect_h
	// );

	// if (screen->w!=dst_w || screen->h!=dst_w || screen->pitch!=dst_p) {
		screen = GFX_resize(dst_w,dst_h,dst_p);
	// }
}
static void video_refresh_callback_main(const void *data, unsigned width, unsigned height, size_t pitch) {
	// return;

	Special_render();

	// static int tmp_frameskip = 0;
	// if ((tmp_frameskip++)%2) return;

	static uint32_t last_flip_time = 0;

	// 10 seems to be the sweet spot that allows 2x in NES and SNES and 8x in GB at 60fps
	// 14 will let GB hit 10x but NES and SNES will drop to 1.5x at 30fps (not sure why)
	// but 10 hurts PS...
	// TODO: 10 was based on rg35xx, probably different results on other supported platforms
	if (fast_forward && SDL_GetTicks()-last_flip_time<10) return;

	// FFVII menus
	// 16: 30/200
	// 15: 30/180
	// 14: 45/180
	// 12: 30/150
	// 10: 30/120 (optimize text off has no effect)
	//  8: 60/210 (with optimize text off)
	// you can squeeze more out of every console by turning prevent tearing off
	// eg. PS@10 60/240

	if (!data) return;

	fps_ticks += 1;

	// if source has changed size (or forced by dst_p==0)
	// eg. true src + cropped src + fixed dst + cropped dst
	if (renderer.dst_p==0 || width!=renderer.true_w || height!=renderer.true_h) {
		selectScaler(width, height, pitch);
		GFX_clearAll();
	}

	// debug
	if (show_debug) {
		int x = 2 + renderer.src_x;
		int y = 2 + renderer.src_y;
		char debug_text[128];
		int scale = renderer.scale;
		if (scale==-1) scale = 1; // nearest neighbor flag

		sprintf(debug_text, "%ix%i %ix", renderer.src_w,renderer.src_h, scale);
		blitBitmapText(debug_text,x,y,(uint16_t*)data,pitch/2, width,height);

		sprintf(debug_text, "%i,%i %ix%i", renderer.dst_x,renderer.dst_y, renderer.src_w*scale,renderer.src_h*scale);
		blitBitmapText(debug_text,-x,y,(uint16_t*)data,pitch/2, width,height);

		sprintf(debug_text, "%.01f/%.01f %i%%", fps_double, cpu_double, (int)use_double);
		blitBitmapText(debug_text,x,-y,(uint16_t*)data,pitch/2, width,height);

		sprintf(debug_text, "%ix%i", renderer.dst_w,renderer.dst_h);
		blitBitmapText(debug_text,-x,-y,(uint16_t*)data,pitch/2, width,height);
	}

	renderer.src = (void*)data;
	renderer.dst = screen->pixels;
	// LOG_info("video_refresh_callback: %ix%i@%i %ix%i@%i\n",width,height,pitch,screen->w,screen->h,screen->pitch);

	// On framebuffer platforms (rg35xx) the settings overlay is drawn straight into the
	// persistent framebuffer; the parts that land in the letterbox bars aren't covered by
	// the game blit, so they linger after the overlay times out. Wipe the whole frame for a
	// couple of pages once it disappears (harmless on the GPU path, which clears every frame).
	if (show_setting) overlay_clear = 2;
	else if (overlay_clear>0) { GFX_clearAll(); overlay_clear--; }

	GFX_blitRenderer(&renderer);

	// Draw the brightness/volume overlay over the game frame (M+volume / volume rocker).
	// Framebuffer platforms (rg35xx/SDL1.2) present the screen surface, so drawing onto it
	// here is enough. GPU platforms (rg35xxplus/SDL2) upload the raw game buffer straight to
	// a texture and bypass the screen surface, so PLAT_setHardwareGroup arms a device-resolution
	// overlay that PLAT_flip composites over the game. Called every frame (0 clears it).
	if (show_setting) {
		GFX_blitHardwareGroup(screen, show_setting);
		if (!GetHDMI()) GFX_blitHardwareHints(screen, show_setting);
	}
	PLAT_setHardwareGroup(show_setting);

	if (!thread_video) GFX_flip(screen);
	last_flip_time = SDL_GetTicks();
}
void video_refresh_callback(const void *data, unsigned width, unsigned height, size_t pitch) {
	if (!data) return;

	if (thread_video) {
		pthread_mutex_lock(&core_mx);

		if (backbuffer && (backbuffer->w!=width || backbuffer->h!=height || backbuffer->pitch!=pitch)) {
			free(backbuffer->pixels);
			SDL_FreeSurface(backbuffer);
			backbuffer = NULL;
		}
		if (!backbuffer) {
			uint16_t* pixels = malloc(height*pitch);
			// backbuffer = SDL_CreateRGBSurface(0,width,height,FIXED_DEPTH,RGBA_MASK_565);
			backbuffer = SDL_CreateRGBSurfaceFrom(pixels, width, height, FIXED_DEPTH, pitch, RGBA_MASK_565);
		}

		memcpy(backbuffer->pixels, data, backbuffer->h*backbuffer->pitch);

		frame_ready = 1;
		pthread_cond_signal(&core_rq);
		pthread_mutex_unlock(&core_mx);
	}
	else video_refresh_callback_main(data,width,height,pitch);
}
///////////////////////////////

// NOTE: sound must be disabled for fast forward to work...
static void audio_sample_callback(int16_t left, int16_t right) {
	if (!fast_forward) SND_batchSamples(&(const SND_Frame){left,right}, 1);
}
static size_t audio_sample_batch_callback(const int16_t *data, size_t frames) {
	if (!fast_forward) return SND_batchSamples((const SND_Frame*)data, frames);
	else return frames;
	// return frames;
};

///////////////////////////////////////

void Core_getName(char* in_name, char* out_name) {
	strcpy(out_name, basename(in_name));
	char* tmp = strrchr(out_name, '_');
	tmp[0] = '\0';
}
void Core_open(const char* core_path, const char* tag_name) {
	LOG_info("Core_open\n");
	core.handle = dlopen(core_path, RTLD_LAZY);

	if (!core.handle) LOG_error("%s\n", dlerror());

	core.init = dlsym(core.handle, "retro_init");
	core.deinit = dlsym(core.handle, "retro_deinit");
	core.get_system_info = dlsym(core.handle, "retro_get_system_info");
	core.get_system_av_info = dlsym(core.handle, "retro_get_system_av_info");
	core.set_controller_port_device = dlsym(core.handle, "retro_set_controller_port_device");
	core.reset = dlsym(core.handle, "retro_reset");
	core.run = dlsym(core.handle, "retro_run");
	core.serialize_size = dlsym(core.handle, "retro_serialize_size");
	core.serialize = dlsym(core.handle, "retro_serialize");
	core.unserialize = dlsym(core.handle, "retro_unserialize");
	core.load_game = dlsym(core.handle, "retro_load_game");
	core.load_game_special = dlsym(core.handle, "retro_load_game_special");
	core.unload_game = dlsym(core.handle, "retro_unload_game");
	core.get_region = dlsym(core.handle, "retro_get_region");
	core.get_memory_data = dlsym(core.handle, "retro_get_memory_data");
	core.get_memory_size = dlsym(core.handle, "retro_get_memory_size");

	void (*set_environment_callback)(retro_environment_t);
	void (*set_video_refresh_callback)(retro_video_refresh_t);
	void (*set_audio_sample_callback)(retro_audio_sample_t);
	void (*set_audio_sample_batch_callback)(retro_audio_sample_batch_t);
	void (*set_input_poll_callback)(retro_input_poll_t);
	void (*set_input_state_callback)(retro_input_state_t);

	set_environment_callback = dlsym(core.handle, "retro_set_environment");
	set_video_refresh_callback = dlsym(core.handle, "retro_set_video_refresh");
	set_audio_sample_callback = dlsym(core.handle, "retro_set_audio_sample");
	set_audio_sample_batch_callback = dlsym(core.handle, "retro_set_audio_sample_batch");
	set_input_poll_callback = dlsym(core.handle, "retro_set_input_poll");
	set_input_state_callback = dlsym(core.handle, "retro_set_input_state");

	struct retro_system_info info = {};
	core.get_system_info(&info);

	Core_getName((char*)core_path, (char*)core.name);
	sprintf((char*)core.version, "%s (%s)", info.library_name, info.library_version);
	strcpy((char*)core.tag, tag_name);
	strcpy((char*)core.extensions, info.valid_extensions);

	core.need_fullpath = info.need_fullpath;

	LOG_info("core: %s version: %s tag: %s (valid_extensions: %s need_fullpath: %i)\n", core.name, core.version, core.tag, info.valid_extensions, info.need_fullpath);

	sprintf((char*)core.config_dir, USERDATA_PATH "/%s-%s", core.tag, core.name);
	sprintf((char*)core.states_dir, SHARED_USERDATA_PATH "/%s-%s", core.tag, core.name);
	sprintf((char*)core.saves_dir, SDCARD_PATH "/Saves/%s", core.tag);
	sprintf((char*)core.bios_dir, SDCARD_PATH "/Bios/%s", core.tag);

	char cmd[512];
	sprintf(cmd, "mkdir -p \"%s\"; mkdir -p \"%s\"", core.config_dir, core.states_dir);
	system(cmd);

	set_environment_callback(environment_callback);
	set_video_refresh_callback(video_refresh_callback);
	set_audio_sample_callback(audio_sample_callback);
	set_audio_sample_batch_callback(audio_sample_batch_callback);
	set_input_poll_callback(input_poll_callback);
	set_input_state_callback(input_state_callback);
}
void Core_init(void) {
	LOG_info("Core_init\n");
	core.init();
	core.initialized = 1;
}
void Core_load(void) {
	LOG_info("Core_load\n");
	struct retro_game_info game_info;
	game_info.path = game.tmp_path[0]?game.tmp_path:game.path;
	game_info.data = game.data;
	game_info.size = game.size;
	LOG_info("game path: %s (%i)\n", game_info.path, game.size);

	core.load_game(&game_info);

	SRAM_read();
	RTC_read();

	// NOTE: must be called after core.load_game!
	struct retro_system_av_info av_info = {};
	core.get_system_av_info(&av_info);
	core.set_controller_port_device(0, RETRO_DEVICE_JOYPAD); // set a default, may update after loading configs

	core.fps = av_info.timing.fps;
	core.sample_rate = av_info.timing.sample_rate;
	double a = av_info.geometry.aspect_ratio;
	if (a<=0) a = (double)av_info.geometry.base_width / av_info.geometry.base_height;
	core.aspect_ratio = a;

	LOG_info("aspect_ratio: %f (%ix%i) fps: %f\n", a, av_info.geometry.base_width,av_info.geometry.base_height, core.fps);
}
void Core_reset(void) {
	core.reset();
}
void Core_unload(void) {
	SND_quit();
}
void Core_quit(void) {
	if (core.initialized) {
		SRAM_write();
		RTC_write();
		core.unload_game();
		core.deinit();
		core.initialized = 0;
	}
}
void Core_close(void) {
	if (core.handle) dlclose(core.handle);
}

///////////////////////////////////////

// TODO: move to PWR_*?
static unsigned getUsage(void) { // from picoarch
	long unsigned ticks = 0;
	long ticksps = 0;
	FILE *file = NULL;

	file = fopen("/proc/self/stat", "r");
	if (!file)
		goto finish;

	if (!fscanf(file, "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu", &ticks))
		goto finish;

	ticksps = sysconf(_SC_CLK_TCK);

	if (ticksps)
		ticks = ticks * 100 / ticksps;

finish:
	if (file)
		fclose(file);

	return ticks;
}

static void trackFPS(void) {
	cpu_ticks += 1;
	static int last_use_ticks = 0;
	uint32_t now = SDL_GetTicks();
	if (now - sec_start>=1000) {
		double last_time = (double)(now - sec_start) / 1000;
		fps_double = fps_ticks / last_time;
		cpu_double = cpu_ticks / last_time;
		use_ticks = getUsage();
		if (use_ticks && last_use_ticks) {
			use_double = (use_ticks - last_use_ticks) / last_time;
		}
		last_use_ticks = use_ticks;
		sec_start = now;
		cpu_ticks = 0;
		fps_ticks = 0;

		// LOG_info("fps: %f cpu: %f\n", fps_double, cpu_double);
	}
}

static void limitFF(void) {
	static uint64_t ff_frame_time = 0;
	static uint64_t last_time = 0;
	static int last_max_speed = -1;

	if (!fast_forward || !max_ff_speed) {
		last_time = 0; // reset pacing; re-primed on the first fast-forwarded frame
		return; // keeps getMicroseconds()'s gettimeofday syscall out of the normal per-frame path
	}

	if (last_max_speed!=max_ff_speed) {
		last_max_speed = max_ff_speed;
		ff_frame_time = 1000000 / (core.fps * (max_ff_speed + 1));
	}

	uint64_t now = getMicroseconds();
	if (last_time == 0) last_time = now;
	int elapsed = now - last_time;
	if (elapsed>0 && elapsed<0x80000) {
		if (elapsed<ff_frame_time) {
			int delay = (ff_frame_time - elapsed) / 1000;
			if (delay>0 && delay<17) { // don't allow a delay any greater than a frame
				SDL_Delay(delay);
			}
		}
		last_time += ff_frame_time;
		return;
	}
	last_time = now;
}

static void* coreThread(void *arg) {
	// force a vsync immediately before loop
	// for better frame pacing?
	GFX_clearAll();
	GFX_flip(screen);

	while (!quit) {
		int run = 0;
		pthread_mutex_lock(&core_mx);
		run = should_run_core;
		pthread_mutex_unlock(&core_mx);

		if (run) {
			core.run();
			limitFF();
			trackFPS();
		}
	}
	pthread_exit(NULL);
}

int main(int argc , char* argv[]) {
	LOG_info("MinArch\n");

	setOverclock(overclock); // default to normal
	// force a stack overflow to ensure asan is linked and actually working
	// char tmp[2];
	// tmp[2] = 'a';

	char core_path[MAX_PATH];
	char rom_path[MAX_PATH];
	char tag_name[MAX_PATH];

	strcpy(core_path, argv[1]);
	strcpy(rom_path, argv[2]);
	getEmuName(rom_path, tag_name);

	LOG_info("rom_path: %s\n", rom_path);

	screen = GFX_init(MODE_MENU);
	PAD_init();
	DEVICE_WIDTH = screen->w;
	DEVICE_HEIGHT = screen->h;
	DEVICE_PITCH = screen->pitch;
	// LOG_info("DEVICE_SIZE: %ix%i (%i)\n", DEVICE_WIDTH,DEVICE_HEIGHT,DEVICE_PITCH);

	VIB_init();
	PWR_init();
	if (!HAS_POWER_BUTTON) PWR_disableSleep();
	MSG_init();

	// Overrides_init();

	Core_open(core_path, tag_name);
	Game_open(rom_path); // nes tries to load gamegenie setting before this returns ffs
	if (!game.is_open) goto finish;

	simple_mode = exists(SIMPLE_MODE_PATH);

	// restore options
	Config_load(); // before init?
	Config_init();
	Config_readOptions(); // cores with boot logo option (eg. gb) need to load options early
	setOverclock(overclock);
	// hide "Prevent Tearing" on platforms that can't honor it (set after config
	// load so a stale saved value can't un-hide it) rather than showing a dead toggle
	if (!PLAT_supportsVsyncToggle()) config.frontend.options[FE_OPT_TEARING].lock = 1;
	GFX_setVsync(prevent_tearing);

	Core_init();

	// TODO: find a better place to do this
	// mixing static and loaded data is messy
	// why not move to Core_init()?
	// ah, because it's defined before options_menu...
	options_menu.items[1].desc = (char*)core.version;

	Core_load();
	Input_init(NULL);
	Config_readOptions(); // but others load and report options later (eg. nes)
	Config_readControls(); // restore controls (after the core has reported its defaults)
	Config_free();

	SND_init(core.sample_rate, core.fps);
	InitSettings(); // after we initialize audio
	Menu_init();
	State_resume();
	Menu_initState(); // make ready for state shortcuts

	if (thread_video) {
		core_mx = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
		core_rq = (pthread_cond_t)PTHREAD_COND_INITIALIZER;
		pthread_create(&core_pt, NULL, &coreThread, NULL);
	}

	PWR_warn(1);
	PWR_disableAutosleep();

	// force a vsync immediately before loop
	// for better frame pacing?
	GFX_clearAll();
	GFX_flip(screen);

	Special_init(); // after config

	sec_start = SDL_GetTicks();
	while (!quit) {
		GFX_startFrame();

		if (!thread_video) {
			core.run();
			limitFF();
			trackFPS();
		}

		if (thread_video && !quit) {
			pthread_mutex_lock(&core_mx);
			while (!frame_ready && !quit) pthread_cond_wait(&core_rq,&core_mx);
			frame_ready = 0;

			if (backbuffer) {
				video_refresh_callback_main(backbuffer->pixels,backbuffer->w,backbuffer->h,backbuffer->pitch);
				GFX_flip(screen);
			}
			pthread_mutex_unlock(&core_mx);
		}

		if (show_menu) Menu_loop();

		if (toggle_thread) {
			toggle_thread = 0;
			if (was_threaded && !thread_video) {
				// LOG_info("was fast forwarding while previously threaded (%i) so re-enabling threading %i\n", thread_video, !thread_video);
				// revert to pre-fast_forward state before toggling
				was_threaded = 0;
				thread_video = !thread_video;
			}
			// LOG_info("toggling thread from %i to %i\n", thread_video, !thread_video);
			thread_video = !thread_video;
			if (thread_video) {
				// enable
				core_mx = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
				core_rq = (pthread_cond_t)PTHREAD_COND_INITIALIZER;
				pthread_create(&core_pt, NULL, &coreThread, NULL);
			}
			else {
				// disable
				pthread_cancel(core_pt);
				pthread_join(core_pt,NULL);

				// force a vsync immediately before loop
				// for better frame pacing?
				GFX_clearAll();
				GFX_flip(screen);
			}
		}
		// LOG_info("frame duration: %ims\n", SDL_GetTicks()-frame_start);

		hdmimon();
	}

	Menu_quit();
	QuitSettings();

finish:

	Game_close();
	Core_unload();

	Core_quit();
	Core_close();

	Config_quit();

	Special_quit();

	MSG_quit();
	PWR_quit();
	VIB_quit();
	SND_quit();
	PAD_quit();
	GFX_quit();

	return EXIT_SUCCESS;
}
