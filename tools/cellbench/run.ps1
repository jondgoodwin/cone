# cellbench run (row 528): one server process and -Procs client processes on this machine, over loopback, one table.
# At the server's RUNSTART and RUNEND (the window the world publishes) it samples the CPU of the server and of every client
# process, per thread, the machine's own load over the window, and prints one summary (also written to build\cellbench-<tag>\summary.txt,
# because loaded-run.ps1 does not pass output back).
#   pwsh tools\cellbench\run.ps1 -Form c -N 1000 -Procs 2 [-W 8] [-CW 4] [-Seconds 10] [-Zones 1] [-Beings 8] [-Crowd] [-InputHz 30] [-Port 47400] [-Tag x]
# Needs CONEC / CONESTD of a built compiler (default: the main checkout's release build). Logs go to build\cellbench-<tag>\.
param([string]$Form = 'c', [int]$N = 1000, [int]$Procs = 2, [int]$W = 8, [int]$CW = 4, [int]$Seconds = 10,
      [int]$Zones = 1, [int]$Beings = 8, [switch]$Crowd, [int]$InputHz = 30, [int]$Port = 47400, [string]$Tag = "", [switch]$Memstats, [int]$Rate = 0)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot\..\..").Path
$cr = if ($Crowd) { 1 } else { 0 }
if (-not $Tag) { $Tag = "$Form-n$N-z$Zones-c$cr-" + (Get-Date -Format HHmmss) }
$out = "$repo\build\cellbench-$Tag"
New-Item -ItemType Directory -Force $out | Out-Null
$vcvars = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'
$env:CONE_DEPS = 'C:\src\cone\deps'
$env:LIB = "C:\libs\SDL3-3.4.16\lib\x64;$($env:CONE_DEPS)\openssl;$($env:CONE_DEPS)\ngtcp2;" + $env:LIB
if (-not $env:CONEC) { $env:CONEC = 'C:\src\cone\build\x64-release\conec.exe' }
if (-not $env:CONESTD) { $env:CONESTD = 'C:\src\cone\build\x64-release\conestd.lib' }
$congo = "$repo\tools\congo\congo.py"
$src = "$repo\packages\netmsg\examples\cellbench.cone"
function Start-Congo([string]$a, [string]$log, [string]$role = 'srv') {
  New-Item -ItemType Directory -Force "$out\$role" | Out-Null
  Copy-Item $src "$out\$role\cellbench.cone" -Force
  $prog = "$out\$role\cellbench.cone"
  $line = "`"$vcvars`" >nul && python `"$congo`" run --release `"$prog`" -- $a"
  if ($Memstats -and $role -eq 'srv') {
    # the server's allocations, counted by packages\memstats (never timed: a tag changes the layout and the scheduling)
    $lib = "$repo\packages\memstats\shim\out\memstats.lib"
    if (-not (Test-Path $lib)) { & "$repo\packages\memstats\shim\build.ps1" }
    New-Item -ItemType Directory -Force "$out\ms" | Out-Null
    $line = "`"$vcvars`" >nul && set _LINK_=`"$lib`"&& set MEMSTATS_OUT=$out\ms&& python `"$congo`" run -D MEMSTATS --release `"$prog`" -- $a"
  }
  Start-Process -FilePath cmd -ArgumentList '/c', "`"$line`"" -WorkingDirectory "$repo\packages" -RedirectStandardOutput $log -RedirectStandardError "$log.err" -PassThru -WindowStyle Hidden
}
function Find-Exe([int]$rootId) {
  for ($i = 0; $i -lt 1200; $i++) {
    $all = Get-CimInstance Win32_Process
    $ids = @($rootId)
    for ($round = 0; $round -lt 6; $round++) {
      $more = $all | Where-Object { $ids -contains $_.ParentProcessId -and $ids -notcontains $_.ProcessId }
      if (-not $more) { break }
      $ids += $more.ProcessId
    }
    $p = $all | Where-Object { $ids -contains $_.ProcessId -and $_.Name -eq 'cellbench.exe' } | Select-Object -First 1
    if ($p) { return [int]$p.ProcessId }
    Start-Sleep -Milliseconds 100
  }
  throw "no cellbench.exe under $rootId"
}
function Snap([int]$id) {
  $pr = Get-Process -Id $id -ErrorAction SilentlyContinue
  if (-not $pr) { return @() }
  @($pr.Threads | ForEach-Object { [pscustomobject]@{ id = $_.Id; cpu = $_.TotalProcessorTime.TotalMilliseconds } })
}
function ReadLog([string]$f) {
  try { $fs = [IO.File]::Open($f, 'Open', 'Read', 'ReadWrite,Delete'); $sr = New-Object IO.StreamReader($fs); $t = $sr.ReadToEnd(); $sr.Close(); $t } catch { '' }
}
$idle = (Get-Counter '\Processor(_Total)\% Idle Time' -SampleInterval 1 -MaxSamples 3 | ForEach-Object { $_.CounterSamples[0].CookedValue } | Measure-Object -Average).Average
$srvLog = "$out\srv.log"
$common = "form=$Form secs=$Seconds beings=$Beings crowd=$cr players=$N"
$s = Start-Congo "server $Port - $common workers=$W zones=$Zones rate=$Rate" $srvLog 'srv'
$srvPid = Find-Exe $s.Id
for ($i = 0; $i -lt 1200; $i++) { if ((ReadLog $srvLog) -match 'LISTENING') { break }; Start-Sleep -Milliseconds 250 }
Start-Sleep 1
$cli = @()
$left = $N
for ($k = 0; $k -lt $Procs; $k++) {
  $share = [int][math]::Floor($left / ($Procs - $k)); $left -= $share
  $log = "$out\cli$k.log"
  $c = Start-Congo "clients $Port - $common n=$share workers=$CW inputhz=$InputHz" $log "cli$k"
  $cli += [pscustomobject]@{ k = $k; share = $share; log = $log; proc = $c; pid = (Find-Exe $c.Id) }
}
$ids = @($srvPid) + @($cli | ForEach-Object { $_.pid })
$a = @{}; $b = @{}; $tA = $null; $tB = $null; $load = $null
$pc = New-Object System.Diagnostics.PerformanceCounter('Processor', '% Processor Time', '_Total'); [void]$pc.NextValue()
$deadline = (Get-Date).AddSeconds(900 + 4 * $Seconds)
while ((Get-Date) -lt $deadline) {
  if (-not (Get-Process -Id $srvPid -ErrorAction SilentlyContinue)) { break }
  $txt = ReadLog $srvLog
  if (-not $tA -and $txt -match 'RUNSTART') { foreach ($i in $ids) { $a[$i] = Snap $i }; $tA = Get-Date; [void]$pc.NextValue() }
  if ($tA -and -not $tB -and $txt -match 'RUNEND') { $load = $pc.NextValue(); foreach ($i in $ids) { $b[$i] = Snap $i }; $tB = Get-Date }
  Start-Sleep -Milliseconds 50
}
$s.WaitForExit(60000) | Out-Null
foreach ($c in $cli) { $c.proc.WaitForExit(120000) | Out-Null }
$win = if ($tA -and $tB) { ($tB - $tA).TotalMilliseconds } else { 0 }
function Cpu([int]$id, [string]$name, [string]$extra) {
  if (-not $a[$id] -or -not $b[$id] -or $win -le 0) { return "{0,-9} no sample" -f $name }
  $rows = foreach ($t in $b[$id]) { $t0 = $a[$id] | Where-Object { $_.id -eq $t.id }; $d0 = if ($t0) { $t0.cpu } else { 0 }; $t.cpu - $d0 }
  $tot = ($rows | Measure-Object -Sum).Sum; $mx = ($rows | Measure-Object -Maximum).Maximum
  "{0,-9} total {1,5:N0}% of a core  busiest thread {2,4:N0}%  {3}" -f $name, (100 * $tot / $win), (100 * $mx / $win), $extra
}
$lines = @()
$lines += "cellbench form=$Form N=$N zones=$Zones beings=$Beings crowd=$cr procs=$Procs serverWorkers=$W clientWorkers=$CW seconds=$Seconds inputHz=$InputHz  window=$([math]::Round($win))ms"
$lines += "machine load: idle before=$([math]::Round($idle,1))%  busy during the window=$([math]::Round($load,1))% (all cores, $([Environment]::ProcessorCount) logical)"
$lines += (Cpu $srvPid 'server' '')
$heardAll = 0; $expAll = 0
foreach ($c in $cli) {
  $t = ReadLog $c.log
  $m = [regex]::Match($t, 'CLIENTS own-region updates heard=(\d+) expected=(\d+)')
  $h = if ($m.Success) { [int64]$m.Groups[1].Value } else { 0 }
  $e = if ($m.Success) { [int64]$m.Groups[2].Value } else { 0 }
  $heardAll += $h; $expAll += $e
  $lines += (Cpu $c.pid ("client$($c.k)") ("conns {0}  own-region heard {1} of {2} ({3:N1}%)" -f $c.share, $h, $e, (100.0 * $h / [math]::Max($e, 1))))
  $lines += ((ReadLog $c.log) -split "`n" | Where-Object { $_ -match '^CLIENTS own-region state age' } | ForEach-Object { "  client$($c.k) $_" })
}
$lines += "all clients: own-region updates heard $heardAll of $expAll ({0:N1}%)" -f (100.0 * $heardAll / [math]::Max($expAll, 1))
# the age over every client process: the histograms merged (100 us buckets; a percentile is its bucket's upper edge)
$hist = @{}
foreach ($c in $cli) {
  $h = (ReadLog $c.log) -split "`n" | Where-Object { $_ -match '^HIST' } | Select-Object -First 1
  if ($h) { foreach ($m in [regex]::Matches($h, '(\d+):(\d+)')) { $k = [int]$m.Groups[1].Value; $hist[$k] = [int64]($hist[$k]) + [int64]$m.Groups[2].Value } }
}
$n = ($hist.Values | Measure-Object -Sum).Sum
function Pct([int]$q) { if (-not $n) { return 0 }; $want = [math]::Ceiling($n * $q / 100.0); $run = 0; foreach ($k in ($hist.Keys | Sort-Object)) { $run += $hist[$k]; if ($run -ge $want) { return ($k + 1) * 100 } }; return 0 }
$lines += "AGE own-region state age, all clients: n=$n p50_us=$(Pct 50) p99_us=$(Pct 99) max_us=$(Pct 100)"
$lines += ((ReadLog $srvLog) -split "`n" | Where-Object { $_ -match '^(WORLD|JOINED|IOTIMES|SNAP|ACTORSTATS)' })
$lines | Tee-Object -FilePath "$out\summary.txt"
