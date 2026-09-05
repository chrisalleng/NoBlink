# Build NoBlink.dll (Win32 / MSVC v143).
#
# Ashita loads only 32-bit plugins, so this must be built from the x86 developer
# environment (vcvars32.bat). Verified with Visual Studio 2022 Build Tools 17.14,
# MSVC 14.44.35207, against the Ashita v4 SDK at $SdkPath.

param(
    [string]$SdkPath  = 'C:\ffxi\Ashita-v4beta\plugins\sdk',
    [string]$VcVars   = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat',
    [switch]$Install
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

if (-not (Test-Path $VcVars)) { throw "vcvars32.bat not found at $VcVars" }
if (-not (Test-Path (Join-Path $SdkPath 'Ashita.h'))) { throw "Ashita SDK not found at $SdkPath" }

$test = 'cl /nologo /std:c++20 /EHsc /W4 /WX NoBlinkPolicyTest.cpp /Fe:NoBlinkPolicyTest.exe'
$cl = 'cl /nologo /std:c++20 /EHsc /O2 /W4 /MD /LD /DWIN32 ' +
      "/I`"$SdkPath`" NoBlink.cpp /link /DEF:exports.def /OUT:NoBlink.dll psapi.lib"

cmd /c "`"$VcVars`" >nul 2>&1 && cd /d `"$here`" && $test && NoBlinkPolicyTest.exe && $cl"
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }

Write-Host "built $(Join-Path $here 'NoBlink.dll')"

if ($Install)
{
    $dest = Join-Path (Split-Path -Parent (Split-Path -Parent $SdkPath)) 'plugins\NoBlink.dll'
    Copy-Item (Join-Path $here 'NoBlink.dll') $dest -Force
    Write-Host "installed -> $dest"
}
