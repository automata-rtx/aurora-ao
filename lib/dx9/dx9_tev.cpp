#include "dx9_tev.hpp"

#ifdef AURORA_ENABLE_D3D9

#include "dx9_texture.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

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

// One TSS operation. A reduced GX pass expands to at most: an optional
// TEMP-writing op (runs first, leaves CURRENT untouched via
// D3DTSS_RESULTARG=TEMP), a main op, and an optional post-scale op.
struct PassOp {
  DWORD op = D3DTOP_SELECTARG1;
  Operand arg1; // Operand default = CURRENT, so a default PassOp is a pass-through
  Operand arg2;
  Operand arg0;
  bool usesArg2 = false;
  bool usesArg0 = false;
  bool complementArg2 = false; // apply D3DTA_COMPLEMENT to arg2
};

struct ReducedPass {
  bool hasTemp = false;
  PassOp tempOp;
  std::array<PassOp, 2> finals{}; // main op, then optional post-scale
  int finalCount = 1;
};

inline Operand current_operand() noexcept { return Operand{}; }
inline Operand temp_operand() noexcept {
  Operand o;
  o.ta = D3DTA_TEMP;
  return o;
}
inline Operand white_operand() noexcept {
  Operand o;
  o.isConst = true;
  o.constValue = 0xFFFFFFFFu;
  return o;
}

// --- RTX Remix material reconstruction -------------------------------------
//
// Remix rebuilds a draw call's material from exactly ONE texture stage - the
// first stage bound to the lowest D3DTSS_TEXCOORDINDEX
// (D3D9Rtx::processTextures) - and that stage's color/alpha op and args become
// the surface's entire albedo and opacity (d3d9_rtx_utils.cpp
// setTextureStageState -> opaque_surface_material_interaction.slangh).
//
// Args it cannot decode (D3DTA_TEMP, D3DTA_CONSTANT, anything carrying
// D3DTA_COMPLEMENT/D3DTA_ALPHAREPLICATE) become RtTextureArgSource::None,
// which the shader resolves to *identity* - vec3(1.0) for color, the sampled
// opacity for alpha arg1 - so those are harmless on their own.
//
// The damage comes from the single-stage view itself: a GX material's first
// TEV stage is rarely the finished albedo. When it computes, say,
// `texture x konst` with a dark konst (which we route through TFACTOR), Remix
// takes that partial result as the whole albedo and the surface renders black
// or near-black - while the texture is resident and correctly bound, so it
// still appears in Remix's texture list. The same applies to alpha: that one
// stage's alpha becomes the whole opacity, so alpha-tested cutouts lose their
// shape (foliage cards render as full quads) or vanish entirely (grass) when
// the first stage's alpha is a blend weight rather than the texture's alpha.
//
// So unless the leading stage already presents the texture the way Remix will
// read it, prepend a stage that does: color = TEXTURE * DIFFUSE, alpha =
// TEXTURE. It writes CURRENT, which the real chain then overwrites, and is
// only emitted when nothing in that GX stage reads CURRENT - so the
// rasterized result is unchanged. (A draw with no vertex colors resolves
// DIFFUSE to None = identity on both sides, so the modulate is a no-op there.)
constexpr DWORD arg_base(DWORD ta) noexcept { return ta & ~(D3DTA_COMPLEMENT | D3DTA_ALPHAREPLICATE); }

// True when this op hands Remix the texture as-is, optionally modulated by
// vertex color - i.e. exactly what the hint stage below would say, so emitting
// the hint would only cost a stage.
bool op_is_plain_texture(const PassOp& op) noexcept {
  if (op.op != D3DTOP_SELECTARG1 && op.op != D3DTOP_MODULATE) {
    return false;
  }
  if (op.complementArg2 || op.usesArg0) {
    return false;
  }
  bool sawTexture = false;
  const auto plain = [&sawTexture](const Operand& o) {
    if (o.isConst || o.ta != arg_base(o.ta)) {
      return false;
    }
    if (o.ta == D3DTA_TEXTURE) {
      sawTexture = true;
      return true;
    }
    return o.ta == D3DTA_DIFFUSE;
  };
  if (!plain(op.arg1)) {
    return false;
  }
  if (op.usesArg2 && !plain(op.arg2)) {
    return false;
  }
  return sawTexture;
}

// Intensity-only GX formats carry no colour: they are masks - eye highlights,
// eye shadows, gradient ramps. When a material mixes them with a colour
// texture, the colour one is the albedo Remix should be shown. Character eyes
// are the case that forced this: their first textured stage samples a 32x32 I8
// highlight mask, with the actual eyeball (64x64 CMPR) two stages later, so
// taking the first textured stage handed Remix a grey blob for the eye.
bool is_color_texture_format(uint32_t fmt) noexcept {
  switch (fmt) {
  case GX_TF_I4:
  case GX_TF_I8:
  case GX_TF_IA4:
  case GX_TF_IA8:
    return false;
  default:
    return true;
  }
}

// GX stage whose texture should become the albedo: the first one sampling a
// colour texture, else simply the first textured stage.
int preferred_albedo_stage() noexcept {
  int firstTextured = -1;
  for (uint32_t i = 0; i < g_gxState.numTevStages; ++i) {
    const auto& s = g_gxState.tevStages[i];
    if (s.texMapId == GX_TEXMAP_NULL || s.texMapId >= static_cast<int>(gx::MaxTextures) ||
        s.texCoordId == GX_TEXCOORD_NULL) {
      continue;
    }
    if (firstTextured < 0) {
      firstTextured = static_cast<int>(i);
    }
    if (is_color_texture_format(g_gxState.loadedTextures[static_cast<size_t>(s.texMapId)].format())) {
      return static_cast<int>(i);
    }
  }
  return firstTextured;
}

bool op_reads(const PassOp& op, DWORD source) noexcept {
  const auto is = [source](const Operand& o) { return !o.isConst && arg_base(o.ta) == source; };
  return is(op.arg1) || (op.usesArg2 && is(op.arg2)) || (op.usesArg0 && is(op.arg0));
}

bool pass_reads(const ReducedPass& r, DWORD source) noexcept {
  if (r.hasTemp && op_reads(r.tempOp, source)) {
    return true;
  }
  for (int i = 0; i < r.finalCount; ++i) {
    if (op_reads(r.finals[i], source)) {
      return true;
    }
  }
  return false;
}

// Appends a x2/x4 post-multiply stage (MODULATE2X/4X against white).
void append_scale(ReducedPass& r, GXTevScale scale, uint64_t hash, bool allowMulti) noexcept {
  if (scale == GX_CS_SCALE_1) {
    return;
  }
  if (scale == GX_CS_DIVIDE_2) {
    warn_once(hash ^ 0x14, "tev: divide-2 scale ignored");
    return;
  }
  if (!allowMulti || r.finalCount >= 2) {
    warn_once(hash ^ 0x19, "tev: scale on add ignored (no stage budget)");
    return;
  }
  PassOp s;
  s.op = scale == GX_CS_SCALE_4 ? D3DTOP_MODULATE4X : D3DTOP_MODULATE2X;
  s.arg1 = current_operand();
  s.arg2 = white_operand();
  s.usesArg2 = true;
  r.finals[r.finalCount++] = s;
}

// Reduces one TEV pass (out = d +/- (a*(1-c) + b*c), bias, scale) to TSS ops.
// `hash` seeds warn_once keys so each distinct unsupported shape logs once.
// allowMulti permits multi-stage decomposition (TEMP register, post-scale);
// when false the pass collapses to the closest single op.
ReducedPass reduce_pass(Operand a, Operand b, Operand c, Operand d, const gx::TevOp& op, uint64_t hash,
                        bool allowMulti) noexcept {
  ReducedPass r;
  PassOp& main = r.finals[0];
  const bool useTemp = allowMulti && g_dx9.tssTemp;

  const bool isCompare = op.op >= GX_TEV_COMP_R8_GT;
  if (isCompare) {
    // out = d + (compare ? c : 0). Assume the compare passes: masks built
    // this way keep their visible texels (dropping c made them invisible).
    warn_once(hash ^ 0x10, "tev: compare-mode op approximated as always-true (d + c)");
    if (c.is_zero() || d.is_zero()) {
      main.op = D3DTOP_SELECTARG1;
      main.arg1 = c.is_zero() ? (d.is_zero() ? d /* zero const */ : d) : c;
    } else {
      main.op = D3DTOP_ADD;
      main.arg1 = d;
      main.arg2 = c;
      main.usesArg2 = true;
    }
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
    if (d.is_zero()) {
      if (subtract) {
        warn_once(hash ^ 0x12, "tev: subtractive lerp approximated as lerp");
      }
      main.op = D3DTOP_LERP; // arg0*arg1 + (1-arg0)*arg2
      main.arg0 = c;
      main.arg1 = b;
      main.arg2 = a;
      main.usesArg0 = true;
      main.usesArg2 = true;
      append_scale(r, op.scale, hash, allowMulti);
      return r;
    }
    if (useTemp) {
      // Exact: lerp into TEMP (CURRENT preserved), then d +/- TEMP.
      r.hasTemp = true;
      r.tempOp.op = D3DTOP_LERP;
      r.tempOp.arg0 = c;
      r.tempOp.arg1 = b;
      r.tempOp.arg2 = a;
      r.tempOp.usesArg0 = true;
      r.tempOp.usesArg2 = true;
      main.op = subtract ? D3DTOP_SUBTRACT : D3DTOP_ADD;
      main.arg1 = d;
      main.arg2 = temp_operand();
      main.usesArg2 = true;
      append_scale(r, op.scale, hash, allowMulti);
      return r;
    }
    warn_once(hash ^ 0x11, "tev: lerp with additive d approximated (d dropped, no TEMP support)");
    main.op = D3DTOP_LERP;
    main.arg0 = c;
    main.arg1 = b;
    main.arg2 = a;
    main.usesArg0 = true;
    main.usesArg2 = true;
    return r;
  }

  if (termIsModulate) {
    if (d.is_zero()) {
      if (subtract) {
        warn_once(hash ^ 0x13, "tev: 0 - x*y approximated as x*y");
      }
      main.op = op.scale == GX_CS_SCALE_2   ? D3DTOP_MODULATE2X
                : op.scale == GX_CS_SCALE_4 ? D3DTOP_MODULATE4X
                                            : D3DTOP_MODULATE;
      if (op.scale == GX_CS_DIVIDE_2) {
        warn_once(hash ^ 0x14, "tev: divide-2 scale ignored");
      }
      main.arg1 = modX;
      main.arg2 = modY;
      main.usesArg2 = true;
      main.complementArg2 = modYComplement;
      return r;
    }
    if (subtract) {
      if (useTemp) {
        // Exact: x*y into TEMP, then d - TEMP.
        r.hasTemp = true;
        r.tempOp.op = D3DTOP_MODULATE;
        r.tempOp.arg1 = modX;
        r.tempOp.arg2 = modY;
        r.tempOp.usesArg2 = true;
        r.tempOp.complementArg2 = modYComplement;
        main.op = D3DTOP_SUBTRACT;
        main.arg1 = d;
        main.arg2 = temp_operand();
        main.usesArg2 = true;
        append_scale(r, op.scale, hash, allowMulti);
        return r;
      }
      warn_once(hash ^ 0x15, "tev: d - x*y approximated as subtract(d, x)");
      main.op = D3DTOP_SUBTRACT;
      main.arg1 = d;
      main.arg2 = modX;
      main.usesArg2 = true;
      return r;
    }
    // d + x*y -> MULTIPLYADD(arg0=d, arg1=x, arg2=y)
    main.op = D3DTOP_MULTIPLYADD;
    main.arg0 = d;
    main.arg1 = modX;
    main.arg2 = modY;
    main.usesArg0 = true;
    main.usesArg2 = true;
    main.complementArg2 = modYComplement;
    append_scale(r, op.scale, hash, allowMulti);
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
    if (op.scale == GX_CS_SCALE_2 || op.scale == GX_CS_SCALE_4) {
      main.op = op.scale == GX_CS_SCALE_4 ? D3DTOP_MODULATE4X : D3DTOP_MODULATE2X;
      main.arg1 = term;
      main.arg2 = white_operand();
      main.usesArg2 = true;
    } else {
      main.op = D3DTOP_SELECTARG1;
      main.arg1 = term;
    }
    return r;
  }
  if (term.is_zero()) {
    if (op.scale == GX_CS_SCALE_2 || op.scale == GX_CS_SCALE_4) {
      main.op = op.scale == GX_CS_SCALE_4 ? D3DTOP_MODULATE4X : D3DTOP_MODULATE2X;
      main.arg1 = d;
      main.arg2 = white_operand();
      main.usesArg2 = true;
    } else {
      main.op = D3DTOP_SELECTARG1;
      main.arg1 = d;
    }
    return r;
  }
  if (subtract) {
    main.op = D3DTOP_SUBTRACT;
    main.arg1 = d;
    main.arg2 = term;
    main.usesArg2 = true;
    append_scale(r, op.scale, hash, allowMulti);
    return r;
  }
  // d + term (+ bias): ADDSIGNED family covers bias -0.5.
  if (op.bias == GX_TB_SUBHALF) {
    main.op = op.scale == GX_CS_SCALE_2 ? D3DTOP_ADDSIGNED2X : D3DTOP_ADDSIGNED;
  } else {
    if (op.bias == GX_TB_ADDHALF) {
      warn_once(hash ^ 0x18, "tev: +0.5 bias ignored");
    }
    main.op = D3DTOP_ADD;
    main.arg1 = d;
    main.arg2 = term;
    main.usesArg2 = true;
    append_scale(r, op.scale, hash, allowMulti);
    return r;
  }
  main.arg1 = d;
  main.arg2 = term;
  main.usesArg2 = true;
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

// The constant that tints the material's albedo, when it has one.
//
// Remix rebuilds a surface from a single stage and, among our two constant
// slots, understands only D3DTA_TFACTOR - it never reads D3DTSS_CONSTANT
// anywhere in its capture path. An arg it cannot decode becomes
// RtTextureArgSource::None, which the shader resolves to *identity*, i.e.
// white. So a lost tint does not darken a surface, it bleaches it: a rupee is
// a luminance texture tinted by a konst, and under Remix it renders greyscale
// while raw D3D9 is correct. Hearts are the same shape of material.
//
// Reporting the tint here lets apply_tev claim TFACTOR for it up front, before
// any other constant can take the slot. That makes the value deterministic
// rather than "whichever operand happened to ask first", and costs nothing:
// materialize() already hands back TFACTOR for a matching constant, so the
// real stage that uses this konst reuses the same slot.
// Names the reduced op in a log line. Only the ops reduce_pass can actually
// produce are listed; anything else falls through to its numeric value, which
// is still enough to look up.
const char* d3d_texture_op_name(DWORD op) noexcept {
  switch (op) {
  case D3DTOP_DISABLE:                   return "DISABLE";
  case D3DTOP_SELECTARG1:                return "SELECTARG1";
  case D3DTOP_SELECTARG2:                return "SELECTARG2";
  case D3DTOP_MODULATE:                  return "MODULATE";
  case D3DTOP_MODULATE2X:                return "MODULATE2X";
  case D3DTOP_MODULATE4X:                return "MODULATE4X";
  case D3DTOP_ADD:                       return "ADD";
  case D3DTOP_ADDSIGNED:                 return "ADDSIGNED";
  case D3DTOP_ADDSIGNED2X:               return "ADDSIGNED2X";
  case D3DTOP_SUBTRACT:                  return "SUBTRACT";
  case D3DTOP_ADDSMOOTH:                 return "ADDSMOOTH";
  case D3DTOP_BLENDDIFFUSEALPHA:         return "BLENDDIFFUSEALPHA";
  case D3DTOP_BLENDTEXTUREALPHA:         return "BLENDTEXTUREALPHA";
  case D3DTOP_BLENDFACTORALPHA:          return "BLENDFACTORALPHA";
  case D3DTOP_BLENDTEXTUREALPHAPM:       return "BLENDTEXTUREALPHAPM";
  case D3DTOP_BLENDCURRENTALPHA:         return "BLENDCURRENTALPHA";
  case D3DTOP_MULTIPLYADD:               return "MULTIPLYADD";
  case D3DTOP_LERP:                      return "LERP";
  default:                               return "?";
  }
}

// Appends the albedo stage's texture identity to a log line, in the same shape
// the multi-texture diagnostic uses, so a reject can be matched against Remix's
// texture categorization list. Without this a reject names a reason but not a
// material, which is the difference between "widen the gate" and "widen the
// gate for THIS item".
const char* albedo_stage_desc(const gx::TevStage& stage) noexcept {
  static thread_local char desc[64];
  uint32_t w = 0;
  uint32_t h = 0;
  uint32_t fmt = 0;
  if (stage.texMapId != GX_TEXMAP_NULL && stage.texMapId < static_cast<int>(gx::MaxTextures)) {
    const auto& obj = g_gxState.loadedTextures[static_cast<size_t>(stage.texMapId)];
    w = obj.width();
    h = obj.height();
    fmt = obj.format();
  }
  std::snprintf(desc, sizeof(desc), " [map%d %ux%u fmt%u]", static_cast<int>(stage.texMapId), w, h, fmt);
  return desc;
}

bool albedo_tint(const DecodedDraw& draw, uint32_t& outTint) noexcept {
  const int idx = preferred_albedo_stage();
  if (idx < 0) {
    info_once(0xA1B0, "albedo tint: no preferred albedo stage; material keeps its raw texture");
    return false;
  }
  const auto& stage = g_gxState.tevStages[static_cast<size_t>(idx)];
  const auto stageIdx = static_cast<uint32_t>(idx);
  // Deliberately NOT the same seed the real emission path uses (see apply_tev,
  // where it is xxh3_hash(stage, 0)). This pass is speculative and runs first,
  // so sharing the seed made it consume reduce_pass's warn_once dedup keys
  // before the real path could, and every unsupported-TEV line a run produced
  // was then attributed to a decode that was thrown away. That cost a full
  // diagnostic cycle: the log looked populated while saying nothing about what
  // was actually drawn.
  const uint64_t cfgHash = xxh3_hash(stage, 0) ^ 0xA1B70000ull;
  const Operand a = color_operand(stage.colorPass.a, stage, stageIdx, true, draw);
  const Operand b = color_operand(stage.colorPass.b, stage, stageIdx, true, draw);
  const Operand c = color_operand(stage.colorPass.c, stage, stageIdx, true, draw);
  const Operand d = color_operand(stage.colorPass.d, stage, stageIdx, true, draw);
  const ReducedPass cp = reduce_pass(a, b, c, d, stage.colorOp, cfgHash, true);

  // Only a plain "texture x constant" lead counts. Anything more elaborate is
  // not a tint, and advertising a guess would be worse than leaving the albedo
  // untinted - the failure mode there is a wrong colour rather than a missing
  // one, which is much harder to spot.
  //
  // Every rejection below is logged. The gate is narrow on purpose, but it was
  // shipped silent, and a material that renders greyscale under Remix then
  // looks identical whether it was rejected here, rejected for a different
  // reason here, or accepted and dropped at one of the emission gates in
  // apply_tev. Naming the reason is what turns "the fix did not work" into a
  // decidable question.
  const PassOp& lead = cp.hasTemp ? cp.tempOp : cp.finals[0];
  // GX output scale is accepted. GX_CS_SCALE_2/SCALE_4 on an otherwise plain
  // texture x konst stage reduce to MODULATE2X/MODULATE4X, and a 2026-08-02 run
  // hit that path on 11 distinct material configs - every one of them a tint
  // that was being dropped for the scale alone.
  //
  // The scale is deliberately NOT carried into the tint. It is a brightness
  // multiplier on the rasterized result, whereas what Remix wants here is a
  // material albedo: doubling that pushes it above 1, which is unphysical for a
  // diffuse surface and would read as a blown-out item rather than a tinted
  // one. The konst is the colour; the scale is not part of it.
  const auto isTintConst = [](const Operand& o) {
    return o.isConst && (o.constValue & 0x00FFFFFFu) != 0x00FFFFFFu;
  };
  const auto readsCurrent = [](const Operand& o) {
    return !o.isConst && arg_base(o.ta) == D3DTA_CURRENT;
  };

  // "Sample in one stage, tint in the next" is a standard GX idiom, and it is
  // the same product as texture x konst - just spread over two stages. Looking
  // only at the albedo stage misses every material written that way, which a
  // 2026-08-03 run showed is the single largest reject bucket (26 configs whose
  // lead is a bare SELECTARG1(TEXTURE), i.e. a stage that only samples).
  //
  // Restricted to the IMMEDIATELY following stage on purpose. Scanning further
  // would have to reason about whatever the intervening stages do to the chain,
  // and a stage that adds or lerps in between breaks the equivalence - at which
  // point the konst is no longer simply the surface's colour and advertising it
  // would be the guess this gate exists to avoid.
  const auto tint_from_next_stage = [&](uint32_t& out) -> bool {
    const uint32_t next = stageIdx + 1;
    if (next >= g_gxState.numTevStages) {
      return false;
    }
    const auto& ns = g_gxState.tevStages[next];
    // A tinting stage samples no texture of its own; if it does, it is a second
    // layer rather than a tint and Remix can only show one of them anyway.
    if (ns.texMapId != GX_TEXMAP_NULL && ns.texCoordId != GX_TEXCOORD_NULL) {
      return false;
    }
    const uint64_t nextHash = xxh3_hash(ns, 0) ^ 0xA1B80000ull;
    const Operand na = color_operand(ns.colorPass.a, ns, next, true, draw);
    const Operand nb = color_operand(ns.colorPass.b, ns, next, true, draw);
    const Operand nc = color_operand(ns.colorPass.c, ns, next, true, draw);
    const Operand nd = color_operand(ns.colorPass.d, ns, next, true, draw);
    const ReducedPass np = reduce_pass(na, nb, nc, nd, ns.colorOp, nextHash, true);
    const PassOp& nl = np.hasTemp ? np.tempOp : np.finals[0];
    const bool modulate =
        nl.op == D3DTOP_MODULATE || nl.op == D3DTOP_MODULATE2X || nl.op == D3DTOP_MODULATE4X;
    if (!modulate || !nl.usesArg2 || nl.usesArg0 || nl.complementArg2) {
      return false;
    }
    if (readsCurrent(nl.arg1) && isTintConst(nl.arg2)) {
      out = nl.arg2.constValue;
      return true;
    }
    if (readsCurrent(nl.arg2) && isTintConst(nl.arg1)) {
      out = nl.arg1.constValue;
      return true;
    }
    return false;
  };

  const bool leadIsModulate =
      lead.op == D3DTOP_MODULATE || lead.op == D3DTOP_MODULATE2X || lead.op == D3DTOP_MODULATE4X;

  // A lead that only presents its texture has nothing to tint with, but the
  // next stage may.
  if (lead.op == D3DTOP_SELECTARG1 && !lead.usesArg2 && !lead.usesArg0 &&
      !lead.arg1.isConst && arg_base(lead.arg1.ta) == D3DTA_TEXTURE) {
    if (tint_from_next_stage(outTint)) {
      {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "albedo tint: claimed from the following stage (texture, then x const)%s",
                      albedo_stage_desc(stage));
        info_once(cfgHash ^ 0xA1BE, buf);
      }
      return true;
    }
  }

  if (!leadIsModulate) {
    // Most of these are correct rejections rather than losses - a plain
    // SELECTARG1(TEXTURE) material has no tint to carry. Naming the op is what
    // separates those from a shape that genuinely should be handled, which the
    // first version of this message could not do.
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "albedo tint: lead op is %s (not a modulate); tint not carried to Remix%s",
                  d3d_texture_op_name(lead.op), albedo_stage_desc(stage));
    warn_once(cfgHash ^ 0xA1B2, buf);
    return false;
  }
  if (!lead.usesArg2) {
    warn_once(cfgHash ^ 0xA1B3, "albedo tint: lead MODULATE has one argument; tint not carried");
    return false;
  }
  if (lead.usesArg0) {
    warn_once(cfgHash ^ 0xA1B4,
              "albedo tint: lead has a third (d) term, i.e. multiply-add; tint not carried");
    return false;
  }
  if (lead.complementArg2) {
    warn_once(cfgHash ^ 0xA1B5, "albedo tint: lead is the a*(1-c) form; tint not carried");
    return false;
  }
  const auto isTexture = [](const Operand& o) {
    return !o.isConst && arg_base(o.ta) == D3DTA_TEXTURE;
  };
  // White is the identity here, so it is not worth a stage.
  const auto isTint = [](const Operand& o) {
    return o.isConst && (o.constValue & 0x00FFFFFFu) != 0x00FFFFFFu;
  };
  if (isTexture(lead.arg1) && isTint(lead.arg2)) {
    outTint = lead.arg2.constValue;
    {
      char buf[160];
      std::snprintf(buf, sizeof(buf), "albedo tint: claimed (texture x const)%s", albedo_stage_desc(stage));
      info_once(cfgHash ^ 0xA1BF, buf);
    }
    return true;
  }
  if (isTexture(lead.arg2) && isTint(lead.arg1)) {
    outTint = lead.arg1.constValue;
    {
      char buf[160];
      std::snprintf(buf, sizeof(buf), "albedo tint: claimed (const x texture)%s", albedo_stage_desc(stage));
      info_once(cfgHash ^ 0xA1BF, buf);
    }
    return true;
  }
  // Distinguish "the constant is white" from "this was never texture x const".
  // The first means the material genuinely wants no tint; the second means the
  // colour is arriving by a route this gate does not model - a vertex colour,
  // or a lerp between two registers keyed by texture intensity - and is the
  // case that would need the gate widening rather than a bug fixing.
  const bool pairing = (isTexture(lead.arg1) && lead.arg2.isConst) ||
                       (isTexture(lead.arg2) && lead.arg1.isConst);
  if (pairing) {
    {
      char buf[160];
      std::snprintf(buf, sizeof(buf), "albedo tint: constant is white; no tint stage needed%s",
                    albedo_stage_desc(stage));
      info_once(cfgHash ^ 0xA1B6, buf);
    }
  } else {
    // Name what the two operands actually were. "texture x vertex colour" is
    // the overwhelmingly common world-geometry case and a correct reject; a
    // const-x-const or register pairing is not, and is the shape that would
    // justify widening this further.
    char buf[200];
    std::snprintf(buf, sizeof(buf),
                  "albedo tint: lead is %s x %s, not texture x const; tint not carried to Remix%s",
                  isTexture(lead.arg1) ? "texture" : (lead.arg1.isConst ? "const" : "non-const"),
                  isTexture(lead.arg2) ? "texture" : (lead.arg2.isConst ? "const" : "non-const"),
                  albedo_stage_desc(stage));
    warn_once(cfgHash ^ 0xA1B7, buf);
  }
  return false;
}

} // namespace

uint32_t apply_tev(const DecodedDraw& draw) noexcept {
  const uint32_t numStages = std::max<uint32_t>(g_gxState.numTevStages, 1);
  ConstAlloc consts;

  // Claim TFACTOR for the albedo tint before anything else can (see
  // albedo_tint). Done here rather than at the hint stage below because the
  // constant slots are allocated as the real stages are emitted, and the hint
  // is written before any of them have run.
  uint32_t hintTint = 0;
  const bool hasHintTint = albedo_tint(draw, hintTint);
  if (hasHintTint) {
    consts.tfactor = hintTint;
    consts.tfactorUsed = true;
  }

  // Diagnostic for multi-texture materials: Remix can only take one of their
  // textures as the surface albedo, so when a character's eye composites an
  // eyeball, a highlight and an eye shadow in one draw, only one survives.
  // Log each distinct layout once so a run's log identifies exactly which
  // texmap/texcoord each stage samples and which one we hand Remix.
  {
    uint64_t layoutKey = 0xD1A6;
    uint32_t textured = 0;
    for (uint32_t i = 0; i < numStages; ++i) {
      const auto& s = g_gxState.tevStages[i];
      if (s.texMapId == GX_TEXMAP_NULL || s.texCoordId == GX_TEXCOORD_NULL) {
        continue;
      }
      ++textured;
      layoutKey = layoutKey * 1315423911u + (static_cast<uint64_t>(s.texMapId) << 8) +
                  static_cast<uint64_t>(s.texCoordId) + i;
    }
    if (textured > 1) {
      char buf[256];
      int off = std::snprintf(buf, sizeof(buf), "multi-texture material (%u textured stages):", textured);
      for (uint32_t i = 0; i < numStages && off > 0 && off < static_cast<int>(sizeof(buf)); ++i) {
        const auto& s = g_gxState.tevStages[i];
        if (s.texMapId == GX_TEXMAP_NULL || s.texCoordId == GX_TEXCOORD_NULL) {
          continue;
        }
        // Dimensions/format identify which texture each stage samples, so a
        // log can be matched against Remix's texture list - the stage order
        // decides which one becomes the albedo, and picking the right one for
        // e.g. an eye needs to know which map is the eyeball.
        uint32_t w = 0;
        uint32_t h = 0;
        uint32_t fmt = 0;
        if (s.texMapId < static_cast<int>(gx::MaxTextures)) {
          const auto& obj = g_gxState.loadedTextures[static_cast<size_t>(s.texMapId)];
          w = obj.width();
          h = obj.height();
          fmt = obj.format();
        }
        off += std::snprintf(buf + off, sizeof(buf) - static_cast<size_t>(off), " [gx%u map%d coord%d %ux%u fmt%u]",
                             i, static_cast<int>(s.texMapId), static_cast<int>(s.texCoordId), w, h, fmt);
      }
      info_once(layoutKey, buf);
    }
  }

  uint32_t d3dStage = 0;
  // The Remix albedo/opacity hint is considered once per draw, at the first
  // GX stage that samples a texture.
  bool remixHintConsidered = false;

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
    const Operand aa = alpha_operand(stage.alphaPass.a, stage, i, hasTexture, draw);
    const Operand ab = alpha_operand(stage.alphaPass.b, stage, i, hasTexture, draw);
    const Operand ac = alpha_operand(stage.alphaPass.c, stage, i, hasTexture, draw);
    const Operand ad = alpha_operand(stage.alphaPass.d, stage, i, hasTexture, draw);

    ReducedPass cp = reduce_pass(ca, cb, cc, cd, stage.colorOp, cfgHash, true);
    ReducedPass ap = reduce_pass(aa, ab, ac, ad, stage.alphaOp, cfgHash ^ 0xA1FA, true);

    // Stage plan: [optional shared TEMP stage] + start-aligned finals.
    bool anyTemp = cp.hasTemp || ap.hasTemp;
    int finalsN = std::max(cp.finalCount, ap.finalCount);
    uint32_t need = (anyTemp ? 1u : 0u) + static_cast<uint32_t>(finalsN);
    if (d3dStage + need > MaxStages) {
      // Not enough stage budget: fall back to single-op approximations.
      cp = reduce_pass(ca, cb, cc, cd, stage.colorOp, cfgHash ^ 0x51, false);
      ap = reduce_pass(aa, ab, ac, ad, stage.alphaOp, cfgHash ^ 0x52, false);
      anyTemp = false;
      finalsN = 1;
      need = 1;
      if (d3dStage + need > MaxStages) {
        warn_once(0x6201 | i << 8, "tev: out of D3D stages, GX stage dropped");
        continue;
      }
    }

    // Skip pure pass-through stages (both passes keep CURRENT) to save the
    // 8-stage budget for meaningful work.
    const auto is_current_pass = [](const ReducedPass& r) {
      return !r.hasTemp && r.finalCount == 1 && r.finals[0].op == D3DTOP_SELECTARG1 && !r.finals[0].arg1.isConst &&
             (r.finals[0].arg1.ta & ~D3DTA_COMPLEMENT & ~D3DTA_ALPHAREPLICATE) == D3DTA_CURRENT;
    };
    if (d3dStage > 0 && is_current_pass(cp) && is_current_pass(ap)) {
      continue;
    }

    static const PassOp kPassthrough{};

    // Hand Remix an albedo/opacity stage it can read as the finished material
    // (see the comment on op_is_plain_texture). It goes at the first GX stage
    // that samples a texture, whatever D3D stage that lands on: Remix scans
    // stages in order and bins by texcoord, so the hint reaches its texcoord's
    // slot before the real stage does. The texture it advertises is the
    // material's *colour* texture (preferred_albedo_stage), which is not
    // necessarily this stage's - see is_color_texture_format.
    //
    // Writing TEMP rather than CURRENT keeps the running chain intact, so the
    // hint is raster-neutral wherever it is placed - TEMP is only ever live
    // within a single GX stage's own decomposition, never across stages. Only
    // when the device has no TEMP register does it fall back to writing
    // CURRENT, which is safe just at the head of the chain and only when
    // nothing in this GX stage reads CURRENT back.
    if (!remixHintConsidered && hasTexture && d3dStage + need < MaxStages) {
      remixHintConsidered = true;
      const int albedoIdx = preferred_albedo_stage();
      const auto& albedoStage =
          albedoIdx >= 0 ? g_gxState.tevStages[static_cast<size_t>(albedoIdx)] : stage;
      const PassOp& colorLead = (anyTemp && cp.hasTemp) ? cp.tempOp : cp.finals[0];
      const PassOp& alphaLead = (anyTemp && ap.hasTemp) ? ap.tempOp : ap.finals[0];
      // A stage that already presents its texture plainly still needs the hint
      // when the colour texture lives on a different stage.
      const bool alreadyPlain = op_is_plain_texture(colorLead) && op_is_plain_texture(alphaLead) &&
                                albedoStage.texMapId == stage.texMapId;
      const bool currentSafe = g_dx9.tssTemp || (d3dStage == 0 && !pass_reads(cp, D3DTA_CURRENT) &&
                                                 !pass_reads(ap, D3DTA_CURRENT));
      // A claimed tint that never reaches a stage is the failure mode that made
      // 389e4d5 look like a no-op in game: TFACTOR is spent up top, so the
      // value is right and the wire is right, but nothing ever emits the stage
      // Remix looks for. Each gate below says so distinctly.
      if (hasHintTint) {
        if (alreadyPlain) {
          warn_once(0xA1C1, "albedo tint: claimed, but the hint stage was skipped because the "
                            "lead already presents its texture plainly; tint never emitted");
        } else if (!currentSafe) {
          warn_once(0xA1C2, "albedo tint: claimed, but no TEMP register and CURRENT is not safe "
                            "here; tint never emitted");
        }
      }
      if (!alreadyPlain && currentSafe) {
        if (IDirect3DBaseTexture9* tex = resolve_texmap(albedoStage.texMapId); tex != nullptr) {
          set_texture(d3dStage, tex);
          apply_sampler(d3dStage, albedoStage.texMapId);
          set_tss(d3dStage, D3DTSS_TEXCOORDINDEX, apply_texgen(d3dStage, albedoStage.texCoordId, draw));
          set_tss(d3dStage, D3DTSS_COLOROP, D3DTOP_MODULATE);
          set_tss(d3dStage, D3DTSS_COLORARG1, D3DTA_TEXTURE);
          set_tss(d3dStage, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
          // Opacity must be the texture's own alpha: it is what Remix
          // alpha-tests against, and it is what gives foliage cards and grass
          // blades their cutout shape.
          set_tss(d3dStage, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
          set_tss(d3dStage, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
          if (g_dx9.tssTemp) {
            set_tss(d3dStage, D3DTSS_RESULTARG, D3DTA_TEMP);
          }
          ++d3dStage;

          // Carry the albedo tint, which the hint's TEXTURE x DIFFUSE cannot
          // express - a modulate takes two args and both are already spoken
          // for. Remix decodes exactly one extra MODULATE against TFACTOR
          // (isTextureFactorBlendingEnabled; rtx.enableMultiStageTextureFactor
          // Blending defaults on), and it matches that against whatever
          // register the *previous* stage wrote. So this has to sit
          // immediately after the hint and read the same register the hint
          // wrote, or it is not recognised.
          //
          // Raster-neutral on the same grounds as the hint: with a TEMP
          // register this only ever touches scratch, and without one the hint
          // already established that nothing in this GX stage reads CURRENT
          // back before the real chain overwrites it.
          if (hasHintTint && d3dStage + need < MaxStages) {
            const DWORD hintReg = g_dx9.tssTemp ? D3DTA_TEMP : D3DTA_CURRENT;
            set_texture(d3dStage, nullptr);
            set_tss(d3dStage, D3DTSS_COLOROP, D3DTOP_MODULATE);
            set_tss(d3dStage, D3DTSS_COLORARG1, hintReg);
            set_tss(d3dStage, D3DTSS_COLORARG2, D3DTA_TFACTOR);
            // Opacity is still the texture's own alpha the hint selected;
            // tinting it would eat alpha-tested cutout shapes.
            set_tss(d3dStage, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            set_tss(d3dStage, D3DTSS_ALPHAARG1, hintReg);
            if (g_dx9.tssTemp) {
              set_tss(d3dStage, D3DTSS_RESULTARG, D3DTA_TEMP);
            }
            ++d3dStage;
            info_once(0xA1CF, "albedo tint: stage emitted; Remix should tint this material");
          } else if (hasHintTint) {
            warn_once(0xA1C3, "albedo tint: claimed, but no stage budget left for it; "
                              "tint never emitted");
          }
        } else if (hasHintTint) {
          warn_once(0xA1C4, "albedo tint: claimed, but the albedo texmap did not resolve; "
                            "tint never emitted");
        }
      }
    }

    for (uint32_t k = 0; k < need; ++k, ++d3dStage) {
      const bool isTempStage = anyTemp && k == 0;
      const int fi = static_cast<int>(k) - (anyTemp ? 1 : 0);
      const PassOp& colorOp = isTempStage ? (cp.hasTemp ? cp.tempOp : kPassthrough)
                              : fi < cp.finalCount ? cp.finals[fi]
                                                   : kPassthrough;
      const PassOp& alphaOp = isTempStage ? (ap.hasTemp ? ap.tempOp : kPassthrough)
                              : fi < ap.finalCount ? ap.finals[fi]
                                                   : kPassthrough;

      // Bind texture + sampler + texgen only on the emitted stages that
      // actually sample it. Binding it on the others too would cost nothing in
      // raster, but Remix treats every stage with a texture bound as a
      // candidate and keeps only two per draw, so duplicates can crowd out a
      // second real texture.
      if (hasTexture && (op_reads(colorOp, D3DTA_TEXTURE) || op_reads(alphaOp, D3DTA_TEXTURE))) {
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
        return materialize(op, consts, d3dStage, stageConstUsed, stageConstValue, cfgHash ^ key ^ (k * 0x100));
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
      if (g_dx9.tssTemp) {
        // Reset RESULTARG on every stage; a previous draw may have left TEMP.
        set_tss(d3dStage, D3DTSS_RESULTARG, isTempStage ? D3DTA_TEMP : D3DTA_CURRENT);
      }
    }
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
    if (g_dx9.tssTemp) {
      set_tss(0, D3DTSS_RESULTARG, D3DTA_CURRENT);
    }
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
      if (g_dx9.tssTemp) {
        set_tss(d3dStage, D3DTSS_RESULTARG, D3DTA_CURRENT);
      }
      ++d3dStage;
    }
  }

  if (consts.tfactorUsed) {
    set_rs(D3DRS_TEXTUREFACTOR, consts.tfactor);
  }

  // Terminate the stage chain. Every unused stage is disabled *and* unbound:
  // D3D9 rasterization stops at the first disabled stage, but Remix's scan
  // skips stages with no texture rather than stopping, so a texture left over
  // from an earlier draw on a high stage would still be collected as a
  // material candidate - and win the albedo slot outright if its stale
  // texcoord index sorted below this draw's own.
  for (uint32_t s = d3dStage; s < MaxStages; ++s) {
    set_texture(s, nullptr);
    set_tss(s, D3DTSS_COLOROP, D3DTOP_DISABLE);
    set_tss(s, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
  }
  return d3dStage;
}

} // namespace aurora::dx9

#endif // AURORA_ENABLE_D3D9
