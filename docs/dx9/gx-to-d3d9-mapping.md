# GX → Direct3D 9 fixed-function translation spec

The D3D9 backend consumes `g_gxState` + raw vertex streams at the
`command_processor.cpp` draw chokepoint and issues immediate D3D9 calls.
This file is the normative mapping. Sections marked **[v1]** are in scope for
the first bring-up; **[later]** items are follow-ups; anything the D3D9 stream
cannot express lives in `unsupported-effects.md` — which is a list of where the
work moves to, not of what was abandoned (scope note below).

> **Scope note, 2026-08-04.** This is a spec for *what the backend emits*, not a
> statement of what the game can render. The rasterized D3D9 image is never
> shown to a player: Remix's renderer is the product, D3D9 is the feed. So
> every "unsupported"/"approximated" below means **the work moves into the fork
> or the Remix API** — all three repos are ours — not that the effect is lost;
> and raw-D3D9 correctness is a safety property, never a requirement. The two
> exceptions that must still rasterize correctly are the **HUD** (§12) and
> **alpha** (§11, §9). Canonical statement:
> [`remix-material-interface.md`](remix-material-interface.md) §0. Passages
> written before that date sometimes still read as if the rasterized image were
> the product; they are corrected as they are touched.

## 1. Device & frame [v1]

- `Direct3DCreate9(D3D_SDK_VERSION)` → `CreateDevice(D3DADAPTER_DEFAULT,
  D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING |
  D3DCREATE_MULTITHREADED | D3DCREATE_FPU_PRESERVE, &pp)`.
  - HWND from SDL3: `SDL_GetPointerProperty(SDL_GetWindowProperties(win),
    SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr)`.
  - Present params: `D3DSWAPEFFECT_DISCARD`, backbuffer `D3DFMT_X8R8G8B8`,
    `AutoDepthStencilFormat = D3DFMT_D24S8`, windowed, vsync from config
    (`D3DPRESENT_INTERVAL_ONE/IMMEDIATE`). Backbuffer size = window client
    size (`native_fb_*`).
  - **Resize → full device recreation, not `Reset`** (`recreate_device`).
    RTX Remix does not re-derive its UI overlay from a mid-run `Reset`: the
    HUD keeps the scale and placement it had at device-creation size, so every
    resized run had a mis-scaled HUD while launching straight into the final
    resolution was correct. Raw D3D9 follows a `Reset` correctly either way, so
    this is purely a Remix workaround. Recreation rebuilds the texture cache
    (every cached texture belongs to the outgoing device) and restarts Remix's
    renderer, so it is **debounced by one stable frame** — a resize drag would
    otherwise recreate every frame. A failed recreation retries each frame
    instead of going dark; a minimized window (0x0) skips the frame.
  - `Reset` is still used for **same-size device-loss recovery** (alt-tab),
    where it is correct and much cheaper. Before Reset: end any offscreen
    pass, end the scene, **unbind every texture** (a resource still bound to
    the device survives our Release and makes Reset fail), then release all
    pool-default resources; managed textures survive.
  - **Render rect (letterbox).** The backbuffer is the full window, but
    drawing targets a centered rect of `AuroraWindowSize::fb_*`
    (`renderWidth/Height` + `renderOffsetX/Y`) — the same size the game lays
    its HUD out against via `AuroraGetRenderSize`, and what the viewport
    policy letterboxes to the game's aspect. `get_backbuffer_size` reports the
    render size, viewport/scissor/EFB-copy rects add the offset, and the bars
    are cleared to black. Rendering into the raw window instead stretched the
    image and desynced HUD placement.
  - **Remix note:** plain HAL device + `*UP` draws + `SetTransform` +
    `SetTexture` + fixed-function is the exact subset Remix intercepts.
- Frame: `begin_frame` → handle device-lost (`TestCooperativeLevel`),
  `Clear(D3DCLEAR_TARGET|ZBUFFER)` using `g_gxState.clearColor/clearDepth`,
  `BeginScene`. `end_frame` → drain FIFO (existing call), `EndScene`,
  `Present`. GX's own EFB clear semantics arrive via copy-clear
  (`copy_tex(clear=true)`) — see §10.
- One global `Dx9State` cache mirrors every render/texture-stage/sampler state
  and transform actually set, so redundant `SetRenderState` etc. are skipped
  (draw merging from `draw_prim` stays enabled via `stateDirty` exactly as on
  the wgpu path).

## 2. Interception points [v1]

All in `lib/gx/command_processor.cpp` (plus 2 in `gfx/common.cpp`, 1 in
`aurora.cpp`), gated on `aurora::dx9::active()`:

| Site | wgpu behavior | d3d9 behavior |
|------|---------------|---------------|
| `draw_prim` | push raw verts + build index buf + merge + `push_gx_draw` | `dx9::draw_prim(prim, fmt, count, ptr)` — decode + draw immediately (no merge needed; state cache makes redundant state cheap) |
| `GX_AURORA_DRAW_SIZED` | same as draw | same decode path |
| `GX_AURORA_DRAW_INDEXED` | pre-built u16 indices | decode verts once, `DrawIndexedPrimitiveUP` with given indices |
| `GX_AURORA_SET_SKINNING` | upload palette/influences to storage buffer | stash raw pointers + counts in `dx9::SkinState` |
| `set_viewport` / `set_scissor` (via `gfx::set_*`) | record commands | `SetViewport` / `SetScissorRect` immediately |
| `copy_tex` (BP 0x52) | resolve pass into texture | §10 |
| `begin/end_offscreen` | offscreen pass | §10 |
| `aurora.cpp` init/frame/shutdown | webgpu | dx9 equivalents; skip `gfx::initialize`, imgui, rmlui (v1) |

The FIFO **state** decoding (BP/CP/XF handlers mutating `g_gxState`) is shared
verbatim — zero changes.

## 3. Transforms [v1]

Matrix convention: aurora `Mat4x4.m0..m3` are the columns of the row-vector
transform (`clip = v * M`), which means **D3D matrix element `_m[r][c] =
aurora.m<c>[r]`** (transpose m0..m3 into D3D columns). aurora `Mat3x4` (XF pos
matrices) are GameCube row-major 3x4: D3D 4x4 world = transpose of 3x3 part,
translation (m0[3], m1[3], m2[3]) into row 3, last column (0,0,0,1).

- **Projection:** `g_gxState.proj` is the raw GX matrix (NDC z ∈ [-1,0]).
  Apply the same correction the wgpu non-reversed path uses
  (`shader_info.cpp:408`): `m2 = m2 + m3` → z ∈ [0,1], near=0. Then transpose
  to D3D layout, `SetTransform(D3DTS_PROJECTION)`. **No reversed-Z on D3D9**
  (Remix prefers conventional depth; `depthFunc` maps directly).
- **Model-view / camera split:** GX pos matrices are model→view (no separate
  camera). The game provides the camera via **`GXSetViewMtx`
  (`GX_AURORA_SET_VIEW_MTX`)** — dusklight calls it from `J3DSys::setViewMtx`,
  the funnel for every view change. When the camera is valid
  (`dx9::g_camera`), each draw uploads `D3DTS_WORLD = pnMtx * view⁻¹` (true
  model→world) and `D3DTS_VIEW = view`; when absent, `WORLD = pnMtx`,
  `VIEW = identity`. `WORLD*VIEW == pnMtx` either way, so rasterization is
  identical — the split exists purely so RTX Remix can reconstruct a camera
  and stable world space. **This is load-bearing for Remix:** its camera
  manager rejects draws whose `objectToView == objectToWorld` (exactly the
  fused/identity-view shape) as `CameraType::Unknown`; with no valid camera,
  `finalizeSkinningData` never rewrites skinned instance transforms and each
  skinned draw inherits `WORLDMATRIX(0)` — a different joint matrix per J3D
  shape packet — scattering character parts. Per draw:
  - No PNMTXIDX attribute: `D3DTS_WORLD = pnMtx[currentPnMtx].pos * view⁻¹`.
  - With PNMTXIDX (matrix-palette draws): §6.
  - `GXSetSkinning` active: §6 (base matrix folded into bones; VIEW = camera).
- **Normals:** D3D9 FF transforms normals by inverse-transpose of world
  automatically? — No: it uses the world matrix directly and
  `D3DRS_NORMALIZENORMALS = TRUE` handles scale. GX's separate nrm matrix is
  dropped. Documented. The original justification — "lighting is OFF in v1
  (§8), so normals only feed texgen" — is a statement about D3D9's own T&L and
  is no longer the whole story: the submitted normals are **geometry** and
  reach Remix's path tracer. Whether the dropped nrm matrix visibly matters
  there depends on non-uniform scale in TP's pos matrices, which is
  **unmeasured** — do not record either answer as known.

## 4. Vertex decoding [v1] — `lib/dx9/dx9_vertex.*`

Input: raw vertex stream (BE from game DLs / LE per `arrays[i].le` for indexed
attrs), `vtxDesc`, `vtxFmts[fmt]`, `arrays` (base ptr + stride). Mirrors the
offset walk of `populate_pipeline_config` (`gx.cpp:683`) and
`calculate_last_vtx_size`.

Output: one interleaved FF vertex struct per draw, built in a scratch buffer,
described by an **FVF code** (`SetFVF`; built in `dx9_vertex.cpp`, set in
`dx9_internal.hpp`) using only FF-legal usages (POSITION, BLENDWEIGHT,
BLENDINDICES, NORMAL, COLOR0, COLOR1, TEXCOORD0-7). Corrected 2026-08-04: this
section specified `CreateVertexDeclaration`; the shipped code uses FVF, and FVF
is the form §6a's `XYZB2|LASTBETA_UBYTE4` requirement is stated against —
dxvk-remix does the FVF→declaration conversion itself. Layout (present-only
fields, in this order):

| Field | Type | Source |
|-------|------|--------|
| position | FLOAT3 | GX_VA_POS: u8/s8/u16/s16 × 2^-frac → float; XY formats get z=0 |
| blend weights | FLOAT1-3 | skinning only (§6): weights 0..n-1 (last implied) |
| blend indices | UBYTE4 (D3DDECLUSAGE_BLENDINDICES) | PNMTXIDX/3 or skin influence bones |
| normal | FLOAT3 | GX_VA_NRM (NBT: take N, drop B/T; frac per type: s8=6, s16=14) |
| diffuse | D3DCOLOR | GX_VA_CLR0: RGB565/RGB8/RGBX8/RGBA4/RGBA6/RGBA8 → BGRA u32; **absent → channel 0's `matColor` when `matSrc = GX_SRC_REG`, else opaque white** (`dx9_vertex.cpp` `defaultDiffuse`). Whether Remix is then told to *use* that colour is a separate, per-draw decision — §8 |
| specular | D3DCOLOR | GX_VA_CLR1 (rare; COLOR1 for channel 1) |
| uv0..uvN | FLOAT2 (FLOAT3 for projected/STQ inputs when needed) | GX_VA_TEXn: u8/s8/u16/s16 × 2^-frac, f32 direct |

- Indexed attributes (`GX_INDEX8/16`): read index from stream (BE), fetch from
  `arrays[attr]` honoring `stride` and `le`. This also covers J3D's
  `GX_AURORA_LOAD_ARRAYBASE` 64-bit host pointers.
- Texcoord count: emit exactly the attrs present; `tcgs` referencing
  TEX0-7 sources map to those slots (§7). Number of active stage coordinates =
  `numTexGens`.
- Primitive conversion reuses the existing `prepare_idx_buffer` triangulation
  (quads/fans/strips → triangle lists, u16). Draws go out as
  `DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, vtxCount, numIndices/3, idx,
  D3DFMT_INDEX16, verts, stride)` (or `DrawPrimitiveUP` for plain triangles).
- `GX_LINES/LINESTRIP/POINTS`: emitted as `D3DPT_LINELIST/LINESTRIP/POINTLIST`
  directly (FF supports them); GX line-width emulation is **[later]**
  (wgpu path expands to quads; lines are debug-ish in TP).
- Winding/cull: GX front face is **clockwise** after aurora's convention swap
  (`to_primitive_state`: FrontFace::CW). D3D9 default front = CW too, so:
  `GX_CULL_BACK → D3DCULL_CCW`? No — D3DCULL_* names the culled side:
  cull back (CCW is back when front=CW) → `D3DCULL_CCW`; `GX_CULL_FRONT` →
  `D3DCULL_CW`; `GX_CULL_NONE/ALL` → `D3DCULL_NONE` (ALL: skip draw entirely).

## 5. Texture objects & samplers [v1] — `lib/dx9/dx9_texture.*`

- **Content-addressed store (load-bearing for RTX Remix).** D3D9 textures are
  owned by a map keyed on dims/format/mips + a 64-bit hash of the source
  bytes (+ TLUT bytes/format for palette formats). `texObjId` entries are a
  thin alias layer on top (id + data/TLUT versions → content key) so the
  per-draw hot path skips hashing. Rationale: Remix identifies game textures
  by content hash and holds references to the D3D9 texture *objects* across
  frames, but dusklight recreates `GXTexObj` wrappers freely — the `dDlst_2D*`
  drawlist items (minimap etc.) build a stack-local texobj **per draw, every
  frame**, each with a fresh `texObjId` that the PC `GXTexObjRAII` wrapper
  evicts right after the draw. With the old `texObjId`-keyed cache that meant
  one D3D9 texture created + destroyed per draw per frame: Remix's texture
  list churned ("textures spamming in and out" in the categorize-textures
  tab) and recreated objects piled up VRAM without bound. With the content
  store, an identical re-init resurrects the **same** D3D9 texture object.
- Eviction: `GXDestroyTexObj` (`evict_texture_object`) only drops the id
  alias; the D3D9 texture stays in the content store. Unused content entries
  age out after ~300 frames (`sweep_caches`, every 32 frames), which also
  bounds genuinely dynamic content (palette animations cycle through a small
  set of stable entries instead of recreating textures).
- Conversion: reuse `convert_texture` (`texture_convert.cpp`) → RGBA8 buffer
  (all GC formats incl. CMPR→RGBA8) → swizzle to BGRA → `D3DFMT_A8R8G8B8`
  managed-pool texture, upload every mip (`LockRect` per level).
  `GX_TF_BC1_PC` → `D3DFMT_DXT1` direct upload. `R8_PC` → `D3DFMT_L8`.
- Palette formats (C4/C8/C14X2): CPU combine — decode indices, apply
  `convert_tlut` output (RGBA8 palette) at conversion time, key includes TLUT
  version. (wgpu does this on the GPU; CPU is fine at TP texture sizes.)
- Sampler state from `GXTexObj_` mode0/mode1 fields (decoded accessors on the
  struct): wrap S/T → `D3DSAMP_ADDRESSU/V` (CLAMP/REPEAT/MIRROR); min/mag
  filters → `D3DTEXF_POINT/LINEAR` (+ `MIPFILTER` from mipmap flag,
  LOD bias via `D3DSAMP_MIPMAPLODBIAS`, max aniso from config).
- EFB-copy textures: separate map keyed by dest pointer + copy size (§10).

## 6. Skinning [v1 — flagship feature]

Two cases, both expressed as **fixed-function indexed vertex blending**, which
RTX Remix understands natively (rest-pose verts hashed, bones replayed):

**(a) Matrix-palette draws (PNMTXIDX attribute present)** — all normal
characters:
- Load `g_gxState.pnMtx[…].pos` (× `view⁻¹` when the camera is set — §3) into
  `SetTransform(D3DTS_WORLDMATRIX(i))` (only when dirty).
- **The palette is compacted per draw.** GX has 10 position matrices, but
  fixed-function indexed blending only reaches
  `D3DCAPS9::MaxVertexBlendMatrixIndex` — **8** on the reference hardware — and
  an out-of-range blend index reads an undefined matrix, scattering every
  vertex that uses `GX_PNMTX9` to the same wrong place. So `decode_draw`
  assigns blend indices in first-use order and records the GX slot behind each
  (`DecodedDraw::pnMtxSlots/pnMtxCount`), and only those matrices are uploaded.
  Excess beyond the cap folds onto slot 0 with a one-shot warning.
  **Remix never showed this**: it ignores the cap, reading the world-matrix
  state directly and skinning on the GPU — the bug is raw-D3D9 only, which is
  why nothing under Remix ever surfaced it. The compaction stays because it is
  free and keeps the standalone image usable for debugging, not because that
  image is a product.
- Vertex gets `BLENDINDICES = UBYTE4(compactedIndex, 0,0,0)` **and one stored
  weight of 1.0**; `D3DRS_VERTEXBLEND = D3DVBF_1WEIGHTS` +
  `D3DRS_INDEXEDVERTEXBLENDENABLE = TRUE`. Plain fixed-function would accept
  the terser `D3DVBF_0WEIGHTS` with indices alone (one matrix, implicit
  weight 1), but **dxvk-remix requires a BLENDWEIGHT vertex element**: its
  `RtxGeometryUtils::dispatchSkinning` early-outs with "draw call has bones
  but no blend weight buffer" when `blendWeightBuffer` is undefined, while
  the draw is still *classified* as skinned — the GPU skinning pass never
  runs and the mesh renders disfigured. `XYZB2|LASTBETA_UBYTE4` decodes to
  FLOAT1 BLENDWEIGHT + UBYTE4 BLENDINDICES in dxvk's FVF→declaration
  conversion, satisfying it. The 1WEIGHTS form is mathematically identical
  under real fixed-function: the implicit second weight is 1−1.0 = 0, so the
  padding bone (index byte 1 = 0) contributes nothing, and Remix's skinning
  shader likewise skips bones with weight ≤ 0.
- TEXMTXIDX attributes (per-vertex texture matrix select) are consumed from
  the stream but **[later]** (rare; env-mapped skinned parts may look off).

**(b) `GXSetSkinning` extension draws (J3DSkinDeform, 2 actors):**
- Palette pointer = `jointCount` × 3x4 row-major host-endian floats → load
  into `D3DTS_WORLDMATRIX(0..jointCount-1)` (jointCount ≤ 256; TP models are
  far below this — assert + fall back to rigid if exceeded).
- Per vertex: position is indexed; use the **position index value** to fetch
  `influences[posIdx * influenceCount .. +influenceCount]` ({u32 bone,
  f32 weight}); emit `influenceCount-1` weights (D3D infers the last) +
  UBYTE4 indices; `D3DRS_VERTEXBLEND = D3DVBF_{1,2,3}WEIGHTS` by count;
  indexed enable TRUE. At least one weight is always stored (a hypothetical
  single-influence stream stores its 1.0 weight explicitly) for the same
  Remix blend-weight-buffer requirement as (a).
- Base matrix handling: with a camera (§3), `skinBaseMtx * view⁻¹` is folded
  into every bone (`W(i) = bone(i) * base * view⁻¹`, `VIEW = view`) so the
  blend output is world-space — the object→world convention Remix's
  `finalizeSkinningData` assumes. Without a camera, bones stay raw and
  `D3DTS_VIEW = skinBaseMtx` (model→view applied after the blend — matches
  the WGSL `skin_base_mtx` exactly since FF computes `v*W(i)` then `*VIEW`).
  VIEW is reset per draw either way.
- Device must report `MaxVertexBlendMatrixIndex ≥ 8` with HW T&L (all Remix
  targets do; if a real cap issue appears, fall back to
  `D3DCREATE_MIXED_VERTEXPROCESSING` + software vertex processing for skinned
  draws, which lifts the palette to 256 — decide at bring-up).

## 7. Texgen (`tcgs`) [v1: common cases]

| GX texgen | D3D9 mapping |
|-----------|--------------|
| `GX_TG_MTX2x4`, src `GX_TG_TEX0-7`, mtx `GX_IDENTITY` | plain UV passthrough (`D3DTTFF_DISABLE`) |
| `GX_TG_MTX2x4`, src TEXn, mtx `GX_TEXMTX0-9` | `SetTransform(D3DTS_TEXTURE<stage>)` = texMtx (2x4 → 4x4), `D3DTTFF_COUNT2` |
| `GX_TG_MTX3x4` + src `GX_TG_POS` | `D3DTSS_TCI_CAMERASPACEPOSITION` + texture matrix (view-space pos ≡ GX pos-matrix output), `COUNT3`/`PROJECTED` as needed |
| `GX_TG_MTX2x4/3x4` + src `GX_TG_NRM` | `D3DTSS_TCI_CAMERASPACENORMAL` + texture matrix (env mapping) |
| post-transform matrix (`ptTexMtxs`) | fold into the same `D3DTS_TEXTUREn` matrix (pre-multiply) when the base allows; else document |
| `GX_TG_SRTG` (color→texcoord), emboss bump | **unsupported** (list) |

Normalize flag: unsupported in FF (used by env-map paths; minor distortion —
documented).

## 8. Color channels & lighting [v1 = unlit]

TP's material colors flow through `colorChannelConfig/State`. v1 policy —
**lighting off** (`D3DRS_LIGHTING = FALSE`), because Remix replaces lighting
wholesale and the game bakes most world lighting into vertex colors:

- Channel with `matSrc = GX_SRC_VTX`: the vertex color is already in the
  stream, but **whether Remix is told to use it as material colour is decided
  per draw, from that channel's lighting enable** (2026-08-04, CI-green,
  untested in game). Lighting **enabled** ⇒ the stream is authored material
  colour that GX would multiply by computed lighting ⇒ **forward it**, and a
  path tracer supplies the light. Lighting **disabled** ⇒ the stream is the
  finished channel output, which is where this game bakes room lighting and
  shadow ⇒ **withhold it**. Aurora states the verdict per draw in
  `D3DMATERIAL9::Specular.r`; the fork sets `isVertexColorBakedLighting` from
  it rather than from the global option, which was necessarily wrong for one of
  the two cases. `vtxUse=` in `matrep.sum` prints which applied. The rule and
  both corrections that produced it: `remix-material-interface.md` §7c.
  Superseded 2026-08-04: an intermediate revision stopped advertising vertex
  colour at all. That was too blunt and threw away real material colour.
- Channel with `matSrc = GX_SRC_REG`: bake `colorChannelState[i].matColor`
  into the per-vertex diffuse at decode time **when no CLR attribute exists**.
  Colour arriving this way is authored by definition, so it is **evaluated as
  material colour** — `eval_operand` used to treat `DIFFUSE` as unconditionally
  white, which erased it.
  This spec also said "if a CLR attribute exists but matSrc=REG, override with
  the register color". **That is not implemented** (`dx9_vertex.cpp` sets
  `defaultDiffuse` only when CLR0 is absent), so such a draw forwards the
  stream colour that GX would have ignored. Whether TP ever emits that
  combination is **unmeasured** — `matSrc=` is already on the `matrep.sum` line,
  so it is a log question, not a judgement call. Recorded rather than silently
  dropped, because it is a divergence from GX, not a decision.
- `lightingEnabled` channels: v1 approximates as
  `matColor * (ambColor + Σ enabled lights…)` → **just `matColor`**, i.e.
  fullbright material color (documented). **[later]**: map GX lights to
  `D3DLIGHT9` point/spot/directional with attenuation (FF supports the math
  closely: GX cosAtt/distAtt ↔ D3D attenuation0/1/2 approximately) and enable
  `D3DRS_LIGHTING` per draw. That would only improve the standalone (non-Remix)
  image, which is never shown — so it earns its **[later]** on its own merits,
  not as a deferred obligation.
- The lighting-enable bit is **read state, not discarded state**, but it is
  *not* the self-illumination signal. What is: whether any TEV colour stage
  reads `GX_CC_RASC`/`RASA` at all (`remix-material-interface.md` §9). Measured
  2026-08-04: the Goron Mines lava has GX lighting **enabled**, so "lighting
  off" is not a proxy for "emitter" — the first emissive rule required it and
  missed the lava.

## 9. TEV → texture-stage states [v1] — `lib/dx9/dx9_tev.*`

> **Remix constraint (load-bearing).** Remix reconstructs a draw's material from
> **one** texture stage, and anything it cannot decode resolves to *identity*
> (white) rather than to an error. That single fact drives most of what this
> section emits.
>
> **`remix-material-interface.md` is authoritative for it** — what Remix reads,
> which ops and args survive, why the hint stage below is *conditional*, and the
> design rules that follow. It is not repeated here: this section used to carry
> its own copy, and that copy was still describing the pre-2026-08-03 hint rule
> weeks after the code changed.
>
> What belongs here, because it is about what we *emit*:
> - A **hint stage** (`color = MODULATE(TEXTURE, DIFFUSE)`,
>   `alpha = SELECTARG1(TEXTURE)`) is prepended **only when Remix would
>   otherwise read the stage wrongly**. It writes TEMP so the real chain is
>   untouched, making it raster-neutral wherever it lands. That neutrality is
>   load-bearing for the **HUD**, which Remix rasterizes rather than
>   path-traces; elsewhere it is a convenience the design is free to spend —
>   `remix-material-interface.md` §5 records that a future revision may write
>   the intended material straight into stage 0, which would also free TFACTOR
>   and stop the §10 ramp being declined as `ramp=tfTaken`.
> - The texture is bound only on emitted stages that actually reference
>   `D3DTA_TEXTURE` — Remix keeps two texture candidates per draw, so
>   duplicates from a split stage crowd out a real second texture.
> - Unused stages are disabled **and unbound**: Remix's scan skips
>   texture-less stages rather than stopping at the first disabled one, so a
>   leftover binding could win the albedo slot.
> - Multi-texture GX materials are approximated **today**, since only one
>   texture becomes the albedo. "Inherently" is what this line used to say; it
>   is a claim about *stock* Remix, and the fork is ours — §10 of
>   `remix-material-interface.md` is the precedent for checking whether such a
>   constraint is real before designing around it.

The heart of the fixed-function mapping. GX TEV per stage computes
`d ± (a*(1-c) + b*c) + bias, × scale` per color and alpha with arbitrary
inputs; D3D9 TSS computes `op(arg1, arg2 [, arg0])` per stage. Strategy:
**pattern-match each TEV stage into the closest TSS op**, with honest
fallbacks, all through a small normalizer:

1. Canonicalize the stage (drop no-op components: `a=b=ZERO`,
   `c=ZERO → result = d + a`, `c=ONE → d + b`, etc.).
2. Try exact patterns (covers the overwhelming majority of TP materials,
   confirmed by the J2D/J3D research):

| TEV pattern (color pass a,b,c,d + op ADD) | TSS |
|---|---|
| `(ZERO, TEXC, RASC, ZERO)` | `MODULATE(TEXTURE, DIFFUSE)` |
| `(ZERO, TEXC, ONE/KONST≈1, ZERO)` / passthrough tex | `SELECTARG1(TEXTURE)` |
| `(ZERO, RASC, …, ZERO)` / PASSCLR | `SELECTARG1(DIFFUSE)` |
| `(ZERO, TEXC, KONST, ZERO)` | `MODULATE(TEXTURE, TFACTOR)` (konst → TFACTOR) |
| `(TEXC/CPREV, TEXC, RASA, ZERO)` — terrain/texture lerp by vertex alpha | `LERP(arg0=DIFFUSE_ALPHA→D3DTA_DIFFUSE|ALPHAREPLICATE, TEXTURE, CURRENT)` |
| `(CPREV, TEXC, KONST, ZERO)` — UI/terrain lerp by konst | `LERP(TFACTOR, TEXTURE, CURRENT)` |
| `(CPREV, TEXC, TEXA, ZERO)` | `BLENDTEXTUREALPHA` |
| `(ZERO, CPREV, RASC, ZERO)` (modulate result by vtx color) | `MODULATE(CURRENT, DIFFUSE)` |
| `(CPREV, ZERO, ZERO, TEXC)` etc. additive | `ADD(TEXTURE, CURRENT)` |
| `d=CPREV, a/b/c=0` (keep) | stage disabled / `SELECTARG1(CURRENT)` |
| `(C0, C1, TEXC, ZERO)` two-colour ramp — font gradient, lava, this game's dominant material shape | **Reproduced exactly, no longer approximated** (2026-08-04, CI-green, untested in game): both endpoints are transmitted — one in `D3DRS_TEXTUREFACTOR`, the other in `D3DMATERIAL9::Diffuse.rgb` — and the fork's shader computes `mix(rampLo, rampHi, albedo)`, which *is* GX's `a*(1-c)+b*c`. Declined back to the single-op approximation when a real stage has already claimed TFACTOR or the pass reads vertex colour; `ramp=` in `matrep.sum` gives the reason. `remix-material-interface.md` §10. Superseded: this row used to say a lerp between two constants was expressible only via per-stage constants — that was a statement about *stock* Remix |

   Alpha pass gets the same treatment on the alpha ops
   (`D3DTSS_ALPHAOP/ARG1/ARG2`); `dstAlpha` override → final stage alpha =
   TFACTOR alpha when `alphaUpdate`.
3. Bias/scale: `scale ×2/×4` → `D3DTOP_MODULATE2X/4X` when the op is
   modulate-shaped, `ADDSIGNED` for bias −0.5; other combos → nearest op
   (logged once per config hash, listed in unsupported doc).
4. Compare-mode TEV ops (GX_TEV_COMP_*): no TSS equivalent → approximated with
   SELECTARG1(CURRENT) + log (rare in TP main scenes). This approximation is
   the standing **suspect** (inference, not a finding) for the torch-flame
   white circle, which *does* reach Remix; the white ground textures it also
   produces are **not a defect**, because that image is never shown. If the
   torch is confirmed, the fix is to state the compare to the fork — not to
   hunt for a better TSS shape.
5. Konst colors: stage konst selector → one shared `D3DRS_TEXTUREFACTOR` per
   draw (first konst wins; per-stage constants used when the runtime exposes
   `PERSTAGECONSTANT`, which Remix/dxvk does). Conflicting multi-konst draws
   are logged + listed.
6. TEV output registers other than PREV (REG0-2 accumulation across stages):
   TSS has only CURRENT. Stages writing REGn then never reading it → treat as
   PREV chain; true multi-register programs → collapse with priority to the
   stage chain that feeds PREV at the end (logged; visual approximation).
7. Stage count: ≤8 TSS stages. TP materials are ≤4 TEV stages virtually
   everywhere (J3D TevBlock4 dominates; 16-stage blocks exist but are rare) —
   overflow truncates from the tail with log.
8. Swap tables ≠ identity: alpha-replicate cases (`RRRA` etc.) map to
   `D3DTA_ALPHAREPLICATE` when the pattern is exactly that; otherwise ignore
   (listed).
9. Indirect texturing stages: ignored in v1 (heat shimmer/water warp lose
   their warp — geometry still draws through the base stage). Listed.

Rasterized channel per stage (`channelId`): COLOR0A0 → DIFFUSE,
COLOR1A1 → SPECULAR (D3DTA_SPECULAR arg), COLOR_ZERO → constant black
(TFACTOR 0 if free, else D3DTA_TEMP trickery avoided in v1 — use
`D3DTA_DIFFUSE` with baked black vertex color case).

## 10. EFB copies & offscreen passes [v1 = color copies real]

- `copy_tex(dest, clear)`: **color-format copies are real** — `StretchRect`
  from the current render target into a `D3DPOOL_DEFAULT` render-target
  texture (ARGB8), honoring `texCopySrc` and the logical→render scale.
  Depth-format copies (and StretchRect failures) register a **1x1
  white/alpha-0 placeholder** instead. `clear=true` executes the GX clear
  semantics on the main target scoped to the `texCopySrc` rect.
- **Copy targets are cached per (dest pointer, copy size)** — not per dest
  alone. The classic bloom filter copies into the *same* guest buffer at 1/4
  and then 1/8 size every frame; a per-dest cache destroyed + recreated the
  render target on every size flip (2x per frame), and RTX Remix stamps every
  new render target with a fresh internal hash — its texture list spammed new
  entries every frame and VRAM grew without bound. Each size now keeps its
  own persistent target; `texture_find_copy` returns the size most recently
  copied into (matching "sample dest = last copy to dest" semantics). Stale
  sizes age out after ~600 frames.
- `begin/end_offscreen` (`GXCreateFrameBuffer`): real offscreen passes via
  render-target + depth surfaces cached by size (`texture_get_offscreen`).
- `resolve_pass`/`create_pass` (mods API): mods are wgpu-only and disabled in
  d3d9 mode (Dusklight side gates them).

## 11. Blend / depth / misc pixel state [v1]

| GX | D3D9 |
|----|------|
| `GX_BM_NONE` | `ALPHABLENDENABLE = FALSE` |
| `GX_BM_BLEND` (src,dst factors) | `SRCBLEND/DESTBLEND` direct table (SRCALPHA↔D3DBLEND_SRCALPHA, etc.; DSTALPHA factors need alpha in backbuffer → use `D3DFMT_A8R8G8B8` backbuffer if cap-supported, else approximate with ONE/ZERO — logged) |
| `GX_BM_SUBTRACT` | `BLENDOP = REVSUBTRACT`, factors ONE/ONE |
| `GX_BM_LOGIC` (blendOp) | only `GX_LO_CLEAR/SET/COPY/NOOP` mapped (via blend factors / color write); others unsupported (listed) |
| `depthCompare/depthFunc/depthUpdate` | `ZENABLE`, `ZFUNC` (direct enum map — no reversal), `ZWRITEENABLE` |
| `alphaCompare(comp0 op comp1)` | single D3D test: reduce — `AND(c, ALWAYS)`→c, `OR(c, NEVER)`→c, matching duplicated comps, `(GEQUAL x AND LEQUAL 0xFF)`→GEQUAL x, `(GREATER 0 OR GREATER 0)`→GREATER 0. Irreducible dual tests → use comp0, log (listed). `ALPHATESTENABLE/ALPHAREF/ALPHAFUNC` |
| `zCompLocBeforeTex` | no D3D9 equivalent (alpha test + early-z interplay is automatic); no-op |
| `colorUpdate/alphaUpdate` | `D3DRS_COLORWRITEENABLE` mask |
| `dstAlpha` const | final-stage alpha via TFACTOR (§9); plus `alphaUpdate` |
| fog | `D3DRS_FOGENABLE` + `FOGVERTEXMODE`: GX LIN → `D3DFOG_LINEAR` with start/end solved from the decoded a/b/c curve (`f = a/(b−z) − c` family); EXP/EXP2 → density approximations. Backwards fog + range-adjust: unsupported (listed) |
| `GX2SetPolygonOffset` | `DEPTHBIAS`/`SLOPESCALEDEPTHBIAS` (float bias ≈ offset/2^24) |
| scissor | `D3DRS_SCISSORTESTENABLE = TRUE` + `SetScissorRect` (render coords from existing mapping helpers) |
| viewport | `D3DVIEWPORT9{X,Y,W,H,MinZ,MaxZ}` from `renderViewport` (znear/zfar already 0..1) |
| line/point size | `D3DRS_POINTSIZE` for points; line width unsupported (D3D9 lines are 1px) — fine for TP |

**Alpha is an exception to the scope note.** Remix builds opacity and the alpha
test from what this section and §9 emit, so the reductions above are not
raster-only: an irreducible dual test collapsed to `comp0` changes what reaches
the path tracer, and a wrong alpha turns foliage into solid quads. Treat those
log lines as defects rather than curiosities.

## 12. UI path [v1]

**The HUD is one of the two things that must still rasterize correctly** —
Remix rasterizes UI draws rather than path-tracing them (`isRenderingUI` in
`d3d9_rtx.cpp`), so for these draws the 2D stage chain *is* the output and the
freedom claimed in the scope note above does not apply. Nothing special is
required for correctness beyond that: J2D emits ortho projection + no-Z +
alpha-blended quads through the same FIFO, and the generic pipeline handles it.
For **Remix UI auto-detection**, the backend tags likely-UI draws
(ortho projection + `depthCompare == false`) and could later switch them to
pre-transformed `D3DFVF_XYZRHW` vertices — **[later]** behind a config flag
(`dx9RhwUi`), since Remix's default heuristics (ortho + no depth) usually
suffice.

## 13. What Remix sees (summary of intentional choices)

- One `IDirect3DDevice9`, single-threaded draws, `DrawIndexedPrimitiveUP`.
- `SetTransform` WORLD/VIEW/PROJECTION per draw. With the game's
  `GXSetViewMtx` feed (§3): WORLD = true model→world, VIEW = the real
  camera — Remix reconstructs a proper camera and stable world space.
  Without it: fused model→view in WORLD, identity VIEW. That rasterizes
  correctly and is still useless, because Remix rejects it (below) — it is a
  failure state to detect, not a mode to fall back to.
- Fixed-function skinning via `D3DTS_WORLDMATRIX(i)` + indexed blending —
  Remix hashes rest-pose vertices and replays bones. Every blended draw
  carries an explicit BLENDWEIGHT stream (never bare `D3DVBF_0WEIGHTS`),
  because dxvk-remix's `dispatchSkinning` skips skinning entirely without
  one (§6a).
- **Bone-space convention:** with the game-supplied camera (§3,
  `GXSetViewMtx`) our blend bones are true object→**world** and
  `D3DTS_VIEW` is the real camera — exactly the convention dxvk-remix's
  `finalizeSkinningData` assumes (`objectToView := worldToView;
  objectToWorld := cameraViewToWorld × objectToView` → identity for our
  draws, since the skinned output is already world-space). World-space bone
  readouts (e.g. the `ReadBoneTransform` graph component) are meaningful.
  Without the camera call (the fused path), bones are model→view with
  identity VIEW — Remix's camera manager then rejects every draw
  (`objectToView == objectToWorld` ⇒ `CameraType::Unknown`), no camera
  exists, `finalizeSkinningData` is skipped, and each skinned draw keeps
  `WORLDMATRIX(0)` (its packet's slot-0 joint) as instance transform —
  character parts scatter. So the fused mode yields a correct rasterized image
  and a broken product: a draw arriving without `GXSetViewMtx` is a defect to
  fix, not a supported configuration.
- `SetTexture(stage 0..n)`. Which stage becomes the albedo is Remix's choice,
  not ours — see `remix-material-interface.md`; the mapper's job is to make the
  stage it picks readable rather than to assume stage order decides.
- Alpha-tested cutouts via ALPHATEST render states (Remix "cutout" category).
  **Alpha is the second thing that must still be right**: Remix builds opacity
  and the alpha test from the stage's alpha op and args, so an alpha shortcut
  that costs nothing in colour terms turns foliage into solid quads.
- **`D3DMATERIAL9` is a side channel, not a lighting input.** `D3DRS_LIGHTING`
  is off (§8), so the struct is inert to rasterization and the fork copies it
  into `LegacyMaterialData` (`set_remix_material`, `dx9_internal.hpp`):
  `Emissive.rgb` the colour the surface presents, `Emissive.a` the
  self-illumination **evidence score** — reported, not acted on —
  `Diffuse.rgb` the ramp endpoint TFACTOR does not carry, `Diffuse.a` "this is
  a ramp", `Ambient.r` "TFACTOR holds the texture-white endpoint",
  `Specular.r` "the vertex colour stream is authored material colour",
  `Specular.g` "aurora evaluated a colour here", `Specular.b` "this material has
  a colour of its own", `Specular.a` **"this material is self-lit"** — the three
  the emissive rule actually reads.
  Field meanings are owned by `remix-material-interface.md` §§7c, 9, 10. This
  is the "prefer stating over encoding" rule in practice: a per-draw fact
  transmitted directly beats one inferred from fixed-function state.
- No pixel/vertex shaders, no MSAA, no sRGB states, no queries.
