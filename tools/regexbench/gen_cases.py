"""Make packages/regex/tests/semantics.cone from a list of cases.

Each case is a pattern, a text and a kind of question. The expected answer comes from the Rust `regex` crate
(tools/regexbench/rust, pinned in its Cargo.lock) and, where Python's `re` means the same, is checked against
Python too: a disagreement is printed, and decided by hand (there are a few, listed in KNOWN_PY_DIFFERENCES).

    cd tools/regexbench/rust && cargo build --release      (CARGO_TARGET_DIR is the twin's shared folder)
    python tools/regexbench/gen_cases.py [path to regexbench.exe]

The answers are in a plain notation: `find` and `bfind` give `s..e s..e` (`-` for none), `caps` gives
the spans of the groups of the first match joined by `,` (`_` for a group that took no part, `none` for no match),
`split` gives each part in `[]`, `replace` and `replaceall` the new text, `ismatch` true or false. The
patterns that Cone refuses on purpose (a bare \\w, \\p{..}, look-around, ...) are not here: tests/errors.cone.
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "..", "packages", "regex", "tests", "semantics.cone")
EXE = sys.argv[1] if len(sys.argv) > 1 else r"C:\src\zz-rusttwin-target\release\regexbench.exe"

# (kind, pattern, text[, replacement])
CASES = []


def add(kind, pat, text, repl=""):
    CASES.append((kind, pat, text, repl))


def finds(pat, *texts):
    for t in texts:
        add("find", pat, t)


# --- literals, concatenation, alternation order
finds("abc", "abc", "xabcabc", "ab", "", "abcab")
finds("héllo", "say héllo héllo", "hello")
finds("日本", "日本語日本", "日")
finds("a|b", "cab", "")
finds("a|ab", "ab", "abab")           # first alternative wins, not the longest
finds("ab|a", "ab", "aab")
finds("abc|abd|ab", "abdabcab")
finds("a||b", "ab")                     # an empty alternative in the middle
finds("a|", "ba")
finds("|a", "ba")
finds("(a|ab)(c|bcd)", "abcd")
finds("foo|foobar", "foobar")
finds("x*|y", "yyxy")
# alternatives that begin with different bytes (one Sparse state), and the ones that do not
finds("foo|bar|qux|zip", "zip foo xx qux bar fo ba")
finds("foo|bar|baz|qux", "foo bar baz qux")
finds("[a-c]x|[d-f]y|z", "ax dy z cy ey fx bx")
finds("[a-c]x|b|c", "bx b cx c")
finds("é|ü|日", "éü日a")
finds("é|e|ü", "éeü")
finds("(?:a|b|)c", "ac bc c d")
finds("(?:|a|b)c", "ac bc c d")
finds("(?:ab|cd|ef)+", "abcdefxab")
add("caps", "(foo|bar|qux)(x)", "qux barx")
add("caps", "(foo|bar|qux)(x)", "barx")
add("caps", "(?:(a)|(b)|(c))+", "abc")
# --- repetition
finds("a*", "baaac", "", "aba")
finds("a+", "baaac", "b")
finds("a?", "baa")
finds("a{2}", "aaaaa", "a")
finds("a{2,}", "aaaaa", "a", "aaaabaaa")
finds("a{2,3}", "aaaaaaa")
finds("a{0}", "aa")
finds("a{0,2}", "aaaaa")
finds("a{1}", "aaa")
finds("a*?", "aaa")
finds("a+?", "aaa")
finds("a??", "aaa")
finds("a{2,3}?", "aaaaaaa")
finds("a{2,}?", "aaaaa")
finds("(?:ab)*", "ababab", "abab")
finds("(?:ab){2}", "ababab")
finds("(a*)*", "aaab")
finds("(a*)+", "aaab")
finds("(a|b)*c", "ababc abc c")
finds("(a+)+b", "aaab")
finds("(?:a|b)+", "xxabbaxbb")
finds("a{1,2}{2}", "aaaaa")
finds("(?:a*)*", "aab")
finds("(?:a?)*?b", "aab")
finds("[0-9]{4}-[0-9]{2}-[0-9]{2}", "on 2026-11-07 and 2027-01-02x")
finds("a.c", "abc a\nc a日c")
# --- classes
finds("[abc]+", "xxabcabzcc")
finds("[a-c]", "abcd")
finds("[^a-c]", "abcd")
finds("[^a]", "aé日b")
finds("[a-]", "a-b")
finds("[-a]", "a-b")
finds("[]a]", "]a")
finds("[^]a]", "]ab")
finds("[a\\-z]+", "a-z b")
finds("[\\]]", "a]")
finds("[\\n\\t]", "a\n\tb")
finds("[α-ω]+", "abc αβγ δ xyz")
finds("[^α-ω]", "aβb")
finds("[é日]", "aé日b")
finds("[a-c[x-z]]+", "abxyzd")
finds("[a-z&&[^aeiou]]+", "hello world")
finds("[a-z--[aeiou]]+", "hello world")
finds("[a-g~~d-k]+", "abcdefghijk")
finds("[[:alpha:]]+", "ab12cd")
finds("[[:^alpha:]]+", "ab12cd")
finds("[[:digit:][:upper:]]+", "aB12c")
finds("[[:punct:]]+", "a,.b")
finds("[[:xdigit:]]+", "zzCafe9g")
finds("[[:word:]]+", "a_1 b")
finds("[[:space:]]", "a b\tc")
finds("[\\x41-\\x43]+", "ABCDE")
finds("[\\x{263A}]", "a☺b")
finds("[\\u263A-\\u263C]+", "☺☻☼x")
finds("[\\u{1F600}-\\u{1F64F}]", "a😀b")
finds(".", "aé日😀")
finds(".", "\n")
finds("(?s).", "a\nb")
finds("[^x]+", "ab\ncd")
finds("(?s)a.*b", "a\nb\nb")
finds("a.*b", "a\nb\nab")
finds(".*", "ab\ncd")
# --- escapes
finds("\\x41\\x42", "xAB")
finds("\\x{263A}", "a☺")
finds("\\u263A", "a☺")
finds("\\u{1F600}", "a😀")
finds("\\U0001F600", "a😀")
finds("\\n", "a\nb")
finds("\\t\\r\\f\\v", "\t\r\x0c\x0b")
finds("\\a", "\x07")
finds("\\.\\+\\*\\?\\(\\)\\|\\[\\]\\{\\}\\^\\$\\\\", ".+*?()|[]{}^$\\")
finds("\\#\\&\\-\\~", "#&-~")
finds("}", "a}")
finds("]", "a]")
# --- anchors and boundaries
finds("^ab", "ab", "abab", "cab")
finds("ab$", "ab", "abab", "abc")
finds("^$", "", "a")
finds("^ab$", "ab", "ab\n", "\nab")
finds("$", "ab\n")
finds("(?m)$", "ab\ncd\n")
finds("(?m)^", "ab\ncd\n")
finds("(?m)^ab$", "ab\nab\nxab\nab")
finds("\\Aab", "abab")
finds("ab\\z", "abab", "ab\n")
finds("(?m)\\Aab", "ab\nab")
finds("(?m)ab\\z", "ab\nab\n")
finds("(?-u:\\b)", "ab cd")
finds("(?-u:\\B)", "ab c")
finds("(?-u:\\bab\\b)", "ab abc ab")
finds("(?-u)\\bfoo\\b", "foo food foo")
finds("(?-u:\\b)", "é")
finds("(?-u:\\B)", "é")
finds("(?-u)\\b", "aé b")
finds("^*", "ab")
# --- perl classes in ASCII mode
finds("(?-u:\\d+)", "a12b345")
finds("(?-u)\\w+", "héllo wörld_1")
finds("(?-u:\\s+)", "a \t\nb")
finds("(?-u)[\\d\\s]+", "a1 2b")
finds("(?-u)[\\w&&[a-f]]+", "ab12cdz")
# --- groups and captures
add("caps", "(a)(b)?", "ac")
add("caps", "(a)(b)?", "ab")
add("caps", "(a)|(b)", "b")
add("caps", "(a|b)+", "abab")
add("caps", "(a+)(b+)?", "aaac")
add("caps", "(?:a)(b)", "ab")
add("caps", "(?P<x>a)(?P<y>b)", "ab")
add("caps", "(?<x>a)(?<y>b)", "ab")
add("caps", "((a)(b))", "ab")
add("caps", "(a(b(c)))", "abc")
add("caps", "(a*)*", "b")
add("caps", "(a*)+", "b")
add("caps", "(a|ab)(c|bcd)(d*)", "abcd")
add("caps", "(a*?)(a*)", "aaa")
add("caps", "(\\d+)-(\\d+)".replace("\\d", "[0-9]"), "tel 555-1234")
add("caps", "^(\\w+)@(\\w+)\\.com$".replace("\\w", "[a-z]"), "bob@example.com")
add("caps", "x*", "")
add("caps", "(x)*", "")
add("caps", "(?i-u)(a)(B)", "xAb")
add("caps", "([a-c]+)([c-e]+)", "abcde")
add("caps", "(a?)*?b", "aab")
add("caps", "(?:(a)|b)*", "ab")
add("caps", "(?:(a)|(b))*", "ab")
add("caps", "(foo)?(bar)?", "bar")
add("caps", "a", "b")
# --- flags
finds("(?i-u)abc", "ABC aBc abc xyz")
finds("(?i-u)[a-c]+", "AbCd")
finds("(?i-u:abc)d", "ABCd ABCD")
finds("(?i-u)a(?-i)b", "AB Ab ab aB")
finds("(?i-u)é", "É é")
finds("(?s:.)", "\n")
finds("(?m:^a$)", "a\na")
finds("(?x) a b c ", "abc a b c")
finds("(?x) a # comment\n b", "ab")
finds("(?x)a\\ b", "a b")
finds("(?x)[a b]+", "a b c")
finds("(?U)a+", "aaa")
finds("(?U)a+?", "aaa")
finds("(?U)a*", "aa")
finds("(?U:a+)b", "aab")
finds("(a(?i-u)b)c", "aBc aBC")
finds("(?i-u)(?-i)a", "A a")
finds("(?-u:a)(?-u)b", "ab")
finds("(?:(?i-u)a)b", "AB Ab")
finds("a(?:b|(?i-u)c|d)", "aB aC ac aD ad")
# --- the edges in the design
finds("", "")
finds("", "abc")
finds("", "é日")
finds("a*", "é")
finds("a*?", "é")
finds("(?-u:\\b)", "")
add("ismatch", "a", "")
add("ismatch", "", "")
add("ismatch", "a+b", "xxaab")
add("ismatch", "a+b", "xxaa")
add("ismatch", "^$", "")
add("ismatch", "(?:a|b)*c", "ababab")
add("ismatch", "(?:a|b)*c", "ababc")
finds("(a)*", "aaa")
finds("[^\\n]*\\n", "ab\ncd\nef")
finds("\\z", "ab")
finds("\\A", "ab")
finds("^^ab$$", "ab")
finds("(?-u)a\\B", "ab a")
finds("é*", "éé a é")
finds("(?:é|日)+", "éé日a日")
finds("xyz", "xy")
finds("日*", "a日日")
# --- long enough to need the whole machine
finds("(?:a|b|c|d|e|f|g)+x", "abcdefgabcdefgx abcdefg")
finds("(a?){12}a{12}", "aaaaaaaaaaaa aaaaaaaaaaaaaaaaaaaaaaaa")
finds("[a-q][^u-z]{5}x", "abcdefx  zzzzzzx")
# --- split
for pat, text in [
    (",", "a,b,c"), (",", "a,b,"), (",", ",a"), (",", ""), (",", "abc"), ("", "abc"), ("", ""), ("a*", "baac"),
    (",[ ]*", "a, b,c ,  d"), ("é", "aébéc"), ("\\n", "x\ny\n"), ("(?-u:\\s+)", " a  b "), ("x", "xxx"),
]:
    add("split", pat, text)
# --- replace and replaceAll
for pat, text, repl in [
    ("a", "banana", "o"), ("a+", "baaanaa", "<$0>"), ("(a)(n)", "banana", "$2$1"), ("(a)(n)", "banana", "${2}_${1}"),
    ("(?P<x>a)(?P<y>n)", "banana", "$y$x"), ("(?P<x>a)(?P<y>n)", "banana", "${y}-${x}"),
    ("a", "banana", "$$"), ("a", "banana", "$"), ("a", "banana", "$$$"), ("a", "banana", "$$1"),
    ("(a)", "banana", "$1x"), ("(a)", "banana", "${1}x"), ("(a)", "banana", "$1_x"),
    ("(a)", "banana", "$2"), ("(a)", "banana", "$nope"), ("(a)", "banana", "${nope}"), ("(a)", "banana", "${1"),
    ("(a)?b", "bab", "[$1]"), ("x*", "abc", "-"), ("x*", "é", "-"), ("", "ab", "."), ("a", "", "b"), ("a", "bbb", "c"),
    ("é", "caféé", "e"), ("(?-u:\\b)", "ab cd", "|"), ("^", "a\nb", ">"), ("(?m)^", "a\nb", ">"), ("$", "a\nb", "<"),
    ("(a)|b", "ab", "[$1]"), ("a|b", "ab", "$0$0"), ("(b)", "abc", "$1$1$0"), ("a", "banana", "$0$0"),
    ("(\\w)(\\w)".replace("\\w", "[a-z]"), "abcd", "$2$1"), ("a+", "baaa", "é"), ("(a)", "a", "$ 1"), ("(a)", "a", "$-1"),
]:
    add("replaceall", pat, text, repl)
    add("replace", pat, text, repl)
# --- bytes mode
BYTES = [
    ("(?-u).", b"a\xffb"), ("(?-u)\\xff", b"a\xffb\xff"), ("(?-u)[^a]", b"a\xffb"), ("(?-u)\\W+", b"a\xff!b"),
    ("(?-u)\\D", b"1\xff2"), ("(?-u)\\S+", b"a b\xff"), ("(?-u)\\xe9", "é".encode()), (".", b"a\xffb"),
    ("(?-u)a\\xff?", b"a\xffa"), ("(?-u)[\\x80-\\xff]+", b"a\xe9\xffb"), ("(?-u)\\x00", b"a\x00b"),
    ("é", "é".encode()), ("(?-u).", b"\xff"), ("(?-u)", b"\xff"), ("(?-u)\\B", b"\xff"), ("(?s-u).", b"\n\xff"),
    ("(?-u)(?i)k", b"K"), ("a.*b", b"a\xffb"), ("(?-u)a.*b", b"a\xffb"),
]
for pat, text in BYTES:
    add("bfind", pat, text)
for pat, text in [("(?-u)(a)(\\xff)?", b"a\xff"), ("(?-u)(.)(.)", b"\xfe\xff")]:
    add("bcaps", pat, text)

# Patterns where Python's `re` means something else; the Rust answer stands on its own
KNOWN_PY_DIFFERENCES = "empty matches next to a match, \\z, (?-u), POSIX classes, \\x{..}, (?P<x>) in replacements, $-replacements"


def tobytes(x):
    return x if isinstance(x, bytes) else x.encode("utf-8")


def hexed(x):
    return tobytes(x).hex()


def rust_answers():
    lines = []
    for kind, pat, text, repl in CASES:
        lines.append("\t".join([kind, hexed(pat), hexed(text), hexed(repl)]))
    out = subprocess.run([EXE, "cases"], input="\n".join(lines) + "\n", capture_output=True, text=True, check=True)
    return [bytes.fromhex(l).decode("utf-8") for l in out.stdout.splitlines()]


def cone_str(s):
    r = ['"']
    for ch in s:
        if ch == "\\":
            r.append("\\\\")
        elif ch == '"':
            r.append('\\"')
        elif ch == "\n":
            r.append("\\n")
        elif ch == "\t":
            r.append("\\t")
        elif ch == "\r":
            r.append("\\r")
        elif ord(ch) < 0x20 or ord(ch) == 0x7F:
            r.append("\\x%02x" % ord(ch))
        else:
            r.append(ch)
    r.append('"')
    return "".join(r)


def python_check(kind, pat, text, repl, want):
    """Python's answer where its syntax matches Rust's for this pattern, else None."""
    if kind not in ("find", "caps", "ismatch"):
        return None
    if any(x in pat for x in ("(?-u", "(?i-u", "(?<", "[[:", "&&", "--", "~~", "\\x{", "\\u{", "\\U", "\\z", "(?U", "(?s-s", "\\a", "(?x)", "(?m)", "(?s)", "(?i", "(?:a|b|c", "(?P")):
        return None
    if "\\u" in pat or isinstance(text, bytes):
        return None
    try:
        r = re.compile(pat)
    except re.error:
        return None
    if kind == "ismatch":
        return str(bool(r.search(text))).lower()
    def b(i):  # Python counts characters, Rust bytes
        return len(text[:i].encode("utf-8"))

    def sp(m, i=0):
        s, e = m.span(i)
        return "%d..%d" % (b(s), b(e))

    if kind == "find":
        # Python reports an empty match right after a match; Rust skips it
        return " ".join(sp(m) for m in r.finditer(text)) or "-"
    m = r.search(text)
    if not m:
        return "none"
    return ",".join("_" if m.span(i) == (-1, -1) else sp(m, i) for i in range(r.groups + 1))


def main():
    answers = rust_answers()
    assert len(answers) == len(CASES)
    py_diffs = 0
    out = []
    out.append("// GENERATED by tools/regexbench/gen_cases.py: do not edit; edit the case list and run it again.")
    out.append("// Each answer is the Rust `regex` crate's (pinned in tools/regexbench/rust), and where Python's `re` means the")
    out.append("// same thing it agreed too (the script prints the differences it found, to be decided by hand).")
    out.append("")
    out.append("mod semantics;")
    out.append("import testing use *;")
    out.append("import collections use *;")
    out.append("import regex;")
    out.append("")
    out.append(HELPERS)
    out.append("fn main() i32 {")
    for i, ((kind, pat, text, repl), want) in enumerate(zip(CASES, answers)):
        if want == "ERR":
            print("RUST REFUSES:", kind, repr(pat))
            continue
        py = python_check(kind, pat, text, repl, want)
        if py is not None and py != want:
            py_diffs += 1
            print("PY DIFFERS:", kind, repr(pat), repr(text), "rust:", want, "python:", py)
        label = "%s %s / %s" % (kind, pat, text if isinstance(text, str) else text.hex())
        label = label.replace("\n", "\\n").replace("\t", "\\t").replace("\r", "\\r")
        label = "".join(c if 0x20 <= ord(c) < 0x7F or ord(c) > 0x7F else "?" for c in label)
        if isinstance(text, bytes):
            call = 'ask("%s", %s, "%s", "")' % (kind, cone_str(pat), text.hex())
        else:
            call = 'ask("%s", %s, %s, %s)' % (kind, cone_str(pat), cone_str(text), cone_str(repl))
        out.append("  expectStr(%s.view(), %s, %s);" % (call, cone_str(want), cone_str(label)))
    out.append("  done()")
    out.append("}")
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out) + "\n")
    print("wrote", len(CASES), "cases;", py_diffs, "differ from Python (see above)")


HELPERS = r'''// The answer, asked of the pattern as written (the bounded backtracker answers groups
// on a short text) and of the same pattern with a dead alternative of 132000 states
// added (a program too big for the backtracker's bits, so the Pike VM answers): the two
// must agree
fn ask(kind: &str, pat: &str, text: &str, repl: &str) String {
  let a = askOne(kind, pat, text, repl);
  if a.view().startsWith("ERR") { return a; }
  var padded = String.from("(?:");
  padded.append(pat);
  padded.append(")|(?:\\z[a-z]{1000}){132}");
  let b = askOne(kind, padded.view(), text, repl);
  if b.view() == a.view() || b.view().startsWith("ERR") { return a; }
  var s = String.from("ENGINES DIFFER: ");
  s.append(a.view());
  s.append(" / ");
  s.append(b.view());
  s
}

// Every match as 'start..end', found by asking for the groups again and again
fn capsList(re: &regex.Regex, t: &str) String {
  var out = String();
  var pos = 0usize;
  var lastEnd = 0xFFFFFFFFFFFFFFFFusize;
  var first = true;
  while pos <= t.len() {
    match re.capturesFrom(t, pos) {
      case let Some(c) {
        let m = c.span(0).unwrap();
        if m.start == m.end && m.end == lastEnd { pos = m.end + 1; }
        else {
          if !first { out.append(" "); }
          first = false;
          out <- m.start, "..", m.end;
          lastEnd = m.end;
          pos = m.end;
        }
      }
      case is None { pos = t.len() + 1; }
    }
  }
  if first { out.append("-"); }
  out
}

// The answer to a question, in the notation gen_cases.py explains
fn askOne(kind: &str, pat: &str, text: &str, repl: &str) String {
  let bytes = kind == "bfind" || kind == "bcaps";
  var out = String();
  var built = regex.Regex.parse(pat);
  if bytes { built = regex.Regex.parseBytes(pat); }
  match built {
    case let Ok(re) {
      var hay = String.from(text);
      if bytes { hay = unhex(text); }
      let t = hay.view();
      if kind == "find" || kind == "bfind" {
        var it = re.matches(t);
        var first = true;
        var go = true;
        while go {
          match it.next() {
            case let Some(m) {
              if !first { out.append(" "); }
              first = false;
              out <- m.start, "..", m.end;
            }
            case is None { go = false; }
          }
        }
        if first { out.append("-"); }
        if capsList(&re, t).view() != out.view() { out.append(" !CAPS-DIFFER"); }
      }
      else if kind == "caps" || kind == "bcaps" {
        match re.captures(t) {
          case let Some(c) {
            var i = 0usize;
            while i < c.len() {
              if i > 0 { out.append(","); }
              match c.span(i) {
                case let Some(m) { out <- m.start, "..", m.end; }
                case is None { out.append("_"); }
              }
              i = i + 1;
            }
          }
          case is None { out.append("none"); }
        }
      }
      else if kind == "ismatch" { out.append(if re.isMatch(t) { "true" } else { "false" }); }
      else if kind == "split" {
        var it = re.split(t);
        var go = true;
        while go {
          match it.next() {
            case let Some(part) { out <- "[", part, "]"; }
            case is None { go = false; }
          }
        }
      }
      else if kind == "replace" { out = re.replace(t, repl); }
      else if kind == "replaceall" { out = re.replaceAll(t, repl); }
    }
    case let Err(e) { out <- "ERR ", e; }
  }
  out
}

fn unhex(h: &str) String {
  var s = String();
  var i = 0usize;
  while i + 1 < h.len() {
    s.appendByte(hexByte(h[i]) * 16u8 + hexByte(h[i + 1]));
    i = i + 2;
  }
  s
}

fn hexByte(c: u8) u8 { if c >= 'a' { c - 'a' + 10u8 } else { c - '0' } }
'''

if __name__ == "__main__":
    main()
