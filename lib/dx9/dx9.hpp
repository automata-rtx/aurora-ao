#pragma once

// Direct3D 9 fixed-function backend.
//
// Alternative execution layer behind Aurora's GX front-end, designed for RTX
// Remix's d3d9->Vulkan runtime: fixed-function T&L (incl. indexed vertex
// blending for skinning), texture-stage-state TEV approximation, no shaders.
//
// The image this backend rasterizes is never shown to a player - it is the
// feed Remix translates, and Remix's renderer is the product. The HUD and
// alpha are the two exceptions that must still rasterize correctly. Read
// docs/dx9/remix-material-interface.md §0 before treating raw-D3D9
// correctness as a goal; docs/dx9/gx-to-d3d9-mapping.md is the mapping spec.
//
// The GX state decoding (fifo/command_processor -> g_gxState) is shared with
// the WebGPU path; these entry points are invoked from the shared code at the
// draw/present chokepoints, guarded by active(). When AURORA_ENABLE_D3D9 is
// not defined (non-Windows), everything collapses to constexpr no-ops so call
// sites need no #ifdefs.

#include <cstdint>

#include <dolphin/gx.h>

namespace aurora::dx9 {

#ifdef AURORA_ENABLE_D3D9

// Lifecycle -----------------------------------------------------------------

// True when the D3D9 backend owns rendering for this process (set by a
// successful initialize(), cleared by shutdown()).
bool active() noexcept;

// Creates the IDirect3D9 device against the SDL window. Returns false if
// D3D9 is unavailable (missing d3d9.dll, no HAL device, ...); the caller
// falls back to other backends.
bool initialize() noexcept;
void shutdown() noexcept;

// Frame boundaries, called from aurora::begin_frame/end_frame in place of the
// gfx/webgpu equivalents. begin_frame handles device-loss and window resizes.
bool begin_frame() noexcept;
void end_frame() noexcept;

// Backbuffer size in pixels (used for logical->render viewport mapping).
void get_backbuffer_size(uint32_t& width, uint32_t& height) noexcept;

// The live IDirect3DDevice9*, or nullptr before creation. Changes across
// device recreation (resize) - callers must poll, not cache.
void* get_device() noexcept;

// Draw path (called from gx::fifo command processing) ------------------------

// Draw vtxCount vertices of the current vertex descriptor/format. `data`
// points at the raw GX vertex stream (vtxSize bytes per vertex), big-endian
// when `bigEndian` (always true for the game FIFO). Consumes current
// g_gxState for all pipeline state.
void draw_prim(GXPrimitive prim, GXVtxFmt fmt, uint16_t vtxCount, const uint8_t* data, uint32_t vtxSize,
               bool bigEndian) noexcept;

// GX_AURORA_DRAW_INDEXED: pre-triangulated draw with host-endian u16 indices.
void draw_indexed(GXVtxFmt fmt, uint16_t vtxCount, const uint8_t* vtxData, uint32_t vtxSize, const uint16_t* indices,
                  uint32_t indexCount, bool bigEndian) noexcept;

// GPU skinning extension (GX_AURORA_SET_SKINNING): keeps host pointers to the
// bone palette (jointCount * mat3x4 row-major f32) and per-position influence
// records ({u32 bone, f32 weight} * influenceCount, keyed by POS index).
// Buffers stay valid until the frame ends (same contract as the wgpu path).
void set_skinning(const void* palette, uint32_t jointCount, const void* influences, uint32_t vtxCount,
                  uint32_t influenceCount) noexcept;
void clear_skinning() noexcept;

// Camera view matrix (GX_AURORA_SET_VIEW_MTX): world->view as 12 f32 row-major
// 3x4. When provided, draws split the GX combined model->view into
// WORLD = model->world (pnMtx * view^-1) and VIEW = camera, so Remix can
// reconstruct a camera, keep geometry in stable world space, and replay
// fixed-function skinning. Without it Remix rejects every draw's camera
// (objectToView == objectToWorld) - a failure state to detect, not a mode to
// run in. See docs/dx9/gx-to-d3d9-mapping.md #3/#13.
void set_camera_view(const float* mtx3x4) noexcept;

// State relays ----------------------------------------------------------------

// Reads g_gxState.renderViewport / renderScissor and applies immediately.
void set_render_viewport() noexcept;
void set_render_scissor() noexcept;

// EFB copy (GXCopyTex): color copies StretchRect the current render target
// (backbuffer or offscreen) into a texture registered for `dest`; depth /
// unsupported formats fall back to a neutral placeholder. Clear semantics are
// applied to the source region. See docs/dx9/gx-to-d3d9-mapping.md #10.
void copy_tex(const void* dest, bool clear) noexcept;

// Offscreen framebuffer passes (GXCreateFrameBuffer): rendered to a real
// render-target texture (minimap, shadow silhouettes, ...).
void begin_offscreen(uint32_t width, uint32_t height) noexcept;
void end_offscreen() noexcept;
bool in_offscreen() noexcept;

// Dusklight: gives the draws that follow a GX_AURORA_DUSKLIGHT_WATER_* role, until
// cleared with _NONE.
//
// The game identifies its own water by J3D material name (dKy_bg_MAxx_proc), which is a
// fact GX never carries - so it is told rather than inferred from TEV state. Remix turns
// a SURFACE draw into a translucent material; without it every water layer falls through
// to an opaque one and reads as a white sheet. It drops a PROJECTED one, which is a
// camera-projected fake reflection over the surface rather than the surface itself, and
// which as a second refracting interface is what stops water reading as one sheet.
// Carried to Remix packed into D3DMATERIAL9::Power - see set_remix_material.
//
// CALLED BY THE COMMAND PROCESSOR, from GX_AURORA_SET_DUSKLIGHT_WATER, and by nothing
// else. The game must not call it: the FIFO is drained in end_frame, so a value written
// here from the game thread is read after every draw the game has issued, and describes
// whichever material was last rather than the one that set it. That is not a subtle
// timing window - it is every draw in the frame, and it cost two test sessions. Both
// failure modes came out of it: left latched, the last water material marked everything
// drained afterwards, and every material in the game turned translucent; cleared after
// each material, the flag was always false by drain time and no water arrived at all.
void set_dusklight_water(uint32_t role, uint32_t tag, uint32_t layer) noexcept;

// Cache eviction mirrors (GX_AURORA_DESTROY_*).
void on_evict_texture(uint32_t texObjId) noexcept;
void on_evict_tlut(uint32_t tlutObjId) noexcept;
void on_evict_copy_texture(const void* dest) noexcept;

#else // !AURORA_ENABLE_D3D9 -------------------------------------------------

constexpr bool active() noexcept { return false; }
inline bool initialize() noexcept { return false; }
inline void shutdown() noexcept {}
inline bool begin_frame() noexcept { return false; }
inline void end_frame() noexcept {}
inline void get_backbuffer_size(uint32_t& width, uint32_t& height) noexcept { width = height = 0; }
inline void* get_device() noexcept { return nullptr; }
inline void draw_prim(GXPrimitive, GXVtxFmt, uint16_t, const uint8_t*, uint32_t, bool) noexcept {}
inline void draw_indexed(GXVtxFmt, uint16_t, const uint8_t*, uint32_t, const uint16_t*, uint32_t, bool) noexcept {}
inline void set_skinning(const void*, uint32_t, const void*, uint32_t, uint32_t) noexcept {}
inline void clear_skinning() noexcept {}
inline void set_camera_view(const float*) noexcept {}
inline void set_dusklight_water(uint32_t, uint32_t, uint32_t) noexcept {}
inline void set_render_viewport() noexcept {}
inline void set_render_scissor() noexcept {}
inline void copy_tex(const void*, bool) noexcept {}
inline void begin_offscreen(uint32_t, uint32_t) noexcept {}
inline void end_offscreen() noexcept {}
inline bool in_offscreen() noexcept { return false; }
inline void on_evict_texture(uint32_t) noexcept {}
inline void on_evict_tlut(uint32_t) noexcept {}
inline void on_evict_copy_texture(const void*) noexcept {}

#endif // AURORA_ENABLE_D3D9

} // namespace aurora::dx9
