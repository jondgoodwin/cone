#!/usr/bin/env python3
"""Cone compiler test suite.

Runs every scenario under ``test/cases/`` and asserts what its category and its
expectations claim. Python 3.11+, no third-party dependencies.

    python test/run.py                  run everything (R1.7)
    python test/run.py core             run one group
    python test/run.py core-overload    run one scenario
    python test/run.py --list           print what would run, run nothing (R2.6)
    python test/run.py --coverage       ErrorCode coverage, run nothing (R6.4)
    python test/run.py --build          build the compiler first (R1.1)
    python test/run.py --bless-codes    regenerate test/codes.toml (R5.2)

The runner's whole vocabulary is the command line, the exit code, stderr,
stdout, and the files a run produced (R2.7). It knows nothing about compiler
internals beyond the diagnostic text format and the ``ErrorCode`` enum it reads
out of ``src/c-compiler/shared/error.h``.

Deferred, and why, is recorded in ``workitems/Add test suite.md``. What is left:
bless (R4.2/R4.4) and diff-driven selection (R2.5). Any scenario asking for a
deferred feature is a hard configuration error rather than something quietly
ignored.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import difflib
import os
import re
import subprocess
import sys
import threading
import time
import tomllib
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CASES = REPO / "test" / "cases"
CODES_TOML = REPO / "test" / "codes.toml"
ERROR_H = REPO / "src" / "c-compiler" / "shared" / "error.h"
IS_WINDOWS = os.name == "nt"

# Tier per group, from the group table in design/Test Suite.md section 1.
# Results are reported tier 0 first, because tier 1 and 2 groups assume the
# foundation works and a foundation break would otherwise be buried under the
# downstream failures it caused (R2.8).
#
# 'driver' is the one group that is not in that table and never will be: it
# tests the compiler's command line rather than the language, so it follows no
# reference-manual chapter (R6.1). It is tier 0 because nothing else means
# anything if conec cannot be invoked.
TIERS = {
    "driver": 0,
    "lexical": 0,
    "core": 0,
    "struct": 1,
    "union": 1,
    "array": 1,
    "closure": 1,
    "ref": 1,
    "move": 1,
    "region": 2,
    "trait": 2,
    "collection": 2,
    "each": 2,
    "typemgmt": 2,
    "generic": 2,
    "module": 2,
    "exception": 2,
    "concurrency": 2,
    "safety": 2,
    "meta": 2,
}

# src/c-compiler/shared/error.h. Matched exactly, never as "nonzero" (R1.2).
EXIT_NAMES = {
    0: "ExitSuccess",
    1: "ExitError",
    2: "ExitNF",
    3: "ExitMem",
    4: "ExitOpts",
    5: "ExitIndent",
    6: "ExitGen",
}

CATEGORIES = ("compile", "run", "warn", "reject", "recover", "driver")

# The category's exit status where cases.toml does not override it (R2.10).
# 'driver' has no default: naming the status is the whole point of the category,
# so it is required rather than inherited.
DEFAULT_EXIT = {"compile": 0, "run": 0, "warn": 0, "reject": 1, "recover": 1}

# Categories whose diagnostics are located, and so may carry //~ annotations.
# 'warn' shares the mechanism with 'reject' because a warning is the same enum
# printed in the same format; only the code's block differs.
ANNOTATABLE = ("reject", "warn")

SCENARIO_KEYS = {
    "category", "description", "tags", "diagnostics", "exit", "xfail",
    "run", "unlocated", "check", "argv",
}


class SuiteError(Exception):
    """A fault in the suite's own configuration, not in the compiler."""


# ---------------------------------------------------------------------------
# Diagnostic identity
# ---------------------------------------------------------------------------

def parse_error_codes(path: Path) -> dict[str, int]:
    """Read ``enum ErrorCode`` so cases can name diagnostics symbolically (R5.1).

    Every member carries an explicit value today, but this still walks the enum
    the way the C compiler does — a member written without one takes the
    previous value plus one — so a code added against the header's own
    convention is read correctly and then reported by ``check_codes_table``
    rather than silently mis-numbered here.
    """
    text = path.read_text(encoding="utf-8", errors="replace")
    match = re.search(r"enum\s+ErrorCode\s*\{(.*?)\}\s*;", text, re.S)
    if not match:
        raise SuiteError(f"no 'enum ErrorCode' found in {path}")
    body = re.sub(r"/\*.*?\*/", "", match.group(1), flags=re.S)
    body = re.sub(r"//[^\n]*", "", body)

    codes: dict[str, int] = {}
    value = 0
    for item in body.split(","):
        item = item.strip()
        if not item:
            continue
        member = re.fullmatch(r"([A-Za-z_]\w*)\s*(?:=\s*(\d+))?", item)
        if not member:
            raise SuiteError(f"cannot read enum member {item!r} in {path}")
        if member.group(2) is not None:
            value = int(member.group(2))
        codes[member.group(1)] = value
        value += 1
    return codes


CODES_HEADER = """\
# ErrorCode name to number, pinned (R5.2).
#
# Regenerate with:  python test/run.py --bless-codes
# and review the diff. The runner compares this table against
# src/c-compiler/shared/error.h before any case runs, and fails naming exactly
# which codes moved.
#
# These numbers are a published interface. Scenarios name codes symbolically
# (R5.1) but the compiler prints the number, so a renumber would invalidate
# every expectation at once and none of them loudly. Explicit values in error.h
# remove that hazard at the source; this table is defense in depth, catching a
# code added without following the header's convention.
#
# Written in the header's own declaration order, so the diff of a code added at
# the end of its block is one line.

"""


def write_codes_table(path: Path, codes: dict[str, int]) -> None:
    """R5.2. Generated, checked in, and verified against the source."""
    lines = [CODES_HEADER]
    for name, value in codes.items():
        lines.append(f"{name} = {value}\n")
    path.write_text("".join(lines), encoding="utf-8", newline="\n")


def check_codes_table(codes: dict[str, int], path: Path) -> None:
    """R5.2. Assert the checked-in table still describes ``error.h``.

    This runs before any case does, and a mismatch is a fault in the suite's own
    configuration rather than a compiler failure: every expectation in the corpus
    is matched on the number the compiler printed, so a renumber makes the whole
    corpus wrong at once. Failing here says so once, by name, instead of leaving
    it to be inferred from a page of unrelated-looking case failures.
    """
    if not path.exists():
        raise SuiteError(
            f"no pinned ErrorCode table at {path.relative_to(REPO).as_posix()} (R5.2)\n"
            f"  generate it with: python test/run.py --bless-codes"
        )
    try:
        with path.open("rb") as handle:
            table = tomllib.load(handle)
    except tomllib.TOMLDecodeError as broken:
        raise SuiteError(f"{path}: {broken}") from None

    bad = sorted(name for name, value in table.items() if not isinstance(value, int))
    if bad:
        raise SuiteError(f"{path}: not a number: {', '.join(bad)}")

    added = [n for n in codes if n not in table]
    removed = [n for n in table if n not in codes]
    moved = [(n, table[n], codes[n]) for n in codes if n in table and table[n] != codes[n]]
    if not (added or removed or moved):
        return

    lines = [
        f"{path.relative_to(REPO).as_posix()} no longer matches "
        f"{ERROR_H.relative_to(REPO).as_posix()} (R5.2)."
    ]
    if moved:
        lines.append("  renumbered, which invalidates every expectation naming them:")
        lines += [f"    {n}: {was} -> {now}" for n, was, now in moved]
    if added:
        lines.append("  in error.h, not in the table:")
        lines += [f"    {n} = {codes[n]}" for n in added]
    if removed:
        lines.append("  in the table, not in error.h:")
        lines += [f"    {n} = {table[n]}" for n in removed]
    lines.append("  regenerate with: python test/run.py --bless-codes, then review the diff")
    raise SuiteError("\n".join(lines))


@dataclass(frozen=True)
class Diagnostic:
    """One counted diagnostic the compiler actually emitted."""
    code: int
    message: str
    path: str | None
    line: int | None
    col: int | None

    @property
    def is_warning(self) -> bool:
        return self.code >= 3000

    def describe(self, codes_by_number: dict[int, str]) -> str:
        name = codes_by_number.get(self.code, "?")
        where = "no location"
        if self.line is not None:
            where = f"{self.path}:{self.line}:{self.col}"
        return f"{name} ({self.code}) at {where}: {self.message}"


DIAG_RE = re.compile(r"^(Error|Warning) (\d+): (.*)$")
LOC_RE = re.compile(r"\^--- (.*):(\d+):(\d+)$")


def parse_diagnostics(stderr: str) -> list[Diagnostic]:
    """Pull counted diagnostics out of stderr.

    ``errorOutCode`` prints three lines: the ``Error n:``/``Warning n:`` header,
    a ``-->`` echo of the source line, and a caret line carrying the location.
    The location is read from that third line only. The echoed source line
    contains the ``//~`` annotation text, so re-scanning it would match a
    scenario's own expectations back to itself.

    ``errorMsg`` prints the header alone, with no location. ``Uncounted`` (9000)
    prints with neither prefix, as does ``errorExit``'s closing summary, so
    neither is picked up here and neither is counted (R3.6).

    Blank lines between the echo and the caret are skipped, and only blank ones:
    with core.autocrlf on, a CRLF working tree gives the echo loop a trailing
    carriage return that stderr's text mode then turns into a second line
    ending, so the same compile prints differently depending on how git checked
    the scenario out (R4.1).
    """
    lines = stderr.split("\n")
    out: list[Diagnostic] = []
    i = 0
    while i < len(lines):
        header = DIAG_RE.match(lines[i])
        if not header:
            i += 1
            continue
        code, message = int(header.group(2)), header.group(3)
        path = line = col = None
        if i + 1 < len(lines) and lines[i + 1].startswith(" --> "):
            after = i + 2
            while after < len(lines) and not lines[after].strip():
                after += 1
            located = LOC_RE.search(lines[after]) if after < len(lines) else None
            if located:
                path = located.group(1)
                line = int(located.group(2))
                col = int(located.group(3))
                i = after
        out.append(Diagnostic(code, message, path, line, col))
        i += 1
    return out


# ---------------------------------------------------------------------------
# Inline //~ annotations
# ---------------------------------------------------------------------------

ANNOT_RE = re.compile(r"//~(\^*)[ \t]*(.*?)[ \t]*$")
ANNOT_BODY_RE = re.compile(
    r"""^(?P<code>[A-Za-z_]\w*)
         (?::(?P<col>\d+))?
         (?:[ \t]+"(?P<message>(?:[^"\\]|\\.)*)")?
         (?P<flags>(?:[ \t]+[A-Za-z][\w-]*)*)
         $""",
    re.X,
)


@dataclass(frozen=True)
class Annotation:
    """One expected located diagnostic, written as ``//~`` in the source."""
    code_name: str
    code: int
    line: int          # source line the diagnostic must be reported on
    col: int | None
    message: str | None
    follow_on: bool
    annot_line: int    # line the //~ itself sits on, for error reporting

    def describe(self) -> str:
        where = f"{self.line}:{self.col}" if self.col is not None else f"{self.line}"
        text = f"{self.code_name} ({self.code}) at {where}"
        if self.message is not None:
            text += f' "{self.message}"'
        if self.follow_on:
            text += " follow-on"
        return text


def parse_annotations(source: Path, codes: dict[str, int]) -> list[Annotation]:
    """Read ``//~ Code[:col] ["substring"] [follow-on]`` out of a scenario (R2.9).

    Carets count lines, and an annotation-only line is a line, so successive
    annotations for one code line each take one more caret. ``core-parse-decls``
    depends on this: three diagnostics on one line, reached with ``//~``,
    ``//~^`` and ``//~^^``.
    """
    text = source.read_text(encoding="utf-8", errors="replace").replace("\r\n", "\n")
    found: list[Annotation] = []
    for number, line in enumerate(text.split("\n"), start=1):
        at = line.find("//~")
        if at < 0:
            continue
        marker = ANNOT_RE.match(line[at:])
        carets, body = marker.group(1), marker.group(2)
        parsed = ANNOT_BODY_RE.match(body)
        if not parsed:
            raise SuiteError(
                f"{source}:{number}: cannot read annotation //~{carets} {body!r}\n"
                f'  expected: //~ Code[:col] ["substring"] [follow-on]'
            )
        name = parsed.group("code")
        if name not in codes:
            raise SuiteError(
                f"{source}:{number}: {name!r} is not an ErrorCode in {ERROR_H.name}"
            )
        flags = parsed.group("flags").split()
        unknown = [f for f in flags if f != "follow-on"]
        if unknown:
            raise SuiteError(
                f"{source}:{number}: unknown annotation flag(s) {' '.join(unknown)}"
            )
        target = number - len(carets)
        if target < 1:
            raise SuiteError(f"{source}:{number}: {len(carets)} carets point above line 1")
        message = parsed.group("message")
        if message is not None:
            message = message.replace('\\"', '"').replace("\\\\", "\\")
        found.append(Annotation(
            code_name=name,
            code=codes[name],
            line=target,
            col=int(parsed.group("col")) if parsed.group("col") else None,
            message=message,
            follow_on="follow-on" in flags,
            annot_line=number,
        ))
    return found


def match_diagnostics(
    expected: list[Annotation], actual: list[Diagnostic], source_rel: str
) -> tuple[list[Annotation], list[Diagnostic]]:
    """Pair annotations with diagnostics; return what was left over on each side.

    A pair needs the same code and the same ``line:column`` (R3.4). Where an
    annotation carries a quoted substring, it is required rather than
    decorative: ``lexical-reject-tokens`` has two diagnostics sharing a code, a
    line and a column, and the substring is the only thing that separates them.
    Annotations carrying one are therefore matched first, so they claim their
    own diagnostic before a substring-less annotation can absorb it.
    """
    unmatched_actual = list(actual)
    unmatched_expected: list[Annotation] = []

    def claim(annotation: Annotation) -> bool:
        for candidate in unmatched_actual:
            if candidate.code != annotation.code:
                continue
            if candidate.line != annotation.line:
                continue
            if annotation.col is not None and candidate.col != annotation.col:
                continue
            if normalize_path(candidate.path or "") != source_rel:
                continue
            if annotation.message is not None and annotation.message not in candidate.message:
                continue
            unmatched_actual.remove(candidate)
            return True
        return False

    for phase in (True, False):
        for annotation in expected:
            if (annotation.message is not None) != phase:
                continue
            if not claim(annotation):
                unmatched_expected.append(annotation)
    return unmatched_expected, unmatched_actual


# ---------------------------------------------------------------------------
# Discovery
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class Check:
    """A named assertion against a generated artifact (R2.3)."""
    name: str
    target: str
    contains: tuple[str, ...]
    excludes: tuple[str, ...]


@dataclass(frozen=True)
class RunSpec:
    name: str
    options: tuple[str, ...]


@dataclass
class Scenario:
    group: str
    tier: int
    name: str
    category: str
    description: str
    tags: tuple[str, ...]
    source: Path | None          # None for 'driver', which has no Cone source
    diagnostics: int | None
    exit_code: int
    runs: tuple[RunSpec, ...]
    checks: tuple[Check, ...]
    unlocated: tuple[dict, ...]
    argv: tuple[str, ...] = ()   # 'driver' only: the whole invocation
    xfail: bool = False
    annotations: list[Annotation] = field(default_factory=list)

    @property
    def source_rel(self) -> str:
        return self.source.relative_to(REPO).as_posix() if self.source else ""

    @property
    def sort_key(self):
        return (self.tier, self.group, self.name)


def _require_keys(where: str, table: dict, allowed: set[str]) -> None:
    unknown = sorted(set(table) - allowed)
    if unknown:
        raise SuiteError(f"{where}: unknown key(s) {', '.join(unknown)}")


def load_group(group_dir: Path, codes: dict[str, int]) -> list[Scenario]:
    group = group_dir.name
    if group not in TIERS:
        raise SuiteError(
            f"{group_dir}: group {group!r} has no tier.\n"
            f"  Groups come from the table in design/Test Suite.md section 1;"
            f" add it there and in TIERS in {Path(__file__).name}."
        )
    toml_path = group_dir / "cases.toml"
    if not toml_path.exists():
        raise SuiteError(f"{group_dir}: no cases.toml")
    try:
        with toml_path.open("rb") as handle:
            data = tomllib.load(handle)
    except tomllib.TOMLDecodeError as broken:
        raise SuiteError(f"{toml_path}: {broken}") from None

    _require_keys(str(toml_path), data, {"support", "scenario"})
    support = {Path(name).stem for name in data.get("support", [])}
    tables = data.get("scenario", {})

    scenarios: list[Scenario] = []
    for name, table in tables.items():
        where = f"{toml_path}: [scenario.{name}]"
        _require_keys(where, table, SCENARIO_KEYS)
        if not name.startswith(group + "-"):
            raise SuiteError(f"{where}: scenario name must start with {group!r}- (R2.11)")
        category = table.get("category")
        if category not in CATEGORIES:
            raise SuiteError(f"{where}: category must be one of {', '.join(CATEGORIES)}")

        # A driver scenario invokes conec without valid Cone source, so argv is
        # the whole input and there is no .cone file for R2.12 to require.
        source: Path | None = None
        argv: tuple[str, ...] = ()
        if category == "driver":
            if "argv" not in table:
                raise SuiteError(f"{where}: a 'driver' scenario needs argv (may be empty)")
            argv = tuple(str(a) for a in table["argv"])
            if "exit" not in table:
                raise SuiteError(
                    f"{where}: a 'driver' scenario needs an explicit exit status;"
                    f" asserting it is the whole category (R1.2)")
            if table.get("run"):
                raise SuiteError(
                    f"{where}: a 'driver' scenario takes its options from argv,"
                    f" so a [[run]] table would be silently ignored")
            stray = group_dir / f"{name}.cone"
            if stray.exists():
                raise SuiteError(f"{where}: a 'driver' scenario must have no {stray.name}")
        else:
            if "argv" in table:
                raise SuiteError(f"{where}: argv belongs to a 'driver' scenario only")
            source = group_dir / f"{name}.cone"
            if not source.exists():
                raise SuiteError(f"{where}: listed scenario has no {source.name} (R2.12)")

        # R2.10 names the total diagnostic count as recover's file-level
        # expectation. It asserts the count rather than each diagnostic, so
        # without it the scenario asserts nothing but the exit status.
        if category == "recover" and table.get("diagnostics") is None:
            raise SuiteError(f"{where}: a 'recover' scenario needs diagnostics = <count>")

        runs = tuple(
            RunSpec(entry.get("name", "default"), tuple(entry.get("options", [])))
            for entry in table.get("run", [])
        ) or (RunSpec("default", ()),)
        if len({r.name for r in runs}) != len(runs):
            raise SuiteError(f"{where}: two runs share a name")

        checks = []
        for entry in table.get("check", []):
            _require_keys(f"{where}.check", entry, {"name", "target", "contains", "excludes"})
            if entry.get("target") not in ("llvmir", "stdout"):
                raise SuiteError(f"{where}.check: target must be 'llvmir' or 'stdout'")
            if entry["target"] == "stdout" and category != "run":
                raise SuiteError(
                    f"{where}.check: only a 'run' scenario produces stdout to check")
            if entry["target"] == "llvmir" and category not in ("compile", "run"):
                raise SuiteError(
                    f"{where}.check: a {category!r} scenario reaches no code generation")
            checks.append(Check(
                name=entry["name"],
                target=entry["target"],
                contains=tuple(entry.get("contains", [])),
                excludes=tuple(entry.get("excludes", [])),
            ))

        scenario = Scenario(
            group=group,
            tier=TIERS[group],
            name=name,
            category=category,
            description=table.get("description", ""),
            tags=tuple(table.get("tags", [])),
            source=source,
            diagnostics=table.get("diagnostics"),
            exit_code=table.get("exit", DEFAULT_EXIT.get(category, 0)),
            runs=runs,
            checks=tuple(checks),
            unlocated=tuple(table.get("unlocated", [])),
            argv=argv,
            xfail=bool(table.get("xfail", False)),
        )
        if source is not None:
            scenario.annotations = parse_annotations(source, codes)
            if scenario.annotations and category not in ANNOTATABLE:
                first = scenario.annotations[0]
                raise SuiteError(
                    f"{source}:{first.annot_line}: only a {' or '.join(ANNOTATABLE)}"
                    f" scenario may carry //~ annotations; this one is {category!r}"
                )
        # A warn scenario that names no warning asserts nothing its category does
        # not already imply, which is the same hole R3.2 closes for compile.
        if category == "warn" and not scenario.annotations and not scenario.unlocated:
            raise SuiteError(
                f"{where}: a 'warn' scenario must name at least one warning,"
                f" as a //~ annotation or an [[unlocated]] entry")
        scenarios.append(scenario)

    # R2.12: a .cone file that is neither a listed scenario nor a listed support
    # module is an error, so a forgotten registration fails loudly.
    listed = {s.name for s in scenarios} | support
    for path in sorted(group_dir.glob("*.cone")):
        if path.stem not in listed:
            raise SuiteError(
                f"{path}: not listed in cases.toml as a scenario or a support"
                f" module (R2.12)"
            )
    return scenarios


def discover(codes: dict[str, int]) -> list[Scenario]:
    if not CASES.is_dir():
        raise SuiteError(f"no case directory at {CASES}")
    scenarios: list[Scenario] = []
    for group_dir in sorted(p for p in CASES.iterdir() if p.is_dir()):
        scenarios.extend(load_group(group_dir, codes))
    return sorted(scenarios, key=lambda s: s.sort_key)


def select(scenarios: list[Scenario], selectors: list[str]) -> list[Scenario]:
    if not selectors:
        return scenarios
    chosen: list[Scenario] = []
    unused = set(selectors)
    for scenario in scenarios:
        names = {scenario.group, scenario.name}
        names |= {f"tag:{t}" for t in scenario.tags}
        names |= {c.name for c in scenario.checks}
        hit = names & set(selectors)
        if hit:
            unused -= hit
            chosen.append(scenario)
    if unused:
        raise SuiteError(
            f"nothing matches {', '.join(sorted(unused))}\n"
            f"  a selector is a group, a scenario, a check name, or tag:<phase>"
        )
    return chosen


# ---------------------------------------------------------------------------
# Toolchain
# ---------------------------------------------------------------------------

def default_conec() -> Path:
    name = "conec.exe" if IS_WINDOWS else "conec"
    return REPO / "build" / "x64-release" / name


def newest_source(root: Path) -> tuple[float, Path | None]:
    newest, newest_path = 0.0, None
    for path in (root / "src").rglob("*"):
        if path.suffix in (".c", ".h") and path.is_file():
            stamp = path.stat().st_mtime
            if stamp > newest:
                newest, newest_path = stamp, path
    build_file = root / "CMakeLists.txt"
    if build_file.is_file() and build_file.stat().st_mtime > newest:
        newest, newest_path = build_file.stat().st_mtime, build_file
    return newest, newest_path


def vcvars_command(vcvars: str, rest: str) -> str:
    """cmd /c with a quoted batch path, as a single string.

    subprocess must not build this command line from a list: list2cmdline
    escapes the quotes around the batch path, and cmd then reports the whole
    quoted path as an unrecognised command. The doubled outer quote is cmd's own
    convention for a /c argument that itself contains quotes.
    """
    return f'cmd /c ""{vcvars}" >nul && {rest}"'


def build_compiler(conec: Path) -> None:
    build_dir = conec.parent
    command: str | list[str] = ["cmake", "--build", str(build_dir)]
    if IS_WINDOWS:
        vcvars = find_vcvars()
        if vcvars:
            command = vcvars_command(vcvars, f'cmake --build "{build_dir}"')
    print(f"building: {command if isinstance(command, str) else ' '.join(command)}",
          flush=True)
    result = subprocess.run(command, cwd=REPO)
    if result.returncode != 0:
        raise SuiteError("compiler build failed")


def check_not_stale(conec: Path, allow_stale: bool) -> None:
    """R1.1. A stale binary fails good sources in ways indistinguishable from a
    language regression, which is why this is a precondition and not a footnote:
    the binary checked in at build/x64-release/ once predated the overload work
    by a week and failed test/test.cone with 17 errors that looked exactly like
    a broken master."""
    if not conec.exists():
        raise SuiteError(f"no compiler at {conec}; run with --build, or pass --conec")
    newest, path = newest_source(REPO)
    if conec.stat().st_mtime >= newest:
        return
    message = (
        f"{conec} is older than {path.relative_to(REPO) if path else 'a compiler source'}.\n"
        f"  A stale binary fails good sources in ways that look like a language\n"
        f"  regression (R1.1). Rebuild, or re-run with --build."
    )
    if not allow_stale:
        raise SuiteError(message)
    print(f"warning: {message}\n", file=sys.stderr)


def find_vcvars() -> str | None:
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) \
        / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if vswhere.exists():
        result = subprocess.run(
            [str(vswhere), "-latest", "-products", "*",
             "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
             "-property", "installationPath"],
            capture_output=True, text=True,
        )
        root = result.stdout.strip().splitlines()
        if root:
            candidate = Path(root[0]) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
            if candidate.exists():
                return str(candidate)
    for edition in ("Community", "Professional", "Enterprise", "BuildTools"):
        candidate = Path(r"C:\Program Files\Microsoft Visual Studio\2022") / edition \
            / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
        if candidate.exists():
            return str(candidate)
    return None


class Linker:
    """Finds a linker once, and reports cleanly when there is none.

    R3.7: where no linker is available the ``run`` scenarios report skipped,
    visibly and distinctly from passed. On Windows ``link.exe`` needs the Visual
    Studio environment, so vcvars is sourced once and its environment reused
    rather than re-run per case.
    """

    def __init__(self, conestd: Path):
        self.conestd = conestd
        self.lock = threading.Lock()
        self.ready = False
        self.env: dict[str, str] | None = None
        self.tool: str | None = None
        self.reason: str | None = None

    def prepare(self) -> str | None:
        from shutil import which
        with self.lock:
            if self.ready:
                return self.reason
            self.ready = True
            if not self.conestd.exists():
                self.reason = f"no conestd library at {self.conestd}"
                return self.reason
            if IS_WINDOWS:
                self.env = self._vs_environment()
                if self.env is None:
                    self.reason = "no Visual Studio environment providing link.exe"
                    return self.reason
                # CreateProcess resolves the program against this process's PATH,
                # not the one being handed to the child, so the linker has to be
                # named absolutely out of the environment vcvars produced.
                self.tool = which("link.exe", path=self.env.get("PATH", ""))
                if self.tool is None:
                    self.reason = "the Visual Studio environment has no link.exe"
            else:
                self.tool = which("cc") or which("gcc")
                if self.tool is None:
                    self.reason = "no cc or gcc on PATH"
            return self.reason

    def _vs_environment(self) -> dict[str, str] | None:
        from shutil import which
        if which("link.exe"):
            return dict(os.environ)
        vcvars = find_vcvars()
        if not vcvars:
            return None
        result = subprocess.run(
            vcvars_command(vcvars, "set"),
            capture_output=True, text=True, errors="replace",
        )
        if result.returncode != 0:
            return None
        env = dict(os.environ)
        for line in result.stdout.splitlines():
            if "=" in line:
                key, value = line.split("=", 1)
                env[key] = value
        return env

    def command(self, obj: Path, exe: Path) -> list[str]:
        if IS_WINDOWS:
            return [
                self.tool, "/NOLOGO", str(obj), str(self.conestd), f"/OUT:{exe}",
                "/SUBSYSTEM:CONSOLE", "msvcrt.lib", "legacy_stdio_definitions.lib",
            ]
        return [self.tool, str(obj), str(self.conestd), "-o", str(exe), "-lm"]


# ---------------------------------------------------------------------------
# Process execution
# ---------------------------------------------------------------------------

@dataclass
class Completed:
    code: int | None
    stdout: str
    stderr: str
    killed: str | None
    seconds: float


def normalize(text: str) -> str:
    """R4.1. Git checks .out files out as CRLF on Windows while the repository
    holds LF, and there are no .gitattributes eol rules, so every comparison is
    line-ending agnostic."""
    return text.replace("\r\n", "\n").replace("\r", "\n")


def normalize_path(text: str) -> str:
    return text.replace("\\", "/")


TIME_RE = re.compile(r"Compile finished in [^ ]+ sec \(\d+ kb\)\.")


def normalize_stderr(text: str) -> str:
    """The success line carries elapsed seconds and kilobytes used, so it is not
    byte-stable. Nothing compares it literally today; this keeps it out of
    failure deltas so a diff shows the change rather than the clock (R4.1)."""
    return TIME_RE.sub("Compile finished in <t> sec (<n> kb).", text)


def execute(cmd: list[str], cwd: Path, out_dir: Path, stem: str,
            timeout: float, max_bytes: int, env: dict | None = None) -> Completed:
    """Run one process with stdin from null and a wall-clock timeout (R1.3).

    The timeout is not optional. ``::name`` at the start of a statement makes
    the parser loop forever emitting unbounded output, so output volume is
    capped too: a 20-second unbounded write would otherwise fill a pipe buffer
    or a disk before the clock ran out.
    """
    out_path = out_dir / f"{stem}.stdout"
    err_path = out_dir / f"{stem}.stderr"
    started = time.monotonic()
    killed = None
    with out_path.open("wb") as out, err_path.open("wb") as err:
        process = subprocess.Popen(
            cmd, cwd=str(cwd), stdin=subprocess.DEVNULL,
            stdout=out, stderr=err, env=env,
        )
        while True:
            code = process.poll()
            if code is not None:
                break
            if time.monotonic() - started > timeout:
                killed = f"timed out after {timeout:g}s"
            elif out_path.stat().st_size + err_path.stat().st_size > max_bytes:
                killed = f"produced more than {max_bytes // 1024} kb of output"
            if killed:
                process.kill()
                code = process.wait()
                break
            time.sleep(0.01)
    seconds = time.monotonic() - started

    def read(path: Path) -> str:
        data = path.read_bytes()[: max_bytes + 4096]
        return normalize(data.decode("utf-8", "replace"))

    return Completed(
        code=None if killed else code,
        stdout=read(out_path),
        stderr=read(err_path),
        killed=killed,
        seconds=seconds,
    )


# ---------------------------------------------------------------------------
# Running a case
# ---------------------------------------------------------------------------

PASS, FAIL, SKIP = "pass", "fail", "skip"

# R3.8. An xfail case that fails is XFAIL, which is not a failure; one that
# passes is XPASS, which is. Reporting XFAIL as pass would lose the distinction
# the mark exists to make, and reporting XPASS as pass would let a fix land
# without anyone noticing the mark is now a lie.
XFAIL, XPASS = "xfail", "xpass"


@dataclass
class Result:
    scenario: Scenario
    run: RunSpec
    status: str
    seconds: float
    commands: list[str] = field(default_factory=list)
    problems: list[str] = field(default_factory=list)
    note: str = ""

    @property
    def label(self) -> str:
        if self.run.name == "default":
            return self.scenario.name
        return f"{self.scenario.name}[{self.run.name}]"


def quote(cmd: list[str]) -> str:
    if IS_WINDOWS:
        return subprocess.list2cmdline(cmd)
    import shlex
    return shlex.join(cmd)


def object_extension(options: tuple[str, ...]) -> str:
    if "--wasm" in options:
        return "wasm"
    return "obj" if IS_WINDOWS else "o"


class Runner:
    def __init__(self, args, codes: dict[str, int], linker: Linker):
        self.args = args
        self.codes = codes
        self.by_number = {value: name for name, value in codes.items()}
        self.conec = args.conec
        self.linker = linker
        self.out_root = REPO / "build" / "testrun"

    def run(self, scenario: Scenario, spec: RunSpec) -> Result:
        # A fault in the runner fails its own case rather than the whole run, so
        # one bad case still leaves the other results readable.
        try:
            result = self._run(scenario, spec)
        except Exception:                                  # noqa: BLE001
            import traceback
            result = Result(scenario, spec, FAIL, 0.0,
                            problems=["the runner itself failed:\n"
                                      + indent(traceback.format_exc())])
        return expected_failure(result)

    def _run(self, scenario: Scenario, spec: RunSpec) -> Result:
        started = time.monotonic()

        # R1.4: output filenames derive from the source basename and the IR dump
        # from the program root, so two cases sharing a basename would collide
        # in a shared directory. Each run gets its own.
        out_dir = self.out_root / scenario.group / f"{scenario.name}__{spec.name}"
        out_dir.mkdir(parents=True, exist_ok=True)
        for stale in out_dir.iterdir():
            if stale.is_file():
                stale.unlink()

        if scenario.category == "driver":
            return self.run_driver(scenario, spec, out_dir, started)

        result = Result(scenario, spec, PASS, 0.0)
        options = list(spec.options)
        if scenario.category in ("compile", "run"):
            options += ["--checktree", "--verify"]        # R3.3
        if any(c.target == "llvmir" for c in scenario.checks):
            options.append("--llvmir")

        out_rel = out_dir.relative_to(REPO).as_posix()
        cmd = [str(self.conec), *options, "-o", out_rel, scenario.source_rel]
        result.commands.append(quote(cmd))
        compiled = execute(cmd, REPO, out_dir, "conec",
                           self.args.timeout, self.args.max_output)

        if compiled.killed:
            result.status = FAIL
            result.problems.append(f"compiler {compiled.killed}")
            result.seconds = time.monotonic() - started
            return result

        diagnostics = parse_diagnostics(compiled.stderr)
        self.check_exit(result, scenario, compiled)
        if scenario.category == "reject":
            self.check_rejection(result, scenario, diagnostics)
        elif scenario.category == "warn":
            self.check_warning(result, scenario, diagnostics)
        elif scenario.category == "recover":
            self.check_recovery(result, scenario, diagnostics)
        else:
            self.check_clean(result, scenario, diagnostics, out_dir, spec)

        # LLVM IR checks are asserted before linking, so they still run on a
        # machine with no linker where the run scenarios themselves skip (R3.7).
        if result.status == PASS:
            self.check_artifacts(result, scenario, out_dir, "llvmir")
        if result.status == PASS and scenario.category == "run":
            self.link_and_run(result, scenario, spec, out_dir)
            if result.status == PASS:
                self.check_artifacts(result, scenario, out_dir, "stdout")

        result.seconds = time.monotonic() - started
        return result

    # -- assertions ---------------------------------------------------------

    def check_exit(self, result: Result, scenario: Scenario, compiled: Completed) -> None:
        """R1.2. Matched exactly against the taxonomy in error.h. Accepting
        'nonzero' would score a mistyped fixture path (ExitNF) and a Windows
        access violation as correct rejections."""
        expected = scenario.exit_code
        if compiled.code == expected:
            return
        result.status = FAIL
        actual = EXIT_NAMES.get(compiled.code, "not in the ErrorCode taxonomy")
        result.problems.append(
            f"exit status {compiled.code} ({actual}), expected "
            f"{expected} ({EXIT_NAMES.get(expected, '?')})"
        )
        if compiled.stderr.strip():
            result.problems.append("stderr:\n" + indent(normalize_stderr(compiled.stderr)))

    def check_clean(self, result: Result, scenario: Scenario,
                    diagnostics: list[Diagnostic], out_dir: Path, spec: RunSpec) -> None:
        """R3.2. compile and run require no diagnostics and explicitly zero
        warnings: warnings do not fail a compile on their own, so an unasserted
        warning count is silently ignorable."""
        errors = [d for d in diagnostics if not d.is_warning]
        warnings = [d for d in diagnostics if d.is_warning]
        if errors:
            result.status = FAIL
            result.problems.append("errors, where the category expects none:\n" + indent(
                "\n".join(d.describe(self.by_number) for d in errors)))
        if warnings:
            result.status = FAIL
            result.problems.append("warnings, where the category expects zero:\n" + indent(
                "\n".join(d.describe(self.by_number) for d in warnings)))
        obj = out_dir / f"{scenario.source.stem}.{object_extension(spec.options)}"
        if result.status == PASS and not obj.exists():
            result.status = FAIL
            result.problems.append(f"no object file emitted at {obj.name}")

    def claim_unlocated(self, result: Result, scenario: Scenario,
                        unlocated: list[Diagnostic]) -> list[Diagnostic]:
        """R2.10. Pair the cases.toml ``unlocated`` entries against what
        ``errorMsg`` printed with no source location, and return the leftovers.
        A declared entry that was not produced fails here; what the caller does
        with the leftovers is the category's business."""
        leftover = list(unlocated)
        for want in scenario.unlocated:
            code = self.codes.get(want.get("code"))
            hit = next((d for d in leftover
                        if d.code == code and want.get("message", "") in d.message), None)
            if hit:
                leftover.remove(hit)
            else:
                result.status = FAIL
                result.problems.append(
                    f"expected unlocated {want.get('code')} "
                    f"\"{want.get('message', '')}\", which was not produced")
        return leftover

    def report_missing(self, result: Result, missing: list[Annotation]) -> None:
        """R3.5. A case separates primary diagnostics from the follow-on ones
        that exist only as consequences, so a recovery change that drops a
        consequent reads differently from one that drops the cause."""
        if not missing:
            return
        result.status = FAIL
        primary = [a for a in missing if not a.follow_on]
        consequent = [a for a in missing if a.follow_on]
        if primary:
            result.problems.append("expected primary diagnostics, not produced:\n"
                                   + indent("\n".join(a.describe() for a in primary)))
        if consequent:
            result.problems.append("expected follow-on diagnostics, not produced:\n"
                                   + indent("\n".join(a.describe() for a in consequent)))

    def check_rejection(self, result: Result, scenario: Scenario,
                        diagnostics: list[Diagnostic]) -> None:
        """R3.4/R3.5. Every annotated diagnostic matched by code and location,
        and no unannotated ones."""
        located = [d for d in diagnostics if d.line is not None]
        unlocated = [d for d in diagnostics if d.line is None]
        missing, extra = match_diagnostics(scenario.annotations, located, scenario.source_rel)
        extra += self.claim_unlocated(result, scenario, unlocated)

        self.report_missing(result, missing)
        if extra:
            result.status = FAIL
            result.problems.append("produced, but not annotated:\n" + indent(
                "\n".join(d.describe(self.by_number) for d in extra)))

        if scenario.diagnostics is not None and len(diagnostics) != scenario.diagnostics:
            result.status = FAIL
            result.problems.append(
                f"{len(diagnostics)} diagnostics, but cases.toml declares "
                f"diagnostics = {scenario.diagnostics}")

    def check_warning(self, result: Result, scenario: Scenario,
                      diagnostics: list[Diagnostic]) -> None:
        """The warn row of the category table in design/Test Suite.md section 4:
        exit 0 (check_exit), the annotated warnings present, no unannotated ones,
        and no errors.

        Located warnings use the same //~ mechanism as reject, because a warning
        is the same enum printed in the same format, and the criterion is the
        same too. warn is the only category that permits a warning at all --
        compile and run require explicitly zero (R3.2) -- so a warn scenario is
        the single place in the suite where a newly introduced spurious warning
        could sit unnoticed. Tolerating unannotated ones would leave that hole
        covered only by an author remembering to write a `diagnostics` count,
        which is exactly the silently-ignorable failure R3.2 exists to close.
        """
        errors = [d for d in diagnostics if not d.is_warning]
        if errors:
            result.status = FAIL
            result.problems.append("errors, where the category expects none:\n" + indent(
                "\n".join(d.describe(self.by_number) for d in errors)))

        warnings = [d for d in diagnostics if d.is_warning]
        located = [d for d in warnings if d.line is not None]
        missing, extra = match_diagnostics(
            scenario.annotations, located, scenario.source_rel)
        extra += self.claim_unlocated(
            result, scenario, [d for d in warnings if d.line is None])
        self.report_missing(result, missing)
        if extra:
            result.status = FAIL
            result.problems.append("warnings produced, but not annotated:\n" + indent(
                "\n".join(d.describe(self.by_number) for d in extra)))

        if scenario.diagnostics is not None and len(diagnostics) != scenario.diagnostics:
            result.status = FAIL
            result.problems.append(
                f"{len(diagnostics)} diagnostics, but cases.toml declares "
                f"diagnostics = {scenario.diagnostics}")

    def check_recovery(self, result: Result, scenario: Scenario,
                       diagnostics: list[Diagnostic]) -> None:
        """The recover row of the category table: exit exactly 1, the expected
        diagnostic count, no crash and no hang.

        The point of recover is that the compiler kept going and produced a sane
        count, so it does not require every diagnostic to be annotated — a
        scenario whose diagnostics are recovery artifacts would otherwise have to
        pin positions that recovery is free to move. The other two assertions are
        already made elsewhere and are not repeated here: a crash is an exit
        status outside the taxonomy, which check_exit catches exactly (R1.2), and
        a hang is the wall-clock timeout in execute (R1.3).
        """
        if len(diagnostics) != scenario.diagnostics:
            result.status = FAIL
            result.problems.append(
                f"{len(diagnostics)} diagnostics, but cases.toml declares "
                f"diagnostics = {scenario.diagnostics}\n"
                + indent("\n".join(d.describe(self.by_number) for d in diagnostics)))

    def run_driver(self, scenario: Scenario, spec: RunSpec,
                   out_dir: Path, started: float) -> Result:
        """The driver row of the category table: invoke conec without valid Cone
        source and assert the exact exit code.

        argv is the whole invocation. Nothing is appended — no -o, no source —
        because what is under test is what conec does with the command line it
        was given, and an argument the runner added would be part of the answer.
        """
        result = Result(scenario, spec, PASS, 0.0)
        cmd = [str(self.conec), *scenario.argv]
        result.commands.append(quote(cmd))
        invoked = execute(cmd, REPO, out_dir, "conec",
                          self.args.timeout, self.args.max_output)
        if invoked.killed:
            result.status = FAIL
            result.problems.append(f"compiler {invoked.killed}")
        else:
            self.check_exit(result, scenario, invoked)
            # coneOptSet reports bad options and prints usage on stdout, not
            # stderr, so a driver failure is invisible without this. Usage runs
            # to sixty lines and says nothing about why the case failed, so only
            # the head of it is shown.
            if result.status == FAIL and invoked.stdout.strip():
                result.problems.append("stdout:\n" + indent(head(invoked.stdout, 10)))
        result.seconds = time.monotonic() - started
        return result

    def link_and_run(self, result: Result, scenario: Scenario,
                     spec: RunSpec, out_dir: Path) -> None:
        """R3.7. Compiles, links against conestd, executes, compares stdout."""
        reason = self.linker.prepare()
        if reason:
            result.status = SKIP
            result.note = f"not linked: {reason}"
            return
        stem = scenario.source.stem
        obj = out_dir / f"{stem}.{object_extension(spec.options)}"
        exe = out_dir / (f"{stem}.exe" if IS_WINDOWS else stem)
        link_cmd = self.linker.command(obj, exe)
        result.commands.append(quote(link_cmd))
        linked = execute(link_cmd, REPO, out_dir, "link",
                         self.args.timeout, self.args.max_output, env=self.linker.env)
        if linked.code != 0:
            result.status = FAIL
            result.problems.append(
                f"link failed with status {linked.code}\n"
                + indent(linked.stdout + linked.stderr))
            return

        result.commands.append(quote([str(exe)]))
        ran = execute([str(exe)], REPO, out_dir, "program",
                      self.args.timeout, self.args.max_output, env=self.linker.env)
        if ran.killed:
            result.status = FAIL
            result.problems.append(f"program {ran.killed}")
            return
        if ran.code != 0:
            result.status = FAIL
            result.problems.append(f"program exited {ran.code}, expected 0")
            return

        expected_path = scenario.source.with_suffix(".out")
        if not expected_path.exists():
            result.status = FAIL
            result.problems.append(f"no expected stdout at {expected_path.name}")
            return
        expected = normalize(expected_path.read_text(encoding="utf-8"))
        if trimmed(ran.stdout) != trimmed(expected):
            result.status = FAIL
            result.problems.append("stdout does not match "
                                   + expected_path.name + ":\n" + indent(delta(
                                       trimmed(expected), trimmed(ran.stdout),
                                       expected_path.name, "actual")))
        result.program_stdout = ran.stdout  # type: ignore[attr-defined]

    def check_artifacts(self, result: Result, scenario: Scenario,
                        out_dir: Path, target: str) -> None:
        """R2.3. Named checks against a generated artifact — LLVM IR, or a run's
        stdout — for what has no source line to attach to."""
        for check in scenario.checks:
            if check.target != target:
                continue
            if check.target == "llvmir":
                # genllvm writes <srcname>.ir after optimization and .preir
                # before it. The post-optimization dump is what reaches the
                # object file, so that is what a symbol assertion is about.
                artifact = out_dir / f"{scenario.source.stem}.ir"
                if not artifact.exists():
                    result.status = FAIL
                    result.problems.append(
                        f"check {check.name!r}: no LLVM IR dump at {artifact.name}")
                    continue
                text = normalize(artifact.read_text(encoding="utf-8", errors="replace"))
            else:
                text = getattr(result, "program_stdout", "")
            for needle in check.contains:
                if needle not in text:
                    result.status = FAIL
                    result.problems.append(
                        f"check {check.name!r} ({check.target}): expected to contain "
                        f"{needle!r}")
            for needle in check.excludes:
                if needle in text:
                    result.status = FAIL
                    result.problems.append(
                        f"check {check.name!r} ({check.target}): expected not to contain "
                        f"{needle!r}")


def trimmed(text: str) -> list[str]:
    lines = text.split("\n")
    while lines and not lines[-1].strip():
        lines.pop()
    return lines


def head(text: str, limit: int) -> str:
    lines = trimmed(text)
    if len(lines) <= limit:
        return "\n".join(lines)
    return "\n".join(lines[:limit] + [f"... {len(lines) - limit} more lines"])


def indent(text: str, prefix: str = "    ") -> str:
    return "\n".join(prefix + line for line in text.rstrip().split("\n"))


def delta(expected: list[str], actual: list[str], left: str, right: str) -> str:
    return "\n".join(difflib.unified_diff(
        expected, actual, fromfile=left, tofile=right, lineterm="", n=2))


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------

MARK = {PASS: "pass", FAIL: "FAIL", SKIP: "skip", XFAIL: "xfail", XPASS: "XPASS"}


def expected_failure(result: Result) -> Result:
    """R3.8. Translate a result under an ``xfail`` mark.

    A failure becomes XFAIL and stops being a failure; a pass becomes XPASS and
    starts being one, which is the half of the requirement that makes the mark
    honest — a defect fixed elsewhere has to be noticed here rather than leaving
    a case asserting a bug that no longer exists. A skip stays a skip: nothing
    ran, so there is nothing to have expected.
    """
    if not result.scenario.xfail or result.status == SKIP:
        return result
    if result.status == FAIL:
        result.status = XFAIL
        first = result.problems[0].split("\n")[0] if result.problems else ""
        result.note = f"expected failure: {first}" if first else "expected failure"
        return result
    result.status = XPASS
    result.problems = [
        "marked xfail, but the case passed. If the defect it recorded is fixed,"
        " drop the xfail; the mark must never outlive what it describes (R3.8)."
    ]
    return result


def report(results: list[Result], scenarios: list[Scenario]) -> int:
    """R2.8. Tier order, tier 0 first."""
    results.sort(key=lambda r: (r.scenario.sort_key, r.run.name))
    tier = group = None
    for result in results:
        if result.scenario.tier != tier:
            tier = result.scenario.tier
            group = None
            print(f"\ntier {tier}")
        if result.scenario.group != group:
            group = result.scenario.group
            print(f"  {group}")
        note = f"  ({result.note})" if result.note else ""
        print(f"    {MARK[result.status]:<5}  {result.label:<28} "
              f"{result.scenario.category:<8} {result.seconds * 1000:5.0f}ms{note}")

    # An XPASS is a failure of the suite's own bookkeeping rather than of the
    # compiler, but it fails the run and is reported the same way (R3.8).
    failures = [r for r in results if r.status in (FAIL, XPASS)]
    if failures:
        print("\n" + "=" * 72)
        print("FAILURES")
        for result in failures:
            print("=" * 72)
            print(f"{result.label}  ({result.scenario.category})"
                  f"  {result.scenario.description}")
            if result.scenario.source_rel:
                print(f"  source: {result.scenario.source_rel}")
            for command in result.commands:           # R4.3: the exact command line
                print(f"  command: {command}")
            for problem in result.problems:
                print(indent(problem, "  "))
            print()

    tally = {status: sum(1 for r in results if r.status == status) for status in MARK}
    print("=" * 72)
    summary = (f"{len(scenarios)} scenarios, {len(results)} runs: "
               f"{tally[PASS]} passed")
    if tally[XFAIL]:
        summary += f", {tally[XFAIL]} expected failures"
    if tally[SKIP]:
        summary += f", {tally[SKIP]} skipped"
    if tally[FAIL]:
        summary += f", {tally[FAIL]} FAILED"
    if tally[XPASS]:
        summary += f", {tally[XPASS]} UNEXPECTEDLY PASSED"
    print(summary)
    return 1 if failures else 0


def list_cases(scenarios: list[Scenario]) -> None:
    """R2.6."""
    tier = group = None
    for scenario in scenarios:
        if scenario.tier != tier:
            tier = scenario.tier
            group = None
            print(f"\ntier {tier}")
        if scenario.group != group:
            group = scenario.group
            print(f"  {group}")
        detail = scenario.category
        if scenario.xfail:
            detail += " xfail"
        if scenario.tags:
            detail += "  tags: " + ",".join(scenario.tags)
        if scenario.category in ANNOTATABLE:
            detail += f"  diagnostics: {len(scenario.annotations)}"
        elif scenario.category == "recover":
            detail += f"  diagnostics: {scenario.diagnostics}"
        print(f"    {scenario.name:<28} {detail}")
        if scenario.category == "driver":
            print(f"      argv: {' '.join(scenario.argv) or '(none)'}"
                  f"  exit {scenario.exit_code}")
        for spec in scenario.runs:
            if spec.name != "default":
                print(f"      run {spec.name}: {' '.join(spec.options)}")
        for check in scenario.checks:
            print(f"      check {check.name} ({check.target})")
    print(f"\n{len(scenarios)} scenarios, "
          f"{sum(len(s.runs) for s in scenarios)} runs")


# ---------------------------------------------------------------------------
# ErrorCode coverage (R6.4)
# ---------------------------------------------------------------------------

# Excluded from the coverage denominator, each for its own reason.
#
# The Exit* codes are not diagnostics: they are process exit statuses, asserted
# by every scenario's category (R1.2) and provoked deliberately by the driver
# group. Counting them as uncovered diagnostics would ask for reject scenarios
# that cannot exist.
BLOCK_MARKERS = ("ErrorCode", "WarnCode", "Uncounted")
# R6.6: WarnIndent goes dead when the language becomes free-form only, and the
# whole corpus is written in braces so that it can. Deliberately not chased.
RETIRED_CODES = ("WarnIndent",)


def diagnostic_codes(codes: dict[str, int]) -> dict[str, int]:
    """The Error*/Warn* codes a reject or warn scenario could name."""
    return {
        name: value for name, value in codes.items()
        if (name.startswith("Error") or name.startswith("Warn"))
        and name not in BLOCK_MARKERS and name not in RETIRED_CODES
    }


def coverage(scenarios: list[Scenario]) -> dict[str, list[str]]:
    """Which scenarios name each code, by //~ annotation or unlocated entry."""
    named: dict[str, list[str]] = {}
    for scenario in scenarios:
        for annotation in scenario.annotations:
            named.setdefault(annotation.code_name, []).append(scenario.name)
        for entry in scenario.unlocated:
            code = entry.get("code")
            if code:
                named.setdefault(code, []).append(scenario.name)
    return {name: sorted(set(where)) for name, where in named.items()}


def coverage_report(scenarios: list[Scenario], codes: dict[str, int]) -> None:
    """R6.4. Which ErrorCode values have no case provoking them.

    The enum is a closed list, which is what makes this cheap and honest: the
    denominator is every diagnostic code the compiler can emit, not a guess. The
    uncovered list is the actionable half — it is what decides which scenario
    gets written next — so it is printed in full, in declaration order, with the
    number beside the name so a code can be found in error.h without a search.
    """
    interesting = diagnostic_codes(codes)
    named = coverage(scenarios)
    covered = {n: named[n] for n in interesting if n in named}
    uncovered = [n for n in interesting if n not in named]

    # A name in a scenario that is not a diagnostic code at all cannot happen:
    # parse_annotations rejects it. An excluded one can, and is worth saying.
    excluded_but_named = sorted(
        n for n in named if n in codes and n not in interesting)

    print("ErrorCode coverage (R6.4)")
    print(f"  from {ERROR_H.relative_to(REPO).as_posix()}, over all "
          f"{len(scenarios)} scenarios")
    print(f"  {len(covered)} of {len(interesting)} diagnostic codes covered, "
          f"{len(uncovered)} not")

    for kind, prefix in (("errors", "Error"), ("warnings", "Warn")):
        missing = [n for n in uncovered if n.startswith(prefix)]
        if not missing:
            continue
        print(f"\nuncovered {kind} ({len(missing)})")
        for name in missing:
            print(f"    {name:<24} {interesting[name]}")

    if covered:
        print(f"\ncovered ({len(covered)})")
        for name, where in covered.items():
            print(f"    {name:<24} {interesting[name]:<6} {', '.join(where)}")

    exits = sum(1 for n in codes if n.startswith("Exit"))
    print("\nexcluded from the denominator")
    for label, why in (
        (f"Exit* ({exits})", "process exit statuses, not diagnostics;"
                             " the driver group asserts them"),
        (", ".join(BLOCK_MARKERS), "block markers, never emitted"),
        (", ".join(RETIRED_CODES), "dead once the language is free-form"
                                   " only (R6.6)"),
    ):
        print(f"    {label:<32}{why}")
    if excluded_but_named:
        print(f"    named anyway by a scenario: {', '.join(excluded_but_named)}")


# ---------------------------------------------------------------------------

def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Run the Cone compiler test suite.",
        epilog="A selector is a group, a scenario, a check name, or tag:<phase>.")
    parser.add_argument("selectors", nargs="*",
                        help="what to run; everything, if omitted (R1.7)")
    parser.add_argument("--list", action="store_true",
                        help="print what would run, and run nothing (R2.6)")
    parser.add_argument("--coverage", action="store_true",
                        help="report ErrorCode values with no case, run nothing (R6.4)")
    parser.add_argument("--bless-codes", action="store_true",
                        help=f"regenerate {CODES_TOML.relative_to(REPO).as_posix()}"
                             f" from error.h, and review the diff (R5.2)")
    parser.add_argument("--build", action="store_true",
                        help="build the compiler before running (R1.1)")
    parser.add_argument("--allow-stale", action="store_true",
                        help="warn instead of refusing when the binary predates a source")
    parser.add_argument("--conec", type=Path, default=None,
                        help=f"compiler binary (default {default_conec().relative_to(REPO)})")
    parser.add_argument("--conestd", type=Path, default=None,
                        help="conestd library to link run scenarios against")
    parser.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 4,
                        help="cases to run at once (R1.5)")
    parser.add_argument("--timeout", type=float, default=20.0,
                        help="wall-clock seconds per process (R1.3)")
    parser.add_argument("--max-output", type=int, default=8 * 1024 * 1024,
                        help="bytes of output before a process is killed")
    args = parser.parse_args(argv)

    args.conec = (args.conec or default_conec()).resolve()
    if args.conestd is None:
        args.conestd = args.conec.parent / ("conestd.lib" if IS_WINDOWS else "libconestd.a")

    try:
        codes = parse_error_codes(ERROR_H)
        if args.bless_codes:
            write_codes_table(CODES_TOML, codes)
            print(f"wrote {CODES_TOML.relative_to(REPO).as_posix()}:"
                  f" {len(codes)} codes. Review the diff.")
            return 0
        # R5.2, before any case runs: every expectation in the corpus is matched
        # on the number the compiler printed, so a renumber makes all of them
        # wrong at once and this is the one place that can say so.
        check_codes_table(codes, CODES_TOML)
        discovered = discover(codes)
        scenarios = select(discovered, args.selectors)
    except SuiteError as failure:
        print(f"error: {failure}", file=sys.stderr)
        return 2

    # Coverage is a property of the whole corpus, so it ignores the selection
    # rather than reporting a backlog against a fraction of it.
    if args.coverage:
        coverage_report(discovered, codes)
        return 0

    if not scenarios:
        print("error: nothing to run", file=sys.stderr)
        return 2

    if args.list:
        list_cases(scenarios)
        return 0

    try:
        if args.build:
            build_compiler(args.conec)
        check_not_stale(args.conec, args.allow_stale)
    except SuiteError as failure:
        print(f"error: {failure}", file=sys.stderr)
        return 2

    runner = Runner(args, codes, Linker(args.conestd))
    work = [(s, spec) for s in scenarios for spec in s.runs]
    started = time.monotonic()
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        results = list(pool.map(lambda item: runner.run(*item), work))
    status = report(results, scenarios)
    print(f"finished in {time.monotonic() - started:.1f}s")
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
