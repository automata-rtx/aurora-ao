# In-flight allocation: side channels and FIFO opcodes

**Audited 2026-08-11.** Investigation only — nothing in this document changed
either feature's behaviour.

This document exists because two shared, finite resources are being allocated by
branches that cannot see each other:

- the `D3DMATERIAL9` side band, whose field map is
  [`remix-material-interface.md`](remix-material-interface.md) §2
- the aurora GX FIFO subcommand space, `include/dolphin/gx/GXAurora.h`

Both are now over-subscribed, and in both cases the collision survives a `git
merge` — one because the obvious conflict resolution is the wrong one, the other
because the place that matters does not conflict at all. That is the failure
CLAUDE.md's "Merges that succeed and are still wrong" describes, twice, live.

**Everything below was read from the branches, not recalled.** Line numbers are
from the commits fetched on 2026-08-11.

---

## 1. `D3DMATERIAL9` side channels

### 1.1 What `Fixed-Function-dev` uses today

`lib/dx9/dx9_internal.hpp:222-239`:

```cpp
inline void set_remix_material(const D3DCOLORVALUE& emissive, const D3DCOLORVALUE& ramp,
                               float tFactorIsHigh, float vertexColorIsMaterial,
                               float evaluated, float colorAuthored, float selfLit,
                               uint32_t texRepIndex, uint32_t texRepStage) noexcept {
  D3DMATERIAL9 mat{};
  mat.Emissive = emissive;
  mat.Diffuse  = ramp;
  mat.Ambient  = D3DCOLORVALUE{tFactorIsHigh, static_cast<float>(texRepIndex),
                               static_cast<float>(texRepStage), 0.f};
  mat.Specular = D3DCOLORVALUE{vertexColorIsMaterial, evaluated, colorAuthored, selfLit};
```

| Field | Carries | Read in the fork | Status |
| :-- | :-- | :-- | :-- |
| `Specular.r` | vertex stream is authored material colour | `d3d9_rtx_utils.cpp` → `isVertexColorBakedLighting` | CI-green, untested in game |
| `Specular.g` | aurora evaluated a presentable colour | `rtx_dusklight_emissive.h:188` | tested 2026-08-06 |
| `Specular.b` | material has a colour of its own | `rtx_dusklight_emissive.h:203` | tested 2026-08-06 |
| `Specular.a` | material is self-lit | `rtx_dusklight_emissive.h:195` | tested 2026-08-06 |
| `Emissive.rgb` | the colour the surface presents | `rtx_dusklight_emissive.h:207` | tested 2026-08-06 |
| `Emissive.a` | evidence score — reported, nothing decides on it | `rtx_dusklight_emissive.h:178` | tested 2026-08-06 |
| `Diffuse.rgb` | the ramp endpoint TFACTOR does not hold | `rtx_dusklight_emissive.h:163` | CI-green, untested in game |
| `Diffuse.a` | this material is a ramp | `rtx_dusklight_emissive.h:153` | CI-green, untested in game |
| `Ambient.r` | which endpoint TFACTOR holds | `rtx_dusklight_emissive.h:157` | CI-green, untested in game |
| **`Ambient.g`** | **HD texture pack index, 1-based** | `rtx_dusklight_texrep.cpp:75` | **tested good in game 2026-08-06** |
| **`Ambient.b`** | **the D3D9 stage `Ambient.g` refers to** | `rtx_dusklight_texrep.cpp:90` | **tested good in game 2026-08-06** |
| `Ambient.a` | — written as literal `0.f` | nothing | nominally spare |
| `Power` | — not written at all | nothing | nominally spare |

`Ambient.g`/`.b` are the only two read on the **rasterized** path
(`D3D9DeviceEx::BindTexture`) as well as the ray-traced one. Losing them breaks
HD texture packs in the world *and* in the HUD, and the HUD half is the one that
lives in a file this fork otherwise never touches.

### 1.2 Who claims what on the unmerged branches

Three parties want the same space. Read from each branch's `set_remix_material`:

| Channel | `Fixed-Function-dev` | `claude/water-…-7baezw` | `claude/dusklight-remix-transparency-e7l766` |
| :-- | :-- | :-- | :-- |
| `Ambient.r` | `tFactorIsHigh` | `tFactorIsHigh` | `tFactorIsHigh` |
| `Ambient.g` | **`texRepIndex`** | **`isWaterSurface`** ⚠ | `texRepIndex` (unchanged) |
| `Ambient.b` | **`texRepStage`** | **`isWaterProjected`** ⚠ | `texRepStage` (unchanged) |
| `Ambient.a` | *spare* | **`waterTag`** ⚠ | **`drawClass`** ⚠ |
| `Power` | *spare* | **`waterLayer`** | *spare* |

- water: `lib/dx9/dx9_internal.hpp:223-247`, fork side `rtx_dusklight_water.h:202,209,216,230`
- transparency: `lib/dx9/dx9_internal.hpp:227-245`, fork side `rtx_dusklight_transparency.cpp:60`

**Two distinct collisions:**

1. **`Ambient.g` + `.b`: water vs. the shipped, tested texrep feature.** The
   water branch's signature has no `texRepIndex`/`texRepStage` parameters at
   all — it forked before texrep landed and reclaims both channels.
2. **`Ambient.a`: water vs. transparency.** Both unmerged, both taking the same
   nominally-spare channel, neither able to see the other.

### 1.3 The answer to "how many channels are free"

**Zero, once water and transparency land — and the second of them to be written
was already planning on space that was gone.**

`Ambient.a` and `Power` are the only two the §2 table has ever called spare.
`Ambient.a` is claimed twice over and `Power` once. Any proposal elsewhere that
assumes "there is a spare side channel" is planning on space that no longer
exists; it needs either a packed encoding (§1.6) or a different transport.

### 1.4 Why the merge is dangerous — measured, not predicted

`git merge origin/claude/water-rendering-investigation-7baezw` into
`Fixed-Function-dev`, run 2026-08-11:

```
Auto-merging docs/dx9/remix-material-interface.md     <- CLEAN
CONFLICT (content): Merge conflict in lib/dx9/dx9_internal.hpp
CONFLICT (content): Merge conflict in lib/dx9/dx9_tev.cpp
```

Correcting one detail of the original report: git **does** conflict, in two
files, and the hunk is a short block rather than a single line. That does not
make it safe — it makes it worse in the documented way, because **the conflict
is not the trap; the resolution is.** Both sides are self-consistent, the water
side is newer and carries a paragraph of reasoning, and taking it drops a
feature that is not mentioned anywhere in the conflict.

The doc, meanwhile, **merges clean** — the water branch never edited those rows,
so §2 keeps describing `Ambient.g`/`.b` as HD texture packs while the code
underneath them writes water flags.

**What the invariants script did about it, before 2026-08-11.** Taking the water
side of both conflicted files and leaving everything else auto-merged:

```
[side-channels] dx9_internal.hpp writes D3DMATERIAL9::Ambient.a but ... has no row for it
[draw-stats]    could not find kDrawStatsPeriod in dx9_internal.hpp
```

Neither message names texrep. Silencing them the obvious way — add a §2 row for
`Ambient.a`, restore the constant — leaves the tree **green**, with §2 still
advertising `Ambient.g`/`.b` as HD texture packs, the code writing water flags
into them, and `grep -r texRepIndex lib/` returning **zero hits**. That was
measured, not reasoned about.

Two further facts found while measuring:

- **The water branch predates `scripts/check_invariants.py` entirely** — the
  file does not exist on it, so nothing has ever checked it. Its own §2 does not
  document `Ambient.g`, `.b`, `.a` or `Power` either; the branch is inconsistent
  with itself, not only with `Fixed-Function-dev`.
- **Taking water's `dx9_internal.hpp` wholesale also reverts `kDrawStatsPeriod`**
  — the `dx9.draws` instrumentation from 2026-08-08. A third regression riding
  the same one-file resolution, and the only one the old script named.

### 1.5 Being behind is not the risk; editing the same hunk while behind is

Three other live branches also predate texrep — `remix-sphere-lights-system-0j781o`,
`lss-hair-rtx-remix-j6uo4t`, `aurora-dx9-rtx-remix-wdxo21`. **They are not the
same hazard.** Merging sphere-lights into `Fixed-Function-dev` was tested:
`dx9_internal.hpp` merges clean and `texRepIndex` survives, because that branch
never touched the function. Only the water branch *rewrote* the signature while
behind, which is what turns staleness into a conflict whose resolution loses
work.

### 1.6 Recommended resolution for the water branch

**Rebase onto `Fixed-Function-dev` and re-derive the assignment against the
current signature. Do not resolve the conflict by hand.**

Resolving by hand asks whoever does it to notice, from a diff that never
mentions texrep, that two parameters went missing. Rebasing puts the water
commits on top of a `set_remix_material` that already has `texRepIndex` and
`texRepStage` in it, so the same question arrives as "where does water go in
*this* signature", which is answerable.

```
git fetch origin
git rebase origin/Fixed-Function-dev claude/water-rendering-investigation-7baezw
python3 scripts/check_invariants.py
```

The rebase does not answer the space problem, and should not be expected to:
water needs four facts and, if transparency lands first, only `Power` remains.
**Suggested — not tested, not implemented, and the branch owner's call:** pack
all four into `Power` as one integer-valued float and leave `Ambient.a` to
transparency. The budget is comfortable:

| Fact | Range | Bits |
| :-- | :-- | --: |
| role (`NONE`/`SURFACE`/`PROJECTED`) | 0–2 | 2 |
| layer (`UNKNOWN`…`INDIRECT`) | 0–7 | 3 |
| MAxx tag | 0–63 is ample for MA02/MA06/MA09/MA10 | 6 |

11 bits total, against the 24 a `float32` represents integers in exactly. The
same packing argument applies to any later feature: **the side band is full, so
the next claim is a packing question, not an allocation one.**

Whoever rebases should also confirm the fork side. `dxvk-remix`'s water branch
does not merely reclaim the channels — `rtx_dusklight_texrep.{h,cpp}` are
**absent from its tree**, so the fork half of the same feature is reverted too.

---

## 2. GX FIFO subcommand `0x0053`

### 2.1 Four branches, one opcode

The highest subcommand on `Fixed-Function-dev` is `GX_AURORA_SET_VIEW_MTX 0x0052`
(`include/dolphin/gx/GXAurora.h:120`). Four live branches each took "the next
one":

| Branch | Symbol | `GXAurora.h` | Payload |
| :-- | :-- | --: | :-- |
| `claude/water-rendering-investigation-7baezw` | `GX_AURORA_SET_DUSKLIGHT_WATER` | :136 | 12 B — role, tag, layer |
| `claude/dusklight-remix-transparency-e7l766` | `GX_AURORA_SET_DRAW_CLASS` | :133 | 4 B — class |
| `claude/remix-texture-geometry-issues-occh2f` | `GX_AURORA_SET_MODEL_IDENTITY` | :129 | 24 B + slot table + 8 B |
| `claude/lss-hair-rtx-remix-j6uo4t` | `GX_AURORA_SET_POS_MTX_REST` | :130 | 52 B |

`remix-texture-geometry-issues-occh2f` additionally takes `0x0054`
(`GX_AURORA_CLEAR_MODEL_IDENTITY`), so it is the one branch whose claim is two
opcodes wide.

Three of the four have game-side callers already written —
`J3DMaterial.cpp:261` and `J3DPacket.cpp:262` (water), `gx_helper.h:88`
(transparency), `remix_skeleton.cpp:399` (model identity),
`J3DShapeMtx.cpp:64` (rest matrices) — so this is not a reservation collision,
it is four features that each emit `0x0053` into the same stream.

### 2.2 Why no conflict appears where it matters

Dispatch is a flat `else if` chain in `lib/gx/command_processor.cpp`, ending in
`else { Log.error("Unknown Aurora subcommand: {:04X}", subCmd); }`. Each branch
appends its arm at a slightly different point, so **the arms merge cleanly**.

Merging `remix-texture-geometry-issues-occh2f` into
`dusklight-remix-transparency-e7l766`, run 2026-08-11:

```
CONFLICT (content): Merge conflict in include/dolphin/gx/GXAurora.h
Auto-merging lib/gx/command_processor.cpp          <- CLEAN
```

Resulting tree:

```
GXAurora.h:134       #define GX_AURORA_SET_DRAW_CLASS      0x0053
GXAurora.h:158       #define GX_AURORA_SET_MODEL_IDENTITY  0x0053

command_processor.cpp:2040   } else if (subCmd == GX_AURORA_SET_MODEL_IDENTITY) {
command_processor.cpp:2090   } else if (subCmd == GX_AURORA_SET_DRAW_CLASS) {
```

The header conflict is real but its obvious resolution is "keep both" — the two
hunks are unrelated, additive, well-commented features. Two differently-named
macros with the same value compile without complaint. **The dispatch, where the
damage is, never conflicts:** both arms land, `SET_MODEL_IDENTITY` is tested
first, and `SET_DRAW_CLASS` becomes unreachable.

### 2.3 The failure signature is corruption, not a silent no-op

Worth stating because it is the opposite of what "the second arm is dead code"
suggests. Each arm advances `pos` by **its own** payload length. When the wrong
arm wins, the reader consumes the wrong number of bytes and the FIFO desyncs —
the remainder of the payload, or the commands after it, are parsed as
subcommands.

In the merge above, a game calling `GXSetDrawClass` emits 4 payload bytes; the
`SET_MODEL_IDENTITY` arm claims them and reads 24 or more, taking following GX
commands as its own operands. Expect a garbled frame, or a `CHECK` failure
naming an opcode the game never called — **not** a missing feature. Anyone
debugging that will start in the wrong place unless they know this.

### 2.4 Recommendation: allocate the opcodes, do not resolve them arm by arm

Resolving arm by arm gets each merge past the compiler and leaves the last
merger to discover the desync. The two collisions above have the same cause —
**a shared numbering with no registry, allocated by branches that cannot see
each other** — and want the same treatment.

**Recommended, and deliberately not implemented here** — assignment is the
owner's to make, and taking a number in this session would repeat the mistake
being reported:

1. **Write the registry down in `GXAurora.h` itself**, as a block above the
   `0x0050` group listing every allocated *and reserved* subcommand, including
   ones that live only on unmerged branches. `GXAurora.h` is the one file every
   claimant already edits, so a claim cannot be made without reading it — and a
   second claim on a reserved number then conflicts *in the registry*, where the
   conflict means something.
2. **Give each in-flight branch its own number now**, before any of them merge —
   for example `0x0053` transparency, `0x0054` water, `0x0055`/`0x0056` model
   identity (it needs two), `0x0057` rest matrices. Any assignment works; what
   matters is that it is made once, centrally, rather than four times in
   parallel. Each branch then rebases and takes its assigned number, which is
   the same motion §1.6 recommends for the channels.
3. **Consider a compile-time guard.** A `static_assert` per opcode, or a
   generated `switch` rather than an `else if` chain, turns a duplicate value
   into a build error. A `switch` gets this from the language for free —
   duplicate `case` labels do not compile — and is the cheaper of the two.

Until then: **check this document and `GXAurora.h` on every live `claude/*`
branch before taking a subcommand number.** Nothing automated can see an
unmerged branch.

---

## 3. What the invariants script does and does not catch

`scripts/check_invariants.py` `check_side_channel_map` was **one-directional and
blind to `Power`** until 2026-08-11. It walked the fields `set_remix_material`
writes and required a §2 row for each; it never asked the reverse, and it only
inspected `Ambient`/`Diffuse`/`Specular`/`Emissive`.

Both gaps are now closed (this session):

- **`Power` is checked**, so water's `mat.Power = waterLayer` is no longer
  invisible. Verified: it now fires on the water merge, where it did not before.
- **The check runs both ways.** A §2 row for a channel `set_remix_material` no
  longer writes is now a failure, naming a dropped feature as the likely cause.

**What that still does not catch, and it is the case in front of us.** The
reverse check finds a channel that stops being *written*. It cannot find a
channel that keeps being written with a **different meaning** — which is exactly
`Ambient.g`/`.b` under the water merge, where both sides write the channel and
only the meaning changes. Verified on the merged tree: the side-channel check
passes.

Nor can any of it see an unmerged branch, which is the whole subject of this
document.

**So the safeguard here is human review, and this document is its input.** Read
§1.2 and §2.1 before claiming a channel or an opcode, and re-derive them from
`git branch -r` rather than from this table if the date above is stale.
