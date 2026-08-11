# Claude session notes — aurora-ao (`main`)

Our fork of `encounter/aurora`, the GX → modern-GPU backend Dusklight renders
through. `main` is **pristine upstream at the commit the Dusklight base pins, plus
one delta**: the enlarged per-frame streaming buffers in `lib/gfx/common.hpp`
(Index 4 MB, Vertex 16 MB, Storage 16 MB; Uniform/TextureUpload 24 MB).

**Keep it that way.** `dusklight-ao` vendors this as the `extern/aurora` submodule
and `dusklight-mods` builds against the result, so an unplanned change here
propagates to both. Documentation is fine; anything in `lib/` or `include/` is a
re-platforming decision.

## The game's code is named in Japanese — aurora's is not

Aurora contains no Japanese and none of its symbols are romaji. But every session
here that asks *why* the GX stream looks a certain way ends up reading
`dusklight-ao`, where **every identifier is a Japanese word in Latin letters**,
preserved 1:1 by the decompilation. Read as English they produce confident, wrong
answers.

`kankyo` (環境) is *environment*. `dKyw_wether_move` is the **weather** system and
`wether` is not a typo to fix.

**`docs/japanese-naming.md` here** covers what this specifically means for aurora —
what of the game's naming crosses the GX boundary and what does not, and the one
thing aurora can do that neither other repo can: **read a baked TEV meaning out of
the shader it generates**, instead of inferring it from a field name. The full
reference is `dusklight-ao/docs/japanese-naming.md`.

Two rules to carry now:

- **Search Japanese with ripgrep, not `grep -P`.** This container's locale is
  `POSIX`. Under it `grep -P '\p{Han}'` over the game tree silently matches
  **nothing** (exit 1, no error) while a raw kana/kanji character class silently
  matches **too much**. `rg` — and the Claude Code `Grep` tool, which is ripgrep —
  is correct either way. If you must use `grep -P`, `export LC_ALL=C.UTF-8` first.
- **Never rename a game symbol**, including inside a comment that quotes one. They
  are load-bearing across `zeldaret/tp` and every grep anyone runs.

Aurora's own code stays ordinary English. The convention describes the code we
*read*, not the code we *write*.

## Docs

| File | Contents |
| :-- | :-- |
| `docs/japanese-naming.md` | what the game's Japanese naming means at the GX boundary, and how to read a baked TEV meaning out of a generated shader |
| `docs/thin-gbuffer-normals.md` | the other place aurora exposes per-draw information the game did not intend to publish |
| `docs/building.md` | building aurora |

## Related repos

- `automata-rtx/dusklight-ao` — the game (Twilight Princess decomp/port); vendors
  this repo at `extern/aurora`
- `automata-rtx/dusklight-mods` — the graphics mods, built against the SDK the
  Dusklight base publishes

Other branches of this fork belong to a separate RTX Remix / fixed-function
experiment. **`main` is the baseline for mod-related work.**
