# Allocation: side channels and FIFO opcodes

**Audited 2026-08-11. The two collisions it was written for are resolved** —
water was rebased onto `Fixed-Function-dev` the same day and re-derived against
the current `set_remix_material`, and the subcommand space now has a registry.
§1.6 and §2.4 record what was done; the rest is kept because the *shape* of both
failures recurs and this is the only account of it.

**Current state, which is what most readers want:**

| Resource | Free | Claimed by unmerged work |
| :-- | :-- | :-- |
| `D3DMATERIAL9` side band | `Ambient.a` only | `Ambient.a` — `claude/dusklight-remix-transparency-e7l766` |
| GX FIFO subcommands | `0x0058` and up | `0x0054`–`0x0057` reserved, see the registry in `GXAurora.h` |

This document exists because two shared, finite resources were being allocated by
branches that cannot see each other:

- the `D3DMATERIAL9` side band, whose field map is
  [`remix-material-interface.md`](remix-material-interface.md) §2
- the aurora GX FIFO subcommand space, `include/dolphin/gx/GXAurora.h`

Both were over-subscribed, and in both cases the collision survived a `git
merge` — one because the obvious conflict resolution is the wrong one, the other
because the place that matters does not conflict at all. That is the failure
CLAUDE.md's "Merges that succeed and are still wrong" describes, twice, live.

**Everything below was read from the branches, not recalled.** Line numbers are
from the commits fetched on 2026-08-11.

---

## 1. `D3DMATERIAL9` side channels

### 1.1 What `Fixed-Function-dev` used before the rebase

**This is the *pre-rebase* state, kept because §1.4 measures against it.** The
current field map is [`remix-material-interface.md`](remix-material-interface.md)
§2, which is the one to read when allocating.

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

**At the time of the audit: zero**, once water and transparency landed as
written. `Ambient.a` and `Power` were the only two §2 had ever called spare;
`Ambient.a` was claimed twice over and `Power` once.

**After the rebase: one — `Ambient.a`.** Water was re-derived to pack all three
of its facts into `Power` alone, which is what bought that back. It is still
spoken for by `claude/dusklight-remix-transparency-e7l766`, so a proposal that
assumes a free channel should confirm against that branch first; and the feature
*after* that one has to pack, because there will be nothing left to pack into.

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

### 1.6 What was done — rebase, not hand resolution

**Carried out 2026-08-11.** The recommendation and the reasoning are kept below
because the next branch in this position needs both.

Water was re-applied on top of the current `set_remix_material` rather than
merged into it, and its channel assignment re-derived: all three facts —
role, MAxx tag, layer — now share `D3DMATERIAL9::Power`, packed as
`tag * 100 + layer * 10 + role`. `texRepIndex` and `texRepStage` keep
`Ambient.g`/`.b` untouched, and `Ambient.a` is left free.

`GX_AURORA_DUSKLIGHT_WATER_PACK` in `GXAurora.h` is the definition and
`rtx_dusklight_water.h` decodes it; `check_invariants.py` now checks the two
against the formula §2 states, because they are one contract in two repositories.

**One thing the rebase does not carry over: the 2026-08-08 in-game measurement.**
It was taken with the facts in `Ambient.g`/`.b`/`.a`. What it establishes — that
the game marks the right draws and the marks survive the FIFO — still holds; that
the *current* transport delivers them is untested.
`remix-material-interface.md` §11 states this split, and names the regression
signature: every draw reports `role=none` and water renders as it did before the
feature existed.

#### The recommendation, for the next branch in this position

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

**Resolved 2026-08-11**: water holds `0x0053`, the other three claimants have
reserved numbers in the registry comment at the top of `GXAurora.h`, and
`check_invariants.py` fails on a duplicate or an unregistered subcommand. §2.4
is what was done; §2.1–2.3 are why.

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

### 2.4 What was done: a registry, and one number each

Resolving arm by arm gets each merge past the compiler and leaves the last
merger to discover the desync. The two collisions above have the same cause —
**a shared numbering with no registry, allocated by branches that cannot see
each other** — and want the same treatment.

Implemented, 2026-08-11:

1. **The registry is written down in `GXAurora.h` itself**, as a block above the
   `0x0050` group listing every allocated *and reserved* subcommand, including
   ones that live only on unmerged branches. `GXAurora.h` is the one file every
   claimant already edits, so a claim cannot be made without reading it — and a
   second claim on a reserved number then conflicts *in the registry*, where the
   conflict means something.
2. **Each in-flight branch has a number reserved**: `0x0054`/`0x0055` model
   identity (it needs two), `0x0056` rest matrices, `0x0057` draw class. Water
   keeps `0x0053` because it merged first. Any assignment would have worked; what
   matters is that it was made once, centrally, rather than four times in
   parallel. Each branch takes its assigned number when it rebases — the same
   motion §1.6 describes for the channels.
3. **`check_invariants.py` enforces both halves**: two defines sharing a value
   fails, and a define missing from the registry fails. That second one is what
   makes a duplicate claim conflict *in the registry*, where the conflict means
   something, instead of in an `else if` chain where it does not.

**Still not done, and worth doing:** the dispatch is still an `else if` chain. A
`switch` would make a duplicate a compile error for free — duplicate `case`
labels do not compile — which is stronger than a script that has to be run.

**And still true regardless:** nothing automated can see an unmerged branch, so
check the live `claude/*` branches before taking a number.

---

## 3. What the invariants script does and does not catch

`scripts/check_invariants.py` `check_side_channel_map` was **one-directional and
blind to `Power`** until 2026-08-11. It walked the fields `set_remix_material`
writes and required a §2 row for each; it never asked the reverse, and it only
inspected `Ambient`/`Diffuse`/`Specular`/`Emissive`.

The script now runs six checks. Three are new or rebuilt as a result of this
audit, and each was verified by breaking the tree deliberately and confirming the
message names the right thing:

| Check | Catches | Verified by |
| :-- | :-- | :-- |
| side channels, **write → row** | a feature takes a channel and leaves §2 advertising it as spare | the original 2026-08-05 case |
| side channels, **row → write** | a merge drops the code that claimed a channel while §2 keeps describing it | blanking the texrep writes with §2 intact |
| `Power` included | the field the old check could not see at all | the water merge, which it now fails |
| duplicate subcommand | two `GX_AURORA_*` defines sharing a value | pointing `SET_VIEW_MTX` at `0x0053` |
| unregistered subcommand | a number taken without adding it to the registry | adding `0x0058` and `0x0054` unlisted |
| water packing | `GXAurora.h` and the documented formula disagreeing, or a role/layer/tag value outgrowing its decimal slot | changing the multiplier; raising `LAYER_MAX` to 12 |

**What none of it catches, and it is the case this audit was about:** a channel
that keeps being written with a **different meaning**. Both directions of the
side-channel check pass, `Power` is present, the field map reads as true, and the
feature underneath has been replaced. Nothing mechanical distinguishes
"`Ambient.g` carries a texture index" from "`Ambient.g` carries a water flag".

The defence is structural rather than mechanical: **a channel must be claimed in
§2 in the same commit that writes it**, so the two halves of the change are
visible in one diff, and the row→write direction fails if a later merge removes
one half without the other.

Nor can any of it see an unmerged branch, which is what the current-state table
at the top of this document is for — and that table is maintained by hand.
