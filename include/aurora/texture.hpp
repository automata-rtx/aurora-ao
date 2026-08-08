#ifndef AURORA_TEXTURE_HPP
#define AURORA_TEXTURE_HPP

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace aurora::texture {

/// Wildcard hash values used by the replacement filename convention ("$" fields).
inline constexpr uint64_t kWildcardTextureHash = 0xFFFFFFFFFFFFFFFFull;
inline constexpr uint64_t kWildcardTlutHash = 0xFFFFFFFFFFFFFFFEull;

struct TextureSourceKey {
  uint64_t textureHash = 0;
  uint64_t tlutHash = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format = 0;
  bool hasTlut = false;

  bool operator==(const TextureSourceKey&) const = default;
};

struct TexturePointerKey {
  const void* data = nullptr;

  bool operator==(const TexturePointerKey&) const = default;
};

using ReplacementKey = std::variant<TexturePointerKey, TextureSourceKey>;

struct RawTextureReplacement {
  std::span<const uint8_t> bytes;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t mipCount = 1;
  uint32_t gxFormat = 0;
  std::string_view label = {};
};

struct ReplacementOptions {
  int32_t priority = 0;
};

struct ReplacementRegistration {
  uint64_t id = 0;
  ReplacementKey key;
};

struct ReplacementGroup {
  std::vector<ReplacementRegistration> registrations;
};

/// One file-backed replacement, as handed to a renderer that loads the file itself rather
/// than going through aurora's decoders.
///
/// `remixIndex` is a 1-based handle aurora assigns at registration and can resolve back from
/// a GX texture per draw. It exists so the D3D9 backend and RTX Remix can agree on *which*
/// replacement a draw wants without either side reproducing the other's hash: the D3D9 stream
/// carries the index, and whoever loaded the file is keyed by the same number.
/// See aurora docs/dx9/texture-replacements.md.
struct ReplacementDescriptor {
  uint64_t id = 0;         ///< registry entry id
  uint32_t remixIndex = 0; ///< 1-based; 0 is never a valid index
  int32_t priority = 0;
  TextureSourceKey key;
  std::string path; ///< absolute where the registry had one; empty for non-file entries
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format = 0;
};

/// Lists the file-backed replacement currently *selected* for each key (priority/sequence
/// resolved), so a caller can hand the files to another renderer. Does not decode anything and
/// does not touch the GPU. Entries with no path (raw/virtual registrations) are skipped, and so
/// are entries that never received an index.
std::vector<ReplacementDescriptor> enumerate_selected_replacements();

/// Parses a replacement filename of the form "tex1_{w}x{h}[_m]_{texhash}[_{tluthash}]_{fmt}[_arb].dds|.png"
/// into the source key it addresses. Hash fields may be "$" (see the wildcard constants above).
/// Returns nullopt if the filename is invalid.
std::optional<TextureSourceKey> parse_replacement_filename(std::string_view filename) noexcept;

/// Callback source for replacement files that do not live on the filesystem.
///
/// read() loads the entire encoded file at `path` into outBytes, returning false if the file is
/// unavailable. It must be thread safe: it is invoked lazily, from arbitrary threads, at any point
/// between registration and unregistration, with internal registry locks held. It must not call
/// back into aurora::texture.
struct VirtualFileSource {
  bool (*read)(void* userData, const char* path, std::vector<uint8_t>& outBytes) = nullptr;
  void* userData = nullptr;
};

/// Registers an encoded (.dds/.png) replacement served through callbacks instead of the filesystem.
/// `path` is a relative '/'-separated virtual path whose final component follows the replacement
/// filename convention and determines the key. Mip sidecars are probed lazily by deriving
/// "{stem}_mipN{ext}" from `path` and calling source.read for each level until it returns false.
///
/// Returns ID 0 (invalid) if the filename does not parse or source.read is null.
ReplacementRegistration register_virtual_replacement(std::string_view path, VirtualFileSource source,
                                                     ReplacementOptions options = {});

ReplacementRegistration register_replacement(ReplacementKey key, RawTextureReplacement replacement,
                                              ReplacementOptions options = {});
void unregister_replacement(const ReplacementRegistration& registration);
void unregister_replacements(std::span<const ReplacementRegistration> registrations);
void unregister_replacements(const ReplacementGroup& group);
void unregister_replacements(const ReplacementKey& key);
void clear_replacements();

ReplacementGroup load_replacement_directory(const std::filesystem::path& root, ReplacementOptions options = {});
void reload_replacement_directory(const std::filesystem::path& root, ReplacementGroup& group,
                                  ReplacementOptions options = {});

bool has_replacement(const GXTexObj* obj, const GXTlutObj* tlut = nullptr);

} // namespace aurora::texture

#endif
