# darkUI Codebase Adversarial Audit — 2026-07-09

Whole-repo adversarial audit (codebase mode). Every source area read in full by a dedicated
reviewer; findings adversarially verified, cross-checked against a second model family (OpenAI
Codex), and reconciled against team-knowledge (no notes touch this C/embedded stack → all
findings are novel debt, no reclassification). Load-bearing findings were re-traced by hand
before landing here.

**Scope:** `workspace/all/{common,minui,minarch,wifi}`, `workspace/rg35xx/*`,
`workspace/rg35xxplus/*`, the `skeleton/` boot+pak scripts, the makefiles, and the docs.
Two target devices only: **RG35xx** (SDL1.2, framebuffer, no Wi-Fi) and **RG35xxSP** /
`rg35xxplus` (H700, SDL2, Wi-Fi). Leftover code/data for any other device is treated as a
finding; missing other-device support is not.

**Method note:** severities and confidence below are *post-reconciliation*. The Codex pass
downgraded four candidate findings and sharpened two others; those moves are recorded inline and
in the Considered & Rejected ledger. Where the two model families diverged on a mechanism
(the Wi-Fi config-injection path), the divergence is preserved as an open question rather than
silently resolved.

---

## 1. Summary (severity order)

| ID | Sev | Area | Issue | Location | Status |
|----|-----|------|-------|----------|--------|
| C1 | Critical | launcher | Delete last root-promoted collection → `stack->items[-1]` OOB deref → crash | `minui.c:1443`,`1067-1075` | CONFIRMED |
| H1 | High | minarch | `strcpy` ROM-folder tag into `core.tag[8]` overflows global `struct Core` | `minarch.c:2783` | CONFIRMED |
| H2 | High | common | `getEmuName` NULL-derefs on a folder name with `(` and no `)` — hit on every folder scan | `utils.c:93-100` | CONFIRMED |
| H3 | High | boot/update | Update is non-transactional: `install.sh` runs after a failed/partial `unzip`; flag+reboot unconditional. Field-observed half-install / "updating" hang | `rg35xx/boot.sh:74-79` + `install.sh`; same in `rg35xxplus/boot.sh:98-110` | CONFIRMED |
| H4 | High | wifi | Attacker-chosen SSID reaches `wpa_supplicant.conf` unescaped on the WPA path (`ssid_esc` computed, dropped); root-run | `wifi.c:213-226` | CONFIRMED (quote-breakout); PLAUSIBLE escalation |
| M1 | Med | SP platform | keymon reads HDMI state from `extcon` path; 4 other callers use `switch` path → HDMI-at-boot state clobbered | `rg35xxplus/keymon.c:39` | CONFIRMED |
| M2 | Med | launcher | Collections show/launch ROMs whose emu pak isn't installed (`hasEmu` guard commented out) | `minui.c:664-690` | CONFIRMED |
| M3 | Med | launcher | `getCollection` bypasses `hide()` — disabled/dot/`map.txt` entries still show & launch | `minui.c:664-690` | CONFIRMED |
| M4 | Med | launcher | `Y DELETE` action fires at promoted root but the hint is gated `stack->count>1` — silent delete modal | `minui.c:1385-1394`/`1581-1583` | CONFIRMED |
| M5 | Med | menu | `band_bot` uses `BUTTON_SIZE` where hint band reserves `PILL_SIZE` → selected Quit row overlaps hint pill; regresses shipped fix `678c3d8` | `menu.c:1575` vs `gfx.c:563` | CONFIRMED |
| M6 | Med | minarch | `State_read` `goto error` before `state_file` init → `fclose()` on indeterminate value | `minarch.c:456-490` | CONFIRMED |
| M7 | Med | minarch | `strcpy(argv)` into `MAX_PATH` path buffers, unchecked; propagates into every save-path `sprintf` | `minarch.c:2968-2969` | CONFIRMED |
| M8 | Med | common | `GFX_wrapText` NULL-derefs when the first token alone exceeds `max_width` (core option desc) | `gfx.c:226` | CONFIRMED |
| M9 | Med | SP platform | Low-battery warning overlay never composited to screen (`PLAT_enableOverlay` stub) | `rg35xxplus/platform.c:946` | CONFIRMED |
| M10 | Med | minarch | No periodic SRAM autosave and no signal handler → power-loss / SIGTERM loses SRAM since last manual save | `minarch.c:355,411,2845` | CONFIRMED (gap) |
| M11 | Med | docs | README documents a `workspace/macos` dev platform + `notes.txt` that don't exist | `README.md:37` | CONFIRMED |
| L1 | Low | skeleton | 17 `default-cube.cfg`/`default-wide.cfg` for non-target devices survive the SP strip; PAKS.md still documents `DEVICE=cube/wide` | `skeleton/.../*.pak/` | CONFIRMED |
| L2 | Low | rg35xx | "Screen Sharpness" menu option is a no-op — `PLAT_setSharpness` ignores its arg; `DE_SCOEF_*` tables dead | `rg35xx/platform.c:384-389,81-143` | CONFIRMED |
| L3 | Low | common | `Settings.version` written but never validated on load (both platforms) | `msettings_core.c:85` | CONFIRMED (latent) |
| L4 | Low | menu | `COLLECTIONS_DIR` duplicates `COLLECTIONS_PATH` — write-path/read-path drift trap | `menu.c:1151` | CONFIRMED |
| L5 | Low | menu | New collection with an existing name silently joins it; add/remove/create write failures are silent | `menu.c:1179-1213,1295-1313` | CONFIRMED |
| L6 | Low | menu | Uppercase-only keyboard → `FAVORITES` case-aliases the hardcoded `Favorites` on FAT32 | `menu.c:1221,1330` | PLAUSIBLE |
| L7 | Low | launcher | `getUniqueName` unbounded `strcpy` chain into `char[MAX_PATH]` | `minui.c:91-104` | CONFIRMED defect / PLAUSIBLE trigger |
| L8 | Low | launcher | `Entry_open` reads `last_path[]` past its block scope (use-after-scope; `-Ofast`) | `minui.c:1080-1092` | CONFIRMED (UB) |
| L9 | Low | common | `allocFile` lacks the `ftell()<0` guard its sibling `getFile` has | `utils.c:168-181` | CONFIRMED (drift) |
| L10 | Low | common | `putInt` `sprintf` into `char[8]` — zero headroom past 7 digits | `utils.c:191-195` | CONFIRMED (latent) |
| L11 | Low | common | `msettings_core` `sprintf` with unchecked `getenv("USERDATA_PATH")` | `msettings_core.c:18` | CONFIRMED (latent) |
| L12 | Low | common | `snd.c` `realloc` without NULL-check; `SND_batchSamples` unbounded spin if ring never drains | `snd.c:84-103,134-170` | CONFIRMED / PLAUSIBLE |
| L13 | Low | common | `GFX_truncateText` unconditional `strcpy` with no `out_size` — latent (FAT `NAME_MAX` bounds it today) | `gfx.c:173` | CONFIRMED (latent; downgraded from High) |
| L14 | Low | launcher | `unlink()` retval discarded on collection delete; `getCollection` types by suffix not `DT_DIR`; dup lines not deduped | `minui.c:1433,664-690` | CONFIRMED |
| L15 | Low | build | Stale `PLATFORM=trimui` / `UNION_PLATFORM` references (non-target device) | `workspace/makefile:6,11`, `makefile.toolchain:6` | CONFIRMED |
| L16 | Low | boot | rg35xx `MinUI.pak` uses `eval $(cat NEXT)` where SP uses `. $NEXT` — divergent next-command handling | `rg35xx MinUI.pak/launch.sh` | CONFIRMED |
| L17 | Low | docs | `github/README.md` titled "MinUI screenshots", assets named `minui-*.png` | `github/README.md` | CONFIRMED (cosmetic) |

Counts: **1 Critical, 4 High, 11 Medium, 17 Low.**

---

## 2. System map

**Two binaries, one shared core.** `minui` (launcher, `workspace/all/minui/`) browses the SD card
and launches paks; `minarch` (`workspace/all/minarch/`) is the libretro frontend. Both link the
shared `workspace/all/common/` layer (`gfx`/`pad`/`pwr`/`snd`/`scaler`/`utils`/`msettings_core`)
and each platform's `PLAT_*` backend + `libmsettings` + `keymon`.

**Real execution path.** Vendor bootloader → platform `boot.sh` (baked into `dmenu.bin`) →
optional update (`unzip MinUI.zip` → `install.sh`) → `MinUI.pak/launch.sh` → loop of
`minui.elf` → (on select) writes a shell command to `/tmp/next` and exits → wrapper runs it
(an emulator pak's `launch.sh` → `minarch.elf core rom`) → back to the loop. Power-off flows
through `PWR_*`/`PLAT_powerOff`.

**Key invariants (and where they're only assumed):**
- Collection line format is `SDCARD_PATH`-relative, one path per line, single `\n`. **Held** —
  `minui.c` write and `menu.c` write agree (verified); the only drift risk is the duplicated
  `COLLECTIONS_DIR`/`COLLECTIONS_PATH` macro (L4).
- `core.tag` fits 8 bytes ("eg. GBC"): **assumed, not enforced** (H1).
- `PLAT_powerOff()` never returns: **held on SP (`exit(0)`), violated on rg35xx** (M-tension, see §4.5).
- Settings struct layout is stable across versions: **assumed, version field unused** (L3).
- Update completes atomically: **not held** (H3).

**Expectation gaps (affordance / docs / DX):**
- Expected a documented local UI-iteration path; README points at `workspace/macos`, which **doesn't exist** (M11).
- Expected `Y DELETE` hint and action to be gated identically; the action ignores depth, the hint doesn't (M4).
- Expected collections to filter by emulator availability like Recents; the filter is **commented out** (M2).
- Expected the "Screen Sharpness" and (SP) low-battery-warning options to do something; both are inert (L2, M9).
- Expected the deferred-ledger's D2 (`PLAT_setCPUSpeed`) to still be a stub; it's now **fully implemented** (§5).

---

## 3. Findings

### C1 — Deleting the last root-promoted collection crashes the launcher · CONFIRMED
`minui.c:1443` + `closeDirectory():1067-1075`. When no system folders are visible, collection
`.txt`s are promoted directly into the root listing. Delete the only one (`stack->count==1`,
root only): `Menu_deleteSelectedCollection` rebuilds root (now empty), then calls
`closeDirectory()` unconditionally at `cnt==0`. `closeDirectory` does `DirectoryArray_pop(stack)`
(count→0) then `top = stack->items[stack->count-1]` → `items[-1]`, an OOB read whose garbage
`Directory*` is dereferenced immediately (`top->selected`) and again next frame in `Menu_draw`.
**Scenario:** minimal card, one collection, no play history, no `Tools/<platform>` folder;
select it, `Y`, confirm → crash.
**Direction:** guard the `closeDirectory()` call with `stack->count>1` (the other caller,
`BTN_B` at `1584`, is already gated); otherwise leave an empty-but-valid root in place. Gating
the `BTN_Y` handler (M4) also closes this blast radius.

### H1 — SD-controlled folder tag overflows `core.tag[8]` · CONFIRMED
`minarch.c:2783` `strcpy((char*)core.tag, tag_name)` into `char tag[8]` (`minarch_internal.h:32`),
no length check. `tag_name` is the parenthesized tag from the ROM's parent folder name
(`getEmuName`) — SD-card-controlled. Any folder tag ≥8 bytes overflows the file-scope
`struct Core core` into the adjacent `core.name[128]`.
**Scenario:** a folder like `Roms/Whatever (SUPERLONGTAG)/…` → global corruption at launch.
**Direction:** `snprintf`/truncate-and-warn.
**Note (Codex):** the neighboring `strcpy(core.extensions, info.valid_extensions)` (`:2784`, 128B,
core-reported) is a **robustness** bug only — a libretro core is already trusted native code, so it
gains nothing by overflowing the frontend. `core.tag` is the real untrusted boundary.

### H2 — `getEmuName` NULL-deref on an unmatched `(` · CONFIRMED
`utils.c:93-100`: `tmp = strrchr(name,'('); if(tmp){ …; tmp = strchr(out_name,')'); tmp[0]='\0'; }`
— `tmp` is NULL when there's a `(` but no `)`. Called from `hasRoms`, `getUniqueName`, resume and
emu-exists checks on **every folder scan** (`minui.c:94,285,511,899,933,984`).
**Scenario:** a user names a system folder `Game Boy (Color` (dropped `)` — an ordinary typo in
MinUI's `<System> (tag)` convention) → launcher crashes on normal browse/resume.
**Direction:** null-check after `strchr`, fall back to the whole post-`(` substring or bail.

### H3 — The update process is not transactional · CONFIRMED (field-observed)
`rg35xx/boot.sh:74-79` + `install/install.sh:56-70`; same shape in `rg35xxplus/boot.sh:98-110`.
The `unzip` exit status gates only the `rm` of `MinUI.zip` — **`install.sh` runs regardless of
whether the extraction succeeded**. `install.sh` then `cp`s `dmenu.bin`/`ramdisk.img`/`boot_logo`
into `/misc` with no error checks and `touch`es the "installed" flag + `reboot`s unconditionally.
On rg35xx the zip is also removed *before* `install.sh` copies `/misc` and sets the flag, so a
power cut in that window leaves a new SD system paired with a stale `/misc` and no retry trigger.
**Scenario (matches the 2026-07-09 incident):** on a slow/degrading TF card, `busybox unzip` of
the 10.6 MB payload stalls or partially fails behind the static "updating" splash; the script has
no error path, so the device either finishes a half-install or reboots into a mismatched system.
**Direction:** stage extraction to a temp dir, verify (entry count / checksum), then atomically
swap and only then set the flag; on any failure keep the previous system and surface a recoverable
error. See design tension §4.1.
**Codex caveat:** "permanent hang" overstates the mechanism — a transient stall isn't permanent,
and a process wedged in uninterruptible kernel I/O would defeat a userspace timeout too. The
**non-transactional install + unconditional flag/reboot** is the CONFIRMED, serious core; whether a
timeout helps depends on the open question in §5.

### H4 — Attacker-chosen SSID reaches `wpa_supplicant.conf` unescaped · CONFIRMED (quote-breakout)
`wifi.c:190-236`. `ssid_esc` is computed (`:196-197`) and used on the open-network and
plain-psk-fallback paths — but **not** on the dominant WPA path (`:213-226`), which runs
`wpa_passphrase` and `fputs`es its raw stdout into `/tmp/wifi.conf`. `wpa_passphrase` echoes
`ssid="<raw>"` with no quote-escaping (it assumes trusted input). The SSID comes from
`wpa_cli scan_results` — i.e. any nearby AP's beacon, up to 32 arbitrary bytes. The file is then
fed to `wpa_supplicant -B` **as root**. `wifi.sh:25-27` has the same unescaped pattern at boot.
**Scenario (CONFIRMED):** an AP named `evil"` breaks the `ssid="…"` quoting → malformed conf →
connect fails for anyone who selects it (a remote DoS on the connect flow).
**Scenario (PLAUSIBLE escalation):** if a literal newline survives `wpa_cli`'s scan output into the
SSID text, the breakout extends to injecting arbitrary `wpa_supplicant` directives (extra
`network={}`, altered `ctrl_interface`). Whether that's reachable depends on the on-device
`wpa_supplicant` build (not vendored here) — see §5.
**Model divergence (recorded, not resolved):** the Claude reviewer traced `wpa_passphrase` as
*not* escaping the echoed SSID; Codex argued it may. Both agree the shell path is safe
(`Wifi_shellQuote` is used correctly everywhere) and that the escaping the author clearly intended
(`ssid_esc`) is silently dropped on the path that matters.
**Direction:** conf-escape the SSID before `wpa_passphrase`, or discard `wpa_passphrase`'s echoed
`ssid=` line and re-emit `ssid_esc`; better still, use `wpa_cli add_network`/`set_network`, which
quote structurally. See design tension §4.2.

### M1 — keymon reads the HDMI switch from the wrong sysfs path · CONFIRMED
`rg35xxplus/keymon.c:39` uses `/sys/class/extcon/hdmi/cable.0/state`; the four other consumers of
the same signal (`platform.c:380`, `libmsettings/msettings.c:43`, `hdmimon.sh:64`,
`MinUI.pak/launch.sh:56`) use `/sys/class/switch/hdmi/cable.0/state`. keymon's `watchHDMI` reads a
node that (by 4-way agreement) doesn't exist → `getInt()` returns 0 → `SetHDMI(0)` clobbers the
correct value `InitSettings` just set. If HDMI is plugged at boot, `settings->hdmi` resets within
~1 s and later hotplug is never reflected in `GetHDMI()` (while `hdmimon.sh` still does the real
fb/audio switch). Breaks HDMI-gated sleep/backlight/volume/rumble logic in `pwr.c`/`platform.c`.
**Direction:** one-character fix — match the other four callers.
**Severity note (Codex):** downgraded from High — this is functionality/availability, not memory
corruption or bricking.

### M2 — Collections launch ROMs whose emulator isn't installed · CONFIRMED
`minui.c:664-690`. `getCollection` filters only on `exists(sd_path)`; the `hasEmu()` check is
present as **commented-out dead code** (`:680-684`). Recents filters `!available` and root folders
are `hasEmu`-gated, but collections aren't. `openRom` (unlike `autoResume` at `:938`) never
verifies `exists(emu_path)` before queuing `'<emu>' '<rom>'` to `/tmp/next`.
**Scenario:** a collection referencing a game for an uninstalled emu (or copied from another build)
shows the entry and, on `A`, queues a launch at a nonexistent `launch.sh` → silent failure to a log.
**Direction:** restore the `hasEmu()` filter; add an `exists(emu_path)` guard in `openRom` as
defense-in-depth.

### M3 — `getCollection` bypasses `hide()` · CONFIRMED
`minui.c:664-690` vs `addEntries:764`. Every other listing applies `hide()`; collections don't, so
a disabled/dot-prefixed entry or a literal `map.txt` referenced by a collection still shows and
launches. **Direction:** apply `hide()` to the resolved filename before pushing the entry.

### M4 — `Y DELETE` action fires without its hint at promoted root · CONFIRMED
`minui.c:1385-1394` (hint gated `stack->count>1`) vs `:1581-1583` (`BTN_Y` handler ungated). At
root with a promoted collection selected, `Y` opens the delete-confirm modal with no on-screen cue.
**Direction:** gate the `BTN_Y` handler on `stack->count>1` (also closes C1) or extend the hint to
fire at root when the selection is a collection `.txt`.

### M5 — Pause-menu Quit row overlaps the hint pill · CONFIRMED (regresses `678c3d8`)
`menu.c:1575` computes `band_bot = DEVICE_HEIGHT/FIXED_SCALE - PADDING - BUTTON_SIZE`, using
`BUTTON_SIZE` (20) where the hint band actually reserves `PILL_SIZE` (30) — `gfx.c:563`
draws the hint pill from `dst->h - SCALE1(PADDING+PILL_SIZE)`. Working in logical units
(`PADDING=10, PILL_SIZE=30, BUTTON_SIZE=20, FIXED_SCALE=2, DEVICE_HEIGHT=480`, identical on both
devices): hint band top = 200; the centering math puts the selected Quit row's white pill at
y 177–207 → a **7-logical-px / 14-device-px overlap** at the same left x-origin. Re-derived by hand;
matches the reviewer's arithmetic.
**Scenario:** open the pause menu on either device and move the cursor to Quit — its selection pill
paints over the bottom-left `MENU/SLEEP` hint pill. Cosmetic, but it defeats the intent of the
shipped "fit the 6-item menu" fix.
**Direction:** use `PILL_SIZE` in `band_bot` (or mirror `gfx.c`'s `dst->h - SCALE1(PADDING+PILL_SIZE)`).

### M6 — `State_read` `fclose()`s an indeterminate pointer on calloc failure · CONFIRMED
`minarch.c:456-490`. `goto error` at `:459` (calloc failure) jumps past the `state_file` init to
the `error:` label, where `if (state_file) fclose(state_file)` reads uninitialized stack. The
sibling `State_write` explicitly zero-inits up front; `State_read` never got the same treatment.
**Scenario:** the exact case the adjacent comment names — a core (e.g. mgba) misreporting a huge
`serialize_size()` makes `calloc` fail → `fclose` on garbage while reporting the state-load error.
**Direction:** mirror `State_write`'s init pattern.

### M7 — Unchecked `strcpy(argv)` into path buffers · CONFIRMED
`minarch.c:2968-2969` copies `argv[1]/argv[2]` into `MAX_PATH` (512) locals with no length check;
the values propagate into `game.path`/`game.name` and every `sprintf(".../%s.sav")`-style save path
(`SRAM_getPath:334`, `RTC_getPath:390`, `State_getPath:447`). A deeply nested ROM path can exceed
512. **Direction:** `snprintf` at the argv boundary; reject/truncate with a logged error.

### M8 — `GFX_wrapText` NULL-derefs on an overlong first token · CONFIRMED
`gfx.c:226`. `prev` is only assigned in the "continue" branch; if the first word already exceeds
`max_width`, the first iteration takes the wrap branch with `prev==NULL` → `prev[0]='\n'`. Reached
via `minarch.c:1370` wrapping core-supplied option descriptions.
**Direction:** guard `if (prev)`, else hard-truncate the single overlong word before the loop.

### M9 — SP low-battery warning overlay is never drawn · CONFIRMED
`rg35xxplus/platform.c:946` `PLAT_enableOverlay` is an empty stub and `PLAT_flip` never composites
`pwr.overlay`. `pwr.c` builds the pill+icon into a software surface that nothing blits.
**Scenario:** battery ≤10%, `PLAT_enableOverlay(1)` fires, nothing changes on screen.
**Direction:** composite the overlay in `PLAT_flip`, or formally document it as inert on SDL2.

### M10 — No SRAM autosave and no signal handler · CONFIRMED (gap)
`minarch.c`. `SRAM_write`/`RTC_write` are called only from `Core_quit()` (clean exit); there is no
interval autosave and no `signal`/`sigaction` anywhere. A power cut or external SIGTERM loses every
SRAM change since the last menu-driven save/sleep.
**Direction:** add a time/event-based SRAM autosave and a handler that sets `quit=1`; or confirm the
launcher's sleep hooks are always reached before real power-off on these targets.

### M11 — README documents a nonexistent dev platform · CONFIRMED
`README.md:37` describes a `workspace/macos` dummy SDL2 platform + `workspace/macos/notes.txt` for
local UI iteration. Neither exists in the tree. A newcomer following the README to iterate locally
hits a dead end. **Direction:** remove the paragraph or restore the platform.

### Low findings (condensed)
- **L1** `skeleton/.../*.pak/default-cube.cfg` + `default-wide.cfg` (17 files) target RG CubeXX /
  RG34xx — non-target devices. `rg35xxplus/platform.c` has no `cube`/`wide`/`DEVICE` handling
  (strip `34a37e6`/`f8e7ae7` removed it), and only `DEVICE=hdmi` is ever set, so `default-$DEVICE.cfg`
  never selects these. `PAKS.md` still documents `DEVICE=cube/wide`. Dead data + stale doc left after
  the code strip. See §4.4.
- **L2** rg35xx "Screen Sharpness" is a no-op: `PLAT_setSharpness` (`platform.c:384-389`) ignores its
  arg and `PLAT_getScaler` never branches on sharpness; the `DE_SCOEF_CRISPY/ZOOMIN/…` tables
  (`:81-143`) are dead. Either wire them or hide the option like Tearing.
- **L3** `Settings.version` (`SETTINGS_VERSION 2`) is written but never validated on load (both
  platforms; the code's own `// TODO`). A same-size future layout change silently reinterprets old
  bytes. Validate `version` after read, fall back to defaults on mismatch.
- **L4** `menu.c:1151` `#define COLLECTIONS_DIR` duplicates `defines.h:27 COLLECTIONS_PATH` (the name
  `minui.c` uses everywhere). Same value today; drift trap between the two binaries' collection paths.
  Delete the local macro.
- **L5** `menu.c` `Menu_newCollection` never checks `exists()` before `fopen(…, "a")` — creating a
  name that already exists silently joins it; add/remove/create write failures are dropped with no
  `Menu_message` (unlike `Menu_saveState`'s "Save failed").
- **L6** *(PLAUSIBLE)* the on-screen keyboard is uppercase-only, so a user recreating `Favorites` can
  only type `FAVORITES`, which on FAT32/exFAT case-folds to the same file as the hardcoded
  case-sensitive `Favorites` row → two rows aliasing one file with contradictory checkmarks. Case-fold
  the `Favorites` exclusion / block case-insensitive name collisions.
- **L7** `minui.c:91-104 getUniqueName` does an unbounded `strcpy` chain into a caller `char[MAX_PATH]`;
  reachable when a collection has two lines sharing a last path segment and `getEmuName` returns a full
  (non-`ROMS_PATH`) path. Bound with `snprintf`.
- **L8** *(UB)* `minui.c:1080-1092 Entry_open` reads `last`/`last_path[]` after the nested block that
  declared them closes; "works" only because nothing reuses the slot before `saveLast`. Built `-Ofast`.
  Hoist the declarations.
- **L9** `utils.c:168-181 allocFile` lacks the `ftell()<0` clamp its sibling `getFile` has → a failed
  `ftell` becomes `SIZE_MAX` → huge OOB `fread`. Add the guard.
- **L10** `utils.c:191-195 putInt` `sprintf` into `char[8]`; `1512000\0` is exactly 8 bytes — zero
  headroom. `snprintf` + size to 16.
- **L11** `msettings_core.c:18` `sprintf(SettingsPath, "%s/msettings.bin", getenv("USERDATA_PATH"))`
  with no NULL-check — UB on a misconfigured/standalone launch. Check + `snprintf`.
- **L12** `snd.c:84 SND_resizeBuffer` doesn't NULL-check `realloc` (leaks + crashes on OOM);
  `SND_batchSamples:134-170` has no upper bound / error return if the ring never drains *(PLAUSIBLE — no
  confirmed reachable path on these two devices)*.
- **L13** `gfx.c:173 GFX_truncateText` does an unconditional `strcpy` with no `out_size`. **Downgraded
  from High:** FAT/exFAT `NAME_MAX` (255) makes a ROM basename fit `display_name[256]`, so the
  `menu.c:1543` call isn't a reachable overflow today. Latent — add an `out_size` param.
- **L14** `minui.c:1433` discards `unlink()`'s retval (silent on read-only mount); `getCollection`
  types entries by suffix without a `DT_DIR` check (a collection line to a folder mis-routes to
  `openRom`); identical collection lines aren't deduped (cosmetic double entry).
- **L15** `workspace/makefile:6,11` and `makefile.toolchain:6` reference `PLATFORM=trimui` /
  `UNION_PLATFORM` — a non-target device in error messages. Update to `rg35xx`/`rg35xxplus`.
- **L16** rg35xx `MinUI.pak/launch.sh` runs the next-command via `CMD=$(cat $NEXT); eval $CMD` while SP
  uses `. $NEXT`. Divergent; harmonize.
- **L17** `github/README.md` is titled "MinUI screenshots" with `minui-*.png` assets — branding drift.

---

## 4. Design tensions (the approach, not a line)

### 4.1 The update path is non-transactional across both platforms
`boot.sh` does `unzip → install.sh → touch flag → reboot` with no staging, no verification, no
rollback, and no error branch (H3). This is the single deepest structural issue and the root of the
field incident. **Alternative to weigh:** extract to a temp dir, verify a manifest/checksum, then do
an atomic directory swap and set the flag last; on any failure, keep the previous system and show a
recoverable message. This turns "power cut / bad card = maybe bricked, maybe looping" into "retry
next boot."

### 4.2 Untrusted-input escaping is hand-rolled per call-site
Three separate escapers exist (`Wifi_shellQuote`, `Wifi_confEscape`, `minui`'s
`escapeSingleQuotes`) and one path silently skips the right one (H4). The shell paths are actually
clean; the failure is a *config-file* escaper applied inconsistently. **Alternative:** a single
config-writer that always escapes, or drive `wpa_supplicant` through `wpa_cli add_network`/
`set_network` (structural quoting), so no code path can forget.

### 4.3 Fixed stack buffers bounded by convention, not construction
`core.tag[8]`, `display_name[256]`, `putInt`'s `char[8]`, the `getUniqueName` chain — sizes match
"what the struct comment implies." Several are safe today only because FAT `NAME_MAX` happens to be
255 (L13) or a value happens to be ≤7 digits (L10). **Alternative:** a project-wide policy —
`snprintf` everywhere and/or a single checked-copy helper in `defines.h` — retires the whole class
(H1, M7, L7, L10, L11, L13) instead of patching each site.

### 4.4 Half-stripped non-target-device residue
The SP strip removed device *code* but left device *data and docs*: 17 `cube`/`wide` cfgs, the
`PAKS.md` `DEVICE=cube/wide` text, the rotated-`-r` boot assets (reachability unconfirmed), and the
`rg40xxcube` userdata migration in `install.sh` (L1). **Decision needed:** are CubeXX / RG34xx truly
out of scope (delete cfgs + assets + migration + doc lines) or supported (the code strip removed
needed handling)? The half-state is the debt.

### 4.5 Shutdown/teardown contract is divergent and unenforced
SP's `PLAT_powerOff` `exit(0)`s; rg35xx's returns after `system("shutdown")`. The shared `*_quit`
functions aren't idempotent (`GFX_quit` unguarded; `PWR/VIB/SND_quit` check `initialized` but never
reset it). If control returns to the main loop on rg35xx before the machine dies, a second teardown
double-frees GFX assets / re-joins joined threads *(PLAUSIBLE per Codex — needs the concrete
return-into-loop path confirmed)*. **Alternative:** document and enforce "`PLAT_powerOff` never
returns" (have `PWR_powerOff` `exit()` after calling it) **and** make the `*_quit` functions
idempotent (reset `initialized`, guard `GFX_quit`). Cheap defense regardless of reachability.

---

## 5. Open questions (need maintainer / on-device answers)

1. **Wi-Fi (H4 escalation):** does the on-device `wpa_supplicant`/`wpa_cli` scan output let a raw
   newline survive inside an SSID? If yes, H4 is Critical (directive injection as root); if no, it's a
   quote-breakout DoS. Not answerable from this repo (wpa is in the vendor rootfs).
2. **Update hang (H3):** when it hung, was `busybox unzip` in uninterruptible kernel I/O (D-state) or a
   userspace stall? Decides whether a watchdog/timeout helps at all or only the transactional rewrite does.
3. **Non-target devices (L1 / §4.4):** are RG CubeXX (`cube`) and RG34xx (`wide`) actually out of scope?
   If so the cfgs/assets/migration/PAKS lines can be deleted; the rotated `-r` boot assets need an
   on-device `cat /sys/class/graphics/fb0/modes` check before removal (they may be needed pre-SDL2).
4. **`init.elf` (SP):** it links SDL 1.2 (`-lSDL`) but `makefile.copy` ships only `libSDL2`. Does the
   vendor rootfs provide `libSDL-1.2.so.0`? `ldd init.elf` on device. If not, the boot chain's
   `init.elf` step is silently failing.
5. **SP battery overlay (M9):** is it meant to work on SDL2, or should it be formally removed?

---

## 6. Prior-audit ledger reconciliation (2026-07-08 deferred findings)

- **D1 — vsync "Prevent Tearing" no-op → hidden (`48446a8`):** **HOLDS, thorough.**
  `PLAT_supportsVsyncToggle()` returns 0; `FE_OPT_TEARING.lock` is set *after* config load so a stale
  saved key can't un-hide it; `.lock` excludes the item from the built menu (not just grays it). The
  `PLAT_setVsync` stub is coherent — real gating is the `gfx.c` `should_vsync` param.
- **D2 — `PLAT_setCPUSpeed` stub → OPEN:** **RESOLVED. Ledger is stale.** `rg35xxplus/platform.c:1013-1032`
  is now a full implementation (maps `CPU_SPEED_*` to 720/1104/1320/1512 MHz, pins min==max, lowers the
  floor before raising to avoid a transient min>max). Also answers `dd82135`'s 720 MHz menu clock.
- **D3 — Apply Panel Fix DTB write safety:** **Cannot audit — the pak is not in the tree.**
  `skeleton/EXTRAS/Tools/rg35xxplus/Apply Panel Fix.pak/` referenced by the deferred doc does not exist
  in git or on disk. Either it was removed or never committed; the deferred doc references a phantom path.
- **D4 — thread_video lost-wakeup → predicate added (`1f4f28c`):** **Predicate HOLDS; a distinct
  quit-path stall remains (PLAUSIBLE).** `frame_ready` is set/cleared under `core_mx` correctly. But the
  frontend advertises `GET_CAN_DUPE=true`; `quit` is set on the core thread (`SHORTCUT_SAVE_QUIT`), and if
  that tick's `video_refresh` is a NULL-dupe (or otherwise doesn't drive `frame_ready`), the main thread's
  `pthread_cond_wait` can block on the quit path. **Codex correction:** `GET_CAN_DUPE` permits
  `video_refresh(NULL)`, not *omission* of the callback — so a strictly-conforming core is unlikely to
  trigger this; treat as defensive robustness. **Direction:** broadcast `core_rq` when `quit` is set, and
  include `quit` in the wait predicate (already partially done).

Also verified holding: `60ba2a5` (per-frame cond reinit removed) HOLDS; `dd44af6` (letterbox-bar clear
on overlay dismiss) HOLDS; `9454083` (Wi-Fi gated to rg35xxplus) HOLDS (build + copy both gated);
`1cefe95` (boot logo refreshed every install) HOLDS.

---

## 7. Considered & rejected (disproved or by-design)

- **`GFX_truncateText` as a High stack overflow** — FAT/exFAT `NAME_MAX`=255 fits `display_name[256]`;
  the 512-byte *source buffer* doesn't imply a 512-byte runtime string (Codex). Kept as L13 (latent).
- **`core.extensions` overflow as a security finding** — a libretro core is trusted native code; overflowing
  the frontend gains it nothing (Codex). Robustness only; folded into H1's note.
- **Shell injection via ROM / collection / SSID names** — `minui` routes launches through
  `escapeSingleQuotes`; `wifi` uses `Wifi_shellQuote` on every `popen`/`system`; `menu.c` has no
  `system`/`popen` at all and its keyboard charset excludes `/`, `.`, and metacharacters. All traced clean.
- **`PLAT_setVsync` / `PLAT_setCPUSpeed` / `PLAT_setNearestNeighbor` / `PLAT_setVideoScaleClip` as lying
  stubs** — each is either an honestly-signaled unsupported feature (vsync via `supportsVsyncToggle` + the
  `gfx.c` param), now implemented (CPU speed), or genuinely dead/uncalled shared API.
- **`is_cubexx`/`is_rg34xx` remnants in `rg35xxplus/platform.c`** — full read confirms the strip is complete
  in that file (a stale memory record claimed otherwise; corrected). The residue is data/docs only (L1).
- **Multi-disc game added to a collection via a specific disc file** — `minarch.c:275-301` recomputes
  `game.m3u_path` from the directory convention at every launch, so multi-disc support survives.
- **`Menu_options` pager off-by-one, 6-item menu scroll off-by-one, double-fast-toggle race,
  zero-collections / missing-dir open, `Menu_confirm` NULL `PWR_update`, `trimSortingMeta` `*str[0]`
  precedence, hat-motion `just_released`, `overclock` freq/voltage table math, `OptionList_reset`
  double-free, `msettings` shm torn/short read, `SetJack` cross-thread race** — each investigated and
  disproved by the area reviewers (traced guards, single-writer, or correct-by-precedence).
- **TOCTOU / symlink on predictable `/tmp` files** (`/tmp/wifi.conf`, `/tmp/next`, `/tmp/data`) and
  **update-zip path traversal** (Codex-suggested classes) — real classes, but low-risk on a single-user
  handheld with no untrusted local users and an official signed payload; noted here rather than raised as
  findings. Worth a pass if multi-user or network-delivered updates are ever added.

---

*Reviewers: 7 parallel full-read area agents (common, launcher, minarch, in-game menu, rg35xx platform,
rg35xxplus platform, wifi) + coordinator direct read of build/docs/scripts. Cross-model: OpenAI Codex
(`codex-cli 0.144.0`) via `ask`. team-knowledge: reachable, no matching notes. Report uncommitted —
the maintainer owns git.*

---

## 8. Remediation (2026-07-09, uncommitted)

Fixes applied in the working tree (21 files) via 5 parallel implementer agents + coordinator direct edits, then reviewed by Codex (gpt-5.6-sol), which caught 4 real defects in the first-pass fixes (all corrected) + 1 improvement + 1 acknowledged limitation.

**Fixed & self/cross-verified:** C1, H1, H2, H4, M1, M2, M3, M4, M5, M6, M7, M8, M11, L3, L4, L5, L6, L7, L8, L9, L10, L11, L12, L13(caller-side), L14(a), L15, L17, D4, plus §4.5 (rg35xx power-off hang-guard + `*_quit` idempotency from the common batch).

**Codex-review corrections (second pass):**
- rg35xx `install.sh` copy-failure branch now `exit 1` (was returning 0 → boot.sh `&& rm` would have dropped the zip despite a failed install).
- SP `boot.sh` now gates `rm` on `install.sh` success; SP `install.sh` returns non-zero on a failed `dmenu.bin` copy.
- `wifi.c` now strips CR/LF from the SSID **before** `wpa_passphrase` (the first-pass fix only dropped the first echoed `ssid=` line; a literal LF split it across lines and leaked the continuation).
- `snd.c` restores `frame_count` on a failed realloc (first-pass fix left it describing the larger, now-unallocated buffer).
- **M10 reverted** — a naive SIGTERM/SIGINT handler set a non-`volatile sig_atomic_t` `quit` and could not wake a thread blocked on `core_rq`, trading a clean kill for a possible hang. M10 stays a documented gap (see §3 M10); periodic SRAM autosave + a correct handler is the real fix, deferred.

**Held (need a decision or on-device validation, NOT auto-fixed):**
- **H3 full rewrite** — the fail-closed gating landed (install.sh no longer runs after a bad unzip; zip retained for retry), but the update is still not transactional (unzip writes into the live tree; no staging/atomic-promotion/rollback). The full rewrite (§4.1) needs on-device testing. **Boot-script edits are inert until `make` rebuilds `dmenu.bin`, and MUST be validated on real hardware before flashing.**
- **L1** — 17 dead `default-cube.cfg`/`default-wide.cfg` + the PAKS.md `DEVICE=cube/wide` lines: deletion held pending explicit go-ahead (removing tracked files / editing an upstream-voiced doc).
- **L2** — rg35xx "Screen Sharpness" no-op: wire the dead `DE_SCOEF_*` coefficients vs. hide the option — a design decision needing on-device confirmation of which levels differ.
- **M9** — SP battery-warning overlay: implement (wire `PLAT_flip`, device-test territory) vs. formally document inert — needs a call.
- **L16** — rg35xx `eval $(cat /tmp/next)` vs SP `. /tmp/next`: benign divergence; harmonizing risks the core launch path with no device to test.

**Verification caveats:** shell scripts pass `sh -n`; the C changes are verified by careful review + Codex cross-model review, NOT by compilation (the cross-toolchains live in Docker). Run `make` in the toolchain before relying on the binaries.
