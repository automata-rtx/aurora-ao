# D3D9 backend — progress log

**What this file is:** the record of what changed in `lib/dx9/`, when, and why —
plus §"Why the backend is shaped the way it is", which collects the decisions a
cold session most often needs and would otherwise have to reconstruct from
scattered entries.

**What this file is not:** the state of the project. That lives in
`dusklight-ao/docs/remix-open-issues.md`. It is also not a system description —
the four documents in §"Where things are documented" own those, and entries here
link to them rather than re-explaining.

---

## How to resume with zero context

1. `README.md`, then `architecture-notes.md`, then `gx-to-d3d9-mapping.md` (the
   spec being implemented). **If the task touches materials or colour, read
   `remix-material-interface.md` first** — it is the system most often reasoned
   about incorrectly here.
2. **If the task is about how the game *looks* under Remix rather than about the
   D3D9 backend, you are in the wrong repo.** Go to
   `dusklight-ao/docs/kankyo-remix.md`.
3. Read §"Why the backend is shaped the way it is" below. It is short and it
   prevents most re-litigation.
4. `CLAUDE.md` at the repo root is the authority on branches, verification and
   the owner's environment.

Build target is Windows. This work is developed in a Linux container, so see
§"Verification vocabulary" before claiming anything compiles.

## Where things are documented

| Question | Document |
| :-- | :-- |
| How does aurora render, and where does the D3D9 backend intercept? | `architecture-notes.md` |
| How should a given GX construct map to D3D9? | `gx-to-d3d9-mapping.md` |
| Why did this surface come out the wrong colour? | `remix-material-interface.md` |
| What do the `matrep.*` log lines mean? | `material-report.md` |
| What can't be expressed at all, and what does Remix constrain? | `unsupported-effects.md` |
| What is broken right now, across all three repos? | `dusklight-ao/docs/remix-open-issues.md` |

## Verification vocabulary

Three different claims, and this project has been burned by conflating them.
Every entry below states which one it is.

| Term | Means |
| :-- | :-- |
| **syntax-checked** | Passes `x86_64-w64-mingw32-g++ -std=c++20 -fsyntax-only` against real MinGW d3d9/windows headers, real repo headers and real absl/fmt/xxhash, in **both** the d3d9-on and d3d9-off configs. Dawn/SDL3 are shimmed, so `lib/gx/gx.cpp` and `lib/gfx/common.cpp` cannot be checked this way. |
| **CI-green** | Built by dusklight-ao's GitHub Actions with the submodule pin bumped. This repo has no CI of its own. |
| **tested in game** | The owner ran it on Windows and reported back. |

"Syntax-checked" says nothing about correctness, and several changes below were
syntax-checked, shipped, and wrong.

---

## Why the backend is shaped the way it is

The decisions that are expensive to rediscover, with the symptom that forced
each one. Grouped by system rather than by date; the checkpoint that established
each is named so the full account is findable.

### Camera and transforms

- **The game's real view matrix is forwarded** through an aurora extension
  (`GXSetViewMtx`, `GX_AURORA_SET_VIEW_MTX`), and `apply_transforms` splits
  `WORLD = pnMtx · view⁻¹`, `VIEW = view`. Without it Remix's camera manager
  rejects every draw as `CameraType::Unknown` — it treats `objectToView ==
  objectToWorld` as "no camera" — which left skinned meshes placed by palette
  slot 0 and disabled Anti-Culling. `WORLD * VIEW` is unchanged, so raw D3D9
  rasterization is identical. *(3.4)*
- **The split is skipped for orthographic draws.** Remix auto-detects ortho
  draws as UI and rasterizes them as a screen overlay; handing that path a 3D
  view matrix mis-shaped the HUD. *(3.13)*
- **Camera-space texgen is compensated** by premultiplying the texture matrix
  with the per-draw model-view inverse (`g_worldViewInv`), because GX texgen
  reads *model-space* inputs where D3D9 gives view-space. Rigid draws only.
  *(3)*
- `world = modelView · viewInv` is guarded by a determinant check that logs
  `camera view matrix not invertible`. It has never fired. *(re-verified
  2026-07-27)*

### Skinning

- **Every blended draw stores at least one explicit weight.** Remix classifies
  a draw as skinned but its skinning compute pass silently early-outs when
  there is no blend-weight buffer, which disfigured every character. The old
  `D3DVBF_0WEIGHTS` + `XYZB1|LASTBETA_UBYTE4` FVF produced no BLENDWEIGHT
  element at all. *(3.3)*
- **The matrix palette is compacted per draw.** GX has 10 position matrices;
  the reference device reports `MaxVertexBlendMatrixIndex = 8`, so index 9 read
  an undefined matrix and scattered those vertices. Invisible under Remix,
  which ignores the fixed-function cap and skins on the GPU — which is why this
  hid for weeks behind correct-looking Remix output. *(3.9)*
- **Draws needing more matrices than the cap are split** into per-palette
  groups rather than folded onto slot 0, which transformed vertices by the
  wrong joint. *(3.10)*

### Textures

- **The texture store is content-addressed**, keyed on dims/format/mips plus a
  hash of the source bytes, with `texObjId` as an alias layer. Remix identifies
  textures per D3D9 *object* and holds references across frames, so the game's
  habit of building a stack-local texobj per draw made VRAM climb past 32 GB
  and the categorization list flicker. An identical re-init now resurrects the
  same D3D9 object. LRU sweep every 32 frames. *(3.5)*
- **EFB copy targets are keyed by (dest, size)**, because the bloom chain
  copies into the same guest buffer at two sizes every frame and a
  destroy/recreate per size gave every target a fresh Remix hash forever. *(3.5)*
- Pointers are therefore stable for the life of the device, which is what makes
  them usable as the join key in `material-report.md`.

### Materials

The whole system is documented in **`remix-material-interface.md`**; that file
is authoritative and this section deliberately does not duplicate it. The
decisions it records as load-bearing:

- Remix rebuilds a material from **one** texture stage, and anything it cannot
  decode resolves to **identity (white)**, not black. An earlier revision of
  these docs claimed black and it cost a full test round. *(3.8)*
- A raster-neutral **hint stage** is prepended when Remix would otherwise read
  the stage wrongly — and **only** then. Emitting it over a material Remix
  already reads correctly is what bleached rupees, hearts and lava. *(3.8
  introduced it, 3.19 made it conditional)*
- **The material's albedo is *evaluated*, not pattern-matched.** The GX colour
  pass is computed with the texture pinned to black and to white; the two
  endpoints say what the surface should look like. **The floor decides the op:**
  a black floor takes `MODULATE` (exact), a coloured floor takes `ADD`, because
  a multiply renders black wherever the texture is black and would replace the
  object's own colour with a hole. The earlier "look for a texture × constant
  multiply" model matched 6 of 111 real materials, and gating `ADD` on the ramp
  ending near white matched 5 — both because this game's materials are additive
  two-colour ramps. *(3.20, corrected 3.21)*
- **Vertex colour is not advertised to Remix**: it carries baked lighting here,
  which a path tracer must not receive in the albedo. Real stages keep it, so
  raw D3D9 is unchanged. *(3.21)*
- **Self-illumination is split: aurora reports evidence, the fork judges.** GX's
  "this channel takes no light" bit is true of **59% of materials**, so it is
  evidence, never a rule. Aurora sends it plus `matSrc` and the presented colour
  over `D3DMATERIAL9::Emissive` — free, because `D3DRS_LIGHTING` is off here so
  no D3D9 material is ever read; the brightness and saturation thresholds live
  in the fork's options so they move without a rebuild. *(3.22)*
- The hint advertises the material's **colour** texture where there is one, and
  a stage that *reads* its texture in preference to one that merely binds it —
  character eyes composite a 32×32 I8 highlight mask with the real 64×64 CMPR
  eyeball two stages later, while other materials open with a setup stage that
  binds a texture and does nothing with it. *(3.14, 3.20)*
- **Multi-stage TEV decomposition uses the TSS TEMP register**
  (`D3DPMISCCAPS_TSSARGTEMP`), making `d ± lerp(a,b,c)` exact instead of
  dropping the `d` term — a bug that deleted the diffuse base of every BG
  light-ramp material and read on screen as a scene-wide dapple. *(3.2)*
- **Unused stages are disabled *and* unbound.** Remix's scan skips stages with
  no texture rather than stopping, so a texture left bound on a high stage by
  an earlier draw could win the albedo slot. *(3.7)*

### Render targets, window and resize

- **EFB colour copies and offscreen passes are real** (`StretchRect` /
  `SetRenderTarget`). Depth-format copies remain a **white / alpha-0**
  placeholder — deliberately not opaque black, which darkened every projected
  consumer. *(3)*
- **A size change recreates the device rather than resetting it.** Remix does
  not re-derive its UI overlay from a mid-run `Reset`, so the HUD kept the scale
  it had at device-creation size. Debounced by one stable frame; the texture
  cache is rebuilt, which is the visible black pause. `Reset` is kept for
  same-size device-loss recovery. *(3.15, confirmed 3.16)*
- **The backbuffer is native-sized with a render rect inside it**, letterboxed
  to the game's aspect and cleared black, because the game lays out its HUD
  against the render size. *(3.7)*

### Fog

`apply_fog_state()` decodes `g_gxState.fog`'s reconstructed A/B/C coefficients
back to view-space start/end — the scale ambiguity resolved with the projection
near plane, `near = m23/(m22 − 1)` — and sets the `D3DRS_FOG*` states per draw.
EXP/EXP2 approximate via `FOGDENSITY = 8·ln2/(end−start)`; REVEXP and ortho
projections stay fog-off. *(3.17)*

**Caveat that has misled people:** with Remix's defaults the captured fog is
consumed by *neither* path — composite fog is skipped while volumetrics are on,
and fog remap is off. A mode must be chosen; see
`dusklight-ao/docs/dx9-fixed-function.md`.

### Integration with the game

- **GX lighting is never evaluated** (`D3DRS_LIGHTING = FALSE`), so the game's
  own Link-following light reaches neither vertex colours nor albedo. That is
  why the sun/moon must be injected through the Remix light API rather than
  captured — and why the GX "this channel is unlit" bit is currently decoded and
  discarded (`remix-open-issues.md` issue 9).
- **Dawn is never initialized**, so every path that calls it unconditionally
  needs a `dx9::active()` guard. Two were found crashing at startup: ImGui
  texture creation and `resize_swapchain`. *(2)*
- **RmlUi document creation is gated** on `dusk::ui::initialize()`, so the game
  must be launchable without the prelaunch picker (`--dvd <path>` or
  `backend.isoPath`). *(2.2)*
- **Mods are skipped entirely** on this backend — their graphics stages are
  inert without WebGPU and a native mod that touches the renderer crashes at
  load. `config.json` is deliberately not rewritten. *(3.13)*
- `aurora_dx9_get_device()` exposes the live device so dusklight's bridge can
  call `dxvk_RegisterD3D9Device`. The pointer changes on resize-recreation, so
  callers poll it. *(3.18)*
- **`dx9: device created …` in the log is the signature that the backend is
  actually active.** More than one "it still crashes" report turned out to be a
  run that never selected D3D9. *(2.1)*

---

## Checkpoint log

Newest first. Per-checkpoint "next run" checklists have been removed once the
run happened; where a run produced a durable finding it is folded into the
section above or into the owning document.

### 3.22 — self-illumination: evidence here, judgement in the fork (2026-08-04)

GX has no emissive term, so there was nothing to translate. What it has is a
colour channel that takes no light with its colour authored in a register — and
that bit alone was **69 of 117 materials** in the last session, so acting on it
would have set 59% of the scene glowing. Splitting the two halves is the whole
design:

- **aurora** reports evidence — unlit, register-sourced, 3D, evaluable, and the
  colour the surface presents — over `D3DMATERIAL9::Emissive`. That field was
  free end to end: this backend keeps `D3DRS_LIGHTING` off, so **the change
  cannot alter the raw D3D9 image**, and the fork was already copying the whole
  `D3DMATERIAL9` and never reading it.
- **the fork** applies the brightness and saturation thresholds, as
  `rtx.dusklight.emissive.*` options in the F1 overlay, so widening or narrowing
  the rule does not cost a rebuild.

Replayed against the 2026-08-03 22:23 log the shipped defaults fire on **8 of
117** materials. Which of the eight is lava is not established; both halves log
their verdict (`selfLit=` on `matrep.sum`, `dusklight.emis` on the fork side,
rejections included) so the next session settles it from the log.

**Syntax-checked** in both configs, and **CI-green** on all 8 dusklight targets
and the fork's 3 Windows configs. Untested in game. Regression signature:
surfaces glowing that should not — most plausibly unlit interior geometry, which
the game authors that way because it forces interior ambient to black.

Design and the measurement: `remix-material-interface.md` §9. Log format:
`material-report.md`.

### 3.21 — the floor decides the op; alpha scale; vertex colour withheld (2026-08-04)

**3.20 was tested: colours came back, but every item had an inverted highlight.**
Reading the real GX programs settled why. These materials are additive — the
heart is literally `out = B80000 + 0.25 × texture` — so advertising a multiply
was structurally wrong, and a multiply renders black wherever the texture is
black. That black *was* the "inverted glint".

Three changes, each from a specific line in the session log:

1. **The floor decides the op**, not the top. A coloured floor takes `ADD`,
   which holds it exactly; only a black floor keeps `MODULATE`. The previous
   near-white gate matched 5 materials out of 111. Cost: `ADD` cannot reproduce
   the texture's scale, so highlights are brighter than the original.
2. **A constant scale on the texture's alpha now reaches Remix**, via TFACTOR's
   alpha channel. 18 of 111 materials are `konst × TEXA`; dropping the konst is
   why HUD fade-ins drew their whole quad opaque.
3. **Vertex colour is no longer advertised.** Owner testing of
   `rtx.vertexColorIsBakedLighting` showed the vertex colours carry baked
   lighting, which a path tracer must not receive in the albedo. Real stages are
   untouched, so raw D3D9 is unchanged. This corrects a claim `kankyo-remix.md`
   had carried for weeks.

**CI-green.** Untested in game. The broadest risk is (3): surfaces that relied
on vertex colour may read flatter or brighter.

### 3.20 — the material model was wrong; evaluate instead of pattern-match (2026-08-04)

**3.19 was tested. It was safe but nearly inert — 3 materials out of 111.** The
log said why, and the answer was a wrong model rather than a coding error.

`albedo_tint()` looked for a literal `texture x constant` multiply. The dominant
shape in this game is `lerp(colourA, colourB, textureIntensity)` — a greyscale
texture selecting between two authored colours, which is how one rupee texture
yields seven rupee colours. A multiply is only the special case where colourA is
black. **104 of 111 materials reported no tint**; the 6 that were detected were
exactly the black-floored ones, and those came out of Remix correctly coloured.

Changes:

1. **`evaluate_albedo()` replaces `albedo_tint()`.** It evaluates the GX colour
   pass twice — texture pinned to black, then to white — and reports the two
   endpoints. No pattern matching.
2. **The op is chosen from the endpoints.** A multiply cannot represent a ramp
   with a non-black floor (it falls to black where the texture is dark, which
   *darkens* the surface), so those advertise `ADD` instead, which Remix also
   decodes. Simulated over the captured materials: **30 now carry colour, up
   from 6.**
3. **`preferred_albedo_stage()` now prefers a stage that *reads* its texture.**
   Materials here routinely open with a setup stage that binds a texture and
   does nothing with it; picking it handed Remix an empty program.
4. **The hint no longer multiplies by vertex colour when the material never read
   it** — that factor is a substituted white on meshes without vertex colours.

Report gains `shape`, `out0`/`out1`, `usesTex`, `usesVtx` and `form`, so the
next log states what each material *is* and what we advertised for it.

Two instrumentation defects from 3.19 fixed: the cross-log join key did not
actually join (the two sides formatted the pointer differently), and the fork's
dedup key included `tFactor`, whose value tracks fog and time of day — 828
distinct values exhausted the 1024 cap in 14 seconds.

**CI-green** — dusklight run 30864069703 built Windows MSVC x86_64 against this
commit and uploaded artifacts. (It was not syntax-checked locally; no MinGW
toolchain was available. The evaluator's *logic* was validated separately by
re-implementing it against the 111 real materials captured in the 2026-08-03
log.) **Untested in game.**

The fork half of the same change set did *not* build first time — a padding
assert in the report's dedup key was wrong — which is worth recording because it
is the one part of this work a compiler could catch and the local harness could
not.

### 3.19 — the greyscale defect was our own hint stage (2026-08-03)

**The 2026-07-29 tint fix (`389e4d5`) was tested and did nothing**, and the
diagnosis behind it was wrong in its central claim. Remix was reading these
materials correctly until aurora's hint stage overwrote them; the fix was gated
behind a predicate that never fired, so it emitted no D3D9 state at all.

Changes: the hint is now conditional (`remix_decodes_albedo()`); the `matrep.*`
material report lands, always on and capped, replacing the old multi-texture
line; `remix-material-interface.md` and `material-report.md` are written.

**Syntax-checked: NO** — no MinGW toolchain in that container. Reviewed by
inspection; the first CI run is the syntax check.

**Untested in game.** Regression signature: surfaces going *dark*, not staying
grey. Full account: `remix-material-interface.md`, and issue 8 in
`dusklight-ao/docs/remix-open-issues.md`.

### 3.18 — expose the D3D9 device for the Remix light API (2026-07-26)

`aurora_dx9_get_device()` / `aurora::dx9::get_device()`. Syntax-checked.

### 3.17 — GX fog forwarded as D3D9 fog render states (2026-07-26)

Implements the mapping spec's fog section, which v1 left disabled. Details in
§Fog above. Syntax-checked (`dx9_backend.cpp` untouched).

### 3.16 — resize workaround confirmed (2026-07-25)

**Tested in game:** resizing works under Remix; the HUD comes back correctly
placed and scaled, after the expected black pause. Owner also noted "a number of
original-game visual effects are broken", never triaged into a list.

### 3.15 — eyes fixed; resize recreates the device (2026-07-25)

**Tested in game:** the colour-texture albedo preference fixed the eyes on every
character checked. A config-only experiment (launching at final resolution)
confirmed the device `Reset` was what broke the HUD under Remix, which is what
justified switching to recreation.

### 3.14 — eye material identified (2026-07-25)

The 3.12 diagnostic named the layout: `[I8 highlight] [CMPR eyeball] [I4 shadow]`,
so the first textured stage was a mask. `preferred_albedo_stage()` added.
Checked against every material in the log — only the eye's selection changed.

### 3.13 — no camera split on ortho draws; mods off in D3D9 (2026-07-25)

Two owner corrections to 3.12, both important: **Remix auto-detects ortho draws
as UI** (so the "HUD is path-traced as world geometry" conclusion was wrong), and
the stray `rtx.ignoreTextures` hashes were not eye-related.

### 3.12 — material diagnostic read; rtx.conf pollution; quad layout (2026-07-25)

- **The 3.11 UV reordering was a no-op** — every stage already reported `uv0`;
  the materials use several texgens over one UV set. The albedo is decided by
  which texmap the hint binds, not by UV order.
- **Remix's `rtx.conf` had accumulated experiment tags**, one hash appearing in
  nearly every category list. *Any conclusion drawn while those are set is
  unsafe* — clear them before judging a build.
- **Quad indices now emit fan order** `(0,1,2)(0,2,3)`; Remix's billboard path
  rejects the other winding-equivalent layout.

### 3.11 — eyes traced to multi-texture materials (2026-07-25)

**Tested in game:** vertex explosions fixed, and most previously-black meshes
including the forest canopy show their textures. Added the multi-texture
material diagnostic (since superseded by `matrep`).

### 3.10 — palette overflow split; hint reaches later stages (2026-07-25)

**Tested in game:** grass renders correctly under Remix; the HUD scales on
resize in raw D3D9. Fixed the remaining explosions (wrong-joint folding) and
made the hint fire at the first *texture-bearing* stage wherever it lands,
writing TEMP so it is raster-neutral at any position.

### 3.9 — raw-D3D9 comparison: the matrix palette overran the cap (2026-07-24)

**The owner ran the build without Remix**, which is the only way several of
these were visible at all:

| | raw D3D9 | under Remix |
|---|---|---|
| rigged characters | vertex explosions | correct |
| ground textures | **pure white** | correct |
| grass / canopy / eyes | visible | broken |

**Ground textures white in raw D3D9 is still open.** Remix is correct because it
only executes one stage. Ranked suspects and the one-line experiment are in
`unsupported-effects.md`.

### 3.8 — correction: `None` is identity, not black (2026-07-24)

3.7 did not fix the black assets and **its premise was wrong**: undecodable args
resolve to identity, not black. The real mechanism is the single-stage view
itself. Introduced the hint stage.

### 3.7 — Remix decodes only a subset of fixed-function state (2026-07-24)

**Tested in game:** VRAM leak fixed. Found the render-rect/letterbox HUD sizing
bug and hardened `reset_device` (end passes, unbind textures, retry on failure,
skip 0×0).

### 3.6 — branch consolidation (2026-07-24)

Superseded. `CLAUDE.md` is the authority on branches.

### 3.5 — Remix VRAM leak: texture objects must be stable (2026-07-24)

**Tested in game:** rigged meshes and terrain correct under Remix; input fixed
by `rtx.useNewGuiInputMethod = False`. New blocker was runaway VRAM, root-caused
to per-frame texture object churn — see §Textures.

### 3.4 — no reconstructable camera; input killed by raw-input registration (2026-07-24)

Two root causes read out of dxvk-remix source: Remix could not derive a camera
from our stream (§Camera), and Remix's new GUI input sink registers the keyboard
with `RIDEV_NOLEGACY`, which suppresses legacy key messages process-wide. The
input fix is a config line, not code: `rtx.useNewGuiInputMethod = False`.

### 3.3 — Remix skinning needs a blend-weight buffer (2026-07-23)

Found by reading dxvk-remix source: `dispatchSkinning` early-outs without one.
See §Skinning.

### 3.2 — the scene-wide dapple was our own dropped `d` term (2026-07-23)

Not the moya cloud-shadow system, which was gated and still rendered. The TEV
mapper was deleting the diffuse base of every BG light-ramp material. Fixed by
multi-stage decomposition through the TEMP register — see §Materials.

### 3.1 — moya hard-disabled (2026-07-22)

Second gate added inside `drawCloudShadow` itself, the single funnel, so the
disable no longer depends on which path invokes it. Link's eyes turned out to be
correct (fullbright washes out pupils — expected in unlit mode).

### 3 — in-game and rendering (2026-07-22)

**Fourth run reached gameplay.** Skinned characters, HUD, particles and pickups
all correct; the owner confirmed the unlit look is what Remix wants. Fixed the
camera-space texgen compensation and implemented real EFB colour copies — see
§Render targets.

### 2.2 — UI document crash fixed (2026-07-22)

`game_main` created RmlUi documents unconditionally; the subclass constructor
dereferenced a null document. See §Integration.

### 2.1 — the run never selected D3D9 (2026-07-22)

A malformed `config.json` silently reverted the backend to WebGPU, and the crash
was a pre-existing wgpu fatal. Also fixed: `shader.cpp vtx_attr` now substitutes
`vec2f(0.0)` for missing TEX0-7 attributes.

### 2 — first Windows run: startup crash fixed (2026-07-22)

Dawn calls on paths reachable in D3D9 mode. See §Integration. The audit of every
other Dawn-touching path is in git history for this entry if it is ever needed;
none of the rest required guards.

### 1 — research, scaffold, doc set (2026-07-22)

Architecture research (`architecture-notes.md`), the initial `lib/dx9/` module
and executor per the mapping spec, and this doc set. All bring-up milestones and
open questions from this entry have since been resolved or superseded.
