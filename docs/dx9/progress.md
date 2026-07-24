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
5. Read `CLAUDE.md` at the repo root — notably: the owner's interactive
   approval prompts are broken in ALL their Claude Code sessions, so never
   use tools that require an approval prompt (work around them instead).

---

## Checkpoint 3.5 — Remix VRAM leak: D3D9 texture objects must be stable across frames (2026-07-24)

**Owner test of 3.4 build (major progress):** rigged meshes AND terrain look
correct under Remix; input fixed by the `rtx.useNewGuiInputMethod = False`
rtx.conf line. New blocker: **runaway VRAM growth under Remix** — the
categorize-textures tab shows textures "spamming in and out rapidly", VRAM
climbs past 32 GB even standing still, performance collapses once the
budget fills. Raw D3D9 is fine. (Link mouth/armpit still unverified.)

**Root cause — per-frame D3D9 texture object churn.** Remix identifies game
textures by a content hash computed once per D3D9 texture *object*
(`d3d9_common_texture.cpp SetupForRtxFrom`, XXH3 over the staging buffer;
render targets instead get a hash from a **global incrementing counter** at
creation) and registers/unregisters them in its tracking maps on object
create/destroy (`ImGUI::AddTexture` / `ClearHash → ReleaseTexture`). Remix
holds references across frames, so texture objects the game recreates every
frame accumulate. We churned objects two ways, both keyed to per-frame
game behavior that webgpu tolerated:

1. **Static textures, id-keyed cache + `GXTexObjRAII`.** Dusklight's PC
   wrapper (`include/helpers/gx_helper.h`) evicts the texObj in its
   destructor, and the `dDlst_2D*` drawlist items (`d_drawlist.cpp` — HUD
   minimap et al.) build a **stack-local texobj per draw, every frame**;
   every `GXInitTexObj` mints a fresh `texObjId` (`GXTexture.cpp
   next_tex_obj_id`). Old cache: new id → miss → CreateTexture + upload →
   draw → RAII evict → Release. One create+destroy per draw per frame ⇒
   Remix hash registry flickers ("in and out") and Remix-side references
   pile up the dead objects' VRAM.
2. **EFB copy targets keyed by dest pointer only.** The classic bloom
   (`m_Do_graphic.cpp`, `BloomMode::Classic` default) copies into the SAME
   guest buffer (`zBufferTex`) at width/4 then width/8 **every frame**; the
   size mismatch made `texture_get_copy_target` destroy + recreate the
   render target twice per frame — and every new RT gets a fresh
   counter-based Remix hash, so these can never dedup, ever.

**Fix (aurora `lib/dx9/dx9_texture.cpp`, full rewrite of the cache):**
- **Content-addressed store**: D3D9 textures owned by a map keyed on
  dims/format/mips + 64-bit hash of source bytes (+ TLUT bytes/format).
  `texObjId` entries are now only an alias layer (id+versions → content key)
  so the hot path skips hashing; `GXDestroyTexObj` drops the alias but NOT
  the texture — an identical re-init next frame **resurrects the same D3D9
  object** (stable objects + stable hashes for Remix; zero churn). Source
  byte size computed GC-tile-accurately (`source_data_size`, mirrors
  GXGetTexBufferSize + PC formats). Palette anims now cycle a bounded set of
  stable objects instead of recreating.
- **LRU aging**: sweep every 32 frames; static content unused ~300 frames is
  released, copy targets after ~600. Bounds worst-case dynamic content.
- **Copy targets keyed by (dest, size)**: each copy size keeps its own
  persistent RT; `texture_find_copy` returns the size most recently copied
  into (sample-dest = last-copy semantics). Bloom's 1/4-1/8 alternation now
  reuses two stable RTs.
- The old `s_byPointer` fallback map (id==0, never invalidated) is gone —
  id-less objects just take the content-hash path (more correct: stale
  pointer reuse can no longer serve old pixels).

Docs: `gx-to-d3d9-mapping.md` §5 (content store rationale) and §10
(rewritten — color copies are real, per-size targets) updated.

**Verification state:** `dx9_texture.cpp` passes the MinGW harness (d3d9
on+off). NOT yet run on Windows. No dusklight code change needed (fix is
generic in aurora); dusklight bump is docs + submodule only.

**Next run checklist (Remix):**
- VRAM stable over minutes standing still (watch the same spot that hit
  32 GB); categorize-textures tab stops flickering — static texture set.
- Raw D3D9: unchanged visuals (cache rework must be invisible).
- Carry-over from 3.4/3.2: Link mouth + left armpit; alpha-compare keys
  0x7100/0x7101 are the next suspects if still broken.
- Optional: `game.bloomMode = Off` for Remix runs — the classic bloom's
  screen-space filter quads are meaningless under a path tracer; with the
  fix they no longer leak, but Off removes them entirely.

## Checkpoint 3.4 — Remix root causes: no reconstructable camera (scattered parts) + RIDEV_NOLEGACY input kill (2026-07-24)

**Owner test of 3.3 build:** skinned characters under Remix changed from
"mangled" to "body parts disconnected and widely separated, still animating
and following the character" — so the blend-weight fix worked (Remix's
skinning compute now runs), but the instance transform is wrong. Also new:
**game input dies under Remix** (can't pass the title sequence) while Remix
hotkeys (Alt+X) work.

**Root cause A — Remix cannot derive a camera from our stream** (read from
dxvk-remix source, `rtx_camera_manager.cpp processCameraData`):
```cpp
if (objectToView == objectToWorld && !isIdentityExact(objectToView))
  return CameraType::Unknown;   // <- every draw we submit, VIEW==identity
```
With no valid camera: `commitGeometryToRT` passes `lastCamera == nullptr`
into `finalizePendingFutures` → `finalizeSkinningData` logs
"[RTX-Compatibility-Warn] Cannot decompose the matrices for a skinned mesh
because the camera is not set" and leaves `objectToWorld = WORLDMATRIX(0)`
— **palette slot 0, a different joint matrix for every J3D shape packet**.
Skinning compute (per-draw staged bones, verified correct) deforms each part
right, then each part is offset by its packet's slot-0 joint ⇒ scattered
parts that animate and track the character. Also explains the pre-3.3
"mangled" look (rest-pose parts placed by single joint matrices). Remix's
own GUI confirms the class of problem: "The game doesn't set up the View
Matrix, Anti-Culling is disabled to prevent visual corruption."

**Fix A — real camera via new aurora extension `GXSetViewMtx`
(`GX_AURORA_SET_VIEW_MTX 0x0052`):**
- dusklight: `J3DSys::setViewMtx` (TARGET_PC override, the single funnel for
  every view change incl. frame-interp, item previews, mirrors) now calls
  `GXSetViewMtx(mViewMtx)`.
- aurora: command processor parses 12 f32 → `dx9::set_camera_view` (stores
  D3D view + affine inverse in `g_camera`; wgpu ignores the command).
- `apply_transforms`: when camera valid — rigid `WORLD = pnMtx*view⁻¹`,
  palette `WORLDMATRIX(i) = pnMtx[i]*view⁻¹`, ext-skinning
  `WORLDMATRIX(i) = bone(i)*skinBase*view⁻¹`; `VIEW = view` in all paths.
  `WORLD*VIEW == pnMtx` exactly, so raw-D3D9 rendering is unchanged; Remix
  now sees a real camera, world-space geometry, and object→world bones (its
  `finalizeSkinningData` convention — instance transform becomes identity).
  Texgen compensation (`g_worldViewInv`) still derives from the COMBINED
  model→view (camera-space texgen input is WORLD*VIEW — unchanged).
  Without the call (prelaunch, boot), behavior is exactly the old fused
  mode.

**Root cause B — input:** Remix's new GUI input sink
(`rtx_overlay_window.cpp`, enabled by default via
`rtx.useNewGuiInputMethod = True`) creates an invisible topmost overlay
window at first frame and calls `RegisterRawInputDevices` for the keyboard
with **`RIDEV_NOLEGACY`** — which suppresses legacy `WM_KEYDOWN/UP/CHAR`
for the whole process. SDL3's window never sees another key message; Remix
itself reads its own WM_INPUT sink + `GetKeyState` (why Alt+X works).
**Fix B (config, not code):** set `rtx.useNewGuiInputMethod = False` in
`rtx.conf` — the old input path routes through dxvk's window-proc hook,
which always forwards to the game's proc (`d3d9_swapchain.cpp
D3D9WindowProc` → `CallWindowProc`). Documented in
`dusklight-ao/docs/dx9-fixed-function.md`. No aurora/dusklight code change
can cleanly fix this (raw-input registration is last-writer-wins
process-wide; fighting Remix for it would break the Remix overlay instead).

**Verification state:** all touched aurora TUs pass the MinGW harness (d3d9
on+off configs). NOT yet built/run on Windows. This build now stacks THREE
untested fix sets: 3.2 TEV/terrain, 3.3 blend weights, 3.4 camera split.

**Next run checklist:**
- Raw D3D9 (no Remix): everything must look IDENTICAL to the last good run
  (camera split is a mathematical no-op) — terrain/TEV items from 3.2,
  characters, UI, minimap.
- Under Remix + `rtx.useNewGuiInputMethod = False` in rtx.conf: input works
  (can pass title); characters intact (no scattering, no mangling);
  Remix log should NOT contain "Cannot decompose the matrices for a skinned
  mesh" nor "draw call has bones but no blend weight buffer"; Anti-Culling
  GUI note about missing View Matrix should be gone.
- If skinned meshes look right but lighting/shadows swim on characters,
  check `rtx.conf` skinning-related options next (bone-count limits).

## Checkpoint 3.3 — RTX Remix skinning disfigurement: blend-weight buffer required (2026-07-23)

**Owner report:** running the D3D9 build under RTX Remix (dxvk-remix
d3d9.dll), translated visuals appear, but GPU-skinned characters are
**horribly disfigured** — correct general location, mangled deformation —
while the same build renders them perfectly under raw D3D9 fixed-function.

**Root cause (found by reading dxvk-remix source, read-only):**
`RtxGeometryUtils::dispatchSkinning` (rtx_geometry_utils.cpp) early-outs —
`Logger::err("...draw call has bones but no blend weight buffer, cannot
apply skinning")` — whenever `geometryData.blendWeightBuffer` is undefined.
Our matrix-palette path (PNMTXIDX draws = all normal characters, incl.
Link) used `D3DVBF_0WEIGHTS` + `D3DFVF_XYZB1|LASTBETA_UBYTE4`: dxvk's
FVF→declaration conversion (d3d9_vertex_declaration.cpp) emits **only** a
BLENDINDICES element for that FVF (beta count 1 − UBYTE4 slot = 0 weight
floats), so no BLENDWEIGHT stream exists. Remix still *classifies* the draw
as skinned (`processSkinning` accepts 0WEIGHTS+INDEXEDVERTEXBLENDENABLE),
captures SkinningData/bones, replaces the transform pipeline — but the
skinning compute pass silently never runs → disfigured mesh. That one-line
`Logger::err` should appear in the owner's Remix log (`ONCE`, so a single
line).

**Fix (aurora):** every blended draw now stores ≥1 explicit weight:
- `dx9_vertex.cpp decode_draw`: PNMTXIDX draws emit a stored `1.0f` weight
  (weightCount 0 → 1; FVF becomes `XYZB2|LASTBETA_UBYTE4` = FLOAT1
  BLENDWEIGHT + UBYTE4 BLENDINDICES). Ext-skinning draws clamp to
  weightCount ∈ [1,3]. Equivalent under real FF: implicit last weight is
  1−1.0 = 0 and both FF and Remix's skinning shader skip weight-0 bones
  (padding index byte 0 is harmless).
- `dx9_draw.cpp apply_transforms`: palette path `D3DVBF_0WEIGHTS` →
  `D3DVBF_1WEIGHTS`.
- Spec updated (`gx-to-d3d9-mapping.md` §6a/§6b/§13) with the Remix
  requirement and a new **bone-space caveat**: our blend matrices are
  model→view (VIEW=identity / skinBaseMtx), while Remix's
  `finalizeSkinningData` assumes bones are object→world and re-derives
  instance transforms from its tracked camera. Composition still places
  vertices correctly on screen; only Remix-side "world-space bone
  transform" readouts (e.g. the ReadBoneTransform graph node) see
  camera-relative values. Documented `[later]` option: route the real game
  view matrix through `D3DTS_VIEW` and re-express bones as model→world.

**Verification state:** both changed TUs pass the MinGW `-fsyntax-only`
harness. NOT yet built/run on Windows. **Note: the owner has NOT yet tested
checkpoint 3.2's TEV/TEMP-register build either** — the next Windows build
carries BOTH the terrain/TEV fixes (3.2) and this Remix skinning fix (3.3).

**Next run checklist (raw D3D9, no Remix):** terrain grass/dirt textures
(3.2), Link mouth/armpit (3.2), minimap (3.2), skinned characters still
correct after the 1WEIGHTS switch (3.3 must not regress FF rendering);
device log line should show `tssTemp=true`. **Under Remix:** characters no
longer disfigured; check the Remix log for the "no blend weight buffer"
error line disappearing.

## Checkpoint 3.2 — the "moya" dapple was our own TEV bug: d-term dropped (2026-07-23)

**Run 6 (build `ee3b15014f` — BOTH moya gates included) + full log analysis
settled it:** the scene-wide dapple is NOT the moya cloud-shadow system (it
was gated and still rendered). It is the BG materials' own baked light-ramp
TEV — `PREV + lerp(shadowColor C0, litColor C1, rampTexture)` — and the
mapper was **dropping the `d` (PREV) term**, i.e. deleting the
diffuse-texture base and leaving only the ramp. Log counts: 13 configs
"lerp with additive d dropped", 27 "more than two distinct constants in one
stage", 3 "scale on add ignored" — all the same material family.

**Fix — multi-stage TEV emission using the TSS TEMP register**
(`D3DPMISCCAPS_TSSARGTEMP`, `g_dx9.tssTemp`, logged at device create):
- `d ± lerp(a,b,c)` now EXACT: stage k computes the lerp with
  `D3DTSS_RESULTARG = D3DTA_TEMP` (CURRENT preserved), stage k+1 does
  `ADD/SUBTRACT(d, TEMP)`. Same for `d − x*y`.
- ×2/×4 scale on add/lerp results: appended `MODULATE2X/4X(CURRENT, white)`
  stage instead of being ignored.
- Constants now spread across the decomposed stages' per-stage CONSTANT
  slots, resolving most of the 27 multi-constant warnings.
- Compare-mode ops approximate as always-true (`d + c`) instead of dropping
  the compare result — mask-style materials (suspected Link mouth/armpit)
  stay visible.
- Stage budget: decomposition falls back to single-op approximations when
  the 8-stage budget would overflow; RESULTARG is reset on every stage
  (stale TEMP from prior draws).

**Expected next run:** terrain shows real grass/dirt with subtle light
variation (compare against the DX12 reference shot); Link mouth/armpit
restored; possibly the minimap too (dst-alpha chain may have been a
casualty of the same bugs). Moya drifting shadows remain disabled by
design. If mouth/armpit still missing, next suspects per log:
"irreducible AND/OR" alpha compares (keys 0x7100/0x7101).

## Checkpoint 3.1 — moya hard-disable; Link mouth/armpit under investigation (2026-07-22)

**Run 5 screenshot review:** texgen fix confirmed working (projections now
land correctly — which made the moya overlay visible *as designed*, but the
owner wants it gone entirely). Link's eyes turned out fine (fullbright
washes out the pupils — unlit mode, expected). Still broken:

1. **Moya still rendered** despite the `dKankyo_cloud_Packet::draw`
   early-out. `drawCloudShadow` (d_kankyo_rain.cpp) is its only caller and
   draws z-test-disabled camera-facing billboards — matching the dapple over
   the whole scene. Second gate added inside `drawCloudShadow` itself (the
   single funnel), so the disable no longer depends on which path invokes
   it. If the effect still shows after a *dusklight* rebuild, the build
   didn't include the game-side commits (aurora-only rebuilds don't pick
   them up).
   The washed-out terrain in the same shot is almost certainly the overlay
   itself (SRCALPHA/additive billboards, Z off, drawn over everything) — 
   verify terrain texture recovers once moya is truly gone.
2. **Link's mouth + left armpit/torso shape missing** (see-through). Konst
   selector resolution audited against GXEnum — correct. Cull/winding parity
   with the wgpu path verified. Remaining suspects: compare-mode TEV ops
   (approximated as passthrough), dst-alpha tricks, multi-konst conflicts —
   all of which log `dx9: unsupported: …` warn-once lines naming the
   construct. **Next run: capture the log and match the warn lines**; if
   compare-mode TEV is the culprit, the fix is a targeted ps_1_x fallback
   (within the SM1 ceiling) or a smarter reduction for that pattern.

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
