# Codebase Audit — Deferred Findings (2026-07-08)

From the whole-repo adversarial audit of 2026-07-08. The CONFIRMED findings with safe fixes
landed in commits `60ba2a5`, `b8100f3`, `6fe32e8`, `134fe5c`. The four below were CONFIRMED but
deferred because the fix or its verification needs **on-device testing** on real RG35xx /
RG35xxSP hardware. (GitHub Issues are disabled on this repo, so they're tracked here.)

**Update 2026-07-08:** D1, D3, D4 have since been implemented (compile-gated on both toolchains,
still pending on-device validation). D2 remains open — it needs the SP's real cpufreq interface.

| ID | Severity | Area | Issue | Status |
|----|----------|------|-------|--------|
| D1 | Medium | rg35xxplus video | 'Prevent Tearing' (vsync) option is a no-op — `PLAT_flip` ignores the param | Fixed `48446a8` (hidden) — verify on device |
| D2 | Medium | rg35xxplus power | `PLAT_setCPUSpeed` is an empty stub — RG35xxSP never downclocks | **OPEN** — needs SP cpufreq paths |
| D3 | High (brick) | panel-fix tool | DTB write lacks post-write readback + uses `bs=1` | Fixed `faa109b` (readback + restore.sh) — verify on device |
| D4 | Medium | minarch thread_video | condvar wait has no predicate (lost-wakeup stall) | Fixed `1f4f28c` (predicate) — verify with thread_video on |

---

## D1 — rg35xxplus vsync option is a no-op

**Severity:** Medium · **Device:** RG35xxSP (rg35xxplus build)

## Problem
The "Prevent Tearing" (vsync) menu option does nothing on the rg35xxplus binary. `GFX_flip`/`GFX_sync` compute a `should_vsync` value from the user's setting and pass it to `PLAT_flip`, but the rg35xxplus implementation names the parameter `ignored` and never reads it — the only vsync behavior comes from `SDL_RENDERER_PRESENTVSYNC` hardcoded at renderer creation, which nothing can toggle at runtime. `PLAT_setVsync` is an empty stub (`// buh`).

- `workspace/rg35xxplus/platform/platform.c:482` — renderer created with `SDL_RENDERER_PRESENTVSYNC` (hardcoded)
- `workspace/rg35xxplus/platform/platform.c:572-574` — `PLAT_setVsync` empty stub
- `workspace/rg35xxplus/platform/platform.c:779` — `PLAT_flip(SDL_Surface* IGNORED, int ignored)`, param never used

Contrast: rg35xx (SDL1.2) genuinely gates an `OWLFB_WAITFORVSYNC` ioctl on its `sync` param (`workspace/rg35xx/platform/platform.c:434-438`).

## Failure scenario
User sets "Prevent Tearing: Off" on an RG35xxSP for lower input latency (or during fast-forward). Every `SDL_RenderPresent` still blocks for a display refresh — the setting has zero effect, up to one refresh of added latency regardless of choice.

## Direction (needs device testing)
Either honor the param — recreate the renderer with/without `PRESENTVSYNC` on toggle (SDL2 has no live per-present swap-interval override for the non-GL renderer), or `SDL_GL_SetSwapInterval` if a GL backend is in use — **or** hide the option on rg35xxplus so the menu doesn't promise a capability the platform can't deliver. Touches the renderer lifecycle, so validate on hardware across LCD + HDMI.

---

## D2 — PLAT_setCPUSpeed stub on rg35xxplus

**Severity:** Medium · **Device:** RG35xxSP (rg35xxplus build)

## Problem
`PLAT_setCPUSpeed` is an empty stub on rg35xxplus:
- `workspace/rg35xxplus/platform/platform.c:957-959` — `void PLAT_setCPUSpeed(int speed) { // TODO: why wasn't this ever implemented? }`

rg35xx implements real frequency scaling via `overclock.elf` (`workspace/rg35xx/platform/platform.c:567-579`). Shared launcher/emulator code calls `PWR_setCPUSpeed(CPU_SPEED_MENU)` (e.g. `minui.c` menu idle) believing it downclocks for battery/thermal — but on RG35xxSP it silently does nothing.

## Failure scenario
RG35xxSP sits in the launcher menu; the frontend thinks it has dropped to `CPU_SPEED_MENU` but the SoC stays at full clock — worse idle battery drain and heat than intended, with no signal that the call was a no-op.

## Direction (needs device testing)
Implement CPU frequency scaling for the rg35xxplus SoC (likely a sysfs `scaling_governor`/`scaling_setspeed` write or an equivalent to rg35xx's `overclock.elf`), mapping the `CPU_SPEED_*` levels to real frequencies. Requires the device to identify the correct cpufreq interface and validate stability at each level.

---

## D3 — Apply Panel Fix DTB write safety

**Severity:** High (brick risk) · **Device:** RG35xxSP (rg35xxplus build)

## Problem
The Apply Panel Fix pak writes a modified DTB to the raw eMMC boot region. Commit `134fe5c` added a best-effort backup of the original region to SD before the write, but two gaps remain, both needing hardware to validate safely:

- `skeleton/EXTRAS/Tools/rg35xxplus/Apply Panel Fix.pak/apply.sh:208` — `dd ... bs=1 ... conv=notrunc` writes byte-at-a-time (one syscall/byte over a multi-KB DTB), maximizing the power-loss window that could leave a half-written, unbootable DTB.
- No **post-write readback**: the script checksums the two temp files *before* writing but never re-reads `$DEV_PATH` after the `dd` to confirm the on-device bytes match what was intended.

## Failure scenario
Power/battery cut mid-`dd` (the normal shutdown path for this class of handheld) leaves the boot partition holding a mix of old and new bytes whose `totalsize`/structure offsets disagree → potentially unbootable, with only the (now-added) SD backup and no restore tool.

## Direction (needs device testing)
1. Read back the written region and re-checksum before declaring success (safe to add; false-positive only shows a scary message, doesn't brick).
2. Use a larger `bs` to shrink the vulnerable window — **changes write behavior, must be tested on device.**
3. Ship a `restore.sh` that flashes the SD backup this pak now writes.

---

## D4 — thread_video lost-wakeup

**Severity:** Medium · **Path:** thread_video mode (off by default)

## Problem
Commit `60ba2a5` deleted the per-frame `pthread_cond` reinitialization (POSIX UB). The remaining, deeper issue is a classic lost-wakeup: the main thread waits on the condvar **without a predicate**, so if the core thread signals before the main thread reaches `pthread_cond_wait`, the wake is lost and the UI stalls until the next video callback.

- `workspace/all/minarch/minarch.c` — producer signal in `video_refresh_callback` (~lines 2648-2664); consumer `pthread_cond_wait` in the main loop's thread_video branch (~2994-2995), no `while(!ready)` predicate guarding it.

Currently inert in practice (the wait happens under `core_mx` with no thread blocked at that instant), but fragile.

## Failure scenario
With "Prioritize Audio" (`thread_video`) enabled, a timing race where the core thread signals frame-ready microseconds before the main thread blocks → the main thread misses the signal and stalls one frame; repeated under load it manifests as intermittent hitching in the exact mode meant to smooth playback.

## Direction (needs device testing)
Add a frame-ready flag set under `core_mx` and loop `while (!ready) pthread_cond_wait(...)` on the consumer; clear it after consuming. Only exercised when thread_video is on, so validate on-device with the option enabled.
