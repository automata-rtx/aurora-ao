# Effects beyond fixed-function / SM1 — living list

GX features the D3D9 fixed-function pipeline cannot carry, and Remix runtime
behaviour that constrains what reaches the path tracer.

**Read this as a work list, not a list of losses.** The raw D3D9 image is never
shown to a player — it is a feed into Remix — so "fixed function cannot express
this" is a statement about the *transport*, not about what the game can look
like. Where the transport cannot carry an effect, the answer is to implement it
**in Remix**, via the Remix API or a fork change. See
[`remix-material-interface.md`](remix-material-interface.md) §0.

The "what we do" column therefore describes the current state, and an entry
saying "open", "nothing" or "not attempted" is an opportunity rather than a
closed door. Update this file whenever the TEV mapper logs a new unsupported
configuration.

## Confirmed unsupported (by design of D3D9 FF)

| # | Effect / GX feature | Where TP uses it | v1 behavior | Remix-side compensation |
|---|---------------------|------------------|-------------|-------------------------|
| 1 | **Indirect texturing** (`GXSetTevIndirect`) | Heat shimmer, water surface warp, magic | Ignored: base stages still draw, no warp | Re-authored in Remix. **Water is now done this way** (`remix-material-interface.md` §11) — the game marks its own water per draw and Remix builds a translucent material, so the warp is replaced rather than reproduced. The torch's kagerou heat haze is simply absent; expected, and **not** the torch-flame bug below |
| 2 | **Compare-mode TEV ops** (`GX_TEV_COMP_*`) | Occasional masking tricks | Approximated as always-true; logged (4 distinct stages in a session) | **Open, and not safely "cosmetic"** — the prime suspect for the torch-flame white circle, which *does* reach Remix. The answer is to evaluate the compare where its operands are known, not to hand-replace a texture |
| 3 | **TEV output registers REG0-2 as true accumulators** | Layered eyes, some sky | Collapsed to the PREV chain, logged (2 distinct stages seen) | Open. Where the program is still *evaluable*, the ramp precedent applies (`remix-material-interface.md` §10): evaluate it in aurora and carry the result on a side channel. These report `unevaluable` today |
| 4 | **>8 effective TEV stages** | Rare (J3D TevBlock16 materials) | Truncated at 8, logged | Same as #3 |
| 5 | **Arbitrary TEV swap tables** (non-identity, non-alpha-replicate) | Rare channel shuffles | Ignored unless expressible as ALPHAREPLICATE | Texture-level fix in Remix if ever visible |
| 6 | **Three or more distinct constants in ONE stage** | Terrain, UI blends — **by far the most frequent unsupported case, 28 distinct stages in one session** | Two per stage are exact (per-draw TFACTOR plus `D3DTSS_CONSTANT`); a third falls back to TFACTOR and silently takes the wrong value; logged | **Understated until 2026-08-04.** A third constant does not have to fit a D3D9 stage slot at all — the fork already reads colours straight out of `D3DMATERIAL9` (§2, §10 there), so this ceiling is a transport choice, not a hard limit. Neither route attempted |
| 7 | **EFB copies** — color copies + offscreen passes are now REAL (StretchRect / SetRenderTarget). Remaining gaps: depth copies (`GX_TF_Z*`) and palette-format copy reinterpretation (shadow silhouette RGB5A3 channel packing is sampled as plain color) | Z-copies; shadowReal channel selection | Depth copies: neutral white/alpha-0 placeholder; palette copies: raw color sampled (approximate shadows) | Path-traced shadows replace `shadowReal` entirely (rtx shadows) |
| 8 | **`GX_TG_SRTG` texgen** (vertex color → texcoord) & **emboss bump** (`GX_TG_BUMP*`) | Emboss-style highlights on a few materials | Texcoord = 0,0; logged | Normal-mapped PBR replacements in Remix |
| 9 | **Fog range adjustment** (`GXSetFogRangeAdj`) + backwards/exp fog exactness | Distance fog tweaks | Plain linear/exp approximation | Done in Remix: the fork drives fog, sky and sky-light from the game's kankyo state (`dxvk-remix documentation/DusklightAtmosphere.md`), so the D3D9 fog is not the output |
| 10 | **Logic-op blending** beyond CLEAR/SET/COPY/NOOP | Very rare on GC titles | Draw falls back to opaque, logged | N/A |
| 11 | **Destination-alpha blend factors** when backbuffer lacks alpha | Some layered effects | Factor swapped to ONE/ZERO approximation, logged | Remix runtime provides A8R8G8B8 — exact there |
| 12 | **Dual alpha-compare** irreducible to one D3D9 test | Rare particle fades | comp0 only, logged | **Not automatically cosmetic:** alpha is one of the two things that must still be right (§0). No instance has been seen; if one appears, carry the second bound to the fork rather than dropping it |
| 13 | **Line width / point size in pixels** (`GXSetLineWidth`) | Debug draws, a few effects | 1px lines; point size best-effort | Negligible |
| 14 | **Z textures / depth-format texture reads** (`GX_TF_Z*`) | Depth-of-field-ish effects | Not bound (black) | Post effects dropped by design |
| 15 | **Per-vertex texture-matrix selection** (`GX_VA_TEXnMTXIDX`) | Env-mapped skinned parts | Stream consumed, effect ignored (uses per-draw matrix) | Minor; material replacement |
| 16 | **GX lighting fidelity** (the per-vertex GC light model) | World/actor lighting where not vertex-baked | Unlit; a `D3DLIGHT9` approximation was never wanted | Remix relights everything, so the GC light model is not the thing to reproduce. What matters is not double-counting: vertex colour is forwarded only where GX says the stream is authored material colour, per draw (§7c there) |
| 17 | ~~**Texture replacement packs (Dolphin-format)** on d3d9~~ | HD pack users | **Implemented 2026-08-05, tested good 2026-08-06** | The pack reaches Remix without its bytes entering D3D9, substituted at two sites because Remix splits the frame. [`texture-replacements.md`](texture-replacements.md) |
| 18 | **Bloom / post-processing chain** | dusk sky glow etc. | Skipped by design (project rule) | Remix bloom/tonemap |

## RTX Remix runtime limitations (not fixed-function limits)

These are constraints of the Remix runtime itself, learned by reading
dxvk-remix source and confirmed against play sessions. They apply *even though*
the D3D9 stream is correct — raw D3D9 renders these cases properly.

Two readings of that last clause, and only one is right. It is how you tell an
R-class problem from a fixed-function one, which is diagnostically useful; it is
**not** a consolation, because the raw image is never shown (§0). And these are
constraints of *stock* Remix — the fork is ours, so an entry here is a candidate
for a fork change rather than a verdict. R11 sat here as a permanent limitation
until 2026-08-04, when it stopped being one.

| # | Remix behavior | Consequence | What we do |
|---|----------------|-------------|------------|
| R1 | **One albedo texture per draw.** Only `colorTextures[0]` is the albedo; `ColorTexture2` is RayPortal-only | A GX material compositing several textures in one draw shows **only one layer**. Character eyes composite a CMPR eyeball with I8/I4 masks | We advertise the **colour** texture (`preferred_albedo_stage`), so the eyeball wins. Multi-pass splitting is designed and not built; a second route is to carry the extra layer on a side channel, the way §10 carries the ramp endpoints |
| R2 | **That one stage's op/args become the whole albedo *and* opacity** | A partial first stage renders the surface black; one whose alpha is a blend weight destroys alpha-test cutouts (foliage as full quads) | A hint stage is prepended, writing TEMP. **Conditional since 2026-08-03** — emitting it over a material Remix already reads correctly replaces a good albedo with a worse one, which is what bleached rupees, hearts and lava. `remix-material-interface.md` §5 |
| R3 | Undecodable args (`D3DTA_TEMP`, `D3DTA_CONSTANT`, `COMPLEMENT`, `ALPHAREPLICATE`) resolve to `RtTextureArgSource::None` = **identity**, not zero | Harmless on their own — worth recording because an earlier revision of these docs claimed they rendered black, and that wrong premise cost a full test round | Nothing needed |
| R4 | **UI overlay is not re-derived from a mid-run device `Reset`** | HUD keeps the scale/placement it had at device-creation size after any resize; raw D3D9 follows the Reset correctly | Resizes **recreate** the device instead (`recreate_device`), debounced one frame. Costs a black screen for the rebuild |
| R5 | **`MaxVertexBlendMatrixIndex` is not enforced** — Remix reads the transform state directly and skins on the GPU | Masked a real raw-D3D9 bug for weeks: GX's 10-deep matrix palette overran the device's 9-index cap and scattered vertices, while Remix looked perfect | Palette is compacted per draw, and overflow draws are split into per-palette groups |
| R6 | **Texture transform element counts > 2 are clamped; projected texture transforms unsupported** (`Use of projected texture transform detected…`) | Projected camera-space texgen (`GX_TG_MTX3x4`) cannot survive into Remix — projected shadows / env maps are wrong there | **Open, not attempted.** Raw D3D9 being correct here buys nothing, since that image is never shown (§0). Two routes: teach the fork the projected transform, or evaluate the texgen per vertex in aurora so what crosses is already projected |
| R7 | **Billboard detection expects fan-order quad indices** | Particle quads miss Remix's billboard path | Quads emit `(0,1,2)(0,2,3)`, the layout `createBillboards` checks for. **Two further gates, read in the fork 2026-08-07 and untested:** generation runs only for instances already in the *unordered* TLAS (i.e. textures categorised as Particle), and `rtx.useIntersectionBillboardsOnPrimaryRays` is **false** by default. Batching the weather emitters (2026-08-08) is what makes this reachable at all — the path walks one instance's quads, and had nothing to work with while each quad was its own draw |
| R8 | **New GUI input method registers raw keyboard with `RIDEV_NOLEGACY`**, killing `WM_KEY*` process-wide | Game input dead under Remix while Remix hotkeys work | `rtx.useNewGuiInputMethod = False` in `rtx.conf` (documented game-side) |
| R9 | Remix **auto-detects orthographic draws as UI** and rasterizes them as a screen overlay | Handing that path a 3D view matrix (the camera split) mis-shapes the HUD | The camera split is skipped for `GX_ORTHOGRAPHIC` draws |
| R10 | **Emissive colour is a constant *or* a texture, never both**, and whichever it is is then run through the **albedo's** texture op | A glow set to a colour arrives as `op(colour, tFactor)`. Separately, a constant glow at any useful intensity swamps the albedo — the lava read as one flat hot colour with no crust (tested 2026-08-04) | **Fixed in the fork, and the choice exposed.** `RtSurface::emissiveSource` selects out of the texture op, and `rtx.dusklight.emissive.colorSource` picks among three readings. GX records no emissive term, so there is no correct answer to hard-code. `remix-material-interface.md` §9 |
| R11 | **No lerp between two constants** in stock Remix | This game's dominant material shape is `lerp(colourA, colourB, texture)`, so every two-colour ramp was an approximation | **Fixed in the fork 2026-08-04 — CI-green, untested in game.** Both endpoints are carried to `RtSurface` and the shader evaluates the GX combiner directly. No struct growth: the spare bits were already there. `remix-material-interface.md` §10 |
| R12 | **Remix identifies textures per D3D9 *object* and holds references to them across frames**, not per content | The game builds a stack-local `GXTexObj` per draw per frame for its 2D drawlists. Keyed naively, that is one D3D9 texture created and destroyed per draw: Remix's texture list churns visibly in the categorize tab and VRAM grows past 32 GB | The store is content-addressed so an identical re-init resurrects the *same* object, and EFB copy targets are keyed by (dest, size) so the bloom chain's two sizes each keep a persistent target. `dx9_texture.cpp:34-43`, `:199-204` |

## Logging contract

Each *distinct* unsupported configuration is logged once per run, so a
play-through produces a to-triage list to fold back into this document.

- **Unsupported cases** (WARN, via `warn_once`):
  `dx9: unsupported: <reason> (key=0x…)`. The key is either a fixed id per
  reason+stage or a hash of the stage configuration, so distinct materials
  count separately — **the number of distinct keys per reason is the useful
  signal**. It is how the 3-constant ceiling was identified as the most frequent
  gap at 28 stages, and compare-mode as the prime suspect for the white ground
  at 4.
- **Material translation** (INFO, `matrep.*`): the full GX → D3D9 → Remix chain
  for each distinct material, always on and bounded. Format and reading guide:
  [`material-report.md`](material-report.md).

## PINNED 2026-07-29 — the torch flame, and how to confirm it later

*Parked deliberately: the owner cannot test for a while. Written so the
investigation can restart cold.*

**The correction that reframed it.** The "bright white circle" seen at a lit
torch is **not** the animated fire; it is a separate circular sprite. So the
earlier inference — that flames were already arriving and already emissive — was
wrong. What was arriving was something else.

The torch emits three named resources at the same position
(`dusklight-ao/src/d/actor/d_a_ep.cpp:423-431`, names from
`d_particle_name.cpp`; the resource names are romanized Japanese, so glossing
them is the convention — `dusklight-ao/docs/japanese-naming-remix.md`): `0x100`
`ZI_J_O_fire_a.jpa` and `0x101` `ZI_J_O_fire_b.jpa`, the fire pair, and `0x103`
`ZI_J_O_kagerou.jpa` — 陽炎 *kagerou*, heat haze. (`0x102`/`fire_c` exists and
the torch does not use it; the Forest Temple candle swaps the fire pair for
`0x83a6`/`0x83a7` and keeps `0x103`.)

**Two deductions worth keeping:**

1. **The heat haze is expected to be broken and is not the bug.** Kagerou is
   indirect texturing — #1 above.
2. **This is probably not the injection boundary.** `fire_a` and `fire_b` are
   emitted back to back, same position, same frame, into the same particle
   system, so they are near-adjacent draws — and an injection boundary cannot
   stably separate two adjacent draws across a whole session. The flame's
   problem is a property of *that draw*: its TEV/blend configuration, its
   texture, or its group.

**The competing hypothesis, and it has evidence.** The white circle may *be* one
of the fire sprites, saturated to white by **#2** — compare-mode approximated as
always-true, the only approximation here that explains white via additive
saturation, and already the prime suspect for the white ground. If so, "the
flame is missing and a glow circle shows" and "the flame renders as a white
blob" are one bug, and it is **this repo's**. Note the difference in stakes: the
white ground is never shown to anyone, but this one reaches Remix.

**How to confirm, when testing resumes — cheapest first:**

1. **Read the aurora log stood at a lit torch.** Free, no rebuild. `warn_once`
   emits the unsupported cases and `matrep.gx` shows the fire's TEV program
   directly, so an unsupported shape can be read off the log rather than
   inferred from a warning that may not have fired.
2. **A/B raw D3D9 against Remix at the same torch.** Decisive on ownership,
   because the white-ground case has a distinctive signature — wrong in raw
   D3D9, *correct* under Remix, since Remix only reads the first texture stage
   and never executes the offending later one. The raw image matters here as a
   **diagnostic instrument**, not as an output.

   | Raw D3D9 | Remix | Reading |
   | :-- | :-- | :-- |
   | circle | correct flame | TEV bug, #2 — this repo |
   | correct flame | circle | capture/categorization — the fork |
   | circle | circle | the resource itself, or a shared earlier stage |
3. Only if both are inconclusive, fall back to the draw-call-ID work in
   `dusklight-ao/docs/remix-open-issues.md` open issue 6.

## Grass patches shade wrongly under Remix, fine in raw D3D9

Reported symptoms: glowing in the dark, being too dark, very delayed lighting
response, and generally reading as a different material from the rest of the
scene. The owner also cannot replace the billboard blades with real geometry,
because the hashes are unstable.

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

**Do not generalise this into "batching is bad under Remix".** It is bad *here*
because grass has a stable alternative — a per-blade display list whose
positions do not move — so batching trades a real identity away for draw calls.
Geometry that has no stable identity to lose is the opposite case: the kankyo
weather particles move every vertex every frame regardless, so their hash
churned before batching and churns after, and collapsing ~1000 draws a frame
into one cost nothing that was not already gone (2026-08-08, tested). Texture
*tagging* survives either way, because tags key on the texture hash rather than
the geometry hash. **The question is not "does this batch?" but "is there a
stable identity here to destroy?"**

*Fix direction, not implemented:* a game-side switch that forces the per-blade
display-list path while under Remix. It costs exactly what the batching saves,
which is why it should be a switch rather than a default, and it is the
prerequisite for everything else the owner wants here — stable hashes make the
blades taggable, replaceable with real geometry, and temporally stable.

The lighting symptoms are partly separate and are catalogued in
`dusklight-ao/docs/remix-open-issues.md` open issue 7.

## Two reclassifications, kept because they are still cited

**Ground textures render pure white in raw D3D9 while Remix shows them
correctly** (Remix only reads the first stage, so it never executes the
offending later one). **Not a defect** — Remix is the output. It is kept only
because the same compare-mode approximation is the prime suspect for the
torch-flame circle above.

**World-space UI billboards reach Remix intermittently** — the targeting arrow
and the fire billboards, which appear and disappear together. **Not an aurora
defect:** the mechanism is Remix's RTX injection boundary, where the first
orthographic z-write-disabled draw on the primary render target ends the
raytraced scene for that frame and everything after it is rasterized-only and
never categorised. Aurora submits these draws correctly; they arrive on the
wrong side of a line Remix draws. Full write-up in
`dusklight-ao/docs/remix-open-issues.md` open issue 6. What is still worth
checking *here*: aurora applies the ortho and z-write state for the letterbox
and fade quads, so if the eventual fix moves that boundary, the state vector
aurora emits for those quads is the thing to confirm.
