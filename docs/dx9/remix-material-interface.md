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
of recognised shapes. Nothing else in the *stage chain* reaches it.

The stage chain is no longer the whole interface. Since 2026-08-04 the fork also
reads several `D3DMATERIAL9` fields that aurora fills in deliberately — that is
how the vertex-colour verdict (§7c), the self-illumination score (§9) and the
two-colour ramp (§10) cross. They are listed in §2. Adding another is a normal
move here, not a last resort: per §0 the fork is ours.

Historically the stage chain was doing two jobs — a *rasterizer program* and a
*message to Remix* — and most defects here came from optimising one and damaging
the other. Per §0 that tension is largely gone: outside the HUD and the alpha
test, **the chain is only a message to Remix**, and where the message cannot be
expressed the answer is to extend Remix rather than to compromise the message.

---

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

`D3DRS_LIGHTING` is off in this backend, so nothing consumes a D3D9 material and
the whole `D3DMATERIAL9` struct was free. Aurora fills it; the fork reads it.
Every one of these exists because the stage chain could not carry the fact, and
each is §0 applied — extend the fork rather than contort the stream.

| Field | Carries | Read in the fork | Detail |
| :-- | :-- | :-- | :-- |
| `Specular.r` | per-draw verdict "the vertex stream is authored material colour" | `d3d9_rtx_utils.cpp` → `isVertexColorBakedLighting` | §7c |
| `Specular.g` | "aurora evaluated a presentable colour for this draw" — 0 for HUD/orthographic and unevaluable draws | `dusklightEmissive::isCandidate` | §9 |
| `Specular.b` | "this material has a colour of its own" — authored in TEV constants, not from the vertex stream and not a bare texture pass-through | `dusklightEmissive::authoredColor` | §9 |
| `Specular.a` | **"this material is self-lit"** — no TEV colour stage reads the rasterized channel | `dusklightEmissive::selfLit` | §9 |
| `Emissive.rgb` | the colour the surface presents — the glow test, and the glow itself under `colorSource=2` | `rtx_instance_manager.cpp` | §9 |
| `Emissive.a` | the self-illumination evidence score, 0..1. **Reported, not used** | — | §9 |
| `Diffuse.rgb` | the ramp endpoint TFACTOR does not hold → `RtSurface::rampOtherColor` | `rtx_instance_manager.cpp` | §10 |
| `Diffuse.a` | "this material is a ramp" → `textureFlags` bit 15 | same | §10 |
| `Ambient.r` | which endpoint TFACTOR holds → `textureFlags` bit 16 | same | §10 |
| `Ambient.g` | 1-based index of the HD texture replacement this draw's albedo wants, 0 for none | `dusklightTexRep::handleFromLegacyMaterial` | [`texture-replacements.md`](texture-replacements.md) |
| `Ambient.b` | the D3D9 stage `Ambient.g` refers to; only meaningful when it is non-zero | `dusklightTexRep::handleForRasterStage` | same |
| `Ambient.a` | what this draw's transparency represents — `GX_AURORA_DRAW_CLASS_*`: 0 none, 1 particle, 2 haze | `rtx_dusklight_transparency.h` → `alphaState.isParticle` | §11 |

**Free channels remaining: `Power`.** Nothing reads it today.
That list is here so the next thing that needs a side channel takes one that is
actually spare — `Ambient.g` and `Ambient.b` were free until 2026-08-05, and
`Ambient.a` until 2026-08-10, and this table is the only place that would have
said so.

Note also that `Ambient.g`/`.b` are the only side-channel fields read on the
**rasterized** path (`D3D9DeviceEx::BindTexture`) as well as the ray-traced one.
Everything else in this table is consumed during material resolution, which UI
draws never reach.

### The three things that do not survive, and are easy to emit by accident

1. **`D3DTSS_CONSTANT` / `D3DTA_CONSTANT`.** Aurora's *second* constant slot.
   Remix never reads it anywhere in its capture path. A colour routed here is
   simply gone.
2. **`D3DTA_TEMP`.** Aurora's scratch register, used by the split-stage
   decomposition. Not decoded as an arg source.
3. **The `D3DTA_COMPLEMENT` and `D3DTA_ALPHAREPLICATE` modifier bits.** An arg
   carrying either is not decoded, even though its base source would have been.

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

Remix will read **one** additional stage, purely to pick up a TFACTOR multiply.
`rtx.enableMultiStageTextureFactorBlending` defaults on.

It matches a `MODULATE`/`2X`/`4X` whose two args are `TFACTOR` and **the
previous stage's output register**. It never matches against `TEXTURE`.

That last clause is load-bearing: a stage reading `MODULATE(TEXTURE, TFACTOR)`
does *not* trigger it — but it does not need to, because that stage is already
decodable on its own (§2). The escape hatch exists for the case where the tint
must be applied to something a previous stage produced.

## 5. The hint stage: what it buys and what it costs

Aurora prepends a synthetic stage advertising
`colour = TEXTURE × DIFFUSE, alpha = TEXTURE`, writing `TEMP` so the real chain
is untouched.

> The `TEMP` trick, and the whole notion of the hint being "raster-neutral",
> dates from when the rasterized image had to stay correct. Per §0 it no longer
> does, outside the HUD — so a future revision is free to write the material it
> wants Remix to read directly into stage 0. That also frees `TFACTOR`, which
> `materialize()` currently hands to the draw's first constant so real stages
> rasterize correctly, and which is the reason a ramp is sometimes declined
> (`ramp=tfTaken`, §10).

**What it fixes** (checkpoint 3.8, all real and all still true): a GX material's
first stage is often *not* the finished albedo. Handed such a stage, Remix
renders the surface black, and — worse — takes that stage's alpha as opacity,
which destroys alpha-tested cutouts. Foliage becomes solid quads; grass
disappears entirely.

**What it costs, and this was missed for months:** the hint *wins* the stage
Remix reads. When the real stage was already being read correctly, the hint
**replaces a correct material with a worse one**. `TEXTURE × DIFFUSE` cannot
express a tint, and Aurora substitutes opaque white for `DIFFUSE` when the mesh
carries no vertex colour — so a tinted luminance texture becomes exactly
grayscale.

**That is the cause of the rupee / heart / lava defect.** Not a channel Remix
cannot decode; a correct material overwritten by a helpful one.

### The rule that follows

> **Emit the hint only when Remix would otherwise read this stage wrongly.**
> It is a repair, not an annotation. Applying a repair to something that is not
> broken is how this class of bug is created.

Aurora implements this as `remix_decodes_albedo()` in `dx9_tev.cpp`: if Remix
will reconstruct the albedo we meant — tint included — the hint is suppressed.
The opacity half is *not* subject to this: alpha must still reach Remix as the
texture's own alpha or cutouts break, so it gates every suppression.

## 6. Where a colour can live, and which ones survive

The game keeps material colour in TEV colour registers (C0–C2) and konst
registers (K0–K3), animated by `.brk` assets. **Aurora sees both** — they reduce
to the same `isConst` operand. Nothing is hidden from us.

The question is only which D3D9 slot a constant lands in:

| Slot | Per | Survives into Remix? |
| :-- | :-- | :-- |
| `D3DRS_TEXTUREFACTOR` | draw | **Yes** |
| `D3DTSS_CONSTANT` | stage | **No** |

> **Design rule: when a material's albedo tint can go to either slot, it must go
> to TFACTOR.** Only one of the two survives. Aurora's `materialize()` gives the
> draw's *first* constant TFACTOR unconditionally, so in practice this means:
> do not let an unimportant constant claim TFACTOR ahead of the albedo tint.
> A third route now exists — give the colour a side channel of its own and read
> it in the fork (§2, §10) — but TFACTOR costs nothing and is already read, so
> spend the channel on what TFACTOR cannot hold.

## 7. The shape this game actually uses — measured, not assumed

**This section replaces an earlier claim that these materials are
"greyscale texture × one tint colour". They are usually not**, and building a
fix on that model is why the 2026-07-29 attempt matched almost nothing.

Measured from the 2026-08-03 session log, over 111 distinct materials, the
dominant shape is a **two-colour ramp**:

```
colour = lerp(colourA, colourB, textureIntensity)
```

The greyscale texture is not a mask waiting to be tinted — it is a *slider
between two authored colours*. That is how one rupee texture yields seven rupee
colours. `texture × colour` is only the **special case where colourA is black**.

The numbers that settle it: with the old "look for a multiply" rule, **104 of
111 materials reported no tint at all**. The 6 that were detected were exactly
the ones whose floor was black — and those came out of Remix correctly coloured.
Same code, same session, split precisely along that line.

Consequences for anything touching this area:

- **Do not pattern-match for a tint.** *Evaluate* the colour pass instead, with
  the texture pinned to black and to white. The two endpoints tell you what the
  material is, and `evaluate_albedo()` in `dx9_tev.cpp` does exactly this.
- **A multiply cannot represent a ramp with a non-black floor.** `tex × C` always
  falls to black where the texture is dark, so multiplying a floor-coloured ramp
  *darkens* it. Remix also decodes `ADD`, which fits that shape: `tex + floor`
  keeps the floor and saturates to white. Pick the op from the endpoints.
  **Since 2026-08-04 this is the fallback path only** — §10 sends both endpoints
  to the fork and the shader evaluates the ramp exactly, so the op choice
  decides only what a *declined* ramp gets.
- **"The texture is greyscale" is normal and correct here.** Any rule of the form
  "an intensity-format texture is not the albedo" is wrong for this game.
  `is_color_texture_format()` exists for a real reason (character eyes composite
  a colour eyeball with intensity masks) but it must never be read as "a
  luminance texture cannot be a material's albedo".
- **A stage can bind a texture and not use it.** Materials here routinely open
  with a setup stage whose colour pass is all `ZERO`, with the real work one
  stage later. Selecting the first *textured* stage rather than the first stage
  that *reads* its texture hands Remix an empty program — the second cause of
  the same defect.

## 7b. Choosing what to advertise — measured 2026-08-04

> **Superseded in part, later the same day.** §10 carries both endpoints to the
> fork and evaluates the ramp exactly, so for any material reporting
> `ramp=tfLow`/`tfHigh` nothing in this section decides what is drawn. It still
> governs every material where the ramp is **declined** (`ramp=tfTaken`,
> `usesVtx`, `flat`, `unevaluable`), which is why the measurements stay.

The one stage carries one op with one constant. Truth is
`lerp(floor, top, texture)`. Neither op reproduces that, so the choice is about
**which end to get right**, and the answer is settled rather than a matter of
taste:

| Floor | Op | Why |
| :-- | :-- | :-- |
| black | `MODULATE(top)` | exact at both ends |
| coloured | **`ADD(floor)`** | a multiply yields black wherever the texture is black, replacing the object's own colour with a hole |
| near-white | `MODULATE` | a descending ramp; `ADD` would drive the whole surface white |

**The evidence.** These materials are additive in GX — the heart is literally
`out = B80000 + 0.25 × texture`, with a colour constant in the `d` term and the
texture only modulating what is added to it. So a multiply is *structurally*
wrong, not mistuned. Advertising one made rupees and hearts render their
highlights black, which is what a multiply does to a coloured floor.

**The accepted cost:** `ADD` cannot reproduce the scale on the texture, so
highlights come out brighter than the original. On a glint that reads as a blown
specular. If that ever needs fixing, the missing datum is the texture's
brightness distribution — add it to the report rather than guessing.

> A previous revision gated `ADD` on the ramp ending near white. That matched
> **5 materials out of 111** and is the reason a fix that "worked" still left
> every item with an inverted highlight. The floor is what decides.

**Opacity follows the same principle.** The hint used to advertise the texture's
alpha unconditionally, which is right for a cutout but drops any constant scale
on it — 18 of 111 materials are `konst × texture-alpha`. A HUD effect fading in
through that konst reached Remix fully opaque and drew its whole quad. The scale
now rides `TFACTOR`'s alpha channel, which the colour tint does not use.

## 7c. Vertex colour: forward it only when GX says it is material colour

**Two corrections, layered.** First (2026-08-04): `kankyo-remix.md` claimed GX
lighting "isn't baked into vertices, so there is no double-counting risk".
Testing `rtx.vertexColorIsBakedLighting` disproved it — turning that
normalisation *off* makes shaded areas visibly **darker**, so the vertex colours
do carry baked lighting. The response then was to stop forwarding `DIFFUSE`
entirely. Second (same day): **that was too blunt**, and it threw away real
material colour.

GX distinguishes the two cases exactly, per draw, in the colour channel's
control (`GXSetChanCtrl`):

| GX state | What the vertex stream is | What we do |
| :-- | :-- | :-- |
| lighting **enabled**, `matSrc = GX_SRC_VTX` | the **material colour** that GX then multiplies by computed lighting — it carries no light of its own | **forward it**; a path tracer supplies the lighting |
| lighting **disabled**, `matSrc = GX_SRC_VTX` | the finished channel output, which is where this game bakes room lighting and shadow | **withhold it** |
| no `CLR0` attribute at all | a constant aurora writes into every vertex, from the channel's material colour register — authored colour by definition | **evaluate it** as part of the material (below) |

The third row was a silent bug worth calling out: `eval_operand` treated
`DIFFUSE` as white unconditionally, so a material whose colour arrived through
that constant evaluated as if it had none. Those materials now evaluate
correctly, which also makes them eligible for the §10 ramp.

Aurora reports the verdict per draw in `D3DMATERIAL9::Specular.r` (§2), and the
fork uses it to set `isVertexColorBakedLighting` per draw instead of taking the
global option — which was necessarily wrong for one of the two cases.
`vtxUse=` on `matrep.sum` prints which of `material`, `bakedLight` or `const`
applied. **CI-green, untested in game** as of 2026-08-04.

**The case that remains a judgement call:** a draw with lighting disabled whose
vertex colour is genuinely authored — a per-vertex tint or fade on an effect,
rather than baked room light. Those are withheld today. If something loses a
colour gradient it should have, that is this, and the log will say `vtxUse=bakedLight`.

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
   strictly better. See §9.
8. **"Remix cannot express X" is a claim about *stock* Remix.** The fork is
   ours. Check whether the constraint is real before designing around it — the
   two-colour ramp was written down as inexpressible and then reproduced
   exactly, a week later than it needed to be (§10).

## 9. Self-illumination — rev 4, tested in game 2026-08-06

GX has **no emissive term**, and two test sessions established something
stronger: **no single GX fact identifies an emitter, and on the one surface this
feature exists for every GX fact reads zero.**

### The three measurements that bracket the problem

| Session | Finding |
| :-- | :-- |
| 2026-08-03, mixed areas | 69 of 117 materials had GX lighting **disabled** — 59% of the scene. "Unlit" is far too broad to act on. |
| 2026-08-04, Goron Mines | Every candidate lava material has `lit=1` — GX lighting **enabled**. "Unlit" also misses the surface the feature exists for. |
| 2026-08-04 + 2026-08-05, same scene twice | The main lava ramp (`mk=D572C706…`, `mk=1C95BA4B…`, 32×32 `GX_TF_IA8`, `tfactor=FFFF0000`, ramp `FF0000 → FFFE63`) scored **0.00** under rev 2's signals. |
| 2026-08-05, replayed over the same log | **The `unlit` signal was measuring the wrong thing.** 36 of 77 materials have a TEV colour program that never reads `GX_CC_RASC`; only 45 have the channel config's lighting flag off; **10 have lighting *on* and still never read it**, and the lava is one of those 10. Its whole colour program is `cc=[C2, C1, TEXC, ZERO]` then a pass-through — there is no raster input anywhere in it. |

That third row is the one that matters. A score of zero here is a **real
answer**, not a missing one, so the cut has to be able to reach zero — and at
zero the score contributes nothing and something else has to carry the rule.

### Status — what is confirmed and what is not

**Tested in game 2026-08-06 and working.** The rule catches the Goron Mines
lava, the emitted colour carries the texture, and the per-material radiance
derivation was reported as "noticeably better" than the flat multiplier it
replaced. `rtx.dusklight.emissive.brightness` was dialled to **10.0**, which is
now the default — 1.0 put an emitter at roughly the brightness of a fully lit
white surface, which is not what a self-lit surface in a dark cave looks like.

Calibrated in **one dark interior**. A bright exterior may want less, and no
exterior emitter has been looked at.

**Not confirmed by that session:** whether the derivation holds for a small
pickup as well as it does for lava — the report named the lava only — and
whether any of the six accepted materials is a false positive outside the mines.
Both are answerable from a `dusklight.emis` log, which prints every emitter's
`radiance=`.

### The rule — self-lit, with a colour of its own

**A surface is self-lit when its TEV colour program never reads the rasterized
channel** (`GX_CC_RASC` / `GX_CC_RASA`). Its colour is then fixed whatever the
lights do — which is what the console draws as full-bright, and what a path
tracer has to *emit* to reproduce. That is the basis.

Rev 2 tested `colorChannelConfig[GX_COLOR0].lightingEnabled` instead, which
describes the *channel* rather than whether the program consumes it, and was
wrong in both directions: it missed the lava (lighting on, never reads `RASC`)
and it scored the full 0.50 for §7c's baked vertex lighting, the opposite of an
emitter. Measured over 77 materials in the 2026-08-05 session — 45 have the
channel flag off, 36 never read the raster channel, and **10 have lighting on
and still never read it**.

**Self-lit is not sufficient**, and the measurement says exactly why. Of the 20
*evaluated* self-lit materials in that session, **9 were EFB copies and
full-screen quads** — 304×224, 608×448, 608×100 `RGBA8`. A white screen blit is
self-lit, and making one emit light would flood the room.

Every one of those is a bare texture pass-through: `out0 = 000000`,
`out1 = FFFFFF`, no colour of its own. Every real emitter carries a colour
**authored in TEV constants over an intensity mask**. So the rule is three
structural facts and one colour test:

| # | Fact | Where it comes from |
| --: | :-- | :-- |
| 1 | aurora evaluated a presentable colour — not HUD/orthographic, not unevaluable | `Specular.g` |
| 2 | **self-lit** — no TEV colour stage reads `RASC`/`RASA` | `Specular.a` |
| 3 | **has a colour of its own** — not mixed from the vertex stream (§7c), not a bare `black → white` pass-through | `Specular.b` |
| 4 | that colour reads as a glow: `chroma ≥ 0.50` **or** `luma ≥ 0.70` | `rtx.dusklight.emissive.glowChroma` / `glowLuma` |

Clause 4 is an **or** on purpose: an authored glow is either a strong colour or
it is near-white-hot, and a muted mid-tone is a surface colour. That single
change is what stopped the brown false positives that had followed this feature
since rev 1.

**There is no score and no threshold.** Two revisions cut on the evidence score
and both missed the lava, which scores 0.00. Aurora still computes and logs the
score because it is free and occasionally informative; nothing decides on it.

### Replayed over the 2026-08-05 Goron Mines log

**6 of 77 materials, no tuning, no false positives:**

| Material | Colour | Luma | Chroma | Shape | Texture |
| :-- | :-- | --: | --: | :-- | :-- |
| `D572C706` | `FF0000` | 0.30 | 1.00 | `ramp` | 32×32 `IA8` |
| `1C95BA4B` | `FF0000` | 0.30 | 1.00 | `ramp` | 32×32 `IA8` |
| `9866CEA2` | `FF0000` | 0.30 | 1.00 | `ramp` | 64×64 `I8` |
| `40D45477` | `FF6B00` | 0.55 | 1.00 | `ramp` | 32×64 `I4` |
| `7E31CACC` | `FF6432` | 0.55 | 0.80 | `ramp` | 32×32 `I8` |
| `EFF68502` | `FFF0A0` | 0.92 | 0.37 | `tex*c` | 128×128 `I4` |

Every lava and fire surface in the room, plus one bright warm glow texture.

Rejections, at the first clause that fired: **30** never evaluated (HUD or
nothing to take a colour from), **27** colour program reads the lit channel,
**9** no colour of their own — every one an EFB copy or full-screen quad — and
**5** a colour that is neither saturated nor bright.

### The transport

`D3DMATERIAL9::Emissive` — RGB the presented colour, **A the evidence score**.
`Specular.g` marks that aurora evaluated the draw at all, `Specular.b` that the
colour is authored. Free end to end: this backend keeps `D3DRS_LIGHTING` off so
nothing reads a D3D9 material, and the fork was already copying the whole struct
unread.

`Specular.g` exists because `Emissive.a` alone could not tell **"aurora scored
this zero"** apart from **"no aurora wrote this material"**, and that ambiguity
is what silently put the main lava out of reach. The fork keeps a `score > 0`
fallback so an older aurora against this fork still behaves as it did.

### What the glow colour is — a choice, `rtx.dusklight.emissive.colorSource`

GX records nothing about what a self-lit surface should emit, so every answer
here is a *reading*. Rev 2 hard-coded one and it was the wrong one; rev 3 makes
it a control.

| Mode | The shader emits | On the lava specifically |
| :-- | :-- | :-- |
| **0 Reconstructed Albedo** (default) | the albedo it has just built, §10 ramp included | `lerp(FF0000, FFFE63, texture)` — the texture drives the colour and both survive. This is the mode that answers "the texture *and* the colour, with neither overpowering the other" |
| 1 Albedo Texture | the albedo texture through the material's own single D3D9 op | the op here is `ADD` against a red TFACTOR, so it emits `texture + red`: red pinned at 1.0, bright end washed to white. It looked right on 2026-08-04 **only because the albedo was that same approximation then** — that preference does not survive the ramp |
| 2 Presented Colour | `Emissive.rgb` verbatim | reported 2026-08-04 as "an almost solid red" — one flat hot colour, no crust |

Mode 2 no longer needs the pre-image inversion rev 2 deleted: `emissiveSource`
(`textureFlags` bits 19–20, `kEmissiveSource*` in `surface_shared.h`) tells the
shader to leave the constant alone, rather than the constant having to survive a
texture op it cannot control.

### What rev 2 got wrong, and how it presented

Rev 2 removed the `useTextureColor` toggle on the reasoning that a flat constant
swamps the albedo, so taking the reconstructed albedo must be strictly better.
Two errors in that:

1. **It removed a control the owner was using.** "Strictly better" was a
   prediction about untested code, and the only tested configuration was the one
   deleted.
2. **It did not apply to the surface in question.** The main lava scores 0.00,
   so at the shipped default of 0.70 no emissive path ran on it at all —
   `emissiveSource` was never set, `intensity` had nothing to scale, and the
   surface rendered as pure albedo.

The 2026-08-05 report — "stuck at an almost solid red without proper texture
definition, no option to change it, intensity does nothing regardless of value"
— is all three of those at once, and it is what a **non-emissive** §10 ramp
looks like in a dark cave: `mix(FF0000, FFFE63, texture)` as a *diffuse
reflectance*, where the red channel is pinned at 1.0 across the whole surface
because both endpoints have `R = 255`.

### The signals we have not been using — and one that was already live

"GX has no emissive term" is true of the *material* format and was allowed to
stand in for a much broader claim, which is wrong: this game visibly has glowing
things, so something in its data produces them. There are at least three
mechanisms, and the TEV-state score is the weakest of them.

**1. Additive blending — the one unambiguous signal, and Remix already acts on
it.** `GX_BM_BLEND` with `GX_BL_ONE`/`GX_BL_ONE` means *add this draw to what is
already in the framebuffer*. That is emission, stated by the hardware, with no
inference. It is how a console-era renderer does a torch flame, a light shaft, a
glow halo or a magic effect.

Verified 2026-08-05 by reading both sides:

- aurora translates it faithfully — `apply_blend_state` in `dx9_draw.cpp` maps
  `GX_BM_BLEND` to `D3DRS_BLENDOP`/`SRCBLEND`/`DESTBLEND` with no special-casing.
- the fork already converts `ONE`/`ONE` to `BlendType::kEmissive`
  (`calculateAlphaState`), and `enableEmissiveBlendEmissiveOverride` replaces the
  material with an emissive one **in the else-if branch immediately above the
  Dusklight rule** — so an additive draw never reaches the score at all.

So that path has been live the whole time. **What is not known is whether any
draw in this game takes it**, because until 2026-08-05 neither log printed the
blend state. `blend=` on `matrep.sum` and `matrep.rmx` closes that; a Goron
Mines log now answers it directly. If flames and glow halos are arriving as
`blend=additive`, then the score only ever needed to cover *opaque* emitters
like the lava surface, and its poor showing is much less surprising.

**2. The game's own light lists.** `g_env_light` carries `pointlight[100]`
(torches, braziers, candles, campfires), `efplight[5]` (effects) and
`dungeonlight[8]` (per-room authored lights, positions and palette colours). The
first two are already forwarded to Remix as sphere lights
(`dusklight-ao/src/dusk/remix_bridge.cpp`). **`dungeonlight` is not**, and it is
exactly the Goron Mines case: authored light sources placed in a dungeon room.

This is §1 "translate, don't tag" applied where it actually belongs — the game
holds a *list of light sources*, and the emissive work has been trying to infer
them from TEV state instead. It is complementary rather than an alternative: a
light makes the room correct, surface emission makes the lava *look* hot, and
running both without care double-counts.

**3. Actor identity.** All three repos are ours. A draw issued by
`d_a_obj_lv3Water` or a fire actor could be marked at the source, per draw,
which is translation rather than tagging. Nothing here needs GX to have recorded
anything.

None of 1–3 is built beyond what is noted above; 1 is instrumented and 2 is
a decision about double-counting, not a research question.

### Identification: `grp=` does not work

`matrep.sum` carries a `grp=` field intended to name the game code that issued a
draw. **It has printed `-` for every material in every session so far.**

Cause, verified by reading the game: `fpcDw_Execute` is where a draw is
*scheduled*, not issued. TP actor draw methods call `mDoExt_modelEntryDL`, which
enters models into a J3D draw buffer walked later by `dDlst_list_c` — long after
the debug group has been popped. The hook was removed rather than left as a dead
instrument; `dusklight-ao/src/f_pc/f_pc_draw.cpp` carries a note recording why.

A correct implementation labels at draw-buffer *execution*, carrying the label
from registration. Nobody has built it. Until then, **the way to identify a
material is its logged shape** — texture size and format, `tfactor`, and the
ramp endpoints, all of which are in `matrep.sum` and now in `dusklight.emis`
too.

### What is still not built

The endpoint remains a small versioned export from the fork's `d3d9.dll`,
called per draw, carrying resolved albedo and emissive directly. Emissive has a
channel now, but it is a repurposed D3D9 field carrying one float of score and
two flags. That is a fallback working hard, not a stated intent.

**The nearer prize was §7's two-colour ramp, and it has been taken** — §10 shows
what a purpose-built channel plus a shader change buys, and it is the pattern
this export would generalise.

## 10. The two-colour ramp — reproduced exactly, 2026-08-04, CI-green and untested in game

Measured 2026-08-04, the Goron Mines lava material is, in GX:

```
cc = [C2, C1, TEXC, ZERO]   ->   out = C2*(1-texture) + C1*texture
C2 = FF0000 (red)   C1 = FFFE63 (bright yellow)
```

A lerp between two saturated colours, per channel. §2 lists what the one-stage
decode reads — args `TEXTURE`, `DIFFUSE`, `TFACTOR`, `CURRENT`, ops reducing to
`t·C` or `t + C` — and **none of it is a lerp between two constants**. That is a
fact about *stock* Remix's stage decode; these docs recorded it as a permanent
limitation for a week, and it was not one:

| Reconstruction | red | green | blue |
| :-- | :-- | :-- | :-- |
| truth | 1 | 0.996·t | 0.388·t |
| `ADD(TEXTURE, TFACTOR=C2)` | 1 ✓ | t (0.4% high) | t — **2.6× high** |
| `MODULATE(TEXTURE, TFACTOR=C1)` | t ✗ | 0.996·t ✓ | 0.388·t ✓ |

`ADD` wins on integrated error (0.125 vs 0.333) and is what §7b selects, but its
residual is entirely in blue, so **highlights desaturate toward white instead of
yellowing** — the surface reads red-and-white. That was the 2026-08-04 report,
"more red than the intense orangeish/yellow". `MODULATE` is not the answer
either: it renders black where the floor is a colour, which is the
inverted-highlight defect §7b exists to remove.

### The fix: stop using one op

Both halves are ours, so the ramp is now carried explicitly and evaluated in the
shader as what GX actually computes:

```
albedo = mix(rampLo, rampHi, albedo);   // per channel, albedo = the texture sample
```

That is `a*(1-c) + b*c` — the GX colour combiner, not an approximation of it.
It is exact for **every** ramp material at once, not just lava.

### How it fits with no room to spare

| Piece | Where | Note |
| :-- | :-- | :-- |
| endpoint 1 | `D3DRS_TEXTUREFACTOR` | already sent; nothing new |
| endpoint 2 | `D3DMATERIAL9::Diffuse.rgb` → `RtSurface::rampOtherColor` → `data15.w` | `data15.w` was permanent zero padding |
| "is a ramp" | `Diffuse.a` → `textureFlags` bit 15 | bits 15-16 were unused |
| "which endpoint tFactor holds" | `Ambient.r` → `textureFlags` bit 16 | |

**The GPU `Surface` struct does not grow.** It is sized to exactly two 128-byte
cachelines and the spare bits were already there. No precision is lost either:
GX colour registers are 8 bits per channel and so is the transport.

### When the ramp is declined

TFACTOR is a **shared render state** and a real stage can claim it before the
hint does. Aurora therefore checks what TFACTOR actually ended up holding rather
than assuming, and declines when it is neither endpoint. `ramp=` on
`matrep.sum` gives the reason:

| `ramp=` | Means |
| :-- | :-- |
| `tfLow` / `tfHigh` | reproduced exactly; TFACTOR holds the texture-black / texture-white endpoint |
| `tfTaken` | a real stage claimed TFACTOR, so only one endpoint is reachable — falls back to §7b |
| `usesVtx` | the colour pass reads the rasterized vertex colour, so two constants do not describe it |
| `flat` / `unevaluable` | nothing to ramp between |

Declining is always safe: the material falls back to the single-op
approximation, which is what shipped before this.

### What this does not cover

- **Multi-stage materials.** Only the pass aurora evaluates is reproduced; a
  material whose colour depends on a previous stage's result is still
  `unevaluable` (§7).
- **Replacement materials.** If one displaces the legacy material, the ramp
  applies to the replacement's albedo — the same exposure the existing tFactor
  multiply already has, not a new one.
- `rtx.dusklight.rampMaterials` turns it off live, which is also how to A/B it
  against the approximation.

---

## 11. Transparency: what a blended draw *is* — 2026-08-10

Written after two complaints that turned out to be one mechanism: enemy death
smoke is noisy and its transparency reads wrong, and the layered fog wall in
front of Death Mountain is noisy and reads wrong in a different way.

**Implemented, syntax-unverified (no MinGW in the session container), untested
in game.** The decode in §11.4 is the one part that was executed — see there.

### 11.1 Remix has two transparency renderers, and picks by texture tag

An alpha-blended D3D9 draw goes down one of two very different paths, chosen in
`rtx_instance_manager.cpp`, `setAlphaState`:

```cpp
out.isParticle = drawCall.testCategoryFlags(InstanceCategories::Particle);
```

That flag comes from **texture categorisation** (`rtx.particleTextures`) and
nothing else. What each path then does:

| | Untagged (today's default) | Tagged a particle |
| :-- | :-- | :-- |
| TLAS | primary, non-opaque | separate **unordered** TLAS |
| How layers combine | a **stochastic pick** — one layer survives per pixel per frame (`resolve.slangh`, `handleStochasticAlphaBlend`; `rtx.enableStochasticAlphaBlend` defaults **true**) | all layers accumulated in one order-independent traversal (`resolveVertexUnordered`) |
| Where its light comes from | a search for a neighbouring **opaque** pixel, borrowing its denoised radiance; the volumetric cache only if that search fails (`composite_alpha_blend.comp.slang`) | the volumetric radiance cache at the particle's own position (`evaluateOpaqueApproximations` → `evalVolumetricNEE`) |

For a dense stack of smoke quads the untagged path is *one random layer a frame,
lit by whatever solid thing happens to be near it on screen*. That is the noise
and the wrong-looking transparency, and they are the same bug.

**Why tagging cannot fix it here.** Rule 1: a tag is one answer per texture and
this game reuses textures across contexts. Worse, several of these draws land
after Remix's RTX injection boundary, where they are never categorised at all —
so the dev-menu tagging UI cannot reach exactly the draws that need it
(`dusklight-ao/docs/remix-open-issues.md` issue 6).

### 11.2 So the game says it per draw

`GXSetDrawClass(GX_AURORA_DRAW_CLASS_*)` → `g_gxState.drawClass` →
`D3DMATERIAL9::Ambient.a` → `DusklightTransparency::treatAsParticle` → OR'd into
`out.isParticle`. Three values: `none` (0), `particle` (1), `haze` (2).

Placed in `m_Do_graphic.cpp` around the six `if (fapGmHIO_getParticle())` blocks
that draw JPA groups under a **perspective** projection. The 2D groups
(`draw2Dgame`, `draw2Dback`, `draw2Dfore`, `draw2Dmenu*`) are deliberately left
unclassified: Remix rasterizes those and they are not world volumes.

A draw carrying no class reads 0 and takes the stock path, so **an older game
build against a newer DLL renders exactly as it does today.** That is why this
needed no protocol bump — it is not part of the `rtx.dusklight.env` push
contract. (It was also worth avoiding: `claude/remix-sphere-lights-system-0j781o`
is unmerged at protocol **11** while `Fixed-Function-dev` is at 7, so 8–11 are
spoken for. Checked 2026-08-10.)

### 11.3 Why `haze` exists but is not promoted by default

The obvious move is to promote both classes and be done. It would make the fog
wall worse, and the reason is a number:

- The froxel grid is capped at `rtx.dusklight.atmosphere.froxelMaxDistanceMaxMeters`
  = **120 m** (`rtx_dusklight_atmosphere.cpp`, `target = clamp(end * scale, min, max)`).
- `viewZToDepthSlice` **saturates** (`froxel.slangh`): a surface past the grid is
  lit as though it stood at the grid's last slice.
- Everything beyond that is instead carried by the game's own fog ramp in the
  composite, whose handover *is* `froxelMaxDistance` (`DusklightAtmosphere.md` §5.2).

Death Mountain is far beyond 120 m. So the split is by distance:

| Class | Where it lives | Path | Why |
| :-- | :-- | :-- | :-- |
| `particle` | near — an enemy dies next to you | unordered TLAS | inside the grid, so the volumetric lighting is real |
| `haze` | hundreds of metres out | composite (unchanged) | outside the grid; promoting it would trade a stochastic pick for a *saturated* froxel lookup **and lose the far fog ramp**, which only the composite path applies |

`rtx.dusklight.transparency.hazeAsParticle` (default false) exists to A/B that
claim in one session rather than argue it.

### 11.4 The other half of the fix, in the fork

The far fog ramp had never been applied to the transparent layer at all —
`applyFog` in `composite.comp.slang` only ever touched `radianceOutput`. So a
distant transparency kept whatever radiance the neighbour search gave it and
**never faded**, while the opaque geometry around it faded correctly. In the far
field that reads as the transparency being wrong rather than as fog being
missing: a fog wall stays crisp against a mountain that has properly dissolved
into the sky.

The ramp is now factored into `dusklightFarFogRamp()` and run for the
alpha-blend layer too, before the coverage weight and before the near in-scatter
is added — which is stricter than the opaque path manages, because there the two
arrive already summed.

**This half is unconditional and needs no classification**, so it applies to
every transparency including ones the game never labels.

### 11.5 How to tell whether any of it happened

- `matrep.sum … class=` — per material: `none`, `particle`, `haze`.
  `class=none` on a draw you expected classified means the game-side call is
  missing or cleared too early. Check that before looking at the fork.
- `dusklight.xparency frames=… classifiedFrames=… particleDraws=… hazeDraws=…
  promotedToUnordered=…` — one bounded line per 600 frames from the fork, behind
  `rtx.dusklight.transparency.reportClasses`. `classifiedFrames=0` while
  transparencies are plainly on screen is the signature of a game build that
  predates `GXSetDrawClass`.

**Regression signatures, so they are recognised rather than discovered:**

- Smoke that was noisy becomes smooth but *flat* — the unordered path lights from
  the volumetric cache, which is isotropic. Expected, and correct for smoke;
  if it reads too dim the cache is the thing to look at, not this.
- Smoke disappears in a room where the volumetrics are off — the unordered path
  has no other light source. `rtx.volumetrics.enable` is the first check.
- Distant transparencies suddenly *over*-fog — the §11.4 ramp is now reaching
  something it should not. `rtx.dusklight.atmosphere.enable = False` isolates it.
- Anything at all changes with `class=none` everywhere in the log — then it was
  not this change, because every path is gated behind a non-zero class.

### 11.6 What is not done

- **Distant particles are still lit from a saturated froxel lookup** if anything
  ever is promoted past 120 m. Closing that properly means either carrying the
  Dusklight fog args into `RaytraceArgs` so the unordered path can run the same
  ramp, or raising the grid cap — the first is correct, the second is cheap.
  Neither was attempted here: `resolve.slangh` is the hottest shared shader path
  in the runtime and this session could not compile it, let alone run it.
- **The fog wall is not identified in the game source.** `haze` is plumbed end to
  end and nothing calls it. Identifying the actor is what `class=` and the
  `dusklight.xparency` line are for; adding the call afterwards is one line and
  one `GXScopedDrawClass`.
