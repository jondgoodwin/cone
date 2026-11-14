#!/usr/bin/env python3
"""Writes the library catalogue's pages (doc/library/*.html) from catalogue_data.py.

Usage: python tools/library/build_catalogue.py
Each package is linked to its README if it has one, else to its folder, on GitHub; the purposes are in
catalogue_data.py. Run tools/library/check_catalogue.py afterwards.
"""
import html, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
from catalogue_data import DOMAINS

BASE = "https://github.com/jondgoodwin/cone"
OUT = os.path.join(ROOT, "doc", "library")


def page(title, sub, body):
    return f"""<html>
  <head>
    <title>{html.escape(title)} - the Cone library</title>
    <meta content="text/html; charset=UTF-8" http-equiv="content-type">
    <meta name="viewport" content="width=device-width, initial-scale=1" />
    <link rel="stylesheet" type="text/css" href="standard.css" />
  </head>
  <body>
    <div id="header">
      <a href="/"><div id="title">Cone</div></a>
      <div id="sub">{html.escape(sub)}</div>
    </div>
    <div class="text">
{body}
    </div>
  </body>
</html>
"""


def link(name):
    d = os.path.join(ROOT, "packages", name)
    if os.path.exists(os.path.join(d, "README.md")):
        return f"{BASE}/blob/master/packages/{name}/README.md"
    return f"{BASE}/tree/master/packages/{name}"


def write(name, text):
    with open(os.path.join(OUT, name), "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def count(subs):
    return sum(len(p) for _, p in subs)


index = ["      <p>Every package in <code>packages/</code>, grouped by the domain it serves. Each domain page lists its packages in "
         "subgroups, one line each; a package appears on exactly one domain page "
         "(<code>tools/library/check_catalogue.py</code> checks it). Detailed descriptions come later.</p>", "      <ul>"]
for pid, title, scope, subs in DOMAINS:
    index.append(f'      <li><b><a href="{pid}.html">{html.escape(title)}</a></b> ({count(subs)} packages): {html.escape(scope)}</li>')
    body = [f"      <h3>{html.escape(title)}</h3>", f"      <p>{html.escape(scope)}</p>"]
    for stitle, pkgs in subs:
        body.append(f"      <h4>{html.escape(stitle)}</h4>")
        body.append("      <ul>")
        for name, purpose in pkgs:
            body.append(f'      <li><a href="{link(name)}"><code>{name}</code></a>: {html.escape(purpose)}</li>')
        body.append("      </ul>")
    body.append('      <p><a href="index.html">All domains</a></p>')
    write(pid + ".html", page(title, "The Cone library", "\n".join(body)))
index.append("      </ul>")
write("index.html", page("The library", "The Cone library: packages by domain", "      <h3>The library</h3>\n" + "\n".join(index)))
print("wrote", len(DOMAINS) + 1, "pages to", OUT)
