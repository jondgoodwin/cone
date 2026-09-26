"""Generate the Cone sources flowbench times flow analysis on.

Each source is built from one "group" of statements: a local struct, a mutable
borrow of it held across a loop whose body branches on the borrow, then a write
and a read of the source after the borrow's last use. That is the shape borrow
freezing has to analyse, repeated until flow's cost is measurable.

    python genstress.py many  500   500 functions of 4 groups each
    python genstress.py plain 500   the same with no borrow held (never gated)
    python genstress.py big   1000  one function of 1,000 groups
    python genstress.py nest  3     500 functions of 4 groups, loops nested 3 deep
    python genstress.py flat  3     the same statements, the loops one after another

The file is written to the current directory as stress_<mode>_<n>.cone, or to
--out. flowbench.py imports this module and calls generate() directly.
"""

import argparse
import pathlib

HEADER = ["mod stress;", "", "struct Pt { pub x i64; pub y i64; }", ""]
FUNCTIONS = 500   # functions in a 'many', 'plain', 'nest' or 'flat' file
GROUPS = 4        # groups per function in those files


def group(i: int) -> list[str]:
    """A borrow held across a loop, then its source used after its last use."""
    return [
        f"  mut p{i} = Pt[{i}, {i}];",
        f"  imm r{i} = &mut p{i};",
        f"  r{i}.x = r{i}.x + 1i64;",
        f"  mut j{i} = 0i64;",
        f"  while j{i} < 3i64 {{",
        f"    if j{i} == 1i64 {{ r{i}.y = j{i}; }} else {{ total = total + r{i}.x; }}",
        f"    j{i} = j{i} + 1i64;",
        f"  }}",
        f"  p{i}.y = 7i64;",
        f"  total = total + p{i}.x;",
    ]


def plain(i: int) -> list[str]:
    """The same work with no borrow held: what an ungated function looks like."""
    return [
        f"  mut p{i} = Pt[{i}, {i}];",
        f"  p{i}.x = p{i}.x + 1i64;",
        f"  mut j{i} = 0i64;",
        f"  while j{i} < 3i64 {{",
        f"    if j{i} == 1i64 {{ p{i}.y = j{i}; }} else {{ total = total + p{i}.x; }}",
        f"    j{i} = j{i} + 1i64;",
        f"  }}",
        f"  total = total + p{i}.x;",
    ]


def looped(i: int, depth: int, nested: bool) -> list[str]:
    """A borrow live around every back edge of 'depth' loops.

    Every loop body uses the borrow; the innermost (or last) one also branches
    on it. Nested, each loop sits in the one before it; flat, the same
    statements follow one another, so the pair isolates what nesting costs.
    """
    out = [
        f"  mut p{i} = Pt[{i}, {i}];",
        f"  imm r{i} = &mut p{i};",
        f"  r{i}.x = r{i}.x + 1i64;",
    ]
    closers = []
    for level in range(depth):
        pad = "  " * (level + 1 if nested else 1)
        j = f"j{i}_{level}"
        body = [f"{pad}  total = total + r{i}.x;"]
        if level == depth - 1:
            body.append(f"{pad}  if {j} == 1i64 {{ r{i}.y = {j}; }} else {{ total = total + r{i}.x; }}")
        out.append(f"{pad}mut {j} = 0i64;")
        out.append(f"{pad}while {j} < 3i64 {{")
        out += body
        close = [f"{pad}  {j} = {j} + 1i64;", f"{pad}}}"]
        if nested:
            closers.insert(0, close)
        else:
            out += close
    for close in closers:
        out += close
    out += [
        f"  p{i}.y = 7i64;",
        f"  total = total + p{i}.x;",
    ]
    return out


def generate(mode: str, n: int) -> str:
    """The text of stress_<mode>_<n>.cone."""
    out = list(HEADER)
    if mode == "big":
        out.append("fn big() i64 {")
        out.append("  mut total = 0i64;")
        for g in range(n):
            out += group(g)
        out.append("  total;")
        out.append("}")
    elif mode in ("many", "plain", "nest", "flat"):
        for f in range(FUNCTIONS if mode in ("nest", "flat") else n):
            out.append(f"fn fn_{f}() i64 {{")
            out.append("  mut total = 0i64;")
            for g in range(GROUPS):
                if mode == "many":
                    out += group(g)
                elif mode == "plain":
                    out += plain(g)
                else:
                    out += looped(g, n, mode == "nest")
            out.append("  total;")
            out.append("}")
            out.append("")
    else:
        raise ValueError(f"unknown stress mode {mode!r}")
    out.append("fn main() {}")
    return "\n".join(out) + "\n"


def write(mode: str, n: int, outdir: pathlib.Path) -> pathlib.Path:
    """Write stress_<mode>_<n>.cone into outdir, unless it already holds that text."""
    path = outdir / f"stress_{mode}_{n}.cone"
    text = generate(mode, n)
    if not path.exists() or path.read_text() != text:
        path.write_text(text)
    return path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("mode", choices=["many", "plain", "big", "nest", "flat"])
    parser.add_argument("n", type=int)
    parser.add_argument("--out", type=pathlib.Path, default=pathlib.Path("."))
    args = parser.parse_args()
    print(write(args.mode, args.n, args.out))


if __name__ == "__main__":
    main()
