<#
The full move sweep split across several game copies at once (each on the private desktop, muted).
  sweep_parallel.ps1 -WorkingDirs <copy 1>\ff7\workingdir, <copy 2>\ff7\workingdir, ...
Copy k sweeps every n-th roster (sweep_driver -Shard k/n). Waits for all; prints the merged result
(tests\sweep\result_<Name><k>.txt per copy). Every copy must hold the same build (DLLs + species.json).
#>
param([Parameter(Mandatory)][string[]]$WorkingDirs, [string]$Name = "")  # -Name: keeps two sweeps' result files apart
$ErrorActionPreference = "Stop"
$n = $WorkingDirs.Count
foreach ($wd in $WorkingDirs) {
    foreach ($f in "DINPUT.dll", "trainer\trainer_core.dll", "trainer\species.json") {
        $h = (Get-FileHash "$wd\$f" -Algorithm MD5).Hash
        $h0 = (Get-FileHash "$($WorkingDirs[0])\$f" -Algorithm MD5).Hash
        if ($h -ne $h0) { throw "$wd\$f differs from $($WorkingDirs[0]) - deploy the same build everywhere" }
    }
}
$jobs = for ($k = 0; $k -lt $n; $k++) {
    Start-Job -ArgumentList $PSScriptRoot, $WorkingDirs[$k], "$k/$n", "_$Name$k" -ScriptBlock {
        param($lab, $wd, $shard, $tag)
        & "$lab\sweep_driver.ps1" -WorkingDir $wd -Shard $shard -Tag $tag
    }
    Start-Sleep -Seconds 3   # stagger the launches
}
$jobs | Wait-Job | Receive-Job | Out-Null
$root = Split-Path $PSScriptRoot
for ($k = 0; $k -lt $n; $k++) { "== copy $k ($($WorkingDirs[$k]))"; Get-Content "$root\tests\sweep\result_$Name$k.txt" }
