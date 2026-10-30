# Windows native GUI build + tests with MSVC (CI: windows-msvc). Run from any PowerShell:
#   powershell -ExecutionPolicy Bypass -File scripts\gui\test-windows.ps1 [-BuildDir build-gui-msvc] [-NoDeps]
# Needs Visual Studio 2022 (C++ workload). Installs cmake/ninja with winget when missing.
param([string]$BuildDir = "", [switch]$NoDeps)
$ErrorActionPreference = "Continue"
$Root = (Resolve-Path "$PSScriptRoot\..\..").Path
$UI = "$Root\Source\FUSE\UI"
if (-not $BuildDir) { $BuildDir = "$Root\build-gui-msvc" }
$results = @(); $failed = $false
function Step($name, [scriptblock]$cmd) {
    Write-Host "`n==> $name"; & $cmd
    if ($LASTEXITCODE -eq 0) { $script:results += "PASS  $name" } else { $script:results += "FAIL  $name"; $script:failed = $true }
}
if (-not $NoDeps) {
    foreach ($t in @(@("cmake","Kitware.CMake"), @("ninja","Ninja-build.Ninja"))) {
        if (-not (Get-Command $t[0] -ErrorAction SilentlyContinue)) { Step "deps ($($t[0]))" { winget install -e --id $t[1] --accept-source-agreements --accept-package-agreements } }
    }
}
if (-not (Get-Command cl -ErrorAction SilentlyContinue)) {   # enter the VS developer environment
    $vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { Write-Host "Visual Studio with C++ tools not found"; exit 1 }
    cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && set" | ForEach-Object { if ($_ -match '^([^=]+)=(.*)$') { Set-Item "env:$($matches[1])" $matches[2] } }
}
Step "configure" { cmake -S $UI -B $BuildDir -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl }
Step "build" { cmake --build $BuildDir -j }
Step "test" { ctest --test-dir $BuildDir --output-on-failure -j4 }
Write-Host "`n==== windows-msvc summary ===="; $results | ForEach-Object { Write-Host "  $_" }
if ($failed) { Write-Host "RESULT: windows-msvc FAIL"; exit 1 } else { Write-Host "RESULT: windows-msvc PASS"; exit 0 }
