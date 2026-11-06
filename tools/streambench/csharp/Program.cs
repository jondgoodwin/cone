// The C# twin of packages/stageio/examples/streamsample.cone (streams PR 7): the same job
// on the same input, with System.IO.Pipelines. A gzip stream arrives over a loopback TCP
// socket; inflate it, take its sha256, and write the inflated bytes to a file.
//
//   dotnet run -c Release -- pipeline 128    socket -> Pipe -> inflate -> sha256 + file, as bytes arrive
//   dotnet run -c Release -- adhoc 128       read the whole body, inflate it in one call, hash, write
//   dotnet run -c Release -- rates 128       each stage alone: inflate, sha256, file write, loopback socket
//
// The data is the pseudo-random run of five words the Cone programs use, gzipped by
// GZipStream at the fastest level (a different compressor from Cone's level 2, so the gzip
// sizes differ a little; both print theirs).
using System.Diagnostics;
using System.IO.Compression;
using System.IO.Pipelines;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;

static byte[] Corpus(int n)
{
    string[] w = { "alpha ", "beta ", "gamma ", "delta ", "epsilon " };
    var d = new byte[n + 16];
    int len = 0;
    ulong s = 7;
    while (len < n)
    {
        s = s * 6364136223846793005UL + 1442695040888963407UL;
        var word = w[(int)((s >> 40) % 5)];
        foreach (char c in word) d[len++] = (byte)c;
    }
    Array.Resize(ref d, n);
    return d;
}

static byte[] Gzip(byte[] data)
{
    using var ms = new MemoryStream();
    using (var gz = new GZipStream(ms, CompressionLevel.Fastest, true)) gz.Write(data, 0, data.Length);
    return ms.ToArray();
}

// A loopback server that sends 'gz' to the first connection in 64 KiB writes, then closes
static (int port, Task done) Serve(byte[] gz)
{
    var l = new TcpListener(IPAddress.Loopback, 0);
    l.Start();
    int port = ((IPEndPoint)l.LocalEndpoint).Port;
    var t = Task.Run(async () =>
    {
        using var c = await l.AcceptTcpClientAsync();
        c.NoDelay = true;
        var s = c.GetStream();
        for (int at = 0; at < gz.Length; at += 65536)
            await s.WriteAsync(gz, at, Math.Min(65536, gz.Length - at));
        l.Stop();
    });
    return (port, t);
}

static async Task<(long ms, bool ok)> PipelineRun(byte[] gz, byte[] want, string file)
{
    var sw = Stopwatch.StartNew();
    var (port, srv) = Serve(gz);
    using var client = new TcpClient();
    await client.ConnectAsync(IPAddress.Loopback, port);
    var pipe = new Pipe(new PipeOptions(pauseWriterThreshold: 262144, resumeWriterThreshold: 131072));
    // stage 1: the socket into the pipe
    var fill = Task.Run(async () =>
    {
        var s = client.GetStream();
        while (true)
        {
            var mem = pipe.Writer.GetMemory(65536);
            int n = await s.ReadAsync(mem);
            if (n == 0) break;
            pipe.Writer.Advance(n);
            var r = await pipe.Writer.FlushAsync();
            if (r.IsCompleted) break;
        }
        await pipe.Writer.CompleteAsync();
    });
    // stage 2: inflate, hash, write
    long total = 0;
    var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
    using (var f = new FileStream(file, FileMode.Create, FileAccess.Write, FileShare.None, 65536, true))
    using (var gzs = new GZipStream(pipe.Reader.AsStream(), CompressionMode.Decompress))
    {
        var buf = new byte[65536];
        while (true)
        {
            int n = await gzs.ReadAsync(buf, 0, buf.Length);
            if (n == 0) break;
            hash.AppendData(buf, 0, n);
            await f.WriteAsync(buf, 0, n);
            total += n;
        }
    }
    await fill;
    await srv;
    var digest = hash.GetHashAndReset();
    sw.Stop();
    return (sw.ElapsedMilliseconds, total == want.Length && digest.AsSpan().SequenceEqual(SHA256.HashData(want)));
}

static async Task<(long ms, bool ok)> AdhocRun(byte[] gz, byte[] want, string file)
{
    var sw = Stopwatch.StartNew();
    var (port, srv) = Serve(gz);
    using var client = new TcpClient();
    await client.ConnectAsync(IPAddress.Loopback, port);
    var body = new MemoryStream();
    await client.GetStream().CopyToAsync(body, 65536);
    body.Position = 0;
    var outMs = new MemoryStream(want.Length);
    using (var gzs = new GZipStream(body, CompressionMode.Decompress)) await gzs.CopyToAsync(outMs);
    var data = outMs.ToArray();
    var digest = SHA256.HashData(data);
    await File.WriteAllBytesAsync(file, data);
    await srv;
    sw.Stop();
    return (sw.ElapsedMilliseconds, data.Length == want.Length && digest.AsSpan().SequenceEqual(SHA256.HashData(want)));
}

static async Task Rates(byte[] gz, byte[] data, string file)
{
    double mb = data.Length / 1e6;
    var sw = Stopwatch.StartNew();
    var o = new MemoryStream(data.Length);
    using (var g = new GZipStream(new MemoryStream(gz), CompressionMode.Decompress)) g.CopyTo(o);
    Console.WriteLine($"inflate alone      {sw.ElapsedMilliseconds} ms  {mb / sw.Elapsed.TotalSeconds:F0} MB/s");
    sw.Restart();
    SHA256.HashData(data);
    Console.WriteLine($"sha256 alone       {sw.ElapsedMilliseconds} ms  {mb / sw.Elapsed.TotalSeconds:F0} MB/s");
    sw.Restart();
    await File.WriteAllBytesAsync(file, data);
    Console.WriteLine($"file write alone   {sw.ElapsedMilliseconds} ms  {mb / sw.Elapsed.TotalSeconds:F0} MB/s");
    sw.Restart();
    var (port, srv) = Serve(gz);
    using var client = new TcpClient();
    await client.ConnectAsync(IPAddress.Loopback, port);
    await client.GetStream().CopyToAsync(Stream.Null, 65536);
    await srv;
    Console.WriteLine($"loopback socket    {sw.ElapsedMilliseconds} ms  {(gz.Length / 1e6) / sw.Elapsed.TotalSeconds:F0} MB/s (of gzip bytes)");
}

string mode = args.Length > 0 ? args[0] : "pipeline";
int mib = args.Length > 1 ? int.Parse(args[1]) : 64;
int runs = args.Length > 2 ? int.Parse(args[2]) : 1;
var data = Corpus(mib * 1048576);
var gzb = Gzip(data);
string file = "streamsample_cs.tmp";
Console.WriteLine($"{mib} MiB inflated, {gzb.Length / 1048576.0:F1} MiB gzip");
for (int i = 0; i < runs; i++)
{
    if (mode == "rates") { await Rates(gzb, data, file); continue; }
    var (ms, ok) = mode == "adhoc" ? await AdhocRun(gzb, data, file) : await PipelineRun(gzb, data, file);
    Console.WriteLine($"{mode} {mib} MiB: {ms} ms, {data.Length / 1000.0 / ms:F0} MB/s {(ok ? "ok" : "WRONG")}");
}
File.Delete(file);
