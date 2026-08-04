#pragma once

#ifdef AURORA_ENABLE_D3D9

#include "dx9_internal.hpp"

namespace aurora::dx9 {

void texture_cache_initialize() noexcept;
void texture_cache_shutdown() noexcept;
void texture_cache_begin_frame() noexcept;
// Releases D3DPOOL_DEFAULT resources ahead of a device Reset - Reset fails
// while any of them is alive. Covers the EFB copy targets and the offscreen
// targets; static textures are D3DPOOL_MANAGED and survive.
void texture_cache_release_default_pool() noexcept;

// Resolves the texture bound to a GX texmap (static / palette / EFB-copy
// placeholder), creating + caching the D3D9 texture on demand. May return
// nullptr (draw binds no texture; TEV mapper falls back accordingly).
IDirect3DBaseTexture9* resolve_texmap(GXTexMapID id) noexcept;

// Applies wrap/filter/LOD sampler state for the texmap onto a D3D stage.
void apply_sampler(uint32_t stage, GXTexMapID id) noexcept;

// EFB copy placeholder registration (depth/unsupported copy formats).
void texture_register_copy_placeholder(const void* dest) noexcept;

// Render-target texture registered for an EFB copy destination, created or
// resized on demand (D3DPOOL_DEFAULT). Returns nullptr on failure.
IDirect3DTexture9* texture_get_copy_target(const void* dest, uint32_t width, uint32_t height) noexcept;

// Copy texture previously registered for this guest pointer, if any.
IDirect3DBaseTexture9* texture_find_copy(const void* data) noexcept;

// Offscreen render target (color RT texture + surface + depth), cached by
// size (D3DPOOL_DEFAULT). Returns nullptr on failure.
struct OffscreenTarget {
  IDirect3DTexture9* color = nullptr;
  IDirect3DSurface9* colorSurface = nullptr;
  IDirect3DSurface9* depth = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
};
OffscreenTarget* texture_get_offscreen(uint32_t width, uint32_t height) noexcept;

} // namespace aurora::dx9

#endif // AURORA_ENABLE_D3D9
