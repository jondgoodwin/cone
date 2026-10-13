#!/usr/bin/env python3
"""Write the registry index entries for the Cone repository's own packages.

    python tools/registry/make_index.py --out <checkout of the index> [--commit REV]
                                        [--git-url URL] [--push]

The index is a git repository of one TOML file per package (tools/congo/README.md,
"Registries"): name, the git URL of the repository holding the source, the one
commit fetched, and the package's folder in that repository. This script reads
packages/*/congo.toml as they are AT ONE COMMIT of the Cone repository (--commit,
default origin/master, resolved to a full hash), and writes an entry for each into
--out, in the text 'congo publish' writes. It can be run again at any later commit:
it rewrites every entry (the same text where nothing moved), and removes an entry
whose git URL is this repository's and whose package is no longer there. Entries
for packages from other repositories are left alone.

The commit must be on a remote branch, since a consumer fetches it from the URL.
Without --push the script only writes the files; with it, it also commits and
pushes them in --out (the credentials are git's own). Python 3.11 or later,
standard library only.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tomllib
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
sys.path.insert(0, str(REPO / "tools" / "congo"))
import congo  # noqa: E402


def git(*args: str, cwd: Path) -> str:
    run = subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True,
                         encoding="utf-8", errors="replace", stdin=subprocess.DEVNULL)
    if run.returncode != 0:
        sys.exit(f"make_index: git {' '.join(args)}: {run.stderr.strip() or run.stdout.strip()}")
    return run.stdout.strip()


def packages_at(repo: Path, commit: str) -> dict[str, str]:
    """name -> folder, for each packages/<dir>/congo.toml in the commit's tree."""
    found: dict[str, str] = {}
    listing = git("ls-tree", "-r", "--name-only", commit, "--", "packages", cwd=repo)
    for line in listing.splitlines():
        m = re.fullmatch(r"packages/([^/]+)/congo\.toml", line)
        if not m:
            continue
        manifest = tomllib.loads(git("show", f"{commit}:{line}", cwd=repo))
        name = manifest.get("package", {}).get("name")
        if not isinstance(name, str) or congo.name_fault(name):
            sys.exit(f"make_index: {line} at {commit[:10]}: the manifest names no usable package")
        if name in found:
            sys.exit(f"make_index: two packages are named {name}: {found[name]} and packages/{m.group(1)}")
        found[name] = f"packages/{m.group(1)}"
    return found


def main() -> int:
    ap = argparse.ArgumentParser(description="Write the index entries for the Cone repository's packages.")
    ap.add_argument("--out", required=True, type=Path, help="a checkout of the index repository")
    ap.add_argument("--repo", type=Path, default=REPO, help="the Cone repository (default: this one)")
    ap.add_argument("--commit", default="origin/master", help="the commit to pin (default: origin/master)")
    ap.add_argument("--git-url", help="the URL the entries record (default: the repository's origin)")
    ap.add_argument("--push", action="store_true", help="commit and push the entries in --out")
    args = ap.parse_args()

    repo = args.repo.resolve()
    commit = git("rev-parse", "--verify", f"{args.commit}^{{commit}}", cwd=repo)
    if not git("branch", "--remotes", "--contains", commit, cwd=repo):
        sys.exit(f"make_index: commit {commit[:10]} is on no remote branch, so nobody could fetch it")
    url = args.git_url or git("remote", "get-url", "origin", cwd=repo)
    out = args.out.resolve()
    if not out.is_dir():
        sys.exit(f"make_index: {out} is not a folder")

    packages = packages_at(repo, commit)
    for name, folder in sorted(packages.items()):
        (out / f"{name}.toml").write_bytes(congo.index_entry_text(name, url, commit, folder).encode("utf-8"))
    removed = []
    for file in sorted(out.glob("*.toml")):
        if file.stem in packages:
            continue
        try:
            entry = tomllib.loads(file.read_text(encoding="utf-8"))
        except (OSError, tomllib.TOMLDecodeError):
            continue
        if entry.get("git") == url:
            file.unlink()
            removed.append(file.stem)
    print(f"make_index: {len(packages)} packages of {url} at {commit}"
          + (f"; removed {', '.join(removed)}" if removed else ""))

    if args.push:
        git("add", "-A", cwd=out)
        if git("status", "--porcelain", cwd=out):
            git("commit", "--quiet", "-m", f"Cone packages at {commit[:10]}", cwd=out)
            git("push", "--quiet", "origin", "HEAD", cwd=out)
            print(f"make_index: pushed from {out}")
        else:
            print("make_index: nothing changed, nothing pushed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
