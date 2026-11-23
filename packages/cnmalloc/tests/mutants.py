#!/usr/bin/env python3
"""mutants.py: Linux's mutation check of the second design, the counterpart of mutants.ps1 (row 550).

Each mutant is shim/cn_shim.c with one safety rule broken; the test meant to catch it must FAIL on it, and the same tests must
PASS on the unmutated shim. The mutants, each test's CNMALLOC setting and the variants live in mutants.json beside this file, the
one copy that mutants.ps1 reads too (what each mutant breaks is described in mutants.ps1's header).

    python3 packages/cnmalloc/tests/mutants.py [--name a|a2m|...|h1|none|all] [--runs 5] [--scratch DIR] [--race-secs 120]

Everything is built and run under the scratch folder (default /tmp/cnmalloc-mutants); nothing in the tree changes except the
package's own build folder. Needs cc and the package's release build (congo build --release, done here).
"""
import argparse, json, os, shutil, subprocess, sys
from pathlib import Path

here = Path(__file__).resolve().parent
pkg = here.parent
tree = pkg.parent.parent
congo = tree / "tools" / "congo" / "congo.py"
data = json.loads((here / "mutants.json").read_text())

CFLAGS = ["-O2", "-fPIC", "-ftls-model=initial-exec", "-fno-builtin-malloc", "-fno-builtin-calloc",
          "-fno-builtin-realloc", "-fno-builtin-free", "-pthread", "-DCN_HOOKS", "-DCN_DEBUG"]


def build_shim(src_dir: Path, out: Path) -> Path:
    """The second design's test build (cn_shim_d.o) of <src_dir>/cn_shim.c."""
    obj = out / "cn_shim_d.o"
    subprocess.run(["cc", *CFLAGS, "-c", str(src_dir / "cn_shim.c"), "-o", str(obj)], check=True)
    return obj


def build_mutant(name: str, scratch: Path) -> Path:
    out = scratch / f"mut-{name}"
    out.mkdir(parents=True, exist_ok=True)
    text = (pkg / "shim" / "cn_shim.c").read_text().replace("\r\n", "\n")
    for old, new in data["mutants"][name]["edits"]:
        count = text.count(old)
        if count != 1:
            raise SystemExit(f"mutation anchor for {name} found {count} times: {old}")
        text = text.replace(old, new)
    (out / "cn_shim.c").write_text(text)
    shutil.copy(pkg / "shim" / "cn_plat.h", out)
    archive = None
    cone_edits = data["mutants"][name].get("cone")
    if cone_edits:                      # (row 580) a mutant of cnmalloc.cone: its own package copy, built, archived
        cp = out / "pkg"
        shutil.rmtree(cp, ignore_errors=True)
        (cp / "src").mkdir(parents=True)
        shutil.copy(pkg / "congo.toml", cp)
        for f in (pkg / "src").iterdir():
            shutil.copy(f, cp / "src")
        ctext = (cp / "src" / "cnmalloc.cone").read_text().replace("\r\n", "\n")
        for old, new in cone_edits:
            count = ctext.count(old)
            if count != 1:
                raise SystemExit(f"mutation anchor for {name} found {count} times in cnmalloc.cone: {old}")
            ctext = ctext.replace(old, new)
        (cp / "src" / "cnmalloc.cone").write_text(ctext)
        subprocess.run([sys.executable, str(congo), "build", "--release"], cwd=cp, check=True)
        archive = out / "libcnpkg.a"
        archive.unlink(missing_ok=True)
        subprocess.run(["ar", "rcs", str(archive), str(cp / "build" / "release" / "cnmalloc.o")], check=True)
    return build_shim(out, out), archive


def run_test(shim_obj: Path, archive: Path, test: str, tag: str, extra: dict, scratch: Path) -> bool:
    """One test on the second design linked from shim_obj (+ the package archive), in its own copy of the package."""
    run = scratch / f"run-{test}-{tag}"
    if run.exists():
        shutil.rmtree(run)
    (run / "src").mkdir(parents=True)
    (run / "tests").mkdir()
    shutil.copy(pkg / "congo.toml", run)
    srcdir = archive.parent / "pkg" / "src"      # (row 580) a Cone-side mutant's own source: the test compiles it in too
    for f in (srcdir if srcdir.is_dir() else pkg / "src").iterdir():
        shutil.copy(f, run / "src")
    for f in (pkg / "tests").glob(f"{test}.*"):
        shutil.copy(f, run / "tests")
    log = scratch / f"log-{test}-{tag}.txt"
    env = dict(os.environ)
    env["CNMALLOC"] = data["cfg"][test]
    env["CONGO_LINK_EXTRA"] = f"{shim_obj} {archive}"
    env["CNTEST_LOG"] = str(scratch / f"numbers-{test}-{tag}.txt")
    env.update(extra)
    with open(log, "w") as lf:
        r = subprocess.run([sys.executable, str(congo), "test", "--no-cache", test], cwd=run, env=env,
                           stdout=lf, stderr=subprocess.STDOUT)
    txt = log.read_text(errors="replace")
    if "could not compile" in txt:
        raise SystemExit(f"test {test} did not compile; see {log}")
    return r.returncode == 0 and f"test {test} ... ok" in txt


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", default="all")
    ap.add_argument("--runs", type=int, default=1)
    ap.add_argument("--scratch", default="/tmp/cnmalloc-mutants")
    ap.add_argument("--race-secs", type=int, default=120)
    a = ap.parse_args()
    scratch = Path(a.scratch)
    scratch.mkdir(parents=True, exist_ok=True)

    # the unmutated build, and the package's release object as an archive
    subprocess.run([sys.executable, str(congo), "build", "--release"], cwd=pkg, check=True)
    archive = scratch / "libcnpkg.a"
    archive.unlink(missing_ok=True)
    subprocess.run(["ar", "rcs", str(archive), str(pkg / "build" / "release" / "cnmalloc.o")], check=True)
    plain = scratch / "plain"
    plain.mkdir(exist_ok=True)
    unmutated = build_shim(pkg / "shim", plain)
    (plain / "cn_plat.h").unlink(missing_ok=True)

    verdicts = []

    def check(what: str, ok: bool):
        verdicts.append(ok)
        print(f"{what:<60} {'ok' if ok else 'WRONG'}", flush=True)

    names = data["order"] + ["none"] if a.name == "all" else a.name.split(",")
    for m in names:
        if m == "none":
            for t in data["unmutated"]:
                n = a.runs if t in data["deterministic"] else 1
                for v, extra in enumerate(data["variants"][t], 1):
                    for i in range(1, n + 1):
                        check(f"unmutated: {t} variant {v} passes (run {i})",
                              run_test(unmutated, archive, t, f"none{v}-{i}", extra, scratch))
        else:
            obj, mut_archive = build_mutant(m, scratch)
            mut = data["mutants"][m]
            extra = dict(mut["env"])
            if mut["test"] == "lfrace":
                extra["CNRACE_MAXSECS"] = str(a.race_secs)
            for i in range(1, a.runs + 1):
                check(f"mutant {m}: {mut['test']} FAILS (run {i})",
                      not run_test(obj, mut_archive or archive, mut["test"], f"{m}{i}", extra, scratch))
    print(f"{sum(verdicts)} of {len(verdicts)} checks as expected; logs and numbers in {scratch}")
    return 0 if all(verdicts) else 1


if __name__ == "__main__":
    sys.exit(main())
