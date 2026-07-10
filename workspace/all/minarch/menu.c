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
#include <dirent.h>
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

#define MENU_ITEM_COUNT 6
#define MENU_SLOT_COUNT 8

enum {
	ITEM_CONT,
	ITEM_SAVE,
	ITEM_LOAD,
	ITEM_OPTS,
	ITEM_COLL,
	ITEM_QUIT,
};

enum {
	STATUS_CONT =  0,
	STATUS_SAVE =  1,
	STATUS_LOAD = 11,
	STATUS_OPTS = 23,
	STATUS_DISC = 24,
	STATUS_QUIT = 30,
	STATUS_RESET= 31,
};

// TODO: I don't love how overloaded this has become
static struct {
	SDL_Surface* bitmap;
	SDL_Surface* overlay;
	char* items[MENU_ITEM_COUNT];
	char* disc_paths[9]; // up to 9 paths, Arc the Lad Collection is 7 discs
	char minui_dir[MAX_PATH];
	char slot_path[MAX_PATH];
	char base_path[MAX_PATH];
	char bmp_path[MAX_PATH];
	char txt_path[MAX_PATH];
	int disc;
	int total_discs;
	int slot;
	int save_exists;
	int preview_exists;
} menu = {
	.bitmap = NULL,
	.disc = -1,
	.total_discs = 0,
	.save_exists = 0,
	.preview_exists = 0,

	.items = {
		[ITEM_CONT] = "Continue",
		[ITEM_SAVE] = "Save",
		[ITEM_LOAD] = "Load",
		[ITEM_OPTS] = "Options",
		[ITEM_COLL] = "Add to Collection",
		[ITEM_QUIT] = "Quit",
	}
};

void Menu_init(void) {
	menu.overlay = SDL_CreateRGBSurface(SDL_SWSURFACE,DEVICE_WIDTH,DEVICE_HEIGHT,FIXED_DEPTH,RGBA_MASK_AUTO);
	SDLX_SetAlpha(menu.overlay, SDL_SRCALPHA, 0x80);
	SDL_FillRect(menu.overlay, NULL, 0);

	char emu_name[MAX_PATH];
	getEmuName(game.path, emu_name);
	sprintf(menu.minui_dir, SHARED_USERDATA_PATH "/.minui/%s", emu_name);
	mkdir(menu.minui_dir, 0755);

	sprintf(menu.slot_path, "%s/%s.txt", menu.minui_dir, game.name);

	if (simple_mode) menu.items[ITEM_OPTS] = "Reset";

	if (game.m3u_path[0]) {
		char* tmp;
		strcpy(menu.base_path, game.m3u_path);
		tmp = strrchr(menu.base_path, '/') + 1;
		tmp[0] = '\0';

		//read m3u file
		FILE* file = fopen(game.m3u_path, "r");
		if (file) {
			char line[256];
			while (fgets(line,256,file)!=NULL) {
				normalizeNewline(line);
				trimTrailingNewlines(line);
				if (strlen(line)==0) continue; // skip empty lines

				char disc_path[MAX_PATH];
				strcpy(disc_path, menu.base_path);
				tmp = disc_path + strlen(disc_path);
				strcpy(tmp, line);

				// found a valid disc path
				if (exists(disc_path)) {
					if (menu.total_discs >= 9) break;
					menu.disc_paths[menu.total_discs] = strdup(disc_path);
					// matched our current disc
					if (exactMatch(disc_path, game.path)) {
						menu.disc = menu.total_discs;
					}
					menu.total_discs += 1;
				}
			}
			fclose(file);
		}
	}
}
void Menu_quit(void) {
	SDL_FreeSurface(menu.overlay);
}
void Menu_beforeSleep(void) {
	// LOG_info("beforeSleep\n");
	SRAM_write();
	RTC_write();
	State_autosave();
	putFile(AUTO_RESUME_PATH, game.path + strlen(SDCARD_PATH));
	PWR_setCPUSpeed(CPU_SPEED_MENU);
}
void Menu_afterSleep(void) {
	// LOG_info("beforeSleep\n");
	unlink(AUTO_RESUME_PATH);
	setOverclock(overclock);
}

enum {
	MENU_CALLBACK_NOP,
	MENU_CALLBACK_EXIT,
	MENU_CALLBACK_NEXT_ITEM,
};

enum {
	MENU_LIST, // eg. save and main menu
	MENU_VAR, // eg. frontend
	MENU_FIXED, // eg. emulator
	MENU_INPUT, // eg. renders like but MENU_VAR but handles input differently
};

static int Menu_message(char* message, char** pairs) {
	GFX_setMode(MODE_MAIN);
	int dirty = 1;
	while (1) {
		GFX_startFrame();
		PAD_poll();

		if (PAD_justPressed(BTN_A) || PAD_justPressed(BTN_B)) break;

		PWR_update(&dirty, NULL, Menu_beforeSleep, Menu_afterSleep);

		if (dirty) {
			GFX_clear(screen);
			GFX_blitMessage(font.medium, message, screen, &(SDL_Rect){0,SCALE1(PADDING),screen->w,screen->h-SCALE1(PILL_SIZE+PADDING)});
			GFX_blitButtonGroup(pairs, 0, screen, 1);
			GFX_flip(screen);
			dirty = 0;
		}
		else GFX_sync();

		hdmimon();
	}
	GFX_setMode(MODE_MENU);
	return MENU_CALLBACK_NOP; // TODO: this should probably be an arg
}

static int Menu_options(MenuList* list);

static int OptionFrontend_optionChanged(MenuList* list, int i) {
	MenuItem* item = &list->items[i];
	Config_syncFrontend(item->key, item->value);
	return MENU_CALLBACK_NOP;
}
static MenuList OptionFrontend_menu = {
	.type = MENU_VAR,
	.on_change = OptionFrontend_optionChanged,
	.items = NULL,
};
static void OptionList_buildMenu(OptionList* list, MenuList* menu) {
	if (menu->items==NULL) {
		// TODO: where do I free this? I guess I don't :sweat_smile:
		if (!list->enabled_count) {
			int enabled_count = 0;
			for (int i=0; i<list->count; i++) {
				if (!list->options[i].lock) enabled_count += 1;
			}
			list->enabled_count = enabled_count;
			list->enabled_options = calloc(enabled_count+1, sizeof(Option*));
			int j = 0;
			for (int i=0; i<list->count; i++) {
				Option* item = &list->options[i];
				if (item->lock) continue;
				list->enabled_options[j] = item;
				j += 1;
			}
		}

		menu->items = calloc(list->enabled_count+1, sizeof(MenuItem));
		for (int j=0; j<list->enabled_count; j++) {
			Option* option = list->enabled_options[j];
			MenuItem* item = &menu->items[j];
			item->key = option->key;
			item->name = option->name;
			item->desc = option->desc;
			item->value = option->value;
			item->values = option->labels;
		}
	}
	else {
		// update values
		for (int j=0; j<list->enabled_count; j++) {
			Option* option = list->enabled_options[j];
			MenuItem* item = &menu->items[j];
			item->value = option->value;
		}
	}
}
static int OptionFrontend_openMenu(MenuList* list, int i) {
	OptionList_buildMenu(&config.frontend, &OptionFrontend_menu);
	Menu_options(&OptionFrontend_menu);
	return MENU_CALLBACK_NOP;
}

static int OptionEmulator_optionChanged(MenuList* list, int i) {
	MenuItem* item = &list->items[i];
	Option* option = OptionList_getOption(&config.core, item->key);
	LOG_info("%s (%s) changed from `%s` (%s) to `%s` (%s)\n", item->name, item->key,
		item->values[option->value], option->values[option->value],
		item->values[item->value], option->values[item->value]
	);
	OptionList_setOptionRawValue(&config.core, item->key, item->value);
	return MENU_CALLBACK_NOP;
}
static int OptionEmulator_optionDetail(MenuList* list, int i) {
	MenuItem* item = &list->items[i];
	Option* option = OptionList_getOption(&config.core, item->key);
	if (option->full) return Menu_message(option->full, (char*[]){ "B","BACK", NULL });
	else return MENU_CALLBACK_NOP;
}
static MenuList OptionEmulator_menu = {
	.type = MENU_FIXED,
	.on_confirm = OptionEmulator_optionDetail, // TODO: this needs pagination to be truly useful
	.on_change = OptionEmulator_optionChanged,
	.items = NULL,
};
static int OptionEmulator_openMenu(MenuList* list, int i) {
	OptionList_buildMenu(&config.core, &OptionEmulator_menu);

	if (OptionEmulator_menu.items[0].name) { // TODO: why doesn't this just use (enabled_)count?
		Menu_options(&OptionEmulator_menu);
	}
	else {
		Menu_message("This core has no options.", (char*[]){ "B","BACK", NULL });
	}

	return MENU_CALLBACK_NOP;
}

static int Option_bindButton(ButtonMapping* button, MenuItem* item) {
	int bound = 0;
	while (!bound) {
		GFX_startFrame();
		PAD_poll();

		// NOTE: off by one because of the initial NONE value
		for (int id=0; id<=LOCAL_BUTTON_COUNT; id++) {
			if (PAD_justPressed(1 << (id-1))) {
				item->value = id;
				button->local = id - 1;
				if (PAD_isPressed(BTN_MENU)) {
					item->value += LOCAL_BUTTON_COUNT;
					button->mod = 1;
				}
				else {
					button->mod = 0;
				}
				bound = 1;
				break;
			}
		}
		GFX_sync();
		hdmimon();
	}
	return MENU_CALLBACK_NEXT_ITEM;
}
static void Option_unbindButton(ButtonMapping* button) {
	button->local = -1;
	button->mod = 0;
}
static int OptionControls_bind(MenuList* list, int i) {
	MenuItem* item = &list->items[i];
	if (item->values!=button_labels) {
		// LOG_info("changed gamepad_type\n");
		return MENU_CALLBACK_NOP;
	}
	return Option_bindButton(&config.controls[item->id], item);
}
static int OptionControls_unbind(MenuList* list, int i) {
	MenuItem* item = &list->items[i];
	if (item->values!=button_labels) return MENU_CALLBACK_NOP;

	Option_unbindButton(&config.controls[item->id]);
	return MENU_CALLBACK_NOP;
}
static int OptionControls_optionChanged(MenuList* list, int i) {
	MenuItem* item = &list->items[i];
	if (item->values!=gamepad_labels) return MENU_CALLBACK_NOP;

	if (has_custom_controllers) {
		gamepad_type = item->value;
		int device = strtol(gamepad_values[item->value], NULL, 0);
		core.set_controller_port_device(0, device);
	}
	return MENU_CALLBACK_NOP;
}
static MenuList OptionControls_menu = {
	.type = MENU_INPUT,
	.desc = "Press A to set and X to clear."
		"\nSupports single button and MENU+button." // TODO: not supported on nano because POWER doubles as MENU
	,
	.on_confirm = OptionControls_bind,
	.on_change = OptionControls_unbind,
	.items = NULL
};
static int OptionControls_openMenu(MenuList* list, int i) {
	LOG_info("OptionControls_openMenu\n");

	if (OptionControls_menu.items==NULL) {

		// TODO: where do I free this?
		OptionControls_menu.items = calloc(RETRO_BUTTON_COUNT+1+has_custom_controllers, sizeof(MenuItem));
		int k = 0;

		if (has_custom_controllers) {
			MenuItem* item = &OptionControls_menu.items[k++];
			item->name = "Controller";
			item->desc = "Select the type of controller.";
			item->value = gamepad_type;
			item->values = gamepad_labels;
			item->on_change = OptionControls_optionChanged;
		}

		for (int j=0; config.controls[j].name; j++) {
			ButtonMapping* button = &config.controls[j];
			if (button->ignore) continue;

			LOG_info("\t%s (%i:%i)\n", button->name, button->local, button->retro);

			MenuItem* item = &OptionControls_menu.items[k++];
			item->id = j;
			item->name = button->name;
			item->desc = NULL;
			item->value = button->local + 1;
			if (button->mod) item->value += LOCAL_BUTTON_COUNT;
			item->values = button_labels;
		}
	}
	else {
		// update values
		int k = 0;

		if (has_custom_controllers) {
			MenuItem* item = &OptionControls_menu.items[k++];
			item->value = gamepad_type;
		}

		for (int j=0; config.controls[j].name; j++) {
			ButtonMapping* button = &config.controls[j];
			if (button->ignore) continue;

			MenuItem* item = &OptionControls_menu.items[k++];
			item->value = button->local + 1;
			if (button->mod) item->value += LOCAL_BUTTON_COUNT;
		}
	}
	Menu_options(&OptionControls_menu);
	return MENU_CALLBACK_NOP;
}

static int OptionShortcuts_bind(MenuList* list, int i) {
	MenuItem* item = &list->items[i];
	return Option_bindButton(&config.shortcuts[item->id], item);
}
static int OptionShortcuts_unbind(MenuList* list, int i) {
	MenuItem* item = &list->items[i];
	Option_unbindButton(&config.shortcuts[item->id]);
	return MENU_CALLBACK_NOP;
}
static MenuList OptionShortcuts_menu = {
	.type = MENU_INPUT,
	.desc = "Press A to set and X to clear."
		"\nSupports single button and MENU+button." // TODO: not supported on nano because POWER doubles as MENU
	,
	.on_confirm = OptionShortcuts_bind,
	.on_change = OptionShortcuts_unbind,
	.items = NULL
};
static char* getSaveDesc(void) {
	switch (config.loaded) {
		case CONFIG_NONE:		return "Using defaults."; break;
		case CONFIG_CONSOLE:	return "Using console config."; break;
		case CONFIG_GAME:		return "Using game config."; break;
	}
	return NULL;
}
static int OptionShortcuts_openMenu(MenuList* list, int i) {
	if (OptionShortcuts_menu.items==NULL) {
		// TODO: where do I free this? I guess I don't :sweat_smile:
		OptionShortcuts_menu.items = calloc(SHORTCUT_COUNT+1, sizeof(MenuItem));
		for (int j=0; config.shortcuts[j].name; j++) {
			ButtonMapping* button = &config.shortcuts[j];
			MenuItem* item = &OptionShortcuts_menu.items[j];
			item->id = j;
			item->name = button->name;
			item->desc = NULL;
			item->value = button->local + 1;
			if (button->mod) item->value += LOCAL_BUTTON_COUNT;
			item->values = button_labels;
		}
	}
	else {
		// update values
		for (int j=0; config.shortcuts[j].name; j++) {
			ButtonMapping* button = &config.shortcuts[j];
			MenuItem* item = &OptionShortcuts_menu.items[j];
			item->value = button->local + 1;
			if (button->mod) item->value += LOCAL_BUTTON_COUNT;
		}
	}
	Menu_options(&OptionShortcuts_menu);
	return MENU_CALLBACK_NOP;
}

static void OptionSaveChanges_updateDesc(void);
static int OptionSaveChanges_onConfirm(MenuList* list, int i) {
	char* message;
	switch (i) {
		case 0: {
			Config_write(CONFIG_WRITE_ALL);
			message = "Saved for console.";
			break;
		}
		case 1: {
			Config_write(CONFIG_WRITE_GAME);
			message = "Saved for game.";
			break;
		}
		default: {
			Config_restore();
			if (config.loaded) message = "Restored console defaults.";
			else message = "Restored defaults.";
			break;
		}
	}
	Menu_message(message, (char*[]){ "A","OKAY", NULL });
	OptionSaveChanges_updateDesc();
	return MENU_CALLBACK_EXIT;
}
static MenuList OptionSaveChanges_menu = {
	.type = MENU_LIST,
	.on_confirm = OptionSaveChanges_onConfirm,
	.items = (MenuItem[]){
		{"Save for console"},
		{"Save for game"},
		{"Restore defaults"},
		{NULL},
	}
};
static int OptionSaveChanges_openMenu(MenuList* list, int i) {
	OptionSaveChanges_updateDesc();
	OptionSaveChanges_menu.desc = getSaveDesc();
	Menu_options(&OptionSaveChanges_menu);
	return MENU_CALLBACK_NOP;
}

static int OptionQuicksave_onConfirm(MenuList* list, int i) {
	Menu_beforeSleep();
	PWR_powerOff();
}

MenuList options_menu = {
	.type = MENU_LIST,
	.items = (MenuItem[]) {
		{"Frontend", "darkUI (" BUILD_DATE " " BUILD_HASH ")",.on_confirm=OptionFrontend_openMenu},
		{"Emulator",.on_confirm=OptionEmulator_openMenu},
		{"Controls",.on_confirm=OptionControls_openMenu},
		{"Shortcuts",.on_confirm=OptionShortcuts_openMenu},
		{"Save Changes",.on_confirm=OptionSaveChanges_openMenu},
		{NULL},
		{NULL},
		{NULL},
	}
};

static void OptionSaveChanges_updateDesc(void) {
	options_menu.items[4].desc = getSaveDesc();
}

#define OPTION_PADDING 8

static void Menu_selectNext(int* selected, int* start, int* end, int count, int visible_rows) {
	*selected += 1;
	if (*selected>=count) {
		*selected = 0;
		*start = 0;
		*end = visible_rows;
	}
	else if (*selected>=*end) {
		*start += 1;
		*end += 1;
	}
}
static void Menu_selectPrev(int* selected, int* start, int* end, int count, int max_visible_options) {
	*selected -= 1;
	if (*selected<0) {
		*selected = count - 1;
		*start = MAX(0,count - max_visible_options);
		*end = count;
	}
	else if (*selected<*start) {
		*start -= 1;
		*end -= 1;
	}
}
static int Menu_options(MenuList* list) {
	MenuItem* items = list->items;
	int type = list->type;

	int dirty = 1;
	int show_options = 1;
	int show_settings = 0;
	int await_input = 0;

	// dependent on option list offset top and bottom, eg. the gray triangles
	int max_visible_options = (screen->h - ((SCALE1(PADDING + PILL_SIZE) * 2) + SCALE1(BUTTON_SIZE))) / SCALE1(BUTTON_SIZE); // 7 for 480, 10 for 720

	int count;
	for (count=0; items[count].name; count++);
	int selected = 0;
	int start = 0;
	int end = MIN(count,max_visible_options);
	int visible_rows = end;

	OptionSaveChanges_updateDesc();

	int defer_menu = false;
	while (show_options) {
		if (await_input) {
			defer_menu = true;
			list->on_confirm(list, selected);

			selected += 1;
			if (selected>=count) {
				selected = 0;
				start = 0;
				end = visible_rows;
			}
			else if (selected>=end) {
				start += 1;
				end += 1;
			}
			dirty = 1;
			await_input = false;
		}

		GFX_startFrame();
		PAD_poll();

		if (PAD_justRepeated(BTN_UP)) {
			Menu_selectPrev(&selected, &start, &end, count, max_visible_options);
			dirty = 1;
		}
		else if (PAD_justRepeated(BTN_DOWN)) {
			Menu_selectNext(&selected, &start, &end, count, visible_rows);
			dirty = 1;
		}
		else {
			MenuItem* item = &items[selected];
			if (item->values && item->values!=button_labels) { // not an input binding
				if (PAD_justRepeated(BTN_LEFT)) {
					if (item->value>0) item->value -= 1;
					else {
						int j;
						for (j=0; item->values[j]; j++);
						item->value = j - 1;
					}

					if (item->on_change) item->on_change(list, selected);
					else if (list->on_change) list->on_change(list, selected);

					dirty = 1;
				}
				else if (PAD_justRepeated(BTN_RIGHT)) {
					if (item->values[item->value+1]) item->value += 1;
					else item->value = 0;

					if (item->on_change) item->on_change(list, selected);
					else if (list->on_change) list->on_change(list, selected);

					dirty = 1;
				}
			}
		}

		// uint32_t now = SDL_GetTicks();
		if (PAD_justPressed(BTN_B)) { // || PAD_tappedMenu(now)
			show_options = 0;
		}
		else if (PAD_justPressed(BTN_A)) {
			MenuItem* item = &items[selected];
			int result = MENU_CALLBACK_NOP;
			if (item->on_confirm) result = item->on_confirm(list, selected); // item-specific action, eg. Save for all games
			else if (item->submenu) result = Menu_options(item->submenu); // drill down, eg. main options menu
			// TODO: is there a way to defer on_confirm for MENU_INPUT so we can clear the currently set value to indicate it is awaiting input?
			// eg. set a flag to call on_confirm at the beginning of the next frame?
			else if (list->on_confirm) {
				if (item->values==button_labels) await_input = 1; // button binding
				else result = list->on_confirm(list, selected); // list-specific action, eg. show item detail view or input binding
			}
			if (result==MENU_CALLBACK_EXIT) show_options = 0;
			else {
				if (result==MENU_CALLBACK_NEXT_ITEM) {
					Menu_selectNext(&selected, &start, &end, count, visible_rows);
				}
				dirty = 1;
			}
		}
		else if (type==MENU_INPUT) {
			if (PAD_justPressed(BTN_X)) {
				MenuItem* item = &items[selected];
				item->value = 0;

				if (item->on_change) item->on_change(list, selected);
				else if (list->on_change) list->on_change(list, selected);

				Menu_selectNext(&selected, &start, &end, count, visible_rows);
				dirty = 1;
			}
		}

		if (!defer_menu) PWR_update(&dirty, &show_settings, Menu_beforeSleep, Menu_afterSleep);

		if (defer_menu && PAD_justReleased(BTN_MENU)) defer_menu = false;

		if (dirty) {
			GFX_clear(screen);
			GFX_blitHardwareGroup(screen, show_settings);

			char* desc = NULL;
			SDL_Surface* text;

			if (type==MENU_LIST) {
				int mw = list->max_width;
				if (!mw) {
					// get the width of the widest item
					for (int i=0; i<count; i++) {
						MenuItem* item = &items[i];
						int w = 0;
						TTF_SizeUTF8(font.small, item->name, &w, NULL);
						w += SCALE1(OPTION_PADDING*2);
						if (w>mw) mw = w;
					}
					// cache the result
					list->max_width = mw = MIN(mw, screen->w - SCALE1(PADDING *2));
				}

				int ox = (screen->w - mw) / 2;
				int oy = SCALE1(PADDING + PILL_SIZE);
				int selected_row = selected - start;
				for (int i=start,j=0; i<end; i++,j++) {
					MenuItem* item = &items[i];
					SDL_Color text_color = COLOR_WHITE;

					// int ox = (screen->w - w) / 2; // if we're centering these (but I don't think we should after seeing it)
					if (j==selected_row) {
						// move out of conditional if centering
						int w = 0;
						TTF_SizeUTF8(font.small, item->name, &w, NULL);
						w += SCALE1(OPTION_PADDING*2);

						GFX_blitPill(ASSET_BUTTON, screen, &(SDL_Rect){
							ox,
							oy+SCALE1(j*BUTTON_SIZE),
							w,
							SCALE1(BUTTON_SIZE)
						});
						text_color = COLOR_BLACK;

						if (item->desc) desc = item->desc;
					}
					text = TTF_RenderUTF8_Blended(font.small, item->name, text_color);
					SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){
						ox+SCALE1(OPTION_PADDING),
						oy+SCALE1((j*BUTTON_SIZE)+1)
					});
					SDL_FreeSurface(text);
				}
			}
			else if (type==MENU_FIXED) {
				// NOTE: no need to calculate max width
				int mw = screen->w - SCALE1(PADDING*2);
				// int lw,rw;
				// lw = rw = mw / 2;
				int ox,oy;
				ox = oy = SCALE1(PADDING);
				oy += SCALE1(PILL_SIZE);

				int selected_row = selected - start;
				for (int i=start,j=0; i<end; i++,j++) {
					MenuItem* item = &items[i];
					SDL_Color text_color = COLOR_WHITE;

					if (j==selected_row) {
						// gray pill
						GFX_blitPill(ASSET_OPTION, screen, &(SDL_Rect){
							ox,
							oy+SCALE1(j*BUTTON_SIZE),
							mw,
							SCALE1(BUTTON_SIZE)
						});
					}

					if (item->value>=0) {
						text = TTF_RenderUTF8_Blended(font.tiny, item->values[item->value], COLOR_WHITE); // always white
						SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){
							ox + mw - text->w - SCALE1(OPTION_PADDING),
							oy+SCALE1((j*BUTTON_SIZE)+3)
						});
						SDL_FreeSurface(text);
					}

					// TODO: blit a black pill on unselected rows (to cover longer item->values?) or truncate longer item->values?
					if (j==selected_row) {
						// white pill
						int w = 0;
						TTF_SizeUTF8(font.small, item->name, &w, NULL);
						w += SCALE1(OPTION_PADDING*2);
						GFX_blitPill(ASSET_BUTTON, screen, &(SDL_Rect){
							ox,
							oy+SCALE1(j*BUTTON_SIZE),
							w,
							SCALE1(BUTTON_SIZE)
						});
						text_color = COLOR_BLACK;

						if (item->desc) desc = item->desc;
					}
					text = TTF_RenderUTF8_Blended(font.small, item->name, text_color);
					SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){
						ox+SCALE1(OPTION_PADDING),
						oy+SCALE1((j*BUTTON_SIZE)+1)
					});
					SDL_FreeSurface(text);
				}
			}
			else if (type==MENU_VAR || type==MENU_INPUT) {
				int mw = list->max_width;
				if (!mw) {
					// get the width of the widest row
					int mrw = 0;
					for (int i=0; i<count; i++) {
						MenuItem* item = &items[i];
						int w = 0;
						int lw = 0;
						int rw = 0;
						TTF_SizeUTF8(font.small, item->name, &lw, NULL);

						// every value list in an input table is the same
						// so only calculate rw for the first item...
						if (!mrw || type!=MENU_INPUT) {
							for (int j=0; item->values[j]; j++) {
								TTF_SizeUTF8(font.tiny, item->values[j], &rw, NULL);
								if (lw+rw>w) w = lw+rw;
								if (rw>mrw) mrw = rw;
							}
						}
						else {
							w = lw + mrw;
						}
						w += SCALE1(OPTION_PADDING*4);
						if (w>mw) mw = w;
					}
					fflush(stdout);
					// cache the result
					list->max_width = mw = MIN(mw, screen->w - SCALE1(PADDING *2));
				}

				int ox = (screen->w - mw) / 2;
				int oy = SCALE1(PADDING + PILL_SIZE);
				int selected_row = selected - start;
				for (int i=start,j=0; i<end; i++,j++) {
					MenuItem* item = &items[i];
					SDL_Color text_color = COLOR_WHITE;

					if (j==selected_row) {
						// gray pill
						GFX_blitPill(ASSET_OPTION, screen, &(SDL_Rect){
							ox,
							oy+SCALE1(j*BUTTON_SIZE),
							mw,
							SCALE1(BUTTON_SIZE)
						});

						// white pill
						int w = 0;
						TTF_SizeUTF8(font.small, item->name, &w, NULL);
						w += SCALE1(OPTION_PADDING*2);
						GFX_blitPill(ASSET_BUTTON, screen, &(SDL_Rect){
							ox,
							oy+SCALE1(j*BUTTON_SIZE),
							w,
							SCALE1(BUTTON_SIZE)
						});
						text_color = COLOR_BLACK;

						if (item->desc) desc = item->desc;
					}
					text = TTF_RenderUTF8_Blended(font.small, item->name, text_color);
					SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){
						ox+SCALE1(OPTION_PADDING),
						oy+SCALE1((j*BUTTON_SIZE)+1)
					});
					SDL_FreeSurface(text);

					if (await_input && j==selected_row) {
						// buh
					}
					else if (item->value>=0) {
						text = TTF_RenderUTF8_Blended(font.tiny, item->values[item->value], COLOR_WHITE); // always white
						SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){
							ox + mw - text->w - SCALE1(OPTION_PADDING),
							oy+SCALE1((j*BUTTON_SIZE)+3)
						});
						SDL_FreeSurface(text);
					}
				}
			}

			if (count>max_visible_options) {
				#define SCROLL_WIDTH 24
				#define SCROLL_HEIGHT 4
				int ox = (screen->w - SCALE1(SCROLL_WIDTH))/2;
				int oy = SCALE1((PILL_SIZE - SCROLL_HEIGHT) / 2);
				if (start>0) GFX_blitAsset(ASSET_SCROLL_UP,   NULL, screen, &(SDL_Rect){ox, SCALE1(PADDING) + oy});
				if (end<count) GFX_blitAsset(ASSET_SCROLL_DOWN, NULL, screen, &(SDL_Rect){ox, screen->h - SCALE1(PADDING + PILL_SIZE + BUTTON_SIZE) + oy});
			}

			if (!desc && list->desc) desc = list->desc;

			if (desc) {
				int w,h;
				GFX_sizeText(font.tiny, desc, SCALE1(12), &w,&h);
				GFX_blitText(font.tiny, desc, SCALE1(12), COLOR_WHITE, screen, &(SDL_Rect){
					(screen->w - w) / 2,
					screen->h - SCALE1(PADDING) - h,
					w,h
				});
			}

			GFX_flip(screen);
			dirty = 0;
		}
		else GFX_sync();
		hdmimon();
	}

	// GFX_clearAll();
	// GFX_flip(screen);

	return 0;
}

static void Menu_scale(SDL_Surface* src, SDL_Surface* dst) {
	// LOG_info("Menu_scale src: %ix%i dst: %ix%i\n", src->w,src->h,dst->w,dst->h);

	uint16_t* s = src->pixels;
	uint16_t* d = dst->pixels;

	int sw = src->w;
	int sh = src->h;
	int sp = src->pitch / FIXED_BPP;

	int dw = dst->w;
	int dh = dst->h;
	int dp = dst->pitch / FIXED_BPP;

	int rx = 0;
	int ry = 0;
	int rw = dw;
	int rh = dh;

	int scaling = screen_scaling;
	if (scaling==SCALE_CROPPED && DEVICE_WIDTH==HDMI_WIDTH) {
		scaling = SCALE_NATIVE;
	}
	if (scaling==SCALE_NATIVE) {
		// LOG_info("native\n");

		rx = renderer.dst_x;
		ry = renderer.dst_y;
		rw = renderer.src_w;
		rh = renderer.src_h;
		if (renderer.scale) {
			// LOG_info("scale: %i\n", renderer.scale);
			rw *= renderer.scale;
			rh *= renderer.scale;
		}
		else {
			// LOG_info("forced crop\n"); // eg. fc on nano, vb on smart
			rw -= renderer.src_x * 2;
			rh -= renderer.src_y * 2;
			sw = rw;
			sh = rh;
		}

		if (dw==DEVICE_WIDTH/2) {
			// LOG_info("halve\n");
			rx /= 2;
			ry /= 2;
			rw /= 2;
			rh /= 2;
		}
	}
	else if (scaling==SCALE_CROPPED) {
		// LOG_info("cropped\n");
		sw -= renderer.src_x * 2;
		sh -= renderer.src_y * 2;

		rx = renderer.dst_x;
		ry = renderer.dst_y;
		rw = sw * renderer.scale;
		rh = sh * renderer.scale;

		if (dw==DEVICE_WIDTH/2) {
			// LOG_info("halve\n");
			rx /= 2;
			ry /= 2;
			rw /= 2;
			rh /= 2;
		}
	}

	if (scaling==SCALE_ASPECT || rw>dw || rh>dh) {
		// LOG_info("aspect\n");
		double fixed_aspect_ratio = ((double)DEVICE_WIDTH) / DEVICE_HEIGHT;
		int core_aspect = core.aspect_ratio * 1000;
		int fixed_aspect = fixed_aspect_ratio * 1000;

		if (core_aspect>fixed_aspect) {
			// LOG_info("letterbox\n");
			rw = dw;
			rh = rw / core.aspect_ratio;
			rh += rh%2;
		}
		else if (core_aspect<fixed_aspect) {
			// LOG_info("pillarbox\n");
			rh = dh;
			rw = rh * core.aspect_ratio;
			rw += rw%2;
			rw = (rw/8)*8; // probably necessary here since we're not scaling by an integer
		}
		else {
			// LOG_info("perfect match\n");
			rw = dw;
			rh = dh;
		}

		rx = (dw - rw) / 2;
		ry = (dh - rh) / 2;
	}

	// LOG_info("Menu_scale (r): %i,%i %ix%i\n",rx,ry,rw,rh);
	// LOG_info("offset: %i,%i\n", renderer.src_x, renderer.src_y);

	// dumb nearest neighbor scaling
	int mx = (sw << 16) / rw;
	int my = (sh << 16) / rh;
	int ox = (renderer.src_x << 16);
	int sx = ox;
	int sy = (renderer.src_y << 16);
	int lr = -1;
	int sr = 0;
	int dr = ry * dp;
	int cp = dp * FIXED_BPP;

	// LOG_info("Menu_scale (s): %i,%i %ix%i\n",sx,sy,sw,sh);
	// LOG_info("mx:%i my:%i sx>>16:%i sy>>16:%i\n",mx,my,((sx+mx) >> 16),((sy+my) >> 16));

	for (int dy=0; dy<rh; dy++) {
		sx = ox;
		sr = (sy >> 16) * sp;
		if (sr==lr) {
			memcpy(d+dr,d+dr-dp,cp);
		}
		else {
	        for (int dx=0; dx<rw; dx++) {
	            d[dr + rx + dx] = s[sr + (sx >> 16)];
				sx += mx;
	        }
		}
		lr = sr;
		sy += my;
		dr += dp;
    }

	// LOG_info("successful\n");
}

void Menu_initState(void) {
	if (exists(menu.slot_path)) menu.slot = getInt(menu.slot_path);
	if (menu.slot==8) menu.slot = 0;

	menu.save_exists = 0;
	menu.preview_exists = 0;
}
static void Menu_updateState(void) {
	// LOG_info("Menu_updateState\n");

	int last_slot = state_slot;
	state_slot = menu.slot;

	char save_path[MAX_PATH];
	State_getPath(save_path);

	state_slot = last_slot;

	sprintf(menu.bmp_path, "%s/%s.%d.bmp", menu.minui_dir, game.name, menu.slot);
	sprintf(menu.txt_path, "%s/%s.%d.txt", menu.minui_dir, game.name, menu.slot);

	menu.save_exists = exists(save_path);
	menu.preview_exists = menu.save_exists && exists(menu.bmp_path);

	// LOG_info("save_path: %s (%i)\n", save_path, menu.save_exists);
	// LOG_info("bmp_path: %s txt_path: %s (%i)\n", menu.bmp_path, menu.txt_path, menu.preview_exists);
}
void Menu_saveState(void) {
	// LOG_info("Menu_saveState\n");

	Menu_updateState();

	state_slot = menu.slot;
	if (!State_write()) {
		Menu_message("Save failed", (char*[]){ "A","OKAY", NULL });
		return;
	}

	if (menu.total_discs) {
		char* disc_path = menu.disc_paths[menu.disc];
		putFile(menu.txt_path, disc_path + strlen(menu.base_path));
	}

	SDL_Surface* bitmap = menu.bitmap;
	if (!bitmap) bitmap = SDL_CreateRGBSurfaceFrom(renderer.src, renderer.true_w, renderer.true_h, FIXED_DEPTH, renderer.src_p, RGBA_MASK_565);
	SDL_RWops* out = SDL_RWFromFile(menu.bmp_path, "wb");
	SDL_SaveBMP_RW(bitmap, out, 1);

	// LOG_info("%s %ix%i\n", menu.bmp_path, bitmap->w,bitmap->h);

	if (bitmap!=menu.bitmap) SDL_FreeSurface(bitmap);

	putInt(menu.slot_path, menu.slot);
}
void Menu_loadState(void) {
	// LOG_info("Menu_loadState\n");

	Menu_updateState();

	if (menu.save_exists) {
		if (menu.total_discs) {
			char slot_disc_name[MAX_PATH];
			getFile(menu.txt_path, slot_disc_name, MAX_PATH);

			char slot_disc_path[MAX_PATH];
			if (slot_disc_name[0]=='/') strcpy(slot_disc_path, slot_disc_name);
			else sprintf(slot_disc_path, "%s%s", menu.base_path, slot_disc_name);

			char* disc_path = menu.disc_paths[menu.disc];
			if (!exactMatch(slot_disc_path, disc_path)) {
				Game_changeDisc(slot_disc_path);
			}
		}

		state_slot = menu.slot;
		putInt(menu.slot_path, menu.slot);
		State_read();
	}
}

static char* getAlias(char* path, char* alias) {
	// LOG_info("alias path: %s\n", path);
	char* tmp;
	char map_path[MAX_PATH];
	strcpy(map_path, path);
	tmp = strrchr(map_path, '/');
	if (tmp) {
		tmp += 1;
		strcpy(tmp, "map.txt");
		// LOG_info("map_path: %s\n", map_path);
	}
	char* file_name = strrchr(path,'/');
	if (file_name) file_name += 1;
	// LOG_info("file_name: %s\n", file_name);

	if (exists(map_path)) {
		FILE* file = fopen(map_path, "r");
		if (file) {
			char line[256];
			while (fgets(line,256,file)!=NULL) {
				normalizeNewline(line);
				trimTrailingNewlines(line);
				if (strlen(line)==0) continue; // skip empty lines

				tmp = strchr(line,'\t');
				if (tmp) {
					tmp[0] = '\0';
					char* key = line;
					char* value = tmp+1;
					if (exactMatch(file_name,key)) {
						strcpy(alias, value);
						break;
					}
				}
			}
			fclose(file);
		}
	}
}

// ---- Collections picker -----------------------------------------------------
// Add/remove the current game to/from a launcher collection (/Collections/<name>.txt).
#define FAV_CHECK "\xe2\x9c\x93" // ✓

static int Menu_inCollection(const char* name, const char* rel) {
	char path[MAX_PATH];
	snprintf(path, sizeof(path), COLLECTIONS_PATH "/%s.txt", name);
	FILE* f = fopen(path, "r");
	if (!f) return 0;
	char line[256];
	int found = 0;
	while (fgets(line, sizeof(line), f)!=NULL) {
		normalizeNewline(line); trimTrailingNewlines(line);
		if (exactMatch(line, rel)) { found = 1; break; }
	}
	fclose(f);
	return found;
}

// Row label with a leading marker when the game is in the collection, so membership
// is visible per-row (MENU_LIST only shows desc as a footer). item->key holds the
// real collection name; item->name is this display string.
static char* Menu_collLabel(const char* name, int in) {
	char* s = malloc(strlen(name) + 8);
	if (!s) return strdup(name);
	sprintf(s, "%s%s", in ? FAV_CHECK " " : "", name);
	return s;
}

static int Menu_toggleCollection(MenuList* list, int i) {
	MenuItem* item = &list->items[i];
	char* rel = game.path + strlen(SDCARD_PATH);
	char* cname = item->key ? item->key : item->name;
	char path[MAX_PATH];
	snprintf(path, sizeof(path), COLLECTIONS_PATH "/%s.txt", cname);
	if (Menu_inCollection(cname, rel)) {
		// remove entry (temp + rename = atomic)
		FILE* in = fopen(path, "r");
		if (in) {
			char tmp[MAX_PATH];
			snprintf(tmp, sizeof(tmp), "%s.tmp", path);
			FILE* out = fopen(tmp, "w");
			if (out) {
				char line[256];
				while (fgets(line, sizeof(line), in)!=NULL) {
					normalizeNewline(line); trimTrailingNewlines(line);
					if (strlen(line)==0 || exactMatch(line, rel)) continue;
					fprintf(out, "%s\n", line);
				}
				fclose(out); fclose(in);
				rename(tmp, path);
			} else fclose(in);
		}
	}
	else {
		mkdir(COLLECTIONS_PATH, 0755);
		FILE* out = fopen(path, "a");
		if (out) { fprintf(out, "%s\n", rel); fclose(out); }
		else Menu_message("Couldn't update collection", (char*[]){ "A","OKAY", NULL });
	}
	// refresh the row label to reflect membership
	free(item->name);
	item->name = Menu_collLabel(cname, Menu_inCollection(cname, rel));
	return MENU_CALLBACK_NOP;
}

static int Menu_collectionNameCmp(const void* a, const void* b) {
	return strcmp(*(const char**)a, *(const char**)b);
}

// Minimal on-screen keyboard for naming a new collection. Returns 1 if the user
// confirmed a non-empty name (written to out), 0 if cancelled.
static int Menu_keyboard(char* out, int maxlen) {
	static const char* KB[] = { "ABCDEFGHIJ", "KLMNOPQRST", "UVWXYZ0123", "456789 -_" };
	const int nrows = 4;
	int cr = 0, cc = 0, len = 0, result = 0, done = 0, dirty = 1;
	out[0] = '\0';

	GFX_setMode(MODE_MAIN);
	while (!done) {
		GFX_startFrame();
		PAD_poll();

		if (PAD_justRepeated(BTN_UP))         { cr = (cr - 1 + nrows) % nrows; dirty = 1; }
		else if (PAD_justRepeated(BTN_DOWN))  { cr = (cr + 1) % nrows; dirty = 1; }
		else if (PAD_justRepeated(BTN_LEFT))  { cc -= 1; dirty = 1; }
		else if (PAD_justRepeated(BTN_RIGHT)) { cc += 1; dirty = 1; }
		int rl = strlen(KB[cr]);
		if (cc < 0) cc = rl - 1;
		if (cc >= rl) cc = 0;

		if (PAD_justPressed(BTN_A)) {
			if (len < maxlen - 1) { out[len++] = KB[cr][cc]; out[len] = '\0'; dirty = 1; }
		}
		else if (PAD_justPressed(BTN_B)) {
			if (len > 0) { out[--len] = '\0'; dirty = 1; }
			else { result = 0; done = 1; }
		}
		else if (PAD_justPressed(BTN_START)) {
			if (len > 0) { result = 1; done = 1; }
		}
		else if (PAD_justPressed(BTN_MENU)) { result = 0; done = 1; }

		PWR_update(&dirty, NULL, Menu_beforeSleep, Menu_afterSleep);
		if (done) break;

		if (dirty) {
			GFX_clear(screen);
			char shown[192];
			snprintf(shown, sizeof(shown), "%s_", out);
			GFX_blitText(font.medium, len ? shown : "Enter collection name",
				0, COLOR_WHITE, screen,
				&(SDL_Rect){ SCALE1(PADDING), SCALE1(PADDING), screen->w - SCALE1(PADDING*2), SCALE1(PILL_SIZE) });

			int cw = SCALE1(30), ch = SCALE1(30);
			int gx = (screen->w - 10 * cw) / 2;
			int gy = SCALE1(PADDING * 2 + PILL_SIZE);
			for (int r = 0; r < nrows; r++) {
				int n = strlen(KB[r]);
				for (int c = 0; c < n; c++) {
					int x = gx + c * cw, y = gy + r * ch;
					if (r == cr && c == cc)
						GFX_blitPill(ASSET_WHITE_PILL, screen, &(SDL_Rect){ x, y, cw, ch });
					char k = KB[r][c];
					char s[4]; if (k == ' ') { strcpy(s, "SP"); } else { s[0] = k; s[1] = '\0'; }
					SDL_Surface* t = TTF_RenderUTF8_Blended(font.small, s, COLOR_WHITE);
					if (t) {
						SDL_BlitSurface(t, NULL, screen, &(SDL_Rect){ x + (cw - t->w) / 2, y + (ch - t->h) / 2 });
						SDL_FreeSurface(t);
					}
				}
			}
			GFX_blitButtonGroup((char*[]){ "A","ADD", "B","DEL", NULL }, 0, screen, 0);
			GFX_blitButtonGroup((char*[]){ "START","SAVE", NULL }, 1, screen, 1);
			GFX_flip(screen);
			dirty = 0;
		}
		else GFX_sync();
		hdmimon();
	}
	GFX_setMode(MODE_MENU);
	return result;
}

static int collections_dirty = 0; // set when the picker list needs rebuilding (new collection added)

static int Menu_newCollection(MenuList* list, int i) {
	char name[128] = "";
	if (Menu_keyboard(name, sizeof(name)) && name[0]) {
		// trim trailing spaces (the keyboard only offers filename-safe chars)
		int n = strlen(name);
		while (n > 0 && name[n-1] == ' ') name[--n] = '\0';
		if (n > 0) {
			// FAT32/exFAT is case-insensitive: a typed "FAVORITES" would land on the
			// same file as the hardcoded Favorites row but not match its case-sensitive
			// exclusion, showing a duplicate aliased row. Fold it into the real one.
			int is_favorites = !strcasecmp(name, "Favorites");
			char* target = is_favorites ? "Favorites" : name;

			mkdir(COLLECTIONS_PATH, 0755);
			char path[MAX_PATH];
			snprintf(path, sizeof(path), COLLECTIONS_PATH "/%s.txt", target);
			int already_existed = exists(path);
			char* rel = game.path + strlen(SDCARD_PATH);
			FILE* out = fopen(path, "a"); // create (or join) + add the current game
			if (!out) {
				Menu_message("Couldn't create collection", (char*[]){ "A","OKAY", NULL });
				return MENU_CALLBACK_NOP;
			}
			fprintf(out, "%s\n", rel);
			fclose(out);
			if (is_favorites) Menu_message("Added to existing collection: Favorites", (char*[]){ "A","OKAY", NULL });
			else if (already_existed) Menu_message("Added to existing collection", (char*[]){ "A","OKAY", NULL });
			collections_dirty = 1; // rebuild the picker so the new collection shows
			return MENU_CALLBACK_EXIT;
		}
	}
	return MENU_CALLBACK_NOP;
}

static void Menu_collections(void) {
	char* rel = game.path + strlen(SDCARD_PATH);
	#define MAX_COLLECTIONS 128
	do {
		collections_dirty = 0;
		char* names[MAX_COLLECTIONS];
		int n = 0;
		DIR* dh = opendir(COLLECTIONS_PATH);
		if (dh) {
			struct dirent* de;
			while ((de = readdir(dh))!=NULL && n < MAX_COLLECTIONS) {
				if (de->d_name[0]=='.' || !suffixMatch(".txt", de->d_name)) continue;
				char base[128];
				strncpy(base, de->d_name, sizeof(base)-1); base[sizeof(base)-1]='\0';
				base[strlen(base)-4] = '\0'; // strip .txt
				if (exactMatch(base, "Favorites")) continue; // added first, below
				names[n++] = strdup(base);
			}
			closedir(dh);
		}
		qsort(names, n, sizeof(char*), Menu_collectionNameCmp);

		// [New Collection], then Favorites, then the sorted collections; NULL terminator
		MenuItem* items = calloc(n + 3, sizeof(MenuItem));
		items[0].name = strdup("[ New Collection ]");
		items[0].on_confirm = Menu_newCollection;
		items[1].key = strdup("Favorites");
		items[1].name = Menu_collLabel("Favorites", Menu_inCollection("Favorites", rel));
		items[1].on_confirm = Menu_toggleCollection;
		for (int i=0; i<n; i++) {
			items[i+2].key = names[i]; // takes ownership of the strdup'd name
			items[i+2].name = Menu_collLabel(names[i], Menu_inCollection(names[i], rel));
			items[i+2].on_confirm = Menu_toggleCollection;
		}

		MenuList list = { .type = MENU_LIST, .desc = "Add to collection", .items = items };
		Menu_options(&list);

		for (int i=0; i<n+2; i++) { free(items[i].name); free(items[i].key); }
		free(items);
	} while (collections_dirty);
}

void Menu_loop(void) {
	menu.bitmap = SDL_CreateRGBSurfaceFrom(renderer.src, renderer.true_w, renderer.true_h, FIXED_DEPTH, renderer.src_p, RGBA_MASK_565);
	// LOG_info("Menu_loop:menu.bitmap %ix%i\n", menu.bitmap->w,menu.bitmap->h);

	SDL_Surface* backing = SDL_CreateRGBSurface(SDL_SWSURFACE,DEVICE_WIDTH,DEVICE_HEIGHT,FIXED_DEPTH,RGBA_MASK_565);
	Menu_scale(menu.bitmap, backing);

	int restore_w = screen->w;
	int restore_h = screen->h;
	int restore_p = screen->pitch;
	if (restore_w!=DEVICE_WIDTH || restore_h!=DEVICE_HEIGHT) {
		screen = GFX_resize(DEVICE_WIDTH,DEVICE_HEIGHT,DEVICE_PITCH);
	}

	SRAM_write();
	RTC_write();
	PWR_warn(0);
	if (!HAS_POWER_BUTTON) PWR_enableSleep();
	PWR_setCPUSpeed(CPU_SPEED_MENU); // set Hz directly
	GFX_setVsync(VSYNC_STRICT);
	GFX_setEffect(EFFECT_NONE);

	int rumble_strength = VIB_getStrength();
	VIB_setStrength(0);

	PWR_enableAutosleep();
	PAD_reset();

	// if (!HAS_POWER_BUTTON && !HAS_POWEROFF_BUTTON) {
	// 	MenuItem* item = &options_menu.items[5];
	// 	item->name = "Quicksave";
	// 	item->desc = "Automatically resume current state next power on.";
	// 	item->on_confirm = OptionQuicksave_onConfirm;
	// }

	// path and string things
	char* tmp;
	char rom_name[MAX_PATH]; // without extension or cruft
	getDisplayName(game.name, rom_name);
	getAlias(game.path, rom_name);

	int rom_disc = -1;
	char disc_name[16];
	if (menu.total_discs) {
		rom_disc = menu.disc;
		sprintf(disc_name, "Disc %i", menu.disc+1);
	}

	int selected = 0; // resets every launch
	Menu_initState();

	int status = STATUS_CONT; // TODO: no longer used?
	int show_setting = 0;
	int dirty = 1;
	int ignore_menu = 0;
	int menu_start = 0;

	SDL_Surface* preview = SDL_CreateRGBSurface(SDL_SWSURFACE,DEVICE_WIDTH/2,DEVICE_HEIGHT/2,FIXED_DEPTH,RGBA_MASK_565); // TODO: retain until changed?

	while (show_menu) {
		GFX_startFrame();
		uint32_t now = SDL_GetTicks();

		PAD_poll();

		if (PAD_justPressed(BTN_UP)) {
			selected -= 1;
			if (selected<0) selected += MENU_ITEM_COUNT;
			dirty = 1;
		}
		else if (PAD_justPressed(BTN_DOWN)) {
			selected += 1;
			if (selected>=MENU_ITEM_COUNT) selected -= MENU_ITEM_COUNT;
			dirty = 1;
		}
		else if (PAD_justPressed(BTN_LEFT)) {
			if (menu.total_discs>1 && selected==ITEM_CONT) {
				menu.disc -= 1;
				if (menu.disc<0) menu.disc += menu.total_discs;
				dirty = 1;
				sprintf(disc_name, "Disc %i", menu.disc+1);
			}
			else if (selected==ITEM_SAVE || selected==ITEM_LOAD) {
				menu.slot -= 1;
				if (menu.slot<0) menu.slot += MENU_SLOT_COUNT;
				dirty = 1;
			}
		}
		else if (PAD_justPressed(BTN_RIGHT)) {
			if (menu.total_discs>1 && selected==ITEM_CONT) {
				menu.disc += 1;
				if (menu.disc==menu.total_discs) menu.disc -= menu.total_discs;
				dirty = 1;
				sprintf(disc_name, "Disc %i", menu.disc+1);
			}
			else if (selected==ITEM_SAVE || selected==ITEM_LOAD) {
				menu.slot += 1;
				if (menu.slot>=MENU_SLOT_COUNT) menu.slot -= MENU_SLOT_COUNT;
				dirty = 1;
			}
		}

		if (dirty && (selected==ITEM_SAVE || selected==ITEM_LOAD)) {
			Menu_updateState();
		}

		if (PAD_justPressed(BTN_B) || (BTN_WAKE!=BTN_MENU && PAD_tappedMenu(now))) {
			status = STATUS_CONT;
			show_menu = 0;
		}
		else if (PAD_justPressed(BTN_A)) {
			switch(selected) {
				case ITEM_CONT:
				if (menu.total_discs && rom_disc!=menu.disc) {
						status = STATUS_DISC;
						char* disc_path = menu.disc_paths[menu.disc];
						Game_changeDisc(disc_path);
					}
					else {
						status = STATUS_CONT;
					}
					show_menu = 0;
				break;

				case ITEM_SAVE: {
					Menu_saveState();
					status = STATUS_SAVE;
					show_menu = 0;
				}
				break;
				case ITEM_LOAD: {
					Menu_loadState();
					status = STATUS_LOAD;
					show_menu = 0;
				}
				break;
				case ITEM_OPTS: {
					if (simple_mode) {
						core.reset();
						status = STATUS_RESET;
						show_menu = 0;
					}
					else {
						int old_scaling = screen_scaling;
						Menu_options(&options_menu);
						if (screen_scaling!=old_scaling) {
							selectScaler(renderer.true_w,renderer.true_h,renderer.src_p);

							restore_w = screen->w;
							restore_h = screen->h;
							restore_p = screen->pitch;
							screen = GFX_resize(DEVICE_WIDTH,DEVICE_HEIGHT,DEVICE_PITCH);

							SDL_FillRect(backing, NULL, 0);
							Menu_scale(menu.bitmap, backing);
						}
						dirty = 1;
					}
				}
				break;
				case ITEM_COLL:
					Menu_collections();
					dirty = 1; // returned from the picker; redraw the menu
				break;
				case ITEM_QUIT:
					status = STATUS_QUIT;
					show_menu = 0;
					quit = 1; // TODO: tmp?
				break;
			}
			if (!show_menu) break;
		}

		PWR_update(&dirty, &show_setting, Menu_beforeSleep, Menu_afterSleep);

		if (dirty) {
			GFX_clear(screen);

			SDL_BlitSurface(backing, NULL, screen, NULL);
			SDL_BlitSurface(menu.overlay, NULL, screen, NULL);

			int ox, oy;
			int ow = GFX_blitHardwareGroup(screen, show_setting);
			int max_width = screen->w - SCALE1(PADDING * 2) - ow;

			char display_name[256];
			int text_width = GFX_truncateText(font.large, rom_name, display_name, max_width, SCALE1(BUTTON_PADDING*2));
			max_width = MIN(max_width, text_width);

			SDL_Surface* text;
			text = TTF_RenderUTF8_Blended(font.large, display_name, COLOR_WHITE);
			GFX_blitPill(ASSET_BLACK_PILL, screen, &(SDL_Rect){
				SCALE1(PADDING),
				SCALE1(PADDING),
				max_width,
				SCALE1(PILL_SIZE)
			});
			SDL_BlitSurface(text, &(SDL_Rect){
				0,
				0,
				max_width-SCALE1(BUTTON_PADDING*2),
				text->h
			}, screen, &(SDL_Rect){
				SCALE1(PADDING+BUTTON_PADDING),
				SCALE1(PADDING+4)
			});
			SDL_FreeSurface(text);

			if (show_setting && !GetHDMI()) GFX_blitHardwareHints(screen, show_setting);
			else GFX_blitButtonGroup((char*[]){ BTN_SLEEP==BTN_POWER?"POWER":"MENU","SLEEP", NULL }, 0, screen, 0);
			GFX_blitButtonGroup((char*[]){ "B","BACK", "A","OKAY", NULL }, 1, screen, 1);

			// list — pills stay full PILL_SIZE (the sprite is fixed-size; shrinking it
			// clips the caps). Instead reserve a row of space for the bottom hints so the
			// 6-item menu sits above them, and use a slightly smaller font.
			int step = PILL_SIZE - 4; // tighter row pitch (pill stays full size, just steps closer)
			int band_top = PADDING + PILL_SIZE;                                   // below the game title
			int band_bot = (DEVICE_HEIGHT / FIXED_SCALE) - PADDING - PILL_SIZE;  // above the bottom hint pill (same size as gfx.c's hint band)
			int content_h = (MENU_ITEM_COUNT - 1) * step + PILL_SIZE; // top of first pill to bottom of last pill
			oy = band_top + ((band_bot - band_top) - content_h) / 2 - PADDING;
			for (int i=0; i<MENU_ITEM_COUNT; i++) {
				char* item = menu.items[i];
				SDL_Color text_color = COLOR_WHITE;

				if (i==selected) {
					// disc change
					if (menu.total_discs>1 && i==ITEM_CONT) {
						GFX_blitPill(ASSET_DARK_GRAY_PILL, screen, &(SDL_Rect){
							SCALE1(PADDING),
							SCALE1(oy + PADDING),
							screen->w - SCALE1(PADDING * 2),
							SCALE1(PILL_SIZE)
						});
						text = TTF_RenderUTF8_Blended(font.medium, disc_name, COLOR_WHITE);
						SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){
							screen->w - SCALE1(PADDING + BUTTON_PADDING) - text->w,
							SCALE1(oy + PADDING + 6)
						});
						SDL_FreeSurface(text);
					}

					TTF_SizeUTF8(font.medium, item, &ow, NULL);
					ow += SCALE1(BUTTON_PADDING*2);

					// pill
					GFX_blitPill(ASSET_WHITE_PILL, screen, &(SDL_Rect){
						SCALE1(PADDING),
						SCALE1(oy + PADDING + (i * step)),
						ow,
						SCALE1(PILL_SIZE)
					});
					text_color = COLOR_WHITE;
				}
				else {
					// shadow
					text = TTF_RenderUTF8_Blended(font.medium, item, COLOR_BLACK);
					SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){
						SCALE1(2 + PADDING + BUTTON_PADDING),
						SCALE1(1 + PADDING + oy + (i * step) + 6)
					});
					SDL_FreeSurface(text);
				}

				// text
				text = TTF_RenderUTF8_Blended(font.medium, item, text_color);
				SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){
					SCALE1(PADDING + BUTTON_PADDING),
					SCALE1(oy + PADDING + (i * step) + 6)
				});
				SDL_FreeSurface(text);
			}

			// slot preview
			if (selected==ITEM_SAVE || selected==ITEM_LOAD) {
				#define WINDOW_RADIUS 4 // TODO: this logic belongs in blitRect?
				#define PAGINATION_HEIGHT 6
				// unscaled
				int hw = DEVICE_WIDTH / 2;
				int hh = DEVICE_HEIGHT / 2;
				int pw = hw + SCALE1(WINDOW_RADIUS*2);
				int ph = hh + SCALE1(WINDOW_RADIUS*2 + PAGINATION_HEIGHT + WINDOW_RADIUS);
				ox = DEVICE_WIDTH - pw - SCALE1(PADDING);
				oy = (DEVICE_HEIGHT - ph) / 2;

				// window
				GFX_blitRect(ASSET_STATE_BG, screen, &(SDL_Rect){ox,oy,pw,ph});
				ox += SCALE1(WINDOW_RADIUS);
				oy += SCALE1(WINDOW_RADIUS);

				if (menu.preview_exists) { // has save, has preview
					// lotta memory churn here
					SDL_Surface* bmp = IMG_Load(menu.bmp_path);
					SDL_Surface* raw_preview = SDL_ConvertSurface(bmp, screen->format, SDL_SWSURFACE);

					// LOG_info("raw_preview %ix%i\n", raw_preview->w,raw_preview->h);

					SDL_FillRect(preview, NULL, 0);
					Menu_scale(raw_preview, preview);
					SDL_BlitSurface(preview, NULL, screen, &(SDL_Rect){ox,oy});
					SDL_FreeSurface(raw_preview);
					SDL_FreeSurface(bmp);
				}
				else {
					SDL_Rect preview_rect = {ox,oy,hw,hh};
					SDL_FillRect(screen, &preview_rect, 0);
					if (menu.save_exists) GFX_blitMessage(font.large, "No Preview", screen, &preview_rect);
					else GFX_blitMessage(font.large, "Empty Slot", screen, &preview_rect);
				}

				// pagination
				ox += (pw-SCALE1(15*MENU_SLOT_COUNT))/2;
				oy += hh+SCALE1(WINDOW_RADIUS);
				for (int i=0; i<MENU_SLOT_COUNT; i++) {
					if (i==menu.slot)GFX_blitAsset(ASSET_PAGE, NULL, screen, &(SDL_Rect){ox+SCALE1(i*15),oy});
					else GFX_blitAsset(ASSET_DOT, NULL, screen, &(SDL_Rect){ox+SCALE1(i*15)+4,oy+SCALE1(2)});
				}
			}

			GFX_flip(screen);
			dirty = 0;
		}
		else GFX_sync();
		hdmimon();
	}

	SDL_FreeSurface(preview);

	PAD_reset();

	GFX_clearAll();
	PWR_warn(1);

	if (!quit) {
		if (restore_w!=DEVICE_WIDTH || restore_h!=DEVICE_HEIGHT) {
			screen = GFX_resize(restore_w,restore_h,restore_p);
		}
		GFX_setEffect(screen_effect);
		GFX_clear(screen);
		video_refresh_callback(renderer.src, renderer.true_w, renderer.true_h, renderer.src_p);

		setOverclock(overclock); // restore overclock value
		if (rumble_strength) VIB_setStrength(rumble_strength);

		GFX_setVsync(prevent_tearing);
		if (!HAS_POWER_BUTTON) PWR_disableSleep();

		if (thread_video) {
			pthread_mutex_lock(&core_mx);
			should_run_core = 1;
			pthread_mutex_unlock(&core_mx);
		}
	}
	else if (exists(NOUI_PATH)) PWR_powerOff(); // TODO: won't work with threaded core, only check this once per launch

	SDL_FreeSurface(menu.bitmap);
	menu.bitmap = NULL;
	SDL_FreeSurface(backing);
	PWR_disableAutosleep();
}
