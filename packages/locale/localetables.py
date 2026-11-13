"""Writes src/localetables.cone: the number and date conventions of the locales the
locale package knows (invariant, en, de), from Unicode's CLDR.

    python localetables.py <folder of CLDR JSON> [version]      (in packages/locale)

The folder is the "main" folder of the cldr-json release (https://github.com/unicode-org/cldr-json:
cldr-json/cldr-numbers-full/main and cldr-json/cldr-dates-full/main merged), so that
<folder>/<tag>/numbers.json and <folder>/<tag>/ca-gregorian.json exist for each tag below. The CLDR
version is the second argument, or the first line of <folder>/../version.txt; it is written into the
output header. The copy used was cldr-json 49.0.0, fetched on 13 Nov 2026.

What is read, for each tag in LOCALES:
  decimal, group, groupSize     numbers.json: symbols-numberSystem-latn, and the primary group of
                                decimalFormats-numberSystem-latn standard
  months, monthsAbbr, days      ca-gregorian.json months and days, format context, wide and
                                abbreviated (days wide), Monday first as the calendar package counts
  dayDot                        whether the long date pattern puts a '.' after the day ("d. MMMM y")
  short                         only for the tags in SHORT: the short date pattern's order,
                                separator and zero padding. en has none until en-US and en-GB are
                                separate locales (design chat, 13 Nov 2026)

invariant is en's data under id 0, without a short date (so nothing already shipped changes); it is not
read from CLDR itself. An id outside the known range reads as invariant. Ids are the position in
LOCALES; the Locale enum in locale.cone lists them in the same order.
"""

import json
import os
import re
import sys

LOCALES = ["invariant", "en", "de"]
SHORT = {"de"}
DAYS = ["mon", "tue", "wed", "thu", "fri", "sat", "sun"]


def load(folder, tag, name):
    with open(os.path.join(folder, tag, name), encoding="utf8") as f:
        return json.load(f)["main"][tag]


def lit(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def chr_lit(s):
    assert len(s) == 1, s
    return "'" + s.replace("\\", "\\\\").replace("'", "\\'") + "'"


def read(folder, tag):
    num = load(folder, tag, "numbers.json")["numbers"]
    sym = num["symbols-numberSystem-latn"]
    pat = num["decimalFormats-numberSystem-latn"]["standard"].split(";")[0]
    group_size = len(pat.split(".")[0].split(",")[-1])
    cal = load(folder, tag, "ca-gregorian.json")["dates"]["calendars"]["gregorian"]
    months = cal["months"]["format"]
    wide = [months["wide"][str(i)] for i in range(1, 13)]
    abbr = [months["abbreviated"][str(i)] for i in range(1, 13)]
    days = [cal["days"]["format"]["wide"][d] for d in DAYS]
    day_dot = bool(re.match(r"d\.", cal["dateFormats"]["long"]))
    short = None
    if tag in SHORT:
        p = cal["dateFormats"]["short"]  # for de: dd.MM.y
        parts = re.findall(r"[dMy]+|[^dMy]+", p)
        order = "".join(x[0] for x in parts if x[0] in "dMy")
        seps = {x for x in parts if x[0] not in "dMy"}
        assert len(seps) == 1 and len(next(iter(seps))) == 1, p
        padded = [x for x in parts if x[0] in "dM"]
        assert all(len(x) == 2 for x in padded) or all(len(x) == 1 for x in padded), p
        short = ({"dMy": 0, "Mdy": 1, "yMd": 2}[order], next(iter(seps)), len(padded[0]) == 2)
    return dict(decimal=sym["decimal"], group=sym["group"], groupSize=group_size,
                months=wide, abbr=abbr, days=days, dayDot=day_dot, short=short)


def main():
    folder = sys.argv[1]
    version = sys.argv[2] if len(sys.argv) > 2 else None
    vf = os.path.join(folder, "..", "version.txt")
    if version is None and os.path.exists(vf):
        version = open(vf).read().split()[0]
    if version is None:
        sys.exit("give the CLDR version as the second argument")
    data = {t: read(folder, t) for t in LOCALES if t != "invariant"}
    data["invariant"] = dict(data["en"], short=None)
    out = []
    out.append("// localetables: the number and date conventions of the locales the locale package knows,")
    out.append("// from Unicode CLDR %s. Written by localetables.py, which says how each is made; change" % version)
    out.append("// that and run it, never this file. Ids, in order: "
               + ", ".join("%d %s" % (i, t) for i, t in enumerate(LOCALES)) + ".")
    out.append("")

    # Each reader is a chain over the id; the last value is invariant's, which an id outside
    # the known range gets too
    def scalar(name, ret, f):
        out.append("fn %s(id: u8) %s {" % (name, ret))
        for i, t in list(enumerate(LOCALES))[1:]:
            out.append("  if id == %du8 {return %s;}" % (i, f(data[t])))
        out.append("  %s" % f(data["invariant"]))
        out.append("}")
        out.append("")

    scalar("decimalOf", "char", lambda d: chr_lit(d["decimal"]))
    scalar("groupOf", "char", lambda d: chr_lit(d["group"]))
    scalar("groupSizeOf", "u8", lambda d: "%du8" % d["groupSize"])
    scalar("dayDotOf", "bool", lambda d: "true" if d["dayDot"] else "false")
    scalar("hasShortOf", "bool", lambda d: "true" if d["short"] else "false")
    scalar("shortOrderOf", "u8", lambda d: "%du8" % (d["short"][0] if d["short"] else 0))
    scalar("shortSepOf", "char", lambda d: chr_lit(d["short"][1] if d["short"] else "-"))
    scalar("shortPadOf", "bool", lambda d: "true" if d["short"] and d["short"][2] else "false")
    for kind, key, n in (("monthWide", "months", 12), ("monthAbbr", "abbr", 12), ("dayWide", "days", 7)):
        # inline: emitted only where a program calls them, so the words of a locale are carried only
        # by a program that writes a month or day name
        out.append("fn %sOf(id: u8, i: usize) &str inline {" % kind)
        for i, t in list(enumerate(LOCALES))[1:]:
            arr = "[" + ", ".join(lit(x) for x in data[t][key]) + "]"
            out.append("  if id == %du8 {let a: Array[&str, %d] = %s; return a[i];}" % (i, n, arr))
        arr = "[" + ", ".join(lit(x) for x in data["invariant"][key]) + "]"
        out.append("  let a: Array[&str, %d] = %s;" % (n, arr))
        out.append("  a[i]")
        out.append("}")
        out.append("")
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "src", "localetables.cone")
    with open(path, "w", encoding="utf8", newline="\n") as f:
        f.write("\n".join(out).rstrip("\n") + "\n")


main()
