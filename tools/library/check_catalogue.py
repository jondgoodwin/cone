#!/usr/bin/env python3
"""Fails unless every package of packages/ appears exactly once in catalogue_data.py and on exactly one domain
page of doc/library/, and the two agree.

Usage: python tools/library/check_catalogue.py
A package is a folder of packages/ holding a congo.toml, a src folder or .cone files at its top (a folder left
holding only old build output is not one). A package is "on a page" when the page links it (a GitHub tree or blob link to
packages/<name>). Reports packages missing from the data or from every page, packages listed twice, names that
are not a package, and a package the data and the pages place on different pages, so a page edited by hand
without its data entry (which build_catalogue.py would then drop) fails here. The full-merge gate runs it.
"""
import os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
LIB = os.path.join(ROOT, "doc", "library")
PK = os.path.join(ROOT, "packages")
LINK = re.compile(r'href="https://github\.com/jondgoodwin/cone/(?:tree|blob)/master/packages/([^/"]+)')
sys.path.insert(0, HERE)
from catalogue_data import DOMAINS


def is_package(d):
    p = os.path.join(PK, d)
    if not os.path.isdir(p):
        return False
    return (os.path.isfile(os.path.join(p, "congo.toml")) or os.path.isdir(os.path.join(p, "src"))
            or any(f.endswith(".cone") for f in os.listdir(p)))  # conestd keeps its source at the top


folders = {d for d in os.listdir(PK) if is_package(d)}

seen = {}
for f in sorted(os.listdir(LIB)):
    if not f.endswith(".html") or f == "index.html":
        continue
    with open(os.path.join(LIB, f), encoding="utf-8") as fh:
        for name in LINK.findall(fh.read()):
            seen.setdefault(name, []).append(f)

data = {}
for page_id, _title, _scope, groups in DOMAINS:
    for _group, packages in groups:
        for name, _purpose in packages:
            data.setdefault(name, []).append(page_id + ".html")

bad = []
for name in sorted(folders - set(seen)):
    bad.append(f"missing: {name} is on no domain page")
for name in sorted(folders - set(data)):
    bad.append(f"missing: {name} has no entry in catalogue_data.py")
for name, pages in sorted(seen.items()):
    if len(pages) > 1:
        bad.append(f"repeated: {name} appears {len(pages)} times on the pages ({', '.join(pages)})")
    if name not in folders:
        bad.append(f"unknown: {name} ({pages[0]}) is not a package of packages/")
for name, pages in sorted(data.items()):
    if len(pages) > 1:
        bad.append(f"repeated: {name} appears {len(pages)} times in catalogue_data.py ({', '.join(pages)})")
    if name not in folders:
        bad.append(f"unknown: {name} in catalogue_data.py is not a package of packages/")
    if name in seen and sorted(seen[name]) != sorted(pages):
        bad.append(f"disagree: {name} is on {', '.join(seen[name])} but catalogue_data.py puts it on {', '.join(pages)}"
                   " (run build_catalogue.py)")
for name in sorted(set(seen) - set(data)):
    if name in folders:
        bad.append(f"page only: {name} is on {seen[name][0]} but not in catalogue_data.py, so a rebuild drops it")
for b in bad:
    print(b)
print(f"{len(folders)} packages, {len(seen)} on pages, {len(data)} in the data, {len(bad)} problem(s)")
sys.exit(1 if bad else 0)
