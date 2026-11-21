# run.ps1: the streaming scenario (row 429, work item D2) from one command.
#
#   run.ps1 [-Alloc crt|mi|sn|cn] [-Instrument] [-Routed] [-Conns 300] [-Sustain 120] [-Idle 30] [-Burst 60] [-Seed 429]
#           [-Mix 70,10,10,10] [-Workers 4] [-Think 100] [-Out <dir>]
#
# A server (server.cone, actorhttp) and a load client (client.cone, httpclient) as two processes of their own, so
# the server's memory is its own. The client runs the mix (small keep-alive request pairs, an event flood, large
# uploads, large downloads; -Mix is the shares of the connections, in that order) across -Conns connections:
# -Sustain seconds of load, -Idle seconds of rest, -Burst seconds of load again. At each change the client calls the
# server's /_phase, which prints the mark with the time ("PHASE name=... ms") and, once row 424's memory.phase trace
# kind lands, emits it. This script reads the client's MARK lines as they come and samples the server process from
# outside at each one: working set, commit (private bytes), page faults, and the server's and client's CPU seconds.
# The measures come out as one JSON line at the end ("STREAMLOAD {...}"), and the same in <Out>\result.json.
# The defaults are the full scenario. Fewer connections and shorter phases (-Conns 60 -Sustain 20 -Idle 12 -Burst 10)
# suit a loaded machine; a run on one is marked "loaded" in the JSON when the machine's CPU was over 25% on average.
#
# -Routed serves through a router's queue route (the routed server: connections are actors) instead of the I/O
# thread's server; the result then says routed=true. Either way the JSON carries ioThreadOwnCpu from the server's
# IOTIMES line: the I/O thread's own CPU (user + kernel over its life), not all the CPU spent on I/O.
#
# -Alloc swaps the server's allocator at the link (row 428's libraries in C:\src\zz-cnmalloc\alloc; cn waits for row 425). -Instrument is accepted and
# recorded, and the files' path is null until row 424's tables exist; an instrumented run is never timed.
#
# It runs the programs with congo (which sets the error mode, so a missing DLL prints a line instead of a dialog).
param(
  [ValidateSet('crt', 'mi', 'sn', 'cn')][string]$Alloc = 'crt',
  [switch]$Instrument,
  [switch]$Routed,
  [int]$Conns = 300,
  [int]$Sustain = 120,
  [int]$Idle = 30,
  [int]$Burst = 60,
  [int]$Seed = 429,
  [string]$Mix = '70,10,10,10',
  [int]$Workers = 4,
  [int]$Think = 100,
  [string]$Out = ''
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = (Resolve-Path (Join-Path $here '..\..\..\..')).Path
$congo = Join-Path $repo 'tools\congo\congo.py'
$vcvars = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'
# the swap (row 428/420): the allocator's library goes on the SERVER's link line through link.exe's _LINK_ variable
# (C:\src\zz-cnmalloc\link-shim.ps1's mechanism), the server built from a fresh copy of server.cone so no earlier
# link is reused; the client always stays on the C runtime. Checked with imports.ps1 in row 428's note.
$allocLib = "C:\src\zz-cnmalloc\alloc\$Alloc.lib"
$shares = $Mix -split ',' | ForEach-Object { [int]$_ }
if ($shares.Count -ne 4 -or ($shares | Measure-Object -Sum).Sum -ne 100) { throw 'run.ps1: -Mix is four shares that sum to 100' }
if (-not $Out) { $Out = Join-Path $env:TEMP ('streamload-' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
New-Item -ItemType Directory -Force $Out | Out-Null
$srvDir = $here
if ($Alloc -ne 'crt') {
  if (-not (Test-Path $allocLib)) { throw "run.ps1: no $allocLib for -Alloc $Alloc (cn waits for row 425)" }
  $srvDir = Join-Path $Out "server-$Alloc"
  New-Item -ItemType Directory -Force $srvDir | Out-Null
  Copy-Item (Join-Path $here 'server.cone') $srvDir -Force
}

if (-not $env:CONE_DEPS) { $env:CONE_DEPS = 'C:\src\cone\deps' }
$env:LIB = "C:\libs\SDL3-3.4.16\lib\x64;$($env:CONE_DEPS)\openssl;$($env:CONE_DEPS)\ngtcp2;" + $env:LIB
if (-not $env:CONEC -and -not (Test-Path (Join-Path $repo 'build\x64-release\conec.exe'))) {
  $env:CONEC = 'C:\src\cone\build\x64-release\conec.exe'
  $env:CONESTD = 'C:\src\cone\build\x64-release\conestd.lib'
}

# a congo run of one program of this folder, as a process tree of its own; returns the cmd process
function Start-Congo([string]$prog, [string]$progArgs, [string]$log, [string]$dir = $here) {
  $line = "`"$vcvars`" >nul && python `"$congo`" run --release `"$dir\$prog`" -- $progArgs"
  Start-Process -FilePath cmd -ArgumentList '/c', "`"$line`"" -WorkingDirectory (Join-Path $repo 'packages') `
    -RedirectStandardOutput $log -RedirectStandardError "$log.err" -PassThru -WindowStyle Hidden
}

# the descendants of a process, by name
function Find-Child([int]$rootId, [string]$name) {
  $all = Get-CimInstance Win32_Process
  $ids = @($rootId)
  for ($round = 0; $round -lt 6; $round++) {
    $more = $all | Where-Object { $ids -contains $_.ParentProcessId -and $ids -notcontains $_.ProcessId }
    if (-not $more) { break }
    $ids += $more.ProcessId
  }
  $all | Where-Object { $ids -contains $_.ProcessId -and $_.Name -eq $name } | Select-Object -First 1
}

function Machine-Cpu { [int](Get-CimInstance Win32_Processor | Measure-Object -Property LoadPercentage -Average).Average }

function Sample([int]$procId, [int]$clientId, [string]$name, [datetime]$t0) {
  $p = Get-Process -Id $procId -ErrorAction SilentlyContinue
  if (-not $p) { return $null }
  $cim = Get-CimInstance Win32_Process -Filter "ProcessId=$procId"
  $c = if ($clientId) { Get-Process -Id $clientId -ErrorAction SilentlyContinue } else { $null }
  [ordered]@{
    mark            = $name
    atS             = [math]::Round(((Get-Date) - $t0).TotalSeconds, 1)
    workingSetMB    = [math]::Round($p.WorkingSet64 / 1MB, 1)
    commitMB        = [math]::Round($p.PrivateMemorySize64 / 1MB, 1)
    pageFaults      = [int64]$cim.PageFaults
    serverCpuSec    = [math]::Round($p.TotalProcessorTime.TotalSeconds, 1)
    clientCpuSec    = if ($c) { [math]::Round($c.TotalProcessorTime.TotalSeconds, 1) } else { $null }
    machineCpuPct   = Machine-Cpu
  }
}

# build both once, apart from the run (each prints its usage and ends)
foreach ($prog in 'server.cone', 'client.cone') {
  if ($prog -eq 'server.cone' -and $Alloc -ne 'crt') { $env:_LINK_ = "`"$allocLib`"" } else { Remove-Item Env:_LINK_ -ErrorAction SilentlyContinue }
  $build = Start-Congo $prog 'x' (Join-Path $Out "build-$prog.log") $(if ($prog -eq 'server.cone') { $srvDir } else { $here })
  $build.WaitForExit()
  $log = Get-Content (Join-Path $Out "build-$prog.log") -Raw
  if ($log -notmatch 'usage:') { throw "run.ps1: $prog did not build; see $Out\build-$prog.log.err and the log beside it" }
}

$cpuBefore = Machine-Cpu
$srvLog = Join-Path $Out 'server.log'
$cliLog = Join-Path $Out 'client.log'
if ($Alloc -ne 'crt') { $env:_LINK_ = "`"$allocLib`"" }
$srv = Start-Congo 'server.cone' "0 $Workers$(if ($Routed) { ' routed' })" $srvLog $srvDir
Start-Sleep -Milliseconds 300
Remove-Item Env:_LINK_ -ErrorAction SilentlyContinue   # the server's process tree has its environment now
$port = $null
for ($i = 0; $i -lt 600 -and -not $port; $i++) {
  Start-Sleep -Milliseconds 100
  if (Test-Path $srvLog) { $m = Select-String -Path $srvLog -Pattern '^LISTENING (\d+)' | Select-Object -First 1; if ($m) { $port = $m.Matches[0].Groups[1].Value } }
}
if (-not $port) { throw "run.ps1: the server did not listen; see $srvLog" }
$srvProc = $null
for ($i = 0; $i -lt 50 -and -not $srvProc; $i++) { $srvProc = Find-Child $srv.Id 'streamserver.exe'; if (-not $srvProc) { Start-Sleep -Milliseconds 100 } }
if (-not $srvProc) { throw 'run.ps1: no streamserver.exe under the server run' }
$serverPid = [int]$srvProc.ProcessId

$t0 = Get-Date
$cli = Start-Congo 'client.cone' "$port $Conns $Sustain $Idle $Burst $Seed $($shares -join ' ') $Think 1 `"$Out`"" $cliLog
Get-ChildItem $Out -Filter 'ack-*' | Remove-Item -ErrorAction SilentlyContinue
$cliProc = $null
$samples = New-Object System.Collections.ArrayList
[void]$samples.Add((Sample $serverPid 0 'start' $t0))
$seen = 0
$clientPid = 0
while (-not $cli.HasExited) {
  if (-not $clientPid) { $cp = Find-Child $cli.Id 'streamclient.exe'; if ($cp) { $clientPid = [int]$cp.ProcessId } }
  if (Test-Path $cliLog) {
    $lines = @(Get-Content $cliLog | Where-Object { $_ -like 'MARK *' })
    while ($seen -lt $lines.Count) {
      $s = Sample $serverPid $clientPid ($lines[$seen].Substring(5)) $t0
      if ($s -and $s.workingSetMB -gt 0) { [void]$samples.Add($s) }   # the last mark can come as the server ends
      New-Item -ItemType File -Force (Join-Path $Out ('ack-' + $lines[$seen].Substring(5))) | Out-Null   # the client waits for this
      $seen++
    }
  }
  Start-Sleep -Milliseconds 50
}
$cli.WaitForExit()
$resultLine = Get-Content $cliLog | Where-Object { $_ -like 'RESULT *' } | Select-Object -First 1
$srv.WaitForExit(15000) | Out-Null
if (-not $srv.HasExited -and (Get-Process -Id $serverPid -ErrorAction SilentlyContinue)) { Stop-Process -Id $serverPid -Force }
if (-not $resultLine) { throw "run.ps1: the client gave no RESULT; see $cliLog and $cliLog.err" }
$client = $resultLine.Substring(7) | ConvertFrom-Json
$cpuAfter = Machine-Cpu
# the I/O thread's own CPU, printed by the server as it ends ("IOTIMES user_ms U kernel_ms K wall_ms W"); not all the CPU spent on I/O
$io = $null
$im = Select-String -Path $srvLog -Pattern '^IOTIMES user_ms (\d+) kernel_ms (\d+) wall_ms (\d+)' | Select-Object -First 1
if ($im) {
  $iu = [int64]$im.Matches[0].Groups[1].Value; $ik = [int64]$im.Matches[0].Groups[2].Value; $iw = [int64]$im.Matches[0].Groups[3].Value
  $io = [ordered]@{ userMs = $iu; kernelMs = $ik; wallMs = $iw; ownCpuPctOfOneCore = if ($iw -gt 0) { [math]::Round(100 * ($iu + $ik) / $iw, 0) } else { $null } }
}
$avgCpu = [math]::Round((($samples | ForEach-Object { $_.machineCpuPct }) + $cpuBefore + $cpuAfter | Measure-Object -Average).Average, 0)
$last = $samples | Where-Object { $_.clientCpuSec } | Select-Object -Last 1
$wallS = [math]::Max(1, ($client.wallMs / 1000))
$doc = [ordered]@{
  scenario   = 'streamload'
  alloc      = $Alloc
  routed     = [bool]$Routed
  ioThreadOwnCpu = $io
  instrument = [ordered]@{ requested = [bool]$Instrument; files = $null; note = 'row 424 tables not landed; an instrumented run is never timed' }
  loaded     = ($avgCpu -gt 25)
  machineCpuPctAvg = $avgCpu
  params     = [ordered]@{ thinkPct = $Think; conns = $Conns; sustainS = $Sustain; idleS = $Idle; burstS = $Burst; seed = $Seed; mix = $Mix; serverWorkers = $Workers }
  traffic    = $client.traffic
  phaseMs    = $client.phaseMs
  wallMs     = $client.wallMs
  clientCpuPctOfOneCore = if ($last) { [math]::Round(100 * $last.clientCpuSec / $wallS, 0) } else { $null }
  serverCpuPctOfOneCore = if ($last) { [math]::Round(100 * $last.serverCpuSec / $wallS, 0) } else { $null }
  serverMemory = $samples
  logs       = $Out
}
$json = $doc | ConvertTo-Json -Depth 8 -Compress
Set-Content -Path (Join-Path $Out 'result.json') -Value $json
Write-Output "STREAMLOAD $json"
