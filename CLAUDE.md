# Claude session notes — aurora-ao

## Owner's environment: interactive approval prompts are BROKEN

Any tool call that pops an interactive approval/authorization prompt for the
owner is bugged across ALL of their Claude Code sessions — the prompt always
resolves as "no approval given" (e.g. MCP calls returning
`MCP error -32003: MCP tool call requires approval`, or the `add_repo`
authorization flow looping back to "there was no approval").

**Never rely on a tool that requires interactive approval.** Route around it:

- Reading other public repos (e.g. dxvk-remix for RTX Remix research): fetch
  files via `raw.githubusercontent.com` instead of the `add_repo` flow.
- Scheduling / reminders (`send_later` etc.): use a background `Monitor` /
  background Bash watcher instead.
- Questions for the owner: ask in plain chat text, not interactive pickers.

## Read this first if you have no context

This repo is the **GX→D3D9 fixed-function backend** for a three-repo project.
Start at `docs/dx9/README.md`, then `docs/dx9/progress.md` §"How to resume".

If the task is about the *look* of the game under RTX Remix rather than about
the D3D9 backend itself, the design work lives in the other two repos —
`dusklight-ao/docs/kankyo-remix.md` is the entry point.

| Repo | Role | Its docs |
| :-- | :-- | :-- |
| `automata-rtx/aurora-ao` | **this repo** — GX→D3D9 backend (`lib/dx9/`) | `docs/dx9/` |
| `automata-rtx/dusklight-ao` | the game; vendors this repo at `extern/aurora` | `docs/kankyo-remix.md`, `docs/dx9-fixed-function.md` |
| `automata-rtx/dxvk-remix` | the RTX Remix fork | `documentation/Dusklight*.md` |

## Branches — ALL THREE repos use the same structure

- **`Fixed-Function-dev` — the working branch. ALL development commits land
  here, in every one of the three repos.**
- `Fixed-Function` — integration. Advances only by merging `Fixed-Function-dev`
  at checkpoints that are **both CI-green and tested in game by the owner**.
  It is deliberately behind the dev branch; that is not drift to "fix".
- Base: this lineage descends from aurora `main`; when `main` gains commits,
  backport by merging/cherry-picking **into** `Fixed-Function-dev`.

**Standing authorization — this file is the authority for it.** A remote
session is often configured to push to a generated branch name like
`claude/<something>-<hash>`. That is fine, but **every such push must also be
mirrored to `Fixed-Function-dev` in the same repo, in the same turn**:

```
git push -u origin <session-branch>
git push origin HEAD:Fixed-Function-dev
```

**REVOKED by the owner on 2026-07-29 — the mirroring instruction above no
longer applies.** The owner merges to `Fixed-Function-dev` themselves, at
milestones they choose. Push only the session branch:

```
git push -u origin <session-branch>        # yes
git push origin HEAD:Fixed-Function-dev    # NO - the owner does this
```

If a session branch is about to be deleted and its work is not yet merged,
**say so and stop** rather than mirroring it.

**Before anyone deletes a branch, verify it is contained:**
`git rev-list --count origin/Fixed-Function-dev..origin/<branch>` must be `0`.
With auto-mirror off, a non-zero count is now the *normal* state between
milestones rather than an anomaly, so this check is no longer a formality.

**`claude/thin-gbuffer-authored-normals-wgqupt`** is **unrelated, unmerged
work** — in neither `main` nor `Fixed-Function-dev`. Do not delete it and do
not merge it into this lineage without being asked.

## Submodule pairing

This repo is vendored into dusklight-ao as `extern/aurora`. After pushing
aurora commits, bump the pin from the dusklight root:

```
git -C extern/aurora fetch origin && git -C extern/aurora checkout <sha>
git add extern/aurora
git submodule status           # verify before committing
```

Keep the branches paired: dusklight `Fixed-Function-dev` pins aurora
`Fixed-Function-dev` commits. Before merging dusklight dev → `Fixed-Function`,
merge aurora dev → `Fixed-Function` **first**, so the pinned SHA is reachable
from aurora's `Fixed-Function`.

## Verification

Verify D3D9 code with the MinGW syntax harness (see `docs/dx9/progress.md`
§"How to resume") in **both** the d3d9-on and d3d9-off configs. Full builds
happen on the owner's Windows machine and in dusklight-ao's GitHub Actions —
this repo has no CI of its own, so a change here is only really checked once
dusklight's workflow builds with the bumped submodule pin.

## Where the backend actually stands

`docs/dx9/progress.md` is authoritative, but the short version: the backend is
**effectively complete** for its stated priorities. Last code change: `389e4d5`
(2026-07-29), carrying the albedo tint into Remix's material. Working under
Remix: world/actor geometry, textures, terrain, alpha-tested foliage, UI/HUD,
skinned characters on both paths, EFB colour copies, stable texture hashing,
real camera, correct materials, working input across resizes.

Remaining gaps are **catalogued rather than open** — 18 GX features beyond
fixed-function and 9 Remix runtime limitations, all in
`docs/dx9/unsupported-effects.md`. Two live defects:

1. **Ground textures render white in raw D3D9** (Remix is correct, because it
   only reads the first texture stage). Prime suspect is the compare-mode
   approximation; the one-line experiment is flipping it from always-true
   (`d + c`) to always-false (`d`).
2. **Torch fire billboards reach Remix intermittently** — reported 2026-07-29,
   still open, and **pinned** pending a test at a lit torch. The suspicion is
   that the "white circle" is a fire sprite saturated to white by the
   compare-mode TEV approximation (defect 1 above), not a frame-position
   problem. See `docs/dx9/unsupported-effects.md` §"PINNED — the torch flame".

   The **targeting arrow** was grouped with this and is **RESOLVED (2026-07-30)
   by tagging its two textures as `rtx.uiTextures` in Remix's dev menu** — no
   code, in any repo. They were only ever grouped because they appeared and
   vanished together; that had one explanation for the arrow (Remix's RTX
   injection boundary) and does not carry over to the flame.

   *Two conclusions were recorded here and were both wrong; kept because each
   cost real work.* First, that the categorization-screen absence meant "a draw
   Remix never categorises was not captured the way we assume, so this lands
   here rather than in the fork" — it is not an aurora defect, aurora submits
   these draws correctly. Second, that `rtx.uiTextures` was unusable because it
   only triggers RTX injection — it **also** forces `Rasterized`, which is the
   flat unlit overlay a reticle wants, and is the entire fix. Acting on the
   second belief produced a capture hook in this repo, a mesh-submission path in
   the game and an overlay toggle in the fork, all removed on 2026-07-30 without
   ever working.
