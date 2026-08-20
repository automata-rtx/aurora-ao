# Why the D3D9 backend is shaped the way it is

The decisions in `lib/dx9/` that are expensive to rediscover, each with the
symptom that forced it. **This is not the state of the project** — that is
`dusklight-ao/docs/remix-open-issues.md` — and it is not a system description:
`gx-to-d3d9-mapping.md`, `remix-material-interface.md`, `material-report.md`,
`texture-replacements.md` and `unsupported-effects.md` own those.

Every decision below has a code comment carrying its full derivation. Those
comments are the authority; this page exists so you know which ones to open.
Start at `README.md` for orientation and the verification vocabulary.

**The one fact that changes how you read the rest:** the raw D3D9 image is a
feed into Remix, not a product. "Fixed function cannot express this" is a
statement about the transport, and the answer is to do the work in Remix. Full
statement: `remix-material-interface.md` §0.

## Camera and transforms

- **The game's real view matrix is forwarded** (`GXSetViewMtx` →
  `GX_AURORA_SET_VIEW_MTX`) and `apply_transforms` splits
  `WORLD = pnMtx · view⁻¹`, `VIEW = view`. Without it Remix's camera manager
  rejects every draw as `CameraType::Unknown`, skinning never runs, and body
  parts scatter. **The split is skipped for orthographic draws**, which Remix
  classifies as UI. Both, with their symptoms: `lib/dx9/dx9_draw.cpp:305-320`;
  see also `dx9.hpp:74-80` and `dx9_internal.hpp:85-90`.
- **Camera-space texgen is compensated** by premultiplying with the per-draw
  model-view inverse, because GX texgen reads model-space inputs where D3D9
  gives view-space. Rigid draws only — `dx9_draw.cpp:380`.
- The inverse is guarded by a determinant check that logs `camera view matrix
  not invertible` (`dx9_backend.cpp:761-764`). It has never fired.

## Skinning

- **The game was already GPU-skinning its characters, and that is a
  prerequisite, not an optimisation.** Normal J3D characters go through
  `calcWeightEnvelopeMtx` — a small per-draw matrix palette blended on the CPU,
  applied per vertex on the GPU via `PNMTXIDX` — so what reaches this backend is
  rest-pose vertices plus a transform, which is exactly what Remix hashes and
  replays. CPU per-vertex deform (`J3DSkinDeform`) is the exception, used by
  **two** actors in the whole game (`d_a_door_boss.cpp:66`,
  `d_a_demo00.cpp:213`), and dusklight offloads even those
  (`src/dusk/gpu_skinning.cpp`, hooked at
  `libs/JSystem/src/J3DGraphAnimator/J3DModel.cpp:461-464`). **A change that
  bakes skinning on the CPU to simplify the stream destroys per-asset
  replacement for characters**, because the mesh hash then changes every frame.
- **Every blended draw stores at least one explicit weight, and the matrix
  palette is compacted per draw**, with overflow draws split into per-palette
  groups. Remix's skinning pass early-outs with no blend-weight buffer; GX's
  10-deep palette overruns the device's 8-index cap, which Remix does not
  enforce — so that bug hid for weeks behind correct-looking Remix output.
  `dx9_draw.cpp:352-366`, and `unsupported-effects.md` R5.

## Textures

- **The texture store is content-addressed** — dims/format/mips plus a hash of
  the source bytes — with `texObjId` as an alias layer, because Remix holds
  references to D3D9 texture *objects* across frames while the game builds a
  stack-local `GXTexObj` per draw. Keying on the id made Remix's texture list
  churn and VRAM grow without bound. `dx9_texture.cpp:34-43`.
- **EFB copy targets are keyed by (dest, size)**, because the bloom chain copies
  into the same guest buffer at two sizes every frame and a destroy/recreate per
  size gave every target a fresh Remix hash forever. `dx9_texture.cpp:199-204`.
- Pointers are therefore stable for the life of the device, which is what makes
  them usable as the join key in `material-report.md`.

## Materials

**`remix-material-interface.md` is authoritative and this page deliberately does
not summarise it.** The four decisions it records — the conditional hint stage,
evaluating the albedo instead of pattern-matching it, forwarding vertex colour
only where GX says it is material rather than baked light, and self-illumination
as a rule rather than a score — are each stated there with their measurements
and their code sites. Read §5, §7, §9 and §10 before changing anything in
`dx9_tev.cpp`.

## Render targets, window and resize

- **EFB colour copies and offscreen passes are real** (`StretchRect` /
  `SetRenderTarget`). Depth-format copies remain a **white / alpha-0**
  placeholder (`0x00FFFFFF`), deliberately not opaque black, which darkened
  every projected consumer. `dx9_texture.cpp:296-299`.
- **A size change recreates the device rather than resetting it**, debounced by
  one stable frame, because Remix does not re-derive its UI overlay from a
  mid-run `Reset`. The visible black pause is the texture cache rebuilding.
  `unsupported-effects.md` R4.
- **The backbuffer is native-sized with a letterboxed render rect inside it**,
  because the game lays out its HUD against the render size.

## Fog

`apply_fog_state()` decodes the BP fog curve back to view-space start/end and
sets `D3DRS_FOG*` per draw. **It is not how the game's fog reaches Remix** —
Remix keeps the first non-NONE fog state of a frame and TP sets fog per object,
so the fork drives fog from kankyo state instead. Derivation, that caveat and
the range-adjust omission: `dx9_draw.cpp:169-186`.

## Integration with the game

- **GX lighting is never evaluated** (`D3DRS_LIGHTING = FALSE`), which is why
  *every* light in this game reaches Remix through the light API rather than
  being captured — the sun/moon as a distant light, fire and glow as sphere
  lights placed at the origin of the effect that draws them
  (`dusklight-ao/docs/effect-lights.md`). The GX "this channel is unlit" bit is
  not discarded: it decides whether vertex colour is forwarded and is one signal
  in the self-illumination rule. It is not sufficient alone — most of a scene is
  unlit and the Goron Mines lava is *lit*.
- **Dawn is never initialized**, so every path that calls it unconditionally
  needs a `dx9::active()` guard. Two were found crashing at startup: ImGui
  texture creation and `resize_swapchain`.
- **Mods are skipped entirely** — their graphics stages are inert without WebGPU
  and a native mod that touches the renderer crashes at load.
- `aurora_dx9_get_device()` exposes the live device for dusklight's
  `dxvk_RegisterD3D9Device`. The pointer changes on resize-recreation, so
  callers poll it.
- **`dx9: device created …` in the log is the signature that the backend is
  actually active** (`dx9_backend.cpp:274`). More than one "it still crashes"
  report turned out to be a run that never selected D3D9.

## Draw count is the cost, not pixels

**Remix charges per draw.** A draw too small for its own BLAS is merged into a
shared bucket but still contributes its own geometry entry and surface, and that
bucket rebuilds whenever anything in it moves. This backend submits each
`GXBegin` block as its own `*UP` call — there is no batching layer — so a
game-side loop that wraps each quad in its own `GXBegin`/`GXEnd` costs one Remix
draw per quad. The kankyo weather emitters did exactly that, up to ~1000 draws a
frame, and rain and snow were unplayable under Remix until they were batched
game-side.

**Tested in game 2026-08-08: the owner reports particle performance "far better
than previously." Read that precisely — the outcome is tested, the counter is
not.** The `dx9.draws` figures were never reported back, so the predicted
collapse to one draw is confirmed by its effect rather than by measurement.
Anyone with a log from a rainy Hyrule Field session can close that gap.

**Writing a dense emitter that batches** is a game-side question with a
game-side answer: `dusklight-ao/src/d/d_kankyo_rain.cpp:6004-6320` carries the
whole recipe — `GXBegin(..., GX_AUTO)` around the loop, per-particle colour
moved out of `GX_TEVREG0` into vertex `CLR0` because a register write cannot
cross a `GXBegin` block, and `KyrRunBatch` for the two emitters that need two
colours per particle and therefore cannot do that (`:6287`).

**Do not generalise this into "batching is good".** Batching destroys asset-hash
identity where a stable one exists — see the grass entry in
`unsupported-effects.md`, which is the opposite case and says why.

`dx9.draws` (`lib/dx9/dx9_draw.cpp`) counts every `DrawPrimitiveUP` /
`DrawIndexedPrimitiveUP` the backend issues and logs mean and peak periodically;
`peak` is carried separately because a spike that only exists while the weather
runs is invisible in a mean. `material-report.md` explains the line.

**Two owner observations that did the diagnostic work, and both are reusable:**
marking the textures as particles in Remix's categorization UI **changed
nothing** — that category picks which TLAS a draw lands in, not what it costs —
and tagging the same texture as **UI made it smooth**, because UI draws never
enter the raytraced scene. As a diagnostic that pair is sharp: if UI-tagging
fixes the frame rate, the answer is draw count. As a fix it is not free.
