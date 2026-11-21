# cellbench rows in one timed command (run it through loaded-run.ps1 -LrTimed; one command stays under about 60 minutes).
#   pwsh tools\cellbench\matrix.ps1 -Set A|B1|B5|Z|CR1|CR5|L [-Seconds 10]   (L: row 529's phase-lock rows)
# Each row is run.ps1's run; every summary is appended to build\cellbench-results-<set>.txt, which is the output
# (loaded-run does not pass any back).
param([string]$Set, [int]$Seconds = 10)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot\..\..").Path
$res = "$repo\build\cellbench-results-$Set.txt"
$rows = switch ($Set) {
  'A'   { foreach ($hz in 60, 90) { foreach ($f in 'm1', 'c', 'm2') { @{ f = $f; n = 8; p = 1; w = 4; cw = 2; b = 28; hz = $hz; tag = "A-$f-$hz" } } } }
  'B1'  { foreach ($n in 100, 1000) { foreach ($f in 'm1', 'm2', 'c') { @{ f = $f; n = $n; p = [int][math]::Ceiling($n / 625.0); w = 8; cw = 4; tag = "B$n-$f" } } } }
  'B5'  { foreach ($f in 'm1', 'm2', 'c') { @{ f = $f; n = 5000; p = 8; w = 8; cw = 4; tag = "B5000-$f" } } }
  'Z'   { foreach ($f in 'm1', 'c') { @{ f = $f; n = 1000; p = 2; w = 8; cw = 4; z = 2; tag = "Z2x500-$f" } } }
  'CR1' { foreach ($n in 100, 1000) { foreach ($f in 'm1', 'm2', 'c') { @{ f = $f; n = $n; p = [int][math]::Ceiling($n / 625.0); w = 8; cw = 4; crowd = $true; tag = "CR$n-$f" } } } }
  'CR5' { foreach ($f in 'm1', 'c') { @{ f = $f; n = 5000; p = 8; w = 8; cw = 4; crowd = $true; tag = "CR5000-$f" } } }
  # Row 529's check, the phase lock (form cl: c with followCell, 3 ms spread) against m1 in the same window, in the rows 528 listed:
  # A 60 Hz, B100, B1000, Z, CR1000 (the Linux 100 row is tools\cellbench\linux.sh, m1:100:1 cl:100:1)
  'L'   {
    foreach ($f in 'm1', 'c', 'cl', 'cl1') { @{ f = $f; n = 8; p = 1; w = 4; cw = 2; b = 28; hz = 60; tag = "A60-$f" } }
    foreach ($n in 100, 1000) { foreach ($f in 'm1', 'cl') { @{ f = $f; n = $n; p = [int][math]::Ceiling($n / 625.0); w = 8; cw = 4; tag = "B$n-$f" } } }
    foreach ($f in 'm1', 'cl') { @{ f = $f; n = 1000; p = 2; w = 8; cw = 4; z = 2; tag = "Z2x500-$f" } }
    foreach ($f in 'm1', 'cl') { @{ f = $f; n = 1000; p = 2; w = 8; cw = 4; crowd = $true; tag = "CR1000-$f" } }
  }
  default { throw "unknown set $Set" }
}
"" | Set-Content $res
foreach ($r in $rows) {
  $a = @{ Form = $r.f; N = $r.n; Procs = $r.p; W = $r.w; CW = $r.cw; Seconds = $Seconds; Tag = "$Set-$($r.tag)" }
  if ($r.z) { $a.Zones = $r.z }
  if ($r.b) { $a.Beings = $r.b }
  if ($r.hz) { $a.InputHz = $r.hz }
  if ($r.crowd) { $a.Crowd = $true }
  Add-Content $res "=== $Set $($r.tag) $(Get-Date -Format HH:mm:ss)"
  try { & "$PSScriptRoot\run.ps1" @a 2>&1 | Where-Object { $_ -notmatch '^SNAP' } | Add-Content $res } catch { Add-Content $res "FAILED: $_" }
}
Add-Content $res "=== done $(Get-Date -Format HH:mm:ss)"
