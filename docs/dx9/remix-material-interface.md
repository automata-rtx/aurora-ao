# The GX → D3D9 → Remix material interface

**This is the most misunderstood system in the project.** Three sessions
produced three confident, mutually incompatible explanations of one colour
defect, and the fix that shipped on 2026-07-29 was a no-op. This document exists
so that does not happen a fourth time.

Companions: [`material-report.md`](material-report.md) is the log that lets you
observe this interface instead of reasoning about it.
[`unsupported-effects.md`](unsupported-effects.md) catalogues what the D3D9
stream cannot carry, and therefore where the work belongs instead.
[`gx-to-d3d9-mapping.md`](gx-to-d3d9-mapping.md) is the translation spec this
constrains.

---

## 0. What the D3D9 renderer is for

**The raw fixed-function image is never shown to anyone.** It exists so that
Remix's DX9→Vulkan translation picks the scene up automatically — geometry,
transforms, textures, the overwhelming majority of a frame, for free. Remix's
renderer is the product; D3D9 is the feed.

Two consequences, and they reverse an earlier assumption in these documents:

1. **Fixed-function limitations are not the ceiling.** Where the D3D9 stage
   chain cannot carry something faithfully enough to reach Remix, the answer is
   to do it **in Remix** — through the Remix API, or by changing the fork —
   rather than contorting the D3D9 stream to approximate it. Both halves are
   ours. `unsupported-effects.md` is a list of *where to do the work*, not a
   list of what we have given up.
2. **"Raw D3D9 stays correct" is not a design goal.** It is sometimes a useful
   safety property — a change that cannot alter the rasterized image cannot
   regress anything outside Remix — but it is never a reason to reject an
   approach. Several constraints in this document exist only because of the old
   assumption; they are marked where they appear.

**Two things must still rasterize correctly**, and they are the exceptions to
everything above:

- **The HUD.** Remix *rasterizes* UI draws rather than path-tracing them
  (`isRenderingUI` in `d3d9_rtx.cpp`), so 2D stage chains are the real output.
- **Alpha.** Remix reads the stage's alpha op and args to build opacity and the
  alpha test, so cutouts depend on that half of the chain being right.

---

## 1. The model, in one paragraph

Aurora reduces a GX TEV program into D3D9 fixed-function texture stages. Remix
does **not execute** those stages. It reads **one** of them, plus two booleans
and one colour, and reconstructs a PBR material by pattern-matching a small set
of recognised shapes. Nothing else in the *stage chain* reaches it — but the
stage chain is no longer the whole interface: since 2026-08-04 the fork also
reads `D3DMATERIAL9` fields that aurora fills in deliberately (§2). Adding
another is a normal move here, not a last resort. Outside the HUD and the alpha
test, **the chain is only a message to Remix**, and where the message cannot be
expressed the answer is to extend Remix rather than to compromise the message.

## 2. What Remix actually reads

Verified in the fork (`dxvk-remix`) rather than assumed:

| What | Where | Note |
| :-- | :-- | :-- |
| **One** texture stage, `firstStage` | `d3d9_rtx.cpp` `processTextures` → `setTextureStageState` | Selected by texcoord bin, *not* by stage order |
| That stage's colour op and two arg sources | `d3d9_rtx_utils.cpp` `convertTextureOp` / `convertTextureArg` | |
| That stage's alpha op and arg sources | same | Becomes opacity, and therefore the alpha test |
| `D3DRS_TEXTUREFACTOR` | per draw | One colour per draw, shared by every stage |
| One extra "multi-stage TFACTOR" boolean | `isTextureFactorBlendingEnabled` | See §4 |
| Blend state, alpha test state | | Drives transparency and the emissive-blend heuristic |

**Ops it decodes:** `SELECTARG1`, `SELECTARG2`, `MODULATE`, `MODULATE2X`,
`MODULATE4X`, `ADD`, `DISABLE`. Anything else silently becomes `MODULATE`.

**Args it decodes:** `TEXTURE`, `DIFFUSE` (the vertex colour), `TFACTOR`,
`CURRENT`. **Nothing else.**

### The side channels — this fork only, not stock Remix

`D3DRS_LIGHTING` is off here, so nothing consumes a D3D9 material and the whole
struct was free. Every row exists because the stage chain could not carry the
fact — §0 applied, extend the fork rather than contort the stream.

| Field | Carries | Read in the fork | Detail |
| :-- | :-- | :-- | :-- |
| `Specular.r` | per-draw verdict "the vertex stream is authored material colour" | `d3d9_rtx_utils.cpp` → `isVertexColorBakedLighting` | §7c |
| `Specular.g` | "aurora evaluated a presentable colour here" — 0 for HUD/ortho and unevaluable draws | `dusklightEmissive::isCandidate` | §9 |
| `Specular.b` | "this material has a colour of its own" — TEV constants, not the vertex stream, not a bare pass-through | `dusklightEmissive::authoredColor` | §9 |
| `Specular.a` | **"this material is self-lit"** — no TEV colour stage reads the rasterized channel | `dusklightEmissive::selfLit` | §9 |
| `Emissive.rgb` | the colour the surface presents — the glow test, and the glow under `colorSource=2` | `rtx_instance_manager.cpp` | §9 |
| `Emissive.a` | the self-illumination evidence score, 0..1. **Reported, not used** | — | §9 |
| `Diffuse.rgb` | the ramp endpoint TFACTOR does not hold → `RtSurface::rampOtherColor` | `rtx_instance_manager.cpp` | §10 |
| `Diffuse.a` | "this material is a ramp" → `textureFlags` bit 15 | same | §10 |
| `Ambient.r` | which endpoint TFACTOR holds → `textureFlags` bit 16 | same | §10 |
| `Ambient.g` | 1-based index of the HD replacement this draw's albedo wants, 0 for none | `dusklightTexRep::handleFromLegacyMaterial` | [`texture-replacements.md`](texture-replacements.md) |
| `Ambient.b` | the D3D9 stage `Ambient.g` refers to; meaningful only when it is non-zero | `dusklightTexRep::handleForRasterStage` | same |
| `Power` | all three water facts packed: `tag * 100 + layer * 10 + role` | `dusklightWater::waterRole` / `waterTag` / `waterLayer` | §11 |

**`Ambient.a` is nominally free and a new per-draw fact should not take it** —
the transport for one is the drawmeta flag word, so **a new fact is a new bit,
not a new channel** (`rtx_dusklight_drawmeta.h:27-50` argues it). Keep
`Ambient.a` for something that must ride the material struct through the
*capture* path, and note the unmerged claim on it by
`claude/dusklight-remix-transparency-e7l766` — nothing automated can see an
unmerged branch.

`check_invariants.py` compares this table against `set_remix_material` in both
directions and checks the water packing against the fork's decode. **What no
check can catch** is a channel written with a *different meaning*: both
directions pass and the map reads as true, so a channel must be claimed in this
table in the same commit that writes it.

`Ambient.g`/`.b` are the only fields read on the **rasterized** path
(`D3D9DeviceEx::BindTexture`) as well as the ray-traced one; everything else is
consumed during material resolution, which UI draws never reach.

**Three things do not survive and are easy to emit by accident:**
`D3DTSS_CONSTANT` / `D3DTA_CONSTANT` (aurora's second constant slot — a colour
routed there is simply gone), `D3DTA_TEMP` (the split-stage scratch register),
and the `D3DTA_COMPLEMENT` / `D3DTA_ALPHAREPLICATE` modifier bits, which
un-decode an arg whose base source would have been fine. `remix_decodes_arg`
(`dx9_tev.cpp:476-492`) is the predicate, with the unclaimed-TFACTOR case that
made the July 2026 fix a silent no-op.

## 3. The failure mode is WHITE, not black

This is the single most important fact in this document.

An argument Remix cannot decode resolves to `RtTextureArgSource::None`, and the
shader resolves `None` to **identity** — `vec3(1.0)` for colour.

So a lost colour term does not darken a surface and does not throw an error. It
**bleaches** it. The surface renders as its texture with the colour multiplied
out, which is visually indistinguishable from "the artist intended no tint."

**Corollary — grayscale is a diagnosis, not just a symptom.** If a surface
renders greyscale under Remix and correct in raw D3D9, a colour term reached
identity. That narrows the cause enormously; treat it as evidence, not as a
vague complaint.

> An earlier revision of the docs claimed undecodable args rendered *black*.
> That was wrong and it cost a full test round. Kept here because the correct
> version is counter-intuitive.

## 4. The multi-stage TFACTOR escape hatch, and its exact shape

Remix will read **one** additional stage, purely to pick up a TFACTOR multiply
(`rtx.enableMultiStageTextureFactorBlending`, on by default). It matches a
`MODULATE`/`2X`/`4X` whose two args are `TFACTOR` and **the previous stage's
output register**, and never matches against `TEXTURE`. That last clause is
load-bearing: `MODULATE(TEXTURE, TFACTOR)` does not trigger it and does not need
to, being decodable on its own (§2). The hatch exists for a tint that must be
applied to something a previous stage produced.

## 5. The hint stage: what it buys and what it costs

Aurora prepends a synthetic stage advertising
`colour = TEXTURE × DIFFUSE, alpha = TEXTURE`, writing `TEMP` so the real chain
is untouched. **What it fixes:** a GX material's first stage is often *not* the
finished albedo, and handed such a stage Remix renders the surface black and
takes that stage's alpha as opacity, destroying alpha-tested cutouts.

**What it costs, and this was missed for months:** the hint *wins* the stage
Remix reads, so over a material that was already read correctly it replaces a
correct material with a worse one — `TEXTURE × DIFFUSE` cannot express a tint,
and `DIFFUSE` becomes opaque white with no vertex colours, so a tinted luminance
texture comes out exactly grayscale. That, not an undecodable channel, was the
rupee / heart / lava defect.

> **Emit the hint only when Remix would otherwise read this stage wrongly.**
> It is a repair, not an annotation. Applying a repair to something that is not
> broken is how this class of bug is created.

`remix_decodes_albedo()` (`dx9_tev.cpp:494-503`) is the test that decides.
Opacity is *not* subject to it — alpha must still reach Remix as the texture's
own alpha or cutouts break, so it gates every suppression.

> The `TEMP` trick and the whole notion of raster-neutrality date from when the
> rasterized image had to stay correct. Per §0 it no longer does outside the
> HUD, so a future revision is free to write the material it wants Remix to read
> straight into stage 0 — which would also free `TFACTOR`, the reason a ramp is
> sometimes declined (`ramp=tfTaken`, §10).

## 6. Where a colour can live, and which ones survive

The game keeps material colour in TEV colour registers (C0–C2) and konst
registers (K0–K3), animated by `.brk` assets. **Aurora sees both** — they reduce
to the same `isConst` operand. The question is only which D3D9 slot a constant
lands in: `D3DRS_TEXTUREFACTOR` is per draw and **survives** into Remix;
`D3DTSS_CONSTANT` is per stage and **does not**.

> **Design rule: an albedo tint that can go to either slot must go to TFACTOR.**
> `materialize()` gives the draw's *first* constant TFACTOR unconditionally, so
> in practice: do not let an unimportant constant claim TFACTOR ahead of the
> albedo tint. A side channel is the third route (§2, §10), but TFACTOR costs
> nothing and is already read — spend the channel on what TFACTOR cannot hold.

## 7. The shape this game actually uses — measured, not assumed

**This section replaces an earlier claim that these materials are
"greyscale texture × one tint colour". They are usually not**, and building a
fix on that model is why the 2026-07-29 attempt matched almost nothing. The
dominant shape is a **two-colour ramp**, `lerp(colourA, colourB, texture)` — the
greyscale texture is a slider between two authored colours, not a mask waiting
to be tinted, which is how one rupee texture yields seven rupee colours.
`texture × colour` is only the special case where colourA is black. The
measurement that settles it, and the "evaluate rather than pattern-match"
argument that follows from it, are at `dx9_tev.cpp:927-941`.

Three consequences that are easy to get wrong and are not in that comment:

- **A multiply cannot represent a ramp with a non-black floor** — `tex × C`
  falls to black where the texture is dark, so it *darkens* a floor-coloured
  ramp. Remix also decodes `ADD`, which fits that shape. Since 2026-08-04 this
  is the fallback path only: §10 sends both endpoints to the fork.
- **"The texture is greyscale" is normal and correct here.** Any rule of the
  form "an intensity-format texture is not the albedo" is wrong for this game.
  `is_color_texture_format()` exists for a real reason (eyes composite a colour
  eyeball with intensity masks) but must never be read that way.
- **A stage can bind a texture and not use it.** Materials here routinely open
  with a setup stage whose colour pass is all `ZERO`. Selecting the first
  *textured* stage rather than the first that *reads* its texture hands Remix an
  empty program — the second cause of the same defect.

## 7b. Choosing what to advertise — measured 2026-08-04

> Governs only materials where the §10 ramp is **declined**. For anything
> reporting `tfLow`/`tfHigh`, nothing here decides what is drawn.

One stage, one op, one constant, against a truth of `lerp(floor, top, texture)`:
the choice is which end to get right, and **the floor decides it, not where the
ramp ends**. A black floor takes `MODULATE`, a coloured floor takes `ADD`
(a multiply yields black wherever the texture is black — a hole in the object's
own colour), and a near-white floor keeps `MODULATE` because `ADD` would drive
the whole surface white. The three cases, the GX measurement that makes a
multiply *structurally* wrong rather than mistuned, and the earlier revision
that gated `ADD` on the ramp's top end instead and matched 5 materials out of
111, are all at `dx9_tev.cpp:1132-1161`.

**Accepted cost:** `ADD` cannot reproduce the scale on the texture, so
highlights come out brighter than the original. If that needs fixing, the
missing datum is the texture's brightness distribution — add it to the report
rather than guessing. Opacity is the same principle applied to alpha, and its
konst-scale case is at `dx9_tev.cpp:954-960`.

## 7c. Vertex colour: forward it only when GX says it is material colour

This game's vertex colours *do* carry baked lighting — turning
`rtx.vertexColorIsBakedLighting` off makes shaded areas visibly darker — but
withholding `DIFFUSE` globally in response was too blunt and threw away real
material colour. GX distinguishes the two cases exactly, per draw, in the colour
channel's control (`GXSetChanCtrl`); the forward/withhold rule and its reason
are at `dx9_tev.cpp:947-952`.

| GX state | What the vertex stream is | What we do |
| :-- | :-- | :-- |
| lighting **enabled**, `matSrc = GX_SRC_VTX` | the **material colour** GX would then light — it carries no light of its own | **forward it** |
| lighting **disabled**, `matSrc = GX_SRC_VTX` | the finished channel output, where this game bakes room lighting and shadow | **withhold it** |
| no `CLR0` attribute at all | a constant aurora writes into every vertex from the channel's material colour register — authored by definition | **evaluate it** as part of the material |

The third row was a silent bug: `eval_operand` treated `DIFFUSE` as white
unconditionally, so a material whose colour arrived that way evaluated as if it
had none. Those materials now evaluate correctly, which also makes them eligible
for the §10 ramp.

The verdict rides `Specular.r` (§2) and sets `isVertexColorBakedLighting` per
draw instead of taking the global option, which was necessarily wrong for one of
the two cases. `vtxUse=` on `matrep.sum` prints which applied. **CI-green,
untested in game** as of 2026-08-04.

**What remains a judgement call:** a draw with lighting disabled whose vertex
colour is genuinely authored — a per-vertex tint or fade on an effect rather
than baked room light. Those are withheld today; if something loses a colour
gradient it should have, that is this, and the log says `vtxUse=bakedLight`.

## 8. Design rules, collected

For anyone changing `dx9_tev.cpp` or adding a Remix-facing feature:

1. **Remix reads one stage.** Any meaning placed in a later stage is lost unless
   it is the §4 TFACTOR pattern — or you give it a side channel of its own and
   teach the fork to read it (§2).
2. **Undecodable means white, not broken.** Losses are silent and look
   intentional. Assume nothing is reporting them.
3. **Repairs must be conditional.** A transformation that helps a broken
   material usually harms a correct one.
4. **Albedo tints go to TFACTOR.** The other constant slot is a black hole.
5. **Opacity is sacred.** It drives the alpha test; damaging it turns foliage
   into solid quads. Never trade opacity fidelity for colour fidelity.
6. **Do not model Remix's heuristics from memory.** They have been described
   wrongly in these docs twice. Read the fork, or read the material report.
7. **Prefer stating over encoding.** Where a per-draw fact can be transmitted
   directly instead of being inferred from fixed-function state, that is
   strictly better.
8. **"Remix cannot express X" is a claim about *stock* Remix.** The fork is
   ours. Check whether the constraint is real before designing around it — the
   two-colour ramp was written down as inexpressible and then reproduced
   exactly, a week later than it needed to be (§10).

## 9. Self-illumination — a rule, not a score

GX has **no emissive term**, and no single GX fact identifies an emitter: on the
one surface this feature exists for, every weighted signal reads zero.

**The rule.** A surface emits when all four hold:

| # | Fact | Where it comes from |
| --: | :-- | :-- |
| 1 | aurora evaluated a presentable colour — not HUD/orthographic, not unevaluable | `Specular.g` |
| 2 | **self-lit** — no TEV colour stage reads `RASC`/`RASA` | `Specular.a` |
| 3 | **has a colour of its own** — not mixed from the vertex stream (§7c), not a bare `black → white` pass-through | `Specular.b` |
| 4 | that colour reads as a glow: `chroma ≥ 0.50` **or** `luma ≥ 0.70` | `rtx.dusklight.emissive.glowChroma` / `glowLuma` |

Clause 2 is the one that was wrong for two revisions, and the distinction is
subtle enough to be worth opening the code for: "self-lit" is a property of the
TEV *colour program*, not of the channel config's lighting flag.
`dx9_tev.cpp:1234-1245` states it with the measurement that settled it. Clause 3
is at `:1287-1295`, with the count of screen blits it exists to exclude. The
score the fork does *not* decide on, and why, is at `:1180-1187`.

**Replayed over the 2026-08-05 Goron Mines log the rule accepts 6 of 77
materials** — every lava and fire surface in the room plus one bright warm glow
texture, no false positives, nothing to tune.

**Tested in game 2026-08-06 and working.** The rule catches the lava and the
emitted colour carries the texture. `rtx.dusklight.emissive.brightness` was
dialled to **10.0**, now the default, **calibrated in one dark interior** — a
bright exterior may want less and none has been looked at. **Not confirmed:**
whether the derivation holds for a small pickup (the report named the lava
only), and whether any of the six is a false positive outside the mines. Both
are answerable from a `dusklight.emis` log.

### What the glow colour is — a choice, `rtx.dusklight.emissive.colorSource`

GX records nothing about what a self-lit surface should emit, so every answer is
a *reading*. Rev 2 hard-coded one, removing a control the owner was using, which
is why it is an option. The three modes and what each does to the lava are in
the option's own help text,
`dxvk-remix/src/dxvk/rtx_render/rtx_dusklight_emissive.h:162-171`.

**The trap that made mode 2 unusable:** by default the shader re-runs the
emissive colour through the *albedo's* texture op, so a glow set to a colour
arrives as `op(colour, tFactor)`. `RtSurface::emissiveSource` (`textureFlags`
bits 19–20, `kEmissiveSource*` in `surface_shared.h`) is what selects out of
that, so no pre-image inversion is needed.

### Two signals this rule does not use

**Additive blending is the one unambiguous emissive signal, and Remix already
acts on it** — `ONE`/`ONE` blending becomes `BlendType::kEmissive` in the branch
immediately above the Dusklight rule, so such a draw never reaches the rule.
**Whether any draw in this game takes that path is still unmeasured**; `blend=`
on `matrep.sum` answers it from one Goron Mines log, and if flames arrive as
`blend=additive` the rule only ever needed to cover opaque emitters.

**The game's own light lists are the other source, and they are the game's job**
(`dusklight-ao/docs/effect-lights.md`). `dungeonlight[8]` — per-room authored
lights with positions and palette colours — is **still not read at all**, and it
is exactly the Goron Mines case. Older prose here described a point-light
mirror; it was **deleted at protocol 17 on 2026-08-16**, not defaulted off.

### Identification: `grp=` names the material, not the actor

`grp=` on `matrep.sum` carries the material's own authored name —
`Mat:MA00_Gake`, 崖 *gake*, cliff — pushed from `J3DMatPacket::draw`, which
brackets the `callDL()` that writes the draw into the FIFO. It is **off by
default** for cost; `DUSK_MAT_LABELS=1` in the environment turns it on with no
rebuild. The gate, the placement rule and the truncation contract (a trailing
`~` marks a name cut at 48 characters) are all at
`dusklight-ao/libs/JSystem/src/J3DGraphBase/J3DPacket.cpp:220-248`. This is not
cosmetic: **the game's own environment system dispatches on these names at
runtime** (`dKy_bg_MAxx_proc`, `d_kankyo.cpp:11399`), so the classification we
have been inferring from TEV state was authored by hand.

**Which actor issued a draw remains unidentified**, and no name reaches Remix —
that transport is deliberately still open, and would ride the drawmeta flag word
rather than the side band (§2). The always-present identification is the logged
shape — texture size and format, `tfactor`, ramp endpoints — plus `tex0hash`.

## 10. The two-colour ramp — reproduced exactly, 2026-08-04, CI-green and untested in game

The Goron Mines lava material is, in GX,
`cc = [C2, C1, TEXC, ZERO]` → `out = C2*(1-texture) + C1*texture`, with
`C2 = FF0000` and `C1 = FFFE63`. A lerp between two saturated colours, per
channel — and **none of stock Remix's one-stage decode is a lerp between two
constants** (§2). These docs recorded that as a permanent limitation for a week,
and it was not one.

| Reconstruction | red | green | blue |
| :-- | :-- | :-- | :-- |
| truth | 1 | 0.996·t | 0.388·t |
| `ADD(TEXTURE, TFACTOR=C2)` | 1 ✓ | t (0.4% high) | t — **2.6× high** |
| `MODULATE(TEXTURE, TFACTOR=C1)` | t ✗ | 0.996·t ✓ | 0.388·t ✓ |

`ADD` wins on integrated error and is what §7b selects, but its residual is
entirely in blue, so highlights desaturate toward white instead of yellowing —
the "more red than the intense orangeish/yellow" report.

**The fix: stop using one op.** Both endpoints are carried explicitly and the
shader evaluates `albedo = mix(rampLo, rampHi, albedo)` — `a*(1-c) + b*c`, the
GX combiner itself, exact for every ramp material at once.

| Piece | Where | Note |
| :-- | :-- | :-- |
| endpoint 1 | `D3DRS_TEXTUREFACTOR` | already sent; nothing new |
| endpoint 2 | `D3DMATERIAL9::Diffuse.rgb` → `RtSurface::rampOtherColor` → `data15.w` | `data15.w` was permanent zero padding |
| "is a ramp" | `Diffuse.a` → `textureFlags` bit 15 | bits 15-16 were unused |
| "which endpoint tFactor holds" | `Ambient.r` → `textureFlags` bit 16 | |

**The GPU `Surface` struct does not grow** — it is sized to exactly two 128-byte
cachelines and the spare bits were already there. No precision is lost: GX
colour registers are 8 bits per channel and so is the transport.

**When the ramp is declined.** TFACTOR is a shared render state and a real stage
can claim it first, so aurora checks what TFACTOR actually holds rather than
assuming. `ramp=` on `matrep.sum` gives the reason — `tfLow`/`tfHigh`
(reproduced exactly), `tfTaken`, `usesVtx`, `flat`, `unevaluable` — each set at
its own condition in `dx9_tev.cpp:1723-1760`. Declining is always safe: the
material falls back to §7b.

Not covered: multi-stage materials whose colour depends on a previous stage's
result (still `unevaluable`), and replacement materials, where the ramp applies
to the replacement's albedo — the same exposure the existing tFactor multiply
already has. `rtx.dusklight.rampMaterials` turns it off live, which is also how
to A/B it.

## 11. Water — marked by the game, translucent in Remix

**Almost all of this system is documented in the fork's own header.**
`dxvk-remix/src/dxvk/rtx_render/rtx_dusklight_water.h` carries the MAxx taxonomy
and the fall-through to `as<OpaqueMaterialData>()` that made every layer an
opaque white sheet, the 1×1 `0x00FFFFFF` placeholder that made it *white* rather
than merely wrong, and why every constant is an option (`:26-57`); why the
draw's own colour texture must not be used as a normal map, and that overlapping
normal maps do not blend in Remix, so a body of water must present **one**
surface (`:369-372`, `:122`); and why a *capture* cannot express water at all,
which is what `applyToReplacements` exists for (`:183-192`). Only what is not
there is below.

**Status.** The game marks the right draws — **measured in game 2026-08-08**, 11
distinct water materials reached Remix. The layer split is a classifier written
from the game's own names and is **untested**. **The transport changed on
2026-08-11 and is untested**: all three facts now pack into
`D3DMATERIAL9::Power` as `tag * 100 + layer * 10 + role`, where the 2026-08-08
measurement was taken with them in `Ambient.g`/`.b`/`.a` — channels HD texture
packs already owned, so merging water as written would have deleted texture
packs silently. **Regression signature if the packing is wrong:** every water
draw reports `role=none` and water renders exactly as it did before the feature
existed; nothing crashes and nothing logs an error. Decimal rather than bit
fields **on purpose** — a human reads this number in a log far more often than
code does, and `power=921` is legible as MA09 / waves / surface.

**The layer names are the original team's, and reading them changed the
design.** An earlier revision hid by MAxx tag and this document recommended
trying `6` — but MA06 is three different surfaces, so that would have deleted a
lake's waves and its shoreline to be rid of its murk. `hideSurfaceTag` is gone;
`layer=` is the field that decides anything, and **an unrecognised layer is
never hidden**.

| Option | The game's word | What it is |
| :-- | :-- | :-- |
| `hideShimmerLayer` | mera — shimmer | the shimmer / heat-haze pass |
| `hideWavesLayer` | nami — wave | waves (`cc_MA06_nami_v_x`) |
| `hideShorelineLayer` | mizugiwa — water's edge | the shoreline (`cc_MA06_mizugiwa_v_x`) |
| `hideMurkLayer` | nigori — turbidity | the murky body (`cc_MA06_NigoriWater_v_x`) |
| `hideAdditiveLayer` | kasan (加算) — addition | additively blended passes |

**`kasan` is the case worth remembering.** Its blend state was *measured* as
`SRC_ALPHA,ONE` a session before anyone read the word. The name had said it all
along — read the vocabulary first (`dusklight-ao/docs/japanese-naming-remix.md`).

**Reading the log** — three hops, each reported once ever, so one session says
*where* a mark died rather than only that it did:

| Line | From | Means |
| :-- | :-- | :-- |
| `dusk.matname name=… role=… tag=MAxx` | game | recognised, command emitted. Every material, water or not; capped at 512, and that cap has been reached |
| `dx9.water: first SURFACE mark decoded from the FIFO` | game | it survived the FIFO and reached the backend |
| `dx9.water: first SURFACE draw translated (… power …)` | game | translated while marked. **Check the number**: `power=0` under a `dusk.matname` line means the packing lost the mark |
| `dusklight.water tex0hash=… ior=… tag=… layer=… power=…` | Remix | Remix decoded the same three facts and built a translucent material |

`PROJECTED` ends at `dusklight.water.projected … hidden=1` instead;
`dusklight.water.replaced` means a hand-authored replacement claimed the draw
first, which is intended and counted separately so it is never mistaken for the
mark failing. Those hashes are the list to author a normal map against, and are
why a lake comes out in patches — one replaced hash fixes the draws using it and
nothing else.

**The rule this cost two test sessions to learn, and it is not about water.**
*A backend global set from the game thread does not describe the draws around
it.* The FIFO is drained in `end_frame`, so everything in
`command_processor.cpp` — including `apply_tev` — runs after the game thread has
issued every draw in the frame, and a side-band global therefore reads as the
*last* material of the frame for *every* draw in it. Both failed revisions are
that one fact from two sides: set-and-never-cleared turned every material in the
game translucent; set-and-cleared-per-packet produced no water at all, zero
`dusklight.water` lines against 7 materials marked. **Anything that must
describe particular draws has to travel in the command stream** —
`GX_AURORA_SET_VIEW_MTX` was already the precedent, and `grp=` (§9) is the same
lesson one layer down the same pipe.
