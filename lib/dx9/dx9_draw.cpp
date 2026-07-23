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

  // Fog: disabled in v1 (Remix replaces atmospherics; docs #11).
  set_rs(D3DRS_FOGENABLE, FALSE);
  return true;
}

void apply_transforms(const DecodedDraw& draw) noexcept {
  set_proj_matrix(to_d3d_proj(g_gxState.proj));

  g_worldViewInv.valid = false;
  const D3DMATRIX identity{{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}}};
  if (draw.skinned) {
    // Fixed-function indexed vertex blending against the bone palette; the
    // model->view base matrix rides in VIEW (applied after the blend).
    const uint32_t jointCount = std::min<uint32_t>(g_skin.jointCount, MaxWorldPalette);
    if (g_skin.jointCount > MaxWorldPalette) {
      warn_once(0x8000, "skinning: joint count exceeds 256, clamping");
    }
    const uint32_t maxIdx = g_dx9.caps.MaxVertexBlendMatrixIndex;
    if (jointCount > maxIdx + 1) {
      warn_once(0x8001 | jointCount << 8, "skinning: joint count exceeds device MaxVertexBlendMatrixIndex");
    }
    for (uint32_t i = 0; i < jointCount; ++i) {
      set_world_matrix(i, to_d3d_3x4(g_skin.palette + static_cast<size_t>(i) * 12));
    }
    set_view_matrix(to_d3d_3x4(g_gxState.skinBaseMtx.data()));
    static constexpr DWORD kBlendMode[] = {D3DVBF_0WEIGHTS, D3DVBF_1WEIGHTS, D3DVBF_2WEIGHTS, D3DVBF_3WEIGHTS};
    set_rs(D3DRS_VERTEXBLEND, kBlendMode[draw.weightCount]);
    set_rs(D3DRS_INDEXEDVERTEXBLENDENABLE, TRUE);
  } else if (draw.hasPnMtxIdx) {
    // Matrix-palette draws (J3D characters): the 10 GX position matrices
    // become the world matrix palette, selected per vertex with weight 1.
    for (uint32_t i = 0; i < gx::MaxPnMtx; ++i) {
      set_world_matrix(i, to_d3d(g_gxState.pnMtx[i].pos));
    }
    set_view_matrix(identity);
    set_rs(D3DRS_VERTEXBLEND, D3DVBF_0WEIGHTS);
    set_rs(D3DRS_INDEXEDVERTEXBLENDENABLE, TRUE);
  } else {
    const D3DMATRIX world = to_d3d(g_gxState.pnMtx[g_gxState.currentPnMtx].pos);
    set_world_matrix(0, world);
    set_view_matrix(identity);
    set_rs(D3DRS_VERTEXBLEND, D3DVBF_DISABLE);
    set_rs(D3DRS_INDEXEDVERTEXBLENDENABLE, FALSE);
    // Camera-space texgen compensation (docs #7): D3D feeds view-space
    // inputs where GX texgen reads model-space; premultiplying the texture
    // matrix with the model-view inverse restores GX semantics (projected
    // shadows/light shafts, env maps). Only well-defined for rigid draws.
    if (mtx_affine_inverse(world, g_worldViewInv.full)) {
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
    for (uint16_t v = 0; v + 3 < vtxCount; v += 4) {
      out.insert(out.end(), {v, static_cast<uint16_t>(v + 1), static_cast<uint16_t>(v + 2),
                             static_cast<uint16_t>(v + 2), static_cast<uint16_t>(v + 3), v});
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
  set_fvf(draw.fvf);
  g_dx9.dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, draw.vtxCount, indexCount / 3, indices, D3DFMT_INDEX16,
                                    draw.verts, draw.stride);
}

} // namespace aurora::dx9

#endif // AURORA_ENABLE_D3D9
