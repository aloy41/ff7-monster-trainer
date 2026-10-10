<#
Deploy the current build into the lab install (never the Steam folder).
  hook\build\DINPUT.dll                    -> <workingdir>\DINPUT.dll
  core\target\i686...\trainer_core.dll     -> <workingdir>\trainer\trainer_core.dll
  out\species.json                         -> <workingdir>\trainer\species.json
Refuses to run while ff7_en is running (the DLLs would be locked anyway).
#>
param([string]$WorkingDir = $(if ($env:FF7_LAB_WORKINGDIR) { $env:FF7_LAB_WORKINGDIR } else { throw "pass -WorkingDir or set FF7_LAB_WORKINGDIR" }))
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot
if ($WorkingDir -like "*\steamapps\*") { throw "refusing to deploy into the Steam install" }
$exe = [IO.Path]::GetFullPath("$WorkingDir\ff7_en.exe")
if (Get-Process ff7_en -ErrorAction SilentlyContinue | Where-Object { $_.Path -ieq $exe }) { throw "ff7_en is running in $WorkingDir; close it first" }
New-Item -ItemType Directory -Force "$WorkingDir\trainer" | Out-Null
Copy-Item "$root\hook\build\DINPUT.dll" "$WorkingDir\DINPUT.dll" -Force
Copy-Item "$root\core\target\i686-pc-windows-msvc\release\trainer_core.dll" "$WorkingDir\trainer\trainer_core.dll" -Force
Copy-Item "$root\out\species.json" "$WorkingDir\trainer\species.json" -Force
# Test rosters (sidecar saves that tests/*.txt load with `core load <slot>`)
New-Item -ItemType Directory -Force "$WorkingDir\save\trainer" | Out-Null
Copy-Item "$root\tests\rosters\*.json" "$WorkingDir\save\trainer\" -Force
Get-Item "$WorkingDir\DINPUT.dll", "$WorkingDir\trainer\trainer_core.dll", "$WorkingDir\trainer\species.json" |
    Select-Object FullName, Length, LastWriteTime
