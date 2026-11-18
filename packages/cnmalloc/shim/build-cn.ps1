# build-cn.ps1: builds cn.lib, the cnmalloc link shim: the Cone package's release object plus shim\cn_shim.c.
#   powershell -File build-cn.ps1 [-Out C:\src\zz-cnmalloc\alloc]      (writes <Out>\cn.lib and <Out>\cn\cn.lib, and <Out>\cnd.lib)
# Then link a program with it: the C runtime's malloc family is replaced (shim\README.md, "Linking it").
# cnd.lib (row 454) is the same source built with -DCN_HOOKS -DCN_DEBUG: the second design's test hooks, and a
# double free that aborts (or calls a test's handler). Only the second design's tests link it.
param([string]$Out = 'C:\src\zz-cnmalloc\alloc')
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$pkg = Split-Path -Parent $here
$congo = Join-Path (Split-Path -Parent (Split-Path -Parent $pkg)) 'tools\congo\congo.py'
$work = Join-Path $pkg 'build\shim'
New-Item -ItemType Directory -Force $work, (Join-Path $Out 'cn') | Out-Null
Push-Location $pkg
python $congo build --release
if ($LASTEXITCODE -ne 0) { Pop-Location; throw 'congo build failed' }
Pop-Location
$obj = Join-Path $pkg 'build\release\cnmalloc.obj'
$bat = Join-Path $work 'build-cn.bat'
Set-Content $bat @"
cd /d $work
cl /nologo /c /O2 /MD /D_ACRTIMP= /Gs999999 "$here\cn_shim.c" /Fo:cn_shim.obj || exit /b 1
lib /nologo /OUT:cn.lib cn_shim.obj "$obj" || exit /b 1
cl /nologo /c /O2 /MD /D_ACRTIMP= /Gs999999 /DCN_HOOKS /DCN_DEBUG "$here\cn_shim.c" /Fo:cn_shim_d.obj || exit /b 1
lib /nologo /OUT:cnd.lib cn_shim_d.obj "$obj" || exit /b 1
"@
cmd /c "`"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat`" >nul 2>&1 && `"$bat`"" 2>&1 | Out-String
if ($LASTEXITCODE -ne 0) { throw 'cl or lib failed' }
Copy-Item (Join-Path $work 'cn.lib') (Join-Path $Out 'cn\cn.lib') -Force
Copy-Item (Join-Path $work 'cn.lib') (Join-Path $Out 'cn.lib') -Force
Copy-Item (Join-Path $work 'cnd.lib') (Join-Path $Out 'cnd.lib') -Force
Get-Item (Join-Path $Out 'cn.lib'), (Join-Path $Out 'cnd.lib') | Select-Object FullName, Length
