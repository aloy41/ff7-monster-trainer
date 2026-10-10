<#
Assemble release\dist\FF7-Monster-Trainer\ from the current builds: the folder a player runs Setup.cmd from.
Contains code and tools only: no game files, no game-derived data (species.json is built on the player's machine).
#>
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot
$dist = Join-Path $PSScriptRoot "dist\FF7-Monster-Trainer"
if (Test-Path $dist) { Remove-Item $dist -Recurse -Force }
New-Item -ItemType Directory -Force "$dist\bin", "$dist\tools" | Out-Null
Copy-Item "$root\hook\build\DINPUT.dll" "$dist\bin\"
Copy-Item "$root\core\target\i686-pc-windows-msvc\release\trainer_core.dll" "$dist\bin\"
foreach ($t in "scene_dump.py", "battle_models.py", "build_species.py", "lgp.py") { Copy-Item "$root\tools\$t" "$dist\tools\" }
Copy-Item "$PSScriptRoot\Setup.ps1", "$PSScriptRoot\Setup.cmd", "$PSScriptRoot\README.md" $dist
# Never ship game files or game-derived data.
$bad = Get-ChildItem $dist -Recurse -File | Where-Object { $_.Extension -in ".lgp", ".bin", ".ff7", ".json" -or $_.Name -like "ff7*.exe" }
if ($bad) { $bad | ForEach-Object { $_.FullName }; throw "dist contains game files or game-derived data" }
Get-ChildItem $dist -Recurse -File | Select-Object @{n = "File"; e = { $_.FullName.Substring($dist.Length + 1) } }, Length
