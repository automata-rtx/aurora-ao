#ifndef DOLPHIN_GXAURORA_H
#define DOLPHIN_GXAURORA_H

#include <dolphin/types.h>

#if __cplusplus
extern "C" {
#endif

//
// Subcommands for GX_AURORA.
//

/**
 * Sets the actual render viewport in native framebuffer coordinates.
 * Must be followed by six f32 values: left, top, width, height, nearz, farz.
 */
#define GX_AURORA_LOAD_VIEWPORT_RENDER 0x0001

/**
 * Sets the actual render scissor in native framebuffer coordinates.
 * Must be followed by four u32 values: left, top, width, height.
 */
#define GX_AURORA_LOAD_SCISSOR_RENDER 0x0002

/**
 * Loads a full 4x4 projection matrix, bypassing GXSetProjection's 6-parameter
 * hardware encoding. Must be followed by sixteen f32 values in row-major order.
 */
#define GX_AURORA_LOAD_PROJECTION_FULL 0x0003

/**
 * Aurora equivalent of CP_REG_ARRAYBASE_ID: sets the base address and size of a vertex array.
 * This command must be followed by a 64-bit memory address, 32-bit size, and 1-byte little-endian flag.
 * The index of the vertex array is given by the lowest 4 bits of the command ID,
 * e.g. writing GX_AURORA_LOAD_ARRAYBASE + 5 will set the vertex array for the sixth vertex attribute.
 * To set strides, use the normal CP_REG_ARRAYSTRIDE_ID register.
 */
#define GX_AURORA_LOAD_ARRAYBASE 0x0010

/**
 * Pushes a debug group to the backend graphics API. These may show in debugging tools such as RenderDoc.
 * Must be followed by a u16 string length and that many UTF-8 characters (no null terminator required).
 * It is considered an error to have unpopped debug groups at the end of the frame. They will be automatically cleared.
 */
#define GX_AURORA_DEBUG_GROUP_PUSH 0x0020

/**
 * Pops a previously pushed debug group.
 * Followed by nothing.
 */
#define GX_AURORA_DEBUG_GROUP_POP 0x0021

/**
 * Sends a debug marker to the backend graphics API.
 * Must be followed by a u16 string length and that many UTF-8 characters (no null terminator required).
 */
#define GX_AURORA_DEBUG_MARKER_INSERT 0x0022

#define GX_AURORA_LOAD_TEXOBJ 0x0030

#define GX_AURORA_LOAD_TLUT 0x0031

#define GX_AURORA_DESTROY_TEXOBJ 0x0032

#define GX_AURORA_DESTROY_TLUT 0x0033

#define GX_AURORA_DESTROY_COPY_TEX 0x0034

#define GX_AURORA_LOAD_COPY_SRC 0x0035

#define GX_AURORA_LOAD_COPY_DST 0x0036

#define GX_AURORA_LOAD_COPY_DEST 0x0037

#define GX_AURORA_REQUEST_DEPTH_SNAPSHOT 0x0038

#define GX_AURORA_BEGIN_OFFSCREEN 0x0039

#define GX_AURORA_END_OFFSCREEN 0x003A

/**
 * Draw primitives with the vertex count derived from a byte length, as written by
 * GXBegin(prim, fmt, GX_AUTO). Must be followed by a u8 draw opcode (vtxfmt|prim),
 * a u32 vertex data byte length, then that many bytes of vertex data. The byte length
 * must be a whole multiple of the current vertex size or zero (no draw).
 */
#define GX_AURORA_DRAW_SIZED 0x0040

/**
 * Draw pre-merged triangles with a prebuilt index buffer, as written by the display
 * list optimizer (aurora::gx::dl::optimize). Must be followed by a u8 draw opcode
 * (vtxfmt | GX_TRIANGLES), a u16 vertex count, a u32 index count, that many u16
 * indices, then vertex count * vertex size bytes of packed vertex data. Index data
 * is always host-endian regardless of stream endianness.
 */
#define GX_AURORA_DRAW_INDEXED 0x0041

/**
 * Enables GPU vertex skinning for subsequent draws. Must be followed by a 64-bit bone palette
 * address (jointCount mat3x4 matrices, row-major), a u32 joint count, a 64-bit influence table
 * address (vtxCount * influenceCount records of {u32 bone index, f32 weight}, keyed by position
 * index), a u32 vertex count, and a u32 influence count (1-4). The palette and influence buffers
 * must remain valid until the frame is rendered.
 */
#define GX_AURORA_SET_SKINNING 0x0050

/**
 * Disables GPU vertex skinning for subsequent draws. Followed by nothing.
 */
#define GX_AURORA_CLEAR_SKINNING 0x0051

/**
 * Provides the current world->view (camera) matrix to the backend. Must be followed
 * by twelve f32 values (row-major 3x4). Purely informational: backends that split
 * WORLD and VIEW transforms (D3D9) use it to present true world-space geometry and
 * a real camera to capture tools such as RTX Remix; the wgpu backend ignores it
 * (GX position matrices already fold the view in).
 */
#define GX_AURORA_SET_VIEW_MTX 0x0052

/**
 * Marks the draws that follow as Dusklight water, until cleared. Must be followed by a u32
 * GX_AURORA_DUSKLIGHT_WATER_* role and a second u32 carrying the material's MAxx tag as a
 * number (9 for MA09, 0 for none). The tag rides along because a body of water is drawn as
 * several surfaces and the renderer needs to tell them apart to keep one.
 *
 * It is a FIFO command rather than a plain backend call because the FIFO is drained in
 * end_frame, not as the game issues draws: a global set from the game thread is read long
 * after the material that set it has finished, so it can only ever describe whichever
 * material happened to be last. Written into the stream it arrives in order, between the
 * draws it brackets, like every other piece of GX state.
 */
#define GX_AURORA_SET_DUSKLIGHT_WATER 0x0053

/** Not water. Clears the mark. */
#define GX_AURORA_DUSKLIGHT_WATER_NONE 0
/** A water surface: the thing a refracting, translucent material belongs on. */
#define GX_AURORA_DUSKLIGHT_WATER_SURFACE 1
/**
 * A camera-projected overlay drawn *over* a water surface, not the surface itself.
 *
 * Twilight Princess draws its water in layers, and one of them (MA02/MA10) has a
 * perspective projection built from the live camera installed as its texture matrix -
 * d_kankyo.cpp dKy_bg_MAxx_proc, C_MTXLightPerspective from the camera fovy and aspect.
 * It is a rasteriser-era fake reflection, and it is screen-projected, so it slides across
 * the surface whenever the camera moves.
 *
 * A path tracer traces that reflection for real off the water surface, so this layer is
 * both redundant and, made refractive, actively wrong: a second interface a few units
 * above the first, carrying a screen-space image through it.
 */
#define GX_AURORA_DUSKLIGHT_WATER_PROJECTED 2

#define GX2_SET_POLYGON_OFFSET 0x1000


/*
 * Debug marker stuff
 */

/**
 * Pushes a debug group to the backend graphics API. These may show in debugging tools such as RenderDoc.
 * It is considered an error to have unpopped debug groups at the end of the frame. They will be automatically cleared.
 */
void GXPushDebugGroup(const char* label);

/**
 * Pop a debug group previously pushed via GXPushDebugGroup().
 */
void GXPopDebugGroup();

/**
 * Sends a debug marker to the backend graphics API. These may show in debugging tools such as RenderDoc.
 */
void GXInsertDebugMarker(const char* label);

typedef enum _AuroraViewportPolicy {
  AURORA_VIEWPORT_FIT = 0,     // Preserve logical aspect in the content framebuffer
  AURORA_VIEWPORT_STRETCH = 1, // Match content framebuffer aspect to the native surface
  AURORA_VIEWPORT_NATIVE = 2,  // Use active framebuffer pixels directly
} AuroraViewportPolicy;

/**
 * Configures content framebuffer sizing and how GXSetViewport/GXSetScissor parameters are applied to rendering.
 * When AURORA_VIEWPORT_NATIVE is used, GXSetTexCopySrc/GXSetTexCopyDst will use native framebuffer resolution.
 */
void AuroraSetViewportPolicy(AuroraViewportPolicy policy);

/**
 * Retrieves the current content framebuffer size.
 */
void AuroraGetRenderSize(u32* width, u32* height);

/**
 * Sets the actual render viewport in native framebuffer coordinates.
 * Overrides the automatically scaled values set by the logical GXSetViewport.
 */
void GXSetViewportRender(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz);

/**
 * Sets the actual render scissor in native framebuffer coordinates.
 * Overrides the automatically scaled values set by the logical GXSetScissor.
 */
void GXSetScissorRender(u32 left, u32 top, u32 wd, u32 ht);

void GX2SetPolygonOffset(f32 mFrontOffset, f32 mFrontScale, f32 mBackOffset, f32 mBackScale, f32 mClamp);

/**
 * Load an arbitrary 4x4 projection matrix, avoiding the 6-parameter hardware encoding.
 */
void GXSetProjectionFull(const void* mtx);

/**
 * Enable GPU vertex skinning for subsequent draws. The vertex position/normal are blended by
 * their bone influences in the shader, matching the CPU linear-blend result. palette points to
 * jointCount mat3x4 bone matrices; influences points to vtxCount * influenceCount records of
 * {u32 bone index, f32 weight}, indexed by vertex position index. baseMtx is the model->view
 * matrix (a 3x4 / 12 f32) applied after the blend, in place of the per-vertex position matrix.
 * influenceCount is clamped to GX_AURORA_MAX_SKIN_INFLUENCES. The palette and influence buffers
 * must stay valid until the frame renders.
 */
void GXSetSkinning(const void* palette, u32 jointCount, const void* influences, u32 vtxCount, u32 influenceCount,
                   const void* baseMtx);

/**
 * Disable GPU vertex skinning for subsequent draws.
 */
void GXClearSkinning(void);

/**
 * Provide the current world->view (camera) matrix (3x4, 12 f32 row-major - e.g.
 * J3DSys::mViewMtx). Rendering output is unchanged on every backend; the D3D9
 * backend uses it to split the GX combined model->view into WORLD (model->world)
 * and VIEW (camera), which RTX Remix requires to reconstruct a camera, place
 * geometry in stable world space, and correctly replay fixed-function skinning.
 * Call whenever the view matrix changes (per frame / per view).
 */
void GXSetViewMtx(const void* mtx);

/**
 * Enable or disable the GPU-skinning debug view. When enabled, matrix-palette-skinned draws (those
 * with a per-vertex PNMTXIDX attribute) render coloured by bone index instead of their normal
 * shading - a way to confirm those draws are skinned per-vertex on the GPU.
 */
void GXSetSkinningDebugView(bool enable);

/**
 * Mark the draws that follow with a GX_AURORA_DUSKLIGHT_WATER_* role, until cleared with
 * GX_AURORA_DUSKLIGHT_WATER_NONE. Rendering output is unchanged on every backend; the D3D9
 * backend forwards the role to RTX Remix, which makes a SURFACE draw a translucent,
 * refracting material instead of an opaque one, and drops a PROJECTED one.
 *
 * Water is a fact about the game's own materials - Twilight Princess names them by
 * convention (dKy_bg_MAxx_proc) - and GX carries nothing that could be read as "this is
 * water", so the game says so directly. Call it around exactly the draws that are water:
 * it is state in the command stream, and like every other GX state it stays set until
 * changed.
 *
 * The role exists because water is not one draw. A body of water arrives as a base
 * surface, usually a scrolling ripple pass over it, and a camera-projected reflection
 * layer - and treating all three as refracting interfaces stacks two or three sheets of
 * glass where there should be one surface.
 */
void GXSetDusklightWater(u32 role, u32 tag);

#define GX_AURORA_MAX_SKIN_INFLUENCES 4

/**
 * Create an offscreen framebuffer and switch rendering to it.
 * All subsequent GX rendering will target this framebuffer until GXRestoreFrameBuffer() is called.
 * Use GXCopyTex to resolve the offscreen content into a texture.
 */
void GXCreateFrameBuffer(u32 width, u32 height);

/**
 * Restore rendering to the main EFB framebuffer.
 * Must be called after GXCreateFrameBuffer() to resume normal rendering.
 */
void GXRestoreFrameBuffer(void);

#if __cplusplus
}
#endif

#endif
