# Row 207 swapped-pair timing: A B B A, 7 rounds, per configuration. A = master build, B = this branch.
# Usage: bench-row207.ps1 -Dir <folder holding zb-before.exe zb-after.exe zb-beforep.exe zb-afterp.exe, bin32.dat>
# zb.exe is the in-process harness (best of 5 compressions, prints "best_ms N MBps M"); the p exes are the same builds
# with Instant counters around the literal-stream encoder, which print "ES <ns>" once per compression.
# Waits (30 s steps, up to 30 min) while quiet-hold.flag exists; samples the system CPU load around each run.
param([string]$Dir = "C:\src\zz-lane288", [int]$Rounds = 7)
$flag = "C:\src\zz-cone-module-tools\quiet-hold.flag"
$w = 0
while ((Test-Path $flag) -and $w -lt 60) { Start-Sleep -Seconds 30; $w++ }
"hold waited $($w * 30) s; flag now present: $(Test-Path $flag)"
Set-Location $Dir
function Load { (Get-Counter '\Processor(_Total)\% Processor Time' -SampleInterval 1 -MaxSamples 1).CounterSamples[0].CookedValue }
function Run($exe, $in, $lvl) {
  $l0 = Load
  $o = & ".\$exe" $in nofile c $lvl 2>&1 | Out-String
  $l1 = Load
  $mbps = [int]([regex]::Match($o, 'MBps (\d+)').Groups[1].Value)
  $es = [regex]::Matches($o, 'ES (\d+) ns') | ForEach-Object { [double]$_.Groups[1].Value / 1e6 }
  $esMin = if ($es) { ($es | Measure-Object -Minimum).Minimum } else { 0 }
  [pscustomobject]@{ mbps = $mbps; es = $esMin; load = ($l0 + $l1) / 2 }
}
function Med($a) { $s = $a | Sort-Object; $s[[int][math]::Floor(($s.Count - 1) / 2)] }
$cfgs = @(
  @{ name = "binary L1"; a = "zb-before.exe"; b = "zb-after.exe"; in = "bin32.dat"; l = 1 },
  @{ name = "binary L3"; a = "zb-before.exe"; b = "zb-after.exe"; in = "bin32.dat"; l = 3 },
  @{ name = "text L1";   a = "zb-before.exe"; b = "zb-after.exe"; in = "gen:100"; l = 1 },
  @{ name = "text L3";   a = "zb-before.exe"; b = "zb-after.exe"; in = "gen:100"; l = 3 },
  @{ name = "binary L1 encodeStream"; a = "zb-beforep.exe"; b = "zb-afterp.exe"; in = "bin32.dat"; l = 1 }
)
foreach ($c in $cfgs) {
  $A = @(); $B = @(); $LA = @(); $LB = @()
  for ($r = 0; $r -lt $Rounds; $r++) {
    foreach ($side in 'A', 'B', 'B', 'A') {
      $exe = if ($side -eq 'A') { $c.a } else { $c.b }
      $x = Run $exe $c.in $c.l
      $v = if ($c.name -like '*encodeStream') { $x.es } else { $x.mbps }
      if ($side -eq 'A') { $A += $v; $LA += $x.load } else { $B += $v; $LB += $x.load }
    }
  }
  $unit = if ($c.name -like '*encodeStream') { "ms" } else { "MB/s" }
  "{0}: A median {1} {2} [{3}-{4}]  B median {5} [{6}-{7}]  B/A {8:N3}  load mean {9:N0}% ({10:N0}-{11:N0})" -f $c.name, (Med $A), $unit, ($A | Measure-Object -Minimum).Minimum, ($A | Measure-Object -Maximum).Maximum, (Med $B), ($B | Measure-Object -Minimum).Minimum, ($B | Measure-Object -Maximum).Maximum, ((Med $B) / (Med $A)), (($LA + $LB) | Measure-Object -Average).Average, (($LA + $LB) | Measure-Object -Minimum).Minimum, (($LA + $LB) | Measure-Object -Maximum).Maximum
}
