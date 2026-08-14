#include "dx9.hpp"

#ifdef AURORA_ENABLE_D3D9

#include "dx9_internal.hpp"
#include "dx9_texture.hpp"
#include "../window.hpp"

#include <SDL3/SDL.h>

#include <absl/container/flat_hash_set.h>

#include <algorithm>
#include <cmath>

namespace aurora::dx9 {
static Module Log("aurora::dx9");

Device g_dx9;
StateCache g_cache;
SkinState g_skin;
WorldViewInv g_worldViewInv;
CameraView g_camera;

static bool s_active = false;
static absl::flat_hash_set<uint64_t> s_warned;
// Material translation report. Deliberately a SEPARATE set from s_warned: a key
// collision between the two would silently suppress one of them, and these keys
// are content hashes rather than hand-assigned ids. See
// docs/dx9/material-report.md for the format and why it is always on.
static absl::flat_hash_set<uint64_t> s_matrep;
static uint32_t s_matrepSuppressed = 0;
// Backbuffer color/depth surfaces, cached after device create/reset so
// offscreen passes can restore them.
static IDirect3DSurface9* s_backbufferColor = nullptr;
static IDirect3DSurface9* s_backbufferDepth = nullptr;
// Window size seen last frame, used to wait out a resize drag before acting.
static uint32_t s_pendingWidth = 0;
static uint32_t s_pendingHeight = 0;

static void release_backbuffer_surfaces() noexcept {
  if (s_backbufferColor != nullptr) {
    s_backbufferColor->Release();
    s_backbufferColor = nullptr;
  }
  if (s_backbufferDepth != nullptr) {
    s_backbufferDepth->Release();
    s_backbufferDepth = nullptr;
  }
}

static void cache_backbuffer_surfaces() noexcept {
  release_backbuffer_surfaces();
  g_dx9.dev->GetRenderTarget(0, &s_backbufferColor);
  g_dx9.dev->GetDepthStencilSurface(&s_backbufferDepth);
}

static uint32_t current_target_width() noexcept {
  return g_dx9.inOffscreen ? g_dx9.offscreenWidth : g_dx9.renderWidth;
}
static uint32_t current_target_height() noexcept {
  return g_dx9.inOffscreen ? g_dx9.offscreenHeight : g_dx9.renderHeight;
}
// Origin of the render area within the current target (letterbox offset on the
// backbuffer, zero inside an offscreen pass, which owns its whole target).
static int32_t target_offset_x() noexcept { return g_dx9.inOffscreen ? 0 : g_dx9.renderOffsetX; }
static int32_t target_offset_y() noexcept { return g_dx9.inOffscreen ? 0 : g_dx9.renderOffsetY; }

bool active() noexcept { return s_active; }

void info_once(uint64_t key, const char* what) noexcept {
  if (s_warned.insert(key).second) {
    Log.info("dx9: {}", what);
  }
}

void warn_once(uint64_t key, const char* what) noexcept {
  if (s_warned.insert(key).second) {
    Log.warn("dx9: unsupported: {} (key={:#x})", what, key);
  }
}

// One line per distinct material configuration, capped so a play session costs
// kilobytes. Format and reading guide: docs/dx9/material-report.md.
bool matrep_should_emit(uint64_t key) noexcept {
  if (s_matrep.contains(key)) {
    return false;
  }
  if (s_matrep.size() >= kMatrepMaxMaterials) {
    if (s_matrepSuppressed++ == 0) {
      Log.info("matrep.trunc side=aurora cap={} - further distinct materials not reported",
               kMatrepMaxMaterials);
    }
    return false;
  }
  s_matrep.insert(key);
  return true;
}

static void apply_default_state() noexcept {
  g_cache.invalidate();
  auto* dev = g_dx9.dev;
  // Fixed-function baseline. D3D9 T&L lighting stays off: Remix relights
  // everything, so reproducing the GC light model buys nothing. GX's
  // per-channel lighting-enable bit is still *read* - it decides whether
  // vertex colour is forwarded as material colour and feeds the emissive
  // score. docs/dx9/gx-to-d3d9-mapping.md #8.
  dev->SetRenderState(D3DRS_LIGHTING, FALSE);
  dev->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
  dev->SetRenderState(D3DRS_NORMALIZENORMALS, TRUE);
  dev->SetRenderState(D3DRS_COLORVERTEX, TRUE);
  dev->SetRenderState(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1);
  dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
  dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
  dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
  dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
  dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
  dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
  dev->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_DISABLE);
  dev->SetRenderState(D3DRS_INDEXEDVERTEXBLENDENABLE, FALSE);
  const D3DMATRIX identity{{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}}};
  dev->SetTransform(D3DTS_VIEW, &identity);
  g_cache.view = identity;
  g_cache.viewValid = true;
}

static HWND window_hwnd() noexcept {
  SDL_Window* win = window::get_sdl_window();
  if (win == nullptr) {
    return nullptr;
  }
  return static_cast<HWND>(
      SDL_GetPointerProperty(SDL_GetWindowProperties(win), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
}

static void fill_present_params(uint32_t width, uint32_t height) noexcept {
  auto& pp = g_dx9.pp;
  pp = {};
  pp.BackBufferWidth = std::max(width, 1u);
  pp.BackBufferHeight = std::max(height, 1u);
  // Alpha in the backbuffer so GX destination-alpha blends work, and because
  // Remix reads the stage's alpha to build opacity and the alpha test
  // (docs/dx9/gx-to-d3d9-mapping.md #11).
  pp.BackBufferFormat = D3DFMT_A8R8G8B8;
  pp.BackBufferCount = 1;
  pp.MultiSampleType = D3DMULTISAMPLE_NONE; // no MSAA by design (Remix)
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow = g_dx9.hwnd;
  pp.Windowed = TRUE; // fullscreen is handled as borderless by SDL
  pp.EnableAutoDepthStencil = TRUE;
  pp.AutoDepthStencilFormat = D3DFMT_D24S8;
  pp.PresentationInterval =
      aurora::g_config.vsync ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
}

static void current_window_size(uint32_t& width, uint32_t& height) noexcept {
  const auto size = window::get_window_size();
  width = size.native_fb_width != 0 ? size.native_fb_width : size.width;
  height = size.native_fb_height != 0 ? size.native_fb_height : size.height;
}

// Recomputes the letterboxed render area from the window's framebuffer size -
// the same size the game asks aurora for when it lays out its HUD - and
// centers it in the backbuffer.
static void update_render_rect() noexcept {
  const auto size = window::get_window_size();
  uint32_t w = size.fb_width != 0 ? size.fb_width : g_dx9.width;
  uint32_t h = size.fb_height != 0 ? size.fb_height : g_dx9.height;
  w = std::clamp(w, 1u, std::max(g_dx9.width, 1u));
  h = std::clamp(h, 1u, std::max(g_dx9.height, 1u));
  g_dx9.renderWidth = w;
  g_dx9.renderHeight = h;
  g_dx9.renderOffsetX = static_cast<int32_t>((g_dx9.width - w) / 2);
  g_dx9.renderOffsetY = static_cast<int32_t>((g_dx9.height - h) / 2);
}

// Creates the device and everything hanging off it. Shared by startup and by
// the resize path, which recreates rather than resets (see recreate_device).
static bool create_device_for(uint32_t width, uint32_t height) noexcept {
  fill_present_params(width, height);

  // FPU_PRESERVE: the game relies on standard FPU behavior. MULTITHREADED as
  // a safety net only; all draws happen on the game thread.
  const DWORD behavior = D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED | D3DCREATE_FPU_PRESERVE;
  HRESULT hr =
      g_dx9.d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_dx9.hwnd, behavior, &g_dx9.pp, &g_dx9.dev);
  if (FAILED(hr)) {
    Log.warn("dx9: HW vertex processing unavailable ({:#x}), trying mixed", static_cast<uint32_t>(hr));
    hr = g_dx9.d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_dx9.hwnd,
                                 D3DCREATE_MIXED_VERTEXPROCESSING | D3DCREATE_MULTITHREADED |
                                     D3DCREATE_FPU_PRESERVE,
                                 &g_dx9.pp, &g_dx9.dev);
  }
  if (FAILED(hr)) {
    Log.error("dx9: CreateDevice failed ({:#x})", static_cast<uint32_t>(hr));
    g_dx9.dev = nullptr;
    return false;
  }

  g_dx9.dev->GetDeviceCaps(&g_dx9.caps);
  g_dx9.perStageConstants = (g_dx9.caps.PrimitiveMiscCaps & D3DPMISCCAPS_PERSTAGECONSTANT) != 0;
  g_dx9.tssTemp = (g_dx9.caps.PrimitiveMiscCaps & D3DPMISCCAPS_TSSARGTEMP) != 0;
  g_dx9.width = g_dx9.pp.BackBufferWidth;
  g_dx9.height = g_dx9.pp.BackBufferHeight;
  g_dx9.deviceLost = false;
  g_dx9.inScene = false;
  g_dx9.inOffscreen = false;
  update_render_rect();
  apply_default_state();
  cache_backbuffer_surfaces();
  texture_cache_initialize();
  return true;
}

// Window resizes recreate the device instead of resetting it. Remix does not
// re-derive its UI overlay from a mid-run Reset, and the HUD is one of the two
// things that must still rasterize correctly (Remix rasterizes UI draws rather
// than path-tracing them). Signature: launching straight into the final
// resolution looked correct while every resized run did not - raw D3D9 follows
// the Reset either way, which is how the fault was localised to Remix.
// Recreating costs a texture-cache rebuild and a Remix renderer restart, but
// resizes are rare and user-driven. docs/dx9/unsupported-effects.md R4.
static bool recreate_device(uint32_t width, uint32_t height) noexcept {
  if (g_dx9.dev != nullptr) {
    if (g_dx9.inOffscreen) {
      end_offscreen();
    }
    if (g_dx9.inScene) {
      g_dx9.dev->EndScene();
      g_dx9.inScene = false;
    }
    for (uint32_t stage = 0; stage < MaxStages; ++stage) {
      g_dx9.dev->SetTexture(stage, nullptr);
    }
    release_backbuffer_surfaces();
    // Every cached texture belongs to the outgoing device.
    texture_cache_shutdown();
    g_dx9.dev->Release();
    g_dx9.dev = nullptr;
  }
  g_cache.invalidate();
  if (!create_device_for(width, height)) {
    Log.warn("dx9: device recreation failed; retrying next frame");
    return false;
  }
  Log.info("dx9: device recreated {}x{} (render {}x{}+{}+{})", g_dx9.width, g_dx9.height, g_dx9.renderWidth,
           g_dx9.renderHeight, g_dx9.renderOffsetX, g_dx9.renderOffsetY);
  return true;
}

bool initialize() noexcept {
  g_dx9.hwnd = window_hwnd();
  if (g_dx9.hwnd == nullptr) {
    Log.error("dx9: no Win32 HWND available");
    return false;
  }
  g_dx9.d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (g_dx9.d3d == nullptr) {
    Log.error("dx9: Direct3DCreate9 failed (d3d9.dll missing?)");
    return false;
  }

  uint32_t width = 0;
  uint32_t height = 0;
  current_window_size(width, height);
  if (!create_device_for(width, height)) {
    g_dx9.d3d->Release();
    g_dx9.d3d = nullptr;
    return false;
  }
  s_active = true;

  Log.info("dx9: device created {}x{} (render {}x{}+{}+{}, maxBlendMtxIdx={}, perStageConstants={}, tssTemp={})",
           g_dx9.width, g_dx9.height, g_dx9.renderWidth, g_dx9.renderHeight, g_dx9.renderOffsetX, g_dx9.renderOffsetY,
           g_dx9.caps.MaxVertexBlendMatrixIndex, g_dx9.perStageConstants, g_dx9.tssTemp);
  return true;
}

void shutdown() noexcept {
  if (!s_active) {
    return;
  }
  release_backbuffer_surfaces();
  texture_cache_shutdown();
  if (g_dx9.dev != nullptr) {
    g_dx9.dev->Release();
    g_dx9.dev = nullptr;
  }
  if (g_dx9.d3d != nullptr) {
    g_dx9.d3d->Release();
    g_dx9.d3d = nullptr;
  }
  g_skin = {};
  g_camera = {};
  s_active = false;
}

static bool reset_device(uint32_t width, uint32_t height) noexcept {
  // Reset fails unless every D3DPOOL_DEFAULT resource is released first, and a
  // resource still bound to the device stays alive through our Release. Drop
  // the offscreen pass (which owns the render target), unbind every texture,
  // then release the default-pool caches.
  if (g_dx9.inOffscreen) {
    end_offscreen();
  }
  if (g_dx9.inScene) {
    g_dx9.dev->EndScene();
    g_dx9.inScene = false;
  }
  for (uint32_t stage = 0; stage < MaxStages; ++stage) {
    g_dx9.dev->SetTexture(stage, nullptr);
  }
  release_backbuffer_surfaces();
  texture_cache_release_default_pool();
  fill_present_params(width, height);
  const HRESULT hr = g_dx9.dev->Reset(&g_dx9.pp);
  if (FAILED(hr)) {
    Log.warn("dx9: Reset failed ({:#x}); retrying next frame", static_cast<uint32_t>(hr));
    // Leave the device marked lost so the next frame retries rather than
    // drawing against a half-reset device.
    g_dx9.deviceLost = true;
    return false;
  }
  g_dx9.width = g_dx9.pp.BackBufferWidth;
  g_dx9.height = g_dx9.pp.BackBufferHeight;
  update_render_rect();
  apply_default_state();
  cache_backbuffer_surfaces();
  return true;
}

bool begin_frame() noexcept {
  // Water is a property of the draws a material brackets, never of a frame. The game closes
  // each bracket in the command stream; clearing here as well means a frame whose stream
  // ends mid-bracket - an early return, an exception, a path that never reaches the close -
  // starts the next drain with water off rather than marking everything until the next
  // water material appears.
  g_dusklightWaterRole = GX_AURORA_DUSKLIGHT_WATER_NONE;
  g_dusklightWaterTag = 0;
  g_dusklightWaterLayer = GX_AURORA_DUSKLIGHT_WATER_LAYER_UNKNOWN;

  uint32_t width = 0;
  uint32_t height = 0;
  current_window_size(width, height);
  if (width == 0 || height == 0) {
    // Minimized: there is nothing to present, and sizing a backbuffer to 0
    // would fail. Skip the frame and pick the size up on restore.
    return false;
  }

  if (g_dx9.dev == nullptr) {
    // A previous recreation failed; keep retrying rather than going dark.
    return s_active && g_dx9.d3d != nullptr && create_device_for(width, height);
  }

  // Device-loss / resize handling.
  const HRESULT coop = g_dx9.dev->TestCooperativeLevel();
  if (coop == D3DERR_DEVICELOST) {
    g_dx9.deviceLost = true;
    return false;
  }
  if (width != g_dx9.width || height != g_dx9.height) {
    // Wait for the size to settle before acting: dragging a window edge
    // reports a new size every frame, and recreating the device (and with it
    // Remix's renderer) per frame would be unusable. One stable frame is
    // enough, and the interim frames just present at the old size.
    if (width == s_pendingWidth && height == s_pendingHeight) {
      s_pendingWidth = 0;
      s_pendingHeight = 0;
      if (!recreate_device(width, height)) {
        return false;
      }
    } else {
      s_pendingWidth = width;
      s_pendingHeight = height;
      update_render_rect();
    }
  } else if (coop == D3DERR_DEVICENOTRESET || g_dx9.deviceLost) {
    // Same size, so a plain Reset is enough to recover a lost device.
    if (!reset_device(width, height)) {
      return false;
    }
    g_dx9.deviceLost = false;
  } else {
    // The backbuffer is unchanged, but the render area still tracks the
    // framebuffer size (viewport policy / display scale can move it).
    update_render_rect();
  }

  const auto& clear = g_gxState.clearColor;
  const D3DCOLOR clearColor = D3DCOLOR_COLORVALUE(clear[0], clear[1], clear[2], clear[3]);
  const bool letterboxed = g_dx9.renderWidth != g_dx9.width || g_dx9.renderHeight != g_dx9.height;
  if (letterboxed) {
    // Bars stay black rather than inheriting the scene's clear color.
    g_dx9.dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, D3DCOLOR_ARGB(255, 0, 0, 0),
                     1.0f, 0);
    D3DRECT rect{g_dx9.renderOffsetX, g_dx9.renderOffsetY,
                 g_dx9.renderOffsetX + static_cast<LONG>(g_dx9.renderWidth),
                 g_dx9.renderOffsetY + static_cast<LONG>(g_dx9.renderHeight)};
    g_dx9.dev->Clear(1, &rect, D3DCLEAR_TARGET, clearColor, 1.0f, 0);
  } else {
    g_dx9.dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, clearColor, 1.0f, 0);
  }
  if (SUCCEEDED(g_dx9.dev->BeginScene())) {
    g_dx9.inScene = true;
  }
  g_dx9.inOffscreen = false;
  texture_cache_begin_frame();
  return true;
}

void end_frame() noexcept {
  if (g_dx9.dev == nullptr) {
    return;
  }
  if (g_dx9.inOffscreen) {
    // Unbalanced GXCreateFrameBuffer; make sure the frame presents the
    // backbuffer and the next Clear hits it.
    end_offscreen();
  }
  if (g_dx9.inScene) {
    g_dx9.dev->EndScene();
    g_dx9.inScene = false;
  }
  draw_stats_end_frame();
  const HRESULT hr = g_dx9.dev->Present(nullptr, nullptr, nullptr, nullptr);
  if (hr == D3DERR_DEVICELOST) {
    g_dx9.deviceLost = true;
  }
}

void get_backbuffer_size(uint32_t& width, uint32_t& height) noexcept {
  // Current render target size: the shared gx layer uses this for
  // logical->render mapping, which must track offscreen passes.
  width = current_target_width();
  height = current_target_height();
}

void* get_device() noexcept {
  return g_dx9.dev;
}

// ---------------------------------------------------------------------------
// Transforms
// ---------------------------------------------------------------------------

void set_proj_matrix(const D3DMATRIX& m) noexcept {
  if (g_cache.projValid && mtx_equal(g_cache.proj, m)) {
    return;
  }
  set_transform(D3DTS_PROJECTION, m);
  g_cache.proj = m;
  g_cache.projValid = true;
}

void set_view_matrix(const D3DMATRIX& m) noexcept {
  if (g_cache.viewValid && mtx_equal(g_cache.view, m)) {
    return;
  }
  set_transform(D3DTS_VIEW, m);
  g_cache.view = m;
  g_cache.viewValid = true;
}

void set_world_matrix(uint32_t slot, const D3DMATRIX& m) noexcept {
  if (slot < g_cache.world.size()) {
    if (g_cache.worldValid[slot] && mtx_equal(g_cache.world[slot], m)) {
      return;
    }
    g_cache.world[slot] = m;
    g_cache.worldValid[slot] = true;
  }
  set_transform(static_cast<D3DTRANSFORMSTATETYPE>(D3DTS_WORLDMATRIX(slot)), m);
}

void set_texture_matrix(uint32_t stage, const D3DMATRIX& m) noexcept {
  if (stage < MaxStages) {
    if (g_cache.texMtxValid[stage] && mtx_equal(g_cache.texMtx[stage], m)) {
      return;
    }
    g_cache.texMtx[stage] = m;
    g_cache.texMtxValid[stage] = true;
  }
  set_transform(static_cast<D3DTRANSFORMSTATETYPE>(D3DTS_TEXTURE0 + stage), m);
}

// ---------------------------------------------------------------------------
// Viewport / scissor (render coordinates; the logical->render mapping is done
// by the shared gx layer before these are called)
// ---------------------------------------------------------------------------

void set_render_viewport() noexcept {
  if (g_dx9.dev == nullptr) {
    return;
  }
  const auto& vp = g_gxState.renderViewport;
  const float maxW = static_cast<float>(current_target_width());
  const float maxH = static_cast<float>(current_target_height());
  const float left = std::clamp(vp.left, 0.f, maxW);
  const float top = std::clamp(vp.top, 0.f, maxH);
  const float right = std::clamp(vp.left + vp.width, left, maxW);
  const float bottom = std::clamp(vp.top + vp.height, top, maxH);
  D3DVIEWPORT9 d3dvp{};
  d3dvp.X = static_cast<DWORD>(left + static_cast<float>(target_offset_x()));
  d3dvp.Y = static_cast<DWORD>(top + static_cast<float>(target_offset_y()));
  d3dvp.Width = std::max<DWORD>(static_cast<DWORD>(right - left), 1);
  d3dvp.Height = std::max<DWORD>(static_cast<DWORD>(bottom - top), 1);
  d3dvp.MinZ = std::clamp(vp.znear, 0.f, 1.f);
  d3dvp.MaxZ = std::clamp(vp.zfar, 0.f, 1.f);
  if (d3dvp.MaxZ < d3dvp.MinZ) {
    std::swap(d3dvp.MinZ, d3dvp.MaxZ);
  }
  g_dx9.dev->SetViewport(&d3dvp);
}

void set_render_scissor() noexcept {
  if (g_dx9.dev == nullptr) {
    return;
  }
  const auto& sc = g_gxState.renderScissor;
  const auto maxW = static_cast<int32_t>(current_target_width());
  const auto maxH = static_cast<int32_t>(current_target_height());
  const auto left = std::clamp<int32_t>(sc.x, 0, maxW);
  const auto top = std::clamp<int32_t>(sc.y, 0, maxH);
  const auto right = std::clamp<int32_t>(sc.x + sc.width, left, maxW);
  const auto bottom = std::clamp<int32_t>(sc.y + sc.height, top, maxH);
  RECT rect;
  rect.left = left + target_offset_x();
  rect.top = top + target_offset_y();
  rect.right = right + target_offset_x();
  rect.bottom = bottom + target_offset_y();
  g_dx9.dev->SetScissorRect(&rect);
}

// ---------------------------------------------------------------------------
// EFB copies & offscreen passes (docs/dx9/gx-to-d3d9-mapping.md #10)
// ---------------------------------------------------------------------------

// Scales the logical copy destination size to render pixels (mirrors the
// shared scale_copy_dst in GXFrameBuffer.cpp).
static void scale_copy_dst(uint32_t& width, uint32_t& height) noexcept {
  width = std::max(g_gxState.texCopyDstWidth, 1u);
  height = std::max(g_gxState.texCopyDstHeight, 1u);
  if (g_gxState.viewportPolicy == AURORA_VIEWPORT_NATIVE) {
    return;
  }
  const auto [logicalW, logicalH] = gx::logical_fb_size();
  if (logicalW == 0 || logicalH == 0) {
    return;
  }
  const float scaleX = static_cast<float>(current_target_width()) / static_cast<float>(logicalW);
  const float scaleY = static_cast<float>(current_target_height()) / static_cast<float>(logicalH);
  width = std::max<uint32_t>(static_cast<uint32_t>(std::lround(static_cast<float>(width) * scaleX)), 1);
  height = std::max<uint32_t>(static_cast<uint32_t>(std::lround(static_cast<float>(height) * scaleY)), 1);
}

// The fork's per-draw metadata export, resolved once and never required.
//
// GetProcAddress rather than a link-time import on purpose: the game already resolves the fork's
// getRtxOptionValue exactly this way and warns when it is absent, and that is what lets the two
// binaries be rebuilt independently. A stock Remix, or a fork built before this landed, simply
// does not have the symbol and every draw behaves as it always did.
namespace {

using PFN_dusklightSetDrawMeta = void(__cdecl*)(const void*, uint32_t);

// Mirrors DusklightDrawMeta in the fork's rtx_dusklight_drawmeta.h. The two are reconciled by
// structSize at the call, not by being kept identical - add fields at the END only.
struct DusklightDrawMetaWire {
  uint32_t structSize;
  uint32_t flags;
};

PFN_dusklightSetDrawMeta resolve_draw_meta_export() noexcept {
  static bool s_resolved = false;
  static PFN_dusklightSetDrawMeta s_fn = nullptr;

  if (!s_resolved) {
    s_resolved = true;
    // Already loaded - the game links d3d9 and the fork *is* d3d9. Not LoadLibrary: taking a second
    // reference to the module the process is running on would be a leak with no purpose.
    if (HMODULE d3d9 = GetModuleHandleW(L"d3d9.dll")) {
      s_fn = reinterpret_cast<PFN_dusklightSetDrawMeta>(
          reinterpret_cast<void*>(GetProcAddress(d3d9, "dusklightSetDrawMeta")));
    }
    Log.info(s_fn != nullptr
                 ? "dx9.drawmeta: the Remix fork exports dusklightSetDrawMeta; per-draw metadata is live"
                 : "dx9.drawmeta: no dusklightSetDrawMeta export in this d3d9.dll, so per-draw metadata is "
                   "inert and every draw behaves as before. Expected against stock Remix or an older fork.");
  }

  return s_fn;
}

}  // namespace

void set_dusklight_draw_meta(uint32_t flags) noexcept {
  const PFN_dusklightSetDrawMeta fn = resolve_draw_meta_export();

  if (fn == nullptr) {
    return;
  }

  const DusklightDrawMetaWire meta { static_cast<uint32_t>(sizeof(DusklightDrawMetaWire)), flags };
  fn(&meta, static_cast<uint32_t>(sizeof(meta)));
}

void set_dusklight_water(uint32_t role, uint32_t tag, uint32_t layer) noexcept {
  // Hop 2 of 3, reported once per role. The three hops a water mark has to survive are
  // game -> FIFO (dusk.matname, game log), FIFO -> backend (here), and backend -> Remix
  // (dusklight.water, Remix log). Two rounds were spent unable to tell which of the three
  // was dropping it, because only the ends were visible; this is the middle.
  static bool s_reportedSurface = false;
  static bool s_reportedProjected = false;
  if (role == GX_AURORA_DUSKLIGHT_WATER_SURFACE && !s_reportedSurface) {
    s_reportedSurface = true;
    Log.info("dx9.water: first SURFACE mark decoded from the FIFO");
  } else if (role == GX_AURORA_DUSKLIGHT_WATER_PROJECTED && !s_reportedProjected) {
    s_reportedProjected = true;
    Log.info("dx9.water: first PROJECTED mark decoded from the FIFO");
  }
  g_dusklightWaterRole = role;
  g_dusklightWaterTag = tag;
  g_dusklightWaterLayer = layer;
}

void copy_tex(const void* dest, bool clear) noexcept {
  if (g_dx9.dev == nullptr) {
    return;
  }

  const auto srcRect = gx::map_logical_scissor(g_gxState.texCopySrc);
  const bool colorCopy = !gx::is_depth_format(g_gxState.texCopyFmt);
  bool copied = false;
  if (colorCopy && srcRect.width > 0 && srcRect.height > 0) {
    uint32_t dstWidth = 0;
    uint32_t dstHeight = 0;
    scale_copy_dst(dstWidth, dstHeight);
    if (IDirect3DTexture9* dst = texture_get_copy_target(dest, dstWidth, dstHeight)) {
      IDirect3DSurface9* srcSurface = nullptr;
      IDirect3DSurface9* dstSurface = nullptr;
      g_dx9.dev->GetRenderTarget(0, &srcSurface);
      dst->GetSurfaceLevel(0, &dstSurface);
      if (srcSurface != nullptr && dstSurface != nullptr) {
        const auto maxW = static_cast<LONG>(current_target_width());
        const auto maxH = static_cast<LONG>(current_target_height());
        RECT src;
        src.left = std::clamp<LONG>(srcRect.x, 0, maxW) + target_offset_x();
        src.top = std::clamp<LONG>(srcRect.y, 0, maxH) + target_offset_y();
        src.right = std::clamp<LONG>(srcRect.x + srcRect.width, 0, maxW) + target_offset_x();
        src.bottom = std::clamp<LONG>(srcRect.y + srcRect.height, 0, maxH) + target_offset_y();
        if (src.right > src.left && src.bottom > src.top) {
          copied = SUCCEEDED(g_dx9.dev->StretchRect(srcSurface, &src, dstSurface, nullptr, D3DTEXF_LINEAR));
          if (!copied) {
            warn_once(0xA000, "copy_tex: StretchRect failed");
          }
        }
      }
      if (dstSurface != nullptr) {
        dstSurface->Release();
      }
      if (srcSurface != nullptr) {
        srcSurface->Release();
      }
    }
  }
  if (!copied) {
    // Depth copies and failures keep a neutral placeholder bound.
    texture_register_copy_placeholder(dest);
  }

  if (clear) {
    // Honor the clear semantics on the current target, scoped to the copy
    // source rect.
    D3DRECT d3dRect{srcRect.x + target_offset_x(), srcRect.y + target_offset_y(),
                    srcRect.x + srcRect.width + target_offset_x(),
                    srcRect.y + srcRect.height + target_offset_y()};
    DWORD flags = 0;
    if (g_gxState.colorUpdate || g_gxState.alphaUpdate) {
      flags |= D3DCLEAR_TARGET;
    }
    if (g_gxState.depthUpdate) {
      flags |= D3DCLEAR_ZBUFFER;
    }
    if (flags != 0) {
      const auto& c = g_gxState.clearColor;
      g_dx9.dev->Clear(1, &d3dRect, flags, D3DCOLOR_COLORVALUE(c[0], c[1], c[2], c[3]), 1.0f, 0);
    }
  }
}

void begin_offscreen(uint32_t width, uint32_t height) noexcept {
  if (g_dx9.dev == nullptr || g_dx9.inOffscreen) {
    return;
  }
  OffscreenTarget* target = texture_get_offscreen(std::max(width, 1u), std::max(height, 1u));
  if (target == nullptr) {
    return;
  }
  g_dx9.dev->SetRenderTarget(0, target->colorSurface);
  g_dx9.dev->SetDepthStencilSurface(target->depth);
  g_dx9.inOffscreen = true;
  g_dx9.offscreenWidth = target->width;
  g_dx9.offscreenHeight = target->height;

  // Fresh pass: clear and reset viewport/scissor to the full target (the
  // wgpu path does the same; the game then sets its own).
  g_dx9.dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);
  D3DVIEWPORT9 vp{0, 0, target->width, target->height, 0.f, 1.f};
  g_dx9.dev->SetViewport(&vp);
  RECT scissor{0, 0, static_cast<LONG>(target->width), static_cast<LONG>(target->height)};
  g_dx9.dev->SetScissorRect(&scissor);
}

void end_offscreen() noexcept {
  if (g_dx9.dev == nullptr || !g_dx9.inOffscreen) {
    return;
  }
  g_dx9.dev->SetRenderTarget(0, s_backbufferColor);
  g_dx9.dev->SetDepthStencilSurface(s_backbufferDepth);
  g_dx9.inOffscreen = false;
  g_dx9.offscreenWidth = 0;
  g_dx9.offscreenHeight = 0;
  // Restore the EFB viewport/scissor state.
  set_render_viewport();
  set_render_scissor();
}

bool in_offscreen() noexcept { return g_dx9.inOffscreen; }

// ---------------------------------------------------------------------------
// Skinning extension
// ---------------------------------------------------------------------------

void set_skinning(const void* palette, uint32_t jointCount, const void* influences, uint32_t vtxCount,
                  uint32_t influenceCount) noexcept {
  g_skin.palette = static_cast<const float*>(palette);
  g_skin.influences = static_cast<const uint8_t*>(influences);
  g_skin.jointCount = jointCount;
  g_skin.vtxCount = vtxCount;
  g_skin.influenceCount = std::min<uint32_t>(influenceCount, 4);
}

void clear_skinning() noexcept { g_skin = {}; }

void set_camera_view(const float* mtx3x4) noexcept {
  const D3DMATRIX view = to_d3d_3x4(mtx3x4);
  D3DMATRIX inv;
  if (mtx_affine_inverse(view, inv)) {
    g_camera.view = view;
    g_camera.viewInv = inv;
    g_camera.valid = true;
  } else {
    warn_once(0x9100, "camera view matrix not invertible; keeping fused WORLD*VIEW");
    g_camera.valid = false;
  }
}

} // namespace aurora::dx9

#endif // AURORA_ENABLE_D3D9
