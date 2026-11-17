# Builds the memstats shim library (out\memstats.lib) with the VS 2022 x64 toolchain,
# and with -Test builds and runs the shim's own C test.
param([switch]$Test)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$out = Join-Path $here 'out'
New-Item -ItemType Directory -Force $out | Out-Null
$vc = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'
$cmd = "call `"$vc`" >nul && cd /d `"$out`" && cl /nologo /O2 /MD /W3 /D_ACRTIMP= /c /Fo:memstats.obj `"$here\memstats.c`" && lib /nologo /out:memstats.lib memstats.obj"
if ($Test) {
  # the test is linked with the shim FIRST, so its malloc and free are the shim's
  $cmd += " && cl /nologo /O1 /MD /W3 /D_ACRTIMP= /Fe:shimtest.exe `"$here\shimtest.c`" /link memstats.lib kernel32.lib && .\shimtest.exe"
}
Set-Content "$out\build.bat" "@echo off`r`n($cmd) > `"$out\build.log`" 2>&1"
cmd /c "$out\build.bat"
Get-Content "$out\build.log" | Where-Object { $_ -notmatch 'vswhere|operable program' }
if ($LASTEXITCODE -ne 0) { throw "memstats shim build or test failed" }
