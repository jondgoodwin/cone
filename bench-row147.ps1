# bench-row147.ps1: swapped-pair timing, master against the row-147 branch (order A B B A, then B A A B, ...).
# Rows: zstd compress (gen:8, level 3, MB/s of input, best of 5 inside the program), zstd decompress (same data,
# MB/s of output, best of 3), http3 qpack bench (ns a list, four configurations; lower is better).
#
# It runs prebuilt release exes from $Dir; the builds are done by $Dir\build.ps1 <base|branch> (zb.cone) and
# $Dir\buildq.ps1 <base|branch> (http3 examples\bench.cone), each copying its exe to $Dir as zb-<side>.exe / qb-<side>.exe.
# $Dir also holds frame.zst (gen:8 compressed at level 3). 'base' is a detached worktree at origin/master 0f2586653,
# 'branch' is this worktree; both are built with the branch's conec.
# Run it when the machine is quiet (check CPU first); it leaves no process behind.
param([string]$Dir = 'C:\src\zz-lane285', [int]$Rounds = 6)
Set-Location $Dir
$cz = @{base=@(); branch=@()}; $dz = @{base=@(); branch=@()}
$q = @{}; foreach ($k in 'base','branch') { $q[$k] = @{} }
function Cpu { (Get-CimInstance Win32_Processor | Measure-Object LoadPercentage -Average).Average }
$loads = @()
for ($r = 0; $r -lt $Rounds; $r++) {
  $order = if ($r % 2 -eq 0) { 'base','branch','branch','base' } else { 'branch','base','base','branch' }
  foreach ($s in $order) {
    $loads += Cpu
    $o = & ".\zb-$s.exe" gen:8 frame.zst 2>&1 | Out-String
    if ($o -match 'comp bytes \d+ best_ms \d+ MBps (\d+)') { $cz[$s] += [int]$Matches[1] }
    if ($o -match 'decomp frame \d+ best_ms \d+ MBps (\d+) sizeok true') { $dz[$s] += [int]$Matches[1] }
    $o = & ".\qb-$s.exe" 2>&1 | Out-String
    $i = 0
    foreach ($m in [regex]::Matches($o, '(\d+) ns a list')) { $q[$s]["q$i"] += @([int]$m.Groups[1].Value); $i++ }
  }
}
function Med($a) { $s = $a | Sort-Object; $n = $s.Count; if ($n % 2) { $s[($n-1)/2] } else { ($s[$n/2-1] + $s[$n/2]) / 2 } }
"CPU load before each run: mean {0:N0}%, max {1}%" -f ($loads | Measure-Object -Average).Average, ($loads | Measure-Object -Maximum).Maximum
foreach ($row in @(@('zstd compress MB/s (higher better)', $cz), @('zstd decompress MB/s (higher better)', $dz))) {
  $b = Med $row[1].base; $a = Med $row[1].branch
  "{0}: master median {1} [{2}]  branch median {3} [{4}]  branch/master {5:N3}" -f $row[0], $b, ($row[1].base -join ','), $a, ($row[1].branch -join ','), ($a / $b)
}
foreach ($k in ($q.base.Keys | Sort-Object)) {
  $b = Med $q.base[$k]; $a = Med $q.branch[$k]
  "qpack {0} ns a list (lower better): master median {1}  branch median {2}  branch/master {3:N3}" -f $k, $b, $a, ($a / $b)
}
