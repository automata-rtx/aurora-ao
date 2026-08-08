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

## What the D3D9 renderer is for — read this before proposing a fix

**The raw fixed-function D3D9 image is never shown to a player.** It exists so
Remix's DX9→Vulkan translation picks the scene up automatically — geometry,
transforms, textures, most of a frame, for free. **Remix's renderer is the
product; D3D9 is the feed.**

So:

- **Fixed-function limits are not the ceiling.** Where the D3D9 stream cannot
  carry something faithfully enough to reach Remix, implement it **in Remix** —
  Remix API or a fork change — rather than contorting D3D9 to approximate it.
  All three repos are ours.
- **"Raw D3D9 stays correct" is not a design goal.** It is occasionally a handy
  safety property, never a reason to reject an approach. Documents written
  before 2026-08-04 sometimes treat it as a requirement; they are wrong and are
  being corrected as they are touched.

**Two exceptions still have to rasterize correctly:** the **HUD** (Remix
rasterizes UI draws rather than path-tracing them) and **alpha** (Remix reads
the stage's alpha to build opacity and the alpha test).

Full statement: `aurora-ao/docs/dx9/remix-material-interface.md` §0.

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

## Merges that succeed and are still wrong

**A clean `git merge` is not a correct merge, and on this project that is not a
theoretical worry.** Several features are developed on parallel branches that
edit the same documents, and git only compares *lines*. It cannot see that two
branches have made the same sentence false.

Two instances, both real:

- **The protocol double-bump.** Two branches independently took protocol 6 → 7.
  Git *did* conflict, on the same lines — and that made it worse, not better:
  both sides said `7`, so the obvious resolution is to keep 7 and move on,
  shipping two features that claim one version. The conflict was flagged; the
  resolution was the trap.
- **The side-channel map.** A feature claimed `D3DMATERIAL9::Ambient.g` and
  `.b` and updated three of the four places that describe the struct. The
  canonical table in `docs/dx9/remix-material-interface.md` §2 merged cleanly
  and went on advertising both channels as spare — so the next feature to want
  one would have taken a channel already in use, surfacing as a material bug
  nowhere near either change.

**So, after any merge — and before pushing one:**

```
python3 scripts/check_invariants.py
```

It checks the facts this repo states in more than one place: the `matrep.sum`
format string against `material-report.md`, the channels `set_remix_material`
writes against the §2 field map, the `dx9.draws` reporting period against the
four documents that quote it (two inside worked example lines a reader will take
as literal output), and leftover conflict markers. This repo has no
CI of its own, so **dusklight-ao's `Invariants` workflow runs it against the
pinned submodule** — but it is fast, so run it here too rather than finding out
after a submodule bump.

**What it cannot check, and therefore what a human still has to:**

- whether a "tested in game" claim is still true after the code under it changed
- whether a document's *prose* still describes reality, as opposed to its
  tables agreeing with the code
- whether two in-flight branches are about to take the same spare side channel —
  nothing can see a branch that has not merged yet

**When auditing documentation after a merge, re-derive the file list from the
diff.** Auditing from memory is how the §2 table was missed twice: every gap
found on the third pass was in a document that had not been edited, which is
exactly the set memory does not surface.

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
camera, working input across resizes, and **HD texture replacement packs**
(2026-08-06 — the pack goes to Remix through the API rather than through D3D9,
so texture tagging is untouched; `docs/dx9/texture-replacements.md`).

**One `GXBegin` block is one D3D9 draw call**, and Remix charges per draw rather
than per pixel — a draw too small for its own BLAS still contributes its own
geometry entry and surface to a bucket that rebuilds every frame. A game-side
loop that wraps each quad in its own `GXBegin`/`GXEnd` is therefore affordable
in raster and ruinous here; the kankyo weather effects spent ~1000 draws a frame
that way until 2026-08-08. `dx9.draws` in the log reports draws per frame (mean
and peak over 600 frames) so this is measurable rather than guessed at.
`docs/dx9/progress.md` §3.32.

Remaining gaps are **catalogued rather than open** — 17 open GX features of 18
catalogued (#17, texture packs, closed 2026-08-05) and 12 Remix runtime
limitations, all in
`docs/dx9/unsupported-effects.md`. That list says *where the work would go*,
not what has been given up: the fork is ours, so a "Remix limitation" is a work
item until someone reads the fork and finds a real wall. The two-colour ramp
sat on that list until 2026-08-04.

Live defects:

1. **Materials.** Colour reaches Remix as of 2026-08-04 (tested). Two-colour
   ramps are now reproduced exactly rather than approximated, and vertex colour
   is forwarded only where GX says it is material rather than baked lighting —
   both **untested**. Read `docs/dx9/remix-material-interface.md` before
   touching any of it.
2. ~~Ground textures render white in raw D3D9~~ — **not a defect.** Remix is
   correct, and the raw image is never shown. Kept only because the same
   compare-mode approximation is a suspect for the torch-flame white circle,
   which *does* reach Remix.
3. **World-space UI billboards reach Remix intermittently.** Not an aurora
   defect — it is Remix's RTX injection boundary. Full analysis in
   `dusklight-ao/docs/remix-open-issues.md` open issue 6.

**Self-illumination is a rule, not a score.** Three revisions cut on a weighted
evidence score and all three missed the Goron Mines lava, which scores **0.00**
on every signal that score is built from. Rev 4 drops it from the decision:

> **self-lit** (no TEV colour stage reads the rasterized channel — *not* the
> channel's lighting flag, which is a different thing and was the bug)
> **AND has a colour of its own** (authored in TEV constants, not mixed from the
> vertex stream and not a bare `black → white` texture pass-through)
> **AND that colour reads as a glow** (saturated **or** near-white-hot)

The middle clause is what keeps EFB copies out — 9 of the 20 self-lit materials
in the measured scene were screen blits. Replayed over that log the rule accepts
**6 of 77 materials**, every lava and fire surface, no false positives, nothing
to tune. `docs/dx9/remix-material-interface.md` §9.

**`grp=` does not work and has never worked** — every material in every session
logs `grp=-`. `fpcDw_Execute` schedules a draw; it does not issue one. The hook
is removed. Identify a material by its logged shape instead: texture size and
format, `tfactor`, and the ramp endpoints. §9 "Identification".

**Two-colour ramps are now reproduced exactly** rather than approximated — the
fork evaluates the GX colour combiner (`a*(1-c) + b*c`) from both endpoints
instead of squeezing it into one D3D9 texture op. `docs/dx9/remix-material-interface.md`
§10. Untested. The framing that delayed this is worth remembering: "Remix cannot
express X" is a statement about *stock* Remix, and **this fork is ours** — check
whether the constraint is real before designing around it.
