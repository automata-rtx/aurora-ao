# HD texture packs on the D3D9 backend

**Implemented 2026-08-05. CI-green on both repos. Tested in game 2026-08-06:
worked on the first try.** One known characteristic — a long first-launch
warm-up — is §5.

Dusklight's Dolphin-format replacement packs reach RTX Remix on the D3D9
backend, and **their bytes never travel through D3D9.** The game's own textures
are still what D3D9 uploads and therefore what Remix hashes, so texture tagging,
`rtx.conf` categories and USD bindings behave exactly as with no pack installed.
Remix loads the pack from its own files and substitutes at draw time.

## 1. Why the replacement is never uploaded

**This is the trap, and the code states it where it happens:**
`lib/dx9/dx9_texture.hpp:21-27`, on `resolve_texmap`. Remix hashes the D3D9
texture, and that hash keys its texture tagging grid, every `rtx.conf` category
and every USD binding — so substituting the bytes would silently invalidate all
three across a whole remaster. **If you ever find yourself making aurora upload
replacement pixels, that is the trap.**

Format is the secondary reason: D3D9 takes A8R8G8B8 and DXT1/3/5, so BC7, BC5
and ASTC packs cannot be expressed there at all.

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

### Where a pack author gets the filenames

A pack file is named
`tex1_{w}x{h}_{textureHash:016x}[_{tlutHash:016x}]_{format}.dds`
(`format_replacement_filename`), which carries **no description** — dimensions, a
content hash and a format enum. That convention is Dolphin's, chosen so emulator
packs drop in unchanged, so the answer to "which file is which" is **dump from a
GameCube emulator running the same ISO**. **Verified 2026-08-11 by the owner**,
who ran the dump and confirmed the output was exactly as expected — a tested
route, not a deduction from the format being shared.

Aurora can also write these files itself (`dump_editable_texture_dds`), disabled
by a hardcoded `allowTextureDumps = false` in dusklight and never wired to a
setting, because the emulator route covers it.

## 3. Two substitution sites, because Remix splits the frame

**A UI draw never reaches material resolution at all** — `makeDrawCallType`
returns `{Rasterized, true}`, `internalPrepareDraw` returns
`PreserveDrawCallAndItsState`, and the draw samples whatever `BindTexture` bound.
No material of any kind is consulted. So substituting only in
`determineMaterialData` would leave the **HUD** — one of the two things that must
still rasterize correctly — at the game's own resolution, which is the single
biggest thing a pack is wanted for. Hence the second site, in
`d3d9_device.cpp`, **the only place this fork touches that file**: a rebase that
drops it loses the HUD half silently while the world half keeps working.

Classification is not aurora's problem, deliberately: the fork already decides
per draw, before either site runs. A texture used in both a HUD and a world draw
needs no decision — whichever site the draw reaches finds the same handle.

**`Ambient.b` carries the stage the index refers to.** Only *dirty* textures
rebind, so a multi-texture draw can rebind a stage this material's index says
nothing about; without the stage check that bind would take the albedo's
replacement, which is the wrong texture rather than a missing one. The
consequence is that on the rasterized path only the albedo stage is
substituted — a missed improvement on multi-texture UI draws, not a wrong pixel.

## 4. Pack rules, and failure signatures

**It does not cost hash stability.** Aurora's upload path is untouched: pack on
and pack off produce the same `tex0hash` values for the same scene, which is
also the cheapest regression check. Nothing new enters the categorization list
either, since no D3D9 texture is created for a replacement.

| Rule | Why |
| :-- | :-- |
| `.dds` only | Remix's asset loader accepts nothing else; PNG entries are skipped with a bounded log |
| Ship mips | rasterized draws generate no sampler feedback; `forceFullMips` compensates but a single-mip large texture still warns |
| BC1–BC5, BC7 are fine | loaded by Remix, not by D3D9 |
| ASTC is unverified | the adapter format gate almost certainly rejects it on desktop GPUs. Do not design around it |
| No `$` TLUT wildcards on animated-palette art | one file would collapse every palette phase onto one image |

| Failure | Looks like | How to tell |
| :-- | :-- | :-- |
| Game never handed the pack over | pack does nothing | overlay: `N selected, 0 handed over`. If N is 0, the directory is empty or nothing parsed |
| Fork ignored it | pack does nothing | handed over > 0 but `0 draws tagged` — the D3D9 stream is not carrying the index, i.e. an aurora older than the fork |
| Wrong stage indexed | a multi-texture surface sharpens the wrong layer | `matrep.sum texrep=` versus which texture that line calls the albedo |
| Still streaming | surfaces sharpen a moment after appearing | `still loading` in the overlay. By design; binding an empty slot renders black |
| Preserve-path latch | a room stays low-res for its whole life, sharpens after a reload | what the `awaitingReplacement` guard exists to prevent; if seen, that guard is not firing |
| HUD soft while the world sharpens | — | `texrep.applyToRaster` off, or the HUD draw is multi-texture and the pack replaces a non-albedo stage |
| Tagging broken | a texture loses its grid entry, or a category stops working | **should be impossible.** `tex0hash=` differing between pack-on and pack-off means aurora's upload path was changed |

**Known gap, not addressed:** with `TerrainBaker::enableBaking()` on, terrain
bakes the legacy texture — the baker consults `getReplacementMaterial` only.
Signature: terrain stays low-res while everything else sharpens.

**Two rejected designs worth not re-proposing.** Returning external materials
from `getReplacementMaterial` puts them through `mergeLegacyMaterial`, and API
materials are built with all dirty flags clear, so the merge copies a
default-constructed `OpaqueMaterialData` over every field and erases the
high-res texture — it would look exactly like "the replacement did not load".
Submitting the geometry through `remixapi_CreateMesh`/`DrawInstance` loses
tagging entirely and loses the ramp and the whole self-illumination rule, since
both read `D3DMATERIAL9` and the API's instance structs have no field for it;
every lava and fire surface would stop glowing, silently.

**What the 2026-08-06 test did not cover**, and so is still only
reasoned-about: a BC7/BC5 pack, a PNG entry actually being skipped, the
device-loss path, a multi-texture UI draw, and `$` TLUT wildcards on
palette-animated art. Also unknown: whether `m_extMaterials` survives a D3D9
device recreation — nothing in either repo establishes it, so the game re-creates
on device change rather than assume.

## 5. The first-launch warm-up (expected, not a defect)

**This section is the single home for it; other documents point here.**

**Observed 2026-08-06.** With a pack installed, the first launch spends a long
period at poor performance before the replacements appear; every later launch has
them essentially immediately.

**Two candidate contributors, and the first explanation written was wrong in a
way worth remembering.** It reasoned from Remix keeping no on-disk cache of
loaded *textures* (verified: `findAsset` reopens the `.dds` every launch and the
only dedupe map dies with the process) to there being no durable cache at all.
There is one: **DXVK writes a pipeline state cache to disk**, on by default, and
Remix's own options document the first-load compilation cost that goes with it.
**Every** first launch is slow for that reason, pack or no pack.

| Contributor | Cached where | Survives a reboot? |
| :-- | :-- | :-- |
| Pipeline/shader compilation | `<exe>.dxvk-cache` on disk | **Yes** — genuinely one-time |
| Our `.dds` reads | nowhere; re-read every launch | **No** — only the volatile OS page cache |

**Which dominates is unmeasured**, and the texture side is partly a *symptom*:
creation is budgeted per frame, so slow frames from any cause stretch how long
the pack takes to finish arriving. "Textures appear late" is not evidence that
textures are what is slow.

**The experiment that separates them costs one reboot** — it clears the OS page
cache and keeps `.dxvk-cache`. Slow again ⇒ texture I/O dominates; fast ⇒ shader
compilation dominated. Measure with the wall-clock gap between the game's
`texrep: N replacement(s) selected` and `texrep: N material(s) created` lines,
cold launch versus warm.

**Our own contribution is real and in our hands:** 16 materials per frame, each
reading its DDS header **synchronously on Remix's CS thread** (both
`preloadTextureAsset` branches pass `async=false`, the `// async load` comment
above the call notwithstanding). Sixteen cold-cache file opens stall that frame,
for `entries / 16` frames.

**If it wants fixing — and only if the reboot test says I/O dominates:** a time
budget instead of a count bounds the *stall* rather than the *count* but does not
reduce total work; a background thread that reads each `.dds` start to finish
populates the OS page cache without touching the CS thread, and sequential reads
are far cheaper than the scattered ones `MapViewOfFile` produces during upload.
**Neither was done, and both would re-open this feature's "tested" claim.**
