#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <errno.h>
#include <sys/stat.h>
#include <string.h>

#include "msettings_core.h"

///////////////////////////////////////

#define SHM_KEY "/SharedSettings"
static char SettingsPath[256];

void *InitSettingsCore(const void *default_settings, size_t settings_size, int *out_is_host) {
	sprintf(SettingsPath, "%s/msettings.bin", getenv("USERDATA_PATH"));

	void *settings;
	int is_host = 0;

	int shm_fd = shm_open(SHM_KEY, O_RDWR | O_CREAT | O_EXCL, 0644); // see if it exists
	if (shm_fd==-1 && errno==EEXIST) { // already exists
		puts("Settings client");
		shm_fd = shm_open(SHM_KEY, O_RDWR, 0644);
		settings = mmap(NULL, settings_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
	}
	else { // host
		puts("Settings host"); // normally keymon
		is_host = 1;
		// we created it so set initial size and populate
		ftruncate(shm_fd, settings_size);
		settings = mmap(NULL, settings_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);

		int fd = open(SettingsPath, O_RDONLY);
		if (fd>=0) {
			read(fd, settings, settings_size);
			// TODO: use settings->version for future proofing?
			close(fd);
		}
		else {
			// load defaults
			memcpy(settings, default_settings, settings_size);
		}
	}

	if (out_is_host) *out_is_host = is_host;
	return settings;
}
void QuitSettingsCore(void *settings, size_t settings_size, int is_host) {
	munmap(settings, settings_size);
	if (is_host) shm_unlink(SHM_KEY);
}
void SaveSettingsCore(void *settings, size_t settings_size) {
	int fd = open(SettingsPath, O_CREAT|O_WRONLY, 0644);
	if (fd>=0) {
		write(fd, settings, settings_size);
		close(fd);
		sync();
	}
}
