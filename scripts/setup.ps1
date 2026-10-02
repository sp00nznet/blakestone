<#
  setup.ps1 - the Quick start (README, Getting Started). Setup.cmd runs this.

  From a plain download to two launchers in this folder:
    1. check the tools (Python 3, CMake, Visual Studio 2022 C++ build tools,
       git; ffmpeg is optional) and offer to install what is missing
    2. find your copy of the games (Steam or GOG, or ask)
    3. copy the game files into original\aog and original\ps
    4. fetch the pcrecomp toolkit next to this folder
    5. lift each game's executable to C, then build both with CMake/MSVC
    6. put a shortcut for each game in this folder

  Every step is skipped when its result is already there, so running it again
  after a failure carries on from where it stopped. Details go to setup.log.
  It runs exactly the commands README "Step by step" lists.

  Nothing is downloaded or bundled from either game: the files come from your
  own install, and everything generated from them stays in work\ and build\.
#>
param([string]$GameDir = "", [switch]$Yes)
$ErrorActionPreference = "Stop"
$Root = Split-Path $PSScriptRoot -Parent
Set-Location $Root
$Log = Join-Path $Root "setup.log"
"=== setup $(Get-Date -Format s)" | Out-File $Log -Encoding utf8

# The toolkit revision this game needs (README, "Building from source").
$PcrecompUrl = "https://github.com/sp00nznet/pcrecomp.git"
# work/blakestone-integration carries the toolkit PRs this needs until they
# are merged; then this becomes a release tag. (CHANGELOG, "Toolkit")
$PcrecompRef = "work/blakestone-integration"

function Say($m) { Write-Host $m; $m | Out-File $Log -Append -Encoding utf8 }
function Fail($m) {
    Write-Host ""
    Write-Host $m -ForegroundColor Red
    Write-Host "Details are in $Log"
    exit 1
}
function Run($what, [scriptblock]$cmd) {
    Say "  $what"
    # native tools write progress to stderr; under "Stop" PowerShell 5.1 would
    # turn every such line into a terminating error
    $ErrorActionPreference = "Continue"
    $out = & $cmd 2>&1
    $out | Out-File $Log -Append -Encoding utf8
    if ($LASTEXITCODE -ne 0) { Fail "$what failed." }
}
function Ask($q) {
    if ($Yes) { return $true }
    $a = Read-Host "$q [y/N]"
    return $a -match '^(y|yes)$'
}
function Have($exe) { return [bool](Get-Command $exe -ErrorAction SilentlyContinue) }

# ---- 1. tools ---------------------------------------------------------------
Say "Checking tools..."
$need = @()
# `python` may be the Microsoft Store alias that only opens the Store; the
# py launcher is what a python.org install puts on PATH.
$py = if (Have "py") { "py" } elseif ((Have "python") -and ((& python --version 2>&1) -match "Python 3")) { "python" } else { $null }
if (-not $py) { $need += @{ name = "Python 3"; id = "Python.Python.3.12"; size = "about 30 MB"; why = "runs the lifter (tools\lift.py)" } }
if (-not (Have "cmake")) { $need += @{ name = "CMake"; id = "Kitware.CMake"; size = "about 35 MB"; why = "configures the build" } }
if (-not (Have "git")) { $need += @{ name = "Git"; id = "Git.Git"; size = "about 60 MB"; why = "fetches the pcrecomp toolkit" } }
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = if (Test-Path $vswhere) { & $vswhere -latest -version "[17.0,18.0)" -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath } else { $null }
if (-not $vs) { $need += @{ name = "Visual Studio 2022 Build Tools (C++)"; id = "Microsoft.VisualStudio.2022.BuildTools"; size = "about 2 GB"; why = "compiles the lifted C"; args = "--override `"--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended`"" } }

foreach ($n in $need) {
    Say "Missing: $($n.name) -- needed because it $($n.why). Download: $($n.size)."
    if (-not (Have "winget")) { Fail "Install $($n.name) yourself (winget is not available), then run Setup again." }
    if (-not (Ask "Install $($n.name) now with winget?")) { Fail "Setup needs $($n.name). Install it and run Setup again." }
    $wargs = @("install", "--id", $n.id, "-e", "--accept-package-agreements", "--accept-source-agreements")
    if ($n.args) { $wargs += $n.args }
    Run "installing $($n.name)" { winget @wargs }
}
if ($need.Count) {
    # a fresh install is on the machine PATH but not in this window's
    $env:Path = [Environment]::GetEnvironmentVariable("Path", "Machine") + ";" + [Environment]::GetEnvironmentVariable("Path", "User")
    $py = if (Have "py") { "py" } else { "python" }
}
if (-not (Have "ffmpeg")) { Say "  (ffmpeg not found: everything works except --record. winget install Gyan.FFmpeg adds it.)" }

# ---- 2. find the games ----------------------------------------------------------
$games = @(
    @{ id = "aog"; exe = "BS_AOG.EXE"; name = "Blake Stone - Aliens of Gold"; steam = "Blake Stone Aliens of Gold\Blake Stone - Aliens of Gold" },
    @{ id = "ps";  exe = "BS_FIRE.EXE"; name = "Blake Stone - Planet Strike"; steam = "Blake Stone Planet Strike\Blake Stone - Planet Strike" }
)

function Steam-Libraries {
    $libs = @()
    foreach ($k in "HKCU:\Software\Valve\Steam", "HKLM:\SOFTWARE\WOW6432Node\Valve\Steam") {
        $p = (Get-ItemProperty $k -ErrorAction SilentlyContinue)
        $base = if ($p.SteamPath) { $p.SteamPath } elseif ($p.InstallPath) { $p.InstallPath } else { $null }
        if (-not $base) { continue }
        $libs += $base
        $vdf = Join-Path $base "steamapps\libraryfolders.vdf"
        if (Test-Path $vdf) {
            Select-String -Path $vdf -Pattern '"path"\s+"([^"]+)"' | ForEach-Object { $libs += ($_.Matches[0].Groups[1].Value -replace '\\\\', '\') }
        }
    }
    # Libraries Steam does not know about any more (another Windows install)
    Get-PSDrive -PSProvider FileSystem | ForEach-Object { $libs += "$($_.Root)SteamLibrary"; $libs += "$($_.Root)Steam" }
    return $libs | Select-Object -Unique
}

function Find-Game($g) {
    if ($GameDir) {
        foreach ($c in @($GameDir, (Join-Path $GameDir $g.steam))) { if (Test-Path (Join-Path $c $g.exe)) { return $c } }
    }
    foreach ($lib in Steam-Libraries) {
        $c = Join-Path $lib "steamapps\common\$($g.steam)"
        if (Test-Path (Join-Path $c $g.exe)) { return $c }
    }
    foreach ($c in "C:\GOG Games\Blake Stone", "C:\GOG Games\Blake Stone Aliens of Gold", "C:\GOG Games\Blake Stone Planet Strike") {
        if (Test-Path (Join-Path $c $g.exe)) { return $c }
    }
    return $null
}

$found = @()
foreach ($g in $games) {
    $dest = Join-Path $Root "original\$($g.id)"
    if (Test-Path (Join-Path $dest $g.exe)) { Say "$($g.name): already copied."; $found += $g; continue }
    $src = Find-Game $g
    if (-not $src -and -not $Yes) {
        $ans = Read-Host "Where is $($g.name)? (the folder holding $($g.exe); Enter to skip this game)"
        if ($ans -and (Test-Path (Join-Path $ans $g.exe))) { $src = $ans }
    }
    if (-not $src) { Say "$($g.name): not found, skipping."; continue }
    Say "$($g.name): copying from $src"
    New-Item -ItemType Directory -Force $dest | Out-Null
    # the game's own files only -- not the DOSBox the store version wraps it in
    Get-ChildItem $src -File | Where-Object { $_.Extension -notin ".conf", ".pdf", ".map" } |
        Copy-Item -Destination $dest
    $found += $g
}
if (-not $found.Count) { Fail "No copy of either game was found. Run Setup again and give the folder that holds BS_AOG.EXE or BS_FIRE.EXE." }

# ---- 3. the toolkit ----------------------------------------------------------------
if ($env:PCRECOMP_HOME -and (Test-Path "$env:PCRECOMP_HOME\tools\lift\lift16.py")) {
    $pcr = $env:PCRECOMP_HOME
} else {
    $pcr = Join-Path (Split-Path $Root -Parent) "pcrecomp"
    if (-not (Test-Path "$pcr\tools\lift\lift16.py")) {
        Run "fetching pcrecomp into $pcr" { git clone --quiet $PcrecompUrl $pcr }
    }
    Run "checking out pcrecomp $PcrecompRef" { git -C $pcr fetch --quiet origin; git -C $pcr checkout --quiet $PcrecompRef }
}
$env:PCRECOMP_HOME = $pcr

# ---- 4. lift + build -------------------------------------------------------------
foreach ($g in $found) {
    Run "lifting $($g.name) to C" { & $py tools\lift.py $g.id }
}
Run "configuring the build" { cmake -B build -G "Visual Studio 17 2022" -A x64 "-DPCRECOMP_HOME=$pcr" }
Run "compiling (a few minutes: the lifted C is large)" { cmake --build build --config Release -- -m }

# ---- 5. shortcuts -----------------------------------------------------------------
$shell = New-Object -ComObject WScript.Shell
foreach ($g in $found) {
    $exe = Join-Path $Root "build\Release\bstone_$($g.id).exe"
    if (-not (Test-Path $exe)) { Fail "The build finished but $exe is missing." }
    $lnk = $shell.CreateShortcut((Join-Path $Root "$($g.name).lnk"))
    $lnk.TargetPath = $exe
    $lnk.WorkingDirectory = $Root
    $lnk.Save()
    Say "Ready: $($g.name).lnk"
}
Say "Done. Double-click a shortcut to play. Alt+Enter toggles fullscreen."
