# streambench, Rust twin

The Rust counterpart of `tools/streambench/csharp` and of `packages/stageio/examples/streamsample.cone` /
`streamrates.cone` (streams PR 7): a gzip stream arrives over a loopback TCP socket; inflate it, take its sha256,
write the inflated bytes to a file. Same corpus (the LCG run of five words), same gzip at level 1, 64 KiB chunks,
same output lines as the C# twin.

    $env:CARGO_TARGET_DIR = "C:\src\zz-rusttwin-target"    # keep target/ off MyDrive and out of the repo
    cargo run --release -- pipeline 128 [runs]   # socket -> bounded channel (4 x 64 KiB) -> inflate -> sha256 + file, as bytes arrive
    cargo run --release -- adhoc 128 [runs]      # whole body read, one-call inflate, hash, write
    cargo run --release -- rates 128 [runs]      # inflate, sha256, file write, loopback socket, each alone
    cargo run --release -- digest 128            # hex sha256 of the inflated data, to compare across twins

Shape: Tokio multi-thread runtime; a server task writes the gzip bytes in 64 KiB writes; a fill task reads the
socket into `BytesMut` and sends frozen `Bytes` through a bounded `mpsc` channel (the C# pipe's 256 KiB window);
one task inflates (`flate2::write::GzDecoder`), updates the `sha2::Sha256`, and writes the file with `tokio::fs`.

## Back ends (what the rates rows separate from the plumbing)
Crates are the ordinary ones, current as of 3 Nov 2026, from crates.io with caret versions (`Cargo.lock` is committed;
the bench suite in `C:\src\zz-bench-suite` pins exact versions in Cargo.toml and takes crates from crates.io the same way).
- inflate/deflate: `flate2` 1.1.10 with its default back end, **miniz_oxide 0.9.1** (pure Rust, with its `simd-adler32`
  checksum). Not zlib or zlib-ng; `flate2`'s `zlib-rs` feature would swap in a faster inflater.
- sha256: `sha2` 0.11.1, which detects the x86 **SHA instructions at run time** (via `cpufeatures`) and uses them
  when the CPU has them (the HX-series laptop does); otherwise it falls back to a software implementation.
- C# uses native zlib and SHA instructions, so the codec gap to C# is largely miniz_oxide against zlib.

The gzip sizes differ between all three because each compresses with its own encoder; each prints its own.
