#include "dx9_tev.hpp"

#ifdef AURORA_ENABLE_D3D9

#include "dx9_texture.hpp"

#include <algorithm>
#include <cmath>

namespace aurora::dx9 {
static Module Log("aurora::dx9::tev");

namespace {

// One TEV operand, classified: either a real D3DTA source or a constant color
// that must be routed through TFACTOR / a per-stage constant.
struct Operand {
  bool isConst = false;
  DWORD ta = D3DTA_CURRENT; // valid when !isConst
  uint32_t constValue = 0;  // ARGB, valid when isConst

  bool is_zero() const noexcept { return isConst && (constValue & 0x00FFFFFFu) == 0 && (constValue >> 24) == 0; }
  bool is_one() const noexcept { return isConst && (constValue & 0x00FFFFFFu) == 0x00FFFFFFu && (constValue >> 24) == 0xFF; }
  bool same_as(const Operand& rhs) const noexcept {
    return isConst == rhs.isConst && (isConst ? constValue == rhs.constValue : ta == rhs.ta);
  }
};

// Per-draw constant allocation: one global TFACTOR + (optionally) one
// D3DTSS_CONSTANT per stage.
struct ConstAlloc {
  uint32_t tfactor = 0;
  bool tfactorUsed = false;
};

inline uint32_t to_u8(float v) noexcept {
  v = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
  return static_cast<uint32_t>(v * 255.f + 0.5f);
}

inline uint32_t pack_argb(const Vec4<float>& c) noexcept {
  return to_u8(c[3]) << 24 | to_u8(c[0]) << 16 | to_u8(c[1]) << 8 | to_u8(c[2]);
}

inline uint32_t pack_gray(float v) noexcept {
  const uint32_t g = to_u8(v);
  return g << 24 | g << 16 | g << 8 | g;
}

// Resolves a konst color selector to ARGB.
uint32_t resolve_kcsel(GXTevKColorSel sel) noexcept {
  switch (sel) {
  case GX_TEV_KCSEL_1:
    return 0xFFFFFFFFu;
  case GX_TEV_KCSEL_7_8:
    return pack_gray(7.f / 8.f);
  case GX_TEV_KCSEL_3_4:
    return pack_gray(3.f / 4.f);
  case GX_TEV_KCSEL_5_8:
    return pack_gray(5.f / 8.f);
  case GX_TEV_KCSEL_1_2:
    return pack_gray(0.5f);
  case GX_TEV_KCSEL_3_8:
    return pack_gray(3.f / 8.f);
  case GX_TEV_KCSEL_1_4:
    return pack_gray(0.25f);
  case GX_TEV_KCSEL_1_8:
    return pack_gray(1.f / 8.f);
  case GX_TEV_KCSEL_K0:
  case GX_TEV_KCSEL_K1:
  case GX_TEV_KCSEL_K2:
  case GX_TEV_KCSEL_K3: {
    const auto& k = g_gxState.kcolors[sel - GX_TEV_KCSEL_K0];
    return pack_argb(k);
  }
  case GX_TEV_KCSEL_K0_R:
  case GX_TEV_KCSEL_K1_R:
  case GX_TEV_KCSEL_K2_R:
  case GX_TEV_KCSEL_K3_R:
    return pack_gray(g_gxState.kcolors[sel - GX_TEV_KCSEL_K0_R][0]);
  case GX_TEV_KCSEL_K0_G:
  case GX_TEV_KCSEL_K1_G:
  case GX_TEV_KCSEL_K2_G:
  case GX_TEV_KCSEL_K3_G:
    return pack_gray(g_gxState.kcolors[sel - GX_TEV_KCSEL_K0_G][1]);
  case GX_TEV_KCSEL_K0_B:
  case GX_TEV_KCSEL_K1_B:
  case GX_TEV_KCSEL_K2_B:
  case GX_TEV_KCSEL_K3_B:
    return pack_gray(g_gxState.kcolors[sel - GX_TEV_KCSEL_K0_B][2]);
  case GX_TEV_KCSEL_K0_A:
  case GX_TEV_KCSEL_K1_A:
  case GX_TEV_KCSEL_K2_A:
  case GX_TEV_KCSEL_K3_A:
    return pack_gray(g_gxState.kcolors[sel - GX_TEV_KCSEL_K0_A][3]);
  default:
    return 0xFFFFFFFFu;
  }
}

float resolve_kasel(GXTevKAlphaSel sel) noexcept {
  switch (sel) {
  case GX_TEV_KASEL_1:
    return 1.f;
  case GX_TEV_KASEL_7_8:
    return 7.f / 8.f;
  case GX_TEV_KASEL_3_4:
    return 3.f / 4.f;
  case GX_TEV_KASEL_5_8:
    return 5.f / 8.f;
  case GX_TEV_KASEL_1_2:
    return 0.5f;
  case GX_TEV_KASEL_3_8:
    return 3.f / 8.f;
  case GX_TEV_KASEL_1_4:
    return 0.25f;
  case GX_TEV_KASEL_1_8:
    return 1.f / 8.f;
  case GX_TEV_KASEL_K0_R:
  case GX_TEV_KASEL_K1_R:
  case GX_TEV_KASEL_K2_R:
  case GX_TEV_KASEL_K3_R:
    return g_gxState.kcolors[sel - GX_TEV_KASEL_K0_R][0];
  case GX_TEV_KASEL_K0_G:
  case GX_TEV_KASEL_K1_G:
  case GX_TEV_KASEL_K2_G:
  case GX_TEV_KASEL_K3_G:
    return g_gxState.kcolors[sel - GX_TEV_KASEL_K0_G][1];
  case GX_TEV_KASEL_K0_B:
  case GX_TEV_KASEL_K1_B:
  case GX_TEV_KASEL_K2_B:
  case GX_TEV_KASEL_K3_B:
    return g_gxState.kcolors[sel - GX_TEV_KASEL_K0_B][2];
  case GX_TEV_KASEL_K0_A:
  case GX_TEV_KASEL_K1_A:
  case GX_TEV_KASEL_K2_A:
  case GX_TEV_KASEL_K3_A:
    return g_gxState.kcolors[sel - GX_TEV_KASEL_K0_A][3];
  default:
    return 1.f;
  }
}

// Rasterized channel -> D3DTA (diffuse/specular) or constant.
Operand ras_operand(GXChannelID channel, const DecodedDraw& draw, bool alphaReplicate) noexcept {
  Operand op;
  switch (channel) {
  case GX_COLOR0A0:
    op.ta = D3DTA_DIFFUSE;
    break;
  case GX_COLOR1A1:
    if (draw.hasSpecular) {
      op.ta = D3DTA_SPECULAR;
    } else {
      op.ta = D3DTA_DIFFUSE;
    }
    break;
  case GX_COLOR_ZERO:
  case GX_COLOR_NULL:
    op.isConst = true;
    op.constValue = 0;
    return op;
  default:
    warn_once(0x4000 | static_cast<uint32_t>(channel), "tev: unsupported raster channel (alpha bump?)");
    op.isConst = true;
    op.constValue = 0;
    return op;
  }
  if (alphaReplicate) {
    op.ta |= D3DTA_ALPHAREPLICATE;
  }
  return op;
}

Operand color_operand(GXTevColorArg arg, const gx::TevStage& stage, uint32_t stageIdx, bool stageHasTexture,
                      const DecodedDraw& draw) noexcept {
  Operand op;
  switch (arg) {
  case GX_CC_CPREV:
    if (stageIdx == 0) {
      op.isConst = true;
      op.constValue = pack_argb(g_gxState.colorRegs[GX_TEVPREV]);
    } else {
      op.ta = D3DTA_CURRENT;
    }
    return op;
  case GX_CC_APREV:
    if (stageIdx == 0) {
      op.isConst = true;
      op.constValue = pack_gray(g_gxState.colorRegs[GX_TEVPREV][3]);
    } else {
      op.ta = D3DTA_CURRENT | D3DTA_ALPHAREPLICATE;
    }
    return op;
  case GX_CC_C0:
  case GX_CC_C1:
  case GX_CC_C2: {
    const uint32_t reg = 1 + (arg - GX_CC_C0) / 2;
    op.isConst = true;
    op.constValue = pack_argb(g_gxState.colorRegs[reg]);
    return op;
  }
  case GX_CC_A0:
  case GX_CC_A1:
  case GX_CC_A2: {
    const uint32_t reg = 1 + (arg - GX_CC_A0) / 2;
    op.isConst = true;
    op.constValue = pack_gray(g_gxState.colorRegs[reg][3]);
    return op;
  }
  case GX_CC_TEXC:
    if (!stageHasTexture) {
      warn_once(0x4100, "tev: TEXC referenced without a bound texture");
      op.isConst = true;
      op.constValue = 0;
      return op;
    }
    op.ta = D3DTA_TEXTURE;
    return op;
  case GX_CC_TEXA:
    if (!stageHasTexture) {
      op.isConst = true;
      op.constValue = 0;
      return op;
    }
    op.ta = D3DTA_TEXTURE | D3DTA_ALPHAREPLICATE;
    return op;
  case GX_CC_RASC:
    return ras_operand(stage.channelId, draw, false);
  case GX_CC_RASA:
    return ras_operand(stage.channelId, draw, true);
  case GX_CC_ONE:
    op.isConst = true;
    op.constValue = 0xFFFFFFFFu;
    return op;
  case GX_CC_HALF:
    op.isConst = true;
    op.constValue = pack_gray(0.5f);
    return op;
  case GX_CC_KONST:
    op.isConst = true;
    op.constValue = resolve_kcsel(stage.kcSel);
    return op;
  case GX_CC_ZERO:
  default:
    op.isConst = true;
    op.constValue = 0;
    return op;
  }
}

Operand alpha_operand(GXTevAlphaArg arg, const gx::TevStage& stage, uint32_t stageIdx, bool stageHasTexture,
                      const DecodedDraw& draw) noexcept {
  Operand op;
  switch (arg) {
  case GX_CA_APREV:
    if (stageIdx == 0) {
      op.isConst = true;
      op.constValue = pack_gray(g_gxState.colorRegs[GX_TEVPREV][3]);
    } else {
      op.ta = D3DTA_CURRENT;
    }
    return op;
  case GX_CA_A0:
  case GX_CA_A1:
  case GX_CA_A2: {
    const uint32_t reg = 1 + (arg - GX_CA_A0);
    op.isConst = true;
    op.constValue = pack_gray(g_gxState.colorRegs[reg][3]);
    return op;
  }
  case GX_CA_TEXA:
    if (!stageHasTexture) {
      op.isConst = true;
      op.constValue = 0;
      return op;
    }
    op.ta = D3DTA_TEXTURE;
    return op;
  case GX_CA_RASA: {
    Operand ras = ras_operand(stage.channelId, draw, false);
    return ras;
  }
  case GX_CA_KONST:
    op.isConst = true;
    op.constValue = pack_gray(resolve_kasel(stage.kaSel));
    return op;
  case GX_CA_ZERO:
  default:
    op.isConst = true;
    op.constValue = 0;
    return op;
  }
}

// Materializes an operand into a D3DTA value for this stage, routing
// constants through TFACTOR or the per-stage constant.
DWORD materialize(const Operand& op, ConstAlloc& consts, DWORD stage, bool& stageConstUsed, uint32_t& stageConstValue,
                  uint64_t warnKey) noexcept {
  if (!op.isConst) {
    return op.ta;
  }
  if (consts.tfactorUsed && consts.tfactor == op.constValue) {
    return D3DTA_TFACTOR;
  }
  if (!consts.tfactorUsed) {
    consts.tfactor = op.constValue;
    consts.tfactorUsed = true;
    return D3DTA_TFACTOR;
  }
  if (g_dx9.perStageConstants) {
    if (stageConstUsed && stageConstValue != op.constValue) {
      warn_once(warnKey, "tev: more than two distinct constants in one stage");
      return D3DTA_TFACTOR;
    }
    stageConstUsed = true;
    stageConstValue = op.constValue;
    return D3DTA_CONSTANT;
  }
  warn_once(warnKey + 1, "tev: multiple constants without per-stage constant support");
  return D3DTA_TFACTOR;
}

struct ReducedOp {
  DWORD op = D3DTOP_SELECTARG1;
  Operand arg1;
  Operand arg2;
  Operand arg0;
  bool usesArg2 = false;
  bool usesArg0 = false;
  bool complementArg2 = false; // apply D3DTA_COMPLEMENT to arg2
  bool complementArg0 = false;
};

// Reduces one TEV pass (out = d +/- (a*(1-c) + b*c), bias, scale) to a TSS op.
// `hash` seeds warn_once keys so each distinct unsupported shape logs once.
ReducedOp reduce_pass(Operand a, Operand b, Operand c, Operand d, const gx::TevOp& op, uint64_t hash) noexcept {
  ReducedOp r;

  const bool isCompare = op.op >= GX_TEV_COMP_R8_GT;
  if (isCompare) {
    warn_once(hash ^ 0x10, "tev: compare-mode op approximated as pass-through");
    r.op = D3DTOP_SELECTARG1;
    r.arg1 = d.is_zero() ? Operand{.ta = D3DTA_CURRENT} : d;
    return r;
  }

  // Normalize the lerp term.
  Operand term; // single-term replacement when the lerp collapses
  bool termIsLerp = false;
  bool termIsModulate = false;
  Operand modX, modY; // term = modX * modY (modY may be complemented via flag)
  bool modYComplement = false;

  if (c.is_zero() || a.same_as(b)) {
    term = a;
  } else if (c.is_one()) {
    term = b;
  } else if (a.is_zero()) {
    termIsModulate = true;
    modX = b;
    modY = c;
  } else if (b.is_zero()) {
    termIsModulate = true;
    modX = a;
    modY = c;
    modYComplement = true;
  } else {
    termIsLerp = true;
  }

  const bool subtract = op.op == GX_TEV_SUB;

  if (termIsLerp) {
    // out = d + lerp; only representable without d.
    if (!d.is_zero()) {
      warn_once(hash ^ 0x11, "tev: lerp with additive d approximated (d dropped)");
    }
    if (subtract) {
      warn_once(hash ^ 0x12, "tev: subtractive lerp approximated as lerp");
    }
    r.op = D3DTOP_LERP; // arg0*arg1 + (1-arg0)*arg2
    r.arg0 = c;
    r.arg1 = b;
    r.arg2 = a;
    r.usesArg0 = true;
    r.usesArg2 = true;
    return r;
  }

  if (termIsModulate) {
    if (d.is_zero()) {
      if (subtract) {
        warn_once(hash ^ 0x13, "tev: 0 - x*y approximated as x*y");
      }
      r.op = op.scale == GX_CS_SCALE_2 ? D3DTOP_MODULATE2X
             : op.scale == GX_CS_SCALE_4 ? D3DTOP_MODULATE4X
                                         : D3DTOP_MODULATE;
      if (op.scale == GX_CS_DIVIDE_2) {
        warn_once(hash ^ 0x14, "tev: divide-2 scale ignored");
      }
      r.arg1 = modX;
      r.arg2 = modY;
      r.usesArg2 = true;
      r.complementArg2 = modYComplement;
      return r;
    }
    if (subtract) {
      warn_once(hash ^ 0x15, "tev: d - x*y approximated as subtract(d, x)");
      r.op = D3DTOP_SUBTRACT;
      r.arg1 = d;
      r.arg2 = modX;
      r.usesArg2 = true;
      return r;
    }
    // d + x*y -> MULTIPLYADD(arg0=d, arg1=x, arg2=y)
    r.op = D3DTOP_MULTIPLYADD;
    r.arg0 = d;
    r.arg1 = modX;
    r.arg2 = modY;
    r.usesArg0 = true;
    r.usesArg2 = true;
    r.complementArg2 = modYComplement;
    return r;
  }

  // Single term.
  if (d.is_zero()) {
    if (subtract) {
      warn_once(hash ^ 0x16, "tev: 0 - x approximated as x");
    }
    if (op.bias == GX_TB_SUBHALF || op.bias == GX_TB_ADDHALF) {
      warn_once(hash ^ 0x17, "tev: bias on single-term pass ignored");
    }
    r.op = D3DTOP_SELECTARG1;
    r.arg1 = term;
    return r;
  }
  if (term.is_zero()) {
    r.op = D3DTOP_SELECTARG1;
    r.arg1 = d;
    return r;
  }
  if (subtract) {
    r.op = D3DTOP_SUBTRACT;
    r.arg1 = d;
    r.arg2 = term;
    r.usesArg2 = true;
    return r;
  }
  // d + term (+ bias): ADDSIGNED family covers bias -0.5.
  if (op.bias == GX_TB_SUBHALF) {
    r.op = op.scale == GX_CS_SCALE_2 ? D3DTOP_ADDSIGNED2X : D3DTOP_ADDSIGNED;
  } else {
    if (op.bias == GX_TB_ADDHALF) {
      warn_once(hash ^ 0x18, "tev: +0.5 bias ignored");
    }
    if (op.scale != GX_CS_SCALE_1) {
      warn_once(hash ^ 0x19, "tev: scale on add ignored");
    }
    r.op = D3DTOP_ADD;
  }
  r.arg1 = d;
  r.arg2 = term;
  r.usesArg2 = true;
  return r;
}

// Applies texgen for a stage: returns the D3DTSS_TEXCOORDINDEX value and sets
// the texture matrix / transform flags.
DWORD apply_texgen(uint32_t d3dStage, GXTexCoordID coordId, const DecodedDraw& draw) noexcept {
  if (coordId == GX_TEXCOORD_NULL || coordId >= gx::MaxTexCoord) {
    set_tss(d3dStage, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    return 0;
  }
  const auto& tcg = g_gxState.tcgs[coordId];

  DWORD index = 0;
  DWORD tci = 0;
  bool cameraSpace = false;
  switch (tcg.src) {
  case GX_TG_POS:
    tci = D3DTSS_TCI_CAMERASPACEPOSITION;
    cameraSpace = true;
    break;
  case GX_TG_NRM:
    tci = D3DTSS_TCI_CAMERASPACENORMAL;
    cameraSpace = true;
    warn_once(0x5000, "texgen: NRM source approximated with camera-space normal");
    break;
  default:
    if (tcg.src >= GX_TG_TEX0 && tcg.src <= GX_TG_TEX7) {
      const int attr = tcg.src - GX_TG_TEX0;
      index = draw.texSlot[attr] >= 0 ? static_cast<DWORD>(draw.texSlot[attr]) : 0;
    } else {
      warn_once(0x5001 | static_cast<uint32_t>(tcg.src) << 8, "texgen: unsupported source (SRTG/bump)");
      set_tss(d3dStage, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
      return 0;
    }
    break;
  }

  const bool projected = tcg.type == GX_TG_MTX3x4 && cameraSpace;
  const bool hasMtx = tcg.mtx != GX_IDENTITY;
  const bool hasPostMtx = tcg.postMtx != GX_PTIDENTITY;

  if (!hasMtx && !hasPostMtx && !cameraSpace) {
    set_tss(d3dStage, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    return index;
  }

  // Build the texture matrix in D3D row-vector layout.
  D3DMATRIX m{{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}}};
  if (hasMtx) {
    const uint32_t mtxIdx = (static_cast<uint32_t>(tcg.mtx) - GX_TEXMTX0) / 3;
    if (mtxIdx < gx::MaxTexMtx) {
      const auto& gxm = g_gxState.texMtxs[mtxIdx];
      if (cameraSpace) {
        // GX texgen reads the *model-space* position/normal, but D3D's
        // camera-space TCI inputs are view-space; premultiply the model-view
        // inverse to restore GX semantics (projected shadows, env maps).
        m = to_d3d(gxm);
        if (g_worldViewInv.valid) {
          m = mtx_multiply(tcg.src == GX_TG_POS ? g_worldViewInv.full : g_worldViewInv.rotation, m);
        } else {
          warn_once(0x5002, "texgen: camera-space source without invertible world (skinned draw?)");
        }
      } else {
        // Input is (s, t): D3D expands 2D coords with a third component of 1,
        // so fold the GX (s,t,1,1) constant terms together.
        m = D3DMATRIX{{{
            gxm.m0[0], gxm.m1[0], 0, 0, //
            gxm.m0[1], gxm.m1[1], 0, 0, //
            gxm.m0[2] + gxm.m0[3], gxm.m1[2] + gxm.m1[3], 1, 0, //
            0, 0, 0, 1, //
        }}};
      }
    }
  } else if (cameraSpace) {
    // Identity texmtx on a camera-space input: still undo the view-space
    // transform so the coordinates match GX's model-space inputs.
    if (g_worldViewInv.valid) {
      m = tcg.src == GX_TG_POS ? g_worldViewInv.full : g_worldViewInv.rotation;
    }
  }
  if (hasPostMtx) {
    const uint32_t postIdx = static_cast<uint32_t>(tcg.postMtx) - GX_PTTEXMTX0;
    if (postIdx / 3 < gx::MaxPTTexMtx) {
      const D3DMATRIX post = to_d3d(g_gxState.ptTexMtxs[postIdx / 3]);
      // Row-vector composition: v * m * post.
      D3DMATRIX result{};
      for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
          float sum = 0.f;
          for (int k = 0; k < 4; ++k) {
            sum += m.m[row][k] * post.m[k][col];
          }
          result.m[row][col] = sum;
        }
      }
      m = result;
    }
  }

  set_texture_matrix(d3dStage, m);
  DWORD ttff = projected ? (D3DTTFF_COUNT3 | D3DTTFF_PROJECTED) : D3DTTFF_COUNT2;
  set_tss(d3dStage, D3DTSS_TEXTURETRANSFORMFLAGS, ttff);
  return tci | index;
}

} // namespace

uint32_t apply_tev(const DecodedDraw& draw) noexcept {
  const uint32_t numStages = std::max<uint32_t>(g_gxState.numTevStages, 1);
  ConstAlloc consts;
  uint32_t d3dStage = 0;

  for (uint32_t i = 0; i < numStages && d3dStage < MaxStages; ++i) {
    const auto& stage = g_gxState.tevStages[i];

    // Indirect stages are not representable; the base lookup still happens.
    if (stage.indTexMtxId != GX_ITM_OFF || stage.indTexBiasSel != GX_ITB_NONE) {
      warn_once(0x6000 | i, "tev: indirect texturing ignored");
    }
    if (stage.colorOp.outReg != GX_TEVPREV || stage.alphaOp.outReg != GX_TEVPREV) {
      warn_once(0x6100 | i, "tev: non-PREV output register treated as PREV");
    }

    const bool hasTexture = stage.texMapId != GX_TEXMAP_NULL && stage.texMapId < static_cast<int>(gx::MaxTextures) &&
                            stage.texCoordId != GX_TEXCOORD_NULL;

    // Reduce both passes.
    const uint64_t cfgHash = xxh3_hash(stage, 0);
    const Operand ca = color_operand(stage.colorPass.a, stage, i, hasTexture, draw);
    const Operand cb = color_operand(stage.colorPass.b, stage, i, hasTexture, draw);
    const Operand cc = color_operand(stage.colorPass.c, stage, i, hasTexture, draw);
    const Operand cd = color_operand(stage.colorPass.d, stage, i, hasTexture, draw);
    ReducedOp colorOp = reduce_pass(ca, cb, cc, cd, stage.colorOp, cfgHash);

    const Operand aa = alpha_operand(stage.alphaPass.a, stage, i, hasTexture, draw);
    const Operand ab = alpha_operand(stage.alphaPass.b, stage, i, hasTexture, draw);
    const Operand ac = alpha_operand(stage.alphaPass.c, stage, i, hasTexture, draw);
    const Operand ad = alpha_operand(stage.alphaPass.d, stage, i, hasTexture, draw);
    ReducedOp alphaOp = reduce_pass(aa, ab, ac, ad, stage.alphaOp, cfgHash ^ 0xA1FA);

    // Skip pure pass-through stages (both passes keep CURRENT) to save the
    // 8-stage budget for meaningful work.
    const auto is_current_pass = [](const ReducedOp& r) {
      return r.op == D3DTOP_SELECTARG1 && !r.arg1.isConst && (r.arg1.ta & ~D3DTA_ALPHAREPLICATE) == D3DTA_CURRENT;
    };
    if (d3dStage > 0 && is_current_pass(colorOp) && is_current_pass(alphaOp)) {
      continue;
    }

    // Bind texture + sampler + texgen for this stage.
    if (hasTexture) {
      IDirect3DBaseTexture9* tex = resolve_texmap(stage.texMapId);
      set_texture(d3dStage, tex);
      if (tex != nullptr) {
        apply_sampler(d3dStage, stage.texMapId);
      }
      set_tss(d3dStage, D3DTSS_TEXCOORDINDEX, apply_texgen(d3dStage, stage.texCoordId, draw));
    } else {
      set_texture(d3dStage, nullptr);
      set_tss(d3dStage, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
      set_tss(d3dStage, D3DTSS_TEXCOORDINDEX, 0);
    }

    // Materialize operands (constants -> TFACTOR / per-stage constant).
    bool stageConstUsed = false;
    uint32_t stageConstValue = 0;
    const auto mat = [&](const Operand& op, uint64_t key) {
      return materialize(op, consts, d3dStage, stageConstUsed, stageConstValue, cfgHash ^ key);
    };

    DWORD carg1 = mat(colorOp.arg1, 0x20);
    DWORD carg2 = colorOp.usesArg2 ? mat(colorOp.arg2, 0x21) : D3DTA_CURRENT;
    DWORD carg0 = colorOp.usesArg0 ? mat(colorOp.arg0, 0x22) : D3DTA_CURRENT;
    if (colorOp.complementArg2) {
      carg2 |= D3DTA_COMPLEMENT;
    }
    DWORD aarg1 = mat(alphaOp.arg1, 0x30);
    DWORD aarg2 = alphaOp.usesArg2 ? mat(alphaOp.arg2, 0x31) : D3DTA_CURRENT;
    DWORD aarg0 = alphaOp.usesArg0 ? mat(alphaOp.arg0, 0x32) : D3DTA_CURRENT;
    if (alphaOp.complementArg2) {
      aarg2 |= D3DTA_COMPLEMENT;
    }

    set_tss(d3dStage, D3DTSS_COLOROP, colorOp.op);
    set_tss(d3dStage, D3DTSS_COLORARG1, carg1);
    set_tss(d3dStage, D3DTSS_COLORARG2, carg2);
    set_tss(d3dStage, D3DTSS_COLORARG0, carg0);
    set_tss(d3dStage, D3DTSS_ALPHAOP, alphaOp.op);
    set_tss(d3dStage, D3DTSS_ALPHAARG1, aarg1);
    set_tss(d3dStage, D3DTSS_ALPHAARG2, aarg2);
    set_tss(d3dStage, D3DTSS_ALPHAARG0, aarg0);
    if (stageConstUsed) {
      set_tss(d3dStage, D3DTSS_CONSTANT, stageConstValue);
    }
    ++d3dStage;
  }

  if (g_gxState.numTevStages > MaxStages) {
    warn_once(0x6200 | g_gxState.numTevStages, "tev: more than 8 stages truncated");
  }

  if (d3dStage == 0) {
    // Nothing survived reduction: emit vertex color.
    set_texture(0, nullptr);
    set_tss(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    set_tss(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    set_tss(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    set_tss(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    set_tss(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    d3dStage = 1;
  }

  // Destination-alpha override: force the final alpha to the constant.
  if (g_gxState.dstAlpha != UINT32_MAX && g_gxState.alphaUpdate && d3dStage <= MaxStages) {
    const uint32_t alpha = g_gxState.dstAlpha & 0xFF;
    const uint32_t desired = alpha << 24 | (consts.tfactorUsed ? consts.tfactor & 0x00FFFFFFu : 0);
    if (consts.tfactorUsed && (consts.tfactor >> 24) != alpha) {
      warn_once(0x6300, "tev: dstAlpha conflicts with constant alpha; using TFACTOR alpha");
    }
    consts.tfactor = desired;
    consts.tfactorUsed = true;
    if (d3dStage < MaxStages) {
      set_texture(d3dStage, nullptr);
      set_tss(d3dStage, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
      set_tss(d3dStage, D3DTSS_COLORARG1, D3DTA_CURRENT);
      set_tss(d3dStage, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
      set_tss(d3dStage, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
      set_tss(d3dStage, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
      set_tss(d3dStage, D3DTSS_TEXCOORDINDEX, 0);
      ++d3dStage;
    }
  }

  if (consts.tfactorUsed) {
    set_rs(D3DRS_TEXTUREFACTOR, consts.tfactor);
  }

  // Terminate the stage chain.
  if (d3dStage < MaxStages) {
    set_tss(d3dStage, D3DTSS_COLOROP, D3DTOP_DISABLE);
    set_tss(d3dStage, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
  }
  return d3dStage;
}

} // namespace aurora::dx9

#endif // AURORA_ENABLE_D3D9
