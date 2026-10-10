<#
FF7: Monster Trainer - setup.

Builds a separate, playable copy of FINAL FANTASY VII (2026 Steam release) with the Monster Trainer mod. The Steam
install is only read, never modified, so the normal game and its achievements/cloud saves keep working.

  1. finds the Steam install (app 3837340) or uses -GameDir
  2. copies it to -Target (default: "<library>\FF7 Monster Trainer", ~3.5 GB)
  3. downloads FFNx-Steam 1.24.3.0 from its GitHub release and checks its SHA-256
  4. sets up the classic engine FFNx runs (ff7_en.exe) exactly as FFNx documents for this release
  5. installs the mod (DINPUT.dll, trainer\trainer_core.dll) and builds the monster data from YOUR scene.bin,
     battle.lgp and kernel.bin (tools\*.py, Python 3.10+, standard library only)
  6. writes a Desktop shortcut

Run from the release folder:  powershell -ExecutionPolicy Bypass -File Setup.ps1 [-GameDir <path>] [-Target <path>]
Re-running updates the mod in an existing target without copying the game again.
#>
param([string]$GameDir = "", [string]$Target = "", [switch]$NoShortcut)
$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$FFNX_URL = "https://github.com/julianxhokaxhiu/FFNx/releases/download/1.24.3/FFNx-Steam-v1.24.3.0.zip"
$FFNX_SHA256 = "2be45f486974f0979b849d0525eb66427df62483ec99e9339e9773e9e52afc0d"

function Step($msg) { Write-Host "==> $msg" -ForegroundColor Cyan }

# ---- 1. find the game -----------------------------------------------------------------------------------------
if (-not $GameDir) {
    Step "Looking for FINAL FANTASY VII (Steam app 3837340)"
    $steam = (Get-ItemProperty "HKCU:\Software\Valve\Steam" -ErrorAction SilentlyContinue).SteamPath
    $libs = @()
    if ($steam) {
        $vdf = Join-Path $steam "steamapps\libraryfolders.vdf"
        $libs += $steam
        if (Test-Path $vdf) {
            $libs += (Select-String -Path $vdf -Pattern '"path"\s+"([^"]+)"' | ForEach-Object { $_.Matches[0].Groups[1].Value -replace '\\\\', '\' })
        }
    }
    foreach ($lib in ($libs | Select-Object -Unique)) {
        $m = Join-Path $lib "steamapps\appmanifest_3837340.acf"
        if (Test-Path $m) {
            $dir = (Select-String -Path $m -Pattern '"installdir"\s+"([^"]+)"').Matches[0].Groups[1].Value
            $GameDir = Join-Path $lib "steamapps\common\$dir"
            break
        }
    }
    if (-not $GameDir) { throw "FINAL FANTASY VII (Steam, 2026 release) not found. Pass -GameDir <install folder>." }
}
$classic = Join-Path $GameDir "ff7\resources\ff7_1.02\ff7_en"
if (-not (Test-Path $classic)) { throw "$GameDir doesn't look like the 2026 FF7 release (missing ff7\resources\ff7_1.02\ff7_en)" }
function Full($p) { [IO.Path]::GetFullPath($p).TrimEnd('\') }
$GameDir = Full $GameDir
Write-Host "    game: $GameDir"
if (-not $Target) {
    # Steam layout <library>\steamapps\common\<game> -> <library>\FF7 Monster Trainer; anything else -> next to the game.
    $base = if ($GameDir -match '\\steamapps\\common\\[^\\]+$') { Split-Path (Split-Path (Split-Path $GameDir)) } else { Split-Path $GameDir }
    $Target = Join-Path $base "FF7 Monster Trainer"
}
$Target = Full $Target
# The Steam install is only ever read: refuse any target that is it, is inside it, or contains it.
if ($Target -ieq $GameDir -or $Target.StartsWith("$GameDir\", 'OrdinalIgnoreCase') -or $GameDir.StartsWith("$Target\", 'OrdinalIgnoreCase')) {
    throw "Target '$Target' overlaps the game install '$GameDir'; choose a separate folder"
}
$wd = Join-Path $Target "ff7\workingdir"
Write-Host "    target: $Target"
$drive = [IO.Path]::GetPathRoot($Target)
$free = (Get-PSDrive -Name $drive.Substring(0, 1)).Free
$need = if (Test-Path (Join-Path $wd "data\battle\battle.lgp")) { 200MB } else { 4GB }
if ($free -lt $need) { throw ("Not enough space on {0}: {1:N1} GB free, about {2:N1} GB needed" -f $drive, ($free / 1GB), ($need / 1GB)) }
# Temporary files go next to the target, not to %TEMP% (the system drive may be full).
$tmp = Join-Path $Target "_setup_tmp"
New-Item -ItemType Directory -Force $tmp | Out-Null

# ---- 2. copy ------------------------------------------------------------------------------------------------------
# Always run: robocopy is incremental (unchanged files are skipped), so an interrupted copy is completed on re-run.
# /XF keeps the files Setup itself modifies or adds from being overwritten with the Steam originals.
Step "Copying the game (about 3.5 GB the first time; the Steam install is only read)"
robocopy $GameDir $Target /E /COPY:DAT /R:1 /W:1 /NFL /NDL /NP /MT:16 /XF FFNx.toml | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy failed (exit $LASTEXITCODE)" }

# ---- 3./4. FFNx ---------------------------------------------------------------------------------------------------
if (-not (Test-Path (Join-Path $wd "AF3DN.P"))) {
    Step "Downloading FFNx-Steam 1.24.3.0"
    $zip = Join-Path $tmp "FFNx-Steam-v1.24.3.0.zip"
    Invoke-WebRequest -Uri $FFNX_URL -OutFile $zip -UseBasicParsing
    $hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
    if ($hash -ne $FFNX_SHA256) { Remove-Item $zip; throw "FFNx download hash mismatch ($hash)" }
    Expand-Archive $zip -DestinationPath $wd -Force
    Remove-Item $zip
}
Step "Setting up the classic engine (as FFNx documents for the 2026 release)"
Copy-Item $classic (Join-Path $wd "ff7_en.exe") -Force
New-Item -ItemType Directory -Force (Join-Path $wd "data\kernel") | Out-Null
Copy-Item (Join-Path $wd "data\lang-ja\kernel\window.bin") (Join-Path $wd "data\kernel\windows.bin") -Force
Set-Content (Join-Path $wd "steam_appid.txt") "3837340" -NoNewline -Encoding ascii
$toml = Join-Path $wd "FFNx.toml"
$text = [IO.File]::ReadAllText($toml)
$text = $text -replace '(?m)^enable_devtools = .*$', 'enable_devtools = false' -replace '(?m)^save_exe_data = .*$', 'save_exe_data = false'
[IO.File]::WriteAllText($toml, $text, (New-Object Text.UTF8Encoding $false))  # keep the encoding, no BOM

# ---- 5. the mod ---------------------------------------------------------------------------------------------------
Step "Installing the Monster Trainer"
New-Item -ItemType Directory -Force (Join-Path $wd "trainer") | Out-Null
Copy-Item (Join-Path $here "bin\DINPUT.dll") (Join-Path $wd "DINPUT.dll") -Force
Copy-Item (Join-Path $here "bin\trainer_core.dll") (Join-Path $wd "trainer\trainer_core.dll") -Force
Remove-Item (Join-Path $wd "trainer\test_script.txt"), (Join-Path $wd "trainer\test_visible.flag") -ErrorAction SilentlyContinue
Copy-Item (Join-Path $here "README.md") (Join-Path $Target "README.md") -Force  # the controls, next to the game

Step "Building the monster data from your game files"
$py = $null
foreach ($cand in "python", "py") {
    $c = (Get-Command $cand -ErrorAction SilentlyContinue).Source
    if (-not $c) { continue }
    # Ask it: the Microsoft Store "python" stub (no Python installed) prints nothing useful and fails, while a real
    # Store-installed Python (also under WindowsApps) answers like any other.
    $v = & $c -c "import sys; print(sys.version_info >= (3, 10))" 2>$null
    if ($v -eq "True") { $py = $c; break }
}
if (-not $py) { throw "Python 3.10+ is needed once, to read your game's monster data (install it from python.org)" }
$out = Join-Path $tmp "data"
New-Item -ItemType Directory -Force $out | Out-Null
$data = Join-Path $wd "data"
$species = Join-Path $wd "trainer\species.json"
Remove-Item $species -ErrorAction SilentlyContinue  # never keep a stale one if a step below fails
function Run-Py($script, [string[]]$a) {
    & $py -I -B (Join-Path $here "tools\$script") @a | Out-Host  # -B: no __pycache__ in the release folder
    if ($LASTEXITCODE -ne 0) { throw "$script failed (exit $LASTEXITCODE)" }
}
Run-Py "scene_dump.py" @((Join-Path $data "lang-en\battle\scene.bin"), $out)
Run-Py "battle_models.py" @((Join-Path $data "battle\battle.lgp"), (Join-Path $out "dex.json"))
Run-Py "build_species.py" @((Join-Path $out "dex.json"), $species)
if (-not (Test-Path $species)) { throw "building species.json failed" }
Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue

# ---- 6. shortcut --------------------------------------------------------------------------------------------------
if (-not $NoShortcut) {
    $lnk = Join-Path ([Environment]::GetFolderPath("Desktop")) "FF7 Monster Trainer.lnk"
    $sh = New-Object -ComObject WScript.Shell
    $s = $sh.CreateShortcut($lnk)
    $s.TargetPath = Join-Path $wd "ff7_en.exe"
    $s.WorkingDirectory = $wd
    $s.Description = "FINAL FANTASY VII: Monster Trainer"
    $s.Save()
    Write-Host "    shortcut: $lnk"
}
Step "Done. Start Steam, then launch 'FF7 Monster Trainer'. $Target\README.md has the controls."
