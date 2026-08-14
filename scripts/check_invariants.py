#!/usr/bin/env python3
"""Facts this repo states in more than one place, checked mechanically.

This repo has no CI of its own, so dusklight-ao's Invariants workflow runs this
over `extern/aurora`. It is also worth running by hand before pushing.

Each check exists because something drifted:

  * `matrep.sum` is emitted from a format string in dx9_tev.cpp and documented
    field by field in material-report.md. A field was added to the log and not
    to the document, so a log carried a field nothing explained.
  * The D3DMATERIAL9 side-channel map in remix-material-interface.md §2 is the
    only place that records which channels are spare. A feature took two and
    updated the code comment but not §2, which went on advertising them as free.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

failures: list[str] = []
checks_run = 0


def fail(check: str, message: str) -> None:
    failures.append(f"[{check}] {message}")


def read(rel: str) -> str | None:
    p = REPO / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else None


def tracked_files() -> list[str]:
    out = subprocess.run(
        ["git", "-C", str(REPO), "ls-files"], capture_output=True, text=True, check=False
    )
    return out.stdout.splitlines() if out.returncode == 0 else []


# ---------------------------------------------------------------------------


def check_conflict_markers() -> None:
    global checks_run
    checks_run += 1
    starts = "<" * 7
    ends = ">" * 7
    suffixes = {".md", ".h", ".hpp", ".cpp", ".c", ".py", ".yml"}
    for rel in tracked_files():
        if Path(rel).suffix not in suffixes or rel == "scripts/check_invariants.py":
            continue
        text = read(rel)
        if text is None:
            continue
        for n, line in enumerate(text.splitlines(), 1):
            if line.startswith(starts) or line.startswith(ends):
                fail("conflict-markers", f"{rel}:{n} leftover merge marker")


def check_matrep_fields_documented() -> None:
    """Every field the matrep.sum line emits must be explained in the doc.

    A log field nobody can look up is the same as no field - the whole point of
    matrep is that a session answers questions without asking the owner.
    """
    global checks_run
    checks_run += 1

    src = read("lib/dx9/dx9_tev.cpp")
    doc = read("docs/dx9/material-report.md")
    if src is None or doc is None:
        fail("matrep", "lib/dx9/dx9_tev.cpp or docs/dx9/material-report.md is missing")
        return

    # The format string is split across adjacent string literals; join them.
    m = re.search(r'Log\.info\(\s*("matrep\.sum.*?")\s*,', src, re.S)
    if not m:
        fail("matrep", "could not find the matrep.sum format string in dx9_tev.cpp")
        return
    fmt = "".join(re.findall(r'"([^"]*)"', m.group(1)))

    fields = sorted(set(re.findall(r"(\w+)=\{", fmt)))
    if not fields:
        fail("matrep", "parsed the matrep.sum format string but found no 'name={' fields")
        return

    for field in fields:
        # Documented either as a table row `field` or in the worked example.
        if re.search(r"`+\*{0,2}`?" + re.escape(field) + r"`", doc) or re.search(
            r"\*\*`" + re.escape(field) + r"`\*\*", doc
        ):
            continue
        if re.search(r"\|\s*\*{0,2}`" + re.escape(field) + r"`", doc):
            continue
        fail(
            "matrep",
            f"matrep.sum emits '{field}=' but docs/dx9/material-report.md has no row for it - "
            f"a log field with no entry is a field nobody can read",
        )


def check_side_channel_map() -> None:
    """The channels set_remix_material writes and §2's field map must agree, both ways.

    Both directions matter, and for different reasons.

    write-without-row is the original case: a feature claims a channel and does
    not update the table, so the next feature reads the table, believes the
    channel is spare, and takes one already in use.

    row-without-write is the case that motivated making this bidirectional, and
    it is the more dangerous of the two because it is what a *merge* produces.
    A branch that forked before a channel was claimed carries a
    set_remix_material that never writes it. Resolving that conflict in that
    branch's favour drops the claim from the code while §2 - which merges
    cleanly, because the branch simply never touched those rows - goes on
    describing it. Checking only writes ⊆ rows passes that tree green.

    Power is checked alongside the four D3DCOLORVALUE fields because it is a
    side channel like any other and was invisible here until 2026-08-11.
    """
    global checks_run
    checks_run += 1

    src = read("lib/dx9/dx9_internal.hpp")
    doc = read("docs/dx9/remix-material-interface.md")
    if src is None or doc is None:
        fail("side-channels", "dx9_internal.hpp or remix-material-interface.md is missing")
        return

    m = re.search(r"inline void set_remix_material\((.*?)\n\}", src, re.S)
    if not m:
        fail("side-channels", "could not find set_remix_material in dx9_internal.hpp")
        return
    body = m.group(1)

    written: set[tuple[str, str]] = set()
    for field in ("Ambient", "Diffuse", "Specular", "Emissive"):
        assign = re.search(
            r"mat\." + field + r"\s*=\s*D3DCOLORVALUE\{([^}]*)\}", body
        )
        if assign:
            # Positional r,g,b,a - a literal 0.f in a slot means "not carrying anything".
            for chan, expr in zip("rgba", [e.strip() for e in assign.group(1).split(",")]):
                if expr and not re.fullmatch(r"0(\.0*)?f?", expr):
                    written.add((field, chan))
        elif re.search(r"mat\." + field + r"\s*=\s*\w+\s*;", body):
            # Assigned wholesale from a parameter: every channel is in use.
            written |= {(field, c) for c in "rgba"}

    # Power is a bare float rather than a D3DCOLORVALUE, so it has no channel.
    power = re.search(r"mat\.Power\s*=\s*([^;]+);", body)
    if power and not re.fullmatch(r"0(\.0*)?f?", power.group(1).strip()):
        written.add(("Power", ""))

    # Only the field map itself counts as documentation - the contiguous run of
    # rows under its header. §2 also carries prose tables *about* channels (which
    # in-flight branch claims which spare one), and those name fields without
    # being a claim that set_remix_material writes them.
    lines = doc.splitlines()
    try:
        start = next(
            n for n, l in enumerate(lines)
            if re.match(r"\|\s*Field\s*\|\s*Carries\s*\|", l)
        )
    except StopIteration:
        fail("side-channels", "could not find the §2 field-map header in remix-material-interface.md")
        return

    field_map: list[str] = []
    for line in lines[start + 1:]:
        if not line.lstrip().startswith("|"):
            break
        field_map.append(line)

    documented: set[tuple[str, str]] = set()
    for line in field_map:
        first_cell = line.split("|")[1] if line.count("|") >= 2 else ""
        for field, chans in re.findall(
            r"`(Ambient|Diffuse|Specular|Emissive)\.([rgba]+)`", first_cell
        ):
            for c in chans:
                documented.add((field, c))
        if re.search(r"`Power`", first_cell):
            documented.add(("Power", ""))

    def name(field: str, chan: str) -> str:
        return f"{field}.{chan}" if chan else field

    for field, chan in sorted(written - documented):
        fail(
            "side-channels",
            f"dx9_internal.hpp writes D3DMATERIAL9::{name(field, chan)} but "
            f"remix-material-interface.md §2 has no row for it - that table is the only place "
            f"recording which channels are still spare",
        )

    for field, chan in sorted(documented - written):
        fail(
            "side-channels",
            f"remix-material-interface.md §2 has a row for D3DMATERIAL9::{name(field, chan)} but "
            f"set_remix_material does not write it - either a merge dropped the feature that "
            f"claimed it, or the row is stale. See docs/dx9/in-flight-allocation.md",
        )


def check_draw_stats_period() -> None:
    """The dx9.draws reporting period is stated in the code and in four documents.

    kDrawStatsPeriod is the only place it is true; material-report.md,
    architecture-notes.md, CLAUDE.md and dusklight's playbook all quote it,
    two of them inside a worked example line (`frames=600`) that a reader will
    take as literal output. Changing the constant without those is the ordinary
    way a log example stops matching the log.
    """
    global checks_run
    checks_run += 1

    hdr = read("lib/dx9/dx9_internal.hpp")
    if hdr is None:
        fail("draw-stats", "lib/dx9/dx9_internal.hpp is missing")
        return

    m = re.search(r"kDrawStatsPeriod\s*=\s*(\d+)", hdr)
    if not m:
        fail("draw-stats", "could not find kDrawStatsPeriod in dx9_internal.hpp")
        return
    period = m.group(1)

    for rel in ["docs/dx9/material-report.md", "docs/dx9/architecture-notes.md", "CLAUDE.md"]:
        doc = read(rel)
        if doc is None:
            continue
        for n, line in enumerate(doc.splitlines(), 1):
            for stated in re.findall(r"every (\d+) frames", line):
                if stated != period:
                    fail(
                        "draw-stats",
                        f"{rel}:{n} says the dx9.draws period is {stated} frames but "
                        f"kDrawStatsPeriod is {period}",
                    )
            for stated in re.findall(r"dx9\.draws frames=(\d+)", line):
                if stated != period:
                    fail(
                        "draw-stats",
                        f"{rel}:{n} shows a dx9.draws example with frames={stated} but "
                        f"kDrawStatsPeriod is {period} - the example cannot occur",
                    )


def check_aurora_opcode_registry() -> None:
    """Every GX_AURORA_* subcommand must be unique and listed in the registry.

    handle_aurora() in command_processor.cpp is a flat `else if` chain. Two
    features that take the same number do NOT conflict there - both arms merge,
    the first tested wins, and because the arms consume different payload
    lengths the loser leaves the FIFO reader mid-payload. The symptom is a
    garbled frame or a CHECK naming an opcode the game never called, which is
    debugged nowhere near the cause.

    On 2026-08-11 four unmerged branches had each taken 0x0053. Nothing can see
    an unmerged branch, so this checks the two things that ARE checkable: that
    the numbers in this tree are distinct, and that each one appears in the
    registry comment - which is what makes a second claimant conflict in a place
    where the conflict means something.
    """
    global checks_run
    checks_run += 1

    hdr = read("include/dolphin/gx/GXAurora.h")
    if hdr is None:
        fail("opcodes", "include/dolphin/gx/GXAurora.h is missing")
        return

    # Subcommand defines only: the payload enums (WATER_LAYER_*, DRAW_CLASS_*)
    # are plain small integers, not slots in the dispatch chain.
    defines: dict[str, int] = {}
    for name, value in re.findall(
        r"^#define\s+(GX_AURORA_[A-Z0-9_]+)\s+(0x[0-9A-Fa-f]{4})\s*$", hdr, re.M
    ):
        defines[name] = int(value, 16)

    if not defines:
        fail("opcodes", "no GX_AURORA_* subcommand defines found in GXAurora.h")
        return

    by_value: dict[int, list[str]] = {}
    for name, value in defines.items():
        by_value.setdefault(value, []).append(name)

    for value, names in sorted(by_value.items()):
        if len(names) > 1:
            fail(
                "opcodes",
                f"{', '.join(sorted(names))} all use subcommand {value:#06x} - the else-if "
                f"dispatch in command_processor.cpp does not conflict on this, so the losing "
                f"arm silently desyncs the FIFO. See docs/dx9/in-flight-allocation.md",
            )

    registry = re.search(r"Aurora subcommand registry(.*?)\*/", hdr, re.S)
    if not registry:
        fail("opcodes", "the subcommand registry comment is gone from GXAurora.h")
        return

    body = registry.group(1)

    # Only the "Allocated:" list counts as coverage by number. The registry also
    # ends with "Next free: 0x00NN", and treating a bare value anywhere in the
    # comment as an entry would let the very next opcode taken pass unlisted -
    # precisely the case this check exists for.
    allocated = re.search(r"Allocated:(.*?)(?:\n\s*\*\s*\n|Reserved)", body, re.S)
    alloc_text = allocated.group(1) if allocated else ""
    ranges = [
        (int(lo, 16), int(hi, 16))
        for lo, hi in re.findall(r"(0x[0-9A-Fa-f]{4})\s*-\s*(0x[0-9A-Fa-f]{4})", alloc_text)
    ]
    singles = {
        int(v, 16)
        for v in re.findall(r"(?<![-\w])(0x[0-9A-Fa-f]{4})(?!\s*-\s*0x)", alloc_text)
    }

    for name, value in sorted(defines.items(), key=lambda kv: kv[1]):
        if name in body:
            continue
        if value in singles or any(lo <= value <= hi for lo, hi in ranges):
            continue
        fail(
            "opcodes",
            f"{name} = {value:#06x} is not in the registry comment in GXAurora.h - that list "
            f"is what makes a second claimant conflict somewhere the conflict is meaningful",
        )


def check_water_packing_contract() -> None:
    """The water packing is one contract written in two repositories.

    GX_AURORA_DUSKLIGHT_WATER_PACK in GXAurora.h encodes role/tag/layer into
    D3DMATERIAL9::Power; rtx_dusklight_water.h in the fork decodes it. Nothing
    links them, so a change to either side is silent - and the failure it
    produces is water rendering exactly as it did before the feature existed,
    which reads as "the feature does not work" rather than "the wire changed".

    Only aurora's half is visible from this repo. Check that the shape it states
    is the shape the documentation states, so the fork's side has something
    unambiguous to be checked against.
    """
    global checks_run
    checks_run += 1

    hdr = read("include/dolphin/gx/GXAurora.h")
    doc = read("docs/dx9/remix-material-interface.md")
    if hdr is None or doc is None:
        fail("water-packing", "GXAurora.h or remix-material-interface.md is missing")
        return

    macro = re.search(
        r"#define\s+GX_AURORA_DUSKLIGHT_WATER_PACK\(role,\s*tag,\s*layer\)\s*\\?\s*\n?\s*(.+)",
        hdr,
    )
    if not macro:
        # Water may simply not be present on this branch; only complain if the
        # documentation says it is.
        if "GX_AURORA_DUSKLIGHT_WATER_PACK" in doc:
            fail(
                "water-packing",
                "remix-material-interface.md describes GX_AURORA_DUSKLIGHT_WATER_PACK but "
                "GXAurora.h does not define it",
            )
        return

    expr = macro.group(1)
    tag_mul = re.search(r"\(u32\)\(tag\)\s*\*\s*(\d+)u", expr)
    layer_mul = re.search(r"\(u32\)\(layer\)\s*\*\s*(\d+)u", expr)
    if not tag_mul or not layer_mul:
        fail(
            "water-packing",
            f"could not read the tag/layer multipliers out of "
            f"GX_AURORA_DUSKLIGHT_WATER_PACK: {expr.strip()}",
        )
        return

    stated = f"tag * {tag_mul.group(1)} + layer * {layer_mul.group(1)} + role"
    if stated not in doc:
        fail(
            "water-packing",
            f"GXAurora.h packs water as `{stated}` but remix-material-interface.md does not "
            f"state that formula - the fork decodes from the documented one",
        )

    # The packing only stays lossless while the ranges fit their decimal slots.
    for macro_name, limit, slot in (
        ("GX_AURORA_DUSKLIGHT_WATER_ROLE_MAX", int(layer_mul.group(1)), "role"),
        ("GX_AURORA_DUSKLIGHT_WATER_LAYER_MAX", int(tag_mul.group(1)) // int(layer_mul.group(1)), "layer"),
    ):
        m = re.search(r"#define\s+" + macro_name + r"\s+(\d+)", hdr)
        if not m:
            fail("water-packing", f"{macro_name} is missing - the packing has no stated bound")
            continue
        if int(m.group(1)) >= limit:
            fail(
                "water-packing",
                f"{macro_name} is {m.group(1)}, which does not fit the {slot} slot "
                f"(< {limit}) - the packed value would carry into the next field",
            )


def check_log_format_strings() -> None:
    """A Log.* format string must be a literal, because fmt's is consteval.

    Log.info and friends take fmt::format_string, whose constructor is consteval from
    fmt 10 onwards. Anything that reads a runtime value in that argument - a ternary
    picking between two messages is the natural way to write it - is not a constant
    expression, and MSVC rejects it as:

        error C7595: 'fmt::v12::fstring<>::fstring': call to immediate function is
        not a constant expression

    which names the fmt header, not the offending line's actual problem.

    Nothing else we can run catches it. scripts/check_syntax.sh shims fmt at the
    container's 9.x, where format_string was still a plain type, and the shim's own
    note says it does not validate format strings. This repo has no CI, so the first
    report comes from dusklight's Windows job several minutes into a build - which is
    where it came from on 2026-08-14, in set_dusklight_draw_meta's resolution notice.

    The fix is always the same: two calls, each with its own literal.
    """
    global checks_run
    checks_run += 1

    # Log.report takes the level first and the format string second; every other level
    # takes the format string first.
    call_re = re.compile(r"\bLog\.(debug|info|warn|error|fatal|report)\s*\(")

    for rel in tracked_files():
        if not rel.endswith((".cpp", ".hpp", ".h")):
            continue
        src = read(rel)
        if src is None or "Log." not in src:
            continue

        # Macro bodies are exempt. In ASSERT/FATAL and friends the format argument is the
        # macro's own parameter, and the literal is supplied by the caller at expansion -
        # which is where consteval evaluates it, so those are correct as written.
        macro_spans: list[tuple[int, int]] = []
        for d in re.finditer(r"^[ \t]*#[ \t]*define\b", src, re.MULTILINE):
            end = d.end()
            while True:
                nl = src.find("\n", end)
                if nl == -1:
                    end = len(src)
                    break
                # A trailing backslash continues the definition onto the next line.
                if src[:nl].rstrip().endswith("\\"):
                    end = nl + 1
                    continue
                end = nl
                break
            macro_spans.append((d.start(), end))

        for m in call_re.finditer(src):
            if any(lo <= m.start() < hi for lo, hi in macro_spans):
                continue
            level, i = m.group(1), m.end()

            if level == "report":
                # Skip the level argument: scan to the first comma at paren depth 0.
                depth = 0
                while i < len(src):
                    c = src[i]
                    if c in "([{":
                        depth += 1
                    elif c in ")]}":
                        if depth == 0:
                            break
                        depth -= 1
                    elif c == "," and depth == 0:
                        i += 1
                        break
                    i += 1

            # Skip whitespace and comments to reach the format argument itself.
            while i < len(src):
                if src[i].isspace():
                    i += 1
                elif src.startswith("//", i):
                    i = src.find("\n", i) + 1 or len(src)
                elif src.startswith("/*", i):
                    i = src.find("*/", i) + 2
                else:
                    break

            # A literal, an adjacent-concatenated literal, or a raw/encoded literal is fine.
            if i < len(src) and (src[i] == '"' or src.startswith(('R"', 'L"', 'u8"'), i)):
                continue

            line = src.count("\n", 0, m.start()) + 1
            snippet = " ".join(src[m.start():m.start() + 90].split())
            fail(
                "log-format",
                f"{rel}:{line}: Log.{level} is called with a format string that is not a "
                f"literal - `{snippet}...`. fmt::format_string is consteval, so MSVC will "
                f"reject this with C7595 naming the fmt header rather than this line. Use "
                f"one call per literal message",
            )


def main() -> int:
    check_conflict_markers()
    check_matrep_fields_documented()
    check_side_channel_map()
    check_draw_stats_period()
    check_aurora_opcode_registry()
    check_water_packing_contract()
    check_log_format_strings()

    if failures:
        print(f"{len(failures)} inconsistency/ies across {checks_run} checks:\n")
        for f in failures:
            print(f"  {f}")
        print(
            "\nThese are facts stated in more than one place that no longer agree. "
            "A clean git merge does not mean they do - see CLAUDE.md, "
            "'Merges that succeed and are still wrong'."
        )
        return 1

    print(f"aurora invariants: {checks_run} checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
