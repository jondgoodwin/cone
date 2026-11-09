// The Rust twin of packages/regex: the reference answers for its tests, and the rows of its benchmark.
//
//   regexbench cases        read cases on stdin, one per line, fields separated by tabs and hex encoded:
//                           kind, pattern, text, replacement; print the answer for each on a line (hex)
//   regexbench bench ...    see bench.rs

mod bench;

use regex::Regex;
use std::io::{BufRead, Write};

fn unhex(s: &str) -> Vec<u8> {
    (0..s.len() / 2).map(|i| u8::from_str_radix(&s[2 * i..2 * i + 2], 16).unwrap()).collect()
}

fn span(m: Option<regex::Match>) -> String {
    match m {
        Some(m) => format!("{}..{}", m.start(), m.end()),
        None => "_".to_string(),
    }
}

fn answer(kind: &str, pat: &str, text: &[u8], repl: &str) -> String {
    if kind == "bfind" || kind == "bcaps" {
        let re = match regex::bytes::Regex::new(pat) {
            Ok(re) => re,
            Err(_) => return "ERR".to_string(),
        };
        if kind == "bfind" {
            let v: Vec<String> = re.find_iter(text).map(|m| format!("{}..{}", m.start(), m.end())).collect();
            return if v.is_empty() { "-".to_string() } else { v.join(" ") };
        }
        return match re.captures(text) {
            None => "none".to_string(),
            Some(c) => (0..c.len())
                .map(|i| c.get(i).map_or("_".to_string(), |m| format!("{}..{}", m.start(), m.end())))
                .collect::<Vec<_>>()
                .join(","),
        };
    }
    let text = std::str::from_utf8(text).unwrap();
    let re = match Regex::new(pat) {
        Ok(re) => re,
        Err(_) => return "ERR".to_string(),
    };
    match kind {
        "ismatch" => re.is_match(text).to_string(),
        "find" => {
            let v: Vec<String> = re.find_iter(text).map(|m| format!("{}..{}", m.start(), m.end())).collect();
            if v.is_empty() { "-".to_string() } else { v.join(" ") }
        }
        "caps" => match re.captures(text) {
            None => "none".to_string(),
            Some(c) => (0..c.len()).map(|i| span(c.get(i))).collect::<Vec<_>>().join(","),
        },
        "split" => re.split(text).map(|s| format!("[{}]", s)).collect::<Vec<_>>().join(""),
        "replace" => re.replace(text, repl).into_owned(),
        "replaceall" => re.replace_all(text, repl).into_owned(),
        _ => panic!("unknown kind {}", kind),
    }
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    match args.get(1).map(|s| s.as_str()) {
        Some("cases") => {
            let stdin = std::io::stdin();
            let mut out = std::io::stdout().lock();
            for line in stdin.lock().lines() {
                let line = line.unwrap();
                if line.is_empty() {
                    continue;
                }
                let f: Vec<&str> = line.split('\t').collect();
                let pat = String::from_utf8(unhex(f[1])).unwrap();
                let text = unhex(f[2]);
                let repl = String::from_utf8(unhex(f.get(3).copied().unwrap_or(""))).unwrap();
                let a = answer(f[0], &pat, &text, &repl);
                let hex: String = a.bytes().map(|b| format!("{:02x}", b)).collect();
                writeln!(out, "{}", hex).unwrap();
            }
        }
        Some("bench") => bench::run(&args[2..]),
        _ => eprintln!("usage: regexbench cases | bench ..."),
    }
}
