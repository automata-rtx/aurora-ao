# Aurora D3D9 fixed-function backend

**Start here.** This directory is the canonical knowledge base for the DirectX 9
fixed-function rendering mode in Aurora, written so development can resume from
these documents alone.

## Mission

Render Dusklight (Twilight Princess) through the **D3D9 API using fixed-function
pipelines**, so the game can be fed into **RTX Remix's D3D9→Vulkan runtime** —
a whole scene picked up automatically, with no per-asset authoring.

**The raw fixed-function image is never shown to a player.** Remix's renderer is
the product; D3D9 is the feed. So fixed-function limits are not the ceiling:
where the D3D9 stream cannot carry something faithfully enough to reach Remix,
the work moves **into Remix** — its API, or the fork, both of which are ours —
rather than being approximated in D3D9. "Raw D3D9 stays correct" is a
sometimes-useful safety property, never a design goal. **Two exceptions still
have to rasterize correctly:** the **HUD** (Remix rasterizes UI draws rather
than path-tracing them) and **alpha** (Remix reads the stage's alpha to build
opacity and the alpha test). Canonical statement, with the consequences spelled
out: [`remix-material-interface.md`](remix-material-interface.md) §0. Documents
here written before 2026-08-04 sometimes still treat raw-D3D9 correctness as a
requirement; they are wrong and are corrected as they are touched.

### Strict rules (from the project owner — do not violate)

1. **Fixed-function first.** Vertex processing is 100% fixed-function T&L,
   including skinning via indexed vertex blending. Pixel work uses **texture
   stage states**; if a combine genuinely cannot be expressed with TSS, a pixel
   shader no newer than **shader model 1.x** is the ceiling — and this backend
   uses **zero** pixel shaders. This bounds what *this backend emits*; it is not
   a bound on the project, which carries what D3D9 cannot in the fork instead.
2. **Priorities, in order:** (a) world/actor geometry including alpha-tested
   billboards, (b) diffuse textures including terrain multi-texture blends,
   (c) UI elements. Everything else may be simplified or dropped.
3. Post-processing (bloom, heat shimmer, depth of field) is **intentionally
   ignored** — Remix discards it anyway.
4. Effects fixed function cannot carry are **documented** in
   [`unsupported-effects.md`](unsupported-effects.md) rather than contorted into
   the stream. That list is where the work **moves to Remix**, not where it
   stops.
5. Visual parity with the WebGPU path is **not** a goal. Being picked up by
   Remix's interception is — "plug-and-play" in the sense that the *stream*
   needs no per-asset authoring, not in the sense that we run stock Remix. The
   shipped runtime is our fork, and the game and the fork are a single versioned
   protocol.

## How to resume with zero context

1. This file, then [`gx-to-d3d9-mapping.md`](gx-to-d3d9-mapping.md) (the spec
   being implemented). **If the task touches materials or colour, read
   [`remix-material-interface.md`](remix-material-interface.md) first** — it is
   the system most often reasoned about incorrectly here.
2. **If the task is about how the game *looks* under Remix rather than about the
   D3D9 backend, you are in the wrong repo.** Go to
   `dusklight-ao/docs/kankyo-remix.md`.
3. [`design-decisions.md`](design-decisions.md) — why the backend is shaped the
   way it is. Short, and it prevents most re-litigation.
4. `CLAUDE.md` at the repo root is the authority on branches, verification and
   the owner's environment.

Build target is Windows; this work is developed in a Linux container, so see the
verification vocabulary below before claiming anything compiles.

**Reading game code to explain a draw?** Twilight Princess's identifiers are the
original Japanese team's names, kept by the decompilation — `kankyo` (環境) is
*environment*, `wether` is the game's spelling of *weather*, and the tree mixes
two romanization systems, so a search for one spelling finds half a feature.
`dusklight-ao/docs/japanese-naming-remix.md` is the reference. Aurora's own code is
unaffected: `lib/dx9/` is ordinary English `camelCase`.

## Document map

| File | Contents |
|------|----------|
| [`gx-to-d3d9-mapping.md`](gx-to-d3d9-mapping.md) | The translation spec: conventions, transforms, vertex decoding, TEV→texture-stage mapping, skinning, EFB copies, UI, and the lossy mappings. Read after this file. |
| [`remix-material-interface.md`](remix-material-interface.md) | **How a GX material becomes a Remix material, what survives and what silently does not.** The most misunderstood system here; read it before touching `dx9_tev.cpp` or diagnosing any colour defect. **§0 is the canonical statement of what the D3D9 renderer is for.** §11 is water. |
| [`material-report.md`](material-report.md) | The `matrep.*` log: format, how to read it, how to join aurora's log to Remix's. This is how material questions get answered without asking the owner to describe pixels. |
| [`unsupported-effects.md`](unsupported-effects.md) | What the stream cannot carry and where that work would go, plus the Remix runtime behaviours that constrain it. |
| [`texture-replacements.md`](texture-replacements.md) | **HD texture packs** — how a Dolphin-format pack reaches Remix without its bytes entering D3D9, why that is what keeps texture tagging stable, and the two substitution sites the HUD forces. Tested good 2026-08-06. |
| [`design-decisions.md`](design-decisions.md) | Why the backend is shaped the way it is: the decisions that are expensive to rediscover, each pointing at the code comment that carries its derivation. |

## Design in one paragraph

Aurora already funnels *every* draw — immediate-mode GX **and** J3D's baked
big-endian display lists — through one chokepoint: `aurora::gx::fifo::process()`
→ `draw_prim()` / `push_gx_draw()` in `lib/gx/command_processor.cpp`, with the
complete decoded GX state in `g_gxState`. **The D3D9 backend reuses all of that
state decoding unchanged and intercepts at that chokepoint.** It decodes the raw
GX vertex stream on the CPU into fixed-function-friendly interleaved vertices,
sets transforms, render states and texture-stage states derived from
`g_gxState`, and issues `DrawIndexedPrimitiveUP` immediately — single-threaded,
inside the game thread's FIFO drain, between `BeginScene`/`EndScene`. Dawn is
never initialized in this mode; textures reuse Aurora's CPU decoders into
`D3DFMT_A8R8G8B8`. Skinning maps to fixed-function **indexed vertex blending**,
and projection and model-view come through `SetTransform`, which is exactly what
Remix introspects.

**Characters are already GPU-skinned game-side**, which is why Remix receives
rest-pose vertices plus a transform and gets stable mesh hashing — a change that
"helps Remix" by baking skinning on the CPU would destroy per-asset replacement
for characters. [`design-decisions.md`](design-decisions.md) §Skinning.

## Verification vocabulary

Three different claims, and this project has been burned by conflating them.
Every entry in these documents states which one it is.

| Term | Means |
| :-- | :-- |
| **syntax-checked** | `scripts/check_syntax.sh` passes: real MinGW `<d3d9.h>`/`<windows.h>`, real repo headers, real absl and fmt, `-fsyntax-only`, in **three** configs — d3d9-on, d3d9-off, and d3d9-on with `-DNDEBUG`. The second is not optional, since every entry point has a no-op stub behind `#else` in `dx9.hpp` and a signature change that misses it fails only there; the third covers the release side of the `AURORA_GFX_DEBUG_GROUPS` blocks, which `include/aurora/gfx.h` defines only when `NDEBUG` is unset. Dawn, SDL3, Tracy and xxHash are shimmed, so `lib/gx/gx.cpp` and `lib/gfx/common.cpp` cannot be checked this way. **It does not validate format strings** — the distro fmt is 9.x against aurora's 11.x. Setup: `apt-get install g++-mingw-w64-x86-64 libfmt-dev libabsl-dev`. |
| **CI-green** | Built by dusklight-ao's GitHub Actions with the submodule pin bumped. This repo has no CI of its own. |
| **tested in game** | The owner ran it on Windows and reported back. |

"Syntax-checked" says nothing about correctness, and several changes have been
syntax-checked, shipped, and wrong. Run `scripts/check_invariants.py` too — it
is fast, and it catches the cross-document drift a compiler cannot see.

## Build & run

- CMake option `AURORA_ENABLE_D3D9` (default `ON` for WIN32, forced `OFF`
  elsewhere).
- Runtime selection: Dusklight `--backend d3d9`, config
  `backend.graphicsBackend = "d3d9"`, or the Prelaunch → Graphics Backend menu.
- For RTX Remix: drop the **fork's** `d3d9.dll` (`automata-rtx/dxvk-remix`, not
  a stock Remix release — game and fork are one versioned protocol) next to the
  Dusklight executable and select the d3d9 backend. Setup and `rtx.conf`:
  `dusklight-ao/docs/dx9-fixed-function.md`.
- **`dx9: device created …` in the log is the signature that the backend is
  actually active.** More than one "it still crashes" report turned out to be a
  run that never selected D3D9.

## Status

Three repos, all developing on `Fixed-Function-dev`; **the authority on branch
rules is `CLAUDE.md` at this repo's root.** This repo carries the D3D9 backend
(`lib/dx9/`) and this doc set, `automata-rtx/dusklight-ao` vendors it at
`extern/aurora`, and `automata-rtx/dxvk-remix` is the Remix fork.

**What is broken right now, across all three repos, lives in exactly one
place:** `dusklight-ao/docs/remix-open-issues.md`.
