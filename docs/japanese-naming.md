# The game's names are Japanese — what that means in aurora

Aurora contains **no Japanese at all** (`rg -l '\p{Hiragana}|\p{Katakana}|\p{Han}'
lib include` → 0 files) and none of its own symbols are romaji. So this document is
not a naming convention for aurora. It is here for two reasons:

1. **Sessions in this repo read the game tree.** Any question about *why* a GX
   command stream looks the way it does is answered in `dusklight-ao`, where every
   identifier is a Japanese word in Latin letters. The canonical reference is
   `dusklight-ao/docs/japanese-naming.md`; §§1–5 there are the rules, and they are
   not restated here.
2. **Aurora is the only place a baked meaning becomes readable.** The game hands us
   TEV state and material names; the *equations* that give them meaning live in
   `.bmd` asset data, not in the source tree. Aurora translates that state into
   WGSL, so aurora is where a question the source cannot answer can be answered by
   reading a generated shader. §3 is that method, and it has an open question
   waiting for it.

---

## 1. The one operational rule, restated because it bites here too

The container's default locale is `POSIX`, and the game's own debug labels — the
best documentation of what its fields mean — are literal kana/kanji. Measured on
`dusklight-ao`:

| Command | POSIX | `LC_ALL=C.UTF-8` |
| :-- | --: | --: |
| `grep -rlP '\p{Han}' src include` | **0**, silently | 427 |
| `grep -rlP '[ぁ-んァ-ヶ一-龥]' src include` | **507** (11 false positives) | 496 |
| `rg -l '\p{Hiragana}\|\p{Katakana}\|\p{Han}' src include` | **496** | 496 |

**Use ripgrep** (and the Claude Code `Grep` tool, which is ripgrep) — it is correct
under either locale. If you shell out to `grep -P`, `export LC_ALL=C.UTF-8` first.
An empty `grep -P` for Japanese is not evidence of absence, and a non-empty one is
not evidence of presence.

---

## 2. What of the game's naming actually crosses into aurora

Aurora sees the game through GX, so most of the vocabulary is filtered out. What
survives, and what does not:

| Carrier | Does the game's name survive? | Notes |
| :-- | :-- | :-- |
| **Material names** | **Yes, when the game pushes them** | `J3DMatPacket::draw` calls `GXPushDebugGroup("Mat: <name>")` immediately before `callDL()` — the real draw site. That arrives here as FIFO subcommand `GX_AURORA_DEBUG_GROUP_PUSH` (`0x0020`) and becomes a WebGPU debug group. |
| **Texture identity** | **No** | `build_source_key` in `lib/gfx/texture_replacement.cpp` hashes the texel data (`XXH64`), TLUT, dimensions and format. Replacement is content-keyed end to end; no game name enters it. |
| **TEV / KColor semantics** | **The values, not the meaning** | `lib/gx/shader.cpp` maps `GX_TEV_KCSEL_K1_R` → `vec3f(ubuf.kcolor1.r)`. What that channel *means* was decided by an artist in a `.bmd`. |
| **Fog** | **The state, not the intent** | `shader.cpp` emits the GX fog curve from `config.fogType`; the game's four *ambient* layers and its separate 「ウソFog」 ("fake fog") term are not GX fog and never reach this path. |
| **Actor / function names** | **No** | Nothing above the GX API crosses. |

### The material-name path, precisely

This matters because it is the one semantic channel the game already ships, and it
is easy to record wrongly in either direction:

- `mpMaterial->mMaterialName` is populated by `AssignMaterialNames` in
  `J3DModelLoader.cpp` under `#if TARGET_PC` — so **the name is live in every PC
  build, release included**.
- The `GXPushDebugGroup` call in `J3DPacket.cpp` that forwards it to us is under
  `#if DEBUG && TARGET_PC`, so **the push is debug-only**.
- On our side, `AURORA_GFX_DEBUG_GROUPS` is defined when `NDEBUG` is not, so a
  release aurora drops the group even if the game pushed one.

**Both sides must be debug builds for a material name to reach a capture.** The
name being live in release is what makes it useful to a game-linked mod, which can
read it directly rather than going through GX.

The names carry real semantics — the codes are `MAnn` (the game calls them
`ポリゴンコード`, "polygon codes"), and the suffix after the code is descriptive
romaji: `MA00_Gake` (崖 cliff), `MA00_Kusa` (草 grass), `MA00_Enkei_Tree_Color`
(遠景 distant scenery), `MA06` variants for 波 *nami* (wave), 濁り *nigori* (murk)
and 水際 *mizugiwa* (shoreline). Note the granularity trap: `MA06` alone covers all
three of those, so a control that cuts on the code is coarser than the name.

---

## 3. Aurora as the instrument: reading a baked meaning

The game writes values into TEV registers whose *interpretation* is authored in the
model, not the code. Reading the game source therefore gives you the field and its
label but not the equation. Aurora closes that gap, because
`aurora::gfx::gx::build_shader` prints the whole TEV chain as WGSL.

**The live question, and it is our own.** `dusklight-mods`' Effect Remover rewrites
TEV KColor 1's red channel on the terrain materials. The game writes
`g_env_light.mFogDensity` there, from two places
(`d_kankyo.cpp:4511` inside a function the game itself named `dKy_cloudshadow_scroll`,
and `d_kankyo.cpp:11456` inside `dKy_bg_MAxx_proc`), and the original team's slider
labels that field **雲影の濃さ — cloud-shadow density** (`d_kankyo.cpp:5003`). Yet the
in-game test that produced the mod's current behaviour found the opposite polarity:
0 makes the shade *darker*, 255 washes it out.

Both facts are solid. The equation that reconciles them is in the material, and
**the shader aurora generates for an `MA04` draw is where it can be read**, not
argued about. Dumping the generated WGSL for one terrain material settles the
polarity, and with it whether "pin it to 255" is engine-faithful or merely
effective. Nobody has done it.

That is the general shape of aurora's contribution to this lens: **the game tells
you what a field is called; aurora tells you what the pixels do with it.**

---

## 4. Rules that apply here

1. **Never rename a game symbol** to make it read as English — including in a
   comment that quotes one. `wether` is the weather system.
2. **Aurora's own code stays English.** `lib/`, `include/` and everything we add is
   ordinary `snake_case`/`camelCase`. Do not romanize anything new; the convention
   describes the code we *read*.
3. **Gloss a game term the first time a document here uses it** (*kankyo* = 環境,
   environment), then use it bare. A reader who does not know the word cannot look
   it up.
4. **Say what was read and what was inferred.** A TEV meaning deduced from a
   Japanese label is a hypothesis; the same meaning read out of a generated shader
   is a measurement. They are different claims and this repo is where the second one
   is available.

---

## See also

- `dusklight-ao/docs/japanese-naming.md` — the canonical reference and the glossary
- `dusklight-mods/docs/japanese-naming.md` — the lens applied to the mods, including
  the open KColor-polarity question §3 describes how to settle
- `docs/thin-gbuffer-normals.md` — the other place aurora exposes per-draw
  information the game did not intend to publish
