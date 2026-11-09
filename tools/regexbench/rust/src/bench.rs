// The rows of the benchmark, twin of packages/regex/examples/regexbench.cone: the same corpora from the same
// generators, the same patterns, the same count and checksum.
//
//   regexbench bench a <row>    side a: the `regex` crate (its full engine: prefilters, lazy DFA, backtracker, Pike VM)
//   regexbench bench b <row>    side b: regex-automata's Pike VM alone, no prefilter (the yardstick for a Pike-only engine)
//
// prints `row us=<microseconds> count=<matches> sum=<checksum>`.

use regex::Regex;
use regex_automata::nfa::thompson::pikevm::PikeVM;
use regex_automata::PatternID;
use std::time::Instant;

struct Rng(u64);

impl Rng {
    fn next(&mut self) -> u32 {
        self.0 = self.0.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
        (self.0 >> 33) as u32
    }
    fn below(&mut self, n: u32) -> u32 {
        self.next() % n
    }
}

fn two(out: &mut String, n: u32) {
    out.push((48 + n / 10) as u8 as char);
    out.push((48 + n % 10) as u8 as char);
}

const LOG_WORDS: [&str; 8] = ["index", "login", "search", "api", "static", "users", "orders", "health"];
const METHODS: [&str; 3] = ["GET", "POST", "PUT"];
const CODES: [&str; 3] = ["200", "404", "500"];
const DICT_WORDS: [&str; 20] = [
    "alpha", "bravo", "charlie", "delta", "echo", "foxtrot", "golf", "hotel", "india", "juliet", "kilo", "lima", "mike",
    "november", "oscar", "papa", "quebec", "romeo", "sierra", "tango",
];
const LETTERS: &[u8] = b"etaoinshrdlcumwfgypbvkjxqz";

fn log_corpus() -> String {
    let mut r = Rng(1);
    let mut s = String::with_capacity(10485760 + 200);
    while s.len() < 10485760 {
        s.push_str("2026-11-");
        two(&mut s, 1 + r.below(28));
        s.push('T');
        two(&mut s, r.below(24));
        s.push(':');
        two(&mut s, r.below(60));
        s.push(':');
        two(&mut s, r.below(60));
        s.push_str("Z ");
        let lv = r.below(50);
        s.push_str(if lv == 0 { "ERROR" } else if lv % 2 == 0 { "INFO" } else { "WARN" });
        s.push_str(" host-");
        two(&mut s, r.below(100));
        s.push(' ');
        s.push_str(METHODS[r.below(3) as usize]);
        s.push_str(" /");
        s.push_str(LOG_WORDS[r.below(8) as usize]);
        s.push_str("?x=");
        s.push_str(&r.below(1000).to_string());
        s.push(' ');
        s.push_str(CODES[r.below(3) as usize]);
        s.push(' ');
        s.push_str(&r.below(2000).to_string());
        s.push_str("ms ip ");
        for i in 0..4 {
            if i > 0 {
                s.push('.');
            }
            s.push_str(&r.below(256).to_string());
        }
        s.push('\n');
    }
    s
}

fn letter(r: &mut Rng) -> char {
    let a = r.below(26);
    let b = r.below(26);
    LETTERS[(if a < b { a } else { b }) as usize] as char
}

fn dict_corpus() -> String {
    let mut r = Rng(2);
    let mut s = String::with_capacity(1000000);
    for i in 0..100000u32 {
        if i % 5000 == 2500 {
            s.push_str(DICT_WORDS[(i / 5000) as usize]);
        } else {
            let n = 3 + r.below(8);
            for _ in 0..n {
                s.push(letter(&mut r));
            }
        }
        s.push('\n');
    }
    s
}

fn text_corpus() -> String {
    let mut r = Rng(3);
    let mut s = String::with_capacity(10485760 + 100);
    let mut words = 0u32;
    while s.len() < 10485760 {
        let n = 2 + r.below(8);
        let odd = r.below(50);
        for k in 0..n {
            if odd == 0 && k == 1 {
                s.push('é');
            } else if odd == 1 && k == 1 {
                s.push('日');
            } else {
                s.push(letter(&mut r));
            }
        }
        if r.below(10) == 0 {
            s.push_str("ing");
        }
        let p = r.below(10);
        if p == 0 {
            s.push(',');
        } else if p == 1 {
            s.push('.');
        } else if p == 2 {
            s.push(';');
        }
        words += 1;
        s.push(if words % 12 == 0 { '\n' } else { ' ' });
    }
    s
}

const P5: &str = "(?-u)\\b(?:alpha|bravo|charlie|delta|echo|foxtrot|golf|hotel|india|juliet|kilo|lima|mike|november|oscar|papa|quebec|romeo|sierra|tango)\\b";

fn pattern_of(row: &str) -> &'static str {
    if row.starts_with("p1") {
        "ERROR"
    } else if row.starts_with("p2") {
        "[0-9]{4}-[0-9]{2}-[0-9]{2}"
    } else if row.starts_with("p3") {
        "(?-u)(\\d+)\\.(\\d+)\\.(\\d+)\\.(\\d+)"
    } else if row.starts_with("p4") {
        "(?-u)(GET|POST|PUT) /(\\w+)\\?x=(\\d+)"
    } else if row.starts_with("p5") {
        P5
    } else {
        "(?-u)\\w+ing\\b"
    }
}

fn mix(h: u64, x: u64) -> u64 {
    h.wrapping_mul(1000003).wrapping_add(x)
}

// What one side can do; each returns (count, checksum)
trait Side {
    fn is_match(&self, text: &str) -> bool;
    fn find_all(&self, text: &str) -> (u64, u64);
    fn caps_all(&self, text: &str) -> (u64, u64);
}

struct A(Regex);
struct B(PikeVM, std::cell::RefCell<regex_automata::nfa::thompson::pikevm::Cache>);

impl Side for A {
    fn is_match(&self, text: &str) -> bool {
        self.0.is_match(text)
    }
    fn find_all(&self, text: &str) -> (u64, u64) {
        let (mut n, mut h) = (0, 0);
        for m in self.0.find_iter(text) {
            n += 1;
            h = mix(mix(h, m.start() as u64), m.end() as u64);
        }
        (n, h)
    }
    fn caps_all(&self, text: &str) -> (u64, u64) {
        let (mut n, mut h) = (0, 0);
        for c in self.0.captures_iter(text) {
            n += 1;
            for i in 0..c.len() {
                match c.get(i) {
                    Some(m) => h = mix(mix(h, m.start() as u64 + 1), m.end() as u64 + 1),
                    None => h = mix(h, 0),
                }
            }
        }
        (n, h)
    }
}

impl Side for B {
    fn is_match(&self, text: &str) -> bool {
        self.0.is_match(&mut self.1.borrow_mut(), text)
    }
    fn find_all(&self, text: &str) -> (u64, u64) {
        let (mut n, mut h) = (0, 0);
        let mut cache = self.1.borrow_mut();
        for m in self.0.find_iter(&mut cache, text) {
            n += 1;
            h = mix(mix(h, m.start() as u64), m.end() as u64);
        }
        (n, h)
    }
    fn caps_all(&self, text: &str) -> (u64, u64) {
        let (mut n, mut h) = (0, 0);
        let mut cache = self.1.borrow_mut();
        for c in self.0.captures_iter(&mut cache, text) {
            n += 1;
            for i in 0..c.group_len() {
                match c.get_group(i) {
                    Some(sp) => h = mix(mix(h, sp.start as u64 + 1), sp.end as u64 + 1),
                    None => h = mix(h, 0),
                }
            }
        }
        (n, h)
    }
}

fn make(side: &str, pat: &str) -> Box<dyn Side> {
    if side == "a" {
        Box::new(A(Regex::new(pat).unwrap()))
    } else {
        let vm = PikeVM::new(pat).unwrap();
        let cache = vm.create_cache();
        Box::new(B(vm, std::cell::RefCell::new(cache)))
    }
}

fn is_row(re: &dyn Side, text: &str) -> (u64, u64) {
    let b = text.as_bytes();
    let (mut pos, mut n, mut yes) = (0usize, 0u64, 0u64);
    while pos < b.len() {
        let mut e = pos;
        while e < b.len() && b[e] != b'\n' {
            e += 1;
        }
        if re.is_match(&text[pos..e]) {
            yes += 1;
        }
        n += 1;
        pos = e + 1;
    }
    (n, yes)
}

pub fn run(args: &[String]) {
    let side = args[0].as_str();
    let row = args[1].as_str();
    let (us, count, sum);
    if row == "compile" {
        let pats: Vec<&str> = (1..=6).map(|p| pattern_of(["p1", "p2", "p3", "p4", "p5", "p6"][p - 1])).collect();
        let t0 = Instant::now();
        let mut n = 0u64;
        for _ in 0..1000 {
            for p in &pats {
                if side == "a" {
                    n += Regex::new(p).unwrap().captures_len() as u64;
                } else {
                    let vm = PikeVM::new(p).unwrap();
                    n += vm.get_nfa().group_info().group_len(PatternID::ZERO) as u64;
                }
            }
        }
        us = t0.elapsed().as_micros();
        count = n;
        sum = n;
    } else if row == "adv64k" || row == "adv1m" {
        let size = if row == "adv64k" { 65536 } else { 1048576 };
        let hay = "a".repeat(size);
        let t0 = Instant::now();
        let mut n = 0u64;
        for p in ["(a*)*b", "(a|a)*b"] {
            if make(side, p).is_match(&hay) {
                n += 1;
            }
        }
        us = t0.elapsed().as_micros();
        count = n;
        sum = size as u64;
    } else {
        let (text, reps) = if row.starts_with("p5") {
            (dict_corpus(), 10)
        } else if row.starts_with("p6") {
            (text_corpus(), 1)
        } else {
            (log_corpus(), 1)
        };
        let re = make(side, pattern_of(row));
        let t0 = Instant::now();
        let mut out = (0, 0);
        for _ in 0..reps {
            out = if row.ends_with("-is") {
                is_row(re.as_ref(), &text)
            } else if row.ends_with("-caps") {
                re.caps_all(&text)
            } else {
                re.find_all(&text)
            };
        }
        us = t0.elapsed().as_micros();
        count = out.0;
        sum = out.1;
    }
    println!("{} us={} count={} sum={}", row, us, count, sum);
}
