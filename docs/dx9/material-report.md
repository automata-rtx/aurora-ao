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
per run. Remix's half is behind `rtx.dusklight.matrep` (default off, `NoSave`),
capped at 1024. A session costs kilobytes. Both print a `matrep.trunc` line
exactly once if they hit the cap — so truncation is never silent.

## Where the lines come from

| Line | Emitted by | When |
| :-- | :-- | :-- |
| `matrep.sum` | aurora, `apply_tev` tail | once per distinct GX material config |
| `matrep.gx` | aurora | one per GX TEV stage of that material |
| `matrep.k` | aurora | once, the GX constants + lighting bit |
| `matrep.d3d` | aurora | one per D3D9 stage actually emitted |
| `matrep.rmx` | fork, `processTextures` tail | once per distinct reconstructed material |

Aurora's lines land in the game log (`<CachePath>/logs/<timestamp>.log`);
Remix's land in `rtx-remix/logs/remix-dxvk.log`. **Both files are needed.**

## Joining the two logs

The two sides cannot share a hash — they compute different things over different
bytes. Join on the **texture pointer**, which is bit-identical on both sides:

```
aurora   matrep.d3d ... tex=0x000001F4A2B30040
fork     matrep.rmx ... tex0ptr=0x000001F4A2B30040
```

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
           hint=skip:remixReadsItAlready hintTex=0x0 hintLoose=0
           tint=detected tintVal=00FF3020 tfactor=FF00FF30 tfUsed=1
           vtxColor=default-white
```

| Field | Means |
| :-- | :-- |
| `mk` | material key — groups the other lines for this material |
| `gxStages` / `d3dStages` | how much the material shrank in translation |
| `albedoGx` / `albedoMap` | which GX stage and texmap we nominated as the albedo |
| `albedoTex` / `fmt` | the nominated texture's size and GX format |
| `colorFmt` | 1 if that format carries colour, 0 if it is an intensity mask |
| `hint` | **the decision that matters** — see below |
| `hintLoose` | 1 if suppression was declined *only* because the material is multi-stage |
| `tint` | whether an albedo tint was detected, and whether a stage carried it |
| `tfactor` / `tfUsed` | the per-draw constant Remix will read, if any |
| `vtxColor` | `stream` (real vertex colours), `default-white`, or `matColor` |

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
matrep.sum ... fmt=GX_TF_I8 colorFmt=0 hint=emitted ... vtxColor=default-white
matrep.rmx ... albedo="TEX * VertexColor0"
```

The texture is a luminance mask, the hint fired, and Remix is multiplying it by
a vertex colour that is white. Texture × white = the texture, uncoloured.

The fixed version:

```
matrep.sum ... hint=skip:remixReadsItAlready tfactor=FF00FF30
matrep.rmx ... albedo="TEX * tFactor(00FF30)"
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

`id` is keyed on the reconstruction inputs, not on the texture — so one texture
used in several contexts produces one line **per context**, which is exactly the
case texture-hash tagging cannot address.

Two caveats worth knowing:

- This reports the *reconstruction*, not the final shaded surface. A replacement
  material can displace it later.
- `vcBaked` reflects `rtx.vertexColorIsBakedLighting`, which is a **global**
  transform that removes vertex-colour brightness and part of its saturation on
  every surface. It is not per-material, and it is not the grayscale cause.

## Reading the other lines

`matrep.gx` shows the material the game asked for — one line per TEV stage, with
GX enum names (`TEXC`, `KONST`, `CPREV`…) taken from `lib/gx/gx_fmt.hpp` so they
can never drift from the enums themselves. `cc=[a,b,c,d]` is the colour pass's
four operands; GX computes `d + (a·(1−c) + b·c)`.

`matrep.k` shows the konst and colour registers — **this is where this game keeps
the colour that distinguishes a green rupee from a red one** — plus `lit=`, the
GX colour-channel lighting bit that indicates a self-lit surface.

`matrep.d3d` shows what we handed D3D9, which is all Remix ever sees. Compare
its stage 0 against `matrep.rmx` to confirm which stage Remix picked.

## Extending it

Add fields rather than lines, and keep every line one line. The format is
grepped, diffed between runs, and read cold by people who do not have the source
open — which is why the enum names are spelled out instead of being numbers.

If you find yourself wanting to ask the owner to observe something, add it here
instead.
