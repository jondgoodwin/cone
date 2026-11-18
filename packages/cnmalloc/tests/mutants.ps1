# mutants.ps1: the second design's mutation check (row 454). Each mutant is cn_shim.c with one safety rule broken; the test
# that is meant to catch it must FAIL on it, and the same tests must PASS on the unmutated shim. Run it again after any change
# to the shim's common paths (lf_alloc, lf_free, lf_release) or to "after a free" (cnm_lf_after_free) in cnmalloc.cone.
#   powershell -File packages\cnmalloc\tests\mutants.ps1 [-Name a|a2|b|none|all] [-Runs 5] [-Scratch <dir>] [-RaceSecs 120]
#
#   a    Rule G's undo removed: the g2 test still runs, but the thread keeps the cell of the page's new life  (caught by lfrace: double free abort)
#   a2   Rule G compares the class half of genclass only, not the generation half                              (caught by lfrace: no undos counted)
#   b    lf_release reads genclass AFTER its fetch_or and its park point, not before the cell stopped being the caller's  (caught by lfstale)
#   none the unmutated shim: lfrace, lfpage, lfstale, doublefree must pass
# Everything is built and run under <Scratch> (default %TEMP%\cnmalloc-mutants); nothing in the tree changes. A mutant's test
# runs -Runs times (b is deterministic: 5 of 5 must fail). lfrace on a mutant is capped at -RaceSecs seconds (CNRACE_MAXSECS):
# a2 is caught by the undo target not being reached, which a shorter cap still shows; the unmutated lfrace runs with its default.
# Windows only (the shim is). Needs the Visual Studio build tools and the package's release build, which build-cn.ps1 makes.
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

# the unmutated libraries (cn.lib, cnd.lib) and the package's release object
$alloc = Join-Path $Scratch 'alloc'
& powershell -NoProfile -File (Join-Path $pkg 'shim\build-cn.ps1') -Out $alloc | Out-Null
$obj = Join-Path $pkg 'build\release\cnmalloc.obj'

function Build-Mutant([string]$m) {
  $out = Join-Path $Scratch "mut-$m"
  New-Item -ItemType Directory -Force $out | Out-Null
  $t = [IO.File]::ReadAllText((Join-Path $pkg 'shim\cn_shim.c')).Replace("`r`n", "`n")
  switch ($m) {
    'a'  { $from = '            lf_release(P, g2, j, bit);' + "`n" + '            if (ask) cnm_lf_grow(g_heap, c);' + "`n" + '            goto restart;'; $to = '            (void)g2;' }
    'a2' { $from = 'if (g2 != g1) {'; $to = 'if ((g2 & 0xFF) != (g1 & 0xFF)) {' }
    'b'  { $from = 'LF_HOOK_OR(g & 0xFF);'; $to = 'LF_HOOK_OR(g & 0xFF);' + "`n" + '  g = P->genclass;' }
    default { throw "unknown mutant $m" }
  }
  if (-not $t.Contains($from)) { throw "mutation anchor missing: $from" }
  $t = $t.Replace($from, $to)
  [IO.File]::WriteAllText("$out\cn_shim.c", $t, (New-Object System.Text.UTF8Encoding($false)))
  $bat = "$out\build.bat"
  Set-Content $bat "cd /d $out`r`ncl /nologo /c /O2 /MD /D_ACRTIMP= /Gs999999 /DCN_HOOKS /DCN_DEBUG `"$out\cn_shim.c`" /Fo:cn_shim_d.obj || exit /b 1`r`nlib /nologo /OUT:cnd.lib cn_shim_d.obj `"$obj`" || exit /b 1"
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
  Copy-Item "$pkg\src\*" "$run\src"
  Copy-Item "$pkg\tests\$test.*" "$run\tests"
  $log = Join-Path $Scratch "log-$test-$tag.txt"
  $save = @{}
  foreach ($k in @('CNMALLOC', '_LINK_', 'LIB', 'CNRACE_MAXSECS', 'CNTEST_LOG')) { $save[$k] = (Get-Item "Env:$k" -ErrorAction SilentlyContinue).Value }
  Push-Location $run
  try {
    $env:CNMALLOC = if ($test -eq 'lfstale') { 'design=lockfree,giveback=now,slots=1,stats' } else { 'design=lockfree,giveback=now,stats' }
    $env:_LINK_ = '"{0}\cnd.lib"' -f $libdir
    $env:LIB = 'C:\libs\SDL3-3.4.16\lib\x64;' + $env:LIB
    $env:CNTEST_LOG = Join-Path $Scratch "numbers-$test-$tag.txt"
    foreach ($k in $extra.Keys) { Set-Item "Env:$k" $extra[$k] }
    $ErrorActionPreference = 'Continue'      # congo writes a failing test's report to stderr
    python $congo test $test *> $log
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
function Check([string]$what, [bool]$ok) { $script:verdicts += [pscustomobject]@{ Check = $what; Ok = $ok }; "{0,-52} {1}" -f $what, $(if ($ok) { 'ok' } else { 'WRONG' }) }

$plan = @{ 'a' = 'lfrace'; 'a2' = 'lfrace'; 'b' = 'lfstale' }
$names = if ($Name -eq 'all') { @('a', 'a2', 'b', 'none') } else { @($Name) }
foreach ($m in $names) {
  if ($m -eq 'none') {
    foreach ($t in 'doublefree', 'lfpage', 'lfrace', 'lfstale') {
      $n = if ($t -eq 'lfstale') { $Runs } else { 1 }      # lfstale is deterministic and cheap: -Runs times; the others once
      for ($i = 1; $i -le $n; $i++) { Check "unmutated: $t passes (run $i)" (Run-Test $alloc $t "none$i" @{}) }
    }
  } else {
    $lib = Build-Mutant $m
    $t = $plan[$m]
    $extra = if ($t -eq 'lfrace') { @{ CNRACE_MAXSECS = "$RaceSecs" } } else { @{} }
    for ($i = 1; $i -le $Runs; $i++) { Check "mutant ${m}: $t FAILS (run $i)" (-not (Run-Test $lib $t "$m$i" $extra)) }
  }
}
"{0} of {1} checks as expected; logs and numbers in {2}" -f @($verdicts | Where-Object Ok).Count, $verdicts.Count, $Scratch
if (@($verdicts | Where-Object { -not $_.Ok }).Count -gt 0) { exit 1 }
