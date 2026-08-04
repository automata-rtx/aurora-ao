# The GX → D3D9 → Remix material interface

**This is the most misunderstood system in the project.** Three sessions
produced three confident, mutually incompatible explanations of one colour
defect, and the fix that shipped on 2026-07-29 was a no-op. This document exists
so that does not happen a fourth time.

Companions: [`material-report.md`](material-report.md) is the log that lets you
observe this interface instead of reasoning about it.
[`unsupported-effects.md`](unsupported-effects.md) catalogues what cannot cross
it at all. [`gx-to-d3d9-mapping.md`](gx-to-d3d9-mapping.md) is the translation
spec this constrains.

---

## 1. The model, in one paragraph

Aurora reduces a GX TEV program into D3D9 fixed-function texture stages. Remix
does **not execute** those stages. It reads **one** of them, plus two booleans
and one colour, and reconstructs a PBR material by pattern-matching a small set
of recognised shapes. Everything else Aurora emits is invisible to Remix and
exists only to make raw D3D9 rasterization correct.

So the D3D9 stage chain is doing two unrelated jobs at once: it is a *rasterizer
program* and it is a *message to Remix*. Those two jobs want different things,
and most of the defects in this area come from optimising one and damaging the
other.

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
is untouched. It is raster-neutral by construction.

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

Remix reads one op with one constant. Truth is `lerp(floor, top, texture)`.
Neither available op reproduces that, so the choice is about **which end to get
right**, and the answer is settled rather than a matter of taste:

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

## 7c. Vertex colour is baked lighting here — do not forward it

**Corrected 2026-08-04, against a claim these docs carried for weeks.**
`kankyo-remix.md` stated that GX lighting "isn't baked into vertices, so there
is no double-counting risk". Testing `rtx.vertexColorIsBakedLighting` disproved
it: turning that normalisation *off* makes shaded areas visibly **darker**, so
the vertex colours do carry baked lighting and shadow.

A path tracer relights the scene. Handing it baked lighting in the albedo
double-counts, and the shading it computes lands on top of shading that is
already painted in.

So the hint no longer advertises `DIFFUSE` at all. **The real D3D9 stages still
use vertex colour**, so raw D3D9 rasterization is unchanged — only what Remix
reads is different. Remix's own `rtx.vertexColorIsBakedLighting` becomes
irrelevant to the albedo as a result; leave it at its default.

## 8. Design rules, collected

For anyone changing `dx9_tev.cpp` or adding a Remix-facing feature:

1. **Remix reads one stage.** Any meaning placed in a later stage is lost unless
   it is the §4 TFACTOR pattern.
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

## 9. The direction this is going

The interface above is a *lossy channel with a silent failure mode*, and both
halves of it are ours. The intended endpoint is that Aurora stops encoding
material intent into fixed-function stages and hoping Remix's heuristics recover
it, and instead **states** it: a small versioned export from the fork's
`d3d9.dll`, called per draw, carrying the resolved albedo tint and emissive.

That is not built. It is recorded here so that work on the encoding is
understood as maintaining a fallback rather than as the long-term answer. The
fallback still matters — it must stay correct for the case where the game and
the DLL are built from different points.

**Emissive belongs on that same channel.** GX carries a per-draw "this colour
channel is not lit" bit that comes straight out of the model file — authored
intent for a self-lit surface. Aurora decodes it today and uses none of it, and
the fork has no route by which an opaque captured draw can be made emissive at
all. Neither half exists yet; see `dusklight-ao/docs/remix-open-issues.md`.
