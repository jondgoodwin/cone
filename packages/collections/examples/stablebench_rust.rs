// The Rust side of stablebench.cone: the same seven inputs (same xorshift, same value rules),
// the same two element types, 1,000,000 values, 9 timed rounds, median and spread in ms.
// Rust's stable sort_by is driftsort. Build and run optimised, outside Congo:
//
//   rustc -C opt-level=3 -o stablebench_rust.exe stablebench_rust.rs && stablebench_rust.exe
//
// (Not part of the Cone build; it is only the reference row of the table.)

use std::time::Instant;

#[derive(Clone)]
struct Rec {
    key: u32,
    id: u32,
    a: u64,
    b: u64,
    c: u64,
    d: u64,
}

fn next_rand(s: &mut u64) -> u64 {
    let mut x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *s = x;
    x
}

fn value(kind: i32, i: usize, n: usize, s: &mut u64) -> u32 {
    let r = (next_rand(s) >> 40) as u32;
    match kind {
        0 => r,
        1 => i as u32,
        2 => (n - i) as u32,
        3 => r % 4,
        4 => if i < n / 2 { i as u32 } else { (n - i) as u32 },
        5 => (i % 37) as u32 * 3 + r % 3,
        _ => if r % 50 == 0 { r } else { i as u32 / 2 },
    }
}

fn kind_name(kind: i32) -> &'static str {
    ["random", "sorted", "reversed", "4 distinct", "organ pipe", "runs of 37", "nearly sorted"][kind as usize]
}

fn report(label: &str, kind: i32, mut t: Vec<f64>, sum: u64) {
    t.sort_by(|a, b| a.partial_cmp(b).unwrap());
    println!("{} {}: rust sort_by median {:.2} ms ({:.0}-{:.0}), checksum {}", label, kind_name(kind), t[4], t[0], t[8], sum);
}

fn main() {
    let n = 1_000_000usize;
    for kind in 0..7 {
        let mut s = 12345u64;
        let src: Vec<u32> = (0..n).map(|i| value(kind, i, n, &mut s)).collect();
        let mut t = Vec::new();
        let mut sum = 0u64;
        for _ in 0..9 {
            let mut w = src.clone();
            let t0 = Instant::now();
            w.sort_by(|a, b| a.cmp(b));
            t.push(t0.elapsed().as_secs_f64() * 1000.0);
            sum = w[0] as u64 + w[n / 2] as u64 * 3 + w[n - 1] as u64 * 7;
        }
        report("u32", kind, t, sum);

        let mut s = 12345u64;
        let src: Vec<Rec> = (0..n)
            .map(|i| {
                let k = value(kind, i, n, &mut s);
                Rec { key: k, id: i as u32, a: 1, b: 2, c: 3, d: k as u64 }
            })
            .collect();
        let mut t = Vec::new();
        let mut sum = 0u64;
        for _ in 0..9 {
            let mut w = src.clone();
            let t0 = Instant::now();
            w.sort_by(|a, b| a.key.cmp(&b.key));
            t.push(t0.elapsed().as_secs_f64() * 1000.0);
            sum = w[0].id as u64 + w[n / 2].id as u64 * 3 + w[n - 1].id as u64 * 7;
        }
        report("40-byte struct", kind, t, sum);
        let _ = (src[0].a, src[0].b, src[0].c, src[0].d);
    }
}
