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
  // Bound for the INDEX8/16 fetch, precomputed here rather than in the per-vertex
  // loop. hasArray is false when the array has no backing store at all -
  // J3DShape issues GDSetArraySized(attr, nullptr, 0, stride) for every attribute
  // a model lacks, so that is a real state rather than a defensive one.
  uint32_t maxIndex = 0;
  bool hasArray = false;
};

// Substituted for an attribute with no backing array, so every consumer below
// reads defined zeros instead of forming an address from a null base. Sized for
// the widest element one index can address (NBT: 9 x F32); the decoder never
// reads more than 3 components from it.
constexpr uint8_t kZeroComps[36] = {};

thread_local std::vector<uint8_t> t_scratch;
// GX position-matrix slot per decoded vertex; only filled for matrix-palette
// draws, and only read by draw_palette_split before the next decode.
thread_local std::vector<uint8_t> t_vtxSlots;

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
    if (p.array != nullptr) {
      // Bound for the array fetch in the decode loop below, precomputed once per
      // attribute rather than per vertex. compBytes is the span one index
      // addresses:
      //   - colours take their width from gx::comp_type_size, not the local
      //     comp_size(), whose parameter is a GXCompType and whose enumerators
      //     collide with the colour formats. A flat 4 would be an overestimate
      //     for RGB565/RGBA4 and would clamp the array's last entry.
      //   - NBT3 is three separate indices into 3-component elements (which is
      //     why srcSize is 3/6 above), so its span is 3 components even though
      //     comp_cnt_count reports 9. Plain NBT is one index over all 9.
      // AttrArray::size is exact where J3DModelLoader derives it and an admitted
      // overcount on J3DShape's deform path - safe for a bound, which is part of
      // why an out-of-range index clamps below rather than dropping the draw.
      // stride 0 (GXInit's default) yields maxIndex 0, leaving the address
      // exactly where it is today. 2026-08-16.
      const uint32_t elemComps = nbt3 ? 3u : static_cast<uint32_t>(p.cnt);
      const uint32_t compBytes =
          elemComps * ((attr == GX_VA_CLR0 || attr == GX_VA_CLR1) ? gx::comp_type_size(attr, attrFmt.type)
                                                                  : comp_size(p.compType));
      p.hasArray = p.array->data != nullptr && p.array->size >= compBytes;
      p.maxIndex = (p.hasArray && p.array->stride != 0) ? (p.array->size - compBytes) / p.array->stride : 0u;
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
  // UV sets are emitted with the material's base texture first, because Remix
  // reconstructs a draw's material from whichever stage carries the *lowest*
  // D3DTSS_TEXCOORDINDEX. Otherwise GX attribute order decides the albedo - on
  // character eyes (eyeball + highlight + shadow masks composited in one draw)
  // that picked a mask. docs/dx9/unsupported-effects.md R1.
  int preferredAttr = -1;
  for (uint32_t s = 0; s < g_gxState.numTevStages && preferredAttr < 0; ++s) {
    const auto& stage = g_gxState.tevStages[s];
    if (stage.texMapId == GX_TEXMAP_NULL || stage.texCoordId == GX_TEXCOORD_NULL ||
        stage.texCoordId >= static_cast<int>(gx::MaxTexCoord)) {
      continue;
    }
    const auto src = g_gxState.tcgs[stage.texCoordId].src;
    if (src >= GX_TG_TEX0 && src <= GX_TG_TEX7) {
      const int attr = src - GX_TG_TEX0;
      if (plan[GX_VA_TEX0 + attr].attrType != GX_NONE) {
        preferredAttr = attr;
      }
    }
  }

  uint32_t uvOffsets[8];
  const auto assign_uv = [&](int t) {
    out.texSlot[t] = static_cast<int8_t>(out.uvCount);
    uvOffsets[t] = dstOffset;
    dstOffset += 8;
    ++out.uvCount;
  };
  for (int t = 0; t < 8; ++t) {
    uvOffsets[t] = 0;
  }
  if (preferredAttr >= 0) {
    assign_uv(preferredAttr);
  }
  for (int t = 0; t < 8; ++t) {
    if (t != preferredAttr && plan[GX_VA_TEX0 + t].attrType != GX_NONE) {
      assign_uv(t);
    }
  }
  out.stride = dstOffset;
  out.vtxCount = vtxCount;
  out.blendIndexOffset = indicesOffset;

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
  // when sourced from register, else opaque white. Colour arriving this way is
  // authored by definition, so the TEV mapper evaluates it as material colour
  // rather than treating DIFFUSE as white - that erasure was a real bug.
  // Known divergence from GX, recorded not forgotten: a draw that HAS CLR0 but
  // sets matSrc=REG should take the register colour and does not; whether TP
  // ever emits that is unmeasured. It is a log question - matrep.k prints
  // matSrc= per material.
  // docs/dx9/gx-to-d3d9-mapping.md #8, remix-material-interface.md §7c.
  uint32_t defaultDiffuse = 0xFFFFFFFFu;
  out.hasVertexColor = plan[GX_VA_CLR0].attrType != GX_NONE;
  if (plan[GX_VA_CLR0].attrType == GX_NONE) {
    const auto& cc = g_gxState.colorChannelConfig[GX_COLOR0];
    if (cc.matSrc == GX_SRC_REG) {
      const auto& mat = g_gxState.colorChannelState[GX_COLOR0].matColor;
      defaultDiffuse = float_to_u8(mat[3]) << 24 | float_to_u8(mat[0]) << 16 | float_to_u8(mat[1]) << 8 |
                       float_to_u8(mat[2]);
    }
  }
  out.defaultDiffuse = defaultDiffuse;

  t_scratch.resize(static_cast<size_t>(out.stride) * vtxCount);
  uint8_t* dstBase = t_scratch.data();
  out.verts = dstBase;

  // GX position-matrix slot -> D3D9 blend index, assigned in first-use order
  // so a draw only ever needs as many blend indices as it has distinct
  // matrices (see DecodedDraw::pnMtxSlots).
  std::array<int8_t, gx::MaxPnMtx> pnMtxRemap{};
  pnMtxRemap.fill(-1);
  const auto maxBlendIndex = static_cast<uint32_t>(g_dx9.caps.MaxVertexBlendMatrixIndex);
  if (out.hasPnMtxIdx) {
    t_vtxSlots.resize(vtxCount);
    out.pnMtxPerVertex = t_vtxSlots.data();
  }

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
        uint32_t index = p.attrType == GX_INDEX8 ? *s : read_val<uint16_t>(s, bigEndian);
        if (i == GX_VA_POS) {
          posIndex = index;
        }
        if (!p.hasArray) {
          // The stream indexes an attribute that has no backing array - either
          // never set, or set to (nullptr, 0) the way J3DShape does for every
          // attribute a model lacks. Read defined zeros rather than form an
          // address off a null base. Not observed; if it ever fires, the pairing
          // of vtxDesc and the array set is what to look at.
          warn_once(0x2300u | i, "vertex: indexed attribute has no backing array; reading zeros");
          comps = kZeroComps;
        } else {
          // The index is an untrusted u8/u16 straight off the FIFO and this is
          // the only place its address is formed. Clamping keeps the mesh on
          // screen and localised - the bound can be conservative (see maxIndex) -
          // while the warn turns what used to be a silent read of adjacent host
          // memory into an answer a log can give. Not observed firing.
          // 2026-08-16.
          if (index > p.maxIndex) {
            warn_once(0x2200u | i, "vertex: array index past the attribute array; clamped to 0");
            index = 0;
          }
          comps = static_cast<const uint8_t*>(p.array->data) + static_cast<size_t>(index) * p.array->stride;
          compBe = !p.array->le;
        }
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
        // selection is not implemented - the per-draw matrix is used instead.
        // docs/dx9/unsupported-effects.md #15.
        break;
      }
      s += p.srcSize;
    }

    if (plan[GX_VA_CLR0].attrType == GX_NONE) {
      std::memcpy(dst + diffuseOffset, &defaultDiffuse, 4);
    }

    if (out.skinned) {
      float weights[3] = {0.f, 0.f, 0.f};
      uint32_t indices = 0;
      // The influence table's length arrives with the skinning bracket
      // (GX_AURORA_SET_SKINNING carries vtxCount) and was stored and never read.
      // The bracket persists until GX_AURORA_CLEAR_SKINNING, so an indexed draw
      // issued inside it against a larger position array would read past the end
      // of a host buffer the game owns. Structurally the bracket spans one shape
      // and the table is sized from that same shape, so this is hardening, not an
      // observed failure. 2026-08-16.
      //
      // What the fallback actually produces, which is what the warning says: weights
      // and indices are left at their initialisers, so under D3DVBF_nWEIGHTS the
      // implicit last weight is 1 - sum(stored) = 1.0 and its blend-index byte is 0.
      // The vertex is therefore bound rigidly to palette matrix 0 - g_cache.world[0],
      // some arbitrary bone - not left unweighted on the model's base transform. On
      // screen that is a vertex dragged to one bone, which is what to look for.
      if (posIndex >= g_skin.vtxCount) {
        warn_once(0x3003, "skinning: position index past the influence table; vertex bound rigidly to palette matrix 0");
      } else {
        const auto* recs = reinterpret_cast<const InfluenceRec*>(g_skin.influences) +
                           static_cast<size_t>(posIndex) * g_skin.influenceCount;
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
      }
      if (out.weightCount > 0) {
        std::memcpy(dst + weightsOffset, weights, out.weightCount * 4);
      }
      std::memcpy(dst + indicesOffset, &indices, 4);
    } else if (out.hasPnMtxIdx) {
      const float one = 1.0f;
      std::memcpy(dst + weightsOffset, &one, 4);
      const uint32_t slot = pnmtxidx < gx::MaxPnMtx ? pnmtxidx : 0;
      t_vtxSlots[v] = static_cast<uint8_t>(slot);
      if (pnMtxRemap[slot] < 0) {
        if (out.pnMtxCount <= maxBlendIndex) {
          pnMtxRemap[slot] = static_cast<int8_t>(out.pnMtxCount);
          out.pnMtxSlots[out.pnMtxCount] = static_cast<uint8_t>(slot);
          ++out.pnMtxCount;
        } else {
          // More distinct matrices than the device can index (GX's palette is
          // 10 deep, hardware commonly stops at 9). The index written here is
          // a placeholder: draw_palette_split re-emits this draw as per-palette
          // groups and rewrites it.
          out.pnMtxOverflow = true;
          pnMtxRemap[slot] = 0;
        }
      }
      const uint32_t indices = static_cast<uint32_t>(pnMtxRemap[slot]);
      std::memcpy(dst + indicesOffset, &indices, 4);
    }
  }

  return true;
}

} // namespace aurora::dx9

#endif // AURORA_ENABLE_D3D9
