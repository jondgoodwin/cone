# Alternates Cone stream, Cone ad hoc, C#, and Rust, N times (default 5), and prints the rows.
# Run on a quiet machine, from PowerShell:
#   $env:CARGO_TARGET_DIR = "C:\src\zz-rusttwin-target"
#   .\tools\streambench\run-alternated.ps1 [-Mib 128] [-Rounds 5] [-SkipCone]
# Needs: cargo (C:\Users\jondg\.cargo\bin), dotnet 9, and for the Cone rows the built
# compiler (vcvars64 + LLVM as in CLAUDE.md; congo is run through cmd with vcvars).
param([int]$Mib = 128, [int]$Rounds = 5, [switch]$SkipCone)
$ErrorActionPreference = "Stop"
$root = Resolve-Path "$PSScriptRoot\..\.."
$env:Path = "C:\Users\jondg\.cargo\bin;$env:Path"
if (-not $env:CARGO_TARGET_DIR) { $env:CARGO_TARGET_DIR = "C:\src\zz-rusttwin-target" }
$rs = Join-Path $env:CARGO_TARGET_DIR "release\streambench.exe"
Push-Location "$PSScriptRoot\rust"; cargo build --release 2>&1 | Select-Object -Last 1; Pop-Location
Push-Location "$PSScriptRoot\csharp"; dotnet build -c Release -o "$env:TEMP\streambench-cs" 2>&1 | Select-Object -Last 1; Pop-Location
$cs = "$env:TEMP\streambench-cs\streambench.dll"
$vc = '"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"'
$env:LIB = "C:\libs\SDL3-3.4.16\lib\x64;" + $env:LIB
function ConeRun($mode) {
  Push-Location $root
  $o = cmd /c "$vc >nul && python tools\congo\congo.py run --release packages/stageio/examples/streamsample.cone -- $mode $Mib" 2>&1
  Pop-Location
  ($o | Select-String "MiB: " | Select-Object -Last 1).Line
}
function Last($o) { ($o | Select-String "MiB: " | Select-Object -Last 1).Line }
$rows = @()
if (-not $SkipCone) { $null = ConeRun "pipeline" }   # warm-up: compiles the sample once
for ($i = 1; $i -le $Rounds; $i++) {
  if (-not $SkipCone) {
    $rows += "round $i  Cone  " + (ConeRun "pipeline")
    $rows += "round $i  Cone  " + (ConeRun "adhoc")
  }
  $rows += "round $i  C#    " + (Last (dotnet $cs pipeline $Mib))
  $rows += "round $i  C#    " + (Last (dotnet $cs adhoc $Mib))
  $rows += "round $i  Rust  " + (Last (& $rs pipeline $Mib))
  $rows += "round $i  Rust  " + (Last (& $rs adhoc $Mib))
  $rows | Select-Object -Last 6
}
"`n=== all rows ==="; $rows
"`n=== each stage alone (one pass each) ==="
if (-not $SkipCone) {
  Push-Location $root
  "Cone"; cmd /c "$vc >nul && python tools\congo\congo.py run --release packages/stages/examples/streamrates.cone -- $Mib" 2>&1
  Pop-Location
}
"C#"; dotnet $cs rates $Mib
"Rust"; & $rs rates $Mib
