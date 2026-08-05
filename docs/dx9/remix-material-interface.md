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
| `Emissive.rgb` | the colour the surface presents — used for the brightness/saturation cut only, not as the glow | `rtx_instance_manager.cpp` | §9 |
| `Emissive.a` | the self-illumination evidence score, 0..1 | cut at `rtx.dusklight.emissive.threshold` | §9 |
| `Diffuse.rgb` | the ramp endpoint TFACTOR does not hold → `RtSurface::rampOtherColor` | `rtx_instance_manager.cpp` | §10 |
| `Diffuse.a` | "this material is a ramp" → `textureFlags` bit 15 | same | §10 |
| `Ambient.r` | which endpoint TFACTOR holds → `textureFlags` bit 16 | same | §10 |

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

## 9. Self-illumination — rev 2, 2026-08-04, CI-green and untested in game

GX has **no emissive term**, and the 2026-08-04 Goron Mines session established
something stronger: **no single GX fact identifies an emitter.**

### The two measurements that bracket the problem

| Session | Finding |
| :-- | :-- |
| 2026-08-03, mixed areas | 69 of 117 materials had GX lighting **disabled** — 59% of the scene. "Unlit" is far too broad to act on. |
| 2026-08-04, Goron Mines only | Every candidate lava material has `lit=1` — GX lighting **enabled**. "Unlit" also misses the one surface this feature exists for. |

So the first revision's rule (`unlit AND register-sourced`) was wrong from both
ends. In the Goron Mines it fired on exactly one material — a brown `965744`
that had already been flagged as the likeliest false positive — and on nothing
else in the room.

### What replaced it: a score, not a predicate

Aurora sums the evidence GX does carry and the fork picks where to cut.

| Evidence | Weight | Why it is evidence |
| :-- | --: | :-- |
| GX lighting disabled for the colour channel | 0.50 | the surface takes no light |
| colour authored in a register, not per-vertex | 0.25 | not §7c's baked lighting |
| a TEV stage scaled past what the console could display (`GX_CS_SCALE_2/4`) | 0.25 | **the only thing in GX that states "brighter than the display"** |

`rtx.dusklight.emissive.threshold` defaults to 0.70, which preserves the old
behaviour (unlit plus one other fact). Dropping it to 0.20 admits the
over-range materials on their own. It is a live overlay control precisely
because nobody can derive the right cut from first principles.

Replayed against the Goron Mines log after the luma/chroma filter: the old rule
gave 2 materials, over-range alone gave 3 (two of them warm 128×128 masks),
and the union gave 5. **Which of those is the lava is still not established** —
see the identification section below, which exists to end that question.

### The transport, and the trap in it

`D3DMATERIAL9::Emissive` — RGB the presented colour, **A the evidence score**.
Free end to end: this backend keeps `D3DRS_LIGHTING` off so nothing reads a
D3D9 material, and the fork was already copying the whole struct unread.

**What the glow colour is.** Not a constant — the **reconstructed albedo**.
A self-lit surface glows the colour it appears, and after §10 that colour is
already computed one block earlier in the shader, so the emissive path just
takes it (`surface.emissiveFollowsAlbedo`, `textureFlags` bit 19).

That was learned the expensive way on 2026-08-04. The first version set
`emissiveColorConstant` to the presented colour, which meant inverting it
through the albedo's texture op first, because the shader re-applies that op to
whatever is set there. It worked and still looked wrong: **a flat colour at
intensity 2 swamps the albedo**, so the Goron Mines lava read as one uniform hot
wash with no crust, described in testing as "an almost solid red". Taking the
albedo removed the constant, the inversion, the `invertible=` log field and the
`useTextureColor` option in one go.

### Identification: `grp=`

The recurring blocker has not been the rule, it is that **nobody can tell which
logged material is the lava.** That is now a log field.

The game pushes one `GXPushDebugGroup` per process draw, labelled with the
process name, at the single funnel every draw passes through
(`fpcDw_Execute`, `dusklight-ao/src/f_pc/f_pc_draw.cpp`). Aurora mirrors the
innermost label into `GXState::currentDebugGroup()` and `matrep.sum` prints it
as `grp=`. So every material now carries the name of the game code that drew
it, permanently, for every future material question — not just this one.

Cost: one short string per drawn process per frame. If frame time regresses
noticeably, this is the first thing to suspect.

### Which endpoint becomes the glow

The more chromatic endpoint of the ramp, ties going to the brighter one — the
same rule §7b uses for the albedo tint.

The glow is not a separate colour at all — see "What the glow colour is" above.

### What is still not built

The endpoint remains a small versioned export from the fork's `d3d9.dll`,
called per draw, carrying resolved albedo and emissive directly. Emissive has a
channel now, but it is a repurposed D3D9 field carrying one float of score.
That is a fallback working hard, not a stated intent.

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
