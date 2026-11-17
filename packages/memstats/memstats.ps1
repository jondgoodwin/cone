# Runs congo with the memstats shim linked and -D MEMSTATS:
#
#   memstats.ps1 <congo verb and arguments...>       e.g.  memstats.ps1 run --release x.cone -- args
#
# Builds shim\out\memstats.lib when missing; puts it on the link line through the _LINK_
# environment variable (link.exe appends it after the objects and conestd.lib, before the default
# libraries, so every malloc, conestd's own included, binds to the shim); sets MEMSTATS_OUT (default .\memstats.jsonl) for the sampler's
# lines and totals; and gives congo -D MEMSTATS after the verb. Set CONGO to use another congo.py.
$ErrorActionPreference = 'Stop'
$lib = Join-Path $PSScriptRoot 'shim\out\memstats.lib'
if (-not (Test-Path $lib)) { & (Join-Path $PSScriptRoot 'shim\build.ps1') }
$congo = if ($env:CONGO) { $env:CONGO } else { Join-Path $PSScriptRoot '..\..\tools\congo\congo.py' }
$env:_LINK_ = '"' + $lib + '"'
if (-not $env:MEMSTATS_OUT) { $env:MEMSTATS_OUT = Join-Path (Get-Location) 'memstats.jsonl' }
$verb, $rest = $args
python $congo $verb -D MEMSTATS @rest
exit $LASTEXITCODE
