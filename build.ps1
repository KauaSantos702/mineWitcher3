# Compila native\ (WitcherNativeBridge.asi + mw3.asi) em Release x64 com o CMake do Visual Studio 2022.
$ErrorActionPreference = "Stop"
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio com ferramentas C++ nao encontrado." }
$cmake = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$src = Join-Path $PSScriptRoot "native"
$bld = Join-Path $src "build"

& $cmake -S $src -B $bld -G "Visual Studio 17 2022" -A x64
if ($LASTEXITCODE -ne 0) { throw "cmake configure falhou" }
& $cmake --build $bld --config Release
if ($LASTEXITCODE -ne 0) { throw "cmake build falhou" }
Get-ChildItem (Join-Path $bld "Release\*.asi") | ForEach-Object { "{0}  {1:N0} bytes" -f $_.Name, $_.Length }
