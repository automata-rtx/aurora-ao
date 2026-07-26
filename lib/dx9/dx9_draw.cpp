#include "dx9.hpp"

#ifdef AURORA_ENABLE_D3D9

#include "dx9_internal.hpp"
#include "dx9_tev.hpp"
#include "dx9_texture.hpp"
#include "dx9_vertex.hpp"

#include <algorithm>
#include <vector>

namespace aurora::dx9 {
static Module Log("aurora::dx9::draw");

namespace {

thread_local std::vector<uint16_t> t_indexScratch;

inline D3DCMPFUNC to_d3d_cmp(GXCompare func) noexcept {
  switch (func) {
  case GX_NEVER:
    return D3DCMP_NEVER;
  case GX_LESS:
    return D3DCMP_LESS;
  case GX_EQUAL:
    return D3DCMP_EQUAL;
  case GX_LEQUAL:
    return D3DCMP_LESSEQUAL;
  case GX_GREATER:
    return D3DCMP_GREATER;
  case GX_NEQUAL:
    return D3DCMP_NOTEQUAL;
  case GX_GEQUAL:
    return D3DCMP_GREATEREQUAL;
  case GX_ALWAYS:
  default:
    return D3DCMP_ALWAYS;
  }
}

inline D3DBLEND to_d3d_blend(GXBlendFactor fac) noexcept {
  switch (fac) {
  case GX_BL_ZERO:
    return D3DBLEND_ZERO;
  case GX_BL_ONE:
    return D3DBLEND_ONE;
  case GX_BL_SRCCLR: // == GX_BL_DSTCLR on the other side
    return D3DBLEND_SRCCOLOR;
  case GX_BL_INVSRCCLR:
    return D3DBLEND_INVSRCCOLOR;
  case GX_BL_SRCALPHA:
    return D3DBLEND_SRCALPHA;
  case GX_BL_INVSRCALPHA:
    return D3DBLEND_INVSRCALPHA;
  case GX_BL_DSTALPHA:
    return D3DBLEND_DESTALPHA;
  case GX_BL_INVDSTALPHA:
    return D3DBLEND_INVDESTALPHA;
  default:
    return D3DBLEND_ONE;
  }
}

void apply_blend_state() noexcept {
  switch (g_gxState.blendMode) {
  case GX_BM_NONE:
    set_rs(D3DRS_ALPHABLENDENABLE, FALSE);
    break;
  case GX_BM_BLEND:
    set_rs(D3DRS_ALPHABLENDENABLE, TRUE);
    set_rs(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    set_rs(D3DRS_SRCBLEND, to_d3d_blend(g_gxState.blendFacSrc));
    set_rs(D3DRS_DESTBLEND, to_d3d_blend(g_gxState.blendFacDst));
    break;
  case GX_BM_SUBTRACT:
    // GX subtract: dst = dst - src.
    set_rs(D3DRS_ALPHABLENDENABLE, TRUE);
    set_rs(D3DRS_BLENDOP, D3DBLENDOP_REVSUBTRACT);
    set_rs(D3DRS_SRCBLEND, D3DBLEND_ONE);
    set_rs(D3DRS_DESTBLEND, D3DBLEND_ONE);
    break;
  case GX_BM_LOGIC:
    switch (g_gxState.blendOp) {
    case GX_LO_COPY:
      set_rs(D3DRS_ALPHABLENDENABLE, FALSE);
      break;
    case GX_LO_NOOP:
      set_rs(D3DRS_ALPHABLENDENABLE, TRUE);
      set_rs(D3DRS_BLENDOP, D3DBLENDOP_ADD);
      set_rs(D3DRS_SRCBLEND, D3DBLEND_ZERO);
      set_rs(D3DRS_DESTBLEND, D3DBLEND_ONE);
      break;
    case GX_LO_CLEAR:
      set_rs(D3DRS_ALPHABLENDENABLE, TRUE);
      set_rs(D3DRS_BLENDOP, D3DBLENDOP_ADD);
      set_rs(D3DRS_SRCBLEND, D3DBLEND_ZERO);
      set_rs(D3DRS_DESTBLEND, D3DBLEND_ZERO);
      break;
    default:
      warn_once(0x7000 | static_cast<uint32_t>(g_gxState.blendOp), "blend: unsupported logic op");
      set_rs(D3DRS_ALPHABLENDENABLE, FALSE);
      break;
    }
    break;
  default:
    set_rs(D3DRS_ALPHABLENDENABLE, FALSE);
    break;
  }
}

// GX has two alpha comparators joined by AND/OR/XOR/XNOR; D3D9 has one.
// Reduce where exact, otherwise use comparator 0 (docs, unsupported #12).
void apply_alpha_compare() noexcept {
  const auto& ac = g_gxState.alphaCompare;
  GXCompare comp = ac.comp0;
  uint32_t ref = ac.ref0;

  const bool same = ac.comp0 == ac.comp1 && ac.ref0 == ac.ref1;
  switch (ac.op) {
  case GX_AOP_AND:
    if (ac.comp0 == GX_ALWAYS) {
      comp = ac.comp1;
      ref = ac.ref1;
    } else if (ac.comp1 == GX_ALWAYS || same) {
      comp = ac.comp0;
      ref = ac.ref0;
    } else if (ac.comp0 == GX_NEVER || ac.comp1 == GX_NEVER) {
      comp = GX_NEVER;
    } else if (ac.comp0 == GX_GEQUAL && ac.comp1 == GX_LEQUAL && ac.ref1 >= 0xFF) {
      // J3D TexEdge: GEQUAL ref AND LEQUAL 0xFF == GEQUAL ref.
      comp = GX_GEQUAL;
      ref = ac.ref0;
    } else {
      warn_once(0x7100, "alpha compare: irreducible AND, using comparator 0");
    }
    break;
  case GX_AOP_OR:
    if (ac.comp0 == GX_ALWAYS || ac.comp1 == GX_ALWAYS) {
      comp = GX_ALWAYS;
    } else if (ac.comp0 == GX_NEVER || same) {
      comp = ac.comp1;
      ref = ac.ref1;
    } else if (ac.comp1 == GX_NEVER) {
      comp = ac.comp0;
      ref = ac.ref0;
    } else {
      warn_once(0x7101, "alpha compare: irreducible OR, using comparator 0");
    }
    break;
  default:
    warn_once(0x7102 | static_cast<uint32_t>(ac.op) << 8, "alpha compare: XOR/XNOR unsupported, using comparator 0");
    break;
  }

  if (comp == GX_ALWAYS) {
    set_rs(D3DRS_ALPHATESTENABLE, FALSE);
    return;
  }
  set_rs(D3DRS_ALPHATESTENABLE, TRUE);
  set_rs(D3DRS_ALPHAFUNC, to_d3d_cmp(comp));
  set_rs(D3DRS_ALPHAREF, ref & 0xFF);
}

// GX fog -> D3D9 fog render states (docs #11).
//
// The BP registers hold the SDK-computed curve f(zs) = A/(B - zs) - C, which for a
// perspective projection collapses to f(ze) = (ze - start)/(end - start) in view units.
// A/B/C alone leave (start, end) scaled by the projection near plane, so it is recovered
// from the current projection matrix: with m22 = -n/(f-n) and m23 = -fn/(f-n) (GX NDC z in
// [-1,0]), near = m23/(m22 - 1).
//
// The point of forwarding these is RTX Remix: its per-draw capture reads D3DRS_FOG* and
// reapplies the exact D3DFOG_LINEAR ramp in its composite (or remaps it into volumetrics),
// which restores the game's environment-driven fog under the path tracer. Raw D3D9 also
// rasterizes with it, which matches the GX output for the linear modes the game uses.
//
// GX range adjust (GXSetFogRangeAdj) is not forwarded: Remix's composite already fogs by
// radial distance, which is what range adjust approximates. Backwards (REVEXP) fog is not
// representable in D3D9 and stays disabled.
void apply_fog_state() noexcept {
  const aurora::gx::FogState& fog = g_gxState.fog;

  // Aurora's BP round trip keeps only the 3-bit function select, so ortho types alias onto
  // the perspective ones. Ortho projections are the game's 2D work and are excluded below,
  // which also keeps Remix from seeing fog on draws it classifies as UI.
  const uint32_t fsel = static_cast<uint32_t>(fog.type) & 7u;

  if (fsel == 0 || g_gxState.projType == GX_ORTHOGRAPHIC) {
    set_rs(D3DRS_FOGENABLE, FALSE);
    return;
  }

  if (fsel == (GX_FOG_PERSP_REVEXP & 7) || fsel == (GX_FOG_PERSP_REVEXP2 & 7)) {
    warn_once(0x7110, "fog: backwards (REVEXP) fog unsupported, disabling");
    set_rs(D3DRS_FOGENABLE, FALSE);
    return;
  }

  // near/far are (empty) legacy macros in windef.h, hence projNear.
  const float m22 = g_gxState.proj.m2[2];
  const float m23 = g_gxState.proj.m2[3];
  const float projNear = m23 / (m22 - 1.0f);
  const float k = fog.b * projNear != 0.0f ? fog.a / (fog.b * projNear) : 0.0f;

  // A degenerate curve (the SDK encodes A=0 for far==near or end==start) fogs nothing.
  if (!(k > 0.0f) || !(projNear > 0.0f)) {
    set_rs(D3DRS_FOGENABLE, FALSE);
    return;
  }

  const float start = fog.c / k;
  const float end = (1.0f + fog.c) / k;

  set_rs(D3DRS_FOGENABLE, TRUE);
  set_rs(D3DRS_FOGCOLOR,
         D3DCOLOR_COLORVALUE(fog.color.x(), fog.color.y(), fog.color.z(), fog.color.w()));
  set_rs(D3DRS_FOGVERTEXMODE, D3DFOG_NONE);

  if (fsel == (GX_FOG_PERSP_LIN & 7)) {
    set_rs(D3DRS_FOGTABLEMODE, D3DFOG_LINEAR);
    set_rs(D3DRS_FOGSTART, std::bit_cast<DWORD>(start));
    set_rs(D3DRS_FOGEND, std::bit_cast<DWORD>(end));
  } else {
    // EXP/EXP2. GX computes visibility = 2^(-8*f) (squared for EXP2); D3D uses
    // e^(-density*d) with d unshifted by start. Matching the slopes gives
    // density = 8*ln2/(end-start); the missing start offset makes this an approximation,
    // which is acceptable - the game's own fog is exclusively GX_FOG_PERSP_LIN and only
    // model material fog blocks could reach this path.
    constexpr float kLn2Times8 = 5.5452f;
    const float density = kLn2Times8 * k;

    set_rs(D3DRS_FOGTABLEMODE, fsel == (GX_FOG_PERSP_EXP & 7) ? D3DFOG_EXP : D3DFOG_EXP2);
    set_rs(D3DRS_FOGDENSITY, std::bit_cast<DWORD>(density));
  }
}

// Returns false when the draw should be skipped entirely (GX_CULL_ALL).
bool apply_pixel_state() noexcept {
  switch (g_gxState.cullMode) {
  case GX_CULL_ALL:
    return false;
  case GX_CULL_FRONT:
    // GX front face is clockwise (after the shared convention swap).
    set_rs(D3DRS_CULLMODE, D3DCULL_CW);
    break;
  case GX_CULL_BACK:
    set_rs(D3DRS_CULLMODE, D3DCULL_CCW);
    break;
  case GX_CULL_NONE:
  default:
    set_rs(D3DRS_CULLMODE, D3DCULL_NONE);
    break;
  }

  set_rs(D3DRS_ZENABLE, g_gxState.depthCompare ? D3DZB_TRUE : D3DZB_FALSE);
  set_rs(D3DRS_ZFUNC, g_gxState.depthCompare ? to_d3d_cmp(g_gxState.depthFunc) : D3DCMP_ALWAYS);
  set_rs(D3DRS_ZWRITEENABLE, (g_gxState.depthCompare && g_gxState.depthUpdate) ? TRUE : FALSE);

  DWORD writeMask = 0;
  if (g_gxState.colorUpdate) {
    writeMask |= D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE;
  }
  if (g_gxState.alphaUpdate) {
    writeMask |= D3DCOLORWRITEENABLE_ALPHA;
  }
  set_rs(D3DRS_COLORWRITEENABLE, writeMask);

  apply_blend_state();
  apply_alpha_compare();

  // Polygon offset (GX2 extension); front values cover the common case.
  const bool backFacing = g_gxState.cullMode == GX_CULL_FRONT;
  const float offset = backFacing ? g_gxState.backOffset : g_gxState.frontOffset;
  const float scale = backFacing ? g_gxState.backScale : g_gxState.frontScale;
  set_rs(D3DRS_DEPTHBIAS, std::bit_cast<DWORD>(offset / 16777215.0f));
  set_rs(D3DRS_SLOPESCALEDEPTHBIAS, std::bit_cast<DWORD>(scale));

  apply_fog_state();
  return true;
}

void apply_transforms(const DecodedDraw& draw) noexcept {
  set_proj_matrix(to_d3d_proj(g_gxState.proj));

  g_worldViewInv.valid = false;
  const D3DMATRIX identity{{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}}};
  // With a camera provided (GX_AURORA_SET_VIEW_MTX), split the GX combined
  // model->view into WORLD = pnMtx*viewInv (true model->world) and VIEW =
  // camera. Rasterization is identical (WORLD*VIEW == pnMtx), but RTX Remix
  // can then derive a real camera - without one its camera manager rejects
  // every draw (objectToView == objectToWorld), finalizeSkinningData never
  // runs, and skinned instances inherit WORLDMATRIX(0) as their transform,
  // scattering body parts. See docs #3/#6/#13.
  //
  // Orthographic draws are excluded. They are the game's 2D work - HUD, menus,
  // fullscreen filter quads - and Remix classifies ortho draws as UI, then
  // rasterizes them as a screen overlay. Handing that path a 3D view matrix
  // with the 2D transform folded into WORLD leaves the overlay derived from a
  // world-space camera instead of the flat 2D setup it expects, which is why
  // the HUD came out oversized and stretched under Remix while raw D3D9 was
  // correct at every window size. 2D draws therefore keep the pre-camera-split
  // fused form (VIEW = identity), which is also what they looked like before
  // the split existed. Rasterization is unaffected either way, since
  // WORLD * VIEW is the same product.
  const bool haveCam = g_camera.valid && g_gxState.projType != GX_ORTHOGRAPHIC;
  const D3DMATRIX& view = haveCam ? g_camera.view : identity;
  if (draw.skinned) {
    // Fixed-function indexed vertex blending against the bone palette; the
    // model->skin-space base matrix is folded into every bone so the blend
    // output lands in world space (camera present) or view space (fused).
    const uint32_t jointCount = std::min<uint32_t>(g_skin.jointCount, MaxWorldPalette);
    if (g_skin.jointCount > MaxWorldPalette) {
      warn_once(0x8000, "skinning: joint count exceeds 256, clamping");
    }
    const uint32_t maxIdx = g_dx9.caps.MaxVertexBlendMatrixIndex;
    if (jointCount > maxIdx + 1) {
      warn_once(0x8001 | jointCount << 8, "skinning: joint count exceeds device MaxVertexBlendMatrixIndex");
    }
    const D3DMATRIX base = to_d3d_3x4(g_gxState.skinBaseMtx.data());
    if (haveCam) {
      const D3DMATRIX baseWorld = mtx_multiply(base, g_camera.viewInv);
      for (uint32_t i = 0; i < jointCount; ++i) {
        const D3DMATRIX bone = to_d3d_3x4(g_skin.palette + static_cast<size_t>(i) * 12);
        set_world_matrix(i, mtx_multiply(bone, baseWorld));
      }
      set_view_matrix(view);
    } else {
      // Fused path: bones stay as-is, the base matrix rides in VIEW
      // (applied after the blend).
      for (uint32_t i = 0; i < jointCount; ++i) {
        set_world_matrix(i, to_d3d_3x4(g_skin.palette + static_cast<size_t>(i) * 12));
      }
      set_view_matrix(base);
    }
    static constexpr DWORD kBlendMode[] = {D3DVBF_0WEIGHTS, D3DVBF_1WEIGHTS, D3DVBF_2WEIGHTS, D3DVBF_3WEIGHTS};
    set_rs(D3DRS_VERTEXBLEND, kBlendMode[draw.weightCount]);
    set_rs(D3DRS_INDEXEDVERTEXBLENDENABLE, TRUE);
  } else if (draw.hasPnMtxIdx) {
    // Matrix-palette draws (J3D characters): the 10 GX position matrices
    // become the world matrix palette, selected per vertex. The vertices
    // store an explicit 1.0 weight (D3DVBF_1WEIGHTS rather than 0WEIGHTS,
    // equivalent under fixed-function) because RTX Remix's GPU skinning
    // requires a blend-weight stream — see decode_draw.
    // Only the matrices this draw actually references, renumbered into a dense
    // range by decode_draw: GX has 10 position matrices but fixed-function
    // indexed blending only reaches D3DCAPS9::MaxVertexBlendMatrixIndex (8
    // here), and an out-of-range blend index reads an undefined matrix, which
    // scatters those vertices far across the world. Remix is unaffected by the
    // cap - it reads the transform state directly and skins on the GPU - so
    // this only ever showed up in raw D3D9.
    for (uint32_t i = 0; i < draw.pnMtxCount; ++i) {
      const D3DMATRIX m = to_d3d(g_gxState.pnMtx[draw.pnMtxSlots[i]].pos);
      set_world_matrix(i, haveCam ? mtx_multiply(m, g_camera.viewInv) : m);
    }
    set_view_matrix(view);
    set_rs(D3DRS_VERTEXBLEND, D3DVBF_1WEIGHTS);
    set_rs(D3DRS_INDEXEDVERTEXBLENDENABLE, TRUE);
  } else {
    const D3DMATRIX modelView = to_d3d(g_gxState.pnMtx[g_gxState.currentPnMtx].pos);
    set_world_matrix(0, haveCam ? mtx_multiply(modelView, g_camera.viewInv) : modelView);
    set_view_matrix(view);
    set_rs(D3DRS_VERTEXBLEND, D3DVBF_DISABLE);
    set_rs(D3DRS_INDEXEDVERTEXBLENDENABLE, FALSE);
    // Camera-space texgen compensation (docs #7): D3D feeds view-space
    // inputs where GX texgen reads model-space; premultiplying the texture
    // matrix with the model-view inverse restores GX semantics (projected
    // shadows/light shafts, env maps). Only well-defined for rigid draws.
    // Always derived from the COMBINED model->view - D3D's camera-space
    // texgen input is WORLD*VIEW, which equals pnMtx on both paths.
    if (mtx_affine_inverse(modelView, g_worldViewInv.full)) {
      g_worldViewInv.rotation = g_worldViewInv.full;
      g_worldViewInv.rotation.m[3][0] = 0.f;
      g_worldViewInv.rotation.m[3][1] = 0.f;
      g_worldViewInv.rotation.m[3][2] = 0.f;
      g_worldViewInv.valid = true;
    }
  }
}

// Triangulates quads/strips/fans into an index list (mirrors the GX topology
// handling of the wgpu path, including the strip winding flip).
uint32_t build_indices(GXPrimitive prim, uint16_t vtxCount, std::vector<uint16_t>& out) noexcept {
  out.clear();
  switch (prim) {
  case GX_QUADS:
    // Fan order (0,1,2 / 0,2,3) rather than (0,1,2 / 2,3,0): identical
    // triangles and winding, but it is the layout Remix's billboard detection
    // expects - it logs "detected unsupported quad index layout for billboard
    // creation" otherwise and falls back to treating particle quads as plain
    // geometry.
    for (uint16_t v = 0; v + 3 < vtxCount; v += 4) {
      out.insert(out.end(), {v, static_cast<uint16_t>(v + 1), static_cast<uint16_t>(v + 2), v,
                             static_cast<uint16_t>(v + 2), static_cast<uint16_t>(v + 3)});
    }
    break;
  case GX_TRIANGLESTRIP:
    for (uint16_t v = 2; v < vtxCount; ++v) {
      if ((v & 1) == 0) {
        out.insert(out.end(),
                   {static_cast<uint16_t>(v - 2), static_cast<uint16_t>(v - 1), v});
      } else {
        out.insert(out.end(),
                   {static_cast<uint16_t>(v - 1), static_cast<uint16_t>(v - 2), v});
      }
    }
    break;
  case GX_TRIANGLEFAN:
    for (uint16_t v = 2; v < vtxCount; ++v) {
      out.insert(out.end(), {static_cast<uint16_t>(0), static_cast<uint16_t>(v - 1), v});
    }
    break;
  default:
    break;
  }
  return static_cast<uint32_t>(out.size());
}

// Draws a matrix-palette mesh that references more distinct matrices than the
// device can index (D3DCAPS9::MaxVertexBlendMatrixIndex + 1 — 9 against GX's
// 10) by partitioning its triangles into groups that each fit, giving every
// group its own palette. Folding the excess onto one slot instead, as the
// first version of the cap fix did, leaves those vertices transformed by the
// wrong joint — a bounded explosion that (unlike an out-of-range index) looks
// identical under Remix, since the bad index is baked into the vertex data.
void draw_palette_split(const DecodedDraw& draw, const uint16_t* indices, uint32_t indexCount) noexcept {
  const auto limit = static_cast<uint32_t>(g_dx9.caps.MaxVertexBlendMatrixIndex) + 1;
  const uint8_t* slots = draw.pnMtxPerVertex;
  if (slots == nullptr || limit < 3 || indexCount < 3) {
    return;
  }

  static thread_local std::vector<uint8_t> s_verts;
  static thread_local std::vector<uint16_t> s_indices;
  static thread_local std::vector<int32_t> s_vtxMap;

  set_fvf(draw.fvf);
  const uint32_t triCount = indexCount / 3;
  uint32_t tri = 0;
  while (tri < triCount) {
    std::array<int8_t, gx::MaxPnMtx> localOf{};
    localOf.fill(-1);
    std::array<uint8_t, gx::MaxPnMtx> groupSlots{};
    uint32_t used = 0;
    s_vtxMap.assign(draw.vtxCount, -1);
    s_verts.clear();
    s_indices.clear();

    // Take triangles until the next one would not fit this group's palette.
    while (tri < triCount) {
      const uint16_t* t = indices + static_cast<size_t>(tri) * 3;
      std::array<uint8_t, 3> triSlots{};
      bool valid = true;
      uint32_t added = 0;
      for (int k = 0; k < 3; ++k) {
        if (t[k] >= draw.vtxCount) {
          valid = false;
          break;
        }
        const uint8_t s = slots[t[k]] < gx::MaxPnMtx ? slots[t[k]] : 0;
        triSlots[k] = s;
        if (localOf[s] < 0) {
          bool dup = false;
          for (int j = 0; j < k; ++j) {
            dup = dup || triSlots[j] == s;
          }
          added += dup ? 0 : 1;
        }
      }
      if (!valid) {
        ++tri;
        continue;
      }
      if (used + added > limit) {
        break;
      }
      for (int k = 0; k < 3; ++k) {
        if (localOf[triSlots[k]] < 0) {
          localOf[triSlots[k]] = static_cast<int8_t>(used);
          groupSlots[used] = triSlots[k];
          ++used;
        }
      }
      for (int k = 0; k < 3; ++k) {
        const uint16_t vi = t[k];
        if (s_vtxMap[vi] < 0) {
          const auto newIndex = static_cast<uint32_t>(s_verts.size() / draw.stride);
          s_vtxMap[vi] = static_cast<int32_t>(newIndex);
          const uint8_t* srcV = draw.verts + static_cast<size_t>(vi) * draw.stride;
          s_verts.insert(s_verts.end(), srcV, srcV + draw.stride);
          const auto localIndex = static_cast<uint32_t>(localOf[triSlots[k]]);
          std::memcpy(s_verts.data() + static_cast<size_t>(newIndex) * draw.stride + draw.blendIndexOffset,
                      &localIndex, 4);
        }
        s_indices.push_back(static_cast<uint16_t>(s_vtxMap[vi]));
      }
      ++tri;
    }

    if (s_indices.size() < 3) {
      break;
    }
    for (uint32_t i = 0; i < used; ++i) {
      const D3DMATRIX m = to_d3d(g_gxState.pnMtx[groupSlots[i]].pos);
      set_world_matrix(i, g_camera.valid ? mtx_multiply(m, g_camera.viewInv) : m);
    }
    g_dx9.dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, static_cast<UINT>(s_verts.size() / draw.stride),
                                      static_cast<UINT>(s_indices.size() / 3), s_indices.data(), D3DFMT_INDEX16,
                                      s_verts.data(), draw.stride);
  }
}

bool can_draw() noexcept { return g_dx9.dev != nullptr && g_dx9.inScene; }

void submit(const DecodedDraw& draw, GXPrimitive prim) noexcept {
  set_fvf(draw.fvf);

  switch (prim) {
  case GX_TRIANGLES:
    if (draw.vtxCount >= 3) {
      g_dx9.dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, draw.vtxCount / 3, draw.verts, draw.stride);
    }
    break;
  case GX_QUADS:
  case GX_TRIANGLESTRIP:
  case GX_TRIANGLEFAN: {
    if (draw.vtxCount < 3) {
      return;
    }
    const uint32_t numIndices = build_indices(prim, static_cast<uint16_t>(draw.vtxCount), t_indexScratch);
    if (numIndices >= 3) {
      g_dx9.dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, draw.vtxCount, numIndices / 3,
                                        t_indexScratch.data(), D3DFMT_INDEX16, draw.verts, draw.stride);
    }
    break;
  }
  case GX_LINES:
    if (draw.vtxCount >= 2) {
      g_dx9.dev->DrawPrimitiveUP(D3DPT_LINELIST, draw.vtxCount / 2, draw.verts, draw.stride);
    }
    break;
  case GX_LINESTRIP:
    if (draw.vtxCount >= 2) {
      g_dx9.dev->DrawPrimitiveUP(D3DPT_LINESTRIP, draw.vtxCount - 1, draw.verts, draw.stride);
    }
    break;
  case GX_POINTS:
    if (draw.vtxCount >= 1) {
      g_dx9.dev->DrawPrimitiveUP(D3DPT_POINTLIST, draw.vtxCount, draw.verts, draw.stride);
    }
    break;
  default:
    warn_once(0x9000 | static_cast<uint32_t>(prim), "unsupported primitive type");
    break;
  }
}

} // namespace

void draw_prim(GXPrimitive prim, GXVtxFmt fmt, uint16_t vtxCount, const uint8_t* data, uint32_t vtxSize,
               bool bigEndian) noexcept {
  if (!can_draw() || vtxCount == 0) {
    return;
  }
  DecodedDraw draw;
  if (!decode_draw(fmt, vtxCount, data, vtxSize, bigEndian, draw)) {
    return;
  }
  if (!apply_pixel_state()) {
    return;
  }
  apply_transforms(draw);
  apply_tev(draw);
  if (draw.pnMtxOverflow) {
    // Needs per-group palettes; build a triangle list for the topology first.
    if (prim == GX_TRIANGLES) {
      t_indexScratch.clear();
      for (uint16_t v = 0; v + 2 < vtxCount; v += 3) {
        t_indexScratch.insert(t_indexScratch.end(),
                              {v, static_cast<uint16_t>(v + 1), static_cast<uint16_t>(v + 2)});
      }
    } else {
      build_indices(prim, static_cast<uint16_t>(draw.vtxCount), t_indexScratch);
    }
    if (t_indexScratch.size() >= 3) {
      draw_palette_split(draw, t_indexScratch.data(), static_cast<uint32_t>(t_indexScratch.size()));
      return;
    }
  }
  submit(draw, prim);
}

void draw_indexed(GXVtxFmt fmt, uint16_t vtxCount, const uint8_t* vtxData, uint32_t vtxSize, const uint16_t* indices,
                  uint32_t indexCount, bool bigEndian) noexcept {
  if (!can_draw() || vtxCount == 0 || indexCount < 3) {
    return;
  }
  DecodedDraw draw;
  if (!decode_draw(fmt, vtxCount, vtxData, vtxSize, bigEndian, draw)) {
    return;
  }
  if (!apply_pixel_state()) {
    return;
  }
  apply_transforms(draw);
  apply_tev(draw);
  if (draw.pnMtxOverflow) {
    draw_palette_split(draw, indices, indexCount);
    return;
  }
  set_fvf(draw.fvf);
  g_dx9.dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, draw.vtxCount, indexCount / 3, indices, D3DFMT_INDEX16,
                                    draw.verts, draw.stride);
}

} // namespace aurora::dx9

#endif // AURORA_ENABLE_D3D9
