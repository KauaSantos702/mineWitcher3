# Sets up the dedicated Minecraft that MineWitcher3 starts next to The Witcher 3:
#   %LOCALAPPDATA%\MineWitcher3\Prism\prismlauncher.exe          portable Prism Launcher (unmodified)
#   %LOCALAPPDATA%\MineWitcher3\Prism\instances\MineWitcher3\     the instance (MC 26.3 + Fabric 0.19.5)
#       .minecraft\mods\fabric-api-<ver>.jar, minewitcher3-<ver>.jar
# The native launcher starts it with:  prismlauncher.exe --launch MineWitcher3
#
# Modeled on SkyCraft's tools/package.ps1 + tools/minecraft-bundle (https://github.com/chasmlol/skycraft, MIT).
# It never touches %APPDATA%\.minecraft and never starts Prism or Minecraft.
#
# Idempotent: re-running keeps Prism's own data (accounts.json, libraries, assets, meta, java, worlds,
# options.txt, logs...). It only (re)writes: the Prism program files when the pinned version changes,
# our keys in prismlauncher.cfg / prismlauncher_update.cfg / instance.cfg, mmc-pack.json, and the
# Fabric API + MineWitcher3 jars in the mods folder.
#
#   powershell -ExecutionPolicy Bypass -File tools\setup_minecraft.ps1 [-Rebuild] [-NoJavaOverride]
#     -Rebuild         rebuild the Fabric mod (fabric\gradle25.cmd build) even if the jar exists
#     -NoJavaOverride  don't point the instance at the installed Temurin 25; let Prism pick/download Java
param([switch]$Rebuild, [switch]$NoJavaOverride)
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$repo = Split-Path -Parent $PSScriptRoot

# ---- Pinned versions ---------------------------------------------------------------------------------
# Prism Launcher: SHA-256 as published by GitHub for the release asset (same pin as SkyCraft 0.1.2).
$prismVersion = "11.1.1"
$prismZip     = "PrismLauncher-Windows-MSVC-Portable-$prismVersion.zip"
$prismUrl     = "https://github.com/PrismLauncher/PrismLauncher/releases/download/$prismVersion/$prismZip"
$prismSha256  = "ab35a770fb06d89d2ccc098079db5db329fb4e68f42b72babd8b095efde3d2d7"
$prismLicUrl  = "https://raw.githubusercontent.com/PrismLauncher/PrismLauncher/$prismVersion/LICENSE"
# Fabric API: Modrinth project P7dR8mSH, version id bNnaTiuM (= fabric_api_version in fabric\gradle.properties).
$fabricApiProject   = "P7dR8mSH"
$fabricApiVersionId = "bNnaTiuM"
$fabricApiVersion   = "0.161.0+26.3"
$fabricApiJar       = "fabric-api-0.161.0+26.3.jar"
$fabricApiSha512    = "ed6b2586d6fde11fde8472f5a527c51e99b67026e46f94d4bfd85e7e28ce5ee299173ee16ad576ceb51f39f98d30a811086a6deb1a86a524859cc16e12da109d"
# Instance components (Prism meta uids). 26.x is unobfuscated but Fabric still ships an (identity)
# intermediary component for it, exactly as SkyCraft's mmc-pack.json does.
$mcVersion     = "26.3"
$lwjglVersion  = "3.4.3"
$loaderVersion = "0.19.5"
# 4 GB fixed heap (min = max, pre-touched: no heap growth while the game runs) and ZGC (short pauses);
# the GC log (instance .minecraft\logs\gc.log) is there to measure pauses (research/08 #6). No commas in
# JvmArgs: Set-IniKeys refuses them (Prism's QSettings would read a list).
$maxHeapMb     = 4096
$minHeapMb     = $maxHeapMb
$jvmArgs       = "--enable-native-access=ALL-UNNAMED -Dminewitcher3.startHidden=true -XX:+UseZGC -XX:+AlwaysPreTouch -Xlog:gc*:file=logs/gc.log"
$userAgent     = "MineWitcher3-setup/0.1 (personal educational mod)"

# ---- Paths -------------------------------------------------------------------------------------------
$base  = Join-Path $env:LOCALAPPDATA "MineWitcher3"
$prism = Join-Path $base "Prism"
$inst  = Join-Path $prism "instances\MineWitcher3"
$mods  = Join-Path $inst ".minecraft\mods"
$modVersion = (Select-String -Path "$repo\fabric\gradle.properties" -Pattern '^version=(.+)$').Matches[0].Groups[1].Value.Trim()
$modJar = "$repo\fabric\build\libs\minewitcher3-$modVersion.jar"

$utf8 = New-Object System.Text.UTF8Encoding($false)
function Write-Text([string]$path, [string]$text) { [IO.File]::WriteAllText($path, $text, $utf8) }
function Get-Sha([string]$path, [string]$alg) { (Get-FileHash -LiteralPath $path -Algorithm $alg).Hash.ToLower() }

# Sets keys in the [General] section of a Prism/Qt ini file, leaving every other line as it is.
# Values must not contain ',', ';', '\' or quotes (Qt's ini parser treats those specially).
function Set-IniKeys([string]$path, [System.Collections.IDictionary]$keys) {
    $lines = New-Object System.Collections.Generic.List[string]
    if (Test-Path -LiteralPath $path) { foreach ($l in [IO.File]::ReadAllLines($path)) { $lines.Add($l) } }
    $start = -1
    for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i].Trim() -eq "[General]") { $start = $i; break } }
    if ($start -lt 0) {
        $lines.Insert(0, "[General]")   # new file, or Prism's old flat format: keys before any section
        $start = 0
    }
    $end = $lines.Count
    for ($i = $start + 1; $i -lt $lines.Count; $i++) { if ($lines[$i] -match '^\s*\[') { $end = $i; break } }
    foreach ($k in $keys.Keys) {
        $v = [string]$keys[$k]
        if ($v -match '[,;\\"]') { throw "ini value for $k contains a character Qt would reinterpret: $v" }
        $found = $false
        for ($i = $start + 1; $i -lt $end; $i++) {
            if ($lines[$i] -match ('^\s*' + [regex]::Escape($k) + '\s*=')) { $lines[$i] = "$k=$v"; $found = $true; break }
        }
        if (-not $found) { $lines.Insert($end, "$k=$v"); $end++ }
    }
    Write-Text $path (($lines -join "`r`n") + "`r`n")
}

# ---- 0. Preconditions ----------------------------------------------------------------------------------
$running = @(Get-Process -Name prismlauncher -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($prism, [StringComparison]::OrdinalIgnoreCase) })
if ($running.Count) { throw "Prism Launcher from $prism is running; close it first (it rewrites its config files on exit)." }
$javaRunning = @(Get-CimInstance Win32_Process -Filter "Name='javaw.exe' OR Name='java.exe'" -ErrorAction SilentlyContinue | Where-Object { $_.CommandLine -and $_.CommandLine -like "*instances\MineWitcher3*" })
if ($javaRunning.Count) { throw "The MineWitcher3 Minecraft instance is running; close it first (its mod jars are in use)." }

# ---- 1. Our mod jar ----------------------------------------------------------------------------------
if ($Rebuild -or -not (Test-Path -LiteralPath $modJar)) {
    Write-Host "Building the Fabric mod (fabric\gradle25.cmd build)..."
    Push-Location "$repo\fabric"
    try {
        & "$repo\fabric\gradle25.cmd" build
        if ($LASTEXITCODE) { throw "the Fabric mod didn't build (exit $LASTEXITCODE)" }
    } finally { Pop-Location }
}
if (-not (Test-Path -LiteralPath $modJar)) { throw "missing $modJar after the build" }

# ---- 2. A fresh, empty download directory (nothing in it is ever executed) ------------------------------
$dl = Join-Path ([IO.Path]::GetTempPath()) ("mw3-setup-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory $dl | Out-Null
try {
    New-Item -ItemType Directory $prism -Force | Out-Null

    # ---- 3. Prism Launcher (portable) -----------------------------------------------------------------
    $stampFile = Join-Path $prism "mw3-prism-version.txt"
    $haveVersion = if (Test-Path -LiteralPath $stampFile) { (Get-Content -LiteralPath $stampFile -Raw).Trim() } else { "" }
    if ($haveVersion -ne $prismVersion -or -not (Test-Path -LiteralPath "$prism\prismlauncher.exe")) {
        Write-Host "Downloading Prism Launcher $prismVersion..."
        $zip = Join-Path $dl $prismZip
        Invoke-WebRequest -Uri $prismUrl -OutFile $zip -UseBasicParsing -UserAgent $userAgent
        $got = Get-Sha $zip SHA256
        if ($got -ne $prismSha256) { throw "$prismZip SHA-256 mismatch: got $got, want $prismSha256" }
        $stage = Join-Path $dl "prism"
        Expand-Archive -LiteralPath $zip -DestinationPath $stage   # .NET refuses entries escaping $stage
        $top = $stage
        if (-not (Test-Path -LiteralPath "$stage\prismlauncher.exe")) {
            $sub = @(Get-ChildItem -LiteralPath $stage -Directory | Where-Object { Test-Path -LiteralPath "$($_.FullName)\prismlauncher.exe" })
            if ($sub.Count -ne 1) { throw "unexpected layout in $prismZip (no prismlauncher.exe)" }
            $top = $sub[0].FullName
        }
        # Program files over the old ones; Prism's data (accounts, instances, libraries...) isn't in the zip.
        Get-ChildItem -LiteralPath $top -Force | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $prism -Recurse -Force }
        $lic = Join-Path $dl "LICENSE"
        Invoke-WebRequest -Uri $prismLicUrl -OutFile $lic -UseBasicParsing -UserAgent $userAgent
        Copy-Item -LiteralPath $lic -Destination "$prism\LICENSE-PrismLauncher.txt" -Force
        Write-Text $stampFile $prismVersion
    } else {
        Write-Host "Prism Launcher $prismVersion already installed."
    }
    # portable.txt makes Prism keep all its data next to the exe (the official portable zip ships it).
    if (-not (Test-Path -LiteralPath "$prism\portable.txt")) { Write-Text "$prism\portable.txt" "" }

    # ---- 4. Prism settings (like SkyCraft's defaults/prismlauncher.cfg) --------------------------------
    Set-IniKeys "$prism\prismlauncher.cfg" ([ordered]@{
        Language                        = "en_US"
        ApplicationTheme                = "dark"
        IconTheme                       = "pe_colored"
        AutomaticJavaDownload           = "true"   # also keeps the setup wizard's Java page away
        AutomaticJavaSwitch             = "true"
        UserAskedAboutAutomaticJavaDownload = "true"
        CloseAfterLaunch                = "true"
        QuitAfterGameStop               = "true"
        ShowConsole                     = "false"
        AutoCloseConsole                = "false"
        ShowConsoleOnError              = "true"
        LowMemWarning                   = "false"  # "not enough free RAM" popup over the game
    })
    # Prism's own updater (prismlauncher_updater.exe): no automatic checks/prompts. Prism has no analytics.
    Set-IniKeys "$prism\prismlauncher_update.cfg" ([ordered]@{ auto_check = "false"; allow_beta = "false" })

    # ---- 5. The instance -------------------------------------------------------------------------------
    New-Item -ItemType Directory $mods -Force | Out-Null
    $pack = [ordered]@{
        components = @(
            [ordered]@{ cachedName = "LWJGL 3"; cachedVersion = $lwjglVersion; cachedVolatile = $true; dependencyOnly = $true; uid = "org.lwjgl3"; version = $lwjglVersion },
            [ordered]@{ cachedName = "Minecraft"; cachedRequires = @([ordered]@{ suggests = $lwjglVersion; uid = "org.lwjgl3" }); cachedVersion = $mcVersion; important = $true; uid = "net.minecraft"; version = $mcVersion },
            [ordered]@{ cachedName = "Intermediary Mappings"; cachedRequires = @([ordered]@{ equals = $mcVersion; uid = "net.minecraft" }); cachedVersion = $mcVersion; cachedVolatile = $true; dependencyOnly = $true; uid = "net.fabricmc.intermediary"; version = $mcVersion },
            [ordered]@{ cachedName = "Fabric Loader"; cachedRequires = @([ordered]@{ uid = "net.fabricmc.intermediary" }); cachedVersion = $loaderVersion; uid = "net.fabricmc.fabric-loader"; version = $loaderVersion }
        )
        formatVersion = 1
    }
    Write-Text "$inst\mmc-pack.json" (($pack | ConvertTo-Json -Depth 6) + "`n")

    $instKeys = [ordered]@{
        InstanceType       = "OneSix"
        name               = "MineWitcher3"
        iconKey            = "default"
        notes              = "Managed by MineWitcher3 (tools/setup_minecraft.ps1): The Witcher 3 starts this instance by itself. Re-running the setup replaces the Fabric API and MineWitcher3 jars in mods."
        OverrideJavaArgs   = "true"
        JvmArgs            = $jvmArgs
        OverrideMemory     = "true"
        MinMemAlloc        = "$minHeapMb"
        MaxMemAlloc        = "$maxHeapMb"
        LowMemWarning      = "false"
        OverrideConsole    = "true"
        ShowConsole        = "false"
        AutoCloseConsole   = "false"
        ShowConsoleOnError = "true"
    }
    $javaw = $null
    if (-not $NoJavaOverride) {
        $jdk = Get-ChildItem "$env:ProgramFiles\Eclipse Adoptium" -Directory -Filter "jdk-25*" -ErrorAction SilentlyContinue | Sort-Object Name -Descending | Select-Object -First 1
        if ($jdk -and (Test-Path -LiteralPath "$($jdk.FullName)\bin\javaw.exe")) { $javaw = "$($jdk.FullName)\bin\javaw.exe" }
    }
    if ($javaw) {
        $instKeys.OverrideJavaLocation = "true"
        $instKeys.AutomaticJava        = "false"
        $instKeys.JavaPath             = $javaw.Replace('\', '/')
    } else {
        $instKeys.OverrideJavaLocation = "false"   # global setting: Prism downloads Mojang's Java 25 itself
        $instKeys.AutomaticJava        = "true"
    }
    Set-IniKeys "$inst\instance.cfg" $instKeys

    # ---- 6. Fabric API from Modrinth (pinned version, hash as Modrinth reports it) ---------------------
    $api = Invoke-RestMethod -Uri "https://api.modrinth.com/v2/version/$fabricApiVersionId" -UserAgent $userAgent
    if ($api.project_id -ne $fabricApiProject -or $api.version_number -ne $fabricApiVersion -or -not ($api.game_versions -contains $mcVersion) -or -not ($api.loaders -contains "fabric")) {
        throw "Modrinth version $fabricApiVersionId isn't Fabric API $fabricApiVersion for $mcVersion"
    }
    $file = @($api.files | Where-Object { $_.primary })[0]
    if (-not $file) { $file = $api.files[0] }
    if ($file.filename -ne $fabricApiJar) { throw "Modrinth file name $($file.filename) != $fabricApiJar" }
    if ($file.hashes.sha512 -ne $fabricApiSha512) { throw "Modrinth now reports sha512 $($file.hashes.sha512) for $fabricApiJar (pinned $fabricApiSha512)" }
    if (-not $file.url.StartsWith("https://cdn.modrinth.com/")) { throw "unexpected download host: $($file.url)" }
    $apiDst = Join-Path $mods $fabricApiJar
    if ((Test-Path -LiteralPath $apiDst) -and (Get-Sha $apiDst SHA512) -eq $fabricApiSha512) {
        Write-Host "Fabric API $fabricApiVersion already installed."
        $apiSrc = $null
    } else {
        Write-Host "Downloading Fabric API $fabricApiVersion..."
        $apiSrc = Join-Path $dl $fabricApiJar
        Invoke-WebRequest -Uri $file.url -OutFile $apiSrc -UseBasicParsing -UserAgent $userAgent
        $got = Get-Sha $apiSrc SHA512
        if ($got -ne $fabricApiSha512) { throw "$fabricApiJar SHA-512 mismatch: got $got" }
        if ((Get-Sha $apiSrc SHA1) -ne $file.hashes.sha1) { throw "$fabricApiJar SHA-1 mismatch" }
    }

    # ---- 7. Mods folder: replace only our jars ---------------------------------------------------------
    Get-ChildItem -LiteralPath $mods -File | Where-Object {
        ($_.Name -like "minewitcher3-*.jar") -or ($_.Name -like "fabric-api-*.jar" -and ($apiSrc -or $_.Name -ne $fabricApiJar))
    } | Remove-Item -Force
    if ($apiSrc) { Copy-Item -LiteralPath $apiSrc -Destination $apiDst -Force }
    Copy-Item -LiteralPath $modJar -Destination (Join-Path $mods "minewitcher3-$modVersion.jar") -Force

    Write-Text "$prism\THIRD-PARTY.txt" @"
MineWitcher3 sets up this copy of Minecraft's launcher so The Witcher 3 can start Minecraft by itself.
It doesn't include Minecraft: Prism Launcher downloads it after you sign in with a Microsoft account
that owns Minecraft: Java Edition.

Prism Launcher $prismVersion (this folder, the unmodified portable Windows build, downloaded from GitHub)
  License: GNU General Public License v3.0 (LICENSE-PrismLauncher.txt)
  Source:  https://github.com/PrismLauncher/PrismLauncher/tree/$prismVersion

Fabric API $fabricApiVersion (instances/MineWitcher3/.minecraft/mods/$fabricApiJar, from Modrinth)
  License: Apache License 2.0
  Source:  https://github.com/FabricMC/fabric

MineWitcher3 (instances/MineWitcher3/.minecraft/mods/minewitcher3-$modVersion.jar)
  License: MIT (derived from SkyCraft by chasmlol, MIT)
"@
    Write-Text "$base\setup-version.txt" "MineWitcher3 $modVersion, Prism Launcher $prismVersion, $fabricApiJar, Minecraft $mcVersion, Fabric Loader $loaderVersion"
} finally {
    Remove-Item -LiteralPath $dl -Recurse -Force -ErrorAction SilentlyContinue
}

# ---- 8. Verify ---------------------------------------------------------------------------------------
$ok = $true
function Check([bool]$cond, [string]$what) { if ($cond) { Write-Host "  ok   $what" } else { Write-Host "  FAIL $what"; $script:ok = $false } }
Write-Host "Verifying $prism"
Check (Test-Path -LiteralPath "$prism\prismlauncher.exe") "prismlauncher.exe"
Check (Test-Path -LiteralPath "$prism\portable.txt") "portable.txt"
$packJson = Get-Content -LiteralPath "$inst\mmc-pack.json" -Raw | ConvertFrom-Json
$uids = @($packJson.components | ForEach-Object { "$($_.uid)=$($_.version)" })
Check (($uids -join " ") -eq "org.lwjgl3=$lwjglVersion net.minecraft=$mcVersion net.fabricmc.intermediary=$mcVersion net.fabricmc.fabric-loader=$loaderVersion") "mmc-pack.json: $($uids -join ', ')"
$cfg = Get-Content -LiteralPath "$inst\instance.cfg" -Raw
Check ($cfg -match "(?m)^JvmArgs=$([regex]::Escape($jvmArgs))\r?$") "instance.cfg JvmArgs"
Check ($cfg -match "(?m)^MaxMemAlloc=$maxHeapMb\r?$") "instance.cfg MaxMemAlloc=$maxHeapMb"
Check ($cfg -match "(?m)^MinMemAlloc=$minHeapMb\r?$") "instance.cfg MinMemAlloc=$minHeapMb"
if ($javaw) { Check (Test-Path -LiteralPath $javaw) "Java: $javaw" } else { Write-Host "  info Java: Prism picks/downloads Java 25" }
$jarDst = Join-Path $mods "minewitcher3-$modVersion.jar"
Check ((Get-Sha $jarDst SHA256) -eq (Get-Sha $modJar SHA256)) "minewitcher3-$modVersion.jar sha256 $(Get-Sha $jarDst SHA256)"
Check ((Get-Sha $apiDst SHA512) -eq $fabricApiSha512) "$fabricApiJar sha512 matches Modrinth"
$modList = @(Get-ChildItem -LiteralPath $mods -File | ForEach-Object Name)
Write-Host "  mods: $($modList -join ', ')"
if (-not $ok) { throw "verification failed" }
Write-Host ""
Write-Host "Done. Next (once): open $prism\prismlauncher.exe, add your Microsoft account, then the game can use:"
Write-Host "  `"$prism\prismlauncher.exe`" --launch MineWitcher3"
