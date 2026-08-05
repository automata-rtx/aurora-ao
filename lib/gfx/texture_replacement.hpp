#pragma once

#include "texture.hpp"
#include <optional>

namespace aurora::gfx::texture_replacement {
void initialize() noexcept;
void shutdown() noexcept;
std::optional<TextureHandle> find_replacement(const GXTexObj_& obj) noexcept;
std::optional<TextureHandle> find_replacement(const GXTexObj_& obj, const GXTlutObj_& tlut) noexcept;
// The 1-based index of the replacement selected for this texture, or 0 for none. Resolves the
// same key the find_replacement overloads do, but stops at the registry: it never decodes a
// file and never touches a GPU, so the D3D9 backend can call it. See aurora::texture::
// ReplacementDescriptor and docs/dx9/texture-replacements.md.
uint32_t find_replacement_index(const GXTexObj_& obj) noexcept;
uint32_t find_replacement_index(const GXTexObj_& obj, const GXTlutObj_& tlut) noexcept;
bool has_replacement(const GXTexObj_& obj) noexcept;
bool has_replacement(const GXTexObj_& obj, const GXTlutObj_& tlut) noexcept;
std::string build_texture_replacement_name(const GXTexObj_& obj) noexcept;
} // namespace aurora::gfx::texture_replacement
