# Installs MineWitcher3: scripts (mods\modMineWitcher3), ASI Loader + mw3.asi + WitcherNativeBridge.asi (bin\x64_dx12)
# and the mod's keys in input.settings. Build first with .\build.ps1.
# -GameDir / -Settings are optional: the game folder (Steam libraries, GOG) and your Documents folder
# (any language, OneDrive or not) are detected automatically.
param(
    [string]$GameDir  = "",
    [string]$Settings = "",
    [switch]$Uninstall
)
$ErrorActionPreference = "Stop"

function Find-GameDir {
    $candidates = @()
    # Steam: every library listed in libraryfolders.vdf
    $steam = (Get-ItemProperty "HKCU:\Software\Valve\Steam" -ErrorAction SilentlyContinue).SteamPath
    if ($steam) {
        $steam = $steam.Replace('/', '\')
        $candidates += Join-Path $steam "steamapps\common\The Witcher 3"
        $vdf = Join-Path $steam "steamapps\libraryfolders.vdf"
        if (Test-Path $vdf) {
            foreach ($m in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) {
                $lib = $m.Groups[1].Value.Replace('\\', '\')
                $candidates += Join-Path $lib "steamapps\common\The Witcher 3"
            }
        }
    }
    # GOG
    foreach ($k in "HKLM:\SOFTWARE\WOW6432Node\GOG.com\Games\1495134320", "HKLM:\SOFTWARE\WOW6432Node\GOG.com\Games\1207664663", "HKLM:\SOFTWARE\GOG.com\Games\1495134320") {
        $p = (Get-ItemProperty $k -ErrorAction SilentlyContinue).path
        if ($p) { $candidates += $p }
    }
    foreach ($c in $candidates) {
        if (Test-Path (Join-Path $c "bin\x64_dx12\witcher3.exe")) { return $c }
    }
    return $null
}

if (-not $GameDir) { $GameDir = Find-GameDir }
if (-not $GameDir -or -not (Test-Path (Join-Path $GameDir "bin\x64_dx12\witcher3.exe"))) {
    throw "The Witcher 3 (bin\x64_dx12\witcher3.exe) not found. Pass it with -GameDir <folder>."
}
if (-not $Settings) {
    # Real Documents folder: follows OneDrive redirection and localized names (Documentos, Dokumente...)
    $Settings = Join-Path ([Environment]::GetFolderPath("MyDocuments")) "The Witcher 3\input.settings"
}
Write-Host "Game: $GameDir"
$modSrc   = Join-Path $PSScriptRoot "mod\modMineWitcher3"
$modDst   = Join-Path $GameDir "mods\modMineWitcher3"
$binDir   = Join-Path $GameDir "bin\x64_dx12"
$buildDir = Join-Path $PSScriptRoot "native\build\Release"
$natives  = @(
    @{ Src = Join-Path $PSScriptRoot "third_party\UltimateASILoader\winmm.dll"; Name = "winmm.dll" },
    @{ Src = Join-Path $buildDir "WitcherNativeBridge.asi"; Name = "WitcherNativeBridge.asi" },
    @{ Src = Join-Path $buildDir "mw3.asi"; Name = "mw3.asi" }
)
$iniSrc = Join-Path $PSScriptRoot "native\mw3.ini"
$iniDst = Join-Path $binDir "mw3.ini"
$sections = "[Exploration]", "[Combat]", "[Swimming]", "[Horse]"
$binds    = Get-Content (Join-Path $PSScriptRoot "input\mw3_input.settings") | Where-Object { $_ -match '^IK_' }

if ($Uninstall) {
    if (Test-Path $modDst) { Remove-Item $modDst -Recurse -Force }
    foreach ($n in $natives) {
        $dst = Join-Path $binDir $n.Name
        if (Test-Path $dst) { Remove-Item $dst -Force }
    }
    if (Test-Path $iniDst) { Remove-Item $iniDst -Force }
    if (Test-Path "$Settings.mw3bak") { Copy-Item "$Settings.mw3bak" $Settings -Force }
    Write-Host "MineWitcher3 removido."
    return
}

foreach ($n in $natives) {
    if (-not (Test-Path $n.Src)) { throw "Falta $($n.Src). Rode .\build.ps1 primeiro." }
}

New-Item -ItemType Directory -Force (Split-Path $modDst) | Out-Null
if (Test-Path $modDst) { Remove-Item $modDst -Recurse -Force }
Copy-Item $modSrc $modDst -Recurse -Force
Write-Host "Scripts copiados para $modDst"

foreach ($n in $natives) {
    $dst = Join-Path $binDir $n.Name
    # Nao sobrescreve um winmm.dll de outro mod que nao seja o ASI Loader.
    if ($n.Name -eq "winmm.dll" -and (Test-Path $dst) -and
        (Get-Item $dst).VersionInfo.FileDescription -ne "Ultimate ASI Loader") {
        throw "Ja existe um winmm.dll diferente em $binDir; remova ou renomeie antes."
    }
    Copy-Item $n.Src $dst -Force
}
Write-Host "ASI Loader, WitcherNativeBridge.asi e mw3.asi copiados para $binDir"
# So instala o mw3.ini na primeira vez: depois ele e do usuario.
if (-not (Test-Path $iniDst)) { Copy-Item $iniSrc $iniDst; Write-Host "mw3.ini criado em $binDir" }

if (-not (Test-Path $Settings)) { throw "input.settings nao encontrado em $Settings (use -Settings)" }
$lines = Get-Content $Settings
# O backup guarda o arquivo original; so e criado na primeira instalacao.
if (-not (Test-Path "$Settings.mw3bak")) { Copy-Item $Settings "$Settings.mw3bak" -Force }
# Regrava as linhas do mod para que atalhos novos entrem em instalacoes antigas.
$clean = $lines | Where-Object { $_ -notmatch 'Action=MW3_' }
$out = foreach ($l in $clean) { $l; if ($sections -contains $l.Trim()) { $binds } }
Set-Content $Settings $out -Encoding Ascii
Write-Host "Atalhos F6-F9 gravados em input.settings (backup original: $Settings.mw3bak)"
