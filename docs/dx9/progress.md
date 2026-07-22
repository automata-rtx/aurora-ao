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
