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
  // Backbuffer size: the window's real pixel size.
  uint32_t width = 0;
  uint32_t height = 0;
  // Render area inside the backbuffer. The shared gx layer scales the GC
  // logical framebuffer into this, and the game lays its HUD out against the
  // same size (AuroraGetRenderSize -> AuroraWindowSize::fb_*), which the
  // viewport policy may letterbox to the game's aspect. Rendering into the
  // full backbuffer instead would stretch the image and desync HUD placement.
  uint32_t renderWidth = 0;
  uint32_t renderHeight = 0;
  int32_t renderOffsetX = 0;
  int32_t renderOffsetY = 0;
  bool inScene = false;
  bool deviceLost = false;
  bool perStageConstants = false; // D3DPMISCCAPS_PERSTAGECONSTANT
  bool tssTemp = false;           // D3DPMISCCAPS_TSSARGTEMP (TEMP register)
  // Offscreen pass state (GXCreateFrameBuffer): draws target a render-target
  // texture until end_offscreen restores the backbuffer.
  bool inOffscreen = false;
  uint32_t offscreenWidth = 0;
  uint32_t offscreenHeight = 0;
};
extern Device g_dx9;

// Model-view (WORLD*VIEW) inverse for the current draw, used to compensate
// D3D's camera-space texgen inputs back to GX's model-space inputs
// (docs #7). Invalid for per-vertex matrix-palette / skinned draws, where a
// single inverse doesn't exist.
struct WorldViewInv {
  D3DMATRIX full{};     // affine inverse incl. translation (POS-source texgen)
  D3DMATRIX rotation{}; // same with translation zeroed (NRM-source texgen)
  bool valid = false;
};
extern WorldViewInv g_worldViewInv;

// Camera (GX_AURORA_SET_VIEW_MTX). When valid, apply_transforms uploads
// WORLD = pnMtx * viewInv (true model->world) and VIEW = view, so RTX Remix
// sees a real camera, world-space geometry, and object->world blend bones
// (its assumed convention). When absent, WORLD carries the GX combined
// model->view and VIEW stays identity - rasterization-identical, but Remix
// cannot reconstruct a camera (skinned draws break there; docs #6/#13).
struct CameraView {
  D3DMATRIX view{};
  D3DMATRIX viewInv{};
  bool valid = false;
};
extern CameraView g_camera;

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
  // Material intent for Remix, carried in the otherwise unused D3DMATERIAL9.
  // Mirrored because SetMaterial dirties the fixed-function vertex constants
  // on every call, and these change per material rather than per draw.
  D3DMATERIAL9 remixMaterial{};
  bool remixMaterialValid = false;

  void invalidate() noexcept {
    rsValid.fill(false);
    for (auto& v : tssValid) {
      v.fill(false);
    }
    for (auto& v : sampValid) {
      v.fill(false);
    }
    texturesValid.fill(false);
    remixMaterialValid = false;
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

// Material intent for Remix, in a side channel that exists because the stage
// chain cannot carry these facts. D3DRS_LIGHTING is off in this backend, so
// nothing consumes a D3D9 material and the whole struct was free; the fork
// copies it into LegacyMaterialData. Field meanings, all in
// docs/dx9/remix-material-interface.md §2:
//
//   Emissive.rgb  the colour the surface presents      §9
//   Emissive.a    self-illumination evidence score - REPORTED, NOT USED §9
//   Diffuse.rgb   the ramp endpoint TFACTOR does not carry   §10
//   Diffuse.a     1 when this material is a two-colour ramp
//   Ambient.r     1 when TFACTOR carries the texture-white endpoint
//   Ambient.g     1-based index of the HD texture replacement for this draw's albedo
//                 texture, 0 for none. Carried as a float because the channel is one;
//                 indices stay exactly representable far past any realistic pack size
//                 (a float holds every integer to 2^24). The fork joins it against the
//                 materials the game created through the Remix API and substitutes the
//                 loaded file. docs/dx9/texture-replacements.md
//   Ambient.b     the D3D9 stage Ambient.g refers to. Only meaningful when Ambient.g is
//                 non-zero. The ray-traced path does not need it - Remix takes its albedo
//                 from the lowest sampling stage, which is the one we measured - but the
//                 rasterized path substitutes per texture bind, and a multi-texture draw
//                 can rebind a *different* stage while this material is current. Without
//                 the stage that bind would take the albedo's replacement
//   Specular.r    1 when the vertex colour stream is authored material
//                 colour rather than baked lighting                §7c
//   Specular.g    1 when aurora evaluated a presentable colour here. 0 for the
//                 HUD and for draws with nothing to take a colour from    §9
//   Specular.b    1 when the material has a colour of its own - authored in
//                 TEV constants, not mixed from the vertex stream and not a
//                 bare texture pass-through                              §9
//   Specular.a    1 when the material is SELF-LIT: no TEV colour stage reads
//                 the rasterized channel, so its colour is fixed whatever
//                 the lights do. This is the basis of the emissive rule  §9
inline void set_remix_material(const D3DCOLORVALUE& emissive, const D3DCOLORVALUE& ramp,
                               float tFactorIsHigh, float vertexColorIsMaterial,
                               float evaluated, float colorAuthored, float selfLit,
                               uint32_t texRepIndex, uint32_t texRepStage) noexcept {
  D3DMATERIAL9 mat{};
  mat.Emissive = emissive;
  mat.Diffuse = ramp;
  mat.Ambient = D3DCOLORVALUE{tFactorIsHigh, static_cast<float>(texRepIndex),
                              static_cast<float>(texRepStage), 0.f};
  mat.Specular = D3DCOLORVALUE{vertexColorIsMaterial, evaluated, colorAuthored, selfLit};
  if (g_cache.remixMaterialValid &&
      std::memcmp(&g_cache.remixMaterial, &mat, sizeof(mat)) == 0) {
    return;
  }
  g_dx9.dev->SetMaterial(&mat);
  g_cache.remixMaterial = mat;
  g_cache.remixMaterialValid = true;
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

// Row-vector composition: result = a * b (apply a, then b).
inline D3DMATRIX mtx_multiply(const D3DMATRIX& a, const D3DMATRIX& b) noexcept {
  D3DMATRIX result{};
  for (int row = 0; row < 4; ++row) {
    for (int col = 0; col < 4; ++col) {
      float sum = 0.f;
      for (int k = 0; k < 4; ++k) {
        sum += a.m[row][k] * b.m[k][col];
      }
      result.m[row][col] = sum;
    }
  }
  return result;
}

// Inverts an affine row-vector transform (rows 0-2 = basis, row 3 =
// translation, last column 0,0,0,1). General 3x3 inverse handles scale.
inline bool mtx_affine_inverse(const D3DMATRIX& m, D3DMATRIX& out) noexcept {
  const float a = m.m[0][0], b = m.m[0][1], c = m.m[0][2];
  const float d = m.m[1][0], e = m.m[1][1], f = m.m[1][2];
  const float g = m.m[2][0], h = m.m[2][1], i = m.m[2][2];
  const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
  if (det > -1e-12f && det < 1e-12f) {
    return false;
  }
  const float inv = 1.0f / det;
  out = D3DMATRIX{{{
      (e * i - f * h) * inv, (c * h - b * i) * inv, (b * f - c * e) * inv, 0.f, //
      (f * g - d * i) * inv, (a * i - c * g) * inv, (c * d - a * f) * inv, 0.f, //
      (d * h - e * g) * inv, (b * g - a * h) * inv, (a * e - b * d) * inv, 0.f, //
      0.f, 0.f, 0.f, 1.f,                                                      //
  }}};
  // Translation: t' = -t * R^-1.
  const float tx = m.m[3][0], ty = m.m[3][1], tz = m.m[3][2];
  out.m[3][0] = -(tx * out.m[0][0] + ty * out.m[1][0] + tz * out.m[2][0]);
  out.m[3][1] = -(tx * out.m[0][1] + ty * out.m[1][1] + tz * out.m[2][1]);
  out.m[3][2] = -(tx * out.m[0][2] + ty * out.m[1][2] + tz * out.m[2][2]);
  return true;
}

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
// Same de-duplication, logged at info level (diagnostics, not problems).
void info_once(uint64_t key, const char* what) noexcept;

// Material translation report: what the GX material was, what we handed D3D9,
// and every decision taken in between. Always on and bounded, because the
// project's rule is that a log answers the question rather than the owner.
// Format, field meanings and worked examples: docs/dx9/material-report.md.
inline constexpr size_t kMatrepMaxMaterials = 512;
// True the first time this material key is seen, false afterwards and once the
// cap is reached (which logs matrep.trunc exactly once). Callers log the lines
// themselves through their own Module, so GX enums format via
// lib/gx/gx_fmt.hpp rather than through a second set of name tables.
bool matrep_should_emit(uint64_t key) noexcept;

// Per-frame D3D9 draw-call count, reported periodically.
//
// A draw costs far more under Remix than it does in raster. A draw too small
// for its own BLAS is merged into a shared one, but it still contributes its
// own VkAccelerationStructureGeometryKHR and its own surface, and that BLAS is
// rebuilt whenever the geometry moves — read in the fork at
// rtx_accel_manager.cpp, `buildInfo.geometryCount = bucket->geometries.size()`
// and the bucket's `originalInstances`. So a thousand single-quad draws is
// expensive in a way a thousand quads in one draw is not. That makes "how many
// draws did this frame cost" the number that decides whether a dense particle
// effect is affordable, and the project's rule is that such a number is logged
// rather than guessed at.
//
// Bounded: one line every kDrawStatsPeriod frames, carrying the period's mean
// and its worst single frame (the mean alone hides a spike that only happens
// while it is raining).
inline constexpr uint32_t kDrawStatsPeriod = 600;
struct DrawStats {
  uint32_t frameDraws = 0;   // draws issued so far this frame
  uint32_t periodFrames = 0; // frames counted since the last report
  uint64_t periodDraws = 0;  // draws summed over those frames
  uint32_t periodPeak = 0;   // worst single frame in the period
};
extern DrawStats g_drawStats;
// Rolls this frame's count into the period totals and logs when the period
// closes. Called once per frame from end_frame().
void draw_stats_end_frame() noexcept;

} // namespace aurora::dx9

#endif // AURORA_ENABLE_D3D9
