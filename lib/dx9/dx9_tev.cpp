#include "dx9_tev.hpp"

#ifdef AURORA_ENABLE_D3D9

#include "dx9_texture.hpp"
// GX enum names for the material report, so the log never carries a second
// copy of them that can drift (docs/dx9/material-report.md).
#include "../gx/gx_fmt.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

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

// D3D9 argument names for the material report. GX enum names come from
// lib/gx/gx_fmt.hpp's format_as overloads - the single source of truth for
// those - but D3D9 arguments have no equivalent, and the two modifier bits are
// worth spelling out because both make Remix drop the argument to identity.
// See docs/dx9/material-report.md.
const char* d3dta_name(DWORD ta) noexcept {
  const DWORD base = ta & ~(D3DTA_COMPLEMENT | D3DTA_ALPHAREPLICATE);
  const bool comp = (ta & D3DTA_COMPLEMENT) != 0;
  const bool rep = (ta & D3DTA_ALPHAREPLICATE) != 0;
  switch (base) {
  case D3DTA_DIFFUSE: return comp ? "~DIFFUSE" : (rep ? "DIFFUSE.a" : "DIFFUSE");
  case D3DTA_CURRENT: return comp ? "~CURRENT" : (rep ? "CURRENT.a" : "CURRENT");
  case D3DTA_TEXTURE: return comp ? "~TEXTURE" : (rep ? "TEXTURE.a" : "TEXTURE");
  case D3DTA_TFACTOR: return comp ? "~TFACTOR" : (rep ? "TFACTOR.a" : "TFACTOR");
  case D3DTA_SPECULAR: return "SPECULAR";
  case D3DTA_TEMP: return comp ? "~TEMP" : (rep ? "TEMP.a" : "TEMP");
  case D3DTA_CONSTANT: return comp ? "~CONSTANT" : (rep ? "CONSTANT.a" : "CONSTANT");
  default: return "?";
  }
}

// The framebuffer blend, named for the material report.
//
// This is the one GX fact that unambiguously means "emits light": ONE/ONE adds
// the draw to what is already there, which is what a torch flame, a light shaft
// or a glow halo is. Remix reads the translated D3D9 blend and treats it as
// emissive on its own, before any of our scoring runs - so a surface that
// scores zero on every TEV signal may still be glowing correctly by this route,
// and until this field existed neither log could say which.
// docs/dx9/remix-material-interface.md §9.
const char* blend_name() noexcept {
  switch (g_gxState.blendMode) {
  case GX_BM_NONE:
    return "off";
  case GX_BM_SUBTRACT:
    return "subtract";
  case GX_BM_LOGIC:
    return "logic";
  case GX_BM_BLEND:
    break;
  default:
    return "?";
  }
  const GXBlendFactor s = g_gxState.blendFacSrc;
  const GXBlendFactor d = g_gxState.blendFacDst;
  if (s == GX_BL_ONE && d == GX_BL_ONE) {
    return "additive";       // Remix: BlendType::kEmissive
  }
  if (s == GX_BL_SRCALPHA && d == GX_BL_ONE) {
    return "additiveAlpha";  // Remix: BlendType::kAlphaEmissive
  }
  if (s == GX_BL_SRCALPHA && d == GX_BL_INVSRCALPHA) {
    return "alpha";
  }
  if (s == GX_BL_ZERO && d == GX_BL_SRCCLR) {
    return "multiply";
  }
  if (s == GX_BL_ONE && d == GX_BL_ZERO) {
    return "opaque";
  }
  return "other";
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
// Remix rebuilds a draw's material from exactly ONE texture stage, anything it
// cannot decode resolves to identity - WHITE - rather than to an error, and
// that one stage's alpha becomes the whole opacity (so alpha-tested cutouts
// live or die by it).
//
// Hence the hint stage below: where Remix would read the real stage wrongly we
// prepend one it reads right, and where Remix would read it correctly we must
// NOT, because the hint reduces the material to a single op and wins the stage
// Remix reads.
//
// This is the most misunderstood system in the project and has been described
// wrongly in comments twice. Everything else - what survives the capture path,
// which endpoint gets advertised, the D3DMATERIAL9 side channels - lives in
// docs/dx9/remix-material-interface.md. Keep it there.
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

// Whether Remix decodes this operand at all; one it cannot decode becomes
// RtTextureArgSource::None, which its shader resolves to *identity* - white
// for colour. A constant only survives in TFACTOR: the per-stage
// D3DTSS_CONSTANT slot is never read. Full list:
// docs/dx9/remix-material-interface.md §2.
//
// An unclaimed TFACTOR counts as decodable, because materialize() hands the
// draw's first constant TFACTOR unconditionally. Missing that is how the July
// 2026 fix silently became a no-op whenever the predicate that used to claim
// the slot declined.
bool remix_decodes_arg(const Operand& o, const ConstAlloc& consts) noexcept {
  if (o.isConst) {
    return !consts.tfactorUsed || consts.tfactor == o.constValue;
  }
  if (o.ta != arg_base(o.ta)) {
    return false; // COMPLEMENT / ALPHAREPLICATE are not decoded
  }
  return o.ta == D3DTA_TEXTURE || o.ta == D3DTA_DIFFUSE || o.ta == D3DTA_TFACTOR;
}

// True when Remix, reading this stage alone, reconstructs the albedo we meant -
// including any tint. This is the test that decides whether the hint stage
// below is needed; emitting the hint when this is already true is what bleached
// rupees, hearts and lava, because the hint replaces a faithful stage with a
// one-op approximation and wins the stage Remix reads.
bool remix_decodes_albedo(const PassOp& op, const ConstAlloc& consts) noexcept {
  if (op.op != D3DTOP_MODULATE && op.op != D3DTOP_MODULATE2X && op.op != D3DTOP_MODULATE4X &&
      op.op != D3DTOP_SELECTARG1 && op.op != D3DTOP_SELECTARG2) {
    return false;
  }
  if (op.usesArg0 || op.complementArg2) {
    return false;
  }
  if (!remix_decodes_arg(op.arg1, consts)) {
    return false;
  }
  if (op.usesArg2 && !remix_decodes_arg(op.arg2, consts)) {
    return false;
  }
  const auto isTex = [](const Operand& o) { return !o.isConst && o.ta == D3DTA_TEXTURE; };
  return isTex(op.arg1) || (op.usesArg2 && isTex(op.arg2));
}

// Intensity-only GX formats carry no colour: they are masks - eye highlights,
// eye shadows, gradient ramps. When a material mixes them with a colour
// texture, the colour one is the albedo Remix should be shown. Character eyes
// are the case that forced this: their first textured stage samples a 32x32 I8
// highlight mask, with the actual eyeball (64x64 CMPR) two stages later, so
// taking the first textured stage handed Remix a grey blob for the eye.
//
// CAUTION: a luminance texture tinted by a konst is a legitimate albedo (it is
// how this game colours rupees and hearts), so "not a colour format" does not
// mean "not the albedo". matrep.sum's colorFmt field exists to catch exactly
// that misselection (docs/dx9/material-report.md).
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

// Does this stage's colour pass actually read its texture? A stage can bind a
// texture and do nothing with it - materials in this game routinely open with a
// setup stage whose colour pass is all ZERO, with the real work one stage later.
// Picking such a stage as the albedo hands Remix an empty program and loses the
// material's colour, which is one of the two ways the greyscale defect happened.
bool stage_colour_reads_texture(const gx::TevStage& s) noexcept {
  const auto reads = [](GXTevColorArg a) { return a == GX_CC_TEXC || a == GX_CC_TEXA; };
  return reads(s.colorPass.a) || reads(s.colorPass.b) || reads(s.colorPass.c) || reads(s.colorPass.d);
}

// GX stage whose texture should become the albedo. Preference order:
//   1. a stage that samples a colour texture AND uses it in its colour pass
//   2. any stage that uses its texture in its colour pass
//   3. the first textured stage at all (last resort)
// See docs/dx9/remix-material-interface.md §7 - a luminance texture that is used
// is a better albedo than a colour texture that is merely bound.
int preferred_albedo_stage() noexcept {
  int firstTextured = -1;
  int firstUsed = -1;
  for (uint32_t i = 0; i < g_gxState.numTevStages; ++i) {
    const auto& s = g_gxState.tevStages[i];
    if (s.texMapId == GX_TEXMAP_NULL || s.texMapId >= static_cast<int>(gx::MaxTextures) ||
        s.texCoordId == GX_TEXCOORD_NULL) {
      continue;
    }
    if (firstTextured < 0) {
      firstTextured = static_cast<int>(i);
    }
    if (!stage_colour_reads_texture(s)) {
      continue;
    }
    if (firstUsed < 0) {
      firstUsed = static_cast<int>(i);
    }
    if (is_color_texture_format(g_gxState.loadedTextures[static_cast<size_t>(s.texMapId)].format())) {
      return static_cast<int>(i);
    }
  }
  return firstUsed >= 0 ? firstUsed : firstTextured;
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
    // A *suspect*, not a confirmed cause, for the torch-flame white circle -
    // which does reach Remix (dusklight-ao/docs/remix-open-issues.md).
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

// What the material's albedo actually is, evaluated rather than pattern-matched.
//
// This game's dominant shape is `lerp(colourA, colourB, textureIntensity)` - a
// greyscale texture selecting between two authored colours, which is how one
// rupee texture yields seven rupee colours. Pattern-matching only the
// "texture x constant" special case is why 104 of 111 materials in the
// 2026-08-03 log reported no tint at all. So evaluate the GX colour pass twice
// instead, texture pinned to black then to white, and report both endpoints.
//
// Both endpoints reach the fork, which reconstructs the lerp exactly (§10); the
// single-op hint below still has to pick one of them, for the materials where
// the ramp is declined. Both are logged, so which was picked is answerable from
// a log rather than by argument.
//
// Full account: docs/dx9/remix-material-interface.md §7, §7b, §10.
struct AlbedoIntent {
  bool valid = false;         // the pass could be evaluated at all
  bool usesTexture = false;   // the colour pass reads its texture
  bool usesVertexColor = false;
  uint32_t out0 = 0;          // colour where the texture reads black
  uint32_t out1 = 0;          // colour where the texture reads white
  // Whether the vertex colour stream is authored material colour - GX colour
  // channel lighting ENABLED, so GX would light it and it carries no light of
  // its own - rather than the finished channel output, which is where this game
  // bakes room lighting and shadow. Forward the first, withhold the second.
  // docs/dx9/remix-material-interface.md §7c.
  bool vertexColorIsMaterial = false;
  // Opacity, evaluated the same way. The hint used to advertise the texture's
  // own alpha unconditionally, which is right for a cutout but drops any
  // constant scale on it - so a HUD effect fading in through a konst alpha
  // reached Remix fully opaque and drew its whole quad. TFACTOR's alpha channel
  // is otherwise unused, so it carries the scale without competing with the
  // colour tint.
  bool alphaValid = false;
  bool alphaUsesTexture = false;
  uint32_t alphaScale = 0xFF;  // opacity where the texture's alpha reads full
  bool hasTint = false;       // worth advertising a TFACTOR
  uint32_t tint = 0xFFFFFFFFu;
  // Which op best approximates the material in the one stage Remix reads - used
  // only where the exact ramp (§10) is declined. Decided by the *floor*, not by
  // where the ramp ends: see the op choice at the tail of evaluate_albedo.
  // Remix decodes both (docs/dx9/remix-material-interface.md §2).
  DWORD hintOp = D3DTOP_MODULATE;
  const char* shape = "unknown";
};

// Chroma (max-min across RGB): how much colour a value carries, as opposed to
// how bright it is. Used to pick which lerp endpoint is the material's colour.
inline uint32_t chroma_of(uint32_t argb) noexcept {
  const uint32_t r = (argb >> 16) & 0xFFu, g = (argb >> 8) & 0xFFu, b = argb & 0xFFu;
  return std::max({r, g, b}) - std::min({r, g, b});
}
inline uint32_t luma_of(uint32_t argb) noexcept {
  const uint32_t r = (argb >> 16) & 0xFFu, g = (argb >> 8) & 0xFFu, b = argb & 0xFFu;
  return (r * 77 + g * 151 + b * 28) >> 8;
}

// Resolve one operand to a per-channel value with the texture pinned to
// `texValue`. Returns false for anything whose value we cannot know here
// (CURRENT/TEMP carry a previous stage's result).
bool eval_operand(const Operand& o, float texValue, const float (&diffuse)[3],
                  bool diffuseIsPerVertex, float (&out)[3], bool& sawTexture,
                  bool& sawVertexColor) noexcept {
  if (o.isConst) {
    out[0] = static_cast<float>((o.constValue >> 16) & 0xFFu) / 255.f;
    out[1] = static_cast<float>((o.constValue >> 8) & 0xFFu) / 255.f;
    out[2] = static_cast<float>(o.constValue & 0xFFu) / 255.f;
    return true;
  }
  const DWORD base = arg_base(o.ta);
  if (base == D3DTA_TEXTURE) {
    sawTexture = true;
    out[0] = out[1] = out[2] = texValue;
  } else if (base == D3DTA_DIFFUSE) {
    // Only genuinely per-vertex when the stream carries CLR0. Otherwise aurora
    // substitutes a constant - the GX channel's material colour register - and
    // that constant is authored material colour, so evaluating it as white
    // silently threw the colour away. Only the per-vertex case makes the
    // material unevaluable.
    sawVertexColor = sawVertexColor || diffuseIsPerVertex;
    out[0] = diffuse[0];
    out[1] = diffuse[1];
    out[2] = diffuse[2];
  } else {
    return false;
  }
  if ((o.ta & D3DTA_COMPLEMENT) != 0) {
    for (float& v : out) {
      v = 1.f - v;
    }
  }
  return true;
}

AlbedoIntent evaluate_albedo(const DecodedDraw& draw) noexcept {
  AlbedoIntent r;
  const int idx = preferred_albedo_stage();
  if (idx < 0) {
    return r;
  }
  const auto& stage = g_gxState.tevStages[static_cast<size_t>(idx)];
  const auto stageIdx = static_cast<uint32_t>(idx);
  const Operand ops[4] = {
      color_operand(stage.colorPass.a, stage, stageIdx, true, draw),
      color_operand(stage.colorPass.b, stage, stageIdx, true, draw),
      color_operand(stage.colorPass.c, stage, stageIdx, true, draw),
      color_operand(stage.colorPass.d, stage, stageIdx, true, draw),
  };

  const float bias = stage.colorOp.bias == GX_TB_ADDHALF    ? 0.5f
                     : stage.colorOp.bias == GX_TB_SUBHALF  ? -0.5f
                                                            : 0.f;
  const float scale = stage.colorOp.scale == GX_CS_SCALE_2   ? 2.f
                      : stage.colorOp.scale == GX_CS_SCALE_4 ? 4.f
                      : stage.colorOp.scale == GX_CS_DIVIDE_2 ? 0.5f
                                                              : 1.f;
  const bool subtract = stage.colorOp.op == GX_TEV_SUB;

  // What D3DTA_DIFFUSE will actually be. With no CLR0 stream it is a constant
  // aurora writes into every vertex, so it is knowable here; with a stream it
  // is per-vertex and white is the only neutral stand-in.
  const uint32_t dd = draw.defaultDiffuse;
  const float diffuse[3] = {
      draw.hasVertexColor ? 1.f : static_cast<float>((dd >> 16) & 0xFFu) / 255.f,
      draw.hasVertexColor ? 1.f : static_cast<float>((dd >> 8) & 0xFFu) / 255.f,
      draw.hasVertexColor ? 1.f : static_cast<float>(dd & 0xFFu) / 255.f,
  };

  uint32_t endpoints[2] = {0, 0};
  for (int end = 0; end < 2; ++end) {
    const float texValue = end == 0 ? 0.f : 1.f;
    float v[4][3];
    for (int i = 0; i < 4; ++i) {
      if (!eval_operand(ops[i], texValue, diffuse, draw.hasVertexColor, v[i], r.usesTexture,
                        r.usesVertexColor)) {
        return r; // depends on a previous stage; not evaluable here
      }
    }
    uint32_t packed = 0xFF000000u;
    for (int ch = 0; ch < 3; ++ch) {
      // GX: out = (d +/- (a*(1-c) + b*c) + bias) * scale
      const float term = v[0][ch] * (1.f - v[2][ch]) + v[1][ch] * v[2][ch];
      float o = (v[3][ch] + (subtract ? -term : term) + bias) * scale;
      o = o < 0.f ? 0.f : (o > 1.f ? 1.f : o);
      packed |= static_cast<uint32_t>(o * 255.f + 0.5f) << (16 - ch * 8);
    }
    endpoints[end] = packed;
  }

  r.valid = true;
  r.out0 = endpoints[0];
  r.out1 = endpoints[1];
  r.vertexColorIsMaterial =
      draw.hasVertexColor && g_gxState.colorChannelConfig[GX_COLOR0].lightingEnabled;

  // Opacity, by the same method: evaluate the alpha pass with the texture's
  // alpha at full. GX alpha operands are scalars, so one channel is enough.
  {
    const Operand aops[4] = {
        alpha_operand(stage.alphaPass.a, stage, stageIdx, true, draw),
        alpha_operand(stage.alphaPass.b, stage, stageIdx, true, draw),
        alpha_operand(stage.alphaPass.c, stage, stageIdx, true, draw),
        alpha_operand(stage.alphaPass.d, stage, stageIdx, true, draw),
    };
    float av[4][3];
    bool ok = true;
    bool sawTex = false;
    bool sawVtx = false;
    for (int i = 0; i < 4 && ok; ++i) {
      ok = eval_operand(aops[i], 1.f, diffuse, draw.hasVertexColor, av[i], sawTex, sawVtx);
    }
    if (ok) {
      const float aBias = stage.alphaOp.bias == GX_TB_ADDHALF   ? 0.5f
                          : stage.alphaOp.bias == GX_TB_SUBHALF ? -0.5f
                                                                : 0.f;
      const float aScale = stage.alphaOp.scale == GX_CS_SCALE_2    ? 2.f
                           : stage.alphaOp.scale == GX_CS_SCALE_4  ? 4.f
                           : stage.alphaOp.scale == GX_CS_DIVIDE_2 ? 0.5f
                                                                   : 1.f;
      const float term = av[0][0] * (1.f - av[2][0]) + av[1][0] * av[2][0];
      float o = (av[3][0] + (stage.alphaOp.op == GX_TEV_SUB ? -term : term) + aBias) * aScale;
      o = o < 0.f ? 0.f : (o > 1.f ? 1.f : o);
      r.alphaValid = true;
      r.alphaUsesTexture = sawTex;
      r.alphaScale = static_cast<uint32_t>(o * 255.f + 0.5f);
    }
  }

  if (!r.usesTexture) {
    r.shape = "flat";
    return r; // no texture in the colour pass; nothing for a tint to modulate
  }

  const uint32_t rgb0 = r.out0 & 0x00FFFFFFu;
  const uint32_t rgb1 = r.out1 & 0x00FFFFFFu;
  if (rgb0 == 0 && rgb1 == 0x00FFFFFFu) {
    r.shape = "tex"; // plain texture, already what Remix would build
    return r;
  }
  if (rgb0 == 0) {
    r.shape = "tex*c"; // the classic tinted mask
  } else {
    r.shape = "ramp"; // a lerp between two real colours
  }

  // Pick the op and constant that best reproduce the two endpoints within the
  // single stage Remix reads. Decided by the *floor*, because a multiply always
  // produces black where the texture is black:
  //
  //   floor black     -> MODULATE is exact at both ends.
  //   floor coloured  -> ADD keeps the floor exactly and overshoots at the
  //                      bright end (a blown specular, not a hole). Measured:
  //                      these are `floor + scale x texture` in GX - the heart
  //                      is `B80000 + 0.25 x texture` - so a multiply is
  //                      structurally wrong, not mistuned.
  //
  // An earlier revision gated ADD on the ramp ending near white: 5 materials
  // out of 111, leaving rupees and hearts with black highlights.
  // Measurements and the accepted cost: docs/dx9/remix-material-interface.md §7b.
  uint32_t pick;
  if (rgb0 == 0) {
    pick = rgb1;
    r.hintOp = D3DTOP_MODULATE;
  } else if (luma_of(rgb0) >= 0xE8) {
    // Descending ramp - bright where the texture is black. ADD would drive the
    // whole surface to white, so keep the multiply and take the more coloured
    // endpoint. Rare, and the single-op hint reproduces it poorly; the exact
    // ramp (§10) still does when TFACTOR ends up holding an endpoint.
    const uint32_t c0 = chroma_of(rgb0), c1 = chroma_of(rgb1);
    pick = c1 > c0 ? rgb1 : rgb0;
    r.hintOp = D3DTOP_MODULATE;
  } else {
    pick = rgb0;
    r.hintOp = D3DTOP_ADD;
  }
  // White would be the identity for a multiply, and black would erase the
  // surface for either op; neither is worth a stage.
  if (!(r.hintOp == D3DTOP_MODULATE && pick == 0x00FFFFFFu) && pick != 0) {
    r.hasTint = true;
    r.tint = 0xFF000000u | pick;
  }
  return r;
}

// ---------------------------------------------------------------------------
// Self-illumination evidence
// ---------------------------------------------------------------------------
// GX has no emissive term and no single GX fact identifies an emitter, so this
// reports *graded evidence*, never a verdict. Two measurements bracket why:
// "unlit" alone was 69 of 117 materials (59%) in one scene - far too broad to
// act on - and the 2026-08-04 Goron Mines lava, the one surface this feature
// exists for, is `lit=1`. The old unlit-based rule was wrong from both ends.
// This sums what GX does say, and it is **reported rather than acted on**:
// three revisions cut on this score and all three missed the lava. What the
// fork actually decides on is the conjunction of `readsRaster`, `colorAuthored`
// and the colour itself - see the two flags below.
//
// Measured 2026-08-05: the Goron Mines lava pool scores **0.00** on all three
// signals in two independent runs. That is what retired the score as a cut.
//
// docs/dx9/remix-material-interface.md §9.
struct SelfLitEvidence {
  float score = 0.f;          // 0..1, summed evidence
  uint32_t color = 0;         // 0x00RRGGBB, the colour the surface presents
  const char* why = "none";   // the evidence that fired, or why none could
  // Reported, deliberately not scored. The score's three signals all read as
  // zero on the Goron Mines lava, so the fork has to be able to cut at 0 - and
  // at 0 it needs something other than the score to keep the world from
  // glowing. This is that something: the presented colour came entirely from
  // TEV constants, with no vertex-stream contribution at all.
  bool colorAuthored = false;
  // Always true at the one call site; the fork reads it to tell "aurora
  // evaluated this material and scored 0" apart from "no aurora present".
  bool evaluated = false;
  // Whether the TEV colour program reads the rasterized (lit) channel at all.
  // Reported alongside matrep.k's lit= because the two disagree on 10 of 77
  // materials, and where they disagree this one is right.
  bool readsRaster = false;
};

// Weights, measured rather than chosen: each fact alone is far too broad
// (`overRange` was 26 of 76 materials in one scene), their conjunctions small.
// The fork's default cut (0.70) still passes the historical rule unlit+register;
// lowering it admits the over-range materials on their own. Changing a weight
// changes what that default means - bump both together (§9).
constexpr float kScoreUnlit = 0.50f;     // TEV colour program never reads the lit channel
constexpr float kScoreRegSrc = 0.25f;    // colour authored in a register, not per-vertex
constexpr float kScoreOverRange = 0.25f; // a TEV stage scales past what GX can display

SelfLitEvidence evaluate_self_lit(const AlbedoIntent& albedo) noexcept {
  SelfLitEvidence e;
  if (g_gxState.projType == GX_ORTHOGRAPHIC) {
    e.why = "ortho";
    return e;
  }
  if (!albedo.valid || !albedo.usesTexture) {
    // Nothing evaluable to take a colour from, so there is nothing to emit.
    e.why = "noColor";
    return e;
  }
  // Past both early returns there is a colour to emit, so the fork may consider
  // this draw at all. Orthographic (HUD) and unevaluable draws never become
  // candidates however low the fork's threshold goes.
  e.evaluated = true;

  const auto& chan = g_gxState.colorChannelConfig[GX_COLOR0];

  // "Takes no light" means the TEV *colour program* never reads the rasterized
  // channel - not that the channel config says lighting is off. Measured
  // 2026-08-05 over a Goron Mines session, and the difference is the whole
  // defect: 10 of 77 materials have lighting enabled and still never read
  // RASC, so their colour is fixed regardless of the lights. The Goron Mines
  // lava is one of them, which is why it scored 0.00 for two sessions.
  //
  // The old test was also wrong in the other direction: lighting *disabled*
  // with RASC read and matSrc=GX_SRC_VTX is this game's baked room lighting
  // (§7c) - the opposite of an emitter - and it used to score the full 0.50.
  bool readsRas = false;
  for (uint32_t i = 0; i < std::max<uint32_t>(g_gxState.numTevStages, 1); ++i) {
    const auto& cp = g_gxState.tevStages[i].colorPass;
    for (const GXTevColorArg arg : {cp.a, cp.b, cp.c, cp.d}) {
      if (arg == GX_CC_RASC || arg == GX_CC_RASA) {
        readsRas = true;
        break;
      }
    }
    if (readsRas) {
      break;
    }
  }
  e.readsRaster = readsRas;
  const bool unlit = !readsRas;
  // Scored separately rather than required: a register source says the channel
  // colour is authored rather than being §7c's per-vertex baked lighting.
  const bool regSrc = chan.matSrc == GX_SRC_REG;
  // A TEV scale above 1 multiplies the stage result past 1.0, which the console
  // then clamps. It is the only thing in GX that states "brighter than the
  // display can show" - the closest the format comes to an emissive term.
  bool overRange = false;
  for (uint32_t i = 0; i < std::max<uint32_t>(g_gxState.numTevStages, 1); ++i) {
    const auto sc = g_gxState.tevStages[i].colorOp.scale;
    if (sc == GX_CS_SCALE_2 || sc == GX_CS_SCALE_4) {
      overRange = true;
      break;
    }
  }

  e.score = (unlit ? kScoreUnlit : 0.f) + (regSrc ? kScoreRegSrc : 0.f) +
            (overRange ? kScoreOverRange : 0.f);
  // "noRas" rather than "unlit": the fact scored is that the colour program
  // never reads the lit channel, which is not the same as lighting being off.
  e.why = unlit ? (regSrc ? (overRange ? "noRas+reg+over" : "noRas+reg")
                          : (overRange ? "noRas+over" : "noRas"))
                : (regSrc ? (overRange ? "reg+over" : "reg") : (overRange ? "over" : "none"));

  // The endpoint that carries the material's colour, same rule the albedo hint
  // uses: prefer the more chromatic end, break ties towards the brighter one.
  const uint32_t rgb0 = albedo.out0 & 0x00FFFFFFu;
  const uint32_t rgb1 = albedo.out1 & 0x00FFFFFFu;

  // "The material has a colour of its own." Two ways it can fail to, and both
  // matter:
  //   - the colour is mixed from the vertex stream, which in this game is baked
  //     room lighting (§7c) rather than an authored colour;
  //   - the program is a bare texture pass-through, black to white. That is the
  //     shape of every EFB copy and full-screen quad in the scene - 9 of the 20
  //     self-lit materials in the 2026-08-05 Goron Mines log - and making a
  //     screen blit emit light is the failure this rules out.
  e.colorAuthored = !albedo.usesVertexColor && !(rgb0 == 0x000000u && rgb1 == 0xFFFFFFu);
  const uint32_t c0 = chroma_of(rgb0), c1 = chroma_of(rgb1);
  e.color = (c1 > c0 || (c1 == c0 && luma_of(rgb1) >= luma_of(rgb0))) ? rgb1 : rgb0;
  return e;
}

} // namespace

uint32_t apply_tev(const DecodedDraw& draw) noexcept {
  const uint32_t numStages = std::max<uint32_t>(g_gxState.numTevStages, 1);
  ConstAlloc consts;

  // Claim TFACTOR for the albedo's colour before anything else can (see
  // evaluate_albedo). Done here rather than at the hint stage below because the
  // constant slots are allocated as the real stages are emitted, and the hint
  // is written before any of them have run.
  const AlbedoIntent albedo = evaluate_albedo(draw);
  const SelfLitEvidence selfLit = evaluate_self_lit(albedo);
  const bool hasHintTint = albedo.hasTint;
  // A constant scale on the texture's alpha, carried in TFACTOR's alpha channel
  // so it does not compete with the colour tint in the RGB channels.
  const bool hasAlphaScale = albedo.alphaValid && albedo.alphaUsesTexture && albedo.alphaScale < 0xFFu;
  const uint32_t hintTint =
      ((hasAlphaScale ? albedo.alphaScale : 0xFFu) << 24) | (albedo.tint & 0x00FFFFFFu);
  if (hasHintTint || hasAlphaScale) {
    // materialize() compares the whole 32-bit value, so a real stage wanting a
    // different constant takes the per-stage D3DTSS_CONSTANT slot instead -
    // which Remix never reads, and which devices without
    // D3DPMISCCAPS_PERSTAGECONSTANT do not have at all (it warns and reuses
    // TFACTOR there).
    consts.tfactor = hintTint;
    consts.tfactorUsed = true;
  }

  // Decisions recorded for the material report emitted at the tail of this
  // function. They are the whole point of the report: every one of them used to
  // be invisible, which is why this defect took three sessions to locate.
  // Field meanings: docs/dx9/material-report.md.
  const char* hintDecision = "notReached";
  const char* hintForm = "-";
  const char* tintDecision = hasHintTint ? "detected" : "none";
  bool hintLooseWouldSuppress = false;
  IDirect3DBaseTexture9* hintTexture = nullptr;

  // Identity of this material configuration, used to report each distinct one
  // once. Hashed over the GX stage configs, so two materials that differ only
  // in which texture object is bound collapse together - which is what we want,
  // since the translation decisions depend on the configuration, not the pixels.
  uint64_t matKey = 0xD1A6;
  for (uint32_t i = 0; i < numStages; ++i) {
    matKey = matKey * 1315423911u + xxh3_hash(g_gxState.tevStages[i], 0);
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
    // It writes TEMP, not CURRENT, so it cannot clobber the value the real
    // stages build on - TEMP is only ever live within a single GX stage's own
    // decomposition, never across stages. Only where the device has no TEMP
    // register does it write CURRENT, which is safe just at the head of the
    // chain and only when nothing in this GX stage reads CURRENT back.
    if (!remixHintConsidered && hasTexture && d3dStage + need < MaxStages) {
      remixHintConsidered = true;
      const int albedoIdx = preferred_albedo_stage();
      const auto& albedoStage =
          albedoIdx >= 0 ? g_gxState.tevStages[static_cast<size_t>(albedoIdx)] : stage;
      const PassOp& colorLead = (anyTemp && cp.hasTemp) ? cp.tempOp : cp.finals[0];
      const PassOp& alphaLead = (anyTemp && ap.hasTemp) ? ap.tempOp : ap.finals[0];
      // A stage that already presents its texture plainly still needs the hint
      // when the colour texture lives on a different stage.
      const bool sameTexture = albedoStage.texMapId == stage.texMapId;
      // Opacity must reach Remix as the texture's own alpha or alpha-tested
      // cutouts break (foliage becomes solid quads, grass disappears). That is
      // the half of the hint that is never in question, so it gates every
      // suppression below.
      const bool alphaPlain = op_is_plain_texture(alphaLead);
      const bool colorPlain = op_is_plain_texture(colorLead);
      // The hint actively destroys a tint Remix would otherwise have read, so
      // suppress it when this stage IS the whole material and Remix decodes it.
      // Restricted to single-stage materials on purpose: on a multi-stage
      // material a later stage may change the colour, and Remix reading only
      // this one would then be confidently wrong. The report's hintLoose field
      // measures what dropping that restriction would catch.
      // Background: docs/dx9/remix-material-interface.md §5.
      const bool colorDecodable = remix_decodes_albedo(colorLead, consts);
      const bool decodableSuppress = sameTexture && alphaPlain && colorDecodable && numStages == 1;
      hintLooseWouldSuppress = sameTexture && alphaPlain && colorDecodable && !colorPlain && numStages != 1;
      const bool alreadyPlain = (colorPlain && alphaPlain && sameTexture) || decodableSuppress;
      const bool currentSafe = g_dx9.tssTemp || (d3dStage == 0 && !pass_reads(cp, D3DTA_CURRENT) &&
                                                 !pass_reads(ap, D3DTA_CURRENT));
      if (alreadyPlain) {
        hintDecision = decodableSuppress && !colorPlain ? "skip:remixReadsItAlready" : "skip:alreadyPlain";
      } else if (!currentSafe) {
        hintDecision = "skip:unsafeCurrent";
      }
      if (!alreadyPlain && currentSafe) {
        IDirect3DBaseTexture9* tex = resolve_texmap(albedoStage.texMapId);
        if (tex == nullptr) {
          hintDecision = "skip:noTexture";
        }
        if (tex != nullptr) {
          hintDecision = "emitted";
          hintTexture = tex;
          set_texture(d3dStage, tex);
          apply_sampler(d3dStage, albedoStage.texMapId);
          set_tss(d3dStage, D3DTSS_TEXCOORDINDEX, apply_texgen(d3dStage, albedoStage.texCoordId, draw));
          // What the hint advertises, in priority order. Remix reads this one
          // stage, so its two argument slots carry whatever matters most:
          //
          //   1. add:tint   - a coloured floor, which a multiply cannot express
          //                   and which must therefore occupy the hint itself
          //   2. mod:vtx    - nothing evaluable; the old unconditional shape.
          //                   A MODULATE tint rides the extra stage below (§4)
          //   3. mod:vtxMat - the vertex stream is authored material colour
          //                   (§7c) and must reach Remix
          //   4. mod:tint   - no vertex colour to preserve
          //   5. tex        - the plain texture
          if (albedo.hasTint && albedo.hintOp == D3DTOP_ADD) {
            set_tss(d3dStage, D3DTSS_COLOROP, D3DTOP_ADD);
            set_tss(d3dStage, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            set_tss(d3dStage, D3DTSS_COLORARG2, D3DTA_TFACTOR);
            hintForm = "add:tint";
          } else if (!albedo.valid) {
            // Nothing could be evaluated, so fall back to the old shape.
            set_tss(d3dStage, D3DTSS_COLOROP, D3DTOP_MODULATE);
            set_tss(d3dStage, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            set_tss(d3dStage, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
            hintForm = "mod:vtx";
          } else if (albedo.usesVertexColor && albedo.vertexColorIsMaterial) {
            // GX says this stream is material colour, not baked lighting, so it
            // has to reach Remix or the surface loses its colour. It takes both
            // argument slots, and unlike the mod:vtx branch this form gets no
            // extra tint stage below (see tintNeedsOwnStage).
            set_tss(d3dStage, D3DTSS_COLOROP, D3DTOP_MODULATE);
            set_tss(d3dStage, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            set_tss(d3dStage, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
            hintForm = "mod:vtxMat";
          } else if (albedo.hasTint) {
            set_tss(d3dStage, D3DTSS_COLOROP, D3DTOP_MODULATE);
            set_tss(d3dStage, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            set_tss(d3dStage, D3DTSS_COLORARG2, D3DTA_TFACTOR);
            hintForm = "mod:tint";
          } else {
            set_tss(d3dStage, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            set_tss(d3dStage, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            hintForm = "tex";
          }
          // Opacity is the texture's own alpha - what Remix alpha-tests
          // against, and what gives foliage cards and grass blades their cutout
          // shape. A constant scale on it (a HUD effect fading in) rides
          // TFACTOR's alpha channel: dropping it made such quads reach Remix
          // fully opaque and draw their whole rectangle.
          if (hasAlphaScale) {
            set_tss(d3dStage, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
            set_tss(d3dStage, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
            set_tss(d3dStage, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);
          } else {
            set_tss(d3dStage, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            set_tss(d3dStage, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
          }
          if (g_dx9.tssTemp) {
            set_tss(d3dStage, D3DTSS_RESULTARG, D3DTA_TEMP);
          }
          ++d3dStage;

          // Carry the albedo tint, which the hint's TEXTURE x DIFFUSE cannot
          // express. Remix decodes one extra MODULATE against TFACTOR, matched
          // against the register the *previous* stage wrote - so this must sit
          // immediately after the hint and read the register the hint wrote
          // (docs/dx9/remix-material-interface.md §4). Only the mod:vtx form
          // needs it; the tint forms already carry the tint themselves.
          const bool tintNeedsOwnStage = hasHintTint && albedo.hintOp == D3DTOP_MODULATE &&
                                         std::strcmp(hintForm, "mod:vtx") == 0;
          if (tintNeedsOwnStage && d3dStage + need >= MaxStages) {
            tintDecision = "skip:budget";
          } else if (hasHintTint && !tintNeedsOwnStage) {
            tintDecision = "inHint";
          }
          if (tintNeedsOwnStage && d3dStage + need < MaxStages) {
            tintDecision = "emitted";
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
          }
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

  // The two-colour ramp, which is this game's dominant material shape and which
  // no stock Remix texture op can express (docs/dx9/remix-material-interface.md
  // §10). The fork reconstructs `lerp(out0, out1, texture)` exactly, but only if
  // it can find both endpoints: one rides TFACTOR, the other rides
  // D3DMATERIAL9::Diffuse.
  //
  // TFACTOR is a shared render state and a real stage can claim it ahead of the
  // hint, so this checks what it actually ended up holding rather than assuming.
  // Declining is safe - the material falls back to the single-op approximation.
  const uint32_t rampLo = albedo.out0 & 0x00FFFFFFu;
  const uint32_t rampHi = albedo.out1 & 0x00FFFFFFu;
  // Unused TFACTOR takes a sentinel above 24 bits, so it matches neither endpoint.
  const uint32_t tfRgb = consts.tfactorUsed ? (consts.tfactor & 0x00FFFFFFu) : 0xFF000000u;
  bool rampValid = false;
  bool rampTFactorIsHigh = false;
  uint32_t rampOther = 0;
  const char* rampWhy = "no";
  if (!albedo.valid || !albedo.usesTexture) {
    rampWhy = "unevaluable";
  } else if (albedo.usesVertexColor) {
    // The endpoints were evaluated against one vertex colour; a streamed one
    // would make them wrong per-vertex.
    rampWhy = "usesVtx";
  } else if (rampLo == rampHi) {
    rampWhy = "flat";
  } else if (tfRgb == rampHi) {
    rampValid = true;
    rampTFactorIsHigh = true;
    rampOther = rampLo;
    rampWhy = "tfHigh";
  } else if (tfRgb == rampLo) {
    rampValid = true;
    rampOther = rampHi;
    rampWhy = "tfLow";
  } else {
    rampWhy = "tfTaken";
  }

  // Hand it all to Remix through the D3DMATERIAL9 side channel (free here:
  // D3DRS_LIGHTING is off, so nothing else consumes it). Field map and the
  // fork's read sites: set_remix_material in dx9_internal.hpp.
  set_remix_material(D3DCOLORVALUE{static_cast<float>((selfLit.color >> 16) & 0xFFu) / 255.f,
                                   static_cast<float>((selfLit.color >> 8) & 0xFFu) / 255.f,
                                   static_cast<float>(selfLit.color & 0xFFu) / 255.f, selfLit.score},
                     D3DCOLORVALUE{static_cast<float>((rampOther >> 16) & 0xFFu) / 255.f,
                                   static_cast<float>((rampOther >> 8) & 0xFFu) / 255.f,
                                   static_cast<float>(rampOther & 0xFFu) / 255.f,
                                   rampValid ? 1.f : 0.f},
                     rampTFactorIsHigh ? 1.f : 0.f,
                     albedo.vertexColorIsMaterial ? 1.f : 0.f,
                     selfLit.evaluated ? 1.f : 0.f,
                     selfLit.colorAuthored ? 1.f : 0.f,
                     selfLit.readsRaster ? 0.f : 1.f,
                     g_dusklightWaterRole == GX_AURORA_DUSKLIGHT_WATER_SURFACE ? 1.f : 0.f,
                     g_dusklightWaterRole == GX_AURORA_DUSKLIGHT_WATER_PROJECTED ? 1.f : 0.f);

  // Hop 3 of 3, reported once ever: a draw was translated while the water mark was set, so
  // a D3DMATERIAL9 carrying Ambient.g = 1 reached the device. If this line is present and
  // Remix still logs no dusklight.water, the loss is on Remix's side of SetMaterial, not
  // here. See set_dusklight_water in dx9.hpp for the other two hops.
  if (g_dusklightWaterRole != GX_AURORA_DUSKLIGHT_WATER_NONE) {
    static bool s_reportedSurface = false;
    static bool s_reportedProjected = false;
    const bool projected = g_dusklightWaterRole == GX_AURORA_DUSKLIGHT_WATER_PROJECTED;
    bool& reported = projected ? s_reportedProjected : s_reportedSurface;
    if (!reported) {
      reported = true;
      Log.info("dx9.water: first {} draw translated (matKey {:#x})",
               projected ? "PROJECTED" : "SURFACE", matKey);
    }
  }

  // The material translation report. Emitted here because this is the only
  // point where the GX input, every decision taken, and the finished D3D9 state
  // all exist at once. Reading guide: docs/dx9/material-report.md.
  if (matrep_should_emit(matKey)) {
    const int albedoIdx = preferred_albedo_stage();
    uint32_t aw = 0;
    uint32_t ah = 0;
    uint32_t afmt = 0;
    int amap = -1;
    if (albedoIdx >= 0) {
      const auto& s = g_gxState.tevStages[static_cast<size_t>(albedoIdx)];
      amap = static_cast<int>(s.texMapId);
      if (s.texMapId >= 0 && s.texMapId < static_cast<int>(gx::MaxTextures)) {
        const auto& obj = g_gxState.loadedTextures[static_cast<size_t>(s.texMapId)];
        aw = obj.width();
        ah = obj.height();
        afmt = obj.format();
      }
    }
    // Pointer printed as bare uppercase 16-hex so it matches the fork's
    // matrep.rmx tex0ptr byte for byte. The two sides previously formatted it
    // differently and the join key silently did not join.
    Log.info("matrep.sum mk={:016X} gxStages={} d3dStages={} albedoGx={} albedoMap={} "
             "albedoTex={}x{} fmt={} colorFmt={} shape={} out0={:06X} out1={:06X} "
             "usesTex={} usesVtx={} alphaScale={:02X} hint={} form={} hintTex={:016X} hintLoose={} "
             "tint={} tintVal={:08X} tfactor={:08X} tfUsed={} vtxColor={} "
             "selfLit={} emisScore={:.2f} emisCol={:06X} emisEval={} emisAuthored={} "
             "blend={} ras={} ramp={} rampOther={:06X} vtxUse={} grp={}",
             matKey, numStages, d3dStage, albedoIdx, amap, aw, ah, static_cast<GXTexFmt>(afmt),
             is_color_texture_format(afmt) ? 1 : 0, albedo.valid ? albedo.shape : "unevaluable",
             albedo.out0 & 0x00FFFFFFu, albedo.out1 & 0x00FFFFFFu, albedo.usesTexture ? 1 : 0,
             albedo.usesVertexColor ? 1 : 0, albedo.alphaValid ? albedo.alphaScale : 0xFFu,
             hintDecision, hintForm,
             reinterpret_cast<uintptr_t>(hintTexture), hintLooseWouldSuppress ? 1 : 0, tintDecision,
             hintTint, consts.tfactorUsed ? consts.tfactor : 0u, consts.tfactorUsed ? 1 : 0,
             draw.hasVertexColor ? "stream"
                                 : (draw.defaultDiffuse == 0xFFFFFFFFu ? "default-white" : "matColor"),
             selfLit.why, selfLit.score, selfLit.color, selfLit.evaluated ? 1 : 0,
             selfLit.colorAuthored ? 1 : 0, blend_name(), selfLit.readsRaster ? 1 : 0,
             rampWhy, rampOther,
             !draw.hasVertexColor ? "const"
                                  : (albedo.vertexColorIsMaterial ? "material" : "bakedLight"),
             g_gxState.currentDebugGroup()[0] != '\0' ? g_gxState.currentDebugGroup() : "-");

    // Per-stage GX detail: the material the game asked for, not our reduction
    // of it. GX enum names come from lib/gx/gx_fmt.hpp.
    for (uint32_t i = 0; i < numStages; ++i) {
      const auto& s = g_gxState.tevStages[i];
      const bool textured = s.texMapId != GX_TEXMAP_NULL && s.texCoordId != GX_TEXCOORD_NULL;
      uint32_t w = 0;
      uint32_t h = 0;
      uint32_t fmt = 0;
      if (textured && s.texMapId >= 0 && s.texMapId < static_cast<int>(gx::MaxTextures)) {
        const auto& obj = g_gxState.loadedTextures[static_cast<size_t>(s.texMapId)];
        w = obj.width();
        h = obj.height();
        fmt = obj.format();
      }
      Log.info("matrep.gx  mk={:016X} st={}/{} map={} coord={} tex={}x{} fmt={} "
               "cc=[{},{},{},{}] cop={} scale={} out={} ca=[{},{},{},{}] aop={} "
               "kc={} ka={} ind={}",
               matKey, i, numStages, s.texMapId, s.texCoordId, w, h, static_cast<GXTexFmt>(fmt),
               s.colorPass.a, s.colorPass.b, s.colorPass.c, s.colorPass.d, s.colorOp.op, s.colorOp.scale,
               s.colorOp.outReg, s.alphaPass.a, s.alphaPass.b, s.alphaPass.c, s.alphaPass.d, s.alphaOp.op,
               s.kcSel, s.kaSel, s.indTexMtxId != GX_ITM_OFF ? 1 : 0);
    }

    // The GX colour constants, which is where this game keeps the colour that
    // distinguishes a green rupee from a red one, and the lighting bit that
    // says whether the surface is self-lit.
    Log.info("matrep.k   mk={:016X} K0={:08X} K1={:08X} K2={:08X} K3={:08X} "
             "C0={:08X} C1={:08X} C2={:08X} lit={} matSrc={}",
             matKey, pack_argb(g_gxState.kcolors[0]), pack_argb(g_gxState.kcolors[1]),
             pack_argb(g_gxState.kcolors[2]), pack_argb(g_gxState.kcolors[3]),
             pack_argb(g_gxState.colorRegs[1]), pack_argb(g_gxState.colorRegs[2]),
             pack_argb(g_gxState.colorRegs[3]),
             g_gxState.colorChannelConfig[GX_COLOR0].lightingEnabled ? 1 : 0,
             g_gxState.colorChannelConfig[GX_COLOR0].matSrc);

    // What we actually handed D3D9, which is all Remix ever sees.
    for (uint32_t s = 0; s < d3dStage && s < MaxStages; ++s) {
      Log.info("matrep.d3d mk={:016X} st={}/{} tex={:016X} cop={} a1={} a2={} aop={} aa1={} aa2={} "
               "res={} konst={:08X}",
               matKey, s, d3dStage, reinterpret_cast<uintptr_t>(g_cache.textures[s]),
               g_cache.tss[s][D3DTSS_COLOROP], d3dta_name(g_cache.tss[s][D3DTSS_COLORARG1]),
               d3dta_name(g_cache.tss[s][D3DTSS_COLORARG2]), g_cache.tss[s][D3DTSS_ALPHAOP],
               d3dta_name(g_cache.tss[s][D3DTSS_ALPHAARG1]), d3dta_name(g_cache.tss[s][D3DTSS_ALPHAARG2]),
               d3dta_name(g_cache.tss[s][D3DTSS_RESULTARG]), g_cache.tss[s][D3DTSS_CONSTANT]);
    }
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
