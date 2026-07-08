#ifndef __msettings_core_h__
#define __msettings_core_h__

#include <stddef.h>

// Shared shm-backed settings-block lifecycle used by every platform's
// libmsettings.c. This code never interprets the platform's Settings struct
// layout -- it only ever moves `settings_size` bytes around -- so it's safe
// to share even though platform Settings structs differ (eg. rg35xxplus
// appends an extra `hdmi` field that rg35xx doesn't have). Each platform
// keeps its own Settings typedef, DefaultSettings value, and field-specific
// logic (SetRawBrightness/SetRawVolume, brightness curves, hdmi handling)
// in its own msettings.c.

// Opens (or attaches to) the "/SharedSettings" POSIX shm block, sized to
// `settings_size`. If this process is the one that created the block (ie.
// it's the host -- normally keymon) *out_is_host is set to 1 and the block
// is populated from "$USERDATA_PATH/msettings.bin" if that file exists, or
// from `default_settings` otherwise. Client processes just mmap the
// existing block. Returns the mmap'd pointer; the caller casts it to its
// own platform-specific Settings*.
void *InitSettingsCore(const void *default_settings, size_t settings_size, int *out_is_host);

// Unmaps `settings` and, on the host process (is_host!=0), unlinks the
// shared-memory object.
void QuitSettingsCore(void *settings, size_t settings_size, int is_host);

// Persists `settings` to "$USERDATA_PATH/msettings.bin" (path computed
// during InitSettingsCore).
void SaveSettingsCore(void *settings, size_t settings_size);

#endif // __msettings_core_h__
