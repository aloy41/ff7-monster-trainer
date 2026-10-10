<#
Full move sweep: every move of every catchable species, one battle per roster of up to 3 moves.
-WorkingDir: the lab by default, or an installed product. Rosters go to <workingdir>\save\trainer (slots 1000+).
-Shard k/n + -Tag: this copy's share of the sweep (lab\sweep_parallel.ps1 runs n copies at once).
A hung battle ends that run (testmode quitifmode); the driver records it and restarts after it.
Result: tests\sweep\result<Tag>.txt ("HANG <slot> <species> <moves>" lines, then "DONE").
#>
param([int]$First = 1000, [string]$WorkingDir = $(if ($env:FF7_LAB_WORKINGDIR) { $env:FF7_LAB_WORKINGDIR } else { throw "pass -WorkingDir or set FF7_LAB_WORKINGDIR" }), [string]$Shard = "0/1",
      [string]$Tag = "")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot
$wd = $WorkingDir
$log = "$wd\trainer\trainer.log"
if (-not (Test-Path $log)) { New-Item -ItemType File $log | Out-Null }
$result = "$root\tests\sweep\result$Tag.txt"
$script = "tests\sweep\full$Tag.txt"
if ($First -eq 1000) { Set-Content $result "" -Encoding ascii }
$plan = @{}
while ($true) {
    $lines = & python -I "$root\lab\sweep_moves.py" "$wd\trainer\species.json" "$wd\save\trainer" "$root\$script" $First $Shard
    foreach ($l in $lines) { $p = $l -split ' ', 2; $plan[[int]$p[0]] = $p[1] }
    if (-not $lines) { Add-Content $result "DONE"; break }
    $mark = (Get-Content $log).Count
    & "$PSScriptRoot\run_test.ps1" -Script $script -TimeoutSec 7200 -WorkingDir $wd | Out-Null
    $new = Get-Content $log | Select-Object -Skip $mark
    # FFNx rewrites its log at every launch: count this run's crashes now.
    $exc = (Select-String -Path "$wd\FFNx.log" -Pattern "Unhandled Exception" -ErrorAction SilentlyContinue).Count
    if ($exc) { Add-Content $result "EXCEPTION x$exc in the run starting at $First" }
    $begun = $new | Select-String "SWEEP-BEGIN (\d+)" | ForEach-Object { [int]$_.Matches[0].Groups[1].Value }
    $ended = $new | Select-String "SWEEP-END (\d+)" | ForEach-Object { [int]$_.Matches[0].Groups[1].Value }
    if ($new | Select-String "SWEEP-ALL-DONE") { Add-Content $result "DONE" ; break }
    $last = [int]($begun | Measure-Object -Maximum).Maximum
    if (-not $last) { Add-Content $result "NO-START at $First"; break }
    if ($ended -contains $last) { Add-Content $result "STOPPED after $last (no hang marker)" ; $First = $last + 1; continue }
    Add-Content $result "HANG $last $($plan[$last])"
    $First = $last + 1
}
