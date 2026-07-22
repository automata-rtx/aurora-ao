#pragma once

#ifdef AURORA_ENABLE_D3D9

#include "dx9.hpp"
#include "../internal.hpp"
#include "../gx/gx.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d9.h>

#include <array>
#include <cstring>

namespace aurora::dx9 {

// The shared GX state decoded by the FIFO command processor is the single
// source of truth for every draw this backend issues.
using gx::g_gxState;

// ---------------------------------------------------------------------------
// Device
// ---------------------------------------------------------------------------

struct Device {
  IDirect3D9* d3d = nullptr;
  IDirect3DDevice9* dev = nullptr;
  HWND hwnd = nullptr;
  D3DPRESENT_PARAMETERS pp{};
  D3DCAPS9 caps{};
  uint32_t width = 0;
  uint32_t height = 0;
  bool inScene = false;
  bool deviceLost = false;
  bool perStageConstants = false; // D3DPMISCCAPS_PERSTAGECONSTANT
  // v1 policy: draws inside offscreen passes (shadow silhouettes etc.) are
  // discarded; see docs/dx9/gx-to-d3d9-mapping.md #10.
  bool inOffscreen = false;
};
extern Device g_dx9;

// ---------------------------------------------------------------------------
// Redundant-state filtering. D3D9 SetRenderState & co. are cheap but Remix
// benefits from a quiet stream; the mirrors also let the FIFO's fine-grained
// state churn collapse naturally between draws.
// ---------------------------------------------------------------------------

constexpr uint32_t MaxRenderState = 256;  // covers all D3DRS_* values
constexpr uint32_t MaxTssState = 33;      // D3DTSS_CONSTANT == 32
constexpr uint32_t MaxSamplerState = 14;  // D3DSAMP_DMAPOFFSET == 13
constexpr uint32_t MaxStages = 8;
constexpr uint32_t MaxWorldPalette = 256; // fixed-function indexed blending limit

struct StateCache {
  std::array<DWORD, MaxRenderState> rs{};
  std::array<bool, MaxRenderState> rsValid{};
  std::array<std::array<DWORD, MaxTssState>, MaxStages> tss{};
  std::array<std::array<bool, MaxTssState>, MaxStages> tssValid{};
  std::array<std::array<DWORD, MaxSamplerState>, MaxStages> samp{};
  std::array<std::array<bool, MaxSamplerState>, MaxStages> sampValid{};
  std::array<IDirect3DBaseTexture9*, MaxStages> textures{};
  std::array<bool, MaxStages> texturesValid{};
  DWORD fvf = 0;
  bool fvfValid = false;
  D3DMATRIX proj{};
  bool projValid = false;
  D3DMATRIX view{};
  bool viewValid = false;
  // World palette mirror: slot 0 doubles as the plain WORLD matrix.
  std::array<D3DMATRIX, 16> world{};
  std::array<bool, 16> worldValid{};
  std::array<D3DMATRIX, MaxStages> texMtx{};
  std::array<bool, MaxStages> texMtxValid{};

  void invalidate() noexcept {
    rsValid.fill(false);
    for (auto& v : tssValid) {
      v.fill(false);
    }
    for (auto& v : sampValid) {
      v.fill(false);
    }
    texturesValid.fill(false);
    fvfValid = false;
    projValid = viewValid = false;
    worldValid.fill(false);
    texMtxValid.fill(false);
  }
};
extern StateCache g_cache;

inline void set_rs(D3DRENDERSTATETYPE state, DWORD value) noexcept {
  const auto idx = static_cast<uint32_t>(state);
  if (idx < MaxRenderState && g_cache.rsValid[idx] && g_cache.rs[idx] == value) {
    return;
  }
  g_dx9.dev->SetRenderState(state, value);
  if (idx < MaxRenderState) {
    g_cache.rs[idx] = value;
    g_cache.rsValid[idx] = true;
  }
}

inline void set_tss(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) noexcept {
  const auto idx = static_cast<uint32_t>(type);
  if (stage < MaxStages && idx < MaxTssState && g_cache.tssValid[stage][idx] && g_cache.tss[stage][idx] == value) {
    return;
  }
  g_dx9.dev->SetTextureStageState(stage, type, value);
  if (stage < MaxStages && idx < MaxTssState) {
    g_cache.tss[stage][idx] = value;
    g_cache.tssValid[stage][idx] = true;
  }
}

inline void set_samp(DWORD stage, D3DSAMPLERSTATETYPE type, DWORD value) noexcept {
  const auto idx = static_cast<uint32_t>(type);
  if (stage < MaxStages && idx < MaxSamplerState && g_cache.sampValid[stage][idx] &&
      g_cache.samp[stage][idx] == value) {
    return;
  }
  g_dx9.dev->SetSamplerState(stage, type, value);
  if (stage < MaxStages && idx < MaxSamplerState) {
    g_cache.samp[stage][idx] = value;
    g_cache.sampValid[stage][idx] = true;
  }
}

inline void set_texture(DWORD stage, IDirect3DBaseTexture9* tex) noexcept {
  if (stage < MaxStages && g_cache.texturesValid[stage] && g_cache.textures[stage] == tex) {
    return;
  }
  g_dx9.dev->SetTexture(stage, tex);
  if (stage < MaxStages) {
    g_cache.textures[stage] = tex;
    g_cache.texturesValid[stage] = true;
  }
}

inline void set_fvf(DWORD fvf) noexcept {
  if (g_cache.fvfValid && g_cache.fvf == fvf) {
    return;
  }
  g_dx9.dev->SetFVF(fvf);
  g_cache.fvf = fvf;
  g_cache.fvfValid = true;
}

inline bool mtx_equal(const D3DMATRIX& a, const D3DMATRIX& b) noexcept { return std::memcmp(&a, &b, sizeof(a)) == 0; }

// ---------------------------------------------------------------------------
// Matrix conversion. Conventions (see docs/dx9/gx-to-d3d9-mapping.md #3):
//  - aurora Mat3x4 rows m0..m2 are GameCube row-major 3x4 (v' = R*v + t).
//  - aurora Mat4x4 m0..m3 are the columns of the row-vector transform.
//  - D3D uses row vectors: element _m[r][c], v' = v * M.
// ---------------------------------------------------------------------------

inline D3DMATRIX to_d3d(const Mat3x4<float>& m) noexcept {
  return D3DMATRIX{{{
      m.m0[0], m.m1[0], m.m2[0], 0.f, //
      m.m0[1], m.m1[1], m.m2[1], 0.f, //
      m.m0[2], m.m1[2], m.m2[2], 0.f, //
      m.m0[3], m.m1[3], m.m2[3], 1.f, //
  }}};
}

// 12 row-major floats (3x4), e.g. skinBaseMtx / skinning palettes.
inline D3DMATRIX to_d3d_3x4(const float* m) noexcept {
  return D3DMATRIX{{{
      m[0], m[4], m[8], 0.f,  //
      m[1], m[5], m[9], 0.f,  //
      m[2], m[6], m[10], 0.f, //
      m[3], m[7], m[11], 1.f, //
  }}};
}

// Projection: fold GX NDC z in [-1,0] to D3D [0,1] (same as the wgpu
// non-reversed path: z column += w column), then transpose to D3D layout.
inline D3DMATRIX to_d3d_proj(const Mat4x4<float>& m) noexcept {
  const Vec4<float> z = m.m2 + m.m3;
  return D3DMATRIX{{{
      m.m0[0], m.m1[0], z[0], m.m3[0], //
      m.m0[1], m.m1[1], z[1], m.m3[1], //
      m.m0[2], m.m1[2], z[2], m.m3[2], //
      m.m0[3], m.m1[3], z[3], m.m3[3], //
  }}};
}

inline void set_transform(D3DTRANSFORMSTATETYPE which, const D3DMATRIX& m) noexcept {
  g_dx9.dev->SetTransform(which, &m);
}

void set_world_matrix(uint32_t slot, const D3DMATRIX& m) noexcept;
void set_view_matrix(const D3DMATRIX& m) noexcept;
void set_proj_matrix(const D3DMATRIX& m) noexcept;
void set_texture_matrix(uint32_t stage, const D3DMATRIX& m) noexcept;

// ---------------------------------------------------------------------------
// Skinning extension state (raw host pointers; see dx9.hpp set_skinning).
// ---------------------------------------------------------------------------

struct SkinState {
  const float* palette = nullptr; // jointCount * 12 floats, row-major 3x4
  const uint8_t* influences = nullptr;
  uint32_t jointCount = 0;
  uint32_t vtxCount = 0;
  uint32_t influenceCount = 0;
};
extern SkinState g_skin;

// ---------------------------------------------------------------------------
// Big-endian-aware stream readers for vertex decoding.
// ---------------------------------------------------------------------------

template <typename T>
inline T read_val(const uint8_t* p, bool be) noexcept {
  T v;
  std::memcpy(&v, p, sizeof(T));
  if constexpr (sizeof(T) > 1) {
    if (be) {
      v = bswap(v);
    }
  }
  return v;
}

// One-shot warning per distinct reason hash, to keep play sessions readable.
void warn_once(uint64_t key, const char* what) noexcept;

} // namespace aurora::dx9

#endif // AURORA_ENABLE_D3D9
