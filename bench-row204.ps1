# Row 204: swapped-pair bench of navmesh's path query (examples\bench.cone: "20 routes across the area ... ms").
# Usage: powershell -File bench-row204.ps1 -Master <master worktree> -Mine <this worktree> [-Rounds 5]
# Each round runs A B B A (A = master build, B = this branch) through `congo run --release` (never the .exe directly).
param([Parameter(Mandatory)]$Master, [Parameter(Mandatory)]$Mine, [int]$Rounds = 5)
$env:CONEC = "C:\src\cone\build\x64-release\conec.exe"
$env:CONE_DEPS = "C:\src\cone\deps"
$env:LIB = "C:\libs\SDL3-3.4.16\lib\x64;" + $env:LIB
$vc = '"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"'
function Run($tree) {
  Push-Location "$tree\packages\navmesh"
  $out = cmd /c "$vc >nul && python ..\..\tools\congo\congo.py run --release examples\bench.cone" 2>&1 | Out-String
  Pop-Location
  $m = [regex]::Match($out, '20 routes across the area \(\d+ complete\) in ([\d.]+) ms')
  $b = [regex]::Match($out, 'polygons in ([\d.]+) ms')
  if (-not $m.Success) { throw "no route line from $tree`n$out" }
  [pscustomobject]@{ Route = [double]$m.Groups[1].Value; Build = [double]$b.Groups[1].Value }
}
Run $Master | Out-Null; Run $Mine | Out-Null   # compile + warm up
$a = @(); $b = @()
for ($r = 1; $r -le $Rounds; $r++) {
  foreach ($w in 'A','B','B','A') {
    $x = if ($w -eq 'A') { Run $Master } else { Run $Mine }
    if ($w -eq 'A') { $a += $x } else { $b += $x }
  }
}
function Med($v) { $s = $v | Sort-Object; $s[[int]($s.Count / 2)] }
$ma = Med ($a.Route); $mb = Med ($b.Route)
"route query (20 routes), ms: master median {0:N2} (min {1:N2}); branch median {2:N2} (min {3:N2}); ratio branch/master {4:N3} (by min {5:N3})" -f $ma, ($a.Route | Measure-Object -Minimum).Minimum, $mb, ($b.Route | Measure-Object -Minimum).Minimum, ($mb / $ma), (($b.Route | Measure-Object -Minimum).Minimum / ($a.Route | Measure-Object -Minimum).Minimum)
"navmesh build, ms: master median {0:N1}; branch median {1:N1}" -f (Med $a.Build), (Med $b.Build)
"samples master: " + ($a.Route -join ' '); "samples branch: " + ($b.Route -join ' ')
