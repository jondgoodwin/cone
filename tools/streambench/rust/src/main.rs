// The Rust twin of packages/stageio/examples/streamsample.cone and packages/stages/examples/streamrates.cone (streams
// PR 7), and of tools/streambench/csharp: the same job on the same input. A gzip stream
// arrives over a loopback TCP socket; inflate it, take its sha256, and write the inflated
// bytes to a file. Tokio for the sockets, tasks and file, `bytes` for the chunks, flate2 for
// the codec, sha2 for the digest.
//
//   cargo run --release -- pipeline 128    socket -> bounded channel -> inflate -> sha256 + file, as bytes arrive
//   cargo run --release -- adhoc 128       read the whole body, inflate it in one call, hash, write
//   cargo run --release -- rates 128       each stage alone: inflate, sha256, file write, loopback socket
//   cargo run --release -- digest 128      print the hex sha256 of the inflated data (to compare across twins)
//
// A third argument repeats the run (runs). Output lines match the C# twin's.
use bytes::{Bytes, BytesMut};
use flate2::write::{GzDecoder, GzEncoder};
use flate2::Compression;
use sha2::{Digest, Sha256};
use std::io::Write;
use std::time::Instant;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{TcpListener, TcpStream};
use tokio::sync::mpsc;
use tokio::task::JoinHandle;

fn corpus(n: usize) -> Vec<u8> {
    let w = ["alpha ", "beta ", "gamma ", "delta ", "epsilon "];
    let mut d = Vec::with_capacity(n + 16);
    let mut s: u64 = 7;
    while d.len() < n {
        s = s
            .wrapping_mul(6364136223846793005)
            .wrapping_add(1442695040888963407);
        d.extend_from_slice(w[((s >> 40) % 5) as usize].as_bytes());
    }
    d.truncate(n);
    d
}

fn gzip(data: &[u8]) -> Vec<u8> {
    // level 1, the counterpart of GZipStream's CompressionLevel.Fastest
    let mut e = GzEncoder::new(Vec::new(), Compression::fast());
    e.write_all(data).unwrap();
    e.finish().unwrap()
}

// A loopback server that sends `gz` to the first connection in 64 KiB writes, then closes
async fn serve(gz: Bytes) -> (u16, JoinHandle<()>) {
    let l = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let port = l.local_addr().unwrap().port();
    let t = tokio::spawn(async move {
        let (mut c, _) = l.accept().await.unwrap();
        c.set_nodelay(true).unwrap();
        let mut at = 0;
        while at < gz.len() {
            let n = (gz.len() - at).min(65536);
            c.write_all(&gz[at..at + n]).await.unwrap();
            at += n;
        }
    });
    (port, t)
}

async fn pipeline_run(gz: &Bytes, want: &[u8], file: &str) -> (u128, bool) {
    let sw = Instant::now();
    let (port, srv) = serve(gz.clone()).await;
    let mut client = TcpStream::connect(("127.0.0.1", port)).await.unwrap();
    // 4 chunks of 64 KiB in flight (the C# pipe pauses at 256 KiB)
    let (tx, mut rx) = mpsc::channel::<Bytes>(4);
    // stage 1: the socket into the channel
    let fill = tokio::spawn(async move {
        let mut buf = BytesMut::with_capacity(65536);
        loop {
            buf.reserve(65536);
            let n = client.read_buf(&mut buf).await.unwrap();
            if n == 0 {
                break;
            }
            if tx.send(buf.split().freeze()).await.is_err() {
                break;
            }
        }
    });
    // stage 2: inflate, hash, write
    let mut total = 0usize;
    let mut hash = Sha256::new();
    let mut f = tokio::fs::File::create(file).await.unwrap();
    let mut dec = GzDecoder::new(Vec::with_capacity(1 << 20));
    while let Some(chunk) = rx.recv().await {
        dec.write_all(&chunk).unwrap();
        let out = dec.get_mut();
        if !out.is_empty() {
            hash.update(&out[..]);
            f.write_all(&out[..]).await.unwrap();
            total += out.len();
            out.clear();
        }
    }
    dec.try_finish().unwrap();
    let out = dec.get_mut();
    if !out.is_empty() {
        hash.update(&out[..]);
        f.write_all(&out[..]).await.unwrap();
        total += out.len();
        out.clear();
    }
    f.flush().await.unwrap();
    drop(f);
    fill.await.unwrap();
    srv.await.unwrap();
    let digest = hash.finalize();
    let ms = sw.elapsed().as_millis();
    (ms, total == want.len() && digest.as_slice() == Sha256::digest(want).as_slice())
}

async fn adhoc_run(gz: &Bytes, want: &[u8], file: &str) -> (u128, bool) {
    let sw = Instant::now();
    let (port, srv) = serve(gz.clone()).await;
    let mut client = TcpStream::connect(("127.0.0.1", port)).await.unwrap();
    let mut body = Vec::new();
    client.read_to_end(&mut body).await.unwrap();
    let mut dec = GzDecoder::new(Vec::with_capacity(want.len()));
    dec.write_all(&body).unwrap();
    let data = dec.finish().unwrap();
    let digest = Sha256::digest(&data);
    tokio::fs::write(file, &data).await.unwrap();
    srv.await.unwrap();
    let ms = sw.elapsed().as_millis();
    (ms, data.len() == want.len() && digest.as_slice() == Sha256::digest(want).as_slice())
}

async fn rates(gz: &Bytes, data: &[u8], file: &str) {
    let mb = data.len() as f64 / 1e6;
    let sw = Instant::now();
    let mut dec = GzDecoder::new(Vec::with_capacity(data.len()));
    dec.write_all(gz).unwrap();
    let _ = dec.finish().unwrap();
    let e = sw.elapsed();
    println!("inflate alone      {} ms  {:.0} MB/s", e.as_millis(), mb / e.as_secs_f64());
    let sw = Instant::now();
    let _ = Sha256::digest(data);
    let e = sw.elapsed();
    println!("sha256 alone       {} ms  {:.0} MB/s", e.as_millis(), mb / e.as_secs_f64());
    let sw = Instant::now();
    tokio::fs::write(file, data).await.unwrap();
    let e = sw.elapsed();
    println!("file write alone   {} ms  {:.0} MB/s", e.as_millis(), mb / e.as_secs_f64());
    let sw = Instant::now();
    let (port, srv) = serve(gz.clone()).await;
    let mut client = TcpStream::connect(("127.0.0.1", port)).await.unwrap();
    let mut sink = vec![0u8; 65536];
    while client.read(&mut sink).await.unwrap() > 0 {}
    srv.await.unwrap();
    let e = sw.elapsed();
    println!(
        "loopback socket    {} ms  {:.0} MB/s (of gzip bytes)",
        e.as_millis(),
        (gz.len() as f64 / 1e6) / e.as_secs_f64()
    );
}

#[tokio::main]
async fn main() {
    let args: Vec<String> = std::env::args().collect();
    let mode = args.get(1).map(String::as_str).unwrap_or("pipeline");
    let mib: usize = args.get(2).map(|s| s.parse().unwrap()).unwrap_or(64);
    let runs: usize = args.get(3).map(|s| s.parse().unwrap()).unwrap_or(1);
    let data = corpus(mib * 1048576);
    if mode == "digest" {
        println!("{}", Sha256::digest(&data).iter().map(|b| format!("{:02x}", b)).collect::<String>());
        return;
    }
    let gzb = Bytes::from(gzip(&data));
    let file = "streamsample_rs.tmp";
    println!("{} MiB inflated, {:.1} MiB gzip", mib, gzb.len() as f64 / 1048576.0);
    for _ in 0..runs {
        if mode == "rates" {
            rates(&gzb, &data, file).await;
            continue;
        }
        let (ms, ok) = if mode == "adhoc" {
            adhoc_run(&gzb, &data, file).await
        } else {
            pipeline_run(&gzb, &data, file).await
        };
        println!(
            "{} {} MiB: {} ms, {:.0} MB/s {}",
            mode,
            mib,
            ms,
            data.len() as f64 / 1000.0 / ms as f64,
            if ok { "ok" } else { "WRONG" }
        );
    }
    let _ = std::fs::remove_file(file);
}
