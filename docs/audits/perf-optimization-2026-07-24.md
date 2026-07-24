# darkUI Performance Optimization Pass — 2026-07-24

Performance-mode pass over the emulation hot paths, cross-checked against a second model
family (OpenAI Codex, frontier `gpt-5.6-sol`) and a parallel Claude hot-path map. The goal
was max sustained emulation FPS / lowest frame latency / lowest battery, with **correctness
proven before anything landed**.

**Scope:** `workspace/all/minarch/minarch.c` (frame loop), `workspace/all/common/{scaler,gfx,snd}.c`,
`workspace/rg35xx/platform/platform.c`, `workspace/rg35xxplus/platform/platform.c`,
`workspace/all/minui/minui.c`, the per-system `skeleton/.../Emus/*.pak` core configs, and the
makefiles. `workspace/rg35xxplus/other/uMTP-Responder` (vendored USB) was out of scope.

Two target devices, and the split matters for every decision below:
- **RG35xx** — Actions ATM7039S, quad Cortex-A9, ARMv7, **no hardware integer divide**,
  hand-written NEON scalers, `/dev/fb0`+ION double buffer presented via the SoC Display
  Engine overlay (true HW page-flip), `OWLFB_WAITFORVSYNC` ioctl. **Weakest silicon — the
  binding constraint.**
- **RG35xxSP** (`rg35xxplus`) — Allwinner H700, quad Cortex-A53, SDL2 GLES renderer,
  GPU-side scaling (`PLAT_getScaler` returns `scale1x1`), `PRESENTVSYNC`.

**Method note:** the headline result is that the frontend was already near-optimal (mature
MinUI/NextUI lineage). The most valuable output of this pass is the **Considered & Rejected**
ledger in §3 — every candidate was traced to source before acting, and the Codex pass's three
top findings were all disproven on verification. Don't re-litigate them.

---

## 1. Changes landed (impact order)

| ID | Impact | Area | Change | Location | Commit |
|----|--------|------|--------|----------|--------|
| P1 | Med (A9, effect on) | scaler | Scanline/grid shading: per-pixel per-channel integer divide → precomputed per-channel LUT. A9 has no HW divide, so this was ~3 emulated divides/pixel/frame with an effect enabled. Bit-identical. | `scaler.c:627-643` | `9be2557` |
| P2 | Low | build | `-flto` on minui, matching minarch. On rg35xx the launcher was getting no LTO at all (`LIBS` carries none there). | `minui/makefile:28` | `9be2557` |
| P3 | Med (PS1 on A9) | pak/clock | PS1 forced to Performance clock on rg35xx (1296→1488 MHz, ~+15%), mirroring the SP's existing `PS.pak`. | `rg35xx/.../PS.pak/default.cfg:1` | `5620936` |

## 2. Detail

**P1 — effect-scaler LUT.** The `Weight2_3`/`Weight3_1`/`Weight3_2` macros darkened each pixel
with `channel*n/d` per colour channel. Every call site passes the second operand as black
(`k=0x0000`), so each weight is a pure function of one RGB565 pixel — precomputable into three
64-entry tables (`weight_2_5`/`weight_3_5`/`weight_3_4`, `v*n/d` for `v` in 0..63). Compile-time
`static const` arrays (no runtime init: a first draft used `__attribute__((constructor))`, but
Codex flagged that a build path not honouring constructors would leave the tables zero → all
effect pixels black; the literal-array form removes that risk entirely). Only the A9 runs these
(the SP does effects on the GPU), which is exactly the chip that lacked a HW divider.
Bit-identity verified exhaustively on the host: all 65536 RGB565 inputs × 3 weights, **0
mismatches** vs the original macros.

**P3 — PS1 clock on rg35xx.** PS1 is the heaviest system these consoles emulate and the A9 is
the weakest chip, yet its `PS.pak` left the clock at Normal (1296 MHz) while the SP's `PS.pak`
already forces Performance for the identical core (`pcsx_rearmed`, `ari64` dynarec + NEON GPU).
Claiming the 1488 MHz ceiling the A9 already supports (`overclock.elf`, `platform.c:598-603`) is
the one place a real clock gap existed. Costs some battery during PS1 sessions — the same
tradeoff the SP already makes for the same system, applied per-system, not globally.

## 3. Considered & Rejected

| Candidate | Source | Verdict | Why |
|-----------|--------|---------|-----|
| Disable threaded video by default | Codex (top rec) | **Rejected** | `thread_video` is already `0` by default (`minarch.c:30`); it's the opt-in "Prioritize Audio" setting. The per-frame `memcpy` under `core_mx` (`minarch.c:2722`) only exists when a user enables it. No default-path cost. |
| Frame pacing busy-wait / idle CPU burn | Codex | **Rejected** | There is no busy-wait. A9 blocks on `OWLFB_WAITFORVSYNC` ioctl; SP blocks on `PRESENTVSYNC`. `PLAT_vsync`'s `SDL_Delay` is a secondary throttle only, not the primary sync. |
| Remove the audio "retry stall" | Codex / map | **Rejected** | `SND_batchSamples`' 10×`SDL_Delay(1)` (`snd.c:158-163`) is deliberate backpressure — it fires only when the ring buffer is full, i.e. the core is running faster than realtime. Removing it desyncs audio. |
| Swap per-system default cores | audit | **Already optimal** | Every system already maps to the fastest built core: `gpsp` (GBA), `pcsx_rearmed`+`ari64`+NEON GPU (PS1), `snes9x2005_plus` (SNES), `picodrive` (MD/GG/SMS), `fceumm`/`gambatte` (NES/GB/GBC). The heavier cores (`snes9x2010`, `genesis_plus_gx`, `beetle_psx`) aren't even compiled (`rg35xx/cores/makefile:1-2`). Nothing can silently fall back to a slow core. |
| CPU governor / overclock | audit | **Already done** | `PLAT_setCPUSpeed` pins the `performance` governor + max clock on both platforms (SP `platform.c:1013-1031`; A9 via `overclock.elf`). |
| SP zero-copy streaming texture (`SDL_LockTexture`) | open lever | **Rejected (would regress)** | Cross-model: SDL2's GLES2 `SDL_UpdateTexture` already passes packed RGB565 straight to `glTexSubImage2D` with no intermediate SDL memcpy, whereas `LockTexture` returns SDL-allocated staging memory and `UnlockTexture` uploads on unlock — so "zero-copy" would *add* a copy. The current `platform.c:785` path is already the lower-copy one. ~600KB/frame × 60 ≈ 37 MB/s is negligible vs the H700's GB/s memory bandwidth; core CPU emulation dominates frame time. |
| A9 build flags (`-mtune=cortex-a9 -march=armv7-a`) look mis-tuned | self | **Rejected (correct)** | Cortex-A9 / ARMv7 is the *correct* silicon for the original rg35xx (ATM7039S). Not the H700. The `rg35xxplus` build correctly targets `cortex-a53`. |

## 4. Verification status

- **P1 correctness:** proven on host — 65536 inputs × 3 weights, 0 mismatches vs the original
  macros. Bit-identical, so no on-device visual regression possible.
- **On-device build + FPS measurement: PENDING.** The build is Docker-via-Colima now (Docker
  Desktop retired): `colima start --vm-type vz --vz-rosetta`, then
  `make PLATFORM=rg35xx build` / `make PLATFORM=rg35xxplus build` (rg35xx first — it owns the
  shared cores). No before/after FPS was captured this pass; the wins are proven-correct and
  reasoned, not yet measured on hardware. Deploy to both consoles follows the build.

## 5. Bottom line

The frontend is near-optimal and two independent model families agree there is no remaining
frontend lever worth the risk. The three landed changes are the real, safe wins that existed;
everything else was either already correct or would regress. Further FPS gains, if wanted, live
in the libretro cores themselves (per-title dynarec/frameskip tuning) — not in this frontend.
