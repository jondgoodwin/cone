#!/usr/bin/env python3
"""Fails unless every folder of packages/ appears on exactly one domain page of doc/library/.

Usage: python tools/library/check_catalogue.py
A package is "on a page" when the page links it (a GitHub tree or blob link to packages/<name>). Reports packages
missing from every page, packages listed twice (on one page or across pages), and names that are not a folder
of packages/. Reads only the pages, not catalogue_data.py, so it checks what is published.
"""
import os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LIB = os.path.join(ROOT, "doc", "library")
PK = os.path.join(ROOT, "packages")
LINK = re.compile(r'href="https://github\.com/jondgoodwin/cone/(?:tree|blob)/master/packages/([^/"]+)')

folders = {d for d in os.listdir(PK) if os.path.isdir(os.path.join(PK, d))}
seen = {}
for f in sorted(os.listdir(LIB)):
    if not f.endswith(".html") or f == "index.html":
        continue
    with open(os.path.join(LIB, f), encoding="utf-8") as fh:
        for name in LINK.findall(fh.read()):
            seen.setdefault(name, []).append(f)

bad = []
for name in sorted(folders - set(seen)):
    bad.append(f"missing: {name} is on no domain page")
for name, pages in sorted(seen.items()):
    if len(pages) > 1:
        bad.append(f"repeated: {name} appears {len(pages)} times ({', '.join(pages)})")
    if name not in folders:
        bad.append(f"unknown: {name} ({pages[0]}) is not a folder of packages/")
for b in bad:
    print(b)
print(f"{len(folders)} packages, {len(seen)} listed, {len(bad)} problem(s)")
sys.exit(1 if bad else 0)
