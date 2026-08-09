# Aurora / Dusklight rendering architecture — research notes

Findings from a full read of `aurora-ao` (the GPU-skinning lineage,
commit `7b7306e`) and targeted exploration of `dusklight-ao` (base `abf26c38d2`).
Everything the D3D9 backend relies on is recorded here with file references.

## 1. The one true draw path

Dusklight's decompiled game code calls **real GX functions** and replays
**original big-endian J3D display lists**. Aurora implements GX as a
Dolphin-style command processor:

1. GX API shims (`lib/dolphin/gx/*.cpp`) serialize calls into a growing FIFO
   byte buffer — big-endian, hardware encoding (`lib/gx/fifo.hpp`,
   `write_u32` byte-swaps on write). `GXCallDisplayList` memcpys the game's
   baked BE display list into the same FIFO (`lib/dolphin/gx/GXDispList.cpp:49`).
2. `aurora::gx::fifo::drain()` → `fifo::process(data, size, bigEndian=true)`
   (`lib/gx/command_processor.cpp:377`) decodes:
   - **BP writes** (`handle_bp`): TEV stages/ops/konst-sel (regs 0xC0-0xDF, 0xF6-0xFD),
     tev order (0x28-0x2F), z/blend/alpha-compare (0x40/0x41/0xF3), fog (0xEE-0xF2),
     scissor (0x20/0x21), tex regs (0x80-0xBB), TEV/K color regs (0xE0-0xE7),
     indirect stages/matrices, EFB copy trigger (0x52 → `copy_tex`).
   - **CP writes** (`handle_cp`): vertex descriptors VCD (0x50/0x60), VAT A/B/C
     formats (0x70-0x97), array strides (0xB0+), current PN matrix (0x30).
   - **XF writes** (`handle_xf` / `copy_xf_data`): pos matrices (addr<0x78, 3x4),
     normal matrices (0x400+, 3x3), tex matrices (0x78+), post-transform tex
     matrices (0x500+), lights (0x600+), channel ctrl (0x100E-0x1011),
     mat/amb colors (0x100A-0x100D), viewport (0x101A), projection (0x1020),
     texgen configs (0x1040+).
   - **Indexed XF loads** (`GX_LOAD_INDX_A..D`): matrix loads from
     `g_gxState.arrays[GX_POS_MTX_ARRAY..]` — this is how J3D's PNGP skinning
     pipeline loads the matrix palette (`J3DShapeMtx.cpp` uses
     `J3DFifoLoadIndx(GX_LOAD_INDX_A, mtxIdx, 0xB000|slot*0xC)`).
   - **Draw opcodes** (0x80-0xBF): `handle_draw` → `draw_prim`.
   - **`GX_AURORA` (custom opcode)** (`handle_aurora`): array bases (64-bit ptrs),
     texobj/tlut loads by pointer, EFB-copy src/dst descriptors, render
     viewport/scissor overrides (resolution scaling), full 4x4 projection,
     offscreen begin/end, `GX_AURORA_DRAW_SIZED`, `GX_AURORA_DRAW_INDEXED`
     (pre-triangulated draws from the DL optimizer), **`GX_AURORA_SET_SKINNING` /
     `CLEAR_SKINNING`** (GPU skinning extension), debug groups.
3. All decoded state lands in **`GXState g_gxState`** (`lib/gx/gx.hpp:286`) —
   the single source of truth at draw time. `stateDirty` gates draw merging.
4. `draw_prim` (`command_processor.cpp:1560`): computes vertex size from
   VCD+VAT, copies the **raw** vertex stream to the streaming vertex buffer,
   CPU-triangulates quads/strips/fans into u16 index lists
   (`prepare_idx_buffer`), merges consecutive same-state draws, then
   `push_gx_draw` builds `PipelineConfig`/`ShaderConfig` (a normalized snapshot
   of TEV/texgen/attr state, `populate_pipeline_config` in `lib/gx/gx.cpp:683`),
   uniform data, bind groups → `gfx::push_draw_command(DrawData)`.
5. The WebGPU execution layer (`lib/gfx/common.cpp`) records passes
   (`RenderPass` with command lists) into per-frame `FramePacket`s; a render
   worker thread encodes them with Dawn. Generated WGSL shaders decode the raw
   vertex bytes **in the vertex shader** (BE swaps, fixed-point scaling,
   indexed-attribute fetches from storage buffers) — `lib/gx/shader.cpp`.

**D3D9 consequence:** intercept at step 4 (before anything wgpu-flavored
happens), decode vertices on the CPU, submit immediately. Steps 1-3 are reused
untouched — that is ~all of Aurora's GX semantics for free.

## 2. GXState fields the D3D9 backend consumes

From `lib/gx/gx.hpp` (`GXState`):

- `pnMtx[10]` (pos+nrm 3x4 pairs), `currentPnMtx` — model→**view** matrices
  (GX has no separate view; XF pos matrices are modelview).
- `proj` (4x4, reconstructed GX projection), `projType` (perspective/ortho).
- `texMtxs[10]` (3x4), `ptTexMtxs[20]` (3x4), `tcgs[8]` (`TcgConfig`:
  type/src/mtx/postMtx/normalize) — texgen.
- `vtxDesc[25]` (GX_NONE/DIRECT/INDEX8/INDEX16 per attr),
  `vtxFmts[8].attrs[25]` (`VtxAttrFmt`: cnt/type/frac), `arrays[25]`
  (`AttrArray`: data ptr, size, stride, le flag) — vertex decoding inputs.
- TEV: `numTevStages`, `tevStages[16]` (`TevStage`: color/alpha passes a-d,
  ops incl. compare modes, konst sels, texCoordId/texMapId/channelId, swap
  sels, indirect params), `tevSwapTable[4]`, `colorRegs[4]` (TEVPREV+REG0-2),
  `kcolors[4]`, `numIndStages`, `indStages[4]`, `indTexMtxs[3]`.
- Channels: `numChans`, `colorChannelConfig[4]` (matSrc/ambSrc/lighting
  enable/diffFn/attnFn), `colorChannelState[4]` (matColor/ambColor/lightMask),
  `lights[8]`.
- Pixel: `blendMode/blendFacSrc/blendFacDst/blendOp(logic)`, `depthCompare/
  depthFunc/depthUpdate`, `zCompLocBeforeTex`, `alphaCompare` (two comparators
  + AND/OR/XOR/XNOR op), `dstAlpha` (forced constant destination alpha or
  UINT32_MAX), `colorUpdate/alphaUpdate`, `cullMode`, fog (`fog.type/a/b/c/color`),
  `clearColor/clearDepth`, line/point size.
- Viewport: `logicalViewport/renderViewport`, `logicalScissor/renderScissor`,
  `viewportPolicy` — logical means 640x480 game coordinates; render means
  scaled framebuffer pixels. Mapping helpers: `map_logical_viewport/scissor`
  (`lib/gx/gx.cpp:284`) using `vi::configured_fb_size()` vs render target size.
- Textures: `loadedTextures[8]` (`GXTexObj_`: data ptr, w/h, GX format, tlut
  id, texObjId + dataVersion for cache identity, wrap/filter fields decoded
  from mode0/mode1 regs), `loadedTluts[20]`, `copyTextures` (EFB-copy dest ptr
  → texture), `textures[8]` (resolved binds — wgpu-specific, D3D9 keeps its own).
- Skinning ext: `skinningActive`, `skinInfluences` (1-4), `skinBaseMtx`
  (model→view 3x4 applied after blending), plus (wgpu path) storage-buffer
  ranges for palette/influences. **D3D9 path stores the raw pointers instead**
  (palette = jointCount × 3x4 row-major floats; influences = vtxCount ×
  influenceCount × {u32 bone, f32 weight}, keyed by the value of the
  vertex's POS index attribute — skinned draws always use indexed positions).
- `frontOffset/frontScale/backOffset/backScale/clamp` — GX2 polygon offset ext.

## 3. Frame lifecycle & threading

- Dusklight main loop (`dusklight-ao/src/m_Do/m_Do_main.cpp:190` `main01()`):
  `aurora_update()` → `aurora_begin_frame()` → `fapGm_Execute()` (game logic +
  all GX emission) → `aurora_end_frame()`.
- `aurora::end_frame` (`lib/aurora.cpp:248`): `gx::fifo::drain()` — i.e. **the
  bulk of FIFO processing happens here, on the game thread** — then wgpu
  submission/present. Additional mid-frame drains occur in
  `gfx::common.cpp` (custom draws, `create_pass`/`resolve_pass`) and around
  EFB copies.
- D3D9 mode: everything happens on the game thread. `BeginScene` on
  `begin_frame`, draws during drains, `EndScene`+`Present` on `end_frame`.
  No render worker, no frame packets. D3D9 device is created with
  `D3DCREATE_MULTITHREADED` only as a safety net (imgui/debug); draws are
  single-threaded.

**The consequence, spelled out, because knowing the above did not prevent it:**
the game thread finishes emitting every draw in the frame *before* the backend
translates any of them. So a backend global set from game code — "the next draws
are X" — is not read next to those draws. It is read once the frame is over, and
describes whichever value was written last, for every draw in the frame.

Per-draw facts must travel **in the FIFO**, as an `GX_AURORA_*` subcommand, so
they arrive in order with the state they describe. `GX_AURORA_SET_VIEW_MTX` and
`GX_AURORA_SET_DUSKLIGHT_WATER` both exist for this reason; the latter cost two
test sessions, once as a flag that marked nothing and once as a flag that marked
everything. `remix-material-interface.md` §11 has the full account.

## 4. Textures

- CPU decoders in `lib/gfx/texture_convert.cpp` (`convert_texture`): every GX
  format (I4/I8/IA4/IA8/RGB565/RGB5A3/RGBA8/CMPR + PC formats R8/RG8/RGBA8/BC1)
  → RGBA8 byte buffers with all mips concatenated (CMPR is decompressed to
  RGBA8; BC1_PC can stay compressed). Palette formats C4/C8/C14X2 decode to
  index data and are combined with TLUTs (`convert_tlut` → RGBA8 palette).
  The wgpu path sometimes does palette combine on the GPU
  (`tex_palette_conv`); **D3D9 does it on the CPU** (trivial index→palette
  lookup, see mapping doc §7).
- Cache identity: `texObjId` + `texDataVersion` (and TLUT equivalents) —
  destroyed via `GX_AURORA_DESTROY_TEXOBJ/TLUT` (hook `evict_texture_object`).
- Texture replacement (Dolphin-format packs, `texture_replacement.cpp`) and
  DDS loading exist; v1 of the D3D9 backend skips replacement packs
  (documented), can be added later via the same DDS decoding. Not a gap under
  Remix: replacement is Remix's own job there, and since 2026-08-04 that
  extends to API-submitted assets too (content-derived mesh hashes +
  `submitExternalDraw` consulting `getReplacementMaterial` in the fork —
  CI-green, no capture taken in game yet).
  Aurora-side packs would only matter to the standalone image.
- EFB copies: `GXCopyTex` → BP 0x52 → `copy_tex(dest, clear)` — the wgpu path
  resolves the current pass into a texture keyed by the destination pointer
  (`copyTextures`). Game uses this for shadow silhouettes (packed RGBA),
  distortion sources, etc. D3D9 v1 policy in mapping doc §10.

## 5. GPU skinning

Two distinct skinning mechanisms exist:

1. **Matrix-palette (the normal path, ~all characters incl. Link):** J3D
   pre-blends weighted envelope matrices on the CPU into a ≤10-entry palette
   per draw packet (`J3DMtxBuffer::calcWeightEnvelopeMtx`), loads them via
   indexed XF loads (GX_LOAD_INDX_A) or `GXLoadPosMtxImm`, and each vertex
   carries `GX_VA_PNMTXIDX` (index = slot*3). Aurora keeps the palette in
   `g_gxState.pnMtx[10]`. **Already GPU skinning** in the Remix sense; maps to
   D3D9 indexed vertex blending with 1 index.
2. **`GXSetSkinning` extension (rare J3DSkinDeform models — 2 actors):**
   dusklight-ao `src/dusk/gpu_skinning.cpp` inverts J3D's joint→vertex lists
   into per-vertex `{u32 bone, f32 weight}[≤4]` tables, computes the palette
   (`anm·invBind` per joint), and brackets the shape's display list with
   `GXSetSkinning(palette, jointCount, influences, vtxCount, 4, baseMtx)` /
   `GXClearSkinning()` (hooks in `J3DSkinDeform::deform` and
   `J3DShapePacket::drawFast`). The WGSL path blends in the vertex shader,
   keyed by the POS index attribute; skinned draws are never merged.

## 6. Dusklight-side integration points (for the D3D9 option)

- Backend resolution: `src/m_Do/m_Do_main.cpp:373` `ResolveDesiredBackend`
  (CLI `--backend` at `:502`, config fallback), `IsBackendAvailable` `:357`.
- Config var: `backend.graphicsBackend` (`src/dusk/settings.h:291`,
  `settings.cpp:164,351`), stores lowercase id strings.
- String↔enum maps: `src/dusk/ui/settings.cpp` — `try_parse_backend` `:112`,
  `backend_name` `:153`, `backend_id` `:176`, `available_backends` `:199`
  (wraps `aurora_get_available_backends()`, hides NULL). Duplicate parse/name
  helpers in `src/dusk/imgui/ImGuiConsole.cpp:439,485`.
- Settings menu control: `src/dusk/ui/settings.cpp:640` ("Prelaunch" tab,
  "Graphics Backend" select, restart required).
- Mods/graphics stages (`GFX_STAGE_*`, `src/dusk/mods/svc/gfx.cpp`) and the
  WGSL mods (`mods/ao_mod`, `mods/shadow_mod`) are **wgpu-only** — must be
  inert/disabled when the d3d9 backend is active.

## 7. Game-side GX usage patterns worth knowing (from dusklight-ao survey)

- **UI (J2D/JUTFont):** ortho `GXSetProjection`, identity PNMTX0, no lighting
  (chan = vertex color source, `GX_DF_NONE`), 1-3 TEV stages (PASSCLR /
  texture passthrough / modulate / lerp-two-textures-by-konst / two-reg
  gradient by glyph alpha), alpha compare `GREATER 0 OR GREATER 0`, no Z, blend
  SRCALPHA/INVSRCALPHA, `GX_QUADS` with f32 pos + RGBA8 color + s16/u16 UVs.
  All expressible in fixed function.
- **J3D materials:** TEV state arrives as raw BP register replays (handled by
  `handle_bp` already). Terrain two-texture blends (dirt↔grass) are data-driven
  TEV programs: lerp `TEXC` of two maps by rasterized alpha (`RASA`) or konst.
  The generic TEV→TSS mapper handles them; no terrain special case needed.
- **Foliage/billboard cutouts (`J3DPEBlockTexEdge`):** alphaCompare
  `GEQUAL 0x80 AND LEQUAL 0xFF`, no blend, Z on, `zCompLoc=false` →
  D3D9 alpha test `GREATEREQUAL 0x80`. Billboarding itself is just a
  view-aligned pos matrix computed by J3D on the CPU — nothing to do.
- **Particles:** immediate-mode quads, alpha `GEQUAL n OR GEQUAL n`,
  additive/standard blends.
- Immediate-mode surface (most-called): `GXBegin/End`, `GXPosition3f32/2f32/3s16`,
  `GXColor1u32/4u8`, `GXTexCoord2s16/2f32/2u16/2s8`, `GXNormal3f32`,
  `GXSetVtxAttrFmt/Desc`, `GXLoadPosMtxImm`, `GXSetCurrentMtx`, full TEV set,
  `GXSetBlendMode/ZMode/AlphaCompare/ZCompLoc`, `GXSetTexCoordGen`,
  `GXSetChanCtrl/MatColor/AmbColor`, `GXLoadTexObj`, `GXSetProjection`,
  `GXSetViewport/Scissor/CullMode`, `GXCallDisplayList`, `GXCopyTex`.
  All of these already funnel into `fifo::process` — the D3D9 backend never
  sees them individually.

## 8. Reversed-Z / depth conventions

The wgpu path uses reversed-Z (`UseReversedZ = true`, near=1) with the
correction folded into the projection matrix (aurora commit `1dde08f`, applied
in `GXSetProjection`/`GXSetProjectionv` shims — check `lib/dolphin/gx/GXTransform.cpp`
when implementing). GX itself produces NDC z ∈ [-1, 0]. D3D9 wants z ∈ [0, 1]
with conventional less-equal testing. The D3D9 backend must:
- take the raw GX projection from `g_gxState.proj`,
- undo/skip the reversed-Z massaging if it was applied pre-FIFO (verify at
  implementation time — see mapping doc §3),
- apply `z' = -z` style remap ([-1,0] → [0,1]) via a constant post-multiply
  matrix, and use `D3DCMP_LESSEQUAL` etc. directly from `depthFunc`.
