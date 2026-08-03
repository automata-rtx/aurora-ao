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

## 7. Why this game hits it so hard

Twilight Princess colours many objects by tinting one shared greyscale texture.
The proof is in the game's own data: the seven rupee colours are seven
byte-identical resource rows — same archive, same model, same animation —
differing only in which animation frame is held. One model, one texture, seven
colours.

So "the texture is greyscale" is *normal and correct* here, and any rule of the
form "an intensity-format texture is not the albedo" is wrong for this game.
`is_color_texture_format()` exists for a real reason (character eyes composite a
colour eyeball with intensity masks) but it must never be read as "a luminance
texture cannot be a material's albedo."

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
