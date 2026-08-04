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

**If the task involves materials, colour, or anything a surface looks like under
Remix, read `docs/dx9/remix-material-interface.md` before doing anything else.**
It is the most misunderstood system in the project — three sessions produced
three incompatible explanations of one defect and shipped a fix that did nothing.

If the task is about the *look* of the game under RTX Remix rather than about
the D3D9 backend itself, the design work lives in the other two repos —
`dusklight-ao/docs/kankyo-remix.md` is the entry point.

| Repo | Role | Its docs |
| :-- | :-- | :-- |
| `automata-rtx/aurora-ao` | **this repo** — GX→D3D9 backend (`lib/dx9/`) | `docs/dx9/` |
| `automata-rtx/dusklight-ao` | the game; vendors this repo at `extern/aurora` | `docs/kankyo-remix.md` (design), `docs/remix-open-issues.md` (what is broken), `docs/remix-test-playbook.md` (how to test), `docs/dx9-fixed-function.md` (setup) |
| `automata-rtx/dxvk-remix` | the RTX Remix fork | `documentation/Dusklight*.md` |

## Branches — ALL THREE repos use the same structure

- **`Fixed-Function-dev` — the working branch. ALL development commits land
  here, in every one of the three repos.**
- `Fixed-Function` — integration. Advances only by merging `Fixed-Function-dev`
  at checkpoints that are **both CI-green and tested in game by the owner**.
  It is deliberately behind the dev branch; that is not drift to "fix".
- Base: this lineage descends from aurora `main`; when `main` gains commits,
  backport by merging/cherry-picking **into** `Fixed-Function-dev`.

**Push only your session branch.** A remote session is usually configured to
push to a generated `claude/<something>-<hash>` branch. Push there and stop —
**the owner merges to `Fixed-Function-dev` themselves**, at milestones they
choose. (An auto-mirror rule existed until 2026-07-29 and was revoked.)

```
git push -u origin <session-branch>        # yes
git push origin HEAD:Fixed-Function-dev    # NO - the owner does this
```

If a session branch is about to be deleted and its work is not yet merged,
**say so and stop** rather than mirroring it.

**Before anyone deletes a branch, verify it is contained:**
`git rev-list --count origin/Fixed-Function-dev..origin/<branch>` must be `0`.
A non-zero count is the *normal* state between milestones, so this check is not
a formality — it is the only thing between a routine cleanup and lost work.

**`claude/thin-gbuffer-authored-normals-wgqupt`** is **unrelated, unmerged
work** — in neither `main` nor `Fixed-Function-dev`. Do not delete it and do
not merge it into this lineage without being asked.

## How this project works — read before proposing a fix

Five rules. They exist because each was learned the expensive way, and following
them is worth more than any individual fix.

### 1. Translate, don't tag

Every Remix project the world over works by hashing textures and hand-authoring
replacements, because the game is a closed box. **All three of our repos are
ours.** We can read the game's intent at the source and hand it to the renderer
directly.

So the default answer to "how do we make Remix understand X" is *translate the
game state*, not *tag the asset*. Tagging gives one answer per texture; this
game reuses textures across contexts constantly, so a tag is wrong somewhere
almost by construction. Translation is per-draw and is right everywhere.

### 2. A question we would have to ask the owner is a defect in the logging

The owner should not be the diagnostic instrument. Asking them to describe a
colour, count an artifact, or judge whether something looks "too dark" produces
answers that are honest and unusable — and it wastes a scarce test window.

**The target loop is: they play, they send a log, we know.** If a question
cannot be answered from a log, the correct response is to add the log line, not
to ask the question. Design instrumentation before designing the fix.

Corollary: **logs must be bounded and self-describing.** One line per distinct
thing, capped, with a truncation notice when the cap is hit, and enum names
spelled out so a reader without the source can follow. A log nobody can read is
the same as no log; a log that fills a disk is worse.

### 3. Do not write inference as finding

This project has three times recorded a plausible mechanism as a verified cause.
One of those shipped and turned out to be a no-op, and the documents kept saying
"FIXED" for a week.

State what you read, cite where, and mark inference as inference. A document
that says "unknown" is more valuable than one that says something confident and
wrong, because the second one stops the next person looking.

### 4. A fix that cannot be observed is a guess

Before shipping a change to a system with no instrumentation, add the
instrumentation. A change that alters behaviour *and* reports on itself is
fine — bundling saves a test window — but a change that alters behaviour and
stays silent cannot be evaluated except by looking at pixels, which is how this
project lost three rounds.

Say plainly what the regression signature of a change is, so it can be
recognised rather than discovered.

### 5. Say what was verified and what was not

"Compiles" and "is correct" are different claims. So are "CI green" and "tested
in game". Every doc entry and every hand-off should make clear which one it is.
There is no penalty here for saying a thing is untested; there is a real cost to
implying it was tested.

## Submodule pairing

This repo is vendored into dusklight-ao as `extern/aurora`. After pushing
aurora commits, bump the pin from the dusklight root:

```
git -C extern/aurora fetch origin && git -C extern/aurora checkout <sha>
git add extern/aurora
git submodule status           # verify before committing
```

Keep the branches paired: dusklight `Fixed-Function-dev` pins aurora
`Fixed-Function-dev` commits.

**Aurora is always merged first.** Whenever a merge carries a submodule bump,
merge aurora into the target branch before dusklight, so the pinned SHA is
reachable from that branch rather than only from a session branch that may later
be deleted. This applies to `Fixed-Function-dev` and `Fixed-Function` alike — a
dusklight branch pinning a SHA that lives only on a `claude/*` branch still
builds today and breaks the moment that branch is cleaned up.

## Verification

Verify D3D9 code with the MinGW syntax harness (see `docs/dx9/progress.md`
§"How to resume") in **both** the d3d9-on and d3d9-off configs. Full builds
happen on the owner's Windows machine and in dusklight-ao's GitHub Actions —
this repo has no CI of its own, so a change here is only really checked once
dusklight's workflow builds with the bumped submodule pin.

## Where the backend actually stands

`docs/dx9/progress.md` is authoritative; the short version is that the backend
is **effectively complete** for its stated priorities. Working under Remix:
world/actor geometry, textures, terrain, alpha-tested foliage, UI/HUD, skinned
characters on both paths, EFB colour copies, stable texture hashing, real
camera, working input across resizes.

Remaining gaps are **catalogued rather than open** — 18 GX features beyond
fixed-function and 11 Remix runtime limitations, all in
`docs/dx9/unsupported-effects.md`.

Live defects:

1. **Materials lose their colour under Remix** — rupees, hearts, Goron Mines
   lava render greyscale while raw D3D9 is correct. Root-caused 2026-08-03 to
   our own Remix hint stage overwriting a material Remix was reading correctly;
   a July fix shipped, was tested, and turned out to be a no-op. A corrected fix
   plus the `matrep` material report are in the tree and **untested**.
   Read `docs/dx9/remix-material-interface.md` before touching this.
2. **Ground textures render white in raw D3D9** (Remix is correct, because it
   only reads the first texture stage). Prime suspect is the compare-mode
   approximation; the one-line experiment is flipping it from always-true
   (`d + c`) to always-false (`d`). Worth making runtime-selectable rather than
   a rebuild, since it is also a suspect for the torch-flame white circle.
3. **World-space UI billboards reach Remix intermittently.** Not an aurora
   defect — it is Remix's RTX injection boundary. Full analysis in
   `dusklight-ao/docs/remix-open-issues.md` open issue 6.

**Self-illumination: first attempt tested 2026-08-04, caught one material, and
it was not lava** (open issue 9). The rule required GX lighting to be off; the
Goron Mines lava has it **on**. Together with the earlier "59% of a scene is
unlit" measurement that settles it: **no single GX fact identifies an emitter.**
Rev 2 scores three weak signals and the fork cuts at a live overlay threshold.
`docs/dx9/remix-material-interface.md` §9.

**Every material now carries `grp=`** — the name of the game code that drew it,
from a debug group the game pushes per process draw. Three investigations have
stalled on "which of these logged materials is the thing on screen"; that
question is retired. Use it.

**Remix cannot express a lerp between two constants**, which is this game's
dominant material shape, so every two-colour ramp is approximated and the lava
reads red-to-white instead of red-to-orange. The arithmetic and the fix are in
`docs/dx9/remix-material-interface.md` §10 — designed, not built. This is
currently the largest single win available on the material path.
