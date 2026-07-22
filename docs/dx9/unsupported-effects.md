# Effects beyond fixed-function / SM1 — living list

Per project rules, effects that cannot be reasonably expressed with
fixed-function texture stages (or, at the outside, ps_1_x) are documented here
rather than hacked around. Each entry notes the in-game impact and whether an
RTX-Remix-side change could compensate. Update this file whenever the TEV
mapper logs a new unsupported configuration.

## Confirmed unsupported (by design of D3D9 FF)

| # | Effect / GX feature | Where TP uses it | v1 behavior | Remix-side compensation |
|---|---------------------|------------------|-------------|-------------------------|
| 1 | **Indirect texturing** (`GXSetTevIndirect`, ind stages/matrices) | Heat shimmer, water surface warp, some magic/distortion | Ignored: base stages still draw, no warp | None needed — distortion is post-like; Remix replaces water/heat with PT materials |
| 2 | **Compare-mode TEV ops** (`GX_TEV_COMP_*`) | Occasional masking tricks | Stage collapsed to CURRENT passthrough, logged | Usually cosmetic masks; replace affected material textures in Remix |
| 3 | **TEV output registers REG0-2 as true accumulators** (multi-register programs) | Complex characters/effects (e.g. layered eyes, some sky) | Collapsed to the PREV chain, logged per config | Material replacement in Remix restores intended look |
| 4 | **>8 effective TEV stages** | Rare (J3D TevBlock16 materials) | Truncated at 8, logged | Same as #3 |
| 5 | **Arbitrary TEV swap tables** (non-identity, non-alpha-replicate) | Rare channel shuffles | Ignored unless expressible as ALPHAREPLICATE | Texture-level fix in Remix if ever visible |
| 6 | **Multiple distinct konst colors per draw** without `PERSTAGECONSTANT` cap | Multi-konst UI blends, some materials | First konst → TFACTOR; exact when runtime exposes per-stage constants (dxvk/Remix does) | Runs correct under Remix (dxvk caps) |
| 7 | **EFB copies as texture sources** (`GXCopyTex` consumers) | Real-shadow silhouettes (RGB5A3 4-caster packing), minimap ripple, heat sources, Z-copies | 1x1 black placeholder; offscreen-pass draws skipped | Path-traced shadows replace `shadowReal` entirely (rtx shadows); heat: see #1 |
| 8 | **`GX_TG_SRTG` texgen** (vertex color → texcoord) & **emboss bump** (`GX_TG_BUMP*`) | Emboss-style highlights on a few materials | Texcoord = 0,0; logged | Normal-mapped PBR replacements in Remix |
| 9 | **Fog range adjustment** (`GXSetFogRangeAdj`) + backwards/exp fog exactness | Distance fog tweaks | Plain linear/exp approximation | Remix replaces atmospherics (`rtx.enableFog` etc.) |
| 10 | **Logic-op blending** beyond CLEAR/SET/COPY/NOOP | Very rare on GC titles | Draw falls back to opaque, logged | N/A |
| 11 | **Destination-alpha blend factors** when backbuffer lacks alpha | Some layered effects | Factor swapped to ONE/ZERO approximation, logged | Remix runtime provides A8R8G8B8 — exact there |
| 12 | **Dual alpha-compare** irreducible to one D3D9 test (e.g. band tests `A>lo AND A<hi`) | Rare particle fades | comp0 only, logged | Negligible |
| 13 | **Line width / point size in pixels** (`GXSetLineWidth`) | Debug draws, a few effects | 1px lines; point size best-effort | Negligible |
| 14 | **Z textures / depth-format texture reads** (`GX_TF_Z*`) | Depth-of-field-ish effects | Not bound (black) | Post effects dropped by design |
| 15 | **Per-vertex texture-matrix selection** (`GX_VA_TEXnMTXIDX`) | Env-mapped skinned parts | Stream consumed, effect ignored (uses per-draw matrix) | Minor; material replacement |
| 16 | **GX lighting fidelity** (per-vertex GC light model incl. attnFn/diffFn specifics) | World/actor lighting where not vertex-baked | v1 unlit (material color × vertex color); optional D3DLIGHT9 approximation later | Irrelevant — Remix relights everything |
| 17 | **Texture replacement packs (Dolphin-format)** on d3d9 | HD pack users | Not loaded in v1 | Remix replacement system supersedes |
| 18 | **Bloom / post-processing chain** | dusk sky glow etc. | Skipped by design (project rule) | Remix bloom/tonemap |

## Watch list (decide during bring-up)

- **RGBA6 dst-alpha pixel format nuances** (`GX_PF_RGBA6_Z24` dither) — likely
  irrelevant at 8-bit; verify banding.
- **`GXSetTexCopySrc` half-scale copies** if/when real EFB copies are
  implemented.
- **ImGui debug overlay** in d3d9 mode — v1 headless (context alive, nothing
  rendered); imgui has a stock DX9 renderer if needed later (would appear in
  Remix as UI).
- **RmlUi menus** (Dusklight settings UI) — RESOLVED: `dusk::ui::update()`
  guards on `aurora::rmlui::is_initialized()` and cleanly no-ops, so the
  settings/prelaunch menus are simply unavailable in d3d9 mode (config file +
  CLI work; the GX-drawn game HUD/menus are unaffected). Documented in
  `dusklight-ao/docs/dx9-fixed-function.md`.

## Logging contract

The TEV mapper logs each *distinct* unsupported `ShaderConfig` hash once per
run at WARN level with a compact description (`dx9-tev: unsupported <reason>
hash=<xxh64> stages=<n>`), so play-through sessions produce a to-triage list to
fold back into this document.
