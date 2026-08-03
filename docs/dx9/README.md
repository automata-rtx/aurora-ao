# Aurora D3D9 fixed-function backend

**Start here.** This directory is the canonical knowledge base for the DirectX 9
fixed-function rendering mode being added to Aurora (and surfaced in Dusklight).
It is written so that development can resume from these documents alone, without
any prior conversation context.

## Mission

Add a new rendering backend to Aurora that renders Dusklight (Twilight Princess)
through the **DirectX 9 API using fixed-function pipelines**, so the game can be
fed into **RTX Remix's standard D3D9→Vulkan runtime** (the d3d9.dll replacement
path) with no Remix SDK integration.

### Strict rules (from the project owner — do not violate)

1. **Fixed-function first.** Vertex processing is 100% fixed-function T&L
   (including skinning via indexed vertex blending). Pixel work uses
   **texture stage states**; if a combine genuinely cannot be expressed with
   TSS, a pixel shader no newer than **shader model 1.x** (ps_1_1..ps_1_4) is
   the ceiling — and v1 of this backend uses **zero pixel shaders**.
2. **Priorities, in order:** (a) world/actor geometry incl. alpha-tested
   billboards, (b) diffuse textures incl. terrain multi-texture blends,
   (c) UI elements (HUD/menus). Everything else may be simplified or dropped.
3. Post-processing (bloom, heat shimmer, depth-of-field…) is **intentionally
   ignored** — Remix discards it anyway.
4. Effects that cannot be reasonably approximated are **documented** in
   [`unsupported-effects.md`](unsupported-effects.md) instead of hacked in.
5. Visual parity with the WebGPU path is **not** a goal. Plug-and-play Remix
   compatibility is the goal.

## Repos & branches

Three repos, all developing on `Fixed-Function-dev`. **The authority on branch
rules is `CLAUDE.md` at this repo's root** — it is kept current and this section
deliberately does not restate it.

This repo carries the D3D9 backend (`lib/dx9/`) and this doc set;
`automata-rtx/dusklight-ao` vendors it at `extern/aurora`;
`automata-rtx/dxvk-remix` is the Remix fork.

The lineage includes the GPU-skinning work, because Remix needs rest-pose
vertices plus GPU-side skinning for stable mesh hashing — see
`dusklight-ao/docs/gpu_skinning_and_platform_direction.md` §4.

## Document map

| File | Contents |
|------|----------|
| [`architecture-notes.md`](architecture-notes.md) | How Aurora renders today (FIFO → command processor → `g_gxState` → draws), how Dusklight feeds it, all integration points. Read second. |
| [`gx-to-d3d9-mapping.md`](gx-to-d3d9-mapping.md) | The full translation spec: vertex decoding, transforms, TEV→texture-stage mapping, skinning, UI, samplers, EFB copies. Read third. |
| [`remix-material-interface.md`](remix-material-interface.md) | **How a GX material becomes a Remix material, what survives and what silently does not.** The most misunderstood system here; read it before touching `dx9_tev.cpp` or diagnosing any colour defect. |
| [`material-report.md`](material-report.md) | The `matrep.*` log: format, how to read it, how to join aurora's log to Remix's. This is how material questions get answered without asking the owner to describe pixels. |
| [`unsupported-effects.md`](unsupported-effects.md) | Living list of effects beyond fixed-function/SM1 and how Remix could compensate. |
| [`progress.md`](progress.md) | Why the backend is shaped the way it is (the decisions that are expensive to rediscover), plus the checkpoint log. **Add an entry whenever `lib/dx9/` changes.** |

If the question is about how the game *looks* under Remix rather than about the
D3D9 backend, the design work is in the other two repos — start at
`dusklight-ao/docs/kankyo-remix.md`.

## Design in one paragraph

Aurora already funnels *every* draw (immediate-mode GX **and** J3D's baked
big-endian display lists) through one chokepoint: `aurora::gx::fifo::process()`
→ `draw_prim()` / `push_gx_draw()` in `lib/gx/command_processor.cpp`, with the
complete decoded GX state in `g_gxState`. The D3D9 backend keeps all of that
state decoding and intercepts at that chokepoint: it decodes the raw GX vertex
stream on the CPU into fixed-function-friendly interleaved vertices (float
positions/normals/UVs, D3DCOLOR colors, blend weights+indices), sets D3D9
transforms/render states/texture-stage states derived from `g_gxState`, and
issues `DrawIndexedPrimitiveUP` calls immediately (single-threaded, inside the
game thread's FIFO drain, between `BeginScene`/`EndScene`). Dawn/WebGPU is never
initialized in this mode; textures reuse Aurora's CPU decoders
(`texture_convert.cpp`) into `D3DFMT_A8R8G8B8` (+`DXT1` for PC formats).
Skinning maps to fixed-function **indexed vertex blending**
(`D3DTS_WORLDMATRIX(i)` palette): PNMTXIDX draws use one index with weight 1;
the `GXSetSkinning` extension bakes ≤4 {index,weight} pairs per vertex.
Projection/model-view come through `SetTransform`, which is exactly what Remix
introspects.

## Build & run (target state)

- CMake option `AURORA_ENABLE_D3D9` (default `ON` for WIN32; forced `OFF` elsewhere).
- Runtime selection: Dusklight `--backend d3d9`, or config `backend.graphicsBackend = "d3d9"`,
  or the in-game Prelaunch → Graphics Backend menu.
- For RTX Remix: drop Remix's `d3d9.dll` next to the Dusklight executable and
  select the d3d9 backend. The backend intentionally uses plain
  `Direct3DCreate9`, `*UP` draw calls, `SetTransform`, `SetTexture`, and
  fixed-function skinning — the API subset Remix hooks best.

## Status

What is broken right now, across all three repos:
`dusklight-ao/docs/remix-open-issues.md`. What changed here and why:
[`progress.md`](progress.md).
