"""Writes the checks of ../zone.cone (between its GENERATED markers) and the sweep sums.

The expected offsets come from Python's own readers of the same IANA release (2026f), not from
the code under test: zoneinfo reading the slim files of the 'tzdata' package, and pytz reading
the fat files of the 'pytz' package. Each instant is asserted equal in both before it is written.
The .tzif files beside this one are those packages' copies of 2026f's.

    python -m pip install --target LIB tzdata pytz
    PYTHONPATH=LIB python make_expected.py > checks.txt

IANA's tz database is in the public domain.
"""
import datetime as dt
import sys
import zoneinfo

import pytz

UTC = dt.timezone.utc
EPOCH = dt.datetime(1970, 1, 1, tzinfo=UTC)


def at(secs):
    return EPOCH + dt.timedelta(seconds=secs)


def secs_of(y, mo, d, h=0, mi=0, s=0):
    return int((dt.datetime(y, mo, d, h, mi, s, tzinfo=UTC) - EPOCH).total_seconds())


class Ref:
    def __init__(self, key):
        self.key = key
        self.zi = zoneinfo.ZoneInfo(key)
        self.pz = pytz.timezone(key)

    def zoneinfo_at(self, secs):
        d = at(secs).astimezone(self.zi)
        return int(d.utcoffset().total_seconds()), d.tzname(), bool(d.dst())

    def pytz_at(self, secs):
        d = at(secs).astimezone(self.pz)
        return int(d.utcoffset().total_seconds()), d.tzname(), bool(d.dst())

    def get(self, secs):
        a = self.zoneinfo_at(secs)
        if secs >= secs_of(2037, 12, 31) or secs < secs_of(1902, 1, 1):
            # pytz reads only the 32-bit data (1901 to 2037) and has no footer rule
            return a
        b = self.pytz_at(secs)
        # pytz rounds a local mean time (LMT) to the minute; the file holds the second
        # (and names two local mean times, LMT and MMT, of one offset alike)
        lmt = "LMT" in (a[1], b[1]) and abs(a[0] - b[0]) < 60 and a[2] == b[2]
        assert a == b or lmt, (self.key, secs, a, b)
        return a

    def changes(self, lo, hi):
        """Every instant in [lo, hi) (seconds) where the offset or the name changes."""
        out = []
        prev = self.get(lo)
        t = lo
        while t < hi:
            n = min(t + 3600, hi)
            cur = self.get(n)
            if cur != prev:
                a, b = t, n          # prev at a, cur at b
                while b - a > 1:
                    m = (a + b) // 2
                    if self.get(m) == prev:
                        a = m
                    else:
                        b = m
                out.append(b)
                prev = cur
            t = n
        return out


def iso(secs):
    return at(secs).strftime("%Y-%m-%dT%H:%M:%SZ")


def line(ref, fn, secs):
    off, name, dst = ref.get(secs)
    d = at(secs)
    return '  check(z, kind, "%s %s", %d, %d, %d, %d, %d, %d, %d, "%s", %s);' % (
        fn, iso(secs), d.year, d.month, d.day, d.hour, d.minute, d.second, off, name,
        "true" if dst else "false")


# name, key, windows (y1, m1, d1, y2, m2, d2) to scan for changes, plain instants
ZONES = [
    ("Sao_Paulo", "America/Sao_Paulo",
     [(1913, 12, 1, 1914, 2, 1), (2008, 10, 1, 2008, 11, 1), (2018, 10, 1, 2019, 3, 1), (2019, 10, 1, 2019, 12, 1)],
     [(1900, 1, 1), (1950, 6, 1), (2019, 11, 3, 3), (2026, 6, 1, 12), (2100, 1, 1)]),
    ("Moscow", "Europe/Moscow",
     [(1919, 7, 1, 1919, 9, 1), (2010, 10, 1, 2011, 4, 30), (2014, 10, 1, 2014, 11, 30)],
     [(1900, 1, 1), (2010, 7, 1), (2012, 7, 1), (2026, 1, 1), (2100, 7, 1)]),
    ("Kyiv", "Europe/Kyiv",
     [(1996, 3, 1, 1996, 11, 1), (2026, 3, 1, 2026, 4, 30), (2026, 10, 1, 2026, 11, 30)],
     [(1900, 1, 1), (2026, 1, 15), (2026, 7, 15), (2100, 3, 28, 0), (2100, 3, 28, 1)]),
    ("New_York", "America/New_York",
     [(1883, 11, 1, 1883, 12, 1), (2006, 10, 1, 2006, 11, 30), (2007, 3, 1, 2007, 3, 30), (2026, 3, 1, 2026, 3, 30), (2026, 10, 25, 2026, 11, 10),
      (2037, 10, 1, 2037, 11, 30), (2038, 3, 1, 2038, 3, 30)],
     [(1850, 1, 1), (2026, 1, 15), (2026, 7, 15), (2100, 7, 4)]),
    ("Lord_Howe", "Australia/Lord_Howe",
     [(2026, 3, 25, 2026, 4, 10), (2026, 9, 25, 2026, 10, 10)],
     [(1850, 1, 1), (2026, 1, 15), (2026, 7, 15)]),
    ("Dublin", "Europe/Dublin",
     [(2026, 3, 20, 2026, 4, 5), (2026, 10, 20, 2026, 11, 5)],
     [(2026, 1, 15), (2026, 7, 15)]),
    ("Nuuk", "America/Nuuk",
     [(2023, 10, 20, 2023, 11, 5), (2026, 3, 20, 2026, 4, 5), (2026, 10, 20, 2026, 11, 5)],
     [(2026, 1, 15), (2026, 7, 15)]),
    ("London", "Europe/London",
     [(1847, 11, 25, 1847, 12, 10), (1941, 4, 1, 1941, 6, 1), (2037, 3, 20, 2037, 4, 5), (2037, 10, 20, 2037, 11, 5), (2038, 3, 20, 2038, 4, 5),
      (2038, 10, 20, 2038, 11, 5)],
     [(1900, 1, 1), (2026, 7, 15), (2100, 1, 1)]),
]


def plain(p):
    y, mo, d = p[:3]
    h = p[3] if len(p) > 3 else 0
    return secs_of(y, mo, d, h)


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "checks"
    if mode == "checks":
        for name, key, windows, plains in ZONES:
            ref = Ref(key)
            print("// %s" % key)
            print("fn check%s(z: &Zone, kind: &str) {" % name.replace("_", ""))
            for w in windows:
                lo, hi = secs_of(*w[:3]), secs_of(*w[3:])
                for c in ref.changes(lo, hi):
                    for t in (c - 1, c, c + 1):
                        print(line(ref, name, t))
            for p in plains:
                print(line(ref, name, plain(p)))
            print("}\n")
    else:
        # Sums over 1900-01-01 to 2100-01-01 every 10007 seconds: of the offsets, of the
        # daylight saving flags, and the number of times the abbreviation changed
        for name, key, _, _ in ZONES:
            ref = Ref(key)
            lo, hi = secs_of(1900, 1, 1), secs_of(2100, 1, 1)
            n = off = dstn = changes = 0
            last = None
            t = lo
            while t < hi:
                o, a, d = ref.zoneinfo_at(t)
                n += 1
                off += o
                dstn += 1 if d else 0
                if last is not None and a != last:
                    changes += 1
                last = a
                t += 10007
            print("%s %s samples=%d offsetSum=%d dstCount=%d nameChanges=%d" % (name, key, n, off, dstn, changes))


main()
