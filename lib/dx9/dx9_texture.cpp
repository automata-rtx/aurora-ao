#include "dx9_texture.hpp"

#ifdef AURORA_ENABLE_D3D9

#include "../gfx/texture_convert.hpp"

#include <absl/container/flat_hash_map.h>

#include <algorithm>
#include <cstring>

namespace aurora::dx9 {
static Module Log("aurora::dx9::tex");

namespace {

struct CachedTexture {
  IDirect3DTexture9* tex = nullptr;
  uint32_t texDataVersion = 0;
  uint32_t tlutObjId = 0;
  uint32_t tlutDataVersion = 0;

  void release() noexcept {
    if (tex != nullptr) {
      tex->Release();
      tex = nullptr;
    }
  }
};

// Static textures keyed by texObjId; textures without an object id (rare)
// fall back to a pointer+dims key in a second map.
absl::flat_hash_map<uint32_t, CachedTexture> s_byObjId;

struct PtrKey {
  const void* data;
  uint32_t width;
  uint32_t height;
  uint32_t format;

  bool operator==(const PtrKey& rhs) const {
    return data == rhs.data && width == rhs.width && height == rhs.height && format == rhs.format;
  }
  template <typename H>
  friend H AbslHashValue(H h, const PtrKey& key) {
    return H::combine(std::move(h), key.data, key.width, key.height, key.format);
  }
};
absl::flat_hash_map<PtrKey, CachedTexture> s_byPointer;

// EFB-copy destinations that have been registered (v1: all share one
// placeholder texture; see docs/dx9/gx-to-d3d9-mapping.md #10).
absl::flat_hash_map<const void*, bool> s_copyDests;
IDirect3DTexture9* s_copyPlaceholder = nullptr;

IDirect3DTexture9* create_placeholder() noexcept {
  IDirect3DTexture9* tex = nullptr;
  if (FAILED(g_dx9.dev->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr))) {
    return nullptr;
  }
  D3DLOCKED_RECT lr{};
  if (SUCCEEDED(tex->LockRect(0, &lr, nullptr, 0))) {
    *static_cast<uint32_t*>(lr.pBits) = 0xFF000000u; // opaque black
    tex->UnlockRect(0);
  }
  return tex;
}

// Uploads a packed RGBA8 mip chain, swizzling to BGRA (D3DFMT_A8R8G8B8).
bool upload_rgba8(IDirect3DTexture9* tex, const uint8_t* src, uint32_t width, uint32_t height,
                  uint32_t mips) noexcept {
  uint32_t w = width;
  uint32_t h = height;
  for (uint32_t level = 0; level < mips; ++level) {
    D3DLOCKED_RECT lr{};
    if (FAILED(tex->LockRect(level, &lr, nullptr, 0))) {
      return false;
    }
    auto* dstRow = static_cast<uint8_t*>(lr.pBits);
    for (uint32_t y = 0; y < h; ++y) {
      const uint8_t* s = src + static_cast<size_t>(y) * w * 4;
      auto* d = reinterpret_cast<uint32_t*>(dstRow);
      for (uint32_t x = 0; x < w; ++x) {
        const uint8_t r = s[x * 4 + 0];
        const uint8_t g = s[x * 4 + 1];
        const uint8_t b = s[x * 4 + 2];
        const uint8_t a = s[x * 4 + 3];
        d[x] = static_cast<uint32_t>(a) << 24 | static_cast<uint32_t>(r) << 16 | static_cast<uint32_t>(g) << 8 | b;
      }
      dstRow += lr.Pitch;
    }
    tex->UnlockRect(level);
    src += static_cast<size_t>(w) * h * 4;
    w = std::max(w / 2, 1u);
    h = std::max(h / 2, 1u);
  }
  return true;
}

IDirect3DTexture9* create_from_rgba8(const uint8_t* data, uint32_t width, uint32_t height, uint32_t mips,
                                     const char* what) noexcept {
  IDirect3DTexture9* tex = nullptr;
  const HRESULT hr =
      g_dx9.dev->CreateTexture(width, height, mips, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr);
  if (FAILED(hr)) {
    Log.warn("dx9: CreateTexture {}x{}x{} failed ({:#x}) for {}", width, height, mips, static_cast<uint32_t>(hr),
             what);
    return nullptr;
  }
  if (!upload_rgba8(tex, data, width, height, mips)) {
    tex->Release();
    return nullptr;
  }
  return tex;
}

// Converts + creates a texture for a GX texobj (non-palette formats).
IDirect3DTexture9* build_static(const GXTexObj_& obj) noexcept {
  const auto fmt = obj.format();
  auto converted =
      gfx::convert_texture(fmt, obj.width(), obj.height(), obj.mip_count(), {static_cast<const uint8_t*>(obj.data), UINT32_MAX});
  if (!converted.data.empty()) {
    // All GC formats (and PC formats without direct-upload support) land here
    // as packed RGBA8 mips.
    return create_from_rgba8(converted.data.data(), obj.width(), obj.height(), obj.mip_count(), "static texture");
  }
  // Direct-upload PC formats: without webgpu the support flags are false for
  // everything except RGBA8_PC, whose source is already packed RGBA8.
  if (fmt == GX_TF_RGBA8_PC) {
    return create_from_rgba8(static_cast<const uint8_t*>(obj.data), obj.width(), obj.height(), obj.mip_count(),
                             "RGBA8_PC texture");
  }
  Log.warn("dx9: unhandled direct-upload format {} for texture {}x{}", fmt, obj.width(), obj.height());
  return nullptr;
}

IDirect3DTexture9* build_palette(const GXTexObj_& obj, const GXTlutObj_& tlut) noexcept {
  auto converted = gfx::convert_texture_palette(
      obj.format(), obj.width(), obj.height(), obj.mip_count(), {static_cast<const uint8_t*>(obj.data), UINT32_MAX},
      tlut.format, tlut.numEntries, {static_cast<const uint8_t*>(tlut.data), static_cast<size_t>(tlut.numEntries) * 2});
  if (converted.data.empty()) {
    return nullptr;
  }
  return create_from_rgba8(converted.data.data(), obj.width(), obj.height(), obj.mip_count(), "palette texture");
}

} // namespace

void texture_cache_initialize() noexcept { s_copyPlaceholder = create_placeholder(); }

void texture_cache_shutdown() noexcept {
  for (auto& [_, entry] : s_byObjId) {
    entry.release();
  }
  s_byObjId.clear();
  for (auto& [_, entry] : s_byPointer) {
    entry.release();
  }
  s_byPointer.clear();
  s_copyDests.clear();
  if (s_copyPlaceholder != nullptr) {
    s_copyPlaceholder->Release();
    s_copyPlaceholder = nullptr;
  }
}

void texture_cache_begin_frame() noexcept {}

void texture_cache_release_default_pool() noexcept {
  // v1: everything lives in D3DPOOL_MANAGED; nothing to do.
}

void texture_register_copy_placeholder(const void* dest) noexcept { s_copyDests[dest] = true; }

IDirect3DBaseTexture9* resolve_texmap(GXTexMapID id) noexcept {
  if (id >= gx::MaxTextures) {
    return nullptr;
  }
  const GXTexObj_& obj = g_gxState.loadedTextures[static_cast<size_t>(id)];

  // EFB-copy source? v1 placeholder.
  if (g_gxState.copyTextures.contains(obj.data) || s_copyDests.contains(obj.data)) {
    return s_copyPlaceholder;
  }
  if (!obj.has_data()) {
    return nullptr;
  }

  const bool isPalette = gx::is_palette_format(obj.format());
  const GXTlutObj_* tlut = nullptr;
  if (isPalette) {
    const auto tlutIdx = static_cast<size_t>(obj.tlut);
    if (tlutIdx >= g_gxState.loadedTluts.size()) {
      return nullptr;
    }
    tlut = &g_gxState.loadedTluts[tlutIdx];
    if (tlut->data == nullptr) {
      return nullptr;
    }
  }

  if (obj.texObjId != 0) {
    if (const auto it = s_byObjId.find(obj.texObjId); it != s_byObjId.end()) {
      auto& entry = it->second;
      const bool tlutOk = !isPalette || (entry.tlutObjId == tlut->tlutObjId &&
                                         entry.tlutDataVersion == tlut->tlutDataVersion);
      if (entry.tex != nullptr && entry.texDataVersion == obj.texDataVersion && tlutOk) {
        return entry.tex;
      }
      entry.release();
      s_byObjId.erase(it);
    }
  } else {
    const PtrKey key{obj.data, obj.width(), obj.height(), obj.format()};
    if (const auto it = s_byPointer.find(key); it != s_byPointer.end()) {
      return it->second.tex;
    }
  }

  IDirect3DTexture9* tex = isPalette ? build_palette(obj, *tlut) : build_static(obj);
  if (tex == nullptr) {
    return nullptr;
  }

  CachedTexture entry{
      .tex = tex,
      .texDataVersion = obj.texDataVersion,
      .tlutObjId = isPalette ? tlut->tlutObjId : 0,
      .tlutDataVersion = isPalette ? tlut->tlutDataVersion : 0,
  };
  if (obj.texObjId != 0 && !obj.no_cache()) {
    s_byObjId.emplace(obj.texObjId, entry);
  } else {
    s_byPointer.emplace(PtrKey{obj.data, obj.width(), obj.height(), obj.format()}, entry);
  }
  return tex;
}

void apply_sampler(uint32_t stage, GXTexMapID id) noexcept {
  const GXTexObj_& obj = g_gxState.loadedTextures[static_cast<size_t>(id)];

  const auto to_address = [](GXTexWrapMode mode) -> DWORD {
    switch (mode) {
    case GX_MIRROR:
      return D3DTADDRESS_MIRROR;
    case GX_REPEAT:
      return D3DTADDRESS_WRAP;
    case GX_CLAMP:
    default:
      return D3DTADDRESS_CLAMP;
    }
  };
  set_samp(stage, D3DSAMP_ADDRESSU, to_address(obj.wrap_s()));
  set_samp(stage, D3DSAMP_ADDRESSV, to_address(obj.wrap_t()));

  DWORD minFilter = D3DTEXF_LINEAR;
  DWORD mipFilter = D3DTEXF_NONE;
  switch (obj.min_filter()) {
  case GX_NEAR:
    minFilter = D3DTEXF_POINT;
    break;
  case GX_LINEAR:
    minFilter = D3DTEXF_LINEAR;
    break;
  case GX_NEAR_MIP_NEAR:
    minFilter = D3DTEXF_POINT;
    mipFilter = D3DTEXF_POINT;
    break;
  case GX_LIN_MIP_NEAR:
    minFilter = D3DTEXF_LINEAR;
    mipFilter = D3DTEXF_POINT;
    break;
  case GX_NEAR_MIP_LIN:
    minFilter = D3DTEXF_POINT;
    mipFilter = D3DTEXF_LINEAR;
    break;
  case GX_LIN_MIP_LIN:
    minFilter = D3DTEXF_LINEAR;
    mipFilter = D3DTEXF_LINEAR;
    break;
  default:
    break;
  }
  const DWORD magFilter = obj.mag_filter() == GX_NEAR ? D3DTEXF_POINT : D3DTEXF_LINEAR;

  DWORD aniso = 1;
  switch (obj.max_aniso()) {
  case GX_ANISO_2:
    aniso = 2;
    break;
  case GX_ANISO_4:
    aniso = 4;
    break;
  default:
    aniso = 1;
    break;
  }
  aniso = std::min<DWORD>(std::max<DWORD>(aniso, 1), g_dx9.caps.MaxAnisotropy != 0 ? g_dx9.caps.MaxAnisotropy : 1);
  if (aniso > 1 && minFilter == D3DTEXF_LINEAR) {
    minFilter = D3DTEXF_ANISOTROPIC;
  }

  set_samp(stage, D3DSAMP_MINFILTER, minFilter);
  set_samp(stage, D3DSAMP_MAGFILTER, magFilter);
  set_samp(stage, D3DSAMP_MIPFILTER, mipFilter);
  set_samp(stage, D3DSAMP_MAXANISOTROPY, aniso);

  const float bias = obj.lod_bias();
  set_samp(stage, D3DSAMP_MIPMAPLODBIAS, std::bit_cast<DWORD>(bias));
  set_samp(stage, D3DSAMP_MAXMIPLEVEL, static_cast<DWORD>(std::max(obj.min_lod(), 0.f)));
}

void on_evict_texture(uint32_t texObjId) noexcept {
  if (const auto it = s_byObjId.find(texObjId); it != s_byObjId.end()) {
    it->second.release();
    s_byObjId.erase(it);
  }
}

void on_evict_tlut(uint32_t tlutObjId) noexcept {
  for (auto it = s_byObjId.begin(); it != s_byObjId.end();) {
    if (it->second.tlutObjId == tlutObjId) {
      it->second.release();
      s_byObjId.erase(it++);
    } else {
      ++it;
    }
  }
}

void on_evict_copy_texture(const void* dest) noexcept { s_copyDests.erase(dest); }

} // namespace aurora::dx9

#endif // AURORA_ENABLE_D3D9
