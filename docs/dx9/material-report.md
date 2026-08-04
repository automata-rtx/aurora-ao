# The material report

A log that answers "what happened to this surface's colour?" without anyone
having to look at pixels and describe them.

Read [`remix-material-interface.md`](remix-material-interface.md) first — this
document is the instrument, that one is the system being measured.

---

## Why it exists

The GX → D3D9 → Remix material path had **no instrumentation at all**. Aurora
logged only its own complaints; the fork logged essentially nothing about
material reconstruction. Neither side could observe what the other made of a
material, so every investigation was a chain of inference validated by looking
at the screen — and three such chains produced three incompatible answers, one
of which shipped and did nothing.

**The project rule this encodes: a question we would have to ask the owner is a
defect in the logging.** The owner should be able to play for ten minutes and
hand over two files.

## What it costs

Aurora's half is **always on**, capped at 512 distinct material configurations
per run. Remix's `matrep.rmx` half is behind `rtx.dusklight.matrep` (default off,
`NoSave`), capped at 1024; its `dusklight.emis` half is behind
`rtx.dusklight.emissive.log` (default **on**), capped at 96. A session costs
kilobytes. Every one of them prints a `.trunc` line exactly once if it hits its
cap — so truncation is never silent.

## Where the lines come from

| Line | Emitted by | When |
| :-- | :-- | :-- |
| `matrep.sum` | aurora, `apply_tev` tail | once per distinct GX material config |
| `matrep.gx` | aurora | one per GX TEV stage of that material |
| `matrep.k` | aurora | once, the GX constants + lighting bit |
| `matrep.d3d` | aurora | one per D3D9 stage actually emitted |
| `matrep.rmx` | fork, `processTextures` tail | once per distinct reconstructed material |
| `dusklight.emis` | fork, instance manager | once per distinct self-illumination candidate |

Aurora's lines land in the game log (`<CachePath>/logs/<timestamp>.log`);
Remix's land in `rtx-remix/logs/remix-dxvk.log`. **Both files are needed.**

## Joining the two logs

The two sides cannot share a hash — they compute different things over different
bytes. Join on the **texture pointer**, which is bit-identical on both sides and
printed in the same format by both (bare uppercase, 16 hex digits):

```
aurora   matrep.sum ... hintTex=000001AF128B83A0
aurora   matrep.d3d ... tex=000001AF128B83A0
fork     matrep.rmx ... tex0ptr=000001AF128B83A0
```

*(Until 2026-08-04 aurora printed `0x1af128b83a0` and the fork printed
`000001AF128B83A0`, so the join key did not actually join. Keep these formats
identical if you touch either side.)*

Aurora's texture objects are content-addressed and stable across frames, so a
pointer identifies a texture for the life of the device. It does **not** survive
a device recreation (a window resize), so join within one continuous stretch of
the log.

`matrep.rmx` also prints `tex0hash`, which is the hash Remix shows in its own
texture categorization UI — use that to tie a log line to something on screen.

## Reading `matrep.sum`

This is the line that usually settles the question on its own.

```
matrep.sum mk=… gxStages=1 d3dStages=1 albedoGx=0 albedoMap=GX_TEXMAP0
           albedoTex=32x32 fmt=GX_TF_I8 colorFmt=0
           shape=ramp out0=B80000 out1=FFFFFF usesTex=1 usesVtx=0
           hint=emitted form=add:tint hintTex=000001AF128B83A0 hintLoose=0
           tint=inHint tintVal=FFB80000 tfactor=FFB80000 tfUsed=1
           vtxColor=default-white selfLit=yes emisCol=B80000
```

| Field | Means |
| :-- | :-- |
| `mk` | material key — groups the other lines for this material |
| `gxStages` / `d3dStages` | how much the material shrank in translation |
| `albedoGx` / `albedoMap` | which GX stage and texmap we nominated as the albedo |
| `albedoTex` / `fmt` | the nominated texture's size and GX format |
| `colorFmt` | 1 if that format carries colour, 0 if it is an intensity mask |
| **`shape`** | what the material *is* — see below |
| **`out0` / `out1`** | **the colour the GX program produces where the texture reads black, and where it reads white.** This is ground truth: it is what the surface should look like |
| `usesTex` / `usesVtx` | whether the colour pass reads its texture / the rasterized vertex colour |
| **`alphaScale`** | opacity where the texture's alpha reads full, `FF` when unscaled. Below `FF` means the material fades its texture alpha by a constant, and that scale rides TFACTOR's alpha channel — dropping it made HUD effects draw their whole quad |
| `hint` | whether the hint stage was emitted — see below |
| **`form`** | what the hint advertised — see below |
| `hintLoose` | 1 if suppression was declined *only* because the material is multi-stage |
| `tint` | `inHint` (the hint carries it), `emitted` (a following stage carries it), `none`, or `skip:budget` |
| `tfactor` / `tfUsed` | the per-draw constant Remix will read |
| `vtxColor` | `stream` (real vertex colours), `default-white`, or `matColor` |
| **`selfLit`** | `yes` if GX says this surface takes no light and its colour is authored, otherwise **why not**: `lit` (GX lighting on), `vtxSrc` (unlit but vertex-sourced, so baked lighting), `ortho` (a 2D draw), `noColor` (nothing evaluable to take a colour from) |
| `emisCol` | the colour handed to the fork as the glow, when `selfLit=yes` |

### `shape` values

| Value | Means |
| :-- | :-- |
| `tex` | plain texture, black to white — Remix needs no help |
| `tex*c` | texture × one colour; a multiply reproduces it **exactly** |
| `ramp` | `lerp(out0, out1, texture)` between two real colours — the common case in this game, and only approximable in the one stage Remix reads |
| `flat` | the colour pass never reads the texture |
| `unevaluable` | depends on a previous stage's result, so it cannot be evaluated here |

### `form` values — what the hint told Remix

| Value | Means |
| :-- | :-- |
| `mod:tint` | `TEXTURE × TFACTOR` — exact when the floor is black |
| `add:tint` | `TEXTURE + TFACTOR` — **whenever the floor is a colour**, because a multiply would render that floor black. See `remix-material-interface.md` §7b |
| `mod:vtx` | `TEXTURE × DIFFUSE` — only when the material could not be evaluated at all. Vertex colour is otherwise never advertised (§7c: it carries baked lighting here) |
| `tex` | the texture alone |

**The quickest read: compare `out0`/`out1` against `tfactor`.** If the material
ramps `B80000 → FFFFFF` and `tfactor=FFB80000` with `form=add:tint`, the colour
survived. If `out1` is a strong colour and `tfUsed=0`, it did not.

### `hint` values

| Value | Meaning |
| :-- | :-- |
| `emitted` | we prepended the hint stage; Remix reads `TEXTURE × DIFFUSE` |
| `skip:alreadyPlain` | the real stage already presents the texture plainly |
| `skip:remixReadsItAlready` | **the fix** — Remix decodes this stage correctly, tint included, so the hint was suppressed |
| `skip:noTexture` | the albedo texmap resolved to nothing |
| `skip:unsafeCurrent` | no TEMP register and the chain reads CURRENT |
| `notReached` | the material has no textured stage |

### The grayscale signature

A surface renders greyscale under Remix and correct in raw D3D9 when a colour
term reached identity. In the report that reads:

```
matrep.sum ... shape=ramp out0=B80000 out1=FFFFFF tfUsed=0 form=mod:vtx
matrep.rmx ... albedo="TEX * VertexColor0"
```

The material should ramp from red to white; `tfUsed=0` says no colour reached
Remix, and the reconstruction is the texture times a vertex colour that is
white. Texture × white = the texture, uncoloured.

The fixed version:

```
matrep.sum ... shape=ramp out0=B80000 out1=FFFFFF form=add:tint tfactor=FFB80000
matrep.rmx ... albedo="TEX + tFactor(b80000)"
```

## Reading `matrep.rmx`

```
matrep.rmx id=… first=0 tex0ptr=… tex0hash=…
           cop=Modulate a1=TEX a2=VertexColor0
           tFactor=FFFFFFFF tfBlend=0 stageTf=1 multiTf=1
           vcBaked=1 albedo="TEX * VertexColor0"
```

`albedo="…"` is a literal rendering of the expression the shader will evaluate.
**It is the end of the argument.** `TEX * 1.0` means a colour term was dropped;
`TEX * VertexColor0` with `vtxColor=default-white` upstream means the hint
bleached it; `TEX * tFactor(…)` means the tint survived.

`id` is keyed on the reconstruction *shape* — the texture, the ops and the
argument sources — so one texture used in several contexts produces one line
**per context**, which is exactly the case texture-hash tagging cannot address.

It deliberately **excludes** `tFactor`'s value. Including it (as the first
version did) meant the same few materials were reported hundreds of times,
because the game's tints track fog and time of day: the 2026-08-03 session
produced 828 distinct tFactor values and exhausted the 1024 cap in 14 seconds.
The value is still printed on every line; it just no longer multiplies them.

Two caveats worth knowing:

- This reports the *reconstruction*, not the final shaded surface. A replacement
  material can displace it later.
- `vcBaked` reflects `rtx.vertexColorIsBakedLighting`, which is a **global**
  transform that removes vertex-colour brightness and part of its saturation on
  every surface. It is not per-material, and it is not the grayscale cause.

## Reading `dusklight.emis`

Self-illumination, the fork's half. One line per distinct candidate, **accepted
or rejected**, because a candidate that missed by 0.02 of saturation is a
threshold to move and that is invisible if only acceptances are printed.

```
dusklight.emis mat=… tex0hash=… color=1,0.42,0 luma=0.55 chroma=1
               minLuma=0.25 minChroma=0.2 verdict=emissive applied=1
```

| Field | Means |
| :-- | :-- |
| `mat` | the material data hash — the same value Remix keys its material cache on |
| `tex0hash` | ties the line to something on screen through Remix's texture categorization UI |
| `color` | what aurora said the surface presents, linear 0..1 |
| `luma` / `chroma` | brightness and saturation of that colour, the two numbers the decision turns on |
| `minLuma` / `minChroma` | the live thresholds, printed so an old log can be read without knowing what they were set to |
| `verdict` | `emissive` or `rejected` |
| `applied` | 0 when the verdict was `emissive` but `rtx.dusklight.emissive.enable` is off |

**Aurora's half of the same decision is `selfLit=` on `matrep.sum`.** A surface
that does not glow and has no `dusklight.emis` line at all was rejected on the
game side; check `selfLit=` there for the reason. A surface with a
`verdict=rejected` line was rejected on the threshold, and the numbers on that
line say by how much.

Bounded at 96 distinct candidates, with a `dusklight.emis.trunc` line if that is
reached. The cap is deliberately small: a scene with hundreds of candidates
means the rule is wrong, not that the cap is too low.

The whole design, and what the rule caught when replayed against the
2026-08-03 session, is in
[`remix-material-interface.md`](remix-material-interface.md) §9.

## Reading the other lines

`matrep.gx` shows the material the game asked for — one line per TEV stage, with
GX enum names (`TEXC`, `KONST`, `CPREV`…) taken from `lib/gx/gx_fmt.hpp` so they
can never drift from the enums themselves. `cc=[a,b,c,d]` is the colour pass's
four operands; GX computes `d + (a·(1−c) + b·c)`.

`matrep.k` shows the konst and colour registers — **this is where this game keeps
the colour that distinguishes a green rupee from a red one** — plus `lit=` and
`matSrc=`, the two GX facts the self-illumination rule turns on. `selfLit=` on
`matrep.sum` is those two already interpreted; these are the raw values.

`matrep.d3d` shows what we handed D3D9, which is all Remix ever sees. Compare
its stage 0 against `matrep.rmx` to confirm which stage Remix picked.

## Extending it

Add fields rather than lines, and keep every line one line. The format is
grepped, diffed between runs, and read cold by people who do not have the source
open — which is why the enum names are spelled out instead of being numbers.

If you find yourself wanting to ask the owner to observe something, add it here
instead.
