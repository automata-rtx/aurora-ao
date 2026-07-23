# D3D9 backend — progress log & resume notes

> **Rule:** update this file at every conversation checkpoint (before context
> compaction) so a fresh context can resume from the repo alone. Newest entry
> first. Keep "Next steps" honest and specific.

## How to resume with zero context

1. Read `docs/dx9/README.md`, then `architecture-notes.md`, then
   `gx-to-d3d9-mapping.md` (this is the spec being implemented).
2. `git log --oneline` on branch `Fixed-Function` (dev branch
   `claude/dusklight-dx9-fixed-function-6uoy92`) in both repos:
   `automata-rtx/aurora-ao` and `automata-rtx/dusklight-ao` (aurora is the
   `extern/aurora` submodule of dusklight).
3. Check "Next steps" of the newest entry below; the code lives in
   `aurora-ao/lib/dx9/`.
4. Build target is Windows (MinGW or MSVC). This work is developed in a Linux
   container: compile-verify D3D9 code with
   `x86_64-w64-mingw32-g++ -fsyntax-only` where possible; full builds/testing
   happen on the owner's Windows machine.

---

## Checkpoint 3 — IN-GAME AND RENDERING (2026-07-22)

**Fourth run reached gameplay.** Screenshot review (Faron-area): skinned
characters (Link) near-perfect incl. textures — fixed-function indexed
vertex blending confirmed working; HUD hearts/buttons/rupees/items perfect;
grass-cut particles, pickups all correct. Owner confirms unlit look is
exactly what Remix needs — keep it.

**Issues found & fixed this round:**
1. **moya (drifting cloud-shadow projection) corrupted textures + wrong
   ground darkening.** Two-part fix: (a) root cause — camera-space texgen
   used D3D view-space inputs where GX texgen reads *model-space* inputs;
   now compensated by premultiplying the texture matrix with the per-draw
   model-view inverse (`g_worldViewInv`, rigid draws only — see
   `dx9_draw.cpp apply_transforms` / `dx9_tev.cpp apply_texgen`); (b) per
   owner requirement, moya is **disabled outright in D3D9 mode**
   (`dKankyo_cloud_Packet::draw` early-out, dusklight) — Remix path-traced
   shadows replace it, matching the owner's existing sun-shadows mod policy.
2. **Minimap black square** — EFB copies were 1x1 placeholders. Implemented
   REAL color EFB copies (`StretchRect` from the current render target into
   per-dest RT textures) and REAL offscreen passes
   (`SetRenderTarget`/depth surface cache by size, draws no longer
   discarded). Placeholder (still used for depth-format copies) changed from
   opaque black to white/alpha-0 — opaque black was darkening every
   projected consumer.
3. Viewport/scissor/copy sizing now tracks the current target (offscreen vs
   backbuffer) via `get_backbuffer_size`.

**Verify next run:** minimap renders; no white blotches on canopy; ground
darkening near player gone; character env-mapped materials (metal shine)
improved by the texgen fix. Skinned draws with camera-space texgen log
`texgen: camera-space source without invertible world` — expected, cosmetic.

## Checkpoint 2.2 — third run: d3d9 active; UI document crash fixed (2026-07-22)

**Progress:** `dx9: device created` confirms the backend now initializes on
the desktop (RTX 5090 machine). Crash moved past aurora into dusklight init:
`EXCEPTION_ACCESS_VIOLATION` fault addr 0x90 in dusklight.exe right after
"Texture replacement directory loaded".

**Root cause:** `game_main` creates RmlUi documents unconditionally.
`dusk::ui::initialize()` correctly returns false without RmlUi, but the
return value was ignored and `push_document(make_unique<Overlay>(), …)` ran
anyway — `Overlay::Overlay()` does `mDocument->GetElementById("fps")` with a
null `mDocument` (base `Document` is null-safe; subclass ctors are not).

**Fix (dusklight `m_Do_main.cpp`):** gate all startup document creation on
`uiAvailable = dusk::ui::initialize()`: Overlay/TouchControls/MenuBar, the
prelaunch picker (falls through to config/CLI DVD path with a clear warning +
existing "No DVD image specified" fatal), CrashReportWindow, and PresetWindow
(which would otherwise fire on any fresh config). Mods UI svc already guards
via `rmlui::is_initialized()`; `update_midna_icon_texture` is CPU-only; the
settings-internal Modal is unreachable without a root document.

**Launch requirement in d3d9 mode:** no prelaunch picker → the game path must
come from `backend.isoPath` or `--dvd <path>`. Recommended:
`dusklight --backend d3d9 --dvd <game.rvz>`.

## Checkpoint 2.1 — second run report: config never selected d3d9 (2026-07-22)

**Symptom:** "still crashes on launch" — but the log shows this run never used
the D3D9 backend at all: `config.json` failed to parse ("parse error at line
80 … unexpected end of input", likely a hand-edit that dropped a brace), so
`backend.graphicsBackend` reverted to defaults → auto → **WebGPU/D3D12**
initialized (RTX 5090). No disc path was configured either, so the game sat
in the no-disc prelaunch flow and died on a **pre-existing wgpu-path fatal**:
`FATAL aurora::gfx::gx unmapped vtx attr 13` — WGSL shader gen aborts when a
texgen references TEX0 coords the vertex stream doesn't provide (stale
numTexGens/TCG state; real hardware would just read garbage).

**Fix:** `shader.cpp vtx_attr` now substitutes `vec2f(0.0)` for missing
TEX0-7 attributes (warn-once per shader config), mirroring the existing
NRM/CLR fallbacks. The D3D9 path already handled this case.

**User-side requirement discovered:** ensure `config.json` is valid JSON (or
delete it and reconfigure), or bypass config entirely with `--backend d3d9`
on the command line. A `dx9: device created …` line in the log is the
signature that the D3D9 backend is actually active.

## Checkpoint 2 — first Windows run: startup crash fixed (2026-07-22)

**Symptom:** with `--backend d3d9` the window appeared then the process died,
`EXCEPTION_ACCESS_VIOLATION` at fault address 0x0 **inside webgpu_dawn.dll**,
immediately after the "Using framebuffer size" log line. The D3D9 device
itself initialized fine (`dx9: device created 1216x896 (maxBlendMtxIdx=8,
perStageConstants=true)` — also confirms per-stage TSS constants and blend
palette caps on real hardware).

**Root cause(s):** code paths that call Dawn unconditionally even when the
D3D9 backend owns rendering (Dawn objects are null → null `this` calls into
webgpu_dawn.dll):

1. **The actual crash:** Dusklight's `imGuiInitCallback`
   (`aurora_imgui_init_callback` → `ImGuiEngine_AddTextures` →
   `aurora_imgui_add_texture`) runs right after the framebuffer-size log and
   creates ImGui icon textures. `imgui::add_texture` fell through to
   `webgpu::g_device.CreateTexture`. Fixed: returns a null `ImTextureID` when
   `dx9::active()` (checked via dx9, not the headless flag, because the
   callback runs *before* `imgui::initialize()`).
2. **The next crash in line:** `window.cpp resize_swapchain()` calls
   `webgpu::resize_swapchain` on `SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED` /
   `DISPLAY_SCALE_CHANGED` / `FutureResize` — guaranteed to fire at startup on
   a 150% display-scale system. Fixed with a `dx9::active()` guard (the D3D9
   backend detects resizes itself in `dx9::begin_frame` via backbuffer-size
   compare + `Reset`). Same guard added to the `RefreshSurface` custom event
   (vsync changes).

**Audit performed** (all other Dawn-touching paths reachable in d3d9 mode):
`webgpu::set_resampler` (enum store — safe), `vi::configured_fb_size` (VI
state — safe), `AuroraGetRenderSize` (window size — safe), debug groups
(vector ops; marker push drops with warn — safe), `depth_peek::read_latest`
(returns false on empty snapshot — safe; note GXPeekZ yields z=0 in d3d9
mode, so peek-based visibility checks like sun flares misbehave — cosmetic),
mods `gfx_get_device_info` (inline null handle, no Dawn call — mod's
problem), `gx::initialize` (pure wgpu, correctly skipped; `fifo::init` runs
from GXInit independently). rmlui paths unreachable (never initialized).

**Expectation for next run:** boots past init; next likely issue class is in
the first real draws (vertex decode / TEV apply / first textures). Get a new
log + crash trace if it dies; if it renders, screenshot the first thing
visible (Nintendo logo / title) and grep the log for `dx9` warnings.

## Checkpoint 1 — research + branches + docs + scaffold (2026-07-22)

**Done:**
- Full architecture research of aurora-ao & dusklight-ao (see
  `architecture-notes.md` — FIFO/command-processor chokepoint, `g_gxState`,
  skinning extension, texture pipeline, frame lifecycle, Dusklight backend
  selection & UI/terrain/billboard GX patterns).
- Branches created & pushed on both repos: `Fixed-Function` +
  `claude/dusklight-dx9-fixed-function-6uoy92`, based on
  `claude/gpu-skinning-72pstj` (GPU skinning is a Remix prerequisite).
- This doc set written (README / architecture-notes / gx-to-d3d9-mapping /
  unsupported-effects / progress).
- Aurora scaffold: `BACKEND_D3D9` enum value, CMake `AURORA_ENABLE_D3D9`
  option + `lib/dx9/` module, init/frame/shutdown branching in `aurora.cpp`,
  interception hooks in `command_processor.cpp` & `gfx` glue, and the initial
  implementation of the D3D9 executor (device/frame/state/vertex-decode/
  texture/TEV modules) per the mapping spec.

**Verification state:** all five `lib/dx9/*.cpp` translation units pass
`x86_64-w64-mingw32-g++ -std=c++20 -fsyntax-only` against real MinGW
d3d9/windows headers, real repo headers, and real absl/fmt/xxhash (Dawn/SDL3
shimmed since binaries can't be fetched in the dev container). The main hook
files (`command_processor.cpp`, `GXFrameBuffer.cpp`) pass the same check in
both D3D9-on and D3D9-off configurations; `gx.cpp`/`common.cpp`/`aurora.cpp`
edits show no errors in edited regions (remaining shim-gap noise is in
untouched wgpu code). NOT yet linked or run — first Windows build may still
surface minor fixes; runtime bring-up is entirely pending.

**Next steps (in order):**
1. Compile on Windows (`cmake -DAURORA_ENABLE_D3D9=ON`), fix fallout.
2. Bring-up milestones: (a) clear color visible, (b) UI/menu quads (J2D) —
   first geometry, (c) 3D world geometry with diffuse textures, (d) alpha-test
   foliage, (e) PNMTXIDX palette draws (Link), (f) `GXSetSkinning` draws,
   (g) terrain lerp materials.
3. Dusklight side: `d3d9` in backend parse/name/id tables + settings menu +
   docs; disable wgpu mods & RmlUi when d3d9 active; submodule bump.
4. First Remix smoke test (drop runtime d3d9.dll, check capture).
5. Fold TEV-mapper warnings from a play session into `unsupported-effects.md`.

**Open questions:**
- RmlUi settings menu availability in d3d9 mode (see unsupported watch list).
- Whether `D3DUSAGE_SOFTWAREPROCESSING` fallback is needed for >8 blend
  matrix index (decide from caps on real hardware/Remix).
