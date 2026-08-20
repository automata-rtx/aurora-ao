# The material report

A log that answers "what happened to this surface's colour?" without anyone
having to look at pixels and describe them. Read
[`remix-material-interface.md`](remix-material-interface.md) first — this
document is the instrument, that one is the system being measured.

**The project rule this encodes: a question we would have to ask the owner is a
defect in the logging.** The owner should be able to play for ten minutes and
hand over two files. Before this existed, the GX → D3D9 → Remix material path
had no instrumentation at all, and three chains of inference produced three
incompatible answers, one of which shipped and did nothing.

**When a log leads you back into game code**, remember its names are romanized
Japanese: `kankyo` (環境) is *environment*, `fpcDw_` is the process-control draw
layer, and the tree spells some words two ways so one search finds half a
feature. `dusklight-ao/docs/japanese-naming-remix.md`.

## Where the lines come from, and what they cost

| Line | Emitted by | When |
| :-- | :-- | :-- |
| `matrep.sum` | aurora, `apply_tev` tail | once per distinct GX material config |
| `matrep.gx` | aurora | one per GX TEV stage of that material |
| `matrep.k` | aurora | once, the GX constants + lighting bit |
| `matrep.d3d` | aurora | one per D3D9 stage actually emitted |
| `matrep.rmx` | fork, `processTextures` tail | once per distinct reconstructed material |
| `dusklight.emis` | fork, instance manager | once per distinct self-illumination candidate |

Aurora's half is **always on**, capped at 512 distinct configurations per run.
`matrep.rmx` is behind `rtx.dusklight.matrep` (default off, `NoSave`), capped at
1024; `dusklight.emis` is behind `rtx.dusklight.emissive.log` (default **on**),
capped at 96. A session costs kilobytes. **Every one of them prints a `.trunc`
line exactly once if it hits its cap, so truncation is never silent.**

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
`000001AF128B83A0`, so the join key did not actually join. **Keep these formats
identical if you touch either side.**)*

Aurora's texture objects are content-addressed and stable across frames, so a
pointer identifies a texture for the life of the device — but **not** across a
device recreation (a window resize), so join within one continuous stretch of
the log. `matrep.rmx` also prints `tex0hash`, which is the hash Remix shows in
its own texture categorization UI: that is how a log line ties to something on
screen.

## Reading `matrep.sum`

This is the line that usually settles the question on its own.

```
matrep.sum mk=… gxStages=1 d3dStages=1 albedoGx=0 albedoMap=GX_TEXMAP0
           albedoTex=32x32 fmt=GX_TF_I8 colorFmt=0
           shape=ramp out0=B80000 out1=FFFFFF usesTex=1 usesVtx=0
           hint=emitted form=add:tint hintTex=000001AF128B83A0 hintLoose=0
           tint=inHint tintVal=FFB80000 tfactor=FFB80000 tfUsed=1
           vtxColor=default-white selfLit=noRas+reg emisScore=0.75
           emisCol=B80000 emisEval=1 emisAuthored=1 blend=opaque ras=0
           ramp=tfLow rampOther=F84040 vtxUse=const texrep=0 grp=-
```

| Field | Means |
| :-- | :-- |
| `mk` | material key — groups the other lines for this material |
| `gxStages` / `d3dStages` | how much the material shrank in translation |
| `albedoGx` / `albedoMap` | which GX stage and texmap we nominated as the albedo |
| `albedoTex` / `fmt` | the nominated texture's size and GX format |
| `colorFmt` | 1 if that format carries colour, 0 if it is an intensity mask |
| **`shape`** | what the material *is* — `tex` (plain, black to white), `tex*c` (texture × one colour, a multiply reproduces it exactly), **`ramp`** (`lerp(out0, out1, texture)`, the common case here), `flat` (the colour pass never reads the texture), `unevaluable` (depends on a previous stage) |
| **`out0` / `out1`** | **the colour the GX program produces where the texture reads black, and where it reads white.** Ground truth: what the surface should look like |
| `usesTex` / `usesVtx` | whether the colour pass reads its texture / the rasterized vertex colour |
| **`alphaScale`** | opacity where the texture's alpha reads full, `FF` when unscaled. Below `FF` means a constant fade rides TFACTOR's alpha — dropping it made HUD effects draw their whole quad |
| `hint` | `emitted`, or why it was skipped: `skip:alreadyPlain`, **`skip:remixReadsItAlready`** (the 2026-08-03 fix — Remix decodes this stage correctly, tint included), `skip:noTexture`, `skip:unsafeCurrent`, `notReached` |
| **`form`** | what the hint advertised: `mod:tint` (`TEXTURE × TFACTOR`, exact when the floor is black), `add:tint` (`TEXTURE + TFACTOR`, whenever the floor is a colour — §7b there), `mod:vtx` (only when the material could not be evaluated), `tex` |
| `hintLoose` | 1 if suppression was declined *only* because the material is multi-stage |
| `tint` | `inHint`, `emitted` (a following stage carries it), `none`, or `skip:budget` |
| **`hintTex`** | the texture the hint stage bound, bare uppercase 16-hex. **The join key** to `matrep.rmx tex0ptr`. `0` means no hint was emitted |
| `tintVal` | the tint colour, `AARRGGBB`. Meaningful only when `tint` is not `none`; compare against `tfactor` to see which stage carried it |
| `tfactor` / `tfUsed` | the per-draw constant Remix will read |
| `vtxColor` | `stream` (real vertex colours), `default-white`, or `matColor` |
| **`vtxUse`** | what GX says that stream *is*, and so what the fork does with it: `material` (forwarded), `bakedLight` (withheld), `const` (evaluated into the material). Rides `D3DMATERIAL9::Specular.r`; §7c there |
| **`texrep`** | the 1-based HD-replacement index for the albedo texture, `0` for none. Rides `Ambient.g`, with its stage in `Ambient.b`. A non-zero value with nothing sharper on screen means the fork did not resolve it, which its own `texrep.rmx` counters separate. See [`texture-replacements.md`](texture-replacements.md) |
| **`grp`** | the material's own authored name, e.g. `Mat:MA00_Gake` — the name the original artists gave the surface, and the one the game's environment code dispatches on. **Off by default**; `DUSK_MAT_LABELS=1` in the environment turns it on with no rebuild. `-` means labels are off or the draw carries none; a trailing `~` marks a truncated name. The gate, its cost and why it is pushed from `J3DMatPacket::draw` rather than a draw-scheduling point are at `dusklight-ao/libs/JSystem/src/J3DGraphBase/J3DPacket.cpp:220-248` |
| **`selfLit`** | which self-illumination evidence fired: `noRas`, `reg`, `over`, a `+`-joined combination, `none`, or `ortho` / `noColor` if excluded before scoring. **`noRas` means the TEV colour program never reads the lit channel** — not that lighting is off, which is a different thing and was scored by mistake until 2026-08-05 |
| `emisScore` | that evidence summed, 0..1. **Reported, not used** — three revisions cut on it and all three missed the lava, which scores 0.00 |
| `emisCol` | the colour aurora says the surface presents |
| `emisEval` | 1 when aurora evaluated a presentable colour here at all. 0 for HUD/orthographic and unevaluable draws. **This is what lets a score of zero still be a candidate** |
| `emisAuthored` | 1 when the material has a colour of its own — not mixed from the vertex stream, not a bare `black → white` pass-through. That last case is every EFB copy and full-screen quad |
| **`ras`** | 1 when some TEV colour stage reads `GX_CC_RASC`/`RASA`. Compare against `lit=` on `matrep.k`: they disagreed on 10 of 77 materials in one session, and where they disagree **this one is what the surface actually does** |
| **`blend`** | `off`, `opaque`, `alpha`, **`additive`**, **`additiveAlpha`**, `multiply`, `subtract`, `logic`, `other`. The two bold ones are the only unambiguous "this emits light" statement in GX, and Remix acts on them *before* the emissive rule runs |
| **`ramp`** | whether the two-colour ramp is reproduced exactly: `tfLow` / `tfHigh` yes; `tfTaken` / `usesVtx` / `flat` / `unevaluable` no, meaning the single-op approximation is in play |
| `rampOther` | the endpoint TFACTOR does not carry |

**The quickest read: check `ramp=` first.** `tfLow`/`tfHigh` means the material
is reproduced *exactly* and `form=` describes only the fallback that would have
been used. Anything else means the approximation is live — then compare
`out0`/`out1` against `tfactor`, and if `out1` is a strong colour with
`tfUsed=0`, no colour reached Remix at all.

### The grayscale signature

A surface renders greyscale under Remix and correct in raw D3D9 when a colour
term reached identity. In the report that reads:

```
matrep.sum ... shape=ramp out0=B80000 out1=FFFFFF tfUsed=0 form=mod:vtx
matrep.rmx ... albedo="TEX * VertexColor0"
```

The material should ramp from red to white; `tfUsed=0` says no colour reached
Remix, and the reconstruction is the texture times a white vertex colour. Fixed,
it reads `form=add:tint tfactor=FFB80000` and `albedo="TEX + tFactor(b80000)"`.

## Reading `matrep.rmx`

```
matrep.rmx id=… first=0 tex0ptr=… tex0hash=…
           cop=Modulate a1=TEX a2=VertexColor0
           tFactor=FFFFFFFF tfBlend=0 stageTf=1 multiTf=1
           vcBaked=1 blend=opaque ramp=1 rampTfHigh=0 rampOther=fffe63
           albedo="TEX * VertexColor0"
```

`albedo="…"` is a literal rendering of the expression the shader will evaluate —
**unless `ramp=1` on the same line**, in which case the shader evaluates
`mix(lo, hi, TEX)` between `tFactor` and `rampOther`, `rampTfHigh` saying which
is which, and this string is only the fallback it replaced. `rampOther` is
printed because "the ramp reached this surface" and "the ramp reached it with
the right second endpoint" are different claims, and only the second is worth
anything.

Otherwise it is the end of the argument. `TEX * 1.0` means a colour term was
dropped; `TEX * VertexColor0` with `vtxColor=default-white` upstream means the
hint bleached it; `TEX * tFactor(…)` means the tint survived.

`blend=additive` or `additiveAlpha` means **Remix's own emissive-blend override
claims that draw**, one branch before the Dusklight rule runs, so that
material's `dusklight.emis` score is irrelevant to it.

`id` is keyed on the reconstruction *shape* — texture, ops, argument sources and
blend class — so one texture used in several contexts produces one line **per
context**, which is exactly the case texture-hash tagging cannot address. It
deliberately **excludes** `tFactor`'s value: including it meant the same few
materials were reported hundreds of times, because the game's tints track fog
and time of day. The value is still printed; it just no longer multiplies lines.

Two caveats: this reports the *reconstruction*, not the final shaded surface (a
replacement material can displace it later), and `vcBaked` — the transform that
removes vertex-colour brightness — is decided **per draw** from aurora's
`vtxUse=` verdict, falling back to the global `rtx.vertexColorIsBakedLighting`
only where aurora says the stream is not authored material colour. It is not the
grayscale cause either way.

## Reading `dusklight.emis`

Self-illumination, the fork's half. One line per distinct candidate, **accepted
or rejected**, because a candidate that missed by 0.02 of saturation is a
threshold to move and that is invisible if only acceptances are printed.

```
dusklight.emis mat=… tex0hash=… color=1,0.42,0 score=0.75 luma=0.55 chroma=1
               selfLit=1 authored=1 ramp=1 rampOther=fffe63 tFactor=ffff0000
               glowChroma=0.5 glowLuma=0.7 src=albedo
               verdict=emissive
```

| Field | Means |
| :-- | :-- |
| `mat` | the material data hash — the value Remix keys its material cache on |
| `tex0hash` | ties the line to something on screen through Remix's texture UI |
| `color` | what aurora said the surface presents, linear 0..1 |
| `score` | aurora's summed GX evidence, the same number as `emisScore` upstream. **Reported, not used** |
| `selfLit` | 1 when no TEV colour stage reads the rasterized channel. The basis of the rule |
| `luma` / `chroma` | brightness and saturation of that colour |
| `authored` | aurora's `emisAuthored` as the fork read it |
| `ramp` / `rampOther` / `tFactor` | the ramp as it reached the fork. Under `src=albedo` these decide what the surface glows, so they are printed here rather than left to be joined by hand |
| `glowChroma` / `glowLuma` / `src` | the two glow constants and the colour source, printed so an old log can be read without knowing what they were set to. The two are **or**'d |
| `verdict` | `emissive` or `rejected` |

**A surface with no `dusklight.emis` line at all was never evaluated by aurora**
(`emisEval=0` — HUD, or nothing to take a colour from), and `selfLit=` on
`matrep.sum` says why. A `verdict=rejected` line failed one of the three facts
or the glow test, and the numbers on the line say which. With
`rtx.dusklight.emissive.enable` off, no line is written at all.

**Two bounding rules, both of which cost a session's evidence before they
existed:**

- Candidates rejected on colour alone are **counted, not enumerated** — a
  `dusklight.emis.grey distinct=N` line, re-emitted on each doubling. On
  2026-08-04 ninety chroma-zero lines spent the entire 96 cap before the player
  reached the lava, so the one question the log existed to answer went
  unanswered.
- Moving any emissive control **clears the memory and reports everything
  again**. Before that, dialling a threshold mid-session produced no new lines
  and what the setting actually did was unrecoverable.

## Reading the other lines

`matrep.gx` shows the material the game asked for — one line per TEV stage, with
GX enum names taken from `lib/gx/gx_fmt.hpp` so they can never drift from the
enums. `cc=[a,b,c,d]` is the colour pass's four operands; GX computes
`d + (a·(1−c) + b·c)`.

`matrep.k` shows the konst and colour registers — **this is where this game keeps
the colour that distinguishes a green rupee from a red one** — plus `lit=` and
`matSrc=`, the two GX facts that decide `vtxUse=` and carry two of the three
self-illumination signals. `selfLit=` and `vtxUse=` are those already
interpreted; these are the raw values.

`matrep.d3d` shows what we handed D3D9 through the texture stages. Compare its
stage 0 against `matrep.rmx` to confirm which stage Remix picked. It is **not**
the whole message any more: the vertex-colour verdict, the emissive facts and
the ramp endpoints ride `D3DMATERIAL9` fields the fork reads directly
(`remix-material-interface.md` §2), and those appear on `matrep.sum`.

`dx9.draws` is not part of the material report and answers a different question —
**how many D3D9 draw calls did a frame cost:**

```
dx9.draws frames=… mean=412 peak=1387 - D3D9 draw calls per frame
```

Counted over a fixed period (`kDrawStatsPeriod`), covering every
`DrawPrimitiveUP` / `DrawIndexedPrimitiveUP` the backend issues, including each
draw of a palette-split skinned mesh. `peak` is reported separately because the
spike that matters only exists while something dense is on screen and a mean
hides it entirely. **Read it when performance is the question rather than
colour** — Remix charges per draw, not per pixel, and a `peak` in the thousands
is the signature ([`design-decisions.md`](design-decisions.md)).

## Extending it

Add fields rather than lines, and keep every line one line. The format is
grepped, diffed between runs, and read cold by people who do not have the source
open — which is why the enum names are spelled out instead of being numbers.
`scripts/check_invariants.py` fails if `matrep.sum` emits a field this document
has no row for.

If you find yourself wanting to ask the owner to observe something, add it here
instead.
