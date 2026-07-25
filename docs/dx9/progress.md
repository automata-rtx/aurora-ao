# D3D9 backend — progress log & resume notes

> **Rule:** update this file at every conversation checkpoint (before context
> compaction) so a fresh context can resume from the repo alone. Newest entry
> first. Keep "Next steps" honest and specific.

## How to resume with zero context

1. Read `docs/dx9/README.md`, then `architecture-notes.md`, then
   `gx-to-d3d9-mapping.md` (this is the spec being implemented).
2. `git log --oneline` on branch `Fixed-Function-dev` (working branch;
   `Fixed-Function` is the integration branch it merges into at tested
   checkpoints) in both repos: `automata-rtx/aurora-ao` and
   `automata-rtx/dusklight-ao` (aurora is the `extern/aurora` submodule of
   dusklight). The old dev branch `claude/dusklight-dx9-fixed-function-*`
   is retired.
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

## Checkpoint 3.15 — eyes confirmed fixed; resize recreates the device (2026-07-25)

**Owner test of 3.14 (build `6a7922c289`): eyes are fixed** on Link and every
other character checked — the colour-texture albedo preference was the answer.
**And the config-only HUD experiment came back positive:** launching directly
at full display resolution in fullscreen gives correct HUD placement and
scaling under Remix.

**That confirms the device Reset is what breaks the HUD under Remix.** Remix
does not re-derive its UI overlay from a mid-run `Reset` — the HUD keeps the
scale and placement it had at device-creation size — while raw D3D9 follows
the Reset correctly, which is exactly the asymmetry seen all along. Every
earlier run created the device at the initial window size (1216x896) and Reset
afterwards, including the "fullscreen from the get-go" case, because the
window is resized *after* device creation.

**Workaround implemented:** size changes now **recreate** the device rather
than resetting it (`recreate_device`), so Remix restarts its renderer at the
new size and re-derives the overlay. Device creation moved into a shared
`create_device_for` used by both startup and resize. The old `reset_device`
path is kept for same-size device-loss recovery (alt-tab), where Reset is
correct and cheaper.

Two details worth keeping in mind:
- **Resizes are debounced by one stable frame.** Dragging a window edge
  reports a new size every frame; recreating the device (and Remix's renderer
  with it) per frame would be unusable. Interim frames present at the old size.
- **The texture cache is rebuilt** on recreation, since every cached texture
  belongs to the outgoing device. That is a one-time cost per resize.
- A failed recreation leaves `g_dx9.dev == nullptr`; `begin_frame` now retries
  creation each frame rather than going permanently dark.

## Checkpoint 3.14 — eyes identified: albedo must be the colour texture (2026-07-25)

**The eye material, from the 3.12 diagnostic** (build `28bb251f24`, two
affected characters on screen):
```
multi-texture material (3 textured stages):
  [gx0 map2 coord2 32x32 fmt1]   <- I8,   intensity-only mask (highlight)
  [gx1 map0 coord0 64x64 fmt14]  <- CMPR, the eyeball colour texture
  [gx2 map1 coord1 32x32 fmt0]   <- I4,   intensity-only mask (eye shadow)
```
GX formats: 0 = I4, 1 = I8, 14 = CMPR. So the **first** textured stage samples
a 32x32 grayscale mask and the actual eyeball is two stages later. Since the
hint stage advertised the first textured stage's texmap, Remix was handed the
highlight mask as the eye's albedo — precisely the owner's "third texture that
fills the socket", with `al_eyeball` never selected by any draw (hovering it
highlighted nothing).

**Fix:** `preferred_albedo_stage()` picks the first stage sampling a *colour*
texture, falling back to the first textured stage when a material has none.
Intensity-only formats (I4/I8/IA4/IA8) are masks — highlights, shadows,
gradient ramps — never albedo. Checked against every material in the log: only
the eye's selection changes; all others already had a colour texture on their
first textured stage, or none at all (two IA8 stages), so they keep exactly
the texture they had.

**HUD under Remix — still unresolved.** The ortho camera-split fix (3.13) did
not change it. Remaining information: raw D3D9 is correct at every size; the
game-side layout is identical under Remix (same `AuroraGetRenderSize`); Remix
rasterizes ortho draws as a UI overlay by its own heuristics; and Remix's log
shows it *does* follow `ResetSwapChain` to the new size. The device is always
created at the initial window size (1216x896 in every log so far) and Reset
when the window changes — including the "fullscreen from the get-go" case,
since the window is resized after device creation. **The next thing to test is
whether a Reset is what breaks it**, which costs no build: set the window size
in `config.json` (`video.lastWindowWidth/Height`, or `video.enableFullscreen`)
so the device is created at the final size and no Reset happens. If the HUD is
then correct, the workaround is to recreate the device instead of resetting it
on size change; if it is still wrong, Reset is exonerated and the cause is in
how Remix rasterizes the UI overlay itself.

## Checkpoint 3.13 — HUD: no camera split on ortho draws; mods off in D3D9 (2026-07-25)

**Two owner corrections to 3.12, both important:**
1. **Remix auto-detects orthographic draws as UI** and rasterizes them as a
   screen overlay — no manual `rtx.uiTextures` tagging needed. The "HUD is
   path-traced as world geometry" conclusion was wrong.
2. The two hashes in `rtx.ignoreTextures` are **not** eye-related, so the
   rtx.conf pollution does not explain the eyes (still worth clearing, but it
   is not the cause).

**HUD — revised cause.** If Remix already treats our HUD as UI, the remaining
difference is what we hand that path. The 3.4 camera split applies to *every*
draw, so a 2D draw arrives as `WORLD = pnMtx x view⁻¹` with `VIEW =` the 3D
camera. Remix's UI overlay is then derived from a world-space camera rather
than the flat 2D setup a classic ortho draw presents — matching "too large,
stretched incorrectly", constant across window sizes and unrelated to resize,
while raw D3D9 stays correct (the product `WORLD * VIEW` is unchanged).

**Fix:** `apply_transforms` now gates the split on
`g_gxState.projType != GX_ORTHOGRAPHIC`. 2D draws revert to the pre-3.4 fused
form (`VIEW = identity`), exactly what they looked like before the split
existed and when the HUD last behaved under Remix; 3D draws keep the camera.
Rasterization is unaffected on both paths.

**Mods are now skipped on the D3D9 backend** (dusklight `m_Do_main.cpp`).
Carrying a modded `config.json` over to a D3D9 build crashed it instantly —
mod graphics stages are inert here (no WebGPU device) and a native mod that
touches the renderer takes the process down at load. All mod search dirs are
dropped when `auroraInfo.backend == BACKEND_D3D9`, landing on the same "no
mods found" path a clean install takes. **`config.json` is deliberately not
rewritten**, so the same config can move between a modded build and a D3D9
test build untouched.

**Still open — the eyes.** The 3.12 diagnostic showed the material layout but
not which texmap is `al_eyeball`; dimensions/format were added to the log for
exactly that, and a face-on-screen log from build `ad4b9f54bd` or later should
identify it. The lever is which texmap the hint stage binds (all stages share
texcoord 0, so the earliest stage wins), *not* UV ordering.

## Checkpoint 3.12 — material diagnostic read; rtx.conf pollution; quad layout (2026-07-25)

Owner supplied both a dusklight log (build `9f374d2361`) **and Remix's own
log** (`rtx-remix/logs/`, remix-1.5.2). Findings:

**The 3.11 UV reordering is a no-op for these materials — correction.** The
diagnostic shows every stage of every multi-texture material reporting `uv0`:
```
multi-texture material (3 textured stages): [gx0 map2 coord2 uv0] [gx1 map0 coord0 uv0] [gx2 map1 coord1 uv0]
multi-texture material (8 textured stages): [gx0 map1 coord0 uv0] … [gx7 map1 coord7 uv0]
```
Different *texcoords* (coord0..7), same *UV attribute* — these materials use
several texgens reading `GX_TG_TEX0` with different texture matrices (the
8-stage ones are the bloom filter, `m_Do_graphic.cpp`). So there is only one
UV set, it is already slot 0, and reordering changes nothing. Remix bins every
stage into texcoord 0, which means **the earliest stage wins** — i.e. the
albedo is decided by our hint stage, which binds the first texture-bearing GX
stage's texmap. That, not the UV order, is the knob for the eye.

Which texmap should win is still unknown, so the diagnostic now prints each
stage's texture dimensions and format (`[gx0 map2 coord2 64x64 fmt5]`) so a
log can be matched against Remix's texture list.

**Remix's rtx.conf has accumulated experiment tags — actively breaking
things.** Two texture hashes are listed in `rtx.ignoreTextures`, so Remix
renders neither, and one of them (`-0x06111A95F200FAF1`) additionally appears
in *every* category list — `uiTextures`, `skyBoxTextures`, `particleTextures`,
`playerModelTextures`, `decalTextures`, `terrainTextures`, `ignoreLights`,
`worldSpaceUiTextures`, … This is almost certainly left over from the owner's
own experiments (they described ignoring the eye-socket texture, and toggling
transparency on the grass texture — `ignoreTransparencyLayerTextures` holds
the other hash). **Any eye conclusion is unsafe until those lists are
cleared.**

**HUD under Remix — answered.** `rtx.uiTextures` contains only that stray
hash, so the HUD is not tagged as UI and Remix path-traces it as world
geometry, which is why it is wrong from the first frame and unrelated to
resizing. Remix's log also shows it *does* follow the resize correctly
(`ResetSwapChain … 3440x1417`), ruling out presentation. Tagging the HUD
textures as UI in the Remix runtime is the standard per-game step.

**Quad index layout (fixed).** Remix logs `InstanceManager: detected
unsupported quad index layout for billboard creation`. `build_indices` emitted
quads as `(0,1,2)(2,3,0)`; Remix expects the fan order `(0,1,2)(0,2,3)`. Same
triangles, same winding — just a rotation of the second triangle's start — so
this is safe, and it lets Remix's billboard path recognise particle quads.

**Other Remix-log items, not yet acted on:**
- `Trying to bind a texture to a mesh without UVs` — matches the `uv-1` stages
  in our diagnostic (camera-space texgen, no TEX attribute). Harmless-looking
  but worth confirming.
- `Use of texture transform element counts beyond 2 … clamped to 2` and
  `Use of projected texture transform … not supported` — our projected
  camera-space texgen (`GX_TG_MTX3x4`) cannot survive into Remix; projected
  shadows/env maps will be wrong there by design.
- `Attempted invert a non-invertible matrix` (once), `[41] common device
  objects were not disposed of` at exit — low priority.

## Checkpoint 3.11 — eyes: multi-texture materials vs Remix's single albedo (2026-07-25)

**Owner test of 3.10:** **vertex explosions fixed**, and under Remix **most/all
previously-black meshes now show their textures, including the forest
canopy**. Two open items: character eyes/iris, and the HUD under Remix.

**Eyes — root cause.** Adult Link's face model carries **three** eye textures:
`al_eyeball`, `highlight02`, `eye_kage01` (named in
`src/d/actor/d_a_alink_wolf.inc`, where the PC port already clamps their
`maxLOD` "to prevent the eyes from disappearing"). They are composited in a
single draw's TEV chain, and **Remix takes exactly one texture per draw as the
albedo** — so two of the three are dropped no matter what. Owner's Remix
observations line up exactly: hovering `al_eyeball` (the iris) highlights no
geometry, a third texture fills the socket, and hiding it leaves a hole.

**Fix (partial, and deliberately so): make the intended texture win the pick.**
Remix reconstructs from the stage with the lowest `D3DTSS_TEXCOORDINDEX`, and
those indices come from *our* UV-slot assignment (`apply_texgen` returns
`draw.texSlot[attr]`). Previously the slot order followed GX attribute order,
so whichever UV set happened to come first decided the albedo. `decode_draw`
now emits the base texture's UV set (the one sampled by the first
texture-bearing TEV stage) in slot 0, so our albedo hint sits in the lowest
bin and reliably wins. The UV data moves with the slot, so raster is
unchanged.

Whether "first texture-bearing stage" is the *right* choice for the eye is not
yet known — hence the diagnostic below. If the eye's first stage turns out to
be the highlight or the shadow rather than the eyeball, the preference needs
to change, and this is the one knob that decides it.

**New diagnostic.** Multi-texture materials now log their layout once per
distinct configuration:
`dx9: multi-texture material (N textured stages): [gx0 map1 coord0 uv0] …`,
via a new `info_once`. A Remix run's log will now name exactly which
texmap/texcoord each eye stage samples and which UV slot it landed in, which
is what is needed to pick correctly instead of guessing.

**What a real fix would require.** Showing all three eye layers under Remix
means the composite cannot stay in one draw: it would need multi-pass
splitting (base pass, then the overlay stages re-emitted as blended decal
draws with their own albedo). That is raster-equivalent for the common
`lerp(base, overlay, alpha)` shape but is a substantial change to the draw
path, so it is not attempted until the diagnostic confirms the material's
structure.

**HUD under Remix — new information.** Owner reports it is wrong **even when
launching fullscreen with no resize**, while raw D3D9 is correct in every
case. That rules out the mid-run-Reset theory from 3.10. The likely
explanation is that Remix has no reason to treat these draws as UI: it
identifies UI by texture hash (`rtx.uiTextures`, plus
`rtx.worldSpaceUiTextures`), and our HUD arrives as ordinary geometry with an
orthographic projection, so it is path-traced as world geometry. Tagging the
HUD textures in the Remix runtime is the standard per-game step and is worth
trying before any code change. A second, cheaper suspect: our HUD draws still
carry the 3D camera in `D3DTS_VIEW` (the 3.4 camera split applies to every
draw), so an ortho HUD draw is presented to Remix as world geometry seen
through the 3D camera — restricting the camera split to non-orthographic
projections is a small, contained experiment if tagging does not resolve it.

## Checkpoint 3.10 — palette overflow split; hint stage reaches later stages (2026-07-25)

**Owner test of 3.9 (build `9489acaa25`), raw + Remix logs both supplied.**
Fixed: **grass renders correctly under Remix** (the 3.8 albedo/opacity stage
works), and **the HUD now scales correctly on resize in raw D3D9** (3.7's
render rect + reset hardening). Remaining: explosions persist but changed
character — bounded, no longer converging on one point, and now **identical
between raw and Remix**; black meshes and canopy black quads persist under
Remix; HUD still doesn't re-lay-out on resize *under Remix only*.

**Root cause (remaining explosions) — confirmed by the log.** Both runs log
`matrix palette larger than MaxVertexBlendMatrixIndex; excess folded to
slot 0` (key 0x3100). 3.9 stopped emitting out-of-range indices but folded
the excess onto slot 0, so those vertices are transformed by the wrong joint.
That also explains the new raw/Remix agreement: the bad index is baked into
the vertex data, so Remix skins with it too — whereas before 3.9 raw D3D9 got
hardware garbage while Remix used the real matrix 9.

**Fix:** `draw_palette_split` (`dx9_draw.cpp`). When a draw needs more
matrices than `MaxVertexBlendMatrixIndex + 1`, its triangles are partitioned
greedily into groups that each fit, and every group is re-emitted with its own
palette and group-local blend indices (vertices shared across groups are
duplicated). `decode_draw` now records each vertex's GX slot
(`pnMtxPerVertex`) and the blend-index offset so the split can regroup, and
flags `pnMtxOverflow` instead of warning. Draws within the cap are untouched.

**Fix (black meshes / canopy) — hint stage placement.** 3.8's albedo/opacity
hint only fired when the texture was in the *first emitted* D3D stage, and was
additionally blocked whenever the GX stage read CURRENT. Materials whose first
TEV stage does untextured setup, with the texture arriving in a later stage,
never got a hint — which fits grass being fixed while the canopy stayed black.
The hint now fires at the first *texture-bearing* GX stage wherever it lands,
and writes **TEMP** instead of CURRENT (`D3DTSS_RESULTARG`), which Remix
ignores when reconstructing but which leaves the running chain untouched, so
it is raster-neutral at any position. TEMP is only ever live within one GX
stage's own decomposition, never across stages, so clobbering it there is
safe. The CURRENT-writing form is kept only for devices without
`D3DPMISCCAPS_TSSARGTEMP`, where it stays restricted to the head of the chain.

**HUD under Remix — not our bug, most likely.** Neither log contains
`Reset failed` or a skipped frame, so the device Reset succeeds under Remix
too, and the game-side layout path is identical in both runs (same SDL window
size → same `AuroraGetRenderSize`). Since raw D3D9 now re-lays-out correctly
with the same game code, the remaining difference is most likely Remix's own
presentation/upscaling not following a mid-run device Reset. Test that
distinguishes it: launch under Remix already at the target window size — if
the HUD is correct at startup and only wrong after a live resize, it is
Remix's swapchain handling, not our layout.

**Verification state:** all dx9 TUs pass the MinGW harness (d3d9 on+off).
Untested on Windows.

**Next run checklist:**
- Raw D3D9: no stray vertices at all now (the split removes the last
  wrong-joint case). This is still the only place the geometry can be judged.
- Remix: black meshes and canopy foliage should pick up their textures and
  alpha cutouts; grass should stay correct.
- If meshes are still black, the next lever is the white-ground work below
  (compare-mode approximation / constant ceiling), since both come from the
  same TEV reduction gaps.

## Checkpoint 3.9 — raw-D3D9 comparison: matrix palette exceeded the hardware blend-index cap (2026-07-24)

**Owner ran the D3D9 build WITHOUT Remix.** (Log corrected after the fact: the
first log attached to this report was a stale file from build `13f4e4699d`;
the real one is build **`beea767eee`** = checkpoint 3.7. Conclusions below are
from the correct log. The build stamp is trustworthy —
`cmake/DetectVersion.cmake` takes it from `git rev-parse HEAD` at configure
time with a `CMAKE_CONFIGURE_DEPENDS` on the git HEAD file, and CI configures
fresh per run.) Findings, and how they differ from the Remix run:

| | raw D3D9 | under Remix |
|---|---|---|
| rigged characters | **vertex explosions**, all toward ~one distant point | correct |
| grass patches | visible | invisible |
| canopy foliage | visible | black quads |
| Link's eyes | visible | white |
| ground textures | **pure white** | correct |

**Root cause (explosions) — the GX matrix palette is deeper than D3D9's
indexed-blending cap.** GX has 10 position matrices; `pnmtxidx` is already
divided by 3 on decode (`dx9_vertex.cpp`), so blend indices ran 0..9. But the
device reports `MaxVertexBlendMatrixIndex = 8` (see the `dx9: device created`
log line), so index 9 is out of range and reads an undefined matrix — every
vertex using `GX_PNMTX9` lands at the same wrong place. **This is invisible
under Remix**, which ignores the fixed-function cap entirely: it reads the
`D3DTS_WORLDMATRIX` state directly and skins on the GPU. That asymmetry is
why the models looked correct in every Remix test while raw D3D9 exploded.

**Fix:** compact the palette per draw. `decode_draw` assigns D3D9 blend
indices in first-use order and records the GX slot for each
(`DecodedDraw::pnMtxSlots/pnMtxCount`); `apply_transforms` uploads only those
matrices, in that order. A draw referencing more distinct matrices than the
device can index folds the excess onto slot 0 with a one-shot warning
(key 0x3100) instead of emitting an out-of-range index.

**Ground textures white in raw D3D9 (not fixed — diagnosis only).** Remix
shows the ground correctly *because* it only reads one texture stage (so the
base texture, which is the first stage, is what it sees); raw D3D9 executes
the whole chain, so a **later** stage is saturating the result. Suspects from
the correct log, most-likely first:
1. `tev: compare-mode op approximated as always-true (d + c)` — **4** distinct
   stages. GX compare ops are `d + (cond ? c : 0)`; D3D9 fixed-function has no
   comparison, and we currently take the always-true branch, so a conditional
   highlight/detail term is added *unconditionally* and can saturate to white.
   This is the only suspect that explains white specifically. Flipping the
   approximation to always-**false** (just `d`, dropping the conditional term)
   is a one-line, easily-reverted change and the obvious next experiment.
2. `tev: more than two distinct constants in one stage` — **28** distinct
   stages, by far the most frequent unsupported case. Per stage we only have
   TFACTOR (one value per draw) + `D3DTSS_CONSTANT` (per stage) = 2 constants;
   a third falls back to TFACTOR and silently takes the wrong value. Raising
   this ceiling means splitting the stage to claim another constant slot.
3. `tev: non-PREV output register treated as PREV` — only **2** stages. GX has
   four TEV output registers (PREV + REG0/1/2) vs D3D9's two usable ones
   (CURRENT + TEMP, the latter gated on `D3DPMISCCAPS_TSSARGTEMP`, which this
   device has). Collapsing REG0 onto PREV corrupts chains that stash and
   re-read. Lower frequency than the above, and the fix (allocate REG0 to
   TEMP, reserving the intra-stage decomposition's TEMP use) is the riskiest.

Texture creation is healthy in this log — no `CreateTexture` failures, no
unhandled formats — so the white is a TEV result, not a missing texture.

**Verification state:** all dx9 TUs pass the MinGW harness (d3d9 on+off). The
palette fix is untested on Windows.

**Next run checklist:**
- Raw D3D9 first: rigged characters should have no stray vertices. This is
  the primary check — Remix cannot show this bug either way.
- Then Remix: characters should still be correct (the compacted palette is
  what Remix's `finalizeSkinningData` reads), plus the 3.8 albedo/opacity
  stage for the black assets, foliage cutouts and grass.

## Checkpoint 3.8 — correction: `None` is identity; the single-stage view is the bug (2026-07-24)

**3.7 did not fix the black assets** (owner retest). Its premise was wrong and
is corrected here — future sessions should not re-derive it.

**What 3.7 got wrong:** it assumed undecodable args
(`D3DTA_TEMP`/`CONSTANT`/`COMPLEMENT`/`ALPHAREPLICATE` →
`RtTextureArgSource::None`) render black. They do not.
`opaque_surface_material_interaction.slangh` resolves `None` to **identity**:
```glsl
chooseTextureArgument(textureArg1, surface.textureColorArg1Source, albedo,
                      surfaceInteraction.vertexColor.rgb, tFactor.rgb, vec3(1.0));
// alpha arg1's None fallback is `opacity` (the sampled texture alpha), arg2's is 1.0
```
That also explains why the 3.7 hint stage never fired on the broken draws: a
stage like `MODULATE(TEXTURE, TFACTOR)` is perfectly *decodable*, so the
"decodable?" test passed and no hint was emitted.

**Actual mechanism:** Remix takes ONE stage's color op as the entire albedo
and its alpha op as the entire opacity. A GX material's first TEV stage is
rarely the finished albedo — commonly `texture × konst` with a dark konst
(which we route through TFACTOR), giving a black/near-black albedo — and its
alpha is often a blend weight rather than the texture's alpha, which destroys
alpha-test cutouts. This single mechanism covers all three owner symptoms:
black assets world-wide, canopy foliage as **opaque black quads** (albedo from
a dark partial result + opacity stuck at 1 → no cutout), and grass
**invisible** (opacity from a blend weight → 0), reappearing as **plain
quads** when Remix's transparency handling is disabled for that texture.

**Fix (`dx9_tev.cpp`):** replace the "decodable?" test with "does the leading
stage already present the texture plainly?" (`op_is_plain_texture`:
`SELECTARG1`/`MODULATE` over `TEXTURE`/`DIFFUSE` only, no constants, no
modifier bits). If not, prepend `color = MODULATE(TEXTURE, DIFFUSE)`,
`alpha = SELECTARG1(TEXTURE)`. Alpha deliberately selects the texture's own
alpha — that is what Remix alpha-tests and what gives foliage/grass their
cutout shape. Same CURRENT-safety guard as 3.7, so raster is unchanged.

**Still unresolved / needs data:**
- **HUD.** Settled by the 3.9 log: `dx9: device created 1216x896 (render
  1216x896+0+0, …)` with `Using framebuffer size 1216x896 scale 1`. Render
  rect == backbuffer at offset 0, so `g_frameBufferAspectFit` is false →
  **"Lock 4:3 Aspect Ratio" is OFF** → policy is `AURORA_VIEWPORT_STRETCH`,
  where filling the window is by design and 3.7's letterbox is inert. Also
  `fb_* == native_fb_*`, so `internalResolutionScale` is not in play. That run
  logged only one `device created` and no resets — **it never resized**, so it
  cannot show the resize behavior at all. To go further: a log from a run that
  actually resizes, plus the same window size on a WebGPU backend for
  comparison. If wgpu re-lays-out the HUD where D3D9 doesn't at identical
  size, it is our bug; if both stretch, what the owner wants is the aspect
  lock (or the game's own widescreen HUD path).
- **Whether the black/foliage symptoms also occur in raw D3D9** (no Remix).
  Everything above assumes Remix-only; if raw D3D9 shows them too, the bug is
  in our TEV reduction, not Remix's reconstruction.
- **Link's iris.** Owner confirms the eye uses two textures; hovering the iris
  texture in Remix highlights no geometry, and a third texture fills the
  socket. Remix binds at most 2 textures per draw and only `colorTextures[0]`
  is albedo for normal materials, so a second-texture iris is unrepresentable
  by design. `vertexColorIsBakedLighting` (default **true**) was checked and
  is not implicated.

## Checkpoint 3.7 — Remix reads ONE texture stage: black materials, HUD scale, resize reset (2026-07-24)

**Owner report (Remix run):** VRAM leak fixed. New: (a) many assets across
the world render with a **black texture** although the textures are present
in Remix's texture-categorization list; (b) **Link's eyes render white** —
the outer/edge eye texture shows, the iris inside does not; (c) the **HUD no
longer scales with the window**, and resizing the window **crashes**.

**Root cause A — Remix decodes only a tiny subset of fixed-function state.**
`D3D9Rtx::processTextures<FixedFunction>` (d3d9_rtx.cpp) picks ONE stage —
the first bound to the lowest `D3DTSS_TEXCOORDINDEX` — and
`setTextureStageState` (d3d9_rtx_utils.cpp) reconstructs the material from
*that stage alone* via:
```cpp
DxvkRtTextureOperation convertTextureOp(uint32_t op) {   // default -> Modulate
  case D3DTOP_DISABLE / SELECTARG1 / SELECTARG2 / MODULATE2X / MODULATE4X / ADD
}
RtTextureArgSource convertTextureArg(uint32_t arg, ...) {
  default: return RtTextureArgSource::None;              // <-- everything else
  case D3DTA_CURRENT: case D3DTA_DIFFUSE: ... case D3DTA_TEXTURE: case D3DTA_TFACTOR:
}
```
The switch runs on the **raw** arg, so `D3DTA_TEMP`, `D3DTA_CONSTANT`, and
any arg carrying `D3DTA_COMPLEMENT` / `D3DTA_ALPHAREPLICATE` all decode to
`None` — the texture drops out of the material and the surface renders black
even though it is resident and correctly bound (hence "visible in the texture
list but not applied"). Our TEV reduction emits all of those routinely:
`D3DTA_TEMP` for the checkpoint-3.2 split-stage decomposition,
`TEXTURE|ALPHAREPLICATE` for GX's TEXA/RASA-as-color operands, per-stage
`D3DTA_CONSTANT` for a second distinct constant.

**Fix A** (`dx9_tev.cpp`): when the leading stage of a textured draw isn't
decodable, prepend a plain `MODULATE(TEXTURE, DIFFUSE)` stage for Remix to
read. It writes CURRENT and the real chain overwrites it, and it is only
emitted when nothing in that GX stage reads CURRENT — so **rasterization is
unchanged**. Also: bind the texture only on emitted stages that actually
reference `D3DTA_TEXTURE` (Remix keeps just two texture candidates per draw,
so duplicates from a decomposed stage crowd out a real second texture), and
disable *and unbind* every unused stage at the end of the chain — Remix's
scan `continue`s past stages with no texture instead of stopping, so a
texture left bound on a high stage by an earlier draw could be collected as a
candidate and win the albedo slot if its stale texcoord sorted lower.

**Root cause B — render size vs. HUD layout size.** The game lays out its HUD
against `AuroraGetRenderSize()` = `AuroraWindowSize::fb_*`, which the
viewport policy (`AURORA_VIEWPORT_FIT`, the default) letterboxes to the
game's aspect. The wgpu path renders into a framebuffer texture of exactly
that size. The D3D9 path created its backbuffer from `native_fb_*` (the raw
window pixels) and reported that as the render target size, so on any
non-4:3 window the image was stretched to fill and the HUD was laid out for a
size we never rendered into.

**Fix B** (`dx9_backend.cpp`, `dx9_internal.hpp`): the backbuffer stays
native-sized, but a new render rect (`renderWidth/Height` +
`renderOffsetX/Y`) tracks `fb_*` centered inside it. `get_backbuffer_size`
reports the render size, viewport/scissor/EFB-copy rects add the offset, and
the letterbox bars are cleared to black instead of the scene clear color.

**Fix C — resize.** `reset_device` now ends any offscreen pass, ends the
scene, and unbinds all textures before releasing default-pool resources (a
resource still bound to the device survives our Release and makes `Reset`
fail); a failed Reset marks the device lost and retries next frame instead of
drawing against a half-reset device; minimized windows (0x0) skip the frame
rather than resetting to a 0-sized backbuffer. **The crash itself is not yet
confirmed** — no log was available for the crashing run. If it persists,
capture `dusklight*.log` from a run that crashes on resize.

**Verification state:** all five dx9 TUs pass the MinGW harness in d3d9
on+off. NOT yet run on Windows. (`lib/gx/gx.cpp` and `lib/gfx/common.cpp`
cannot be syntax-checked with the harness — the shim's Dawn headers are
stubs; unchanged here.)

**Next run checklist:**
- Black assets: expect textures to appear. Any that stay black are draws
  whose *chosen* stage still isn't decodable — grab the Remix log and check
  for `[RTX-Compatibility-Info] Texture 0 without valid hash detected`.
- Link's eyes: may still be wrong. Remix binds at most 2 textures per draw
  and only uses `colorTextures[0]` as albedo for normal materials
  (`ColorTexture2` is RayPortal-only per d3d9_rtx.cpp), so a second-stage
  iris decal may be unrepresentable by design — confirm whether the iris is
  a separate draw before pursuing.
- HUD should scale with the window again, with black letterbox bars and no
  horizontal stretch on wide windows. Raw D3D9 aspect should now match the
  wgpu backends.
- Resize should no longer crash; if it does, the log is required.

## Checkpoint 3.6 — branch consolidation (2026-07-24)

Owner reorganized both repos around two branches (same names in each):
`Fixed-Function` = integration (fast-forwarded to the 3.5 tip, CI green),
`Fixed-Function-dev` = working branch for all commits, merged into
`Fixed-Function` at tested checkpoints. Aurora's lineage descends from
aurora `main` (17 ahead at reorg time, main unmoved); dusklight's shares
its fork-point ancestry with `ao` (the fork's active line, 12 commits to
potentially backport at reorg time) and `main` (upstream tracker) — so
backports are ordinary merges/cherry-picks. The generated dev branch
`claude/dusklight-dx9-fixed-function-6uoy92` is retired (deletable once no
session is bound to it). Also: dusklight CI got a vcpkg-install retry loop
after a transient TLS flake failed the MSVC arm64 job pre-compile
(dusklight `5807989f06`); run #101 confirmed all 8 jobs green.

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
