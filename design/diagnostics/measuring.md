How to find out what this compiler actually does, rather than what it looks like
it does. Every claim in these notes marked *measured* was produced this way.

Use this when a design note says something you cannot confirm by reading, when
you need to know whether a code path runs at all, or when a change's effect is
not visible in the diagnostics.

## The technique

**Instrument the thing under test to report instead of act, compile every
corpus source, and read what comes out.**

```bash
for f in $(find test/cases -name "*.cone"); do
  ./build/x64-release/conec.exe -o build/probe "$f" 2>&1 | grep "^PROBE"
done
```

**Silence is the result you want when the claim is "nothing does this".** That
is the shape most worth measuring, because reading cannot establish a negative.
The kinds of thing it catches: a clone function carrying analysis marks into its
copy, a walk leaking a scope counter on one of its exits, a dispatch arm that
fires nowhere in the corpus at all, and a release that never happens.

For a question about *order* — which declaration is analyzed when, and from
where — print declaration-level entry to `inodeNameRes` and `inodeTypeCheck`,
including its early return when the work is already done, plus the `blockFlow`
call in `fnDclTypeCheck`.

For "is this arm reachable?", put a `fprintf(stderr, ...)` in it, rebuild, and
compile against it — the suite first, then sources written to try to get there.
**Never an `assert`**: `NDEBUG` compiles it out of the Release build, which is
the only build the runner uses. This is how the audit of the twenty-two
"unreachable" sites found the three a source actually reaches; reading the code
had not found them.

## Reading what the compiler produced

| Flag | Writes | Use it for |
| --- | --- | --- |
| `--ir` | a Cone IR/AST dump, written to `<srcname>.ast` — `inodePrint` names it after the source compiled, not after the corelib pseudo-source the program node's own lexer points at | what a phase built or lowered a construct into |
| `--llvmir` | **two** files, `<name>.preir` before optimization and `<name>.ir` after | what generation emitted. Read `.preir` — `.ir` has been through mem2reg and GVN and no longer resembles the emission |
| `--checktree` | nothing, unless it finds a hole | an expression node with no `vtype`, or a block with no statements. `test/run.py` passes it on every compile |
| `--verify` | LLVM's own module verification | malformed IR — a phi with the wrong predecessors, a truncation of a pointer |
| `--asm` | `.asm`/`.s`, or `.wat` under `--wasm` | the final instruction selection |

**`--verify` is off by default**, so malformed IR is written out silently unless
you ask. No corpus scenario fails it today, but the corpus is the only thing it
has been run over. Turn it on whenever a change touches basic blocks or phis.

The output directory must already exist and each run writes several files, so
use a git-ignored directory such as `build/probe/`.

Counting adjustments in `.preir` is how ownership questions get settled: an
allocation that emits `call i8* @malloc` and no matching
`getelementptr i64, i64* %n, i64 -1` leaks.

## Running the suite

```powershell
python test/run.py
```

`--list` prints what would run; a group, scenario, check name or `tag:<phase>`
narrows it. `--build` builds first. [Test Suite](test-suite.md) is the authoring
guide.

**A stale `conec` fails good sources in ways indistinguishable from a language
regression.** A binary left over from an earlier session once failed the
smoke-test input with 17 errors purely because it predated a merge. The runner
refuses to run against a binary older than any compiler source; outside the
runner, build before believing any failure.

One caveat: **in a fresh git worktree every file's mtime is the checkout time**,
so the staleness guard fires against a binary that is in fact current. Either
build inside the worktree, or verify individual sources by compiling them
directly.

## Provenance

Each design note states near the top whether its claims were measured or read.
That is not a formality. A note that is merely plausible is worse than no note,
because it gets trusted. During the analysis re-factor, every confident reading
of this compiler that was not measured turned out wrong at least once.

Read carefully, measure where you are uncertain, and say which you did. A claim
a reader would act on destructively gets measured.
