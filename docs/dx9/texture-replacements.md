# HD texture packs on the D3D9 backend

**Implemented 2026-08-05. CI-green on both repos. Tested in game 2026-08-06:
worked on the first try.** One known characteristic — a long first-launch
warm-up — is §9.

Dusklight's Dolphin-format replacement packs now reach RTX Remix on the D3D9
backend. **Their bytes never travel through D3D9.** The game's own textures are
still what D3D9 uploads and therefore what Remix hashes, so texture tagging,
`rtx.conf` categories and USD bindings behave exactly as they do with no pack
installed. The pack is loaded by Remix from its own files and substituted at
draw time.

This replaces an earlier revision of this document that proposed substituting
the replacement bytes inside aurora at D3D9 texture creation. That design worked
but cost every texture hash in the game; §7 says why it was dropped.

---

## 1. Why not just upload the replacement to D3D9

Two reasons, one fatal.

**Format.** D3D9 takes A8R8G8B8, DXT1/3/5 and little else. Packs using BC7, BC5
or ASTC cannot be expressed at all.

**Tagging, and this is the fatal one.** Remix hashes subresource 0 of every
D3D9 texture (`d3d9_common_texture.cpp:666-689`) and feeds the result to
`ImGUI::AddTexture`, which is the *sole* writer of `g_imguiTextureMap`
(`dxvk_imgui.cpp:651`) — the map the texture categorization grid iterates
(`:2360`). The same hash is what `rtx.conf` category lists and USD material
bindings are keyed on, because `LegacyMaterialData::updateCachedHash()` is
literally `m_cachedHash = colorTextures[0].getImageHash()`
(`rtx_materials.h:1869`).

Change the bytes and every one of those moves. Installing a pack would silently
invalidate a remaster; editing one texture in a pack would silently invalidate
that material. Keeping the original in D3D9 is not a compromise here — it is the
only arrangement in which the tags mean anything stable.

## 2. The mechanism

```
aurora, load time    every file-backed replacement gets a dense 1-based index
                     (ReplacementEntry::remixIndex, assigned at registration)

game, first frames   enumerate_selected_replacements() -> for each entry:
                       remixapi_CreateMaterial{ hash  = 0xD05C<<48 | index,
                                                albedoTexture = <absolute .dds>,
                                                pNext = MaterialInfoOpaqueEXT }
                     16 per frame; the fork reads the DDS header synchronously
                     on its CS thread, so a whole pack in one frame is a hitch

aurora, per draw     resolve_texmap() reports the index for the texture it just
                     resolved; the lowest bound stage wins
                     -> D3DMATERIAL9::Ambient.g = index, Ambient.b = its stage

fork, per draw
  ray-traced         determineMaterialData(): overwrite AlbedoOpacityTexture on
                     the already-converted legacy material
  rasterized (HUD)   D3D9DeviceEx::BindTexture(): bind the loaded view instead
                     of the game's own
```

**`remixapi_CreateMaterial` is used purely as a file loader.** Its material is
never bound as a material. It is the only tested path in this runtime from a
file path to a resident `TextureRef`, it already carries the `.dds`-only gate,
and its handle is caller-chosen (`info->hash` verbatim), so we can namespace it.

**Why an index rather than a hash join.** Aurora would otherwise have to
reproduce Remix's `XXH3` over subresource 0, which depends on Remix's mip
packing, our `LockRect` pitch, and `rtx.useObsoleteHashOnTextureUpload` staying
false. None of those is a contract. The index is a number aurora owns.

## 3. Two substitution sites, because Remix splits the frame

This is the part that is easy to get wrong. **A UI draw never reaches material
resolution at all.** `makeDrawCallType` returns `{Rasterized, true}` for it
(`d3d9_rtx.cpp:519-524`), `internalPrepareDraw` returns
`PreserveDrawCallAndItsState`, and the draw then samples the view that
`BindTexture` bound. No material of any kind is consulted.

So substituting only in `determineMaterialData` would leave the **HUD** — one of
the two things that still has to rasterize correctly — at the game's own
resolution, which is the single biggest thing a pack is wanted for. Hence the
second site.

Classification is not aurora's problem and deliberately so: the fork already
decides, per draw, before either site runs. A texture used in both a HUD draw
and a world draw needs no decision — whichever site the draw reaches finds the
same handle.

**`Ambient.b` carries the stage the index refers to.** Only *dirty* textures
rebind, so a multi-texture draw can rebind a stage this material's index says
nothing about; without the stage check that bind would take the albedo's
replacement, which is the wrong texture rather than a missing one. The
consequence is that on the rasterized path only the albedo stage is
substituted — a missed improvement on multi-texture UI draws, not a wrong pixel.

## 4. What this costs, and what it does not

**It does not cost hash stability.** Aurora's upload path is untouched. Pack on
and pack off produce the same `tex0hash` values for the same scene, which is
also the cheapest way to check this has not regressed.

**It does not grow the categorization list.** No D3D9 texture is created for a
replacement, so nothing new can enter `g_imguiTextureMap`. The list is the same
length it is today, holding the same textures.

**It does cost `.dds`.** Remix's asset loader rejects anything else
(`rtx_asset_data_manager.cpp:578-591`) while aurora's registry accepts `.png`
too. PNG entries are skipped with a bounded log rather than failing quietly.

**Pack rules that follow:**

| Rule | Why |
| :-- | :-- |
| `.dds` only | Remix's loader accepts nothing else |
| Ship mips | rasterized draws generate no sampler feedback; `forceFullMips` is on by default to compensate, but a single-mip large texture still warns |
| BC1/BC2/BC3/BC4/BC5/BC7 are fine | loaded by Remix, not by D3D9 |
| ASTC is unverified | the adapter format gate almost certainly rejects it on desktop GPUs. Do not design around it |
| No `$` TLUT wildcards on animated-palette art | one file would collapse every palette phase onto one image |

## 5. Where the code is

| Repo | File | What |
| :-- | :-- | :-- |
| aurora | `include/aurora/texture.hpp` | `ReplacementDescriptor`, `enumerate_selected_replacements()` |
| aurora | `lib/gfx/texture_replacement.{hpp,cpp}` | `remixIndex` on each entry; `find_replacement_index()`, which resolves the key without decoding a file or touching a GPU |
| aurora | `lib/dx9/dx9_texture.cpp` | `ContentEntry::remixIndex`, resolved once per distinct content; `resolve_texmap()` out-param |
| aurora | `lib/dx9/dx9_tev.cpp` | records the lowest bound stage's index; `matrep.sum texrep=` |
| aurora | `lib/dx9/dx9_internal.hpp` | `Ambient.g` = index, `Ambient.b` = stage |
| dusklight | `src/dusk/remix_bridge.cpp` | `updateTextureReplacements()`, the creation budget, `env.texrep*` readouts |
| dusklight | `src/dusk/settings.*` | `game.remixTextureReplacements`, launch-only |
| fork | `rtx_dusklight_texrep.{h,cpp}` | handle decode, residency, both substitutions, counters. Also listed in `src/dxvk/meson.build` |
| fork | `rtx_scene_manager.cpp` | ray-traced site, the preserve-path guard, the frame-end counter roll |
| fork | `d3d9_device.cpp` | rasterized site — the **only** place this fork touches that file |
| fork | `rtx_dusklight_env.h` | the four `env.texrep*` readouts the game pushes |
| fork | `dxvk_imgui.cpp` | the HD Texture Pack overlay section, and `kRequiredProtocol` |

**Protocol 7.** The game and the fork are a single versioned protocol; build
both from the same commit point.

## 6. Failure modes and their signatures

| Failure | What it looks like | How to tell |
| :-- | :-- | :-- |
| Game never handed the pack over | pack does nothing | overlay: `Game: N selected, 0 handed over`. If N is 0 too, the directory is empty or nothing parsed |
| Fork ignored it | pack does nothing | overlay: handed over > 0 but `0 draws tagged` — the D3D9 stream is not carrying the index, i.e. an aurora older than the fork |
| PNG pack | pack does nothing, log is loud | `texrep: skipping <file> - Remix loads .dds only`, and `texrepSkipped` climbs |
| Wrong stage indexed | a multi-texture surface sharpens the wrong layer | `matrep.sum texrep=` versus which texture the same line reports as the albedo |
| Still streaming | surfaces sharpen a moment after appearing | `still loading` in the overlay. By design — the alternative is binding an empty slot, which renders black |
| Preserve-path latch | a room stays low-res for its whole life, sharpens after a reload | this is what the `awaitingReplacement` guard exists to prevent; if seen, that guard is not firing |
| HUD unchanged while the world sharpens | HUD soft | `rtx.dusklight.texrep.applyToRaster` off, or the HUD draw is multi-texture and the pack replaces a non-albedo stage |
| Tagging broken | a texture loses its grid entry, or an `rtx.conf` category stops working | **should be impossible here.** `tex0hash=` differing between pack-on and pack-off means aurora's upload path was changed |
| Launch hitch | stutter proportional to pack size | `handed over` climbing across frames is intended; a single-frame jump means the budget is not applied |

**Known gap, not addressed:** with `TerrainBaker::enableBaking()` on, terrain
bakes the legacy texture — the baker consults `getReplacementMaterial` only.
Signature: terrain stays low-res while everything else sharpens.

## 7. Rejected alternatives

- **Substitute the replacement bytes into D3D9** (the earlier revision of this
  document). Simpler, and it is the only way to sharpen a *non-albedo* HUD
  stage — but it re-keys every texture hash in the game, so installing or
  editing a pack silently invalidates every tag, category and USD binding.
  Also cannot carry BC7/BC5/ASTC.
- **Return external materials from `getReplacementMaterial`.**
  `determineMaterialData` would then call `mergeLegacyMaterial` on them, and API
  materials are built with all dirty flags clear — the merge copies a
  default-constructed `OpaqueMaterialData` over every field, erasing the
  high-res texture. It would look exactly like "the replacement did not load".
  The one-field swap after `as<OpaqueMaterialData>()` also preserves the sampler
  override and the ignore-alpha flag, both of which the USD replacement path
  drops.
- **Submit the geometry through `remixapi_CreateMesh`/`DrawInstance`.** Loses
  tagging entirely (`setupCategoriesForTexture` is never called on that path),
  and loses the two-colour ramp and the whole self-illumination rule, because
  both read `D3DMATERIAL9` and the API's instance structs have no field for it.
  Every lava and fire surface would stop glowing, silently.
- **A 1×1 or synthetic D3D9 proxy, to keep the original out of memory.**
  Byte-identical proxies collapse onto one hash, so many textures become one
  grid entry; per-texel alpha is destroyed, and the D3D9 alpha test compares it;
  and for UI draws the proxy is literally what the player sees.
- **Splitting policy in aurora on `g_gxState.projType`.** Aurora's texture cache
  is content-keyed across draws, so a texture used in both 2D and 3D would have
  to be built twice under two keys, and UI hashes would churn with the pack.

## 8. Verified / inferred / unknown

**Verified in source:** the tagging chain and that it runs before any material
work; that `getReplacementMaterial` cannot reach `m_extMaterials`; that a UI
draw consults no material; that `remixapi_MaterialInfo` takes paths only, that
only `.dds` is accepted, that the read is synchronous on the CS thread, that the
handle is `info->hash` verbatim with 0 rejected, and that re-registering a
handle is ignored rather than an update; that `topath` copies the path
synchronously, so the caller's wide string need not outlive the call; that
`makePreloadSource` drops every texture path unless a material EXT is chained;
that `ManagedTexture::requestMips` is atomic and `MAX_MIPS` is 32.

Also verified: `LegacyMaterialData::computeIdentityHash()`
(`rtx_materials.cpp:40-120`) hashes `Ambient.r` but **not** `.g` or `.b`. That
is why the preserve path needs an explicit guard — a stable instance would
otherwise latch its pre-residency material forever. Leaving the index out of
that hash is nonetheless safe: `colorTextureHash0` is in it, and the index is a
function of the same texture content, so it carries no information the hash does
not already have. Nothing else in the fork reads `Ambient.g` or `Ambient.b`;
both were unwritten before this change.

**Inferred, not measured:** that the lowest bound D3D9 stage is always the one
the fork assigns `colorTextures[0]`; that `tryRequestMips` is sufficient to hold
a raster-substituted texture at full resolution against the streamer's own
recomputation; that a Dolphin-format pack replaces the GX *source* texture and
so should still be modulated by the TEV op, tFactor and the ramp.

**Unknown:** whether `m_extMaterials` survives a D3D9 device recreation —
nothing in either repo establishes it, so the game re-creates on device change
rather than assume; whether ASTC-in-DDS is accepted anywhere in the chain.

**Tested in game 2026-08-06 and it worked first try.** The aurora half is also
syntax-checked in both the d3d9-on and d3d9-off configs, and both repos are
CI-green.

What the test did **not** cover, and so is still only reasoned-about: a pack
containing BC7/BC5 (the format argument for this design is read from source, not
demonstrated); a PNG entry actually being skipped and logged; the device-loss
path (resize → materials re-created); a multi-texture UI draw, where only the
albedo stage is substituted; and palette-animated art with a `$` TLUT wildcard.

## 9. The first-launch warm-up (expected, not a defect)

**Observed 2026-08-06.** With a pack installed, the first launch spends a long
period at poor performance before the replacements appear. Every later launch
has them essentially immediately.

**Remix keeps no on-disk cache of loaded textures** — verified:
`AssetDataManager::findAsset` opens the `.dds` with `std::fopen` for the header
and `CreateFileMapping`/`MapViewOfFile` for the data on every launch
(`rtx_asset_data_manager.cpp:200,294-305`), and the only dedupe,
`m_assetHashToTextures` (`rtx_texture_manager.cpp:1435-1448`), dies with the
process. So nothing in the runtime is warm on launch 2 that was cold on
launch 1.

**The difference is therefore the OS file cache.** *Inference from the absence
of any other mechanism, not a measurement.* It fits the shape: `MapViewOfFile`
faults pages in lazily during upload, so a cold cache spreads its cost over a
long period rather than into one stall.

**Our own contribution is real and is in our hands.** The game creates
`kTexRepCreationsPerFrame = 16` materials per frame, and each
`remixapi_CreateMaterial` reads the DDS header **synchronously on Remix's CS
thread** — both `preloadTextureAsset` branches pass `async=false`, the
`// async load` comment above the call notwithstanding. Sixteen cold-cache file
opens per frame stalls that frame, for as many frames as `entries / 16`.

**Measure it rather than argue about it.** The game logs
`texrep: N replacement(s) selected by the registry` at the start of the pass and
`texrep: N material(s) created, M skipped` at the end. The wall-clock gap
between those two lines, cold launch versus warm launch, is exactly this cost.

**If it wants fixing:** make the budget time-based rather than count-based —
spend a fixed millisecond budget per frame instead of a fixed count — so a cold
cache stretches the ramp instead of stretching each frame. Deliberately not done
as part of the 2026-08-06 tested change; doing it later re-opens the "tested"
claim for this feature and nothing else.
