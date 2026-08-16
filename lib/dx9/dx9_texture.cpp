#include "dx9_texture.hpp"

#ifdef AURORA_ENABLE_D3D9

#include "../gfx/texture_convert.hpp"
#include "../gfx/texture_replacement.hpp"

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <absl/hash/hash.h>

#include <algorithm>
#include <cstring>
#include <string_view>
#include <vector>

namespace aurora::dx9 {
static Module Log("aurora::dx9::tex");

namespace {

// Frame counter driven by texture_cache_begin_frame(); used to age out cache
// entries that stopped being referenced.
uint32_t s_frameIndex = 0;
constexpr uint32_t kSweepInterval = 32;
// Static textures: how long an unused content entry stays alive. The window
// only matters for content that genuinely changes (palette animations etc.);
// stable content is re-referenced every frame and never ages out.
constexpr uint32_t kKeepStaticFrames = 300;
// EFB copy render targets are tiny in count; keep them longer so effects that
// run intermittently (bloom variants, senses, warps) don't thrash targets.
constexpr uint32_t kKeepCopyFrames = 600;

// --------------------------------------------------------------------------
// Static textures: content-addressed store, texObjId as an alias layer.
//
// Load-bearing for Remix, which holds references to D3D9 texture *objects*
// across frames while the game recreates GXTexObj wrappers per draw per frame
// (dDlst 2D lists). Keying by texObjId meant a create+destroy cycle per draw:
// Remix's texture list churned ("textures spamming in and out") and VRAM grew
// without bound. Key = dims/format/mips + hash of the source bytes (+ TLUT for
// palette formats); evicting an id never destroys the texture.
// Full rationale: docs/dx9/gx-to-d3d9-mapping.md #5.
// --------------------------------------------------------------------------

struct ContentKey {
  uint64_t contentHash = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format = 0;
  uint32_t mips = 0;

  bool operator==(const ContentKey& rhs) const {
    return contentHash == rhs.contentHash && width == rhs.width && height == rhs.height && format == rhs.format &&
           mips == rhs.mips;
  }
  template <typename H>
  friend H AbslHashValue(H h, const ContentKey& key) {
    return H::combine(std::move(h), key.contentHash, key.width, key.height, key.format, key.mips);
  }
};

struct ContentEntry {
  IDirect3DTexture9* tex = nullptr;
  uint32_t lastUsedFrame = 0;
  // 1-based HD-replacement index for this exact content, resolved once when the entry is
  // built. Lives here rather than on IdEntry because the id alias is churned per draw and
  // erased on GXTexObj eviction while this texture survives - and because the replacement is
  // a pure function of the source bytes, which is exactly what ContentKey identifies.
  uint32_t remixIndex = 0;

  void release() noexcept {
    if (tex != nullptr) {
      tex->Release();
      tex = nullptr;
    }
  }
};

// Owns the D3D9 textures.
absl::flat_hash_map<ContentKey, ContentEntry> s_byContent;

// Alias layer: texObjId -> content key + the versions it was resolved with.
struct IdEntry {
  ContentKey key;
  uint32_t texDataVersion = 0;
  uint32_t tlutObjId = 0;
  uint32_t tlutDataVersion = 0;
  uint32_t lastUsedFrame = 0;
};
absl::flat_hash_map<uint32_t, IdEntry> s_byObjId;

uint64_t hash_bytes(const void* data, size_t size) noexcept {
  return absl::Hash<std::string_view>{}(std::string_view(static_cast<const char*>(data), size));
}

// boost::hash_combine-style mixer; inputs are already absl hashes.
constexpr uint64_t combine_hash(uint64_t seed, uint64_t value) noexcept {
  return seed ^ (value + 0x9E3779B97F4A7C15ull + (seed << 6) + (seed >> 2));
}

// Byte size of the source mip chain, mirroring the GC tile layout
// (GXGetTexBufferSize) plus the direct-upload PC formats.
size_t source_data_size(uint32_t format, uint32_t width, uint32_t height, uint32_t mips) noexcept {
  uint32_t shiftX = 0;
  uint32_t shiftY = 0;
  uint32_t tileBytes = 32;
  uint32_t pcBytesPerTexel = 0;
  switch (format) {
  case GX_TF_I4:
  case GX_TF_C4:
  case GX_TF_CMPR:
    shiftX = 3;
    shiftY = 3;
    break;
  case GX_TF_I8:
  case GX_TF_IA4:
  case GX_TF_C8:
    shiftX = 3;
    shiftY = 2;
    break;
  case GX_TF_IA8:
  case GX_TF_RGB565:
  case GX_TF_RGB5A3:
  case GX_TF_C14X2:
    shiftX = 2;
    shiftY = 2;
    break;
  case GX_TF_RGBA8:
    shiftX = 2;
    shiftY = 2;
    tileBytes = 64;
    break;
  case GX_TF_R8_PC:
    pcBytesPerTexel = 1;
    break;
  case GX_TF_RG8_PC:
    pcBytesPerTexel = 2;
    break;
  case GX_TF_RGBA8_PC:
    pcBytesPerTexel = 4;
    break;
  case GX_TF_BC1_PC: {
    size_t total = 0;
    for (uint32_t mip = 0; mip < mips; ++mip) {
      total += static_cast<size_t>((width + 3) / 4) * ((height + 3) / 4) * 8;
      width = std::max(width / 2, 1u);
      height = std::max(height / 2, 1u);
    }
    return total;
  }
  default:
    // Unknown format: fall back to a conservative 32bpp estimate.
    pcBytesPerTexel = 4;
    break;
  }

  size_t total = 0;
  for (uint32_t mip = 0; mip < mips; ++mip) {
    if (pcBytesPerTexel != 0) {
      total += static_cast<size_t>(width) * height * pcBytesPerTexel;
    } else {
      const uint32_t tileX = (width + (1u << shiftX) - 1) >> shiftX;
      const uint32_t tileY = (height + (1u << shiftY) - 1) >> shiftY;
      total += static_cast<size_t>(tileX) * tileY * tileBytes;
    }
    width = std::max(width / 2, 1u);
    height = std::max(height / 2, 1u);
  }
  return total;
}

ContentKey make_content_key(const GXTexObj_& obj, const GXTlutObj_* tlut) noexcept {
  const uint32_t mips = obj.mip_count();
  const size_t srcSize = source_data_size(obj.format(), obj.width(), obj.height(), mips);
  uint64_t hash = hash_bytes(obj.data, srcSize);
  if (tlut != nullptr) {
    hash = combine_hash(hash, hash_bytes(tlut->data, static_cast<size_t>(tlut->numEntries) * 2));
    hash = combine_hash(hash, static_cast<uint64_t>(tlut->format));
  }
  return ContentKey{
      .contentHash = hash,
      .width = obj.width(),
      .height = obj.height(),
      .format = obj.format(),
      .mips = mips,
  };
}

// --------------------------------------------------------------------------
// EFB copies.
// --------------------------------------------------------------------------

// EFB-copy destinations stubbed with the shared neutral placeholder
// (depth/unsupported formats; see docs/dx9/gx-to-d3d9-mapping.md #10).
absl::flat_hash_map<const void*, bool> s_copyDests;
IDirect3DTexture9* s_copyPlaceholder = nullptr;

// Real EFB copy targets, keyed by guest destination pointer AND copy size.
// The same destination is commonly copied to at several sizes per frame
// (the bloom filter downsamples into the same buffer at 1/4 then 1/8), so
// each size keeps its own persistent render target - recreating one target
// per size change would hand RTX Remix a stream of brand-new render targets
// (each with a fresh internal hash) every frame.
struct CopyTarget {
  IDirect3DTexture9* tex = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t lastUsedFrame = 0;

  void release() noexcept {
    if (tex != nullptr) {
      tex->Release();
      tex = nullptr;
    }
  }
};

struct CopyDest {
  std::vector<CopyTarget> sizes;
  // Size most recently copied into; sampling the destination reads this one.
  uint32_t activeWidth = 0;
  uint32_t activeHeight = 0;
};
absl::flat_hash_map<const void*, CopyDest> s_copyTargets;

// Offscreen render targets cached by size (like the wgpu offscreen cache).
absl::flat_hash_map<uint64_t, OffscreenTarget> s_offscreenTargets;

// Render-target allocation failures, reported once per distinct size.
//
// Both allocators below are reached from the FIFO drain: the copy path runs
// every frame (the bloom chain copies into the same destination at two sizes
// every frame - see CopyDest above and docs/dx9/progress.md §3.5), and the
// offscreen path runs every frame in a scene that uses an offscreen pass -
// inferred from its call site, not measured. D3DPOOL_DEFAULT render targets are
// exactly what fails under VRAM pressure, the condition in which an uncapped
// log is least affordable. Only the *logging* is memoized: the allocation is
// still retried every call, so a target that succeeds once the driver frees
// memory still appears. 2026-08-16.
//
// Deliberately NOT warn_once's s_warned set. That one prints
// "dx9: unsupported: ..." and its keys are the triage list of unsupported GX
// features (docs/dx9/unsupported-effects.md, where "the number of distinct keys
// per reason is the useful signal"); a driver allocation failure is not one.
// Keys are (tag << 48) | (width << 16) | height - disjoint fields, so the tag
// cannot alias into the dimensions.
//
// Re-armed by texture_cache_release_default_pool(), which is what runs ahead of
// a device Reset and at shutdown: the targets these keys describe are destroyed
// there, so the same size failing again after a Reset is a new failure and is
// reported again. A latch that never re-arms is a log that goes silent for the
// rest of the run.
constexpr uint64_t kCopyTargetWarnTag = 0xA1ull << 48;
constexpr uint64_t kOffscreenTargetWarnTag = 0xA2ull << 48;
constexpr uint32_t kTargetWarnMaxKeys = 16;
absl::flat_hash_set<uint64_t> s_targetWarned;
uint32_t s_targetWarnSuppressed = 0;

bool target_warn_should_emit(uint64_t key) noexcept {
  if (s_targetWarned.contains(key)) {
    return false;
  }
  if (s_targetWarned.size() >= kTargetWarnMaxKeys) {
    if (s_targetWarnSuppressed++ == 0) {
      Log.warn("dx9: render-target failures cap={} distinct sizes reached - further sizes not reported",
               kTargetWarnMaxKeys);
    }
    return false;
  }
  s_targetWarned.insert(key);
  return true;
}

void release_offscreen_targets() noexcept {
  for (auto& [_, t] : s_offscreenTargets) {
    if (t.depth != nullptr) {
      t.depth->Release();
    }
    if (t.colorSurface != nullptr) {
      t.colorSurface->Release();
    }
    if (t.color != nullptr) {
      t.color->Release();
    }
  }
  s_offscreenTargets.clear();
}

IDirect3DTexture9* create_placeholder() noexcept {
  IDirect3DTexture9* tex = nullptr;
  if (FAILED(g_dx9.dev->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr))) {
    return nullptr;
  }
  D3DLOCKED_RECT lr{};
  if (SUCCEEDED(tex->LockRect(0, &lr, nullptr, 0))) {
    // White with zero alpha: neutral for modulate-style consumers and
    // invisible under alpha blending (an opaque-black placeholder darkened
    // everything that projected it, e.g. ground shadows).
    *static_cast<uint32_t*>(lr.pBits) = 0x00FFFFFFu;
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

void sweep_caches() noexcept {
  for (auto it = s_byContent.begin(); it != s_byContent.end();) {
    if (s_frameIndex - it->second.lastUsedFrame > kKeepStaticFrames) {
      it->second.release();
      s_byContent.erase(it++);
    } else {
      ++it;
    }
  }
  for (auto it = s_byObjId.begin(); it != s_byObjId.end();) {
    if (s_frameIndex - it->second.lastUsedFrame > kKeepStaticFrames) {
      s_byObjId.erase(it++);
    } else {
      ++it;
    }
  }
  for (auto it = s_copyTargets.begin(); it != s_copyTargets.end();) {
    auto& sizes = it->second.sizes;
    std::erase_if(sizes, [](CopyTarget& t) {
      if (s_frameIndex - t.lastUsedFrame > kKeepCopyFrames) {
        t.release();
        return true;
      }
      return false;
    });
    if (sizes.empty()) {
      s_copyTargets.erase(it++);
    } else {
      ++it;
    }
  }
}

} // namespace

void texture_cache_initialize() noexcept { s_copyPlaceholder = create_placeholder(); }

void texture_cache_shutdown() noexcept {
  for (auto& [_, entry] : s_byContent) {
    entry.release();
  }
  s_byContent.clear();
  s_byObjId.clear();
  s_copyDests.clear();
  texture_cache_release_default_pool();
  if (s_copyPlaceholder != nullptr) {
    s_copyPlaceholder->Release();
    s_copyPlaceholder = nullptr;
  }
}

void texture_cache_begin_frame() noexcept {
  ++s_frameIndex;
  if (s_frameIndex % kSweepInterval == 0) {
    sweep_caches();
  }
}

void texture_cache_release_default_pool() noexcept {
  // Copy targets and offscreen targets are D3DPOOL_DEFAULT; they must be
  // released ahead of a device Reset and are recreated lazily afterwards.
  for (auto& [_, dest] : s_copyTargets) {
    for (auto& target : dest.sizes) {
      target.release();
    }
  }
  s_copyTargets.clear();
  release_offscreen_targets();
  // Re-arm the render-target failure log along with the targets it describes. Every
  // key in it names a size that no longer has an allocation, so the next failure at
  // that size is a fresh one rather than a repeat.
  s_targetWarned.clear();
  s_targetWarnSuppressed = 0;
}

void texture_register_copy_placeholder(const void* dest) noexcept { s_copyDests[dest] = true; }

IDirect3DTexture9* texture_get_copy_target(const void* dest, uint32_t width, uint32_t height) noexcept {
  auto& entry = s_copyTargets[dest];
  for (auto& target : entry.sizes) {
    if (target.width == width && target.height == height) {
      target.lastUsedFrame = s_frameIndex;
      entry.activeWidth = width;
      entry.activeHeight = height;
      return target.tex;
    }
  }
  IDirect3DTexture9* tex = nullptr;
  const HRESULT hr = g_dx9.dev->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                              D3DPOOL_DEFAULT, &tex, nullptr);
  if (FAILED(hr)) {
    if (target_warn_should_emit(kCopyTargetWarnTag | static_cast<uint64_t>(width) << 16 | height)) {
      Log.warn("dx9: copy target {}x{} creation failed ({:#x}) - reported once per size, still retried every frame",
               width, height, static_cast<uint32_t>(hr));
    }
    if (entry.sizes.empty()) {
      s_copyTargets.erase(dest);
    }
    return nullptr;
  }
  entry.sizes.push_back(CopyTarget{tex, width, height, s_frameIndex});
  entry.activeWidth = width;
  entry.activeHeight = height;
  return tex;
}

IDirect3DBaseTexture9* texture_find_copy(const void* data) noexcept {
  if (const auto it = s_copyTargets.find(data); it != s_copyTargets.end()) {
    for (auto& target : it->second.sizes) {
      if (target.width == it->second.activeWidth && target.height == it->second.activeHeight) {
        target.lastUsedFrame = s_frameIndex;
        return target.tex;
      }
    }
  }
  return nullptr;
}

OffscreenTarget* texture_get_offscreen(uint32_t width, uint32_t height) noexcept {
  const uint64_t key = static_cast<uint64_t>(width) << 32 | height;
  auto& target = s_offscreenTargets[key];
  if (target.color != nullptr) {
    return &target;
  }
  do {
    if (FAILED(g_dx9.dev->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                        &target.color, nullptr))) {
      break;
    }
    if (FAILED(target.color->GetSurfaceLevel(0, &target.colorSurface))) {
      break;
    }
    if (FAILED(g_dx9.dev->CreateDepthStencilSurface(width, height, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE,
                                                    &target.depth, nullptr))) {
      break;
    }
    target.width = width;
    target.height = height;
    return &target;
  } while (false);
  if (target_warn_should_emit(kOffscreenTargetWarnTag | static_cast<uint64_t>(width) << 16 | height)) {
    Log.warn("dx9: offscreen target {}x{} creation failed - reported once per size, still retried every frame", width,
             height);
  }
  // The erase below is required, not incidental: colorSurface/color are
  // Release()d without being nulled, so leaving the entry would hand out a
  // dangling target on the next call. It memoizes nothing.
  if (target.colorSurface != nullptr) {
    target.colorSurface->Release();
  }
  if (target.color != nullptr) {
    target.color->Release();
  }
  s_offscreenTargets.erase(key);
  return nullptr;
}

IDirect3DBaseTexture9* resolve_texmap(GXTexMapID id, uint32_t* outRemixIndex) noexcept {
  const auto report = [&](uint32_t index) noexcept {
    if (outRemixIndex != nullptr) {
      *outRemixIndex = index;
    }
  };
  report(0);
  if (id >= gx::MaxTextures) {
    return nullptr;
  }
  const GXTexObj_& obj = g_gxState.loadedTextures[static_cast<size_t>(id)];

  // EFB-copy source? Real color copies first; depth/unsupported copies get
  // the neutral placeholder. Palette-format copies (e.g. shadow silhouettes)
  // are sampled as plain color for now — approximate but visible.
  // Render targets have no source bytes to key a replacement on, so they keep index 0.
  if (IDirect3DBaseTexture9* copy = texture_find_copy(obj.data)) {
    return copy;
  }
  if (s_copyDests.contains(obj.data)) {
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

  // Fast path: id alias with matching data/TLUT versions skips hashing.
  if (obj.texObjId != 0) {
    if (const auto it = s_byObjId.find(obj.texObjId); it != s_byObjId.end()) {
      auto& entry = it->second;
      const bool tlutOk = !isPalette || (entry.tlutObjId == tlut->tlutObjId &&
                                         entry.tlutDataVersion == tlut->tlutDataVersion);
      if (entry.texDataVersion == obj.texDataVersion && tlutOk) {
        if (const auto cit = s_byContent.find(entry.key); cit != s_byContent.end()) {
          entry.lastUsedFrame = s_frameIndex;
          cit->second.lastUsedFrame = s_frameIndex;
          report(cit->second.remixIndex);
          return cit->second.tex;
        }
      }
      // Stale versions or content entry aged out: drop the alias, re-resolve.
      s_byObjId.erase(it);
    }
  }

  // Content lookup: identical source bytes resurrect the same D3D9 texture
  // no matter how many GXTexObj wrappers the game churns through.
  const ContentKey key = make_content_key(obj, tlut);
  auto cit = s_byContent.find(key);
  if (cit == s_byContent.end()) {
    IDirect3DTexture9* tex = isPalette ? build_palette(obj, *tlut) : build_static(obj);
    if (tex == nullptr) {
      return nullptr;
    }
    // Resolved once per distinct content, not per draw: the registry lookup hashes the source
    // bytes again, which is the same cost make_content_key just paid, and doing it per draw
    // would double it for every textured draw in the frame.
    const uint32_t remixIndex = isPalette ? gfx::texture_replacement::find_replacement_index(obj, *tlut)
                                          : gfx::texture_replacement::find_replacement_index(obj);
    cit = s_byContent.emplace(key, ContentEntry{tex, s_frameIndex, remixIndex}).first;
  } else {
    cit->second.lastUsedFrame = s_frameIndex;
  }
  report(cit->second.remixIndex);

  if (obj.texObjId != 0 && !obj.no_cache() && (!isPalette || !tlut->no_cache())) {
    s_byObjId.insert_or_assign(obj.texObjId, IdEntry{
                                                 .key = key,
                                                 .texDataVersion = obj.texDataVersion,
                                                 .tlutObjId = isPalette ? tlut->tlutObjId : 0,
                                                 .tlutDataVersion = isPalette ? tlut->tlutDataVersion : 0,
                                                 .lastUsedFrame = s_frameIndex,
                                             });
  }
  return cit->second.tex;
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
  // Only the id alias dies with the GXTexObj; the D3D9 texture stays in the
  // content store so an identical re-init reuses it (stable objects for
  // Remix). Unclaimed content ages out in sweep_caches().
  s_byObjId.erase(texObjId);
}

void on_evict_tlut(uint32_t tlutObjId) noexcept {
  for (auto it = s_byObjId.begin(); it != s_byObjId.end();) {
    if (it->second.tlutObjId == tlutObjId) {
      s_byObjId.erase(it++);
    } else {
      ++it;
    }
  }
}

void on_evict_copy_texture(const void* dest) noexcept {
  s_copyDests.erase(dest);
  if (const auto it = s_copyTargets.find(dest); it != s_copyTargets.end()) {
    for (auto& target : it->second.sizes) {
      target.release();
    }
    s_copyTargets.erase(it);
  }
}

} // namespace aurora::dx9

#endif // AURORA_ENABLE_D3D9
