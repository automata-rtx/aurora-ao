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
| 2 | **Compare-mode TEV ops** (`GX_TEV_COMP_*`) | Occasional masking tricks | Approximated as always-true (`d + c`) — keeps mask-style consumers visible; logged (4 distinct stages in a play session) | Usually cosmetic masks; replace affected material textures in Remix |
| 3 | **TEV output registers REG0-2 as true accumulators** (multi-register programs) | Complex characters/effects (e.g. layered eyes, some sky) | Collapsed to the PREV chain, logged per config (2 distinct stages seen) | Material replacement in Remix restores intended look |
| 4 | **>8 effective TEV stages** | Rare (J3D TevBlock16 materials) | Truncated at 8, logged | Same as #3 |
| 5 | **Arbitrary TEV swap tables** (non-identity, non-alpha-replicate) | Rare channel shuffles | Ignored unless expressible as ALPHAREPLICATE | Texture-level fix in Remix if ever visible |
| 6 | **Three or more distinct constants in ONE stage** | Terrain, UI blends, many materials — **by far the most frequent unsupported case (28 distinct stages in one play session)** | Two per stage are exact (TFACTOR, which is per-draw, plus the per-stage `D3DTSS_CONSTANT`; the reference device does expose `PERSTAGECONSTANT`). A third falls back to TFACTOR and silently takes the wrong value; logged | Raising the ceiling needs the stage split to claim another constant slot — not attempted. Material replacement otherwise |
| 7 | **EFB copies** — color copies + offscreen passes are now REAL (StretchRect / SetRenderTarget). Remaining gaps: depth copies (`GX_TF_Z*`) and palette-format copy reinterpretation (shadow silhouette RGB5A3 channel packing is sampled as plain color) | Z-copies; shadowReal channel selection | Depth copies: neutral white/alpha-0 placeholder; palette copies: raw color sampled (approximate shadows) | Path-traced shadows replace `shadowReal` entirely (rtx shadows) |
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

## RTX Remix runtime limitations (not fixed-function limits)

These are constraints of the Remix runtime itself, learned by reading
dxvk-remix source and confirmed against play sessions. They apply *even though*
the D3D9 stream is correct — raw D3D9 renders these cases properly.

| # | Remix behavior | Consequence | What we do |
|---|----------------|-------------|------------|
| R1 | **One albedo texture per draw.** Remix reconstructs a material from a single texture stage (the first bound to the lowest `D3DTSS_TEXCOORDINDEX`) and only `colorTextures[0]` is the albedo — `ColorTexture2` is RayPortal-only | Any GX material compositing several textures in one draw shows **only one layer** under Remix. Character eyes composite an eyeball (CMPR) with I8/I4 highlight and shadow masks | We advertise the **colour** texture (`preferred_albedo_stage`), so the eyeball wins over the masks. Showing all layers would need multi-pass splitting (base pass + overlay stages re-emitted as blended decal draws) — designed but not built |
| R2 | **That one stage's op/args become the whole albedo *and* opacity** | A partial first stage (e.g. `texture x dark konst`) renders the surface black; a first stage whose alpha is a blend weight destroys alpha-test cutouts (foliage as full quads, grass invisible) | A raster-neutral hint stage (`MODULATE(TEXTURE, DIFFUSE)` / `SELECTARG1(TEXTURE)`) is prepended, writing TEMP so the real chain is untouched |
| R3 | Undecodable args (`D3DTA_TEMP`, `D3DTA_CONSTANT`, `COMPLEMENT`, `ALPHAREPLICATE`) resolve to `RtTextureArgSource::None` = **identity**, not zero | Harmless on their own — worth recording because an earlier revision of these docs claimed they rendered black, and that wrong premise cost a full test round | Nothing needed |
| R4 | **UI overlay is not re-derived from a mid-run device `Reset`** | HUD keeps the scale/placement it had at device-creation size after any resize; raw D3D9 follows the Reset correctly | Resizes **recreate** the device instead (`recreate_device`), debounced one frame. Costs a black screen for the rebuild |
| R5 | **`MaxVertexBlendMatrixIndex` is not enforced** — Remix reads the transform state directly and skins on the GPU | Masked a real raw-D3D9 bug for weeks: GX's 10-deep matrix palette overran the device's 9-index cap and scattered vertices, while Remix looked perfect | Palette is compacted per draw, and overflow draws are split into per-palette groups |
| R6 | **Texture transform element counts > 2 are clamped; projected texture transforms unsupported** (`Use of projected texture transform detected…`) | Projected camera-space texgen (`GX_TG_MTX3x4`) cannot survive into Remix — projected shadows / env maps are wrong there | Nothing; documented. Raw D3D9 is correct |
| R7 | **Billboard detection expects fan-order quad indices** (`unsupported quad index layout for billboard creation`) | Particle quads miss Remix's billboard path | Quads emit `(0,1,2)(0,2,3)` |
| R8 | **New GUI input method registers raw keyboard with `RIDEV_NOLEGACY`**, killing `WM_KEY*` process-wide | Game input dead under Remix while Remix hotkeys work | `rtx.useNewGuiInputMethod = False` in `rtx.conf` (documented game-side) |
| R9 | Remix **auto-detects orthographic draws as UI** and rasterizes them as a screen overlay | Handing that path a 3D view matrix (the camera split) mis-shapes the HUD | The camera split is skipped for `GX_ORTHOGRAPHIC` draws |

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

Each *distinct* unsupported configuration is logged once per run, so a
play-through produces a to-triage list to fold back into this document.

- **Unsupported cases** (WARN, via `warn_once`):
  `dx9: unsupported: <reason> (key=0x…)`. The key is either a fixed id per
  reason+stage or a hash of the stage configuration, so distinct materials
  count separately — the *number of distinct keys* per reason is the useful
  signal (it is how the 3-constant ceiling was identified as the most frequent
  gap at 28 stages, and the compare-mode approximation as the prime suspect
  for the raw-D3D9 white ground at 4).
- **Multi-texture materials** (INFO, via `info_once`):
  `dx9: multi-texture material (N textured stages): [gx0 map2 coord2 32x32 fmt1] …`
  — per stage: GX stage index, texmap, texcoord, texture dimensions and GX
  format. This is what identified the eye material (I8 mask first, CMPR
  eyeball two stages later) and is the tool to reach for whenever Remix shows
  the wrong texture for a material.

**Known raw-D3D9 defect still open:** ground textures render pure white in raw
D3D9 while Remix shows them correctly (Remix only reads the first stage, so it
never executes the offending later stage). Suspects, in order: #2
(compare-mode approximated as always-true — the only one that explains *white*
via additive saturation), #6 (constant ceiling), #3 (register collapse).

**Open, reported 2026-07-29 — world-space UI billboards reach Remix
intermittently, and never reach its texture categorization screen.** Two draws
behave as one group: the **targeting arrow** and the **fire billboards** in
torch-lit areas. Observations from the owner:

- They appear and disappear together, which is the strongest hint that one draw
  path owns both.
- The fire billboards were seen appearing during **room transitions** in the
  Forest Temple.
- The targeting arrow rendered correctly only while actively targeting, and not
  reliably even then — it depended on player and camera position.
- Under shadow the arrow goes dim, i.e. it is being **lit as ordinary world
  geometry** when it is meant to read as unlit UI.
- **Neither appears in Remix's texture categorization screen at all.** That is
  the part that makes this an aurora question rather than a Remix tagging
  question: a draw Remix never categorises is a draw it did not capture the way
  we think it did, so no amount of dev-menu tagging can reach it.

Nothing diagnosed yet. The first thing to establish is which submission path
these two share and whether they are going out as ortho/2D draws (which are
deliberately excluded from fog, and may be excluded from more than that) or as
world draws with an unusual state vector. Full context in
`dusklight-ao/docs/kankyo-remix.md` open issue 6.
