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
	const char *userdata_path = getenv("USERDATA_PATH");
	if (!userdata_path) {
		fprintf(stderr, "InitSettingsCore: USERDATA_PATH is not set\n");
		abort();
	}
	snprintf(SettingsPath, sizeof(SettingsPath), "%s/msettings.bin", userdata_path);

	void *settings;
	int is_host = 0;

	int shm_fd = shm_open(SHM_KEY, O_RDWR | O_CREAT | O_EXCL, 0644); // see if it exists
	if (shm_fd==-1 && errno==EEXIST) { // already exists
		puts("Settings client");
		shm_fd = shm_open(SHM_KEY, O_RDWR, 0644);
		if (shm_fd==-1) {
			fprintf(stderr, "InitSettingsCore: client shm_open failed: %s\n", strerror(errno));
			abort();
		}

		// the host may not have ftruncate'd the object to its final size
		// yet -- mmap'ing settings_size onto a still-zero-length object
		// would SIGBUS on first field access, so poll (bounded) until the
		// host has caught up.
		struct stat st;
		int ready = 0;
		for (int i=0; i<200; i++) {
			if (fstat(shm_fd, &st)==0 && (size_t)st.st_size>=settings_size) {
				ready = 1;
				break;
			}
			usleep(1000);
		}
		if (!ready) {
			fprintf(stderr, "InitSettingsCore: shm object never reached expected size\n");
			close(shm_fd);
			abort();
		}

		settings = mmap(NULL, settings_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
		if (settings==MAP_FAILED) {
			fprintf(stderr, "InitSettingsCore: client mmap failed: %s\n", strerror(errno));
			close(shm_fd);
			abort();
		}
		close(shm_fd); // mmap keeps its own reference
	}
	else if (shm_fd==-1) { // some other failure (EACCES/EMFILE/ENOSPC/etc) -- not a client, and not usable as host either
		fprintf(stderr, "InitSettingsCore: shm_open failed: %s\n", strerror(errno));
		abort();
	}
	else { // host
		puts("Settings host"); // normally keymon
		is_host = 1;
		// we created it so set initial size and populate
		if (ftruncate(shm_fd, settings_size)==-1) {
			fprintf(stderr, "InitSettingsCore: ftruncate failed: %s\n", strerror(errno));
			close(shm_fd);
			shm_unlink(SHM_KEY);
			abort();
		}
		settings = mmap(NULL, settings_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
		if (settings==MAP_FAILED) {
			fprintf(stderr, "InitSettingsCore: host mmap failed: %s\n", strerror(errno));
			close(shm_fd);
			shm_unlink(SHM_KEY);
			abort();
		}
		close(shm_fd); // mmap keeps its own reference

		int fd = open(SettingsPath, O_RDONLY);
		if (fd>=0) {
			ssize_t n = read(fd, settings, settings_size);
			// TODO: use settings->version for future proofing?
			close(fd);
			if (n<0 || (size_t)n<settings_size) {
				// short/torn/failed read -- don't trust a partially
				// populated buffer, fall back to defaults instead
				memcpy(settings, default_settings, settings_size);
			}
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
	// write to a temp file and atomically rename over the real path so a
	// power cut mid-write can't leave a torn settings file behind
	char tmp_path[sizeof(SettingsPath)+8];
	snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", SettingsPath);

	int fd = open(tmp_path, O_CREAT|O_TRUNC|O_WRONLY, 0644);
	if (fd>=0) {
		ssize_t n = write(fd, settings, settings_size);
		fsync(fd);
		close(fd);
		if (n==(ssize_t)settings_size) {
			rename(tmp_path, SettingsPath);
		}
		sync();
	}
}
