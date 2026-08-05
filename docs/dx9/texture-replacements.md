# HD texture packs on the D3D9 backend

**Status: investigation, no code written.** Everything below is read out of the
three repos at the commits named at the end. Where a claim is inference rather
than something read, it says so.

**Question asked:** can Dusklight's existing high-res texture replacements be
fed through the fixed-function D3D9 stream so Remix picks them up, without the
original low-res textures also reaching Remix's texture categorization list?

**Answer: yes.** The registry half already runs in D3D9 mode; only the
consumption half is WebGPU-bound, and the seam to cut at is one function call
above the wgpu upload. Because the substitution happens where the D3D9 texture
is *created*, no `IDirect3DTexture9` is ever made for a replaced original, so
Remix never hashes one and the categorization list does not grow at all.

This document supersedes the dismissal in
[`unsupported-effects.md`](unsupported-effects.md) #17 and
[`architecture-notes.md`](architecture-notes.md) §Textures, both of which said
aurora-side packs "only matter to the standalone image". That is right for
path-traced surfaces and **wrong for the HUD** — see §2.

---

## 1. What already works, and what does not

The replacement system splits cleanly into a **registry** (which file replaces
which GX texture) and a **consumer** (turn that file into a GPU texture). Only
the consumer is tied to WebGPU.

| Half | Where | Runs in D3D9 mode today? |
| :-- | :-- | :-- |
| Directory scan, filename parsing, key building, priority/wildcard resolution, LRU budget | `lib/gfx/texture_replacement.cpp` | **Yes.** Pure CPU; touches no wgpu symbol. |
| Registration from the game | dusklight `src/dusk/texture_replacements.cpp` → `aurora::texture::load_replacement_directory` | **Yes.** `dusk::texture_replacements::reload()` is called at `src/m_Do/m_Do_main.cpp:701`, unconditionally once a backend other than `BACKEND_NULL` came up. |
| Registration from mods | dusklight `src/dusk/mods/svc/texture.cpp:180,199` → `register_virtual_replacement` | **No — but not for a texture reason.** The mechanism is registry-only and would work; mod *discovery* is skipped wholesale on this backend (`m_Do_main.cpp:901`), so nothing ever calls it. See §5.5. |
| Decode the file to CPU pixels | `load_file_replacement` / `load_virtual_replacement` (`texture_replacement.cpp:726-732`) → `gfx::ConvertedTexture` | **Yes.** DDS/PNG decode, no device involved. |
| Upload to a GPU texture | `create_converted_texture_handle` (`texture_replacement.cpp:738`) → `g_device.CreateTexture` | **No.** `g_device` is never initialized in D3D9 mode. |

So the registry is populated in D3D9 mode right now — from the user directory
`<ConfigPath>/texture_replacements/`, which is the route an HD pack takes — and
is simply never asked.
The only callers of `find_replacement()` are `resolve_static_texture` and
`resolve_static_palette_texture` in `lib/gx/gx.cpp:177,212`, both on the wgpu
path. `lib/dx9/dx9_texture.cpp` goes straight from the GX source bytes to
`convert_texture()` and never consults the registry.

**The seam is `ConvertedTexture`** (`lib/gfx/texture_convert.hpp`): format,
width, height, mip count, and one packed byte blob. That is precisely what
`dx9_texture.cpp` already consumes from `convert_texture()`. The public
`find_replacement()` returns a `gfx::TextureHandle` (a wgpu texture) — one step
too far.

## 2. Why this is worth doing even though "Remix replaces textures anyway"

Two reasons. The first is the one the existing docs missed.

### 2a. The HUD is rasterized, so Remix's replacement system never touches it

`D3D9Rtx::isRenderingUI()` (`src/d3d9/d3d9_rtx.cpp:561`) classifies a draw as UI
when the projection is orthographic with z-write off — `rtx.orthographicIsUI`
defaults to `true` (`src/d3d9/d3d9_rtx.h:40`). Dusklight's HUD is exactly that,
which is why the camera split is already skipped for ortho draws (progress §3.13).

UI draws are rasterized on top of the traced image; they never reach
`getReplacementMaterial` (`rtx_scene_manager.cpp:791`), which is on the
path-traced material path. **So a Remix USD mod cannot sharpen the HUD.** The
pixels Remix rasterizes are the pixels we put in the D3D9 texture, and nothing
else. Feeding the HD pack through D3D9 is the only lever there is.

A useful side effect: because the classification is *projection*-based and not
hash-based, changing HUD texture contents does not disturb UI detection. (The
hash-keyed `rtx.uiTextures` set is the other route into `isRenderingUI` and we
do not rely on it.)

### 2b. For path-traced surfaces it removes albedo from the authoring job

`getReplacementMaterial` is keyed on `LegacyMaterialData::getHash()`, and that
hash is literally the stage-0 texture's content hash — `rtx_materials.h:1869`:

```cpp
void updateCachedHash() {
  m_cachedHash = colorTextures[0].getImageHash();
}
```

So if the D3D9 texture already carries HD albedo, a Remix material replacement
for that hash only needs to supply normal / roughness / metalness. That is the
stated goal, and it follows directly.

## 3. Keeping the originals out of Remix's categorization list

The concern is correct and this project has already been bitten by the same
mechanism (progress §3.5: "VRAM climb past 32 GB and the categorization list
flicker"). The chain, read end to end:

1. `D3D9CommonTexture::SetupForRtxFrom` (`src/d3d9/d3d9_common_texture.cpp:666-690`)
   hashes **subresource 0 only**, `XXH3_64bits(buffer->mapPtr(0), buffer->info().size)`.
2. It then calls `ImGUI::AddTexture(imageHash, …)` (`:688`).
3. `ImGUI::AddTexture` (`src/dxvk/imgui/dxvk_imgui.cpp:651`) inserts into
   `g_imguiTextureMap` (`:171`) — one entry per distinct hash.
4. `showTextureSelectionGrid` (`:2326`) iterates that map to build the
   categorization grid, and it is not free at scale: when it runs out of
   `VkDescriptorPoolCreateInfo::maxSets` it logs and **truncates the list**
   (`:2412`).

An entry is removed only by `ClearHash()`, i.e. when the D3D9 texture dies.

**Therefore: substitute at creation, not after.** If `resolve_texmap` builds the
D3D9 texture from the replacement pixels instead of the GX pixels, no D3D9
texture object exists for the original, so steps 1-4 never run for it. The list
holds exactly the same *number* of entries as today; only the contents and the
hashes change. Nothing extra to suppress.

Note the scope of "not in memory": the GX source bytes stay in the game's own
asset heap, because they are the key the replacement is looked up by. They are
never uploaded, never hashed by Remix, and never occupy VRAM.

## 4. Shape of the change

Two files, plus one invalidation hook.

**1. `lib/gfx/texture_replacement.{hpp,cpp}` — add a backend-neutral accessor.**
Something like `find_replacement_pixels(obj[, tlut])` returning the decoded CPU
blob. It reuses `find_source_replacement_key_locked`, `select_entry`, the
wildcard rules and the LRU budget unchanged; only the terminal step differs
(`create_converted_texture_handle` → return the bytes).

Returning `gfx::ConvertedTexture` directly is fine, despite it carrying a
`wgpu::TextureFormat`. No file under `lib/dx9/` mentions wgpu in its own source,
but every one of them already includes the Dawn headers transitively:
`dx9_internal.hpp` → `lib/gx/gx.hpp` → `lib/gfx/common.hpp` →
`<webgpu/webgpu_cpp.h>`. So this adds no coupling that is not there today, and
the accessor can hand back the loader's own struct.

*(One harness caveat: the MinGW syntax check shims Dawn, so whether a
`wgpu::TextureFormat` switch in `dx9_texture.cpp` compiles under it depends on
how complete that shim's enum is. A real build is unaffected.)*

The cache in `s_cacheByKey` currently stores a `TextureHandle`; for the D3D9
path it would have to hold the CPU blob instead. Two backends never run at once,
so a variant or a mode switch is enough — but this is the one part of the change
that is not purely additive.

**2. `lib/dx9/dx9_texture.cpp` — consult it in `resolve_texmap` (`:475`).**
After the `texObjId` fast path and after `ContentKey` is computed
(`make_content_key`, `:166`), before `build_static` / `build_palette` (`:303`,
`:322`). On a hit, create the D3D9 texture from the *replacement's* dims, mip
count and format; on a miss, exactly today's behaviour.

Memoize the decision on `ContentKey`, so the lookup runs once per distinct
content rather than once per draw. `ContentKey` hashes the whole source mip
chain, which subsumes the base level the replacement key is built from, so equal
`ContentKey`s mean the same replacement decision — to exactly the extent that
the existing content store is already trusted not to collide. No new risk, but
it is a hash, not a byte comparison. (This does not make the *first* lookup
free; see §5.4.)

**3. Cache invalidation.** Every registry mutation calls
`clear_static_texture_cache()` (`texture_replacement.cpp:1036,1074,1102,1128,1134,1169`),
which sets a flag consumed **only** in `lib/gx/gx.cpp:163,197` — the wgpu path.
Without a D3D9 equivalent, the graphics-tuner toggle
(`dusk/ui/graphics_tuner.cpp:102` → `texture_replacements::set_enabled`) and mod
load/unload silently do nothing on d3d9. `gx.cpp:414` already calls into
`dx9::on_evict_copy_texture`, so there is an established pattern for the hook.

Whether a runtime toggle is even *desirable* is a separate question — see §5.1.

## 5. What it costs

### 5.1 Hash churn is the real price, and it is not small

Because Remix's material hash *is* the bytes we upload (§2b), and because
`rtx.conf` category lists (`rtx.uiTextures`, `rtx.ignoreTextures`, …) are keyed
the same way, **every USD material binding and every hash-keyed rtx.conf entry
is a function of the pack's file contents.** Enabling the pack re-keys the whole
scene once. Editing one pack texture re-keys that one material.

Practical consequence: choose the pack, freeze it, *then* author the Remix
side — and treat the pack as part of the game↔fork protocol rather than as a
user-facing toggle. A settings switch that silently invalidates a remaster is
worse than no switch.

This cuts with the project's "translate, don't tag" rule rather than against it:
we are not adding tags, we are changing what the existing hashes are derived
from. But it is the reason this should not ship as a casual option.

### 5.2 Format coverage

| Loader produces | D3D9 target | Notes |
| :-- | :-- | :-- |
| `RGBA8Unorm` (all PNG, `png_io.cpp:93`; some DDS) | `D3DFMT_A8R8G8B8` | `upload_rgba8` already does the swizzle. |
| `BGRA8Unorm` (DDS) | `D3DFMT_A8R8G8B8` | Already in D3D9 channel order — straight copy. |
| `BC1RGBAUnorm` | `D3DFMT_DXT1` | Direct. |
| `BC3RGBAUnorm` | `D3DFMT_DXT5` | Direct. |
| `BC5RGUnorm`, `BC7RGBAUnorm`, all ASTC (`dds_io.cpp:143-227`) | — | **No D3D9 equivalent.** CPU-decode or reject with a bounded log. |

Dolphin-format HD packs are overwhelmingly PNG, DXT1 and DXT5, so coverage is
good. *(Inference: from the format conventions of Dolphin packs, not measured
against any specific Twilight Princess pack.)*

### 5.3 Memory is not obviously worse and can be better

Every GC texture already reaches D3D9 as 32bpp `D3DFMT_A8R8G8B8`:
`convert_texture` decompresses CMPR to RGBA8 (`texture_convert.cpp:632`) and
`create_from_rgba8` (`dx9_texture.cpp:285`) always creates `A8R8G8B8`. A 256×256
CMPR texture is 32 KB on disc and **256 KB in D3D9 today**. A 512×512 DXT1
replacement is 128 KB — half the current cost at four times the pixels.

Uncompressed packs are the expensive case: 4× linear is 16× the bytes, and it is
charged twice, because `D3DPOOL_MANAGED` maps to
`D3D9_COMMON_TEXTURE_MAP_MODE_BACKED` in the fork
(`src/d3d9/d3d9_common_texture.h:549-557`), which keeps a host-memory buffer
alongside the `VkImage` — and that buffer is what Remix hashes, so it cannot
simply be dropped. **Recommend DDS/BC packs over PNG packs**, and say so
wherever this gets documented for users.

### 5.4 First-touch decode lands on the game thread

`find_replacement` loads and decodes the file synchronously while holding
`s_registryMutex`. On the wgpu path that already happens, but in D3D9 mode
`resolve_texmap` runs inside the FIFO drain between `BeginScene`/`EndScene`, so
a first sighting hitches in a worse place. Not a blocker; worth a preload pass
or an async decode if it shows up.

### 5.5 Mod-supplied replacements would still not load

Mods are disabled wholesale on this backend: `m_Do_main.cpp:901` drops every mod
search directory when the backend is D3D9, because mod graphics stages are inert
without WebGPU and a native mod that touches the renderer crashed the process at
load. So `register_virtual_replacement` never gets called and a texture pack
shipped *inside a mod* does not reach the registry — regardless of anything in
this document.

Only the user directory `<ConfigPath>/texture_replacements/` works on d3d9.
Widening that would mean letting texture-only mods through the discovery gate,
which is a separate change with its own crash-risk argument to make. Worth
knowing before promising pack authors anything.

### 5.6 What this does not fix

Nothing here changes EFB copies, palette-format *dynamic* textures, or anything
else in `unsupported-effects.md`. Replacements are for static GX textures only —
which is what the packs address anyway.

## 6. If it is implemented, what would tell us it worked

Per project rule 4, the instrumentation to add alongside:

- One bounded, deduplicated log line per replaced texture at creation:
  source key (the `tex1_…` name, `build_texture_replacement_name` already
  formats it), replacement dims/format, and the resulting D3D9 format. Capped
  with a truncation notice.
- A one-line summary at the end of the first scene: N textures resolved, M
  replaced, K rejected for unsupported format.
- `matrep.*` already logs texture size and format per material
  ([`material-report.md`](material-report.md)); replaced textures will show
  their new dimensions there, which is the cross-check that the substitution
  reached Remix rather than just the D3D9 cache.

Regression signature if it goes wrong: HUD and world textures render at the
right *size* but wrong *content* (a mis-sized upload reading past the blob), or
Remix's texture list doubles in length (substitution happening after creation
instead of instead of it — the exact failure this design exists to avoid).

---

## Sources

Read at:

| Repo | Commit |
| :-- | :-- |
| `aurora-ao` | `a0f31ba` (`Fixed-Function-dev`) |
| `dusklight-ao` | `8ea5aa1` (`Fixed-Function-dev`) |
| `dxvk-remix` | `3ae3dcd` (`Fixed-Function-dev`) |

Nothing in this document was tested in game, and no code was changed to produce
it.
