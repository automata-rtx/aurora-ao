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
   `dusklight-ao/docs/kankyo-remix.md`. If it sends you into game code, read
   `dusklight-ao/docs/japanese-naming.md` too — those identifiers are romanized
   Japanese, and one romanization finds half a feature.
3. Read §"Why the backend is shaped the way it is" below. It is short and it
   prevents most re-litigation.
4. `CLAUDE.md` at the repo root is the authority on branches, verification and
   the owner's environment.

Build target is Windows. This work is developed in a Linux container, so see
§"Verification vocabulary" before claiming anything compiles.

## The one architectural fact that changes how you read the rest

**The raw D3D9 image is a feed into Remix, not a product.** Nobody plays it. So
"fixed function cannot express this" is a statement about the transport, and the
answer is to do the work in Remix. Entries below written before 2026-08-04
sometimes justify a decision with "raw D3D9 is unchanged" — read that as a
safety note, not as the reason.

**Two things do still have to rasterize correctly:** the **HUD** (Remix
rasterizes UI draws rather than path-tracing them) and **alpha** (Remix reads
the stage's alpha to build opacity and the alpha test). Full statement:
`remix-material-interface.md` §0.

## Where things are documented

| Question | Document |
| :-- | :-- |
| How does aurora render, and where does the D3D9 backend intercept? | `architecture-notes.md` |
| How should a given GX construct map to D3D9? | `gx-to-d3d9-mapping.md` |
| Why did this surface come out the wrong colour? | `remix-material-interface.md` |
| What do the `matrep.*` log lines mean? | `material-report.md` |
| How do HD texture packs reach Remix, and why not through D3D9? | `texture-replacements.md` |
| What the feed can't carry today, and where that work would go? | `unsupported-effects.md` |
| What is broken right now, across all three repos? | `dusklight-ao/docs/remix-open-issues.md` |

## Verification vocabulary

Three different claims, and this project has been burned by conflating them.
Every entry below states which one it is.

| Term | Means |
| :-- | :-- |
| **syntax-checked** | `scripts/check_syntax.sh` passes. Real MinGW `<d3d9.h>`/`<windows.h>`, real repo headers, real absl and fmt, `-fsyntax-only`, in **both** the d3d9-on and d3d9-off configs — the second is not optional, since every entry point has a no-op stub behind `#else` in `dx9.hpp` and a signature change that misses it fails only there. Dawn, SDL3, Tracy and xxHash are shimmed (`scripts/syntax-harness/`), so `lib/gx/gx.cpp` and `lib/gfx/common.cpp` cannot be checked this way. **It does not validate format strings** — the distro fmt is 9.x against aurora's 11.x, and the shim says so. Setup: `apt-get install g++-mingw-w64-x86-64 libfmt-dev libabsl-dev`. |
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
  slot 0 and disabled Anti-Culling. `WORLD * VIEW` is unchanged, so the
  rasterized image is identical — a safety note, not the reason. *(3.4)*
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
  already reads correctly is what bleached rupees, hearts and lava. It writes
  TEMP, so it disturbs neither the HUD nor alpha — the two things that do still
  have to rasterize correctly. *(3.8 introduced it, 3.19 made it conditional)*
- **The material's albedo is *evaluated*, not pattern-matched.** The GX colour
  pass is computed with the texture pinned to black and to white; the two
  endpoints say what the surface should look like. **The floor decides the op:**
  a black floor takes `MODULATE` (exact), a coloured floor takes `ADD`, because
  a multiply renders black wherever the texture is black and would replace the
  object's own colour with a hole. The earlier "look for a texture × constant
  multiply" model matched 6 of 111 real materials, and gating `ADD` on the ramp
  ending near white matched 5 — both because this game's materials are additive
  two-colour ramps. Since 3.24 the ramp itself is reproduced exactly in the fork
  (next bullet); this op choice is what a material takes when that path declines.
  *(3.20, corrected 3.21)*
- **Vertex colour is forwarded only where GX says it is material colour.** With
  the colour channel's lighting enabled the stream is the material GX then
  lights, so it carries no light of its own and a path tracer can have it; with
  lighting disabled it is the finished output, which is where this game bakes
  room light. Withholding it globally (3.21) was too blunt and threw away real
  colour. With no CLR0 attribute at all it is a constant and is simply
  evaluated. *(3.21, corrected 3.25)*
- **Two-colour ramps are reproduced, not approximated.** `lerp(A, B, texture)`
  is this game's dominant material shape and no stock Remix op expresses it, so
  the fork evaluates the GX combiner directly from two endpoints carried in
  spare surface bits. Aurora declines when TFACTOR was claimed by a real stage,
  and the material falls back to the single-op approximation. *(3.24)*
- **Self-illumination is a rule, not a score.** No single GX fact identifies an
  emitter, and the lava scores **zero** on all three weighted signals, so the
  score is not the rule either. What is: **self-lit** (no TEV colour stage reads
  the rasterized channel) AND **a colour of its own** (authored in TEV
  constants, not the vertex stream and not a bare texture pass-through) AND that
  colour **reading as a glow**. Aurora sends all three over
  `D3DMATERIAL9::Specular` — free, because `D3DRS_LIGHTING` is off here so no
  D3D9 material is ever read. 6 of 77 materials in the measured scene, no false
  positives, nothing to tune. *(3.22, corrected 3.23, 3.27, 3.28)*
- **`grp=` does not work.** It was meant to label every material with the game
  code that drew it, and it prints `-` for all of them: the hook was at a draw
  *scheduling* point, not an issuing one. Removed. Identifying a material still
  means reading its texture size, format and ramp endpoints. *(3.23, corrected
  3.27)*
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
  why **every** light in this game reaches Remix through the light API rather
  than being captured: the sun/moon as a distant light, and fire and glow as
  sphere lights placed at the origin of the effect that draws them
  (`dusklight-ao/docs/effect-lights.md`, tested in game 2026-08-07). Indoors
  the latter are the whole lighting solution — the sun/moon is gated off there
  and Remix has no dome light type, so a sky is never NEE-sampled. **The GX "this channel is unlit" bit is no longer discarded**: it is
  the heaviest of the three signals in the self-illumination score *(3.23)* and
  it is what decides whether vertex colour is forwarded *(3.25)*. It is not
  sufficient on its own — 59% of one scene is unlit and the Goron Mines lava is
  *lit* (`remix-open-issues.md` issue 9).
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

### 3.33 — water rebased onto the current side band, and the allocation is checked (2026-08-11)

**Two shared resources were being allocated by branches that could not see each
other, and both collisions survived a `git merge`.** The full audit is
`in-flight-allocation.md`; this is what changed here.

**The side band.** `claude/water-rendering-investigation-7baezw` forked before HD
texture packs claimed `Ambient.g`/`.b` and wrote its water flags into them, plus
`Ambient.a` and `Power`. Merging it as written deletes texture packs: the code
conflict resolves the wrong way — the water side is newer, self-consistent and
well commented — and the field map merges clean, so nothing in the diff says what
was lost. Measured, not predicted: resolving it that way and silencing the two
complaints the old check raised left the tree green with
`grep -r texRepIndex lib/` returning nothing.

Rebased rather than merged, and the assignment re-derived. All three water facts
now share `Power`:

```
Power = tag * 100 + layer * 10 + role
```

role 0-2, layer 0-7, tag 0-99, maximum 9972, and a `float32` holds every integer
to 16777216 exactly. Decimal rather than bit fields because the number is read by
a human in a log far more often than by code — `power=921` is legible as MA09 /
waves / surface. One field rather than three so `Ambient.a` survives; it is now
the only spare channel there is.

**The subcommand space.** Four live branches had each taken `0x0053`. The
`#define`s conflict, but the `else if` chain in `command_processor.cpp` does
**not** — both arms merge, the first tested wins, and because the arms consume
different payload lengths the loser leaves the reader mid-payload and desyncs the
FIFO. The symptom is a garbled frame or a `CHECK` naming an opcode the game never
called, which is debugged nowhere near the cause. `GXAurora.h` now carries a
registry comment listing every allocated number and reserving `0x0054`-`0x0057`
for the other three claimants.

**Checks, because human review is not the safeguard here.**
`check_invariants.py` goes from 4 to 6: the side-channel map now runs **both**
directions and includes `Power` (the reverse is what catches a merge that drops
the code claiming a channel while the table still describes it), duplicate and
unregistered subcommands fail, and the water packing is checked against the
formula §2 states. Each was verified by breaking the tree and confirming the
message names the right thing.

`scripts/check_syntax.sh` is new and is why "syntax-checked" above now names
something runnable. It caught a real error during this work: a first attempt
compiled `dx9_tev.cpp` cleanly and it turned out `AURORA_ENABLE_D3D9` was
undefined, so the whole file had been `#ifdef`-ed away — a vacuous pass reported
as a real one, which is the exact failure mode the vocabulary table exists for.

**Status.** Syntax-checked, both configs. **Not built, not tested in game.** The
2026-08-08 measurement that water reaches Remix was taken on the old wire and
does not cover this one. Regression signature: every draw decodes `role=none` and
water renders opaque and white-ish exactly as before the feature existed;
`power=` on the `dx9.water` and `dusklight.water` lines is what says so.

### 3.32 — dense particles were a draw-call problem, and the backend can now count draws (2026-08-08)

**Tested in game 2026-08-08: the owner reports particle performance is "far
better than previously."** Read that precisely — the *outcome* is tested. The
`dx9.draws` figures were not reported back, so the predicted collapse from
~1000 draws a frame to one is confirmed **by its effect, not by the counter**.
Anyone with a log from a rainy Hyrule Field session can close that gap in a
minute, and should.

**The defect was in the game, not here**, but it is recorded in this repo
because the cost it exposed is a property of how this backend submits draws.
Rain in Hyrule Field and snow in the Snowpeak exteriors ran at unusable frame
rates under Remix. The kankyo weather effects are immediate-mode GX and were
emitting **one `GXBegin`/`GXEnd` per quad** — `dKyr_drawRain` up to 250 drops ×
4 offset layers, so up to a thousand draws a frame. (`dKy` is *kankyo*, 環境,
the game's environment system; weather lives inside it.) This backend submits each
`GXBegin` block as its own `*UP` call (`command_processor.cpp` decodes and
submits immediately; there is no batching layer), so every quad became a
separate draw in Remix. Fixed game-side in `d_kankyo_rain.cpp` by hoisting
`GXBegin`/`GXEnd` out of the per-quad loops.

**Why this is worth knowing here: Remix charges per draw, not per pixel.** A
draw too small for its own BLAS is merged into a shared bucket, but it still
contributes its own `VkAccelerationStructureGeometryKHR` and its own surface,
and that bucket rebuilds whenever the geometry moves — every frame, for
weather. Read in the fork at `rtx_accel_manager.cpp`
(`buildInfo.geometryCount = bucket->geometries.size()`, and the bucket's
`originalInstances`).

**Two owner observations did the diagnostic work, and both are reusable:**

- **Marking the textures as particles in Remix's categorization UI changed
  nothing.** That category only picks which TLAS a draw lands in and how the
  resolve loop treats it. It is not a performance control, and if a particle
  problem does not respond to it the cost is per draw, not per pixel.
- **Tagging the same texture as UI made it smooth.** UI draws never enter the
  raytraced scene, so that swap removes per-draw cost and nothing else. As a
  *diagnostic* it is sharp: if UI-tagging fixes the frame rate, the answer is
  draw count. (As a fix it is not free — it also removes the effect from the
  path tracer, and for snow it double-composites the game's own planar
  reflection copies.)

**This does not contradict the grass entry in
[`unsupported-effects.md`](unsupported-effects.md), and the difference is the
point.** There, batching is called out as *destroying* asset-hash identity,
because `rtx.geometryAssetHashRuleString` is `positions,indices,…` and grass has
a stable per-blade display-list path available. Particles have no such path:
every drop moves every frame, so the hash churned before batching and churns
after. Batching costs nothing that was not already lost, and texture *tagging*
is unaffected either way because tags key on the texture hash, not the geometry
hash. **Batching is wrong for geometry that could have a stable identity and
right for geometry that cannot.**

**New in this repo: `dx9.draws`.** `lib/dx9/dx9_draw.cpp` counts every
`DrawPrimitiveUP` / `DrawIndexedPrimitiveUP` the backend issues, including each
draw of a palette-split skinned mesh, and logs one line every 600 frames:

```
dx9.draws frames=600 mean=412 peak=1387 - D3D9 draw calls per frame
```

`peak` is carried separately from `mean` because the spike only exists while
the weather is running, and a mean over 600 frames hides it. This existed
nowhere before, which is why a thousand-draw frame went unnoticed for months.

**Syntax-checked** in both the d3d9-on and d3d9-off configs. The counter is
plain arithmetic on a frame boundary; it has no failure mode that a build would
not catch.

### 3.31 — HD texture packs: tested good, and the first-launch cost explained (2026-08-06)

**Tested in game. Worked on the first try** — replacements appear, texture
tagging is unaffected, nothing else regressed.

One characteristic came out of the session: **a long first-launch warm-up**,
after which every later launch has the pack immediately.

**And the first explanation written for it was wrong in the usual way** — it is
worth reading §9 for the shape of the mistake, not just the answer. Remix keeps
no on-disk cache of loaded *textures*: verified, `findAsset` reopens the `.dds`
every launch and the only dedupe map dies with the process. From that the entry
concluded the OS file cache must be the cause. It does not follow, and the step
skipped a durable cache that does exist: **DXVK writes a pipeline state cache to
disk**, on by default, and this runtime's own options document the first-load
compilation cost that goes with it. Every first launch is slow for that reason,
pack or no pack.

So there are two candidates, one durable and one volatile, and **which dominates
is unmeasured**. The texture side is also partly a *symptom*: creation is
budgeted per frame, so slow frames from any cause stretch how long the pack
takes to finish arriving. "Textures appear late" is not evidence that textures
are what is slow.

The experiment that separates them costs one reboot — it clears the OS page
cache and keeps `.dxvk-cache`. Fixes are named in §9 and deliberately **not**
taken, so this feature's "tested" claim stays intact.
[`texture-replacements.md`](texture-replacements.md) §9.

What the session did *not* exercise, and so is still only reasoned-about: BC7/BC5
packs, a PNG entry being skipped, the device-loss path, multi-texture UI draws,
and `$` TLUT wildcards on animated art.

### 3.30 — HD texture packs reach Remix without entering D3D9 (2026-08-05)

**Implemented. Protocol 6 → 7. CI-green on both repos; tested good 2026-08-06
(see 3.31).** [`texture-replacements.md`](texture-replacements.md) is the design.

3.29 concluded "substitute the replacement bytes at D3D9 texture creation". That
works and it is the wrong trade: Remix's material hash *is* the stage-0 D3D9
texture's content hash, so it would re-key every tag, every `rtx.conf` category
and every USD binding the moment a pack is installed or edited. It also cannot
carry BC7/BC5.

So the bytes do not go through D3D9 at all. **Aurora's upload path is
unchanged** — that is the whole point, and it makes tagging bit-identical to a
pack-less run rather than merely "still working". The game hands each `.dds` to
`remixapi_CreateMaterial` (used purely as a file loader; the material is never
bound) and aurora tags each draw with a 1-based index in `Ambient.g`, with the
stage it describes in `Ambient.b`.

**Two substitution sites, and this is the part that is easy to get wrong.** A UI
draw returns `{Rasterized, true}` from `makeDrawCallType` and never reaches
material resolution at all — it samples whatever `BindTexture` bound. So the
material-side swap alone would have left the HUD at the game's own resolution,
which is most of what a pack is wanted for. The fork substitutes in
`determineMaterialData` for path-traced draws and in `D3D9DeviceEx::BindTexture`
for rasterized ones.

`Ambient.b` exists because only *dirty* textures rebind: a multi-texture draw
can rebind a stage the index says nothing about, and substituting there would be
the wrong texture rather than a missing one.

Two failure modes worth naming because both are silent: an unresolved handle
falls back to the game's texture rather than binding the empty slot (which
renders black, and would read as "the pack broke everything"); and the preserve
path is held off only while a handle is *unsettled*, with "no material for this
index" counting as settled, so a pack entry the game never created cannot
disable instance preservation forever.

Regression signature: `matrep.sum texrep=` and the overlay's two counter rows
separate "the game never handed it over" from "the fork ignored it". If
`tex0hash=` ever differs between pack-on and pack-off for the same scene, the
upload path was changed and tagging *has* broken — that should be impossible.

### 3.29 — HD texture packs on d3d9: feasible, and #17 was wrong about the HUD (2026-08-05) — SUPERSEDED IN PART BY 3.30

**Investigation only. No code changed.** Its §4 change list is superseded: the
substitute-in-aurora design it proposed was dropped for the reason above. Its
finding about the HUD stands and is why 3.30 has two substitution sites.

The registry half of the replacement system **already runs in D3D9 mode** —
`dusk::texture_replacements::reload()` fires at `m_Do_main.cpp:701` for any
backend but `BACKEND_NULL`, and registration never touches wgpu. What is missing
is the consumer: `find_replacement()` returns a wgpu `TextureHandle` built by
`g_device.CreateTexture`, and `lib/dx9/dx9_texture.cpp` never asks. The seam is
one call earlier, at the `gfx::ConvertedTexture` the DDS/PNG loaders already
produce.

**The originals stay out of Remix's list for free, if the substitution happens
at creation.** Remix hashes a D3D9 texture in `SetupForRtxFrom`
(`d3d9_common_texture.cpp:666`) and feeds `g_imguiTextureMap` via
`ImGUI::AddTexture`; that map *is* the categorization grid, and it truncates
when it outruns the descriptor pool. Build the D3D9 texture from the
replacement instead of from the GX bytes and no texture object exists for the
original, so nothing to hash and nothing to suppress. Entry count is unchanged
from today.

**Correction to `unsupported-effects.md` #17 and `architecture-notes.md`
§Textures**, both of which said aurora-side packs "only matter to the standalone
image". True for path-traced surfaces; **false for the HUD.** `isRenderingUI`
(`d3d9_rtx.cpp:561`, `orthographicIsUI` default true) classifies our HUD by
projection, those draws are rasterized, and rasterized draws never reach
`getReplacementMaterial`. The D3D9 texture is the only lever on HUD fidelity
that exists. Both documents corrected.

**The cost to weigh before any code:** `LegacyMaterialData::updateCachedHash()`
is `m_cachedHash = colorTextures[0].getImageHash()` (`rtx_materials.h:1869`), so
every USD binding and every hash-keyed `rtx.conf` entry is a function of the
pack's bytes. Enabling the pack re-keys the scene once; editing one pack texture
re-keys that material. Pick the pack, freeze it, then author — and treat it as
part of the protocol rather than as a runtime toggle.

### 3.28 — self-lit is the rule; the score is retired (2026-08-05)

**The score never should have been the cut.** Three revisions used one and all
three missed the lava. 3.27 corrected *which* fact the 0.50 signal measured;
this removes the weighting entirely.

A GameCube surface is **self-lit** when no TEV colour stage reads the rasterized
channel — its colour is then fixed whatever the lights do, which is what the
console draws full-bright. That is the basis. It is not sufficient: of the 20
evaluated self-lit materials in the 2026-08-05 session, **9 are EFB copies and
full-screen quads** (304×224, 608×448, 608×100 `RGBA8`), and a white screen blit
must not light the room.

Every one of those is a bare texture pass-through — `out0=000000`,
`out1=FFFFFF`, no colour of its own — and every real emitter carries a colour
authored in TEV constants over an intensity mask. So `colorAuthored`
(`Specular.b`) now means *"has a colour of its own"* and covers both failure
modes: mixed from the vertex stream (§7c baked room lighting) or a plain
pass-through. `Specular.a` carries self-lit.

    emissive = self-lit AND own colour AND (chroma >= 0.50 OR luma >= 0.70)

The **or** in the last clause is what killed the brown false positives this
feature carried since rev 1: an authored glow is a strong colour or it is
near-white-hot, and a muted mid-tone is a surface colour.

**Replayed over that log: 6 of 77 materials, no false positives, nothing to
tune** — `D572C706`, `1C95BA4B`, `9866CEA2` (`FF0000`), `40D45477` (`FF6B00`),
`7E31CACC` (`FF6432`), `EFF68502` (`FFF0A0`). Rejections: 30 not evaluated, 27
read the lit channel, 9 no colour of their own, 5 neither saturated nor bright.

Fork side: `threshold`, `requireAuthoredColor`, `minLuma` and `minChroma` are
gone; `colorSource` defaults to the reconstructed albedo, because with §10's
ramp exact that is `lerp(FF0000, FFFE63, texture)` — the texture drives the
colour and neither overpowers the other. The evidence score is still computed
and logged and decides nothing.

**Syntax-checked**, both configs. Untested in game. Regression signature: too
much glowing means the glow test is too loose — the `dusklight.emis` line names
which clause admitted each material. Nothing glowing at all means `Specular.a`
is not arriving, which an aurora older than this commit would cause.

### 3.27 — the lava scores zero, and the glow is a choice again (2026-08-05) — SUPERSEDED IN PART BY 3.28

**Tested, and it disproved 3.26 rather than tuning it.**

The main Goron Mines lava pool (`mk=D572C706…`, `mk=1C95BA4B…`, 32×32
`GX_TF_IA8`, `tfactor=FFFF0000`, ramp `FF0000 → FFFE63`) scores **0.00** on all
three evidence signals — GX lighting on, channel colour from the vertex stream,
no over-range stage — identically in the 2026-08-04 and 2026-08-05 logs. The
fork's `isCandidate` tested `score > 0`, a hidden second threshold no overlay
setting could move, so that surface was never an emitter at any setting.

Which is the whole of the 2026-08-05 report: "stuck at an almost solid red
without proper texture definition, no option to change it, emissive intensity
does nothing regardless of value" is what a **non-emissive** 3.24 ramp looks
like — `mix(FF0000, FFFE63, texture)` as a diffuse reflectance in a dark cave,
red pinned at 1.0 across the surface because both endpoints have `R = 255`.

Aurora's half: two facts now ride `D3DMATERIAL9::Specular`, both free.
`Specular.g` says aurora evaluated a presentable colour here at all — which lets
a score of zero be admissible while HUD and unevaluable draws still are not —
and `Specular.b` says the colour came entirely from TEV constants, which is what
makes the fork's threshold of 0 survivable. The score itself is unchanged;
existing weights still mean what they meant. `matrep.sum` gains `emisEval=` and
`emisAuthored=`.

The fork's half: threshold defaults to 0, `requireAuthoredColor` joins
`minLuma`/`minChroma` as the actual rule, and **`colorSource` restores as a
control what 3.26 removed** — reconstructed albedo / albedo texture through its
own op (default) / flat presented colour.

**3.26 was wrong in two ways worth remembering.** It removed the only tested
configuration on the strength of a prediction about untested code; and its
closing claim that "the lava scores 0.25 on the over-range signal alone" was
true of *a* lava material, not the pool — the same log has five lava-family
materials scoring 0.00, 0.50, 0.50, 0.75 and 0.75, and nothing identifies which
is on screen, because `grp=` does not work.

**Syntax-checked**, both configs. Untested in game. Regression signature: too
much of the world glowing means threshold 0 is too wide — raise it. Nothing
glowing at all means `Specular.g` is not arriving, which an aurora older than
this commit against a newer fork would cause (the fork keeps a `score > 0`
fallback for exactly that).

### 3.26 — the glow is the albedo (2026-08-04) — SUPERSEDED BY 3.27

**Tested.** The lava needed the "Emit The Texture" toggle to look right and was
otherwise "an almost solid red"; the heart needed it too. The cause was the
design, not tuning: a flat emissive constant at intensity 2 swamps the albedo,
so a molten surface reads as one uniform hot colour with no crust. The texture
toggle only helped because it happened to vary across the surface.

So the constant is gone. A self-lit surface glows the colour it appears, and
after 3.24 that colour is already computed one block earlier in the shader —
`emissiveColor = albedo`. GX has no emissive term to disagree with the albedo,
so reconstructing one separately only invited it to be wrong.

Net removal: the pre-image inversion, the `invertible=` log field and the
`useTextureColor` option all existed to get a constant through the albedo's
texture op intact, and none survives.

Also claimed by this entry, and **wrong**: "the lava scores 0.25 on the
over-range signal alone". That was true of one lava-family material, not of the
pool, which scores 0.00. See 3.27.

**Syntax-checked**, both configs. Untested in game.

### 3.25 — vertex colour, forwarded selectively (2026-08-04)

3.21 stopped advertising `DIFFUSE` altogether because vertex colour carries
baked lighting here. That was true and too blunt: **GX distinguishes the two
cases per draw** and we were throwing away the good one.

- lighting **enabled** on the colour channel: the vertex stream is the material
  colour GX then lights, so it carries no light of its own → **forward it**
- lighting **disabled**: the stream is the finished output, where this game
  bakes room light → **withhold it**
- **no `CLR0` attribute**: aurora substitutes a constant from the channel's
  material colour register. `eval_operand` was treating `DIFFUSE` as white
  unconditionally, so those materials evaluated as if they had no colour at
  all — a silent loss, now fixed, which also makes them §10 ramp-eligible.

The verdict rides `D3DMATERIAL9::Specular.r` and the fork sets
`isVertexColorBakedLighting` **per draw** from it rather than from the global
option, which was necessarily wrong for one of the two cases. `vtxUse=` on
`matrep.sum` prints `material`, `bakedLight` or `const`.

**Syntax-checked** both configs, and **CI-green** on all 8 dusklight targets and
the fork's 3 Windows configs. Untested in game. Regression signature: surfaces
that should be flat gaining a per-vertex tint (forwarding too much), or shaded
areas going flat/bright (the constant case now colouring something it should
not).

Known judgement call, deliberately left: a lighting-disabled draw whose vertex
colour is genuinely authored — a tint or fade on an effect rather than baked
room light — is withheld. `vtxUse=bakedLight` in the log is where to look if
something loses a gradient it should have.

### 3.24 — reproduce the GX colour combiner instead of approximating it (2026-08-04)

**The framing was wrong, not just the numbers.** "No single Remix op can express
a lerp between two constants" is a fact about *stock* Remix — and this fork is
ours. So the ramp is no longer approximated: both endpoints are carried to the
surface and the shader evaluates

```
albedo = mix(rampLo, rampHi, albedo);   // per channel
```

which is `a*(1-c) + b*c`, the GX colour combiner itself. Exact for **every** ramp
material at once, not just the lava that prompted it.

It fits with **no growth of the GPU `Surface` struct** (sized to exactly two
128-byte cachelines) and **no precision loss** (GX registers are 8 bits per
channel and so is the transport): one endpoint already rode `TFACTOR`, the other
takes `data15.w` which was permanent zero padding, and the two flags take
`textureFlags` bits 15-16 which were unused.

Aurora declines when `TFACTOR` was claimed by a real stage rather than the hint,
because then only one endpoint is reachable; `ramp=` on `matrep.sum` gives the
reason and the material falls back to the previous approximation.

**Syntax-checked** (aurora half) and **CI-green** on all 8 dusklight targets and
the fork's 3 Windows configs. The Slang half compiled without a single
diagnostic; what broke the first fork build was `CheckRtInstanceSize` — adding
fields to `RtSurface` grows `RtInstance`, and that guard is release-only so no
container check can see it. Untested in game.

Regression signature: ramp materials rendering as a flat colour, or with light
and dark inverted, would mean the endpoints are swapped;
`rtx.dusklight.rampMaterials` turns it off live for an A/B against the
approximation.

### 3.23 — the emissive premise was wrong; label every draw (2026-08-04) — SUPERSEDED BY 3.28

> **`rtx.dusklight.emissive.threshold` no longer exists.** 3.28 retired the score
> and the cut with it — the rule is a conjunction now, and the only dial is
> `rtx.dusklight.emissive.brightness`. Kept because the *measurements* below are
> still the evidence base.

**3.22 was tested in the Goron Mines. It fired on one material and that material
was not lava.** The rule required GX lighting to be *disabled*; every candidate
lava material has **`lit=1`**. Combined with the earlier "59% of a scene is
unlit" measurement, the conclusion is that **no single GX fact identifies an
emitter** — "unlit" is simultaneously too broad and too narrow.

Three changes:

1. **Evidence is scored, not required.** Lighting disabled 0.50, colour authored
   in a register 0.25, **a TEV stage scaled past what the console could display
   0.25** — the last is new, and is the only thing in GX that states "brighter
   than the display". The fork cut at `rtx.dusklight.emissive.threshold`, live
   in the overlay, so widening no longer costs a rebuild.
2. **`grp=` on every `matrep.sum` line.** The game pushes a debug group per
   process draw at `fpcDw_Execute` — and aurora mirrors the innermost label into
   `GXState::currentDebugGroup()`. **This did not work and has been removed.**
   `fpcDw_Execute` is where a draw is *scheduled*, not issued; every material in
   every session since has logged `grp=-`. "Which logged material is the lava?"
   is still open. See `remix-material-interface.md` §9 "Identification".
3. **A real bug in 3.22's glow colour**, found by reading the shader rather than
   by testing: `emissiveColorConstant` is re-run through the *albedo's* texture
   op, so the glow arrived as `colour + tFactor`. The fork now sets the
   pre-image and declines when the op cannot be inverted. **(Superseded by
   3.26: the glow is the albedo, so there is no constant to pre-invert.)**

**Syntax-checked** in both configs. Untested in game. Regression signature: the
`grp=` push costs one short string per drawn process per frame — if frame time
regresses noticeably, suspect it first. *(Moot: the push was removed once it
proved to label nothing. See 3.27.)*

Not fixed here, and now documented rather than guessed at: **no stock Remix op
expresses a lerp between two constants**, so the lava's `lerp(red, yellow, t)`
reproduced as red-to-white. `remix-material-interface.md` §10 has the
arithmetic. **Superseded by 3.24**, which stopped reading that as a wall — the
fork is ours — and made the shader evaluate the combiner exactly.

### 3.22 — self-illumination: evidence here, judgement in the fork (2026-08-04)

**Superseded by 3.23**, which tested this and found the premise wrong: the rule
here *required* the colour channel to be unlit, and every Goron Mines lava
material has `lit=1`. Evidence is scored now, not required. The split below —
aurora reports, the fork judges — survived and is still the design.

GX has no emissive term, so there was nothing to translate. What it has is a
colour channel that takes no light with its colour authored in a register — and
that bit alone was **69 of 117 materials** in the last session, so acting on it
would have set 59% of the scene glowing. Splitting the two halves is the whole
design:

- **aurora** reports evidence — unlit, register-sourced, 3D, evaluable, and the
  colour the surface presents — over `D3DMATERIAL9::Emissive`. That field was
  free end to end: this backend keeps `D3DRS_LIGHTING` off so nothing consumes a
  D3D9 material at all, and the fork was already copying the whole
  `D3DMATERIAL9` and never reading it. (It therefore also cannot alter the
  rasterized image — a safety note, not why the channel was chosen.)
- **the fork** applies the brightness and saturation thresholds, as
  `rtx.dusklight.emissive.*` options in the F1 overlay, so widening or narrowing
  the rule does not cost a rebuild.

Replayed against the 2026-08-03 22:23 log the shipped defaults fire on **8 of
117** materials. Which of the eight is lava is not established; both halves log
their verdict (`selfLit=` on `matrep.sum`, `dusklight.emis` on the fork side,
rejections included) so the next session settles it from the log.

**Syntax-checked** in both configs, and **CI-green** on all 8 dusklight targets
and the fork's 3 Windows configs. Untested in game *as written* — the test came
immediately after and is written up in 3.23. Regression signature:
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
   lighting, which a path tracer must not receive in the albedo. This corrects a
   claim `kankyo-remix.md` had carried for weeks. **Superseded by 3.25**, which
   found this too blunt: GX distinguishes baked lighting from authored material
   colour per draw, and only the former should be withheld.

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

**Superseded 2026-08-04: white ground in raw D3D9 is not a defect.** Remix is
correct — it only executes one stage — and the raw image is never shown to
anyone. The compare-mode approximation underneath it is still worth reading,
because it is a suspect for the torch-flame white circle, which *does* reach
Remix. Ranked suspects and the one-line experiment are in
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
