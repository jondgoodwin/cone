# slackmutant.ps1: the large-block slack's mutation check (row 547). The bound check in largeAlloc's cache hit
# ("if ent.committed > keep {") is replaced by "if false {", so a reused block keeps its whole cached commit;
# tests\slack.cone must FAIL on the mutant and PASS on the unmutated source. The source file is put back afterwards.
#   powershell -File packages\cnmalloc\tests\slackmutant.ps1
# Needs CONEC and CONE_DEPS set as for any congo run, and the Visual Studio environment (vcvars64) on the path.
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$pkg = Split-Path -Parent $here
$congo = Join-Path (Split-Path -Parent (Split-Path -Parent $pkg)) 'tools\congo\congo.py'
$src = Join-Path $pkg 'src\cnmalloc.cone'
$orig = [IO.File]::ReadAllText($src)
$anchor = 'if ent.committed > keep {'
if (([regex]::Matches($orig, [regex]::Escape($anchor))).Count -ne 1) { throw 'mutation anchor not found exactly once' }
function Run-Slack {
  $ErrorActionPreference = 'Continue'
  Push-Location $pkg
  try { $out = & python $congo test slack 2>&1 | Out-String } finally { Pop-Location }
  return ($out -match 'test slack \.\.\. ok')
}
$okBefore = Run-Slack
try {
  [IO.File]::WriteAllText($src, $orig.Replace($anchor, 'if false {'), (New-Object System.Text.UTF8Encoding($false)))
  $okMutant = Run-Slack
} finally {
  [IO.File]::WriteAllText($src, $orig, (New-Object System.Text.UTF8Encoding($false)))
}
$okAfter = Run-Slack
"unmutated: $(if ($okBefore) {'pass'} else {'FAIL'}); mutant (bound check removed): $(if ($okMutant) {'PASSED (not caught)'} else {'fails (caught)'}); unmutated again: $(if ($okAfter) {'pass'} else {'FAIL'})"
if (-not $okBefore -or $okMutant -or -not $okAfter) { exit 1 }
