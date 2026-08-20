# Claude session notes — aurora-ao

**Interactive approval prompts are broken for the owner** — they always resolve
as "no approval given". Read other public repos via `raw.githubusercontent.com`
rather than `add_repo`, use a background watcher rather than `send_later`, and
ask questions in plain chat.

## Read this first if you have no context

This repo is the **GX→D3D9 fixed-function backend** (`lib/dx9/`) for a three-repo
project. Start at `docs/dx9/README.md`. `automata-rtx/dusklight-ao` is the game
and vendors this repo at `extern/aurora` (`docs/kankyo-remix.md` for the design,
`docs/remix-open-issues.md` for what is broken, `docs/japanese-naming-remix.md`
for reading its symbol names); `automata-rtx/dxvk-remix` is the RTX Remix fork
(`documentation/Dusklight*.md`).

**If the task involves materials, colour, or what a surface looks like under
Remix, read `docs/dx9/remix-material-interface.md` before doing anything else.**
It is the most misunderstood system in the project — three sessions produced
three incompatible explanations of one defect and shipped a fix that did nothing.

| This repo's documents | Holds |
| :-- | :-- |
| `docs/dx9/README.md` | the router: mission, document map, how to resume, build and run, verification vocabulary |
| `docs/dx9/design-decisions.md` | why the backend is shaped the way it is, each decision with the symptom that forced it |
| `docs/dx9/remix-material-interface.md` | how a GX material becomes a Remix material — §0 is canonical, §2's field map is CI-parsed |
| `docs/dx9/material-report.md` | the material report log: every field, and how to read it |
| `docs/dx9/gx-to-d3d9-mapping.md` | the normative translation: conventions, gotchas, where it is deliberately lossy |
| `docs/dx9/unsupported-effects.md` | what the backend cannot express, as a work list rather than a list of losses |
| `docs/dx9/texture-replacements.md` | HD packs: the never-upload trap and the two substitution sites |

Status claims live in `dusklight-ao/docs/remix-open-issues.md`, not here.

## What the D3D9 renderer is for

**The raw fixed-function D3D9 image is never shown to a player.** It exists so
Remix's DX9→Vulkan translation picks the scene up for free. **Remix's renderer is
the product; D3D9 is the feed** — so where D3D9 cannot carry something
faithfully, implement it in Remix rather than contorting D3D9. All three repos
are ours, and "raw D3D9 stays correct" is not a design goal. "Remix cannot
express X" is a statement about *stock* Remix; check whether the constraint is
real before designing around it. The two-colour ramp sat on the unsupported list
for that reason alone.

**Two exceptions must still rasterize:** the **HUD** (Remix rasterizes UI draws)
and **alpha** (Remix reads the stage's alpha for opacity and the alpha test).
Full statement: `docs/dx9/remix-material-interface.md` §0.

## Five rules, each learned expensively

1. **Translate, don't tag.** A texture tag is wrong somewhere by construction; a
   per-draw translation of the game's own state is right everywhere.
2. **A question we would have to ask the owner is a defect in the logging.** They
   play, they send a log, we know. Bounded, capped, self-describing.
3. **Do not write inference as finding.** "Unknown" beats confident and wrong.
4. **A fix that cannot be observed is a guess.** Instrument first; state the
   regression signature.
5. **Say what was verified.** "Compiles", "CI green" and "tested in game" are
   three different claims.

## The game's symbols are named in Japanese

This repo's own code is English `camelCase` and GX names come from Nintendo's
SDK, but **the game symbols these documents cite are romanized Japanese** —
`kankyo` (環境) is *environment*, and `wether` is the weather system, not a typo
to fix. Search in both romanizations (the tree mixes kunrei-shiki and Hepburn for
one word, so an empty grep proves nothing) and `export LC_ALL=C.UTF-8` first, or
`grep -P` on kana silently matches nothing. Reading a material name usually
settles a classification before any measurement does. Full reference:
`dusklight-ao/docs/japanese-naming-remix.md`.

## Branches — all three repos use the same structure

- **`Fixed-Function-dev` is the working branch.** All development commits land
  there, in every repo.
- `Fixed-Function` is integration, advancing only at checkpoints that are both
  CI-green and tested in game. Being behind is intended, not drift to fix.

**Push only your session branch** — the owner merges. If a session branch is
about to be deleted and its work is not merged, **say so and stop** rather than
mirroring it. Before any deletion,
`git rev-list --count origin/Fixed-Function-dev..origin/<branch>` must be `0`; a
non-zero count is the normal state between milestones.
**`claude/thin-gbuffer-authored-normals-wgqupt`** is unrelated, unmerged work —
do not delete it, do not merge it into this lineage without being asked.

**Submodule pairing.** After pushing aurora commits, bump the pin from the
dusklight root (`git -C extern/aurora checkout <sha>` → `git add extern/aurora` →
`git submodule status`). **Aurora is always merged first**, so the pinned SHA is
reachable from the target branch rather than only from a session branch that may
later be deleted.

## Verifying a change without a Windows machine

```
scripts/check_syntax.sh      # needs g++-mingw-w64-x86-64, libfmt-dev, libabsl-dev
scripts/check_invariants.py  # after any merge, before any push
```

`check_syntax.sh` type-checks against **real** MinGW `<d3d9.h>`/`<windows.h>` and
real repo headers, in **three** configs, each of which exists for a reason:

- **d3d9-on** — the shipping path.
- **d3d9-off** — every entry point has a no-op stub behind `#else` in `dx9.hpp`,
  and a signature change that misses the stub fails only here.
- **d3d9-on + NDEBUG** — `AURORA_GFX_DEBUG_GROUPS` is defined by
  `include/aurora/gfx.h` **only when `NDEBUG` is unset**, so code behind it was
  never checked in the configuration CI actually ships.

**It is not a build.** It does not link, does not run, and does not validate
format strings — the container's fmt is 9.x against aurora's 11.x.
"Syntax-checked" is the honest claim; "compiles" is dusklight's CI and "works" is
the owner. `lib/gx/gx.cpp` and `lib/gfx/common.cpp` reach Dawn proper and are not
covered.

**One consequence of that fmt gap is checked separately.** From fmt 10 the
`format_string` constructor is `consteval`, so a `Log.*` format argument must be
a constant expression — and `Log.info(cond ? "a" : "b")` is not one. MSVC rejects
it as `error C7595`, naming the fmt header rather than the ternary. **The fix is
always two calls, each with its own literal.** `check_invariants.py` requires
every `Log.*` format string to be a literal; macro bodies are exempt, since
`ASSERT`/`FATAL` receive their literal at the expansion site.

`check_invariants.py` checks the facts this repo states in more than one place —
the `matrep.sum` fields against `material-report.md`, the channels
`set_remix_material` writes against §2's field map (**both** directions), the
`dx9.draws` period, the GX subcommand registry, conflict markers. This repo has
no CI, so dusklight's `Invariants` workflow runs it against the pinned submodule;
it is fast, so run it here rather than finding out after a submodule bump.

**A clean `git merge` is not a correct merge.** No script can see whether a
"tested in game" claim survived the change under it, whether prose still
describes reality, whether an unmerged branch is about to take the same number,
or — the hole neither direction of the side-channel check closes — a channel
written with a **different meaning**. Two allocation registries, each argued at
its own site rather than here: a new per-draw fact is a new **bit**, not a new
`D3DMATERIAL9` channel
(`dxvk-remix/src/dxvk/rtx_render/rtx_dusklight_drawmeta.h:27-50`; `0x0058` is
that export's subcommand), and GX FIFO subcommand numbers come from the registry
comment at `include/dolphin/gx/GXAurora.h:210-232`.

## Where the backend stands

The backend is **effectively complete** for its stated priorities; the reasoning
behind its shape is `docs/dx9/design-decisions.md`, and what it still cannot
express is `docs/dx9/unsupported-effects.md`.

**One `GXBegin` block is one D3D9 draw call, and Remix charges per draw rather
than per pixel** — a draw too small for its own BLAS still contributes its own
geometry entry and surface to a bucket that rebuilds every frame. A game-side
loop wrapping each quad in its own `GXBegin`/`GXEnd` is affordable in raster and
ruinous here; the kankyo weather effects spent ~1000 draws a frame that way until
2026-08-08. `dx9.draws` in the log reports mean and peak draws per frame, so this
is measurable rather than guessed at.

Two live defects worth knowing before you start reading: colour reaches Remix
(tested 2026-08-04) but the exact two-colour ramp and the vertex-colour
forwarding rule beside it are **untested** — read
`docs/dx9/remix-material-interface.md` before touching either; and world-space UI
billboards reach Remix intermittently, which is Remix's injection boundary rather
than an aurora defect. The ledger is
`dusklight-ao/docs/remix-open-issues.md`.
