// Minimal Dawn shim for the syntax harness. Not Dawn: it declares only the
// names aurora's headers mention, so our own code can be type-checked without a
// Dawn checkout. Anything that actually touches the GPU is out of scope here.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace wgpu {

struct StringView {
  const char* data = nullptr;
  size_t length = 0;
  operator std::string_view() const { return {data ? data : "", length}; }
};

enum class BackendType : uint32_t { Undefined, Null, WebGPU, D3D11, D3D12, Metal, Vulkan, OpenGL, OpenGLES };
enum class TextureFormat : uint32_t { ASTC10x10Unorm, ASTC10x10UnormSrgb, ASTC10x5Unorm, ASTC10x5UnormSrgb, ASTC10x6Unorm, ASTC10x6UnormSrgb, ASTC10x8Unorm, ASTC10x8UnormSrgb, ASTC12x10Unorm, ASTC12x10UnormSrgb, ASTC12x12Unorm, ASTC12x12UnormSrgb, ASTC4x4Unorm, ASTC4x4UnormSrgb, ASTC5x4Unorm, ASTC5x4UnormSrgb, ASTC5x5Unorm, ASTC5x5UnormSrgb, ASTC6x5Unorm, ASTC6x5UnormSrgb, ASTC6x6Unorm, ASTC6x6UnormSrgb, ASTC8x5Unorm, ASTC8x5UnormSrgb, ASTC8x6Unorm, ASTC8x6UnormSrgb, ASTC8x8Unorm, ASTC8x8UnormSrgb, BC1RGBAUnorm, BC1RGBAUnormSrgb, BC2RGBAUnorm, BC2RGBAUnormSrgb, BC3RGBAUnorm, BC3RGBAUnormSrgb, BC4RSnorm, BC4RUnorm, BC5RGSnorm, BC5RGUnorm, BC6HRGBFloat, BC6HRGBUfloat, BC7RGBAUnorm, BC7RGBAUnormSrgb, BGRA8Unorm, BGRA8UnormSrgb, Depth32Float, Depth24Plus, Depth24PlusStencil8, ETC2RGB8A1UnormSrgb, ETC2RGB8UnormSrgb, ETC2RGBA8UnormSrgb, R16Sint, R32Float, R8Unorm, RG8Unorm, RGBA8Unorm, RGBA8UnormSrgb, Stencil8, Undefined };
enum class LoadOp : uint32_t { Undefined, Clear, Load };
enum class StoreOp : uint32_t { Undefined, Store, Discard };
enum class TextureViewDimension : uint32_t { Undefined, e1D, e2D, e2DArray, Cube, CubeArray, e3D };
enum class TextureDimension : uint32_t { Undefined, e1D, e2D, e3D };
enum class TextureAspect : uint32_t { Undefined, All, StencilOnly, DepthOnly };
enum class FilterMode : uint32_t { Undefined, Nearest, Linear };
enum class MipmapFilterMode : uint32_t { Undefined, Nearest, Linear };
enum class AddressMode : uint32_t { Undefined, ClampToEdge, Repeat, MirrorRepeat };
enum class CompareFunction : uint32_t { Undefined, Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always };
enum class IndexFormat : uint32_t { Undefined, Uint16, Uint32 };
enum class PrimitiveTopology : uint32_t { Undefined, PointList, LineList, LineStrip, TriangleList, TriangleStrip };

struct Extent3D { uint32_t width = 0; uint32_t height = 1; uint32_t depthOrArrayLayers = 1; };
struct Origin3D { uint32_t x = 0, y = 0, z = 0; };
struct Color { double r = 0, g = 0, b = 0, a = 0; };
struct AdapterInfo { StringView vendor, architecture, device, description; BackendType backendType{}; };
struct SurfaceConfiguration { TextureFormat format{}; uint32_t width = 0; uint32_t height = 0; };
struct SamplerDescriptor {
  AddressMode addressModeU{}, addressModeV{}, addressModeW{};
  FilterMode magFilter{}, minFilter{};
  MipmapFilterMode mipmapFilter{};
  float lodMinClamp = 0.f, lodMaxClamp = 32.f;
  CompareFunction compare{};
  uint16_t maxAnisotropy = 1;
  const char* label = nullptr;
};
struct VertexBufferLayout { uint64_t arrayStride = 0; size_t attributeCount = 0; const void* attributes = nullptr; };

// Opaque handles. Dawn's are refcounted wrappers; for a syntax check an empty
// class with the conversions our headers rely on is enough.
#define AURORA_SHIM_HANDLE(name)                                                   \
  class name {                                                                     \
   public:                                                                         \
    name() = default;                                                              \
    explicit operator bool() const { return false; }                               \
    bool operator==(const name&) const { return true; }                            \
    bool operator!=(const name&) const { return false; }                           \
alignas(void*) char _shim[sizeof(void*)]{};                                        \
  }

AURORA_SHIM_HANDLE(Device);       AURORA_SHIM_HANDLE(Queue);
AURORA_SHIM_HANDLE(Surface);      AURORA_SHIM_HANDLE(Instance);
AURORA_SHIM_HANDLE(Texture);      AURORA_SHIM_HANDLE(TextureView);
AURORA_SHIM_HANDLE(Sampler);      AURORA_SHIM_HANDLE(Buffer);
AURORA_SHIM_HANDLE(BindGroup);    AURORA_SHIM_HANDLE(BindGroupLayout);
AURORA_SHIM_HANDLE(RenderPipeline); AURORA_SHIM_HANDLE(CommandEncoder);
AURORA_SHIM_HANDLE(RenderPassEncoder); AURORA_SHIM_HANDLE(ShaderModule);
AURORA_SHIM_HANDLE(PipelineLayout);
#undef AURORA_SHIM_HANDLE

struct TexelCopyTextureInfo { Texture texture; uint32_t mipLevel = 0; Origin3D origin{}; TextureAspect aspect{}; };
struct TexelCopyBufferInfo { Buffer buffer; uint64_t offset = 0; uint32_t bytesPerRow = 0; uint32_t rowsPerImage = 0; };

struct TexelCopyBufferLayout { uint64_t offset = 0; uint32_t bytesPerRow = 0; uint32_t rowsPerImage = 0; };

} // namespace wgpu

// C-API names aurora's headers mention by their WGPU* spelling.
struct WGPUBindGroupDescriptor;
struct WGPUBindGroupLayoutDescriptor;
struct WGPUSamplerDescriptor;
struct WGPURenderPipelineDescriptor;
struct WGPUShaderModuleDescriptor;
struct WGPUPipelineLayoutDescriptor;
