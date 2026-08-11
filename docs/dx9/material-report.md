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
           vtxColor=default-white selfLit=noRas+reg emisScore=0.75
           emisCol=B80000 emisEval=1 emisAuthored=1 blend=opaque ras=0
           ramp=tfLow rampOther=F84040 vtxUse=const texrep=0 class=none phase=bgXlu grp=-
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
| **`hintTex`** | the D3D9 texture the hint stage bound, as a bare uppercase 16-hex pointer. **This is the join key** between this line and the fork's `matrep.rmx tex0ptr` — the two sides once formatted it differently and the join silently did not join. `0` means no hint stage was emitted |
| `tintVal` | the tint colour itself, `AARRGGBB`. Meaningful only when `tint` is not `none`; compare against `tfactor` to see whether the hint carried it or a following stage did |
| `tfactor` / `tfUsed` | the per-draw constant Remix will read |
| `vtxColor` | `stream` (real vertex colours), `default-white`, or `matColor` |
| **`vtxUse`** | what GX says that stream *is*, and therefore what the fork does with it: `material` (lighting enabled → authored colour, forwarded), `bakedLight` (lighting disabled → finished output, withheld), `const` (no `CLR0`, evaluated into the material). Sent per draw in `D3DMATERIAL9::Specular.r`; see `remix-material-interface.md` §7c |
| **`texrep`** | the 1-based HD-replacement index for the texture the fork will treat as this material's albedo, `0` if the pack has none for it. Sent per draw in `D3DMATERIAL9::Ambient.g`, with the stage it refers to in `Ambient.b`. This is the join key between a log line here and what Remix substituted — a non-zero value with nothing sharper on screen means the fork did not resolve it, which its own `texrep.rmx` counters separate. See `texture-replacements.md` |
| **`class`** | what the game declared this draw represents, via `GXSetDrawClass`: `none`, `particle` (smoke, dust, an explosion puff — many overlapping quads whose blended sum is the effect), or `haze` (layered translucent scenery standing in for distance). Sent per draw in `D3DMATERIAL9::Ambient.a`; the fork turns anything but `none` into `alphaState.isParticle`, which moves the draw out of the stochastic alpha-blend path and into the unordered TLAS. `none` on a draw you expected to be classified means the game-side call is missing or was cleared too early — that is the first thing to check before looking at the fork. See `remix-material-interface.md` §11 |
| **`phase`** | which of the game's draw lists issued this draw, set by `GXSetDrawPhase` around the draw-list call itself: `skyOpa`, `skyXlu`, `bgOpa`, `bgXlu`, `middle`, `actorOpa`, `actorXlu`, `zxlu`, `filter`, `invisible`, `screen`, `last3D`, `ui2D`, or `none`. **This is the working replacement for `grp`**, and it works for the reason `grp` does not: it is written into the FIFO where commands are *issued* rather than where a draw is *scheduled*, and it is not compiled out in release. Diagnostic only — nothing renders differently because of it. Sent in the high byte of `D3DMATERIAL9::Ambient.a`, packed above the draw class |
| **`grp`** | which piece of game code drew this, from a `GXPushDebugGroup` the game has open. **Currently always `-`** — see below |
| **`selfLit`** | which self-illumination evidence fired: `noRas`, `reg`, `over`, or a `+`-joined combination; `none` if none did; `ortho` / `noColor` if the material was excluded before scoring. **`noRas` means the TEV colour program never reads the lit channel** — not that lighting is off, which is a different thing and was scored by mistake until 2026-08-05 |
| `emisScore` | that evidence summed, 0..1. **Reported, not used** — three revisions cut on it and all three missed the lava, which scores 0.00. What the fork decides on is `emisEval`, `ras`, `emisAuthored` and the colour |
| `emisCol` | the colour aurora says the surface presents |
| `emisEval` | 1 when aurora evaluated a presentable colour here at all. 0 for HUD/orthographic and unevaluable draws, which the fork never considers however low its threshold goes. This is what lets a score of zero still be a candidate |
| `emisAuthored` | 1 when the material has a colour of its own — authored in TEV constants, not mixed from the vertex stream and not a bare `black → white` texture pass-through. That last case is every EFB copy and full-screen quad, and excluding it is what keeps a screen blit from lighting the room |
| **`ras`** | 1 when some TEV colour stage reads `GX_CC_RASC`/`RASA`. Compare against `lit=` on the `matrep.k` line: they disagreed on 10 of 77 materials in one session, and where they disagree **this one is what the surface actually does** |
| **`blend`** | the framebuffer blend: `off`, `opaque`, `alpha`, **`additive`**, **`additiveAlpha`**, `multiply`, `subtract`, `logic`, `other`. The two bold ones are the only unambiguous "this emits light" statement in GX, and Remix acts on them *before* the emissive score is consulted — see `remix-material-interface.md` §9 |
| **`ramp`** | whether the two-colour ramp is reproduced exactly (§10): `tfLow` / `tfHigh` yes, `tfTaken` / `usesVtx` / `flat` / `unevaluable` no. Anything but `tfLow`/`tfHigh` means the material fell back to the single-op approximation |
| `rampOther` | the endpoint TFACTOR does not carry |

### `shape` values

| Value | Means |
| :-- | :-- |
| `tex` | plain texture, black to white — Remix needs no help |
| `tex*c` | texture × one colour; a multiply reproduces it **exactly** |
| `ramp` | `lerp(out0, out1, texture)` between two real colours — the common case in this game. Reproduced **exactly** by the fork when `ramp=tfLow`/`tfHigh`; approximated in the one stage only when the ramp is declined (§10) |
| `flat` | the colour pass never reads the texture |
| `unevaluable` | depends on a previous stage's result, so it cannot be evaluated here |

### `form` values — what the hint told Remix

| Value | Means |
| :-- | :-- |
| `mod:tint` | `TEXTURE × TFACTOR` — exact when the floor is black |
| `add:tint` | `TEXTURE + TFACTOR` — **whenever the floor is a colour**, because a multiply would render that floor black. See `remix-material-interface.md` §7b |
| `mod:vtx` | `TEXTURE × DIFFUSE` — only when the material could not be evaluated at all. The hint does not otherwise reach for vertex colour; whether the *stream* is forwarded is a separate, per-draw decision that GX answers (§7c), reported as `vtxUse=` |
| `tex` | the texture alone |

**The quickest read: check `ramp=` first.** `tfLow` or `tfHigh` means the
material is reproduced *exactly* and `form=` no longer describes what the shader
evaluates — it is the fallback that would have been used. Anything else means
the approximation is in play, and then `form=` matters: compare `out0`/`out1`
against `tfactor`. If `out1` is a strong colour and `tfUsed=0`, no colour
reached Remix at all.

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
           vcBaked=1 blend=opaque ramp=1 rampTfHigh=0 rampOther=fffe63
           albedo="TEX * VertexColor0"
```

`albedo="…"` is a literal rendering of the expression the shader will evaluate —
**unless `ramp=1` on the same line**, in which case the shader evaluates
`mix(lo, hi, TEX)` between `tFactor` and `rampOther` — `rampTfHigh` says which
is which — and this string is only the fallback it replaced. `rampOther` is
printed because "the ramp reached this surface" and "the ramp reached it with
the right second endpoint" are different claims, and only the second one is
worth anything.
Otherwise it is the end of the argument. `TEX * 1.0` means a colour term was dropped;
`TEX * VertexColor0` with `vtxColor=default-white` upstream means the hint
bleached it; `TEX * tFactor(…)` means the tint survived.

`blend=additive` or `additiveAlpha` on a line means **Remix's own emissive-blend
override claims that draw**, one branch before the Dusklight rule runs. Such a
material is already emissive and its `dusklight.emis` score is irrelevant to it.

`id` is keyed on the reconstruction *shape* — the texture, the ops, the argument
sources and the blend class — so one texture used in several contexts produces
one line **per context**, which is exactly the case texture-hash tagging cannot
address. Blend is in the key because the same texture drawn opaque and drawn
additively are different materials to Remix, and collapsing them would hide the
draw the field was added to find.

It deliberately **excludes** `tFactor`'s value. Including it (as the first
version did) meant the same few materials were reported hundreds of times,
because the game's tints track fog and time of day: the 2026-08-03 session
produced 828 distinct tFactor values and exhausted the 1024 cap in 14 seconds.
The value is still printed on every line; it just no longer multiplies them.

Two caveats worth knowing:

- This reports the *reconstruction*, not the final shaded surface. A replacement
  material can displace it later.
- `vcBaked` is the transform that removes vertex-colour brightness and part of
  its saturation. Since 2026-08-04 it is decided **per draw** from aurora's
  `vtxUse=` verdict (`D3DMATERIAL9::Specular.r`), falling back to the global
  `rtx.vertexColorIsBakedLighting` only where aurora says the stream is not
  authored material colour — a single global answer was necessarily wrong for
  one of the two cases. It is not the grayscale cause either way.

### `grp=` does not work — verified 2026-08-04, unchanged 2026-08-05

It was added to end the recurring "which of these logged materials is the thing
on screen?" question, and it does not. Every material in a Goron Mines session
reported `grp=-`.

**The hook was in the wrong place, and the reason generalises.** It pushed a
group in `fpcDw_Execute`, which is the funnel every process draw is
*scheduled* through — not the one GX commands are *issued* through. TP actor
draw methods call `mDoExt_modelEntryDL`, which enters the model into a J3D draw
buffer; the FIFO writes happen later when `dDlst_list_c` walks that buffer, by
which time the group has been popped.

The push has been removed rather than left in place looking functional. Doing
it properly means labelling where the draw buffer is *executed*, with the label
carried there from registration — real J3D plumbing, not a one-liner.

Until then, identifying a material means the texture hash (`tex0hash` on
`matrep.rmx`, which Remix's own texture UI shows) or its colour signature.

## Reading `dusklight.emis`

Self-illumination, the fork's half. One line per distinct candidate, **accepted
or rejected**, because a candidate that missed by 0.02 of saturation is a
threshold to move and that is invisible if only acceptances are printed.

```
dusklight.emis mat=… tex0hash=… color=1,0.42,0 score=0.75 luma=0.55 chroma=1
               selfLit=1 authored=1 ramp=1 rampOther=fffe63 tFactor=ffff0000
               glowChroma=0.5 glowLuma=0.7 src=albedo
               verdict=emissive applied=1
```

| Field | Means |
| :-- | :-- |
| `mat` | the material data hash — the same value Remix keys its material cache on |
| `tex0hash` | ties the line to something on screen through Remix's texture categorization UI |
| `color` | what aurora said the surface presents, linear 0..1 |
| `score` | aurora's summed GX evidence, the same number as `emisScore` upstream. **Reported, not used** — two revisions cut on it and both missed the lava, which scores 0.00 |
| `selfLit` | 1 when no TEV colour stage reads the rasterized channel. This is the basis of the rule |
| `luma` / `chroma` | brightness and saturation of that colour |
| `authored` | aurora's `emisAuthored` as the fork read it |
| `ramp` / `rampOther` / `tFactor` | the §10 ramp as it reached the fork. Under `src=albedo` these decide what the surface glows, so they are printed here rather than left to be joined by hand from two logs |
| `glowChroma` / `glowLuma` / `src` | the two glow constants and the colour source, printed so an old log can be read without knowing what they were set to. The two are **or**'d: an authored glow is a strong colour or it is near-white-hot |
| `verdict` | `emissive` or `rejected` |
| `applied` | 0 when the verdict was `emissive` but `rtx.dusklight.emissive.enable` is off |

**Aurora's half of the same decision is `selfLit=` / `emisScore=` on
`matrep.sum`.** A surface with no `dusklight.emis` line at all was never
evaluated by aurora (`emisEval=0` — HUD, or nothing to take a colour from);
`selfLit=` there says why. A `verdict=rejected` line failed one of the
three facts or the glow test, and the numbers on the line say which.

**Two bounding rules, both of which cost a session's evidence before they
existed:**

- Candidates rejected on colour alone are **counted, not enumerated** — a
  `dusklight.emis.grey distinct=N` line, re-emitted on each doubling. On
  2026-08-04 ninety chroma-zero lines spent the entire 96 cap before the player
  reached the lava, so the one question the log existed to answer went
  unanswered.
- Moving any emissive control **clears the memory and reports everything
  again**. Before that, once-per-material meant dialling a threshold mid-session
  produced no new lines, and what the setting actually did was unrecoverable.

The informative cap is 96, with a `dusklight.emis.trunc` line if it is reached.

The whole design, and what the rule caught when replayed against the 2026-08-04
Goron Mines session, is in
[`remix-material-interface.md`](remix-material-interface.md) §9.

## Reading the other lines

`matrep.gx` shows the material the game asked for — one line per TEV stage, with
GX enum names (`TEXC`, `KONST`, `CPREV`…) taken from `lib/gx/gx_fmt.hpp` so they
can never drift from the enums themselves. `cc=[a,b,c,d]` is the colour pass's
four operands; GX computes `d + (a·(1−c) + b·c)`.

`matrep.k` shows the konst and colour registers — **this is where this game keeps
the colour that distinguishes a green rupee from a red one** — plus `lit=` and
`matSrc=`, the two GX facts that decide `vtxUse=` (§7c) and that carry two of the
three self-illumination signals (§9; the third is the TEV over-range scale).
`selfLit=` and `vtxUse=` on `matrep.sum` are those already interpreted; these are
the raw values.

`matrep.d3d` shows what we handed D3D9 through the texture stages, which is all
Remix reconstructs a material from. Compare its stage 0 against `matrep.rmx` to
confirm which stage Remix picked. It is **not** the whole message any more: the
vertex-colour verdict, the emissive score and the ramp endpoints ride
`D3DMATERIAL9` fields the fork reads directly (`remix-material-interface.md` §2),
and those appear on `matrep.sum` rather than here.

`dx9.draws` is not part of the material report and answers a different question —
**how many D3D9 draw calls did a frame cost**:

```
dx9.draws frames=600 mean=412 peak=1387 - D3D9 draw calls per frame
```

One line every 600 frames, counting every `DrawPrimitiveUP` /
`DrawIndexedPrimitiveUP` the backend issues, including each draw of a
palette-split skinned mesh. `peak` is reported separately because the spike that
matters only exists while something dense is on screen — weather, a crowd of
effects — and a mean over 600 frames hides it entirely.

Read it when performance is the question rather than colour. Remix charges per
draw, not per pixel: a draw too small for its own BLAS still contributes its own
geometry entry and surface to a bucket that rebuilds every frame. A game-side
loop that wraps each quad in its own `GXBegin`/`GXEnd` therefore costs one draw
per quad, which is how the kankyo weather effects came to spend ~1000 draws a
frame ([`progress.md`](progress.md) §3.32). If a dense effect is slow, this line
is the first thing to look at, and a `peak` in the thousands is the signature.

## Extending it

Add fields rather than lines, and keep every line one line. The format is
grepped, diffed between runs, and read cold by people who do not have the source
open — which is why the enum names are spelled out instead of being numbers.

If you find yourself wanting to ask the owner to observe something, add it here
instead.
