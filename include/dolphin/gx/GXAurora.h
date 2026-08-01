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
 * Annotates the most recent load of a GX position matrix with the constant
 * transform from the storage space of the vertices drawn with it to the
 * model's rest space. Followed by a u32 position-matrix id (GX_PNMTX0..9)
 * and twelve f32 (row-major 3x4). Cleared by the next load of the same
 * position matrix. Backends that re-emit stable rest-space geometry (D3D9
 * for RTX Remix) consume it; rasterization is unchanged everywhere.
 */
#define GX_AURORA_SET_POS_MTX_REST 0x0053

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
 * Declare, immediately after loading a GX position matrix (id = GX_PNMTX0..
 * GX_PNMTX9), that the vertices drawn with it are stored in a space other
 * than the model's rest space: restMtx (3x4, 12 f32 row-major) maps stored
 * coordinates to rest-space coordinates. J3D stores a single-joint ("full
 * weight") shape's vertices in the joint's local frame while enveloped
 * shapes' vertices sit in model/bind space, so one character's merged mesh
 * mixes coordinate spaces. With the annotation the D3D9 backend rewrites
 * decoded vertices into rest space and compensates the world matrices with
 * the inverse - rasterization is identical, but the bytes RTX Remix hashes,
 * skins and captures form one coherent rest-pose mesh. Any load of the same
 * position matrix clears the annotation, so unannotated engine code keeps
 * its current behavior. The wgpu backend ignores it.
 */
void GXSetPosMtxRest(const void* restMtx, u32 id);

/**
 * Enable or disable the GPU-skinning debug view. When enabled, matrix-palette-skinned draws (those
 * with a per-vertex PNMTXIDX attribute) render coloured by bone index instead of their normal
 * shading - a way to confirm those draws are skinned per-vertex on the GPU.
 */
void GXSetSkinningDebugView(bool enable);

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
