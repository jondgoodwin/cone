# netbench run (row 522): one netmsg server process and -Procs client processes on this machine, over loopback.
# The clients' connections are split evenly among the processes. At the server's RUNSTART and RUNEND (the window of
# the sends) it samples the CPU of the server and of every client process, per thread, and prints one table: the CPU
# of each process (total, as a percent of one core, and its busiest thread, the one that saturates first), and the
# updates each client process heard against what it should have.
#   powershell tools\netbench\run.ps1 -N 5000 -Procs 4 [-W 4] [-CW 4] [-Seconds 10] [-Bytes 1171] [-Port 47400]
# CPU readings are approximate when the machine is shared. Needs CONEC / CONESTD of a built compiler (default: the
# main checkout's release build). Logs go to build\netbench-<tag>\.
param([int]$N = 1000, [int]$Procs = 2, [int]$W = 4, [int]$CW = 4, [int]$Seconds = 10, [int]$Bytes = 1171,
      [int]$Port = 47400, [string]$Tag = "", [switch]$Build)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot\..\..").Path
if (-not $Tag) { $Tag = "n$N-p$Procs-" + (Get-Date -Format HHmm) }
$out = "$repo\build\netbench-$Tag"
New-Item -ItemType Directory -Force $out | Out-Null
$vcvars = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'
$env:CONE_DEPS = 'C:\src\cone\deps'
$env:LIB = "C:\libs\SDL3-3.4.16\lib\x64;$($env:CONE_DEPS)\openssl;$($env:CONE_DEPS)\ngtcp2;" + $env:LIB
if (-not $env:CONEC) { $env:CONEC = 'C:\src\cone\build\x64-release\conec.exe' }
if (-not $env:CONESTD) { $env:CONESTD = 'C:\src\cone\build\x64-release\conestd.lib' }
$congo = "$repo\tools\congo\congo.py"
$src = "$repo\packages\netmsg\examples\netbench.cone"
# Each process runs its own copy of the file: congo names its build folder after the file's path, and a second
# process linking over the exe of a running one fails (Access is denied)
function Start-Congo([string]$a, [string]$log, [string]$role = 'srv') {
  New-Item -ItemType Directory -Force "$out\$role" | Out-Null
  Copy-Item $src "$out\$role\netbench.cone" -Force
  $prog = "$out\$role\netbench.cone"
  $line = "`"$vcvars`" >nul && python `"$congo`" run --release `"$prog`" -- $a"
  Start-Process -FilePath cmd -ArgumentList '/c', "`"$line`"" -WorkingDirectory "$repo\packages" -RedirectStandardOutput $log -RedirectStandardError "$log.err" -PassThru -WindowStyle Hidden
}
function Find-Exe([int]$rootId) {
  for ($i = 0; $i -lt 600; $i++) {
    $all = Get-CimInstance Win32_Process
    $ids = @($rootId)
    for ($round = 0; $round -lt 6; $round++) {
      $more = $all | Where-Object { $ids -contains $_.ParentProcessId -and $ids -notcontains $_.ProcessId }
      if (-not $more) { break }
      $ids += $more.ProcessId
    }
    $p = $all | Where-Object { $ids -contains $_.ProcessId -and $_.Name -eq 'netbench.exe' } | Select-Object -First 1
    if ($p) { return [int]$p.ProcessId }
    Start-Sleep -Milliseconds 100
  }
  throw "no netbench.exe under $rootId"
}
if ($Build) { $b = Start-Congo '' "$out\build.log"; $b.WaitForExit(); Get-Content "$out\build.log" | Select-Object -Last 5; return }

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
$s = Start-Congo "server $Port $N $Seconds $Bytes $W" $srvLog 'srv'
$srvPid = Find-Exe $s.Id
for ($i = 0; $i -lt 400; $i++) { if ((ReadLog $srvLog) -match 'LISTENING') { break }; Start-Sleep -Milliseconds 250 }
Start-Sleep 1
$cli = @()
$left = $N
for ($k = 0; $k -lt $Procs; $k++) {
  $share = [int][math]::Floor($left / ($Procs - $k)); $left -= $share
  $log = "$out\cli$k.log"
  $c = Start-Congo "clients $Port $share $Seconds $CW" $log "cli$k"
  $cli += [pscustomobject]@{ k = $k; share = $share; log = $log; proc = $c; pid = 0 }
}
# every client process is started before any is waited for, so they compile side by side (lane 941)
foreach ($c in $cli) { $c.pid = Find-Exe $c.proc.Id }
$ids = @($srvPid) + @($cli | ForEach-Object { $_.pid })
$a = @{}; $b = @{}; $tA = $null; $tB = $null
$deadline = (Get-Date).AddSeconds(240 + 4 * $Seconds)
while ((Get-Date) -lt $deadline) {
  if (-not (Get-Process -Id $srvPid -ErrorAction SilentlyContinue)) { break }
  $txt = ReadLog $srvLog
  if (-not $tA -and $txt -match 'RUNSTART') { foreach ($i in $ids) { $a[$i] = Snap $i }; $tA = Get-Date }
  if ($tA -and -not $tB -and $txt -match 'RUNEND') { foreach ($i in $ids) { $b[$i] = Snap $i }; $tB = Get-Date }
  Start-Sleep -Milliseconds 50
}
$s.WaitForExit(30000) | Out-Null
foreach ($c in $cli) { $c.proc.WaitForExit(90000) | Out-Null }
$win = if ($tA -and $tB) { ($tB - $tA).TotalMilliseconds } else { 0 }
function Cpu([int]$id, [string]$name, [string]$extra) {
  if (-not $a[$id] -or -not $b[$id] -or $win -le 0) { return "{0,-9} no sample" -f $name }
  $rows = foreach ($t in $b[$id]) { $t0 = $a[$id] | Where-Object { $_.id -eq $t.id }; $d0 = if ($t0) { $t0.cpu } else { 0 }; $t.cpu - $d0 }
  $tot = ($rows | Measure-Object -Sum).Sum; $mx = ($rows | Measure-Object -Maximum).Maximum
  "{0,-9} total {1,5:N0}% of a core  busiest thread {2,4:N0}%  {3}" -f $name, (100 * $tot / $win), (100 * $mx / $win), $extra
}
"netbench N=$N procs=$Procs serverWorkers=$W clientWorkers=$CW seconds=$Seconds bytes=$Bytes  window=$([math]::Round($win))ms  machine idle before=$([math]::Round($idle,1))% (CPU readings approximate: shared machine)"
Cpu $srvPid 'server' ''
$sent = 0; $heard = 0
foreach ($c in $cli) {
  $t = ReadLog $c.log
  $m = [regex]::Match($t, 'CLIENTS updates heard=(\d+) expected~=(\d+)')
  $h = if ($m.Success) { [int64]$m.Groups[1].Value } else { 0 }
  $e = [int64]$c.share * $Seconds * 30
  $sent += $e; $heard += $h
  Cpu $c.pid ("client$($c.k)") ("conns {0}  heard {1} of ~{2} ({3:N1}%)" -f $c.share, $h, $e, (100.0 * $h / [math]::Max($e, 1)))
}
"all clients: heard $heard of ~$sent ({0:N1}%)" -f (100.0 * $heard / [math]::Max($sent, 1))
(ReadLog $srvLog) -split "`n" | Where-Object { $_ -match '^(SERVER|JOINED|IOTIMES)' }
