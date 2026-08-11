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


def main() -> int:
    check_conflict_markers()
    check_matrep_fields_documented()
    check_side_channel_map()
    check_draw_stats_period()

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
