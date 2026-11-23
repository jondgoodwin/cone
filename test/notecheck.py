#!/usr/bin/env python3
"""Note checker for the design notes: doc/design/ and compiler/c/doc/.

Two rules fail the run:
  1. every relative markdown link resolves to a file (or folder) that exists,
     and a '#anchor' on a link to a markdown file names a heading in it;
  2. no note holds a wiki-link ([[name]] or [[name|label]]): the dependency runs
     one way, work item to design note, so a note never points at a work item.

One rule only reports, for a human to read ("used to" also means "employed to"):
  3. phrases that narrate a note's own history ("used to", "no longer", "then
     fixed", "has since", "now <verb>s"). Rule: doc/design/_index.md, "Conventions".

Fenced code blocks and inline code are not read. Exit 0 when rules 1 and 2 hold,
1 otherwise; rule 3 never changes the exit code. `--quiet` hides rule 3's list.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
ROOTS = (REPO / "doc" / "design", REPO / "compiler" / "c" / "doc")

LINK = re.compile(r"(?<!\!)\[[^\]\n]*\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)")
WIKI = re.compile(r"\[\[[^\]\n]+\]\]")
INLINE_CODE = re.compile(r"`[^`\n]*`")
HISTORY = re.compile(
    r"\b(used to|no longer|then fixed|has since|have since|now (?:\w+s|returns|writes|reads|allows|refuses)\b)",
    re.IGNORECASE)
FENCE = re.compile(r"^\s*(```|~~~)")


def heading_anchors(path: Path, cache: dict[Path, set[str]]) -> set[str]:
    if path not in cache:
        anchors: set[str] = set()
        fenced = False
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            if FENCE.match(line):
                fenced = not fenced
            elif not fenced and line.startswith("#"):
                text = INLINE_CODE.sub(lambda m: m.group(0).strip("`"), line.lstrip("#").strip())
                slug = re.sub(r"[^\w\- ]", "", text.lower()).replace(" ", "-")
                anchors.add(slug)
        cache[path] = anchors
    return cache[path]


def prose_lines(path: Path):
    """(line number, text) for each line outside a fence, with inline code blanked.
    A code span may run over a line break inside one paragraph."""
    lines: list[str] = []
    fenced = False
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if FENCE.match(line):
            fenced = not fenced
            lines.append("")
        else:
            lines.append("" if fenced else line)
    text = "\n".join(lines)
    text = re.sub(r"`[^`\n]*(?:\n[^`\n]+)*`", lambda m: "\n" * m.group(0).count("\n"), text)
    for number, line in enumerate(text.split("\n"), 1):
        yield number, line


def check_notes(roots, repo: Path) -> tuple[list[str], list[str], int]:
    """(failures, narrated-history hits, notes read) for the notes under 'roots'."""
    notes = sorted(p for root in roots if root.is_dir() for p in root.rglob("*.md"))
    failures: list[str] = []
    history: list[str] = []
    anchors: dict[Path, set[str]] = {}
    for note in notes:
        rel = note.relative_to(repo).as_posix()
        for number, text in prose_lines(note):
            for target in LINK.findall(text):
                if re.match(r"[a-z][a-z0-9+.-]*:", target) or target.startswith("#") and len(target) == 1:
                    continue                    # a URL, or a bare '#'
                path_part, _, anchor = target.partition("#")
                dest = note if not path_part else (note.parent / path_part)
                if not dest.exists():
                    failures.append(f"{rel}:{number}: dangling link {target}")
                elif anchor and dest.suffix == ".md" and anchor.lower() not in heading_anchors(dest, anchors):
                    failures.append(f"{rel}:{number}: no heading for the anchor in {target}")
            for wiki in WIKI.findall(text):
                failures.append(f"{rel}:{number}: wiki-link to a work item {wiki}")
            for hit in HISTORY.finditer(text):
                history.append(f"{rel}:{number}: {hit.group(0)!r}")
    return failures, history, len(notes)


def selftest() -> int:
    """The three rules on a small tree of notes; returns the number of checks that hold."""
    import tempfile
    with tempfile.TemporaryDirectory(prefix="cone-notecheck-") as tmp:
        root = Path(tmp) / "doc"
        root.mkdir()
        (root / "a.md").write_text(
            "# Title\n\n## Second part\n\n[ok](b.md) [ok2](b.md#heading-b) [self](#second-part)\n"
            "[gone](missing.md) [badanchor](b.md#nope) [web](https://x.example/y)\n"
            "[[Work Item|label]] but `[[in code]]` and `[c](nowhere.md)` are not read\n"
            "```\n[[fenced]] [c](nowhere.md)\n```\n"
            "It used to be so.\n", encoding="utf-8")
        (root / "b.md").write_text("# Heading B\n", encoding="utf-8")
        failures, history, count = check_notes([root], Path(tmp))
        want = [
            "doc/a.md:6: dangling link missing.md",
            "doc/a.md:6: no heading for the anchor in b.md#nope",
            "doc/a.md:7: wiki-link to a work item [[Work Item|label]]",
        ]
        assert failures == want, failures
        assert history == ["doc/a.md:11: 'used to'"], history
        assert count == 2, count
    return 3


def main(argv: list[str]) -> int:
    if "--selftest" in argv:
        print(f"notecheck: {selftest()} checks hold")
        return 0
    quiet = "--quiet" in argv
    failures, history, count = check_notes(ROOTS, REPO)
    for line in failures:
        print(f"notecheck: {line}")
    if history and not quiet:
        print(f"notecheck: {len(history)} possible narrated-history phrase(s), for a human to read (not a failure):")
        for line in history:
            print(f"  {line}")
    print(f"notecheck: {count} notes, {len(failures)} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
