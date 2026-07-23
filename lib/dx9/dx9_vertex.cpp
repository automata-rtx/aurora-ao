#include "dx9_vertex.hpp"

#ifdef AURORA_ENABLE_D3D9

#include <algorithm>
#include <cmath>
#include <vector>

namespace aurora::dx9 {
static Module Log("aurora::dx9::vtx");

namespace {

struct AttrPlan {
  GXAttrType attrType = GX_NONE;
  uint8_t cnt = 0;      // component count (colors: 1)
  uint8_t compType = 0; // GXCompType / color type
  uint8_t frac = 0;
  uint8_t srcSize = 0;  // bytes this attr occupies in the stream
  const gx::AttrArray* array = nullptr; // for INDEX8/16
};

thread_local std::vector<uint8_t> t_scratch;

inline float read_comp(const uint8_t* p, uint8_t compType, uint8_t frac, bool be) noexcept {
  const float scale = 1.0f / static_cast<float>(1u << frac);
  switch (compType) {
  case GX_U8:
    return static_cast<float>(*p) * scale;
  case GX_S8:
    return static_cast<float>(static_cast<int8_t>(*p)) * scale;
  case GX_U16:
    return static_cast<float>(read_val<uint16_t>(p, be)) * scale;
  case GX_S16:
    return static_cast<float>(read_val<int16_t>(p, be)) * scale;
  case GX_F32:
  default:
    return read_val<float>(p, be);
  }
}

inline uint8_t comp_size(uint8_t compType) noexcept {
  switch (compType) {
  case GX_U8:
  case GX_S8:
    return 1;
  case GX_U16:
  case GX_S16:
    return 2;
  default:
    return 4;
  }
}

// Decodes a GX vertex color into D3DCOLOR (ARGB).
inline uint32_t read_color(const uint8_t* p, uint8_t type, bool be) noexcept {
  switch (type) {
  case GX_RGB565: {
    const uint16_t v = read_val<uint16_t>(p, be);
    const uint32_t r = (v >> 11 & 0x1F) * 255 / 31;
    const uint32_t g = (v >> 5 & 0x3F) * 255 / 63;
    const uint32_t b = (v & 0x1F) * 255 / 31;
    return 0xFF000000u | r << 16 | g << 8 | b;
  }
  case GX_RGB8:
    return 0xFF000000u | static_cast<uint32_t>(p[0]) << 16 | static_cast<uint32_t>(p[1]) << 8 | p[2];
  case GX_RGBX8:
    return 0xFF000000u | static_cast<uint32_t>(p[0]) << 16 | static_cast<uint32_t>(p[1]) << 8 | p[2];
  case GX_RGBA4: {
    const uint16_t v = read_val<uint16_t>(p, be);
    const uint32_t r = (v >> 12 & 0xF) * 17;
    const uint32_t g = (v >> 8 & 0xF) * 17;
    const uint32_t b = (v >> 4 & 0xF) * 17;
    const uint32_t a = (v & 0xF) * 17;
    return a << 24 | r << 16 | g << 8 | b;
  }
  case GX_RGBA6: {
    const uint32_t v =
        static_cast<uint32_t>(p[0]) << 16 | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]);
    const uint32_t r = (v >> 18 & 0x3F) * 255 / 63;
    const uint32_t g = (v >> 12 & 0x3F) * 255 / 63;
    const uint32_t b = (v >> 6 & 0x3F) * 255 / 63;
    const uint32_t a = (v & 0x3F) * 255 / 63;
    return a << 24 | r << 16 | g << 8 | b;
  }
  case GX_RGBA8:
  default:
    return static_cast<uint32_t>(p[3]) << 24 | static_cast<uint32_t>(p[0]) << 16 |
           static_cast<uint32_t>(p[1]) << 8 | p[2];
  }
}

inline uint32_t float_to_u8(float v) noexcept {
  v = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
  return static_cast<uint32_t>(v * 255.f + 0.5f);
}

struct InfluenceRec {
  uint32_t bone;
  float weight;
};

} // namespace

bool decode_draw(GXVtxFmt fmt, uint16_t vtxCount, const uint8_t* data, uint32_t vtxSize, bool bigEndian,
                 DecodedDraw& out) noexcept {
  out = {};
  const auto& vtxFmt = g_gxState.vtxFmts[fmt];

  // Build the per-attribute source plan (mirrors calculate_last_vtx_size).
  std::array<AttrPlan, GX_VA_MAX_ATTR> plan{};
  uint32_t srcOffset = 0;
  for (int i = GX_VA_PNMTXIDX; i <= GX_VA_TEX7; ++i) {
    const auto attr = static_cast<GXAttr>(i);
    const auto type = g_gxState.vtxDesc[i];
    if (type == GX_NONE) {
      continue;
    }
    const auto& attrFmt = vtxFmt.attrs[i];
    auto& p = plan[i];
    p.attrType = type;
    p.cnt = gx::comp_cnt_count(attr, attrFmt.cnt);
    p.compType = static_cast<uint8_t>(attrFmt.type);
    p.frac = attrFmt.frac;
    const bool nbt3 = attr == GX_VA_NRM && attrFmt.cnt == GX_NRM_NBT3;
    switch (type) {
    case GX_DIRECT:
      p.srcSize = gx::comp_type_size(attr, attrFmt.type) * p.cnt;
      break;
    case GX_INDEX8:
      p.srcSize = nbt3 ? 3 : 1;
      p.array = &g_gxState.arrays[i];
      break;
    case GX_INDEX16:
      p.srcSize = nbt3 ? 6 : 2;
      p.array = &g_gxState.arrays[i];
      break;
    default:
      warn_once(0x1000 + i, "invalid vertex attr type");
      return false;
    }
    srcOffset += p.srcSize;
  }
  if (srcOffset != vtxSize || plan[GX_VA_POS].attrType == GX_NONE) {
    warn_once(0x2000 | srcOffset, "vertex size mismatch or missing position");
    return false;
  }

  const bool skinActive = g_gxState.skinningActive && g_skin.palette != nullptr && g_skin.influences != nullptr;
  const bool posIndexed = plan[GX_VA_POS].attrType == GX_INDEX8 || plan[GX_VA_POS].attrType == GX_INDEX16;
  out.skinned = skinActive && posIndexed;
  if (skinActive && !posIndexed) {
    warn_once(0x3001, "skinning active but position not indexed; drawing rigid");
  }
  out.hasPnMtxIdx = !out.skinned && plan[GX_VA_PNMTXIDX].attrType != GX_NONE;
  // Every blended draw stores at least one explicit weight. Fixed-function
  // D3D9 would accept D3DVBF_0WEIGHTS with indices alone, but the RTX Remix
  // runtime (dxvk-remix dispatchSkinning) refuses to run its GPU skinning
  // pass unless the vertex declaration carries a BLENDWEIGHT element — the
  // draw is then classified as skinned yet never skinned, mangling the mesh.
  // A stored weight of 1.0 under D3DVBF_1WEIGHTS is equivalent to 0WEIGHTS:
  // the implicit last weight becomes 0, so its (padding) bone contributes
  // nothing. See docs/dx9/gx-to-d3d9-mapping.md #6.
  if (out.skinned) {
    out.weightCount = std::clamp(g_skin.influenceCount, 2u, 4u) - 1;
  } else if (out.hasPnMtxIdx) {
    out.weightCount = 1;
  }
  const bool hasBlendIndices = out.skinned || out.hasPnMtxIdx;

  out.hasNormal = plan[GX_VA_NRM].attrType != GX_NONE;
  out.hasSpecular = plan[GX_VA_CLR1].attrType != GX_NONE;

  // Destination layout (FVF order): pos, weights, indices, normal, diffuse,
  // specular, uv0..n. Diffuse is always emitted.
  uint32_t dstOffset = 12;
  const uint32_t weightsOffset = dstOffset;
  dstOffset += out.weightCount * 4;
  const uint32_t indicesOffset = dstOffset;
  dstOffset += hasBlendIndices ? 4 : 0;
  const uint32_t normalOffset = dstOffset;
  dstOffset += out.hasNormal ? 12 : 0;
  const uint32_t diffuseOffset = dstOffset;
  dstOffset += 4;
  const uint32_t specularOffset = dstOffset;
  dstOffset += out.hasSpecular ? 4 : 0;
  uint32_t uvOffsets[8];
  for (int t = 0; t < 8; ++t) {
    if (plan[GX_VA_TEX0 + t].attrType != GX_NONE) {
      out.texSlot[t] = static_cast<int8_t>(out.uvCount);
      uvOffsets[t] = dstOffset;
      dstOffset += 8;
      ++out.uvCount;
    } else {
      uvOffsets[t] = 0;
    }
  }
  out.stride = dstOffset;
  out.vtxCount = vtxCount;

  // FVF code.
  DWORD fvf;
  if (hasBlendIndices) {
    static constexpr DWORD kBlendFvf[] = {D3DFVF_XYZB1, D3DFVF_XYZB2, D3DFVF_XYZB3, D3DFVF_XYZB4};
    fvf = kBlendFvf[out.weightCount] | D3DFVF_LASTBETA_UBYTE4;
  } else {
    fvf = D3DFVF_XYZ;
  }
  if (out.hasNormal) {
    fvf |= D3DFVF_NORMAL;
  }
  fvf |= D3DFVF_DIFFUSE;
  if (out.hasSpecular) {
    fvf |= D3DFVF_SPECULAR;
  }
  fvf |= static_cast<DWORD>(out.uvCount) << D3DFVF_TEXCOUNT_SHIFT;
  out.fvf = fvf;

  // Default diffuse when the stream carries no CLR0: channel-0 material color
  // when sourced from register, else opaque white (docs #8).
  uint32_t defaultDiffuse = 0xFFFFFFFFu;
  if (plan[GX_VA_CLR0].attrType == GX_NONE) {
    const auto& cc = g_gxState.colorChannelConfig[GX_COLOR0];
    if (cc.matSrc == GX_SRC_REG) {
      const auto& mat = g_gxState.colorChannelState[GX_COLOR0].matColor;
      defaultDiffuse = float_to_u8(mat[3]) << 24 | float_to_u8(mat[0]) << 16 | float_to_u8(mat[1]) << 8 |
                       float_to_u8(mat[2]);
    }
  }

  t_scratch.resize(static_cast<size_t>(out.stride) * vtxCount);
  uint8_t* dstBase = t_scratch.data();
  out.verts = dstBase;

  const uint8_t* src = data;
  for (uint32_t v = 0; v < vtxCount; ++v, src += vtxSize) {
    uint8_t* dst = dstBase + static_cast<size_t>(v) * out.stride;
    const uint8_t* s = src;
    uint32_t posIndex = 0;
    uint8_t pnmtxidx = 0;

    for (int i = GX_VA_PNMTXIDX; i <= GX_VA_TEX7; ++i) {
      const auto& p = plan[i];
      if (p.attrType == GX_NONE) {
        continue;
      }

      // Locate the attribute's component data (direct or via array fetch).
      const uint8_t* comps = s;
      bool compBe = bigEndian;
      if (p.attrType == GX_INDEX8 || p.attrType == GX_INDEX16) {
        const uint32_t index =
            p.attrType == GX_INDEX8 ? *s : read_val<uint16_t>(s, bigEndian);
        if (i == GX_VA_POS) {
          posIndex = index;
        }
        comps = static_cast<const uint8_t*>(p.array->data) + static_cast<size_t>(index) * p.array->stride;
        compBe = !p.array->le;
      }

      switch (i) {
      case GX_VA_PNMTXIDX:
        pnmtxidx = *s / 3;
        break;
      case GX_VA_POS: {
        float pos[3] = {0.f, 0.f, 0.f};
        const uint8_t csize = comp_size(p.compType);
        for (uint8_t c = 0; c < p.cnt && c < 3; ++c) {
          pos[c] = read_comp(comps + c * csize, p.compType, p.frac, compBe);
        }
        std::memcpy(dst, pos, 12);
        break;
      }
      case GX_VA_NRM: {
        float nrm[3] = {0.f, 0.f, 0.f};
        const uint8_t csize = comp_size(p.compType);
        // NBT/NBT3 store 9 components; only the normal (first 3) is used.
        for (uint8_t c = 0; c < 3; ++c) {
          nrm[c] = read_comp(comps + c * csize, p.compType, p.frac, compBe);
        }
        std::memcpy(dst + normalOffset, nrm, 12);
        break;
      }
      case GX_VA_CLR0: {
        const uint32_t color = read_color(comps, p.compType, compBe);
        std::memcpy(dst + diffuseOffset, &color, 4);
        break;
      }
      case GX_VA_CLR1: {
        const uint32_t color = read_color(comps, p.compType, compBe);
        std::memcpy(dst + specularOffset, &color, 4);
        break;
      }
      default:
        if (i >= GX_VA_TEX0 && i <= GX_VA_TEX7) {
          const int t = i - GX_VA_TEX0;
          float uv[2] = {0.f, 0.f};
          const uint8_t csize = comp_size(p.compType);
          for (uint8_t c = 0; c < p.cnt && c < 2; ++c) {
            uv[c] = read_comp(comps + c * csize, p.compType, p.frac, compBe);
          }
          std::memcpy(dst + uvOffsets[t], uv, 8);
        }
        // TEXnMTXIDX: consumed from the stream, per-vertex texture matrix
        // selection is not supported in v1 (docs, unsupported #15).
        break;
      }
      s += p.srcSize;
    }

    if (plan[GX_VA_CLR0].attrType == GX_NONE) {
      std::memcpy(dst + diffuseOffset, &defaultDiffuse, 4);
    }

    if (out.skinned) {
      const auto* recs = reinterpret_cast<const InfluenceRec*>(g_skin.influences) +
                         static_cast<size_t>(posIndex) * g_skin.influenceCount;
      float weights[3] = {0.f, 0.f, 0.f};
      uint32_t indices = 0;
      for (uint32_t k = 0; k < g_skin.influenceCount; ++k) {
        uint32_t bone = recs[k].bone;
        if (bone > 255) {
          warn_once(0x3002, "skin bone index > 255");
          bone = 255;
        }
        indices |= bone << (k * 8);
        if (k < out.weightCount) {
          weights[k] = recs[k].weight;
        }
      }
      if (out.weightCount > 0) {
        std::memcpy(dst + weightsOffset, weights, out.weightCount * 4);
      }
      std::memcpy(dst + indicesOffset, &indices, 4);
    } else if (out.hasPnMtxIdx) {
      const float one = 1.0f;
      std::memcpy(dst + weightsOffset, &one, 4);
      const uint32_t indices = pnmtxidx;
      std::memcpy(dst + indicesOffset, &indices, 4);
    }
  }

  return true;
}

} // namespace aurora::dx9

#endif // AURORA_ENABLE_D3D9
