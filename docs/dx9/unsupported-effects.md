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
| R2 | **That one stage's op/args become the whole albedo *and* opacity** | A partial first stage (e.g. `texture x dark konst`) renders the surface black; a first stage whose alpha is a blend weight destroys alpha-test cutouts (foliage as full quads, grass invisible) | A raster-neutral hint stage (`MODULATE(TEXTURE, DIFFUSE)` / `SELECTARG1(TEXTURE)`) is prepended, writing TEMP so the real chain is untouched. **Conditional since 2026-08-03** — emitting it over a material Remix already reads correctly replaces a good albedo with a worse one, which is what bleached rupees, hearts and lava. See [`remix-material-interface.md`](remix-material-interface.md) §5 |
| R3 | Undecodable args (`D3DTA_TEMP`, `D3DTA_CONSTANT`, `COMPLEMENT`, `ALPHAREPLICATE`) resolve to `RtTextureArgSource::None` = **identity**, not zero | Harmless on their own — worth recording because an earlier revision of these docs claimed they rendered black, and that wrong premise cost a full test round | Nothing needed |
| R4 | **UI overlay is not re-derived from a mid-run device `Reset`** | HUD keeps the scale/placement it had at device-creation size after any resize; raw D3D9 follows the Reset correctly | Resizes **recreate** the device instead (`recreate_device`), debounced one frame. Costs a black screen for the rebuild |
| R5 | **`MaxVertexBlendMatrixIndex` is not enforced** — Remix reads the transform state directly and skins on the GPU | Masked a real raw-D3D9 bug for weeks: GX's 10-deep matrix palette overran the device's 9-index cap and scattered vertices, while Remix looked perfect | Palette is compacted per draw, and overflow draws are split into per-palette groups |
| R6 | **Texture transform element counts > 2 are clamped; projected texture transforms unsupported** (`Use of projected texture transform detected…`) | Projected camera-space texgen (`GX_TG_MTX3x4`) cannot survive into Remix — projected shadows / env maps are wrong there | Nothing; documented. Raw D3D9 is correct |
| R7 | **Billboard detection expects fan-order quad indices** (`unsupported quad index layout for billboard creation`) | Particle quads miss Remix's billboard path | Quads emit `(0,1,2)(0,2,3)` |
| R8 | **New GUI input method registers raw keyboard with `RIDEV_NOLEGACY`**, killing `WM_KEY*` process-wide | Game input dead under Remix while Remix hotkeys work | `rtx.useNewGuiInputMethod = False` in `rtx.conf` (documented game-side) |
| R9 | Remix **auto-detects orthographic draws as UI** and rasterizes them as a screen overlay | Handing that path a 3D view matrix (the camera split) mis-shapes the HUD | The camera split is skipped for `GX_ORTHOGRAPHIC` draws |
| R10 | **Emissive colour is a constant *or* a texture, never both**, and whichever it is then gets run through the **albedo's** texture op with the emissive substituted for the texture sample | A glow set to a colour arrives as `op(colour, tFactor)` — with `ADD` that is roughly double. And since this game keeps material colour in a GX constant while its textures are intensity masks, a textured glow comes out **white** | The constant is set to the *pre-image* of the op, and emission is declined outright when the op cannot be inverted rather than glowing a wrong colour. See [`remix-material-interface.md`](remix-material-interface.md) §9 |
| R11 | **No lerp between two constants.** The one stage Remix reads offers `TEXTURE`, `DIFFUSE`, `TFACTOR`, `CURRENT` and ops that reduce to `t·C` or `t + C` | This game's dominant material shape is `lerp(colourA, colourB, texture)` (§7), so **every** two-colour ramp is an approximation. Goron Mines lava (`lerp(red, yellow, t)`) reproduces as red-to-white | `ADD` from the floor, which has the lowest measured error but desaturates highlights. Exact reproduction needs both endpoints carried to a surface field — designed in [`remix-material-interface.md`](remix-material-interface.md) §10, not built |

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
- **Material translation** (INFO, `matrep.*`): the full GX → D3D9 → Remix chain
  for each distinct material, always on and bounded. Format and reading guide:
  [`material-report.md`](material-report.md). This **replaces** the older
  `multi-texture material (N textured stages)` line, which reported only
  multi-texture draws and keyed on fields it did not print.

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
- **Neither appears in Remix's texture categorization screen at all.**

**Investigated 2026-07-29 — this is NOT an aurora defect.** An earlier revision
of this entry concluded that "a draw Remix never categorises is a draw it did
not capture the way we think it did, so this is an aurora question." That was
wrong, and it is left recorded rather than deleted because it is the second time
on this project that a plausible recon conclusion has pointed at the wrong repo.

The mechanism is Remix's **RTX injection boundary**: the first orthographic,
z-write-disabled draw on the primary render target ends the raytraced scene for
that frame, and every draw after it is rasterized-only and never categorised.
Aurora submits these draws correctly — they arrive on the wrong side of a line
Remix draws. Full write-up in `dusklight-ao/docs/remix-open-issues.md` open issue 6.

**What is still worth checking here:** nothing urgent. Aurora is where the
ortho and z-write state for the letterbox and fade quads is applied, so if the
eventual fix involves moving that boundary, the state vector aurora emits for
those quads is the thing to confirm.

### PINNED 2026-07-29 — the torch flame, and how to confirm it later

*Parked deliberately: the owner cannot test for a while. This section is written
so the investigation can restart cold.*

**The correction that reframed it.** The "bright white circle" seen at a lit
torch is **not** the animated fire. It is a separate circular sprite. So the
earlier inference — that flames were already arriving and already emissive — was
wrong; what was arriving was something else.

**The torch emits three named resources at the same position**
(`dusklight-ao/src/d/actor/d_a_ep.cpp:423-431`, names from
`d_particle_name.cpp`):

| ID | Resource | Role |
| :-- | :-- | :-- |
| `0x100` | `ZI_J_O_fire_a.jpa` | fire A |
| `0x101` | `ZI_J_O_fire_b.jpa` | fire B |
| `0x103` | `ZI_J_O_kagerou.jpa` | 陽炎, heat haze |

(`0x102` / `fire_c` exists but the torch does not use it. The Forest Temple
candle, `d_a_obj_lv1Candle00`, swaps the fire pair for `0x83a6`/`0x83a7` and
keeps `0x103`.)

**Two deductions worth keeping:**

1. **The heat haze is expected to be broken and is not the bug.** Kagerou is
   indirect texturing — entry #1 above, "heat shimmer … ignored, base stages
   still draw, no warp".
2. **This is probably not the injection boundary.** `fire_a` and `fire_b` are
   emitted back to back, same position, same frame, into the same particle
   system. If they share a draw group they are near-adjacent draws, and an
   injection boundary cannot stably separate two adjacent draws across a whole
   session. So the flame's problem is a property of *that draw* — its TEV/blend
   configuration, its texture, or its group.

**The competing hypothesis, and it has evidence.** The white circle may *be* one
of the fire sprites, saturated to white by entry **#2** — compare-mode TEV ops
approximated as always-true (`d + c`). That entry is already the prime suspect
for the white-ground defect and is noted there as *the only approximation that
explains white via additive saturation*. If so, "the flame is missing and a glow
circle shows" and "the flame renders as a white blob" are one bug, and it is
**this repo's**, not Remix's.

**How to confirm, when testing resumes — cheapest first:**

1. **Read the aurora log stood at a lit torch.** Free, no rebuild. The backend
   emits `dx9: unsupported: <reason> (key=0x…)` (`warn_once`) and the full
   `matrep.*` material report ([`material-report.md`](material-report.md)),
   whose per-stage `matrep.gx` lines show the fire's TEV program directly — so
   an unsupported shape can be read off the log rather than inferred from a
   warning that may not have fired. This is the same class of instrumentation that
   identified the 3-constant ceiling at 28 stages and compare-mode at 4.
2. **A/B raw D3D9 against Remix at the same torch.** Decisive on ownership,
   because the known white-ground defect has a distinctive signature — wrong in
   raw D3D9, *correct* under Remix, since Remix only reads the first texture
   stage and never executes the offending later one.

   | Raw D3D9 | Remix | Reading |
   | :-- | :-- | :-- |
   | circle | correct flame | TEV bug, #2 — this repo |
   | correct flame | circle | capture/categorization — the fork |
   | circle | circle | the resource itself, or a shared earlier stage |
3. Only if both are inconclusive, fall back to the draw-call-ID work in
   `remix-open-issues.md` open issue 6.

### Two more Remix-visible material defects, reported 2026-07-29

**Rupees, hearts and Goron Mines lava render greyscale under Remix, correct in
raw D3D9.**

**Root-caused 2026-08-03, and the cause is not what this section said for a
week.** The full account is in
[`remix-material-interface.md`](remix-material-interface.md); the short version:

- The colour is **not** lost in a channel Remix cannot decode. Remix reads
  `MODULATE(TEXTURE, TFACTOR)` correctly, and `materialize()` already routes a
  material's first constant to TFACTOR.
- It is lost because **aurora's own Remix hint stage overwrites the material
  Remix was reading correctly.** The hint wins the stage Remix reads, and it can
  only say `TEXTURE × DIFFUSE` — with `DIFFUSE` substituted as opaque white when
  the mesh carries no vertex colours. Texture × white = greyscale.
- The 2026-07-29 fix (`389e4d5`) added a repair stage carrying the tint, but
  gated the whole thing behind `albedo_tint()`, a predicate narrow enough that
  it declined these materials. The commit emitted **no additional D3D9 state at
  all** — a byte-for-byte no-op, which is exactly what testing reported.

**Fixed 2026-08-03** by suppressing the hint when Remix already decodes the
stage correctly (`remix_decodes_albedo()` in `dx9_tev.cpp`), and by the
[material report](material-report.md), which makes the decision observable
instead of inferred. The suppression is deliberately restricted to
single-stage materials; the report's `hintLoose` field measures what widening it
would catch.

The general rule this leaves behind, and it survives the corrected diagnosis:
**route a material's albedo tint to TFACTOR rather than the per-stage constant
whenever there is a choice, because only one of the two survives into Remix.**

**Grass patches shade wrongly under Remix, fine in raw D3D9.** Reported
symptoms: glowing in the dark, being too dark, very delayed lighting response,
and generally reading as a different material from the rest of the scene. The
owner also cannot replace the billboard blades with real geometry, because the
hashes are unstable.

The hash instability has a specific and fixable cause. `dGrass_packet_c::draw`
has **two** paths:

| Path | How it draws | Hash consequence |
| :-- | :-- | :-- |
| **Batched** (the default for standing grass) | one immediate-mode `GXBegin(GX_TRIANGLES, GX_VTXFMT1, GX_AUTO)` stream, `GXLoadPosMtxImm(identity)`, every blade pre-transformed into world space and merged into four buckets | one giant instance whose **vertex positions change whenever any blade moves, is cut, regrows, or changes bucket** → asset hash churns every frame |
| **Per-blade** (only used for regrowing blades) | `GXCallDisplayList(mp_Mkusa_9q_DL, …)` with a per-blade `GXLoadPosMtxImm(get_model_mtx(...))` | static display-list geometry + a transform → **stable hash**, one instance per blade, taggable and replaceable |

Remix's `rtx.geometryAssetHashRuleString` defaults to
`positions,indices,geometrydescriptor`, so positions are load-bearing for
identity. The batching optimisation — good for raster draw-call count — is
precisely what destroys that identity, and the Remix-friendly path already
exists in the same function.

*Fix direction, not implemented:* a game-side switch that forces the per-blade
display-list path while under Remix. It costs exactly what the batching saves,
which is why it should be a switch rather than a default, and it is the
prerequisite for everything else the owner wants here — stable hashes make the
blades taggable, replaceable with real geometry, and temporally stable.

The lighting symptoms are partly separate and are catalogued in
`dusklight-ao/docs/remix-open-issues.md` open issue 7, since they involve Remix
options as well as this repo.
