# mutants.ps1: the second design's mutation check (rows 454 and 506). Each mutant is cn_shim.c with one safety rule broken; the
# test that is meant to catch it must FAIL on it, and the same tests must PASS on the unmutated shim. Run it again after any change
# to the shim's common paths (lf_alloc, lf_free, lf_release, lf_fill, the grouped return) or to "after a free" (cnm_lf_after_free).
#   powershell -File packages\cnmalloc\tests\mutants.ps1 [-Name a|a2m|a2s|b|m1|...|m7|none|all] [-Runs 5] [-Scratch <dir>] [-RaceSecs 120]
#
#   a    Rule G's undo removed: the g2 test still runs, but the thread keeps the cell of the page's new life  (caught by lfrace: double free abort)
#   a2m  the multi-bit fill's Rule G compare (MB-G) tests the class half of genclass only, not the generation half  (caught by lfmulti's same-class rounds, deterministically: the allocator hangs and lfmulti's 120 s watchdog fires)
#   a2s  lf_alloc's single-cell Rule G compare tests the class half only  (NOT CAUGHT by any current test: lfrace's race reached only 5 to 17 Rule G undos of 3000 in 5 runs at 120 s and passed; a driven same-class round on lf_alloc is a queued follow-up. Run it by name only; it is not in 'all')
#   b    lf_release reads genclass AFTER its fetch_or and its park point, not before the cell stopped being the caller's  (caught by lfstale)
#   (row 506, the multi-bit fill and the grouped return; Ruminations\row-460-rules-and-tests-2026-11-18.md, section 4)
#   m1   no g2 test on the multi path (MB-G)                                    (caught by lfmulti: stamps hit, wrong-class cells)
#   m2   the multi-bit undo gives back only the lowest bit of the take          (caught by lfmulti: bits not restored)
#   m3   a fill's cells linked before its g2 test (MB-W)                         (caught by lfmulti: stamps hit)
#   m4   a group's genclass read after its fetch_or (RT-G)                       (caught by lfstale, grouped: CNSTALE_GROUPED=1)
#   m5   a cell's link read after its group's release (RT-L)                     (caught by lfreturn: a wild link followed)
#   m6   the multi-bit undo passes g1, not g2                                    (caught by lfmulti's whole-word rounds: page left UNLISTED)
#   m7   the grouped return's double-free test on the group's lowest bit only (RT-D)  (caught by doublefree's grouped part)
#   none the unmutated shim: doublefree, lfpage (single and grouped), lfrace (plain and with fills), lfstale (single and grouped),
#        lfmulti and lfreturn must pass
# Everything is built and run under <Scratch> (default %TEMP%\cnmalloc-mutants); nothing in the tree changes. A mutant's test
# runs -Runs times (all but a are deterministic: 5 of 5 must fail; a2s is the known gap, see above). lfrace on a mutant is capped at -RaceSecs seconds
# (CNRACE_MAXSECS): a2s is not caught within it; the unmutated lfrace runs
# with its default. Windows only (the shim is). Needs the Visual Studio build tools and the package's release build, which
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

# the unmutated libraries (cn.lib, cnd.lib) and the package's release object
$alloc = Join-Path $Scratch 'alloc'
& powershell -NoProfile -File (Join-Path $pkg 'shim\build-cn.ps1') -Out $alloc | Out-Null
$obj = Join-Path $pkg 'build\release\cnmalloc.obj'

# each mutant: pairs of (exact text, replacement); every anchor must occur exactly once in cn_shim.c
$mutants = @{
  'a'  = @(, @(("            lf_release(P, g2, j, bit);`n            if (ask) cnm_lf_grow(g_heap, c);`n            goto restart;"), '            (void)g2;'))
  'a2m' = @(, @('if (g2 != g1) {                               // MB-G:', 'if ((g2 & 0xFF) != (g1 & 0xFF)) {               // MB-G:'))
  'a2s' = @(, @('if (g2 != g1) {                               // Rule G:', 'if ((g2 & 0xFF) != (g1 & 0xFF)) {               // Rule G:'))
  'b'  = @(, @('LF_HOOK_OR(g & 0xFF);                               // tests only (CN_HOOKS): a park point after the fetch_or', ('LF_HOOK_OR(g & 0xFF);' + "`n" + '  g = P->genclass;')))
  'm1' = @(, @('if (g2 != g1) {                               // MB-G', 'if (0) {                               // MB-G'))
  'm2' = @(, @('lf_release(P, g2, j, want);', 'lf_release(P, g2, j, want & (0 - want));'))
  'm3' = @(
    @('LF_HOOK_TAKE(c, want);', ('size_t early_got; void* early = lf_link(P, c, j, want, &early_got);' + "`n" + '          LF_HOOK_TAKE(c, want);')),
    @('void* head = lf_link(P, c, j, want, got);', 'void* head = early; *got = early_got;'))
  'm4' = @(, @(("  uint64_t g = P->genclass;                           // RT-G: before this group's fetch_or`n  uint64_t old = (uint64_t)_InterlockedOr64((volatile LONG64*)&P->bits[j], (LONG64)mask);`n  LF_HOOK_GROUP(g & 0xFF);"),
                ("  uint64_t old = (uint64_t)_InterlockedOr64((volatile LONG64*)&P->bits[j], (LONG64)mask);`n  LF_HOOK_GROUP(0);`n  uint64_t g = P->genclass;")))
  'm5' = @(, @(("    while (p && n && m < LF_RBATCH) { a[m++] = p; p = *(void**)p; n--; }    // RT-L: the batch's links, all read first`n    lf_give_cells(a, m, cl);"),
                ("    a[m++] = p; n--;`n    lf_give_cells(a, m, cl);`n    p = *(void**)a[0];")))
  'm6' = @(, @('lf_release(P, g2, j, want);', 'lf_release(P, g1, j, want);'))
  'm7' = @(, @('if (old & mask) {                                   // RT-D', 'if (old & mask & (0 - mask)) {                      // RT-D'))
}

function Build-Mutant([string]$m) {
  $out = Join-Path $Scratch "mut-$m"
  New-Item -ItemType Directory -Force $out | Out-Null
  $t = [IO.File]::ReadAllText((Join-Path $pkg 'shim\cn_shim.c')).Replace("`r`n", "`n")
  if (-not $mutants.ContainsKey($m)) { throw "unknown mutant $m" }
  foreach ($pair in $mutants[$m]) {
    $from = $pair[0]; $to = $pair[1]
    $count = ([regex]::Matches($t, [regex]::Escape($from))).Count
    if ($count -ne 1) { throw "mutation anchor for $m found $count times: $from" }
    $t = $t.Replace($from, $to)
  }
  [IO.File]::WriteAllText("$out\cn_shim.c", $t, (New-Object System.Text.UTF8Encoding($false)))
  $bat = "$out\build.bat"
  Set-Content $bat "cd /d $out`r`ncl /nologo /c /O2 /MD /D_ACRTIMP= /Gs999999 /DCN_HOOKS /DCN_DEBUG `"$out\cn_shim.c`" /Fo:cn_shim_d.obj || exit /b 1`r`nlib /nologo /OUT:cnd.lib cn_shim_d.obj `"$obj`" || exit /b 1"
  cmd /c "`"$vcvars`" >nul 2>&1 && `"$bat`"" 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "cl or lib failed for mutant $m" }
  return $out
}

# each test's CNMALLOC setting, and the variants (extra environment) the unmutated run checks
$cfg = @{
  'lfstale'    = 'design=lockfree,giveback=now,slots=1,stats,cache=16'
  'lfmulti'    = 'design=lockfree,giveback=now,slots=1,stats,cache=16'
  'lfreturn'   = 'design=lockfree,giveback=now,slots=1,stats,cache=32'
  'doublefree' = 'design=lockfree,giveback=now,stats,cache=16'
  'lfpage'     = 'design=lockfree,giveback=now,stats,cache=16'
  'lfrace'     = 'design=lockfree,giveback=now,stats'
}
$variants = @{
  'doublefree' = @(@{})
  'lfpage'     = @(@{}, @{ CNPAGE_GROUPED = '1' })
  'lfrace'     = @(@{}, @{ CNRACE_WORKERS = '1'; CNMALLOC = 'design=lockfree,giveback=now,stats,cache=7' })
  'lfstale'    = @(@{}, @{ CNSTALE_GROUPED = '1' })
  'lfmulti'    = @(@{})
  'lfreturn'   = @(@{})
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
  foreach ($k in @('CNMALLOC', '_LINK_', 'LIB', 'CNRACE_MAXSECS', 'CNTEST_LOG', 'CNRACE_WORKERS', 'CNPAGE_GROUPED', 'CNSTALE_GROUPED')) { $save[$k] = (Get-Item "Env:$k" -ErrorAction SilentlyContinue).Value }
  Push-Location $run
  try {
    $env:CNMALLOC = $cfg[$test]
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
function Check([string]$what, [bool]$ok) { $script:verdicts += [pscustomobject]@{ Check = $what; Ok = $ok }; "{0,-60} {1}" -f $what, $(if ($ok) { 'ok' } else { 'WRONG' }) }

# each mutant's catching test, with the variant that catches it
$plan = @{
  'a' = @('lfrace', @{}); 'a2m' = @('lfmulti', @{}); 'a2s' = @('lfrace', @{}); 'b' = @('lfstale', @{})
  'm1' = @('lfmulti', @{}); 'm2' = @('lfmulti', @{}); 'm3' = @('lfmulti', @{}); 'm4' = @('lfstale', @{ CNSTALE_GROUPED = '1' })
  'm5' = @('lfreturn', @{}); 'm6' = @('lfmulti', @{}); 'm7' = @('doublefree', @{})
}
$names = if ($Name -eq 'all') { @('a', 'a2m', 'b', 'm1', 'm2', 'm3', 'm4', 'm5', 'm6', 'm7', 'none') } else { $Name.Split(',') }
foreach ($m in $names) {
  if ($m -eq 'none') {
    foreach ($t in 'doublefree', 'lfpage', 'lfrace', 'lfstale', 'lfmulti', 'lfreturn') {
      $deterministic = $t -in @('lfstale', 'lfmulti', 'lfreturn', 'doublefree')
      $n = if ($deterministic) { $Runs } else { 1 }      # the driven tests are cheap: -Runs times; lfrace and lfpage once
      $v = 0
      foreach ($extra in $variants[$t]) {
        $v++
        for ($i = 1; $i -le $n; $i++) { Check "unmutated: $t variant $v passes (run $i)" (Run-Test $alloc $t "none$v-$i" $extra) }
      }
    }
  } else {
    $lib = Build-Mutant $m
    $t = $plan[$m][0]
    $extra = @{} + $plan[$m][1]
    if ($t -eq 'lfrace') { $extra['CNRACE_MAXSECS'] = "$RaceSecs" }
    for ($i = 1; $i -le $Runs; $i++) { Check "mutant ${m}: $t FAILS (run $i)" (-not (Run-Test $lib $t "$m$i" $extra)) }
  }
}
"{0} of {1} checks as expected; logs and numbers in {2}" -f @($verdicts | Where-Object Ok).Count, $verdicts.Count, $Scratch
if (@($verdicts | Where-Object { -not $_.Ok }).Count -gt 0) { exit 1 }
