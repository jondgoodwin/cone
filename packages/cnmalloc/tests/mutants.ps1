# mutants.ps1: the second design's mutation check (rows 454 and 506). Each mutant is cn_shim.c with one safety rule broken; the
# test that is meant to catch it must FAIL on it, and the same tests must PASS on the unmutated shim. Run it again after any change
# to the shim's common paths (lf_alloc, lf_free, lf_release, lf_fill, the grouped return) or to "after a free" (cnm_lf_after_free).
#   powershell -File packages\cnmalloc\tests\mutants.ps1 [-Name a|a2m|a2s|b|m1|...|m7|h1|none|all] [-Runs 5] [-Scratch <dir>] [-RaceSecs 120]
# (Row 550) The mutants themselves, each test's setting and variants live in mutants.json beside this file, which the Linux runner
# mutants.py reads too: one copy, so the two cannot drift. The text each mutant replaces is in that file; what each breaks:
#
#   a    Rule G's undo removed: the g2 test still runs, but the thread keeps the cell of the page's new life  (caught by lfrace: double free abort)
#   a2m  the multi-bit fill's Rule G compare (MB-G) tests the class half of genclass only, not the generation half  (caught by lfmulti's same-class rounds, deterministically: the allocator hangs, lfmulti's waitFor gives up after 5 s and its checks fail; the 120 s watchdog does not fire)
#   a2s  lf_alloc's single-cell Rule G compare tests the class half only  (caught by lfmulti's single-cell rounds (row 532), deterministically by its checks: a non-worker's lf_alloc is parked at LF_HOOK_TAKE while the page comes back as the same class; the taker keeps the cell, so its bit is not restored, Rule G's count does not rise and the page stays UNLISTED)
#   b    lf_release reads genclass AFTER its fetch_or and its park point, not before the cell stopped being the caller's  (caught by lfstale)
#   (row 506, the multi-bit fill and the grouped return; Ruminations\row-460-rules-and-tests-2026-11-18.md, section 4)
#   m1   no g2 test on the multi path (MB-G)                                    (caught by lfmulti: stamps hit, wrong-class cells)
#   m2   the multi-bit undo gives back only the lowest bit of the take          (caught by lfmulti: bits not restored)
#   m3   a fill's cells linked before its g2 test (MB-W)                         (caught by lfmulti: stamps hit)
#   m4   a group's genclass read after its fetch_or (RT-G)                       (caught by lfstale, grouped: CNSTALE_GROUPED=1)
#   m5   a cell's link read after its group's release (RT-L)                     (caught by lfreturn: a wild link followed)
#   m6   the multi-bit undo passes g1, not g2                                    (caught by lfmulti's whole-word rounds: page left UNLISTED)
#   m7   the grouped return's double-free test on the group's lowest bit only (RT-D)  (caught by doublefree's grouped part)
#   (row 539, the scan's start word)
#   h1   the start word's key names the page but not its generation: a retired page's next life starts at the old life's word  (caught by lfmulti: its filler's word is not the one the round set up; found 19 Nov when the key was the page alone)
#   (row 580, the slot cap; a mutant of cnmalloc.cone, not of the shim)
#   s1   the default slot cap left at 8 (the setting's default 0 replaced by 8)  (caught by lfslots: sixteen threads on one class never take it above 8 active slots)
#   (row 603, the re-listing threshold; the tests that look for one free cell to list a page run with relist=1 in mutants.json)
#   e1   the free's edge test is "the word was empty" (n + 1 == 1) whatever the threshold  (caught by lfstorm: no page that a free
#        brings to its threshold is ever listed again, so the class takes a new page for every hundred allocations and its page count
#        grows with the operations while takes per operation fall to about 0.01; the page-count check fails)
#   s0   the out-of-memory sweep removed (lf_oom returns 0)                                (caught by lfoom: a 64 MiB heap filled, the hidden cells freed, and the
#        first allocation after them fails; the model's s0 is the same mutation)
#   none the unmutated shim: doublefree, lfpage (single and grouped), lfrace (plain, with fills, and both again at the default
#        threshold), lfstale (single and grouped), lfmulti, lfreturn, lfstorm (default, relist=2, relist=3) and lfoom (default,
#        relist=2, relist=3) and lfslots must pass
# Everything is built and run under <Scratch> (default %TEMP%\cnmalloc-mutants); nothing in the tree changes. A mutant's test
# runs -Runs times (all but a are deterministic: 5 of 5 must fail). lfrace on a mutant is capped at -RaceSecs seconds
# (CNRACE_MAXSECS); the unmutated lfrace runs with its default. Windows only (cl and link; Linux's is mutants.py). Needs the Visual Studio build tools and the package's release build, which
# build-cn.ps1 makes.
param(
  [string]$Name = 'all',
  [int]$Runs = 1,
  [string]$Scratch = (Join-Path $env:TEMP 'cnmalloc-mutants'),
  [int]$RaceSecs = 120
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$pkg = Split-Path -Parent $here
$tree = Split-Path -Parent (Split-Path -Parent $pkg)
$congo = Join-Path $tree 'tools\congo\congo.py'
$vcvars = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'
New-Item -ItemType Directory -Force $Scratch | Out-Null

# the mutants and each test's setting, from the data file both runners read
function To-Env($o) { $h = @{}; if ($o) { foreach ($p in $o.PSObject.Properties) { $h[$p.Name] = [string]$p.Value } }; return $h }
$data = Get-Content (Join-Path $here 'mutants.json') -Raw | ConvertFrom-Json
$cfg = @{}; foreach ($p in $data.cfg.PSObject.Properties) { $cfg[$p.Name] = [string]$p.Value }
$variants = @{}; foreach ($p in $data.variants.PSObject.Properties) { $variants[$p.Name] = @($p.Value | ForEach-Object { To-Env $_ }) }
$deterministicTests = @($data.deterministic)

# the unmutated libraries (cn.lib, cnd.lib) and the package's release object
$alloc = Join-Path $Scratch 'alloc'
& powershell -NoProfile -File (Join-Path $pkg 'shim\build-cn.ps1') -Out $alloc | Out-Null
$obj = Join-Path $pkg 'build\release\cnmalloc.obj'

function Build-Mutant([string]$m) {
  $out = Join-Path $Scratch "mut-$m"
  New-Item -ItemType Directory -Force $out | Out-Null
  $t = [IO.File]::ReadAllText((Join-Path $pkg 'shim\cn_shim.c')).Replace("`r`n", "`n")
  $mut = $data.mutants.PSObject.Properties[$m]
  if (-not $mut) { throw "unknown mutant $m" }
  foreach ($pair in $mut.Value.edits) {
    $from = $pair[0]; $to = $pair[1]
    $count = ([regex]::Matches($t, [regex]::Escape($from))).Count
    if ($count -ne 1) { throw "mutation anchor for $m found $count times: $from" }
    $t = $t.Replace($from, $to)
  }
  [IO.File]::WriteAllText("$out\cn_shim.c", $t, (New-Object System.Text.UTF8Encoding($false)))
  Copy-Item (Join-Path $pkg 'shim\cn_plat.h') $out -Force
  $useObj = $obj
  if ($mut.Value.PSObject.Properties['cone']) {           # (row 580) a mutant of the Cone side: its own copy of the package, built
    $cp = Join-Path $out 'pkg'
    if (Test-Path $cp) { Remove-Item -Recurse -Force $cp }
    New-Item -ItemType Directory -Force "$cp\src" | Out-Null
    Copy-Item (Join-Path $pkg 'congo.toml') $cp
    Copy-Item (Join-Path $pkg 'src\*') "$cp\src"
    $c = [IO.File]::ReadAllText("$cp\src\cnmalloc.cone").Replace("`r`n", "`n")
    foreach ($pair in $mut.Value.cone) {
      $from = $pair[0]; $to = $pair[1]
      $count = ([regex]::Matches($c, [regex]::Escape($from))).Count
      if ($count -ne 1) { throw "mutation anchor for $m found $count times in cnmalloc.cone: $from" }
      $c = $c.Replace($from, $to)
    }
    [IO.File]::WriteAllText("$cp\src\cnmalloc.cone", $c, (New-Object System.Text.UTF8Encoding($false)))
    Push-Location $cp
    python $congo build --release | Out-Null
    $code = $LASTEXITCODE
    Pop-Location
    if ($code -ne 0) { throw "congo build failed for mutant $m" }
    $useObj = Join-Path $cp 'build\release\cnmalloc.obj'
  }
  $bat = "$out\build.bat"
  Set-Content $bat "cd /d $out`r`ncl /nologo /c /O2 /MD /D_ACRTIMP= /Gs999999 /DCN_HOOKS /DCN_DEBUG `"$out\cn_shim.c`" /Fo:cn_shim_d.obj || exit /b 1`r`nlib /nologo /OUT:cnd.lib cn_shim_d.obj `"$useObj`" || exit /b 1"
  cmd /c "`"$vcvars`" >nul 2>&1 && `"$bat`"" 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "cl or lib failed for mutant $m" }
  return $out
}

# one test run on the second design linked from <libdir>\cnd.lib, in its own copy of the package; returns $true when the test passed
function Run-Test([string]$libdir, [string]$test, [string]$tag, [hashtable]$extra) {
  $run = Join-Path $Scratch "run-$test-$tag"
  if (Test-Path $run) { Remove-Item -Recurse -Force $run }
  New-Item -ItemType Directory -Force "$run\src", "$run\tests" | Out-Null
  Copy-Item "$pkg\congo.toml" $run
  $srcdir = if (Test-Path "$libdir\pkg\src") { "$libdir\pkg\src" } else { "$pkg\src" }     # (row 580) a Cone-side mutant's own source: the test compiles it in too
  Copy-Item "$srcdir\*" "$run\src"
  Copy-Item "$pkg\tests\$test.*" "$run\tests"
  $log = Join-Path $Scratch "log-$test-$tag.txt"
  $save = @{}
  foreach ($k in @('CNMALLOC', '_LINK_', 'LIB', 'CNRACE_MAXSECS', 'CNTEST_LOG', 'CNRACE_WORKERS', 'CNPAGE_GROUPED', 'CNSTALE_GROUPED')) { $save[$k] = (Get-Item "Env:$k" -ErrorAction SilentlyContinue).Value }
  Push-Location $run
  try {
    $env:CNMALLOC = $cfg[$test]
    $env:_LINK_ = '"{0}\cnd.lib"' -f $libdir
    $env:LIB = 'C:\libs\SDL3-3.4.16\lib\x64;' + $env:LIB
    $env:CNTEST_LOG = Join-Path $Scratch "numbers-$test-$tag.txt"
    foreach ($k in $extra.Keys) { Set-Item "Env:$k" $extra[$k] }
    $ErrorActionPreference = 'Continue'      # congo writes a failing test's report to stderr
    python $congo test --no-cache $test *> $log
    $code = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
  } finally {
    Pop-Location
    foreach ($k in $save.Keys) { if ($null -eq $save[$k]) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue } else { Set-Item "Env:$k" $save[$k] } }
  }
  $txt = Get-Content $log -Raw
  if ($txt -match 'could not compile') { throw "test $test did not compile; see $log" }
  return ($code -eq 0 -and $txt -match "test $test \.\.\. ok")
}

$verdicts = @()
function Check([string]$what, [bool]$ok) { $script:verdicts += [pscustomobject]@{ Check = $what; Ok = $ok }; "{0,-60} {1}" -f $what, $(if ($ok) { 'ok' } else { 'WRONG' }) }

$names = if ($Name -eq 'all') { @($data.order) + @('none') } else { $Name.Split(',') }
foreach ($m in $names) {
  if ($m -eq 'none') {
    foreach ($t in @($data.unmutated)) {
      $deterministic = $t -in $deterministicTests
      $n = if ($deterministic) { $Runs } else { 1 }      # the driven tests are cheap: -Runs times; lfrace and lfpage once
      $v = 0
      foreach ($extra in $variants[$t]) {
        $v++
        for ($i = 1; $i -le $n; $i++) { Check "unmutated: $t variant $v passes (run $i)" (Run-Test $alloc $t "none$v-$i" $extra) }
      }
    }
  } else {
    $lib = Build-Mutant $m
    $mut = $data.mutants.PSObject.Properties[$m].Value
    $t = $mut.test
    $extra = To-Env $mut.env
    if ($t -eq 'lfrace') { $extra['CNRACE_MAXSECS'] = "$RaceSecs" }
    for ($i = 1; $i -le $Runs; $i++) { Check "mutant ${m}: $t FAILS (run $i)" (-not (Run-Test $lib $t "$m$i" $extra)) }
  }
}
"{0} of {1} checks as expected; logs and numbers in {2}" -f @($verdicts | Where-Object Ok).Count, $verdicts.Count, $Scratch
if (@($verdicts | Where-Object { -not $_.Ok }).Count -gt 0) { exit 1 }
