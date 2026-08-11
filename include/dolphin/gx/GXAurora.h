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
 * Gives the draws that follow a water role, until cleared with _NONE. Must be followed by
 * three u32: a GX_AURORA_DUSKLIGHT_WATER_* role, the MAxx tag as a number (9 for MA09, 0
 * for none), and a GX_AURORA_DUSKLIGHT_WATER_LAYER_* pass.
 *
 * The game identifies its own water by J3D material name (dKy_bg_MAxx_proc), which is a
 * fact GX never carries - so it is told rather than inferred from TEV state. Only the D3D9
 * backend consumes it; every other backend renders water as it always did.
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
/** Highest role value. The packing in GX_AURORA_DUSKLIGHT_WATER_PACK depends on it. */
#define GX_AURORA_DUSKLIGHT_WATER_ROLE_MAX 2

/*
 * Which layer of a body of water a draw is, named from the game's own material names.
 *
 * Twilight Princess is a Japanese production and the decompilation preserves its naming, so
 * these words are the developers' own labels for the passes: a lake is drawn as several of
 * them stacked, and "the murky one" is a different surface from "the wave one" even though
 * both carry the MA06 tag. The tag alone is too coarse to tell them apart - it was tried,
 * and hiding MA06 would have deleted a lake's shoreline and its waves to be rid of its murk.
 *
 * UNKNOWN is the safe value: a name nobody has classified stays visible.
 */
#define GX_AURORA_DUSKLIGHT_WATER_LAYER_UNKNOWN   0
/** mera - the shimmer / heat-haze surface pass. */
#define GX_AURORA_DUSKLIGHT_WATER_LAYER_SHIMMER   1
/** nami - waves. */
#define GX_AURORA_DUSKLIGHT_WATER_LAYER_WAVES     2
/** mizugiwa - the water's edge, where it meets the shore. */
#define GX_AURORA_DUSKLIGHT_WATER_LAYER_SHORELINE 3
/** nigori - the murky body of the water. */
#define GX_AURORA_DUSKLIGHT_WATER_LAYER_MURK      4
/** funsui - a fountain. An object rather than a layer of a lake. */
#define GX_AURORA_DUSKLIGHT_WATER_LAYER_FOUNTAIN  5
/** kasan - an additively blended pass. Light over a surface, not a surface. */
#define GX_AURORA_DUSKLIGHT_WATER_LAYER_ADDITIVE  6
/** The indirect-textured pass: the warp the game uses to fake refraction. */
#define GX_AURORA_DUSKLIGHT_WATER_LAYER_INDIRECT  7
/** Highest layer value. The packing in GX_AURORA_DUSKLIGHT_WATER_PACK depends on it. */
#define GX_AURORA_DUSKLIGHT_WATER_LAYER_MAX       7

/**
 * All three water facts packed into one number, for D3DMATERIAL9::Power.
 *
 *     Power = tag * 100 + layer * 10 + role
 *
 * The side band has exactly one field left (docs/dx9/remix-material-interface.md §2), and
 * water needs three facts, so they share it. Decimal rather than bit fields on purpose:
 * the number is read by a human in a log far more often than by code, and `power=921` is
 * legible as MA09 / waves / surface where `0x25` is not. Nothing is lost by it - role is
 * 0-2, layer 0-7, tag 0-99, so the maximum is 9972 and a float32 represents every integer
 * to 16777216 exactly.
 *
 * The fork decodes this in rtx_dusklight_water.h. The two sides are checked against each
 * other by scripts/check_invariants.py; change one and the check names the other.
 */
#define GX_AURORA_DUSKLIGHT_WATER_TAG_MAX 99
#define GX_AURORA_DUSKLIGHT_WATER_PACK(role, tag, layer) \
  ((u32)(tag) * 100u + (u32)(layer) * 10u + (u32)(role))

/*
 * ---------------------------------------------------------------------------------------
 * Aurora subcommand registry - READ THIS BEFORE TAKING A NUMBER
 * ---------------------------------------------------------------------------------------
 *
 * Every number above is dispatched by one `else if` arm in lib/gx/command_processor.cpp
 * handle_aurora(). Two features that take the same number do NOT produce a merge conflict
 * there: both arms land, the first one tested wins, and because the arms consume different
 * payload lengths the loser leaves the reader mid-payload and desyncs the FIFO. The symptom
 * is a garbled frame or a CHECK naming an opcode the game never called - not a missing
 * feature - so it is debugged in the wrong place.
 *
 * That is not hypothetical. On 2026-08-11 four unmerged branches had each taken 0x0053.
 *
 * Allocated:  0x0001-0x0003, 0x0010, 0x0020-0x0022, 0x0030-0x003A,
 *             0x0040-0x0041, 0x0050-0x0053, 0x1000
 *
 * Reserved for work in flight - do not take these, and do not assume a branch still wants
 * one without looking. Each branch takes its assigned number when it rebases:
 *
 *     0x0054  GX_AURORA_SET_MODEL_IDENTITY    claude/remix-texture-geometry-issues-occh2f
 *     0x0055  GX_AURORA_CLEAR_MODEL_IDENTITY  (same branch - it needs two)
 *     0x0056  GX_AURORA_SET_POS_MTX_REST      claude/lss-hair-rtx-remix-j6uo4t
 *     0x0057  GX_AURORA_SET_DRAW_CLASS        claude/dusklight-remix-transparency-e7l766
 *
 * Next free: 0x0058. Add it to this list in the same commit that defines it -
 * scripts/check_invariants.py fails if a GX_AURORA_* define is missing from the registry,
 * or if two of them share a value.
 */

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
 * Mark the draws that follow as water, until cleared with GX_AURORA_DUSKLIGHT_WATER_NONE.
 * `role` is a GX_AURORA_DUSKLIGHT_WATER_* value, `tag` the MAxx number (9 for MA09, 0 for
 * none), `layer` a GX_AURORA_DUSKLIGHT_WATER_LAYER_* pass.
 *
 * Call it around the draws it describes - J3DMaterial::load is where the game does it,
 * because that is the point where a material's GX state is programmed and therefore the
 * only place that brackets exactly its own draws. Do NOT set a backend global instead: the
 * FIFO is drained in end_frame, so a value written from the game thread is read after every
 * draw the game issued and describes whichever material was last. That cost two test
 * sessions, in both directions - left latched it turned every material in the game
 * translucent, and cleared per material no water arrived at all.
 */
void GXSetDusklightWater(u32 role, u32 tag, u32 layer);

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
