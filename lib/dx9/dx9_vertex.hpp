#pragma once

#ifdef AURORA_ENABLE_D3D9

#include "dx9_internal.hpp"

namespace aurora::dx9 {

// Result of CPU-decoding one GX draw's vertex stream into an interleaved
// fixed-function vertex layout (see docs/dx9/gx-to-d3d9-mapping.md #4).
struct DecodedDraw {
  const uint8_t* verts = nullptr;
  uint32_t stride = 0;
  uint32_t vtxCount = 0;
  DWORD fvf = 0;
  // Fixed-function vertex blending inputs for this draw:
  bool hasPnMtxIdx = false; // per-vertex matrix palette (PNMTXIDX attr)
  bool skinned = false;     // GXSetSkinning influences baked into the vertices
  uint32_t weightCount = 0; // float weights stored per vertex (0 rigid;
                            // always 1-3 when blended, for Remix's sake)
  // Compacted matrix palette for hasPnMtxIdx draws: D3D9 blend index i selects
  // GX position matrix pnMtxSlots[i]. GX has 10 position matrices but
  // fixed-function indexed blending only reaches
  // D3DCAPS9::MaxVertexBlendMatrixIndex (8 on the reference hardware), so the
  // slots a draw actually uses are renumbered into a dense range instead of
  // being passed through - an out-of-range index reads an undefined matrix and
  // scatters those vertices across the world.
  std::array<uint8_t, gx::MaxPnMtx> pnMtxSlots{};
  uint32_t pnMtxCount = 0;
  // GX texcoord attr (VA_TEXn) -> uv slot in the decoded vertex, -1 if absent.
  std::array<int8_t, 8> texSlot{-1, -1, -1, -1, -1, -1, -1, -1};
  uint8_t uvCount = 0;
  bool hasNormal = false;
  bool hasSpecular = false;
};

// Decodes vtxCount vertices from the raw GX stream (vtxSize bytes each,
// big-endian when `bigEndian`) according to g_gxState.vtxDesc / vtxFmts[fmt] /
// arrays. Always emits a diffuse color (vertex color, or the channel-0
// material color / white when the stream has none). Returns false when the
// stream cannot be decoded (unsupported layout logged via warn_once).
bool decode_draw(GXVtxFmt fmt, uint16_t vtxCount, const uint8_t* data, uint32_t vtxSize, bool bigEndian,
                 DecodedDraw& out) noexcept;

} // namespace aurora::dx9

#endif // AURORA_ENABLE_D3D9
