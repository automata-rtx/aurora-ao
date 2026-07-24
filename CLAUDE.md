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

## Project context (DX9 fixed-function / RTX Remix work)

- **Branch structure (same in aurora-ao and dusklight-ao):**
  - `Fixed-Function` — the FF DX9 renderer branch (integration). Advances
    only by merging `Fixed-Function-dev` at tested/CI-green checkpoints.
  - `Fixed-Function-dev` — the working branch. ALL development commits land
    here. Never push to any other branch (except the checkpoint merges into
    `Fixed-Function`).
  - Base: this lineage descends from aurora `main`; when `main` gains new
    commits, backport by merging/cherry-picking into `Fixed-Function-dev`.
  - Legacy `claude/dusklight-dx9-fixed-function-6uoy92` is retired. If a
    remote session is still configured to push there, mirror the same
    commits to `Fixed-Function-dev` — this file is the standing
    authorization for that.
- This repo is vendored into dusklight-ao as the `extern/aurora` submodule;
  after pushing aurora commits, bump the submodule pin in dusklight-ao.
  Keep the branches paired across repos: dusklight `Fixed-Function-dev`
  pins aurora `Fixed-Function-dev` commits; before merging dusklight
  dev → `Fixed-Function`, merge aurora dev → `Fixed-Function` first so the
  pinned SHA is reachable from aurora's `Fixed-Function`.
- The D3D9 backend docs live in `docs/dx9/` — start with `README.md`, current
  state and resume notes in `progress.md` (update it at every checkpoint).
- Verify D3D9 code with the MinGW syntax harness (see `progress.md` §"How to
  resume") in both d3d9-on and d3d9-off configs; full builds happen on the
  owner's Windows machine and in dusklight-ao's GitHub Actions.
