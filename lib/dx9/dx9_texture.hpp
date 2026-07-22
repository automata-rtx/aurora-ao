#pragma once

#ifdef AURORA_ENABLE_D3D9

#include "dx9_internal.hpp"

namespace aurora::dx9 {

void texture_cache_initialize() noexcept;
void texture_cache_shutdown() noexcept;
void texture_cache_begin_frame() noexcept;
// Releases D3DPOOL_DEFAULT resources ahead of a device Reset. (v1 keeps
// everything in the managed pool; hook kept for the future EFB-copy targets.)
void texture_cache_release_default_pool() noexcept;

// Resolves the texture bound to a GX texmap (static / palette / EFB-copy
// placeholder), creating + caching the D3D9 texture on demand. May return
// nullptr (draw binds no texture; TEV mapper falls back accordingly).
IDirect3DBaseTexture9* resolve_texmap(GXTexMapID id) noexcept;

// Applies wrap/filter/LOD sampler state for the texmap onto a D3D stage.
void apply_sampler(uint32_t stage, GXTexMapID id) noexcept;

// EFB copy placeholder registration (see dx9_backend copy_tex).
void texture_register_copy_placeholder(const void* dest) noexcept;

} // namespace aurora::dx9

#endif // AURORA_ENABLE_D3D9
