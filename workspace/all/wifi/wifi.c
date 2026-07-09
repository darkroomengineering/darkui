// Graphical Wi-Fi manager: scan, connect, and enable/disable wlan0.
// On a successful connect the credentials are persisted to
// $SDCARD_PATH/.userdata/wifi.txt, which skeleton/SYSTEM/rg35xxplus/bin/wifi.sh
// reads on boot to reconnect automatically. This removes the need to hardcode
// any SSID/password anywhere in the image.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <msettings.h>

#include "sdl.h"
#include "defines.h"
#include "api.h"
#include "utils.h"

#define WIFI_IFACE			"wlan0"
#define WIFI_CONF_PATH		"/tmp/wifi.conf"
#define WIFI_MAX_NETWORKS	32
#define WIFI_VISIBLE_ROWS	9
#define WIFI_SSID_LEN		33
#define WIFI_PASS_LEN		128

typedef struct WifiNetwork {
	char ssid[WIFI_SSID_LEN];
	int signal;
	int open; // no WPA/WEP/PSK/SAE in the scan flags
} WifiNetwork;

static WifiNetwork networks[WIFI_MAX_NETWORKS];
static int network_count = 0;

///////////////////////////////
// status helpers

static int Wifi_isOn(void) {
	char status[16] = "";
	getFile("/sys/class/net/" WIFI_IFACE "/operstate", status, sizeof(status));
	trimTrailingNewlines(status);
	return prefixMatch("up", status);
}

static int Wifi_hasIPv4(void) {
	int found = 0;
	FILE* p = popen("ip -4 addr show " WIFI_IFACE " 2>/dev/null", "r");
	if (p) {
		char line[256];
		while (fgets(line, sizeof(line), p)) {
			if (containsString(line, "inet ")) { found = 1; break; }
		}
		pclose(p);
	}
	return found;
}

static int Wifi_stateCompleted(void) {
	int found = 0;
	FILE* p = popen("wpa_cli -i " WIFI_IFACE " status 2>/dev/null", "r");
	if (p) {
		char line[256];
		while (fgets(line, sizeof(line), p)) {
			if (containsString(line, "wpa_state=COMPLETED")) { found = 1; break; }
		}
		pclose(p);
	}
	return found;
}

///////////////////////////////
// scanning

static void Wifi_scanTrigger(void) {
	system("wpa_cli -i " WIFI_IFACE " scan >/dev/null 2>&1");
}

static int Wifi_isOpenFlags(char* flags) {
	if (containsString(flags, "WPA")) return 0;
	if (containsString(flags, "WEP")) return 0;
	if (containsString(flags, "PSK")) return 0;
	if (containsString(flags, "SAE")) return 0;
	return 1;
}

static int Wifi_networkCmp(const void* a, const void* b) {
	const WifiNetwork* na = (const WifiNetwork*)a;
	const WifiNetwork* nb = (const WifiNetwork*)b;
	return nb->signal - na->signal; // descending: stronger (less negative) first
}

// Wi-Fi may be off (wlan0 down / wpa_supplicant not running) -> this just
// yields an empty list, which is fine.
static void Wifi_scanResults(void) {
	network_count = 0;

	FILE* p = popen("wpa_cli -i " WIFI_IFACE " scan_results 2>/dev/null", "r");
	if (!p) return;

	char line[512];
	int line_no = 0;
	while (fgets(line, sizeof(line), p) && network_count<WIFI_MAX_NETWORKS) {
		line_no += 1;
		if (line_no==1) continue; // header: bssid / frequency / signal level / flags / ssid

		normalizeNewline(line);
		trimTrailingNewlines(line);
		if (line[0]=='\0') continue;

		// tab-separated: bssid \t frequency \t signal_level \t flags \t ssid
		char* fields[5];
		int nf = 0;
		char* cur = line;
		fields[nf++] = cur;
		while (nf<5) {
			char* t = strchr(cur, '\t');
			if (!t) break;
			*t = '\0';
			cur = t + 1;
			fields[nf++] = cur;
		}
		if (nf<5) continue; // malformed row

		int signal = atoi(fields[2]);
		char* flags = fields[3];
		char* ssid = fields[4];
		if (ssid[0]=='\0') continue;

		int open = Wifi_isOpenFlags(flags);

		// dedup by ssid, keep the strongest signal
		int existing = -1;
		for (int i=0; i<network_count; i++) {
			if (exactMatch(networks[i].ssid, ssid)) { existing = i; break; }
		}
		if (existing>=0) {
			if (signal>networks[existing].signal) {
				networks[existing].signal = signal;
				networks[existing].open = open;
			}
			continue;
		}

		snprintf(networks[network_count].ssid, WIFI_SSID_LEN, "%s", ssid);
		networks[network_count].signal = signal;
		networks[network_count].open = open;
		network_count += 1;
	}
	pclose(p);

	qsort(networks, network_count, sizeof(WifiNetwork), Wifi_networkCmp);
}

///////////////////////////////
// connecting + persistence
//
// ssid comes from the scan (attacker-controllable: any nearby AP can broadcast
// an SSID containing shell/config metacharacters) and password comes from the
// on-screen keyboard, so both are escaped before ever being interpolated into
// a shell command or a quoted wpa_supplicant config value.

// escapes '"' and '\' for safe use inside a wpa_supplicant quoted config value
static void Wifi_confEscape(char* out, size_t outlen, char* in) {
	size_t o = 0;
	for (size_t i=0; in[i]!='\0' && o+2<outlen; i++) {
		char c = in[i];
		if (c=='"' || c=='\\') out[o++] = '\\';
		if (o+1>=outlen) break;
		out[o++] = c;
	}
	out[o] = '\0';
}

// wraps a string in single quotes (POSIX-safe against any byte, including $, `, ", \)
// for safe interpolation into a shell command string run via system()/popen()
static void Wifi_shellQuote(char* out, size_t outlen, char* in) {
	size_t o = 0;
	if (outlen<3) { if (outlen>0) out[0] = '\0'; return; }
	out[o++] = '\'';
	for (size_t i=0; in[i]!='\0'; i++) {
		if (o+6>=outlen) break; // room for worst-case escape + closing quote + nul
		if (in[i]=='\'') { out[o++]='\''; out[o++]='\\'; out[o++]='\''; out[o++]='\''; }
		else out[o++] = in[i];
	}
	out[o++] = '\'';
	out[o] = '\0';
}

static int Wifi_writeConfig(char* ssid, char* password, int open) {
	FILE* f = fopen(WIFI_CONF_PATH, "w");
	if (!f) return 0;
	fprintf(f, "ctrl_interface=/var/run/wpa_supplicant\nupdate_config=1\n");
	fclose(f);

	char ssid_esc[WIFI_SSID_LEN*2];
	Wifi_confEscape(ssid_esc, sizeof(ssid_esc), ssid);

	if (open) {
		f = fopen(WIFI_CONF_PATH, "a");
		if (!f) return 0;
		fprintf(f, "network={\n\tssid=\"%s\"\n\tkey_mgmt=NONE\n}\n", ssid_esc);
		fclose(f);
		return 1;
	}

	// prefer wpa_passphrase (hashes the psk); fall back to a plain psk="" block
	char ssid_q[WIFI_SSID_LEN*4+4];
	char pass_q[WIFI_PASS_LEN*4+4];
	Wifi_shellQuote(ssid_q, sizeof(ssid_q), ssid);
	Wifi_shellQuote(pass_q, sizeof(pass_q), password);

	char cmd[sizeof(ssid_q)+sizeof(pass_q)+64];
	snprintf(cmd, sizeof(cmd), "wpa_passphrase %s %s 2>/dev/null", ssid_q, pass_q);
	int wrote_psk = 0;
	FILE* p = popen(cmd, "r");
	if (p) {
		f = fopen(WIFI_CONF_PATH, "a");
		char line[256];
		while (fgets(line, sizeof(line), p)) {
			if (f) fputs(line, f);
			wrote_psk = 1;
		}
		pclose(p);
		if (f) fclose(f);
	}
	if (!wrote_psk) {
		char pass_esc[WIFI_PASS_LEN*2];
		Wifi_confEscape(pass_esc, sizeof(pass_esc), password);
		f = fopen(WIFI_CONF_PATH, "a");
		if (!f) return 0;
		fprintf(f, "network={\n\tssid=\"%s\"\n\tpsk=\"%s\"\n}\n", ssid_esc, pass_esc);
		fclose(f);
	}
	return 1;
}

static void Wifi_persist(char* ssid, char* password) {
	char* sdcard = getenv("SDCARD_PATH");
	if (!sdcard || sdcard[0]=='\0') sdcard = "/mnt/sdcard";

	char dir[MAX_PATH];
	snprintf(dir, sizeof(dir), "%s/.userdata", sdcard);
	mkdir(dir, 0755);

	char path[MAX_PATH];
	snprintf(path, sizeof(path), "%s/wifi.txt", dir);
	FILE* f = fopen(path, "w");
	if (f) {
		fprintf(f, "%s\n%s\n", ssid, password ? password : "");
		fclose(f);
	}
}

// line 1 = ssid, line 2 = password (may be empty for an open network)
static int Wifi_loadPersisted(char* ssid, size_t ssid_len, char* password, size_t pass_len) {
	char* sdcard = getenv("SDCARD_PATH");
	if (!sdcard || sdcard[0]=='\0') sdcard = "/mnt/sdcard";

	char path[MAX_PATH];
	snprintf(path, sizeof(path), "%s/.userdata/wifi.txt", sdcard);
	if (!exists(path)) return 0;

	FILE* f = fopen(path, "r");
	if (!f) return 0;

	ssid[0] = '\0';
	password[0] = '\0';
	char line[256];
	if (fgets(line, sizeof(line), f)) {
		normalizeNewline(line); trimTrailingNewlines(line);
		snprintf(ssid, ssid_len, "%s", line);
	}
	if (fgets(line, sizeof(line), f)) {
		normalizeNewline(line); trimTrailingNewlines(line);
		snprintf(password, pass_len, "%s", line);
	}
	fclose(f);
	return ssid[0]!='\0';
}

// blocking: writes the config, brings up wlan0, waits for association then a lease.
// on success this also persists the credentials so wifi.sh reconnects on boot.
static int Wifi_connect(char* ssid, char* password, int open) {
	if (!Wifi_writeConfig(ssid, password, open)) return 0;

	system("killall wpa_supplicant 2>/dev/null");
	system("ip link set " WIFI_IFACE " up 2>/dev/null");
	system("wpa_supplicant -B -i " WIFI_IFACE " -c " WIFI_CONF_PATH " 2>/dev/null");

	int associated = 0;
	for (int i=0; i<30; i++) { // ~15s
		if (Wifi_stateCompleted()) { associated = 1; break; }
		usleep(500000);
	}
	if (!associated) return 0;

	system("dhclient -nw " WIFI_IFACE " 2>/dev/null || udhcpc -b -i " WIFI_IFACE " 2>/dev/null");

	int has_ip = 0;
	for (int i=0; i<20; i++) { // ~10s
		if (Wifi_hasIPv4()) { has_ip = 1; break; }
		usleep(500000);
	}
	if (!has_ip || !Wifi_isOn()) return 0;

	Wifi_persist(ssid, password);
	return 1;
}

static void Wifi_disable(void) {
	system("killall wpa_supplicant 2>/dev/null");
	system("killall dhclient 2>/dev/null");
	system("ip link set " WIFI_IFACE " down 2>/dev/null");
}

///////////////////////////////
// minimal on-screen keyboard for password entry, adapted from minarch's
// Menu_keyboard. Uses the tool's own screen (from GFX_init), not minarch's
// global/menu-mode plumbing.

static int Wifi_keyboard(SDL_Surface* screen, char* out, int maxlen) {
	static const char* KB[] = {
		"ABCDEFGHIJKLM",
		"NOPQRSTUVWXYZ",
		"abcdefghijklm",
		"nopqrstuvwxyz",
		"0123456789",
		" -_!@#$%^&*()+=.,",
	};
	const int nrows = 6;
	int cr = 0, cc = 0, len = 0, result = 0, done = 0, dirty = 1;
	out[0] = '\0';

	while (!done) {
		PAD_poll();

		if (PAD_justRepeated(BTN_UP))         { cr = (cr - 1 + nrows) % nrows; dirty = 1; }
		else if (PAD_justRepeated(BTN_DOWN))  { cr = (cr + 1) % nrows; dirty = 1; }
		else if (PAD_justRepeated(BTN_LEFT))  { cc -= 1; dirty = 1; }
		else if (PAD_justRepeated(BTN_RIGHT)) { cc += 1; dirty = 1; }
		int rl = strlen(KB[cr]);
		if (cc<0) cc = rl - 1;
		if (cc>=rl) cc = 0;

		if (PAD_justPressed(BTN_A)) {
			if (len<maxlen - 1) { out[len++] = KB[cr][cc]; out[len] = '\0'; dirty = 1; }
		}
		else if (PAD_justPressed(BTN_B)) {
			if (len>0) { out[--len] = '\0'; dirty = 1; }
			else { result = 0; done = 1; }
		}
		else if (PAD_justPressed(BTN_START)) {
			if (len>0) { result = 1; done = 1; }
		}
		else if (PAD_justPressed(BTN_MENU)) { result = 0; done = 1; }

		PWR_update(&dirty, NULL, NULL, NULL);
		if (done) break;

		if (dirty) {
			GFX_clear(screen);

			char shown[WIFI_PASS_LEN + 2];
			snprintf(shown, sizeof(shown), "%s_", out);
			GFX_blitText(font.medium, len ? shown : "Enter password", 0, COLOR_WHITE, screen,
				&(SDL_Rect){ SCALE1(PADDING), SCALE1(PADDING), screen->w - SCALE1(PADDING*2), SCALE1(PILL_SIZE) });

			int cw = SCALE1(30), ch = SCALE1(30);
			int gy = SCALE1(PADDING*2 + PILL_SIZE);
			for (int r=0; r<nrows; r++) {
				int n = strlen(KB[r]);
				int rgx = (screen->w - n*cw) / 2;
				for (int c=0; c<n; c++) {
					int x = rgx + c*cw, y = gy + r*ch;
					if (r==cr && c==cc)
						GFX_blitPill(ASSET_WHITE_PILL, screen, &(SDL_Rect){ x, y, cw, ch });
					char k = KB[r][c];
					char s[4]; if (k==' ') { strcpy(s, "SP"); } else { s[0] = k; s[1] = '\0'; }
					SDL_Surface* t = TTF_RenderUTF8_Blended(font.small, s, COLOR_WHITE);
					if (t) {
						SDL_BlitSurface(t, NULL, screen, &(SDL_Rect){ x + (cw-t->w)/2, y + (ch-t->h)/2 });
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
	}
	return result;
}

static void Wifi_showMessage(SDL_Surface* screen, char* msg) {
	GFX_clear(screen);
	GFX_blitMessage(font.large, msg, screen, &(SDL_Rect){0, 0, screen->w, screen->h});
	GFX_flip(screen);
}

// On -> bring wlan0 up; if we have saved credentials, reconnect to them.
static void Wifi_enable(SDL_Surface* screen) {
	system("ip link set " WIFI_IFACE " up 2>/dev/null");

	char ssid[WIFI_SSID_LEN] = "";
	char password[WIFI_PASS_LEN] = "";
	if (Wifi_loadPersisted(ssid, sizeof(ssid), password, sizeof(password))) {
		char msg[96];
		snprintf(msg, sizeof(msg), "Connecting to %s...", ssid);
		Wifi_showMessage(screen, msg);
		Wifi_connect(ssid, password, password[0]=='\0');
	}
}

///////////////////////////////

int main(int argc, char* argv[]) {
	PWR_setCPUSpeed(CPU_SPEED_MENU);

	SDL_Surface* screen = GFX_init(MODE_MAIN);
	PAD_init();
	PWR_init();
	InitSettings();

	Wifi_showMessage(screen, "Scanning for networks...");
	Wifi_scanTrigger();
	usleep(2500000);
	Wifi_scanResults();

	int selected = 0;
	int list_top = 0;
	char status_line[128] = "";

	int quit = 0;
	int dirty = 1;
	while (!quit) {
		PAD_poll();

		if (PAD_anyPressed() || PAD_anyJustReleased()) dirty = 1;

		if (PAD_justPressed(BTN_B)) {
			quit = 1;
		}

		if (PAD_justPressed(BTN_Y)) {
			Wifi_showMessage(screen, "Scanning for networks...");
			Wifi_scanTrigger();
			usleep(2500000);
			Wifi_scanResults();
			if (selected>=network_count) selected = network_count>0 ? network_count - 1 : 0;
			status_line[0] = '\0';
			dirty = 1;
		}

		if (PAD_justPressed(BTN_X)) {
			if (Wifi_isOn()) {
				Wifi_showMessage(screen, "Turning Wi-Fi off...");
				Wifi_disable();
				network_count = 0;
			}
			else {
				Wifi_showMessage(screen, "Turning Wi-Fi on...");
				Wifi_enable(screen);
				Wifi_showMessage(screen, "Scanning for networks...");
				Wifi_scanTrigger();
				usleep(2500000);
				Wifi_scanResults();
			}
			selected = 0;
			list_top = 0;
			status_line[0] = '\0';
			dirty = 1;
		}

		if (network_count>0 && PAD_justRepeated(BTN_UP)) {
			selected = (selected - 1 + network_count) % network_count;
			status_line[0] = '\0';
			dirty = 1;
		}
		if (network_count>0 && PAD_justRepeated(BTN_DOWN)) {
			selected = (selected + 1) % network_count;
			status_line[0] = '\0';
			dirty = 1;
		}

		if (network_count>0 && PAD_justPressed(BTN_A)) {
			WifiNetwork* net = &networks[selected];
			char ssid_copy[WIFI_SSID_LEN];
			snprintf(ssid_copy, sizeof(ssid_copy), "%s", net->ssid);
			int is_open = net->open;

			char password[WIFI_PASS_LEN] = "";
			int proceed = is_open ? 1 : Wifi_keyboard(screen, password, sizeof(password));

			if (proceed) {
				char msg[96];
				snprintf(msg, sizeof(msg), "Connecting to %s...", ssid_copy);
				Wifi_showMessage(screen, msg);

				int ok = Wifi_connect(ssid_copy, password, is_open);
				snprintf(status_line, sizeof(status_line),
					ok ? "Connected to %s" : "Failed to connect to %s", ssid_copy);
			}
			dirty = 1;
		}

		if (selected<list_top) list_top = selected;
		if (network_count>0 && selected>=list_top + WIFI_VISIBLE_ROWS) list_top = selected - WIFI_VISIBLE_ROWS + 1;
		if (list_top<0) list_top = 0;

		PWR_update(&dirty, NULL, NULL, NULL);

		if (dirty) {
			GFX_clear(screen);

			char header[64];
			snprintf(header, sizeof(header), "Wi-Fi: %s", Wifi_isOn() ? "On" : "Off");
			GFX_blitText(font.large, header, 0, COLOR_WHITE, screen,
				&(SDL_Rect){ SCALE1(PADDING), SCALE1(PADDING), screen->w - SCALE1(PADDING*2), SCALE1(PILL_SIZE) });

			int list_y = SCALE1(PADDING*2 + PILL_SIZE);
			int list_bottom = screen->h - SCALE1(PADDING*2 + PILL_SIZE*2);

			if (network_count==0) {
				GFX_blitMessage(font.large, Wifi_isOn() ? "No networks found" : "Wi-Fi is off",
					screen, &(SDL_Rect){0, list_y, screen->w, list_bottom - list_y});
			}
			else {
				int available_width = screen->w - SCALE1(PADDING*2);
				int end = MIN(network_count, list_top + WIFI_VISIBLE_ROWS);
				for (int i=list_top, j=0; i<end; i++, j++) {
					int y = list_y + j*SCALE1(PILL_SIZE);

					char label[80];
					snprintf(label, sizeof(label), "%s%s", networks[i].open ? "" : "* ", networks[i].ssid);

					char display[80];
					int text_width = GFX_truncateText(font.large, label, display, available_width, SCALE1(BUTTON_PADDING*2));

					if (i==selected) {
						GFX_blitPill(ASSET_WHITE_PILL, screen, &(SDL_Rect){
							SCALE1(PADDING), y, MIN(available_width, text_width), SCALE1(PILL_SIZE)
						});
					}
					GFX_blitText(font.large, display, 0, COLOR_WHITE, screen,
						&(SDL_Rect){ SCALE1(PADDING + BUTTON_PADDING), y + SCALE1(4), available_width - SCALE1(BUTTON_PADDING*2), SCALE1(PILL_SIZE) });
				}
			}

			if (status_line[0]) {
				GFX_blitText(font.small, status_line, 0, COLOR_WHITE, screen,
					&(SDL_Rect){ SCALE1(PADDING), list_bottom, screen->w - SCALE1(PADDING*2), SCALE1(PILL_SIZE) });
			}

			GFX_blitButtonGroup((char*[]){ "Y","RESCAN", "X","ON/OFF", NULL }, 0, screen, 0);
			GFX_blitButtonGroup((char*[]){ "B","EXIT", "A","CONNECT", NULL }, 1, screen, 1);

			GFX_flip(screen);
			dirty = 0;
		}
		else GFX_sync();
	}

	QuitSettings();
	PWR_quit();
	PAD_quit();
	GFX_quit();

	return EXIT_SUCCESS;
}
