# GX → Direct3D 9 fixed-function translation spec

The D3D9 backend consumes `g_gxState` plus raw vertex streams at the
`command_processor.cpp` draw chokepoint and issues immediate D3D9 calls. This
file is the normative mapping: the conventions, the gotchas, and the places
where the translation is deliberately lossy. **What the backend cannot express
at all is in [`unsupported-effects.md`](unsupported-effects.md)**, and how what
it does emit is decoded on the far side is
[`remix-material-interface.md`](remix-material-interface.md).

**Scope.** This is a spec for what the backend *emits*, not for what the game
can render. The rasterized image is a feed into Remix, never a product, so
"approximated" here means the work moves into the fork — not that the effect is
lost. The two exceptions that must still rasterize correctly are the **HUD**
(§12) and **alpha** (§11, §9). Canonical statement:
[`remix-material-interface.md`](remix-material-interface.md) §0. Passages
written before 2026-08-04 sometimes still read as if the rasterized image were
the product; they are corrected as they are touched.

## 1. Device & frame

Plain HAL device, `D3DCREATE_HARDWARE_VERTEXPROCESSING | MULTITHREADED |
FPU_PRESERVE`, `D3DFMT_X8R8G8B8` backbuffer with `D3DFMT_D24S8` auto depth,
HWND from SDL3. `*UP` draws + `SetTransform` + `SetTexture` + fixed function is
the exact subset Remix intercepts.

Three arrangements that are not obvious from the code:

- **A resize recreates the device rather than calling `Reset`**
  (`recreate_device`), debounced by one stable frame, because Remix does not
  re-derive its UI overlay from a mid-run `Reset` — the HUD keeps the scale it
  had at device-creation size. `Reset` is still used for same-size device-loss
  recovery, where it is correct and much cheaper; before it, **unbind every
  texture**, since a resource still bound to the device survives our `Release`
  and makes `Reset` fail.
- **The backbuffer is the whole window; drawing targets a centred render rect**
  inside it, letterboxed to the game's aspect and cleared black, because the
  game lays its HUD out against `AuroraGetRenderSize`. `get_backbuffer_size`
  reports the render size; viewport, scissor and EFB-copy rects add the offset.
- One `Dx9State` cache mirrors every render/stage/sampler state actually set,
  so redundant sets are skipped.

## 2. Interception points

Everything is in `lib/gx/command_processor.cpp` (plus two sites in
`gfx/common.cpp` and one in `aurora.cpp`), gated on `aurora::dx9::active()`.
`draw_prim`, `GX_AURORA_DRAW_SIZED` and `GX_AURORA_DRAW_INDEXED` decode and
submit immediately — there is no merge layer, because the state cache makes
redundant state cheap. **The FIFO state decoding (BP/CP/XF handlers mutating
`g_gxState`) is shared verbatim with the wgpu path; zero changes.**

Two consequences that have each cost a session:

- **One `GXBegin` block is one D3D9 draw**, and Remix charges per draw. See
  `design-decisions.md` §"Draw count is the cost, not pixels".
- **The FIFO is drained in `end_frame`**, after the game thread has issued every
  draw, so a backend global set from the game thread describes the *last*
  material of the frame for *every* draw in it. Per-draw facts must travel as
  FIFO commands. `remix-material-interface.md` §11 has both measured failures.

## 3. Transforms

**Matrix convention.** Aurora `Mat4x4.m0..m3` are the *columns* of the
row-vector transform (`clip = v * M`), so **D3D element `_m[r][c] =
aurora.m<c>[r]`** — transpose m0..m3 into D3D columns. Aurora `Mat3x4` (XF pos
matrices) are GameCube row-major 3×4: the D3D 4×4 world is the transpose of the
3×3 part, with translation `(m0[3], m1[3], m2[3])` in row 3.

**Projection.** `g_gxState.proj` is the raw GX matrix, NDC z ∈ [−1,0]. Apply the
same correction the wgpu non-reversed path uses — **`m2 = m2 + m3`** → z ∈
[0,1], near = 0 — then transpose. **No reversed-Z on D3D9**: Remix prefers
conventional depth and `depthFunc` maps directly. `to_d3d_proj`
(`dx9_internal.hpp:307-317`) carries the same in a comment.

**Model-view / camera split.** GX pos matrices are model→view with no separate
camera; the game supplies one through `GXSetViewMtx`, and each draw then
uploads `WORLD = pnMtx · view⁻¹`, `VIEW = view`. `WORLD*VIEW == pnMtx` either
way, so rasterization is identical — the split exists so Remix can reconstruct a
camera. **A draw arriving without `GXSetViewMtx` is a defect to fix, not a
supported configuration**: Remix rejects `objectToView == objectToWorld` as
`CameraType::Unknown`, `finalizeSkinningData` never runs, and every skinned draw
inherits `WORLDMATRIX(0)`, scattering character parts. Ortho draws are excluded
(§12). Code and symptoms: `dx9_draw.cpp:305-320`.

**Normals — an open question, marked as inference.** D3D9 fixed function
transforms normals by the world matrix directly, with
`D3DRS_NORMALIZENORMALS = TRUE` for scale; GX's separate nrm matrix is dropped.
The original justification was "lighting is off, so normals only feed texgen",
and that is no longer the whole story: the submitted normals are **geometry**
and reach Remix's path tracer. **Whether the dropped nrm matrix visibly matters
there depends on non-uniform scale in TP's pos matrices, which is unmeasured.**
Do not record either answer as known.

## 4. Vertex decoding — `lib/dx9/dx9_vertex.*`

One interleaved vertex struct per draw in a scratch buffer, described by an
**FVF code** (not a vertex declaration — dxvk-remix does the FVF→declaration
conversion itself, and §6's `XYZB2|LASTBETA_UBYTE4` requirement is stated
against the FVF form). Fields in this order, present-only: position, blend
weights, blend indices, normal, diffuse, specular, uv0..uvN.

Four decode rules worth stating:

- **Diffuse when `GX_VA_CLR0` is absent** takes channel 0's `matColor` if
  `matSrc = GX_SRC_REG`, else opaque white (`defaultDiffuse`). Whether Remix is
  then *told* to use it is a separate per-draw decision — §8.
- **Indexed attributes** read a BE index from the stream and fetch from
  `arrays[attr]` honouring `stride` and `le`; this also covers J3D's
  `GX_AURORA_LOAD_ARRAYBASE` 64-bit host pointers.
- **Primitive conversion** reuses `prepare_idx_buffer` (quads/fans/strips →
  u16 triangle lists). Lines and points go out directly; GX line width is not
  representable.
- **Winding.** GX front face is clockwise after aurora's convention swap, and so
  is D3D9's default — but `D3DCULL_*` names the *culled* side, so
  `GX_CULL_BACK → D3DCULL_CCW` and `GX_CULL_FRONT → D3DCULL_CW`. `GX_CULL_ALL`
  skips the draw.

## 5. Texture objects & samplers — `lib/dx9/dx9_texture.*`

**The store is content-addressed** — dims/format/mips plus a 64-bit hash of the
source bytes (plus TLUT for palette formats) — with `texObjId` as a thin alias
layer so the hot path skips hashing. That is load-bearing for Remix and the
reason is at `dx9_texture.cpp:34-43`. Eviction (`GXDestroyTexObj`) drops only
the id alias; content entries age out after ~300 frames, swept every 32.

Conversion reuses `convert_texture` → RGBA8 → BGRA → `D3DFMT_A8R8G8B8` managed
texture, every mip uploaded. `GX_TF_BC1_PC` → `D3DFMT_DXT1` direct, `R8_PC` →
`D3DFMT_L8`, palette formats combined on the CPU. Sampler state comes from
`GXTexObj_` mode0/mode1.

**HD replacement packs change none of this.** The uploaded bytes are always the
game's own, deliberately — the D3D9 texture is what Remix hashes. The content
store gains one field, `ContentEntry::remixIndex`, resolved once per distinct
content rather than per draw. [`texture-replacements.md`](texture-replacements.md).

## 6. Skinning

Two cases, both expressed as fixed-function indexed vertex blending, which Remix
understands natively (rest-pose vertices hashed, bones replayed).

**(a) Matrix-palette draws (`PNMTXIDX` present)** — all normal characters. The
palette is compacted per draw and every blended draw stores at least one
explicit weight; both requirements, with the symptoms that forced them, are at
`dx9_draw.cpp:352-366`. The short form: `MaxVertexBlendMatrixIndex` is 8 against
GX's 10 slots, and dxvk-remix's `dispatchSkinning` early-outs with "draw call
has bones but no blend weight buffer" while still *classifying* the draw as
skinned — so the mesh renders disfigured rather than unskinned.
`XYZB2|LASTBETA_UBYTE4` decodes to FLOAT1 BLENDWEIGHT + UBYTE4 BLENDINDICES,
which satisfies it and is mathematically identical under real fixed function.

**(b) `GXSetSkinning` extension draws** — `J3DSkinDeform`, used by two actors in
the whole game. The palette is `jointCount` × 3×4 row-major floats loaded into
`D3DTS_WORLDMATRIX(0..n)`; per vertex, the *position index* selects
`influences[posIdx * influenceCount ...]`, `{u32 bone, f32 weight}` each. With a
camera, `skinBaseMtx · view⁻¹` is folded into every bone so the blend output is
world space — the object→world convention `finalizeSkinningData` assumes.
Without one, bones stay raw and `D3DTS_VIEW = skinBaseMtx`.

`TEXMTXIDX` (per-vertex texture matrix select) is consumed from the stream and
ignored; rare, and env-mapped skinned parts may look off.

## 7. Texgen (`tcgs`)

| GX texgen | D3D9 mapping |
|-----------|--------------|
| `GX_TG_MTX2x4`, src `GX_TG_TEX0-7`, mtx `GX_IDENTITY` | plain UV passthrough (`D3DTTFF_DISABLE`) |
| `GX_TG_MTX2x4`, src TEXn, mtx `GX_TEXMTX0-9` | `SetTransform(D3DTS_TEXTURE<stage>)`, `D3DTTFF_COUNT2` |
| `GX_TG_MTX3x4` + src `GX_TG_POS` | `D3DTSS_TCI_CAMERASPACEPOSITION` + texture matrix, `COUNT3`/`PROJECTED` as needed |
| `GX_TG_MTX2x4/3x4` + src `GX_TG_NRM` | `D3DTSS_TCI_CAMERASPACENORMAL` + texture matrix (env mapping) |
| post-transform matrix (`ptTexMtxs`) | folded into the same `D3DTS_TEXTUREn` matrix when the base allows |
| `GX_TG_SRTG` (colour→texcoord), emboss bump | **unsupported** |

**Camera-space texgen needs compensation**: GX texgen reads *model-space*
inputs where D3D9 gives view-space, so the texture matrix is premultiplied with
the per-draw model-view inverse (`dx9_draw.cpp:380`). Rigid draws only. The
GX normalize flag has no fixed-function equivalent; projected transforms do not
survive into Remix at all (`unsupported-effects.md` R6).

## 8. Colour channels & lighting — unlit by policy

`D3DRS_LIGHTING = FALSE`, because Remix replaces lighting wholesale and this
game bakes most world lighting into vertex colours. Mapping GX lights to
`D3DLIGHT9` would only improve the standalone image, which is never shown.

**The lighting-enable bit is read state, not discarded state**, and it decides
two different things:

- **Whether vertex colour is forwarded**, per draw, stated in
  `D3DMATERIAL9::Specular.r` — lighting enabled means the stream is authored
  material colour and a path tracer supplies the light; lighting disabled means
  it is the finished output, which is where this game bakes room light.
  `remix-material-interface.md` §7c.
- **Not** self-illumination. That is whether any TEV colour stage reads
  `GX_CC_RASC`/`RASA` at all, which is a different question and was the bug —
  the Goron Mines lava has GX lighting *enabled*. §9 there.

**One recorded divergence from GX.** This spec once said "if a CLR attribute
exists but `matSrc = GX_SRC_REG`, override with the register colour". That is
**not implemented** — `dx9_vertex.cpp` sets `defaultDiffuse` only when CLR0 is
absent — so such a draw forwards a stream colour GX would have ignored. Whether
TP ever emits that combination is **unmeasured**, and `matSrc=` is already on
the `matrep.sum` line, so it is a log question rather than a judgement call.

## 9. TEV → texture-stage states — `lib/dx9/dx9_tev.*`

> **Remix reconstructs a material from one texture stage, and anything it
> cannot decode resolves to identity (white) rather than to an error.** That
> single fact drives most of what this section emits, and
> [`remix-material-interface.md`](remix-material-interface.md) is authoritative
> for it. It is not repeated here: this section used to carry its own copy, and
> that copy was still describing the pre-2026-08-03 hint rule weeks after the
> code changed.

What belongs here, because it is about what we *emit*:

- A **hint stage** is prepended **only when Remix would otherwise read the stage
  wrongly**, writing TEMP so the real chain is untouched (§5 there).
- The texture is bound only on emitted stages that actually reference
  `D3DTA_TEXTURE` — Remix keeps two texture candidates per draw, so duplicates
  from a split stage crowd out a real second texture.
- Unused stages are disabled **and unbound**: Remix's scan skips texture-less
  stages rather than stopping at the first disabled one, so a leftover binding
  could win the albedo slot.

GX TEV computes `d ± (a*(1−c) + b*c) + bias, × scale` per colour and alpha with
arbitrary inputs; D3D9 computes `op(arg1, arg2 [, arg0])`. The strategy is to
canonicalize each stage (drop `a=b=ZERO`, `c=ZERO → d + a`, `c=ONE → d + b`)
and then match:

| TEV colour pass (a, b, c, d) | TSS |
|---|---|
| `(ZERO, TEXC, RASC, ZERO)` | `MODULATE(TEXTURE, DIFFUSE)` |
| `(ZERO, TEXC, ONE/KONST≈1, ZERO)` | `SELECTARG1(TEXTURE)` |
| `(ZERO, RASC, …, ZERO)` / PASSCLR | `SELECTARG1(DIFFUSE)` |
| `(ZERO, TEXC, KONST, ZERO)` | `MODULATE(TEXTURE, TFACTOR)` |
| `(TEXC/CPREV, TEXC, RASA, ZERO)` — terrain lerp by vertex alpha | `LERP(DIFFUSE\|ALPHAREPLICATE, TEXTURE, CURRENT)` |
| `(CPREV, TEXC, KONST, ZERO)` — UI/terrain lerp by konst | `LERP(TFACTOR, TEXTURE, CURRENT)` |
| `(CPREV, TEXC, TEXA, ZERO)` | `BLENDTEXTUREALPHA` |
| `(ZERO, CPREV, RASC, ZERO)` | `MODULATE(CURRENT, DIFFUSE)` |
| `(CPREV, ZERO, ZERO, TEXC)` and other additives | `ADD(TEXTURE, CURRENT)` |
| `d=CPREV`, `a/b/c=0` | stage disabled / `SELECTARG1(CURRENT)` |
| `(C0, C1, TEXC, ZERO)` — two-colour ramp, this game's dominant shape | **Reproduced exactly, not approximated.** Both endpoints are transmitted and the fork's shader computes `mix(rampLo, rampHi, albedo)`, which *is* GX's `a*(1−c)+b*c`. `remix-material-interface.md` §10 |

The alpha pass gets the same treatment on `D3DTSS_ALPHAOP/ARG1/ARG2`; a
`dstAlpha` override makes the final stage's alpha TFACTOR alpha when
`alphaUpdate`. The nine fallbacks, each logged once per distinct configuration:

1. **Bias / scale.** `×2`/`×4` → `MODULATE2X`/`4X` where the op is
   modulate-shaped, `ADDSIGNED` for bias −0.5; other combinations take the
   nearest op.
2. **Compare-mode ops** (`GX_TEV_COMP_*`) have no TSS equivalent and are
   approximated as always-true. **This is the standing suspect — inference, not
   a finding — for the torch-flame white circle**, which does reach Remix. If it
   is confirmed, the fix is to state the compare to the fork, not to hunt for a
   better TSS shape.
3. **Konst colours.** One shared `D3DRS_TEXTUREFACTOR` per draw, first konst
   wins, with per-stage constants where the runtime exposes `PERSTAGECONSTANT`
   (Remix/dxvk does). Conflicting multi-konst draws are logged.
4. **TEV output registers other than PREV.** TSS has only CURRENT. Stages that
   write REGn and never read it are treated as the PREV chain; true
   multi-register programs collapse with priority to the chain feeding PREV.
5. **Stage count** truncates at 8 from the tail. TP is ≤4 TEV stages nearly
   everywhere.
6. **Non-identity swap tables** map to `D3DTA_ALPHAREPLICATE` when the pattern
   is exactly that, and are otherwise ignored.
7. **Indirect texturing stages are ignored** — the base stage still draws, the
   warp is gone.
8. **Rasterized channel per stage**: COLOR0A0 → DIFFUSE, COLOR1A1 → SPECULAR,
   COLOR_ZERO → constant black.
9. **Multi-texture GX materials are approximated today**, since only one texture
   becomes the albedo. "Inherently" is what this line used to say; that is a
   claim about *stock* Remix, and the fork is ours.

## 10. EFB copies & offscreen passes

Colour-format copies are **real** — `StretchRect` from the current render target
into a `D3DPOOL_DEFAULT` render-target texture, honouring `texCopySrc` and the
logical→render scale. Depth-format copies and `StretchRect` failures register a
1×1 **white / alpha-0** placeholder, deliberately not opaque black, which
darkened every projected consumer (`dx9_texture.cpp:296-299`). `clear=true`
executes the GX clear semantics scoped to the `texCopySrc` rect.

**Copy targets are cached per (dest pointer, copy size)**, not per dest — the
bloom filter copies into the same guest buffer at two sizes every frame, and a
per-dest cache handed Remix a brand-new render target, with a fresh internal
hash, twice a frame forever (`dx9_texture.cpp:199-204`). Stale sizes age out
after ~600 frames. `begin/end_offscreen` uses real render-target and depth
surfaces cached by size. Mods are wgpu-only and disabled here.

## 11. Blend / depth / misc pixel state

| GX | D3D9 |
|----|------|
| `GX_BM_NONE` | `ALPHABLENDENABLE = FALSE` |
| `GX_BM_BLEND` | `SRCBLEND`/`DESTBLEND` direct table; DSTALPHA factors need alpha in the backbuffer, else approximated with ONE/ZERO and logged |
| `GX_BM_SUBTRACT` | `BLENDOP = REVSUBTRACT`, factors ONE/ONE |
| `GX_BM_LOGIC` | only `CLEAR/SET/COPY/NOOP`; others unsupported |
| `depthCompare/Func/Update` | `ZENABLE`, `ZFUNC` (direct enum map, no reversal), `ZWRITEENABLE` |
| `alphaCompare(comp0 op comp1)` | reduced to one D3D test where possible; irreducible dual tests use comp0 and log |
| `colorUpdate/alphaUpdate` | `D3DRS_COLORWRITEENABLE` mask |
| `dstAlpha` const | final-stage alpha via TFACTOR (§9) |
| fog | `FOGENABLE` + `FOGVERTEXMODE`; start/end solved from the decoded curve. **Not how the game's fog reaches Remix** — `dx9_draw.cpp:169-186` |
| `GX2SetPolygonOffset` | `DEPTHBIAS` / `SLOPESCALEDEPTHBIAS` |
| scissor / viewport | `SetScissorRect` / `D3DVIEWPORT9` in render coordinates |
| line / point size | `D3DRS_POINTSIZE`; D3D9 lines are 1px |

**Alpha is an exception to the scope note.** Remix builds opacity and the alpha
test from what this section and §9 emit, so an irreducible dual test collapsed to
`comp0` changes what reaches the path tracer, and a wrong alpha turns foliage
into solid quads. Treat those log lines as defects rather than curiosities.

## 12. UI path

**The HUD is one of the two things that must still rasterize correctly** —
Remix rasterizes UI draws rather than path-tracing them (`isRenderingUI` in
`d3d9_rtx.cpp`), so for these draws the 2D stage chain *is* the output. Nothing
special is needed for that: J2D emits ortho + no-Z + alpha-blended quads through
the same FIFO. Remix's own heuristics (ortho + no depth) detect them; the camera
split is skipped for exactly this reason (§3).

**One consequence is load-bearing.** Because a UI draw is rasterized it never
reaches material resolution, so *no* material-side mechanism can affect it — not
a USD replacement, and none of the `D3DMATERIAL9` side channels except the two
the HD texture pack added. That is why the pack substitutes in **two** places:
the material for path-traced draws, and `D3D9DeviceEx::BindTexture` for these.
Sharpening the HUD is not possible any other way.
[`texture-replacements.md`](texture-replacements.md) §3.

## 13. What Remix sees — the intentional choices

- One `IDirect3DDevice9`, single-threaded, `DrawIndexedPrimitiveUP`, no shaders,
  no MSAA, no sRGB states, no queries.
- `SetTransform` WORLD/VIEW/PROJECTION per draw, split by the game's real camera
  (§3). Without that feed Remix reconstructs no camera at all — a failure state
  to detect, not a mode to fall back to.
- Fixed-function skinning via `D3DTS_WORLDMATRIX(i)` + indexed blending, with an
  explicit BLENDWEIGHT stream on every blended draw (§6a). **Bone-space
  convention:** with the camera, bones are true object→world and `D3DTS_VIEW` is
  the real camera, which is exactly what `finalizeSkinningData` assumes, so
  world-space bone readouts are meaningful.
- `SetTexture(stage 0..n)`. **Which stage becomes the albedo is Remix's choice,
  not ours** — the mapper's job is to make the stage it picks readable, not to
  assume stage order decides. The one place aurora states an opinion is the HD
  pack's `Ambient.b`, and it reports what it observed rather than asserting a
  convention.
- Alpha-tested cutouts via ALPHATEST render states (Remix's "cutout" category).
- **`D3DMATERIAL9` is a side channel, not a lighting input.** `D3DRS_LIGHTING`
  is off, so the struct is inert to rasterization and the fork copies it into
  `LegacyMaterialData` (`set_remix_material`, `dx9_internal.hpp`). **The
  allocation table is `remix-material-interface.md` §2** and is checked
  mechanically in both directions — do not add a claim here without adding its
  §2 row in the same commit. This is "prefer stating over encoding" in practice:
  a per-draw fact transmitted directly beats one inferred from fixed-function
  state.
