<#
Run many test scripts across several game copies at once (each copy runs one game at a time, on the private
desktop, muted, through run_test.ps1).
  run_parallel.ps1 -WorkingDirs <copy 1>\ff7\workingdir, <copy 2>\ff7\workingdir, ... -Scripts tests\a.txt, tests\b.txt, ...
Scripts are dealt to copies round-robin. Each script's full run_test output goes to tests\out\<script>.log, and its
screenshots to tests\out\<script>\. Prints one summary line per script (PASS marker / TIMEOUT count / exceptions).
Every copy must hold the same build; tests that need a save or roster find it in each copy (deploy.ps1 copies
tests\rosters\*.json into every copy it is pointed at).
#>
param([Parameter(Mandatory)][string[]]$WorkingDirs, [Parameter(Mandatory)][string[]]$Scripts, [int]$TimeoutSec = 900)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot
$out = "$root\tests\out"
New-Item -ItemType Directory -Force $out | Out-Null
$n = $WorkingDirs.Count
$jobs = for ($k = 0; $k -lt $n; $k++) {
    $mine = @(for ($i = $k; $i -lt $Scripts.Count; $i += $n) { $Scripts[$i] })
    if (-not $mine) { continue }
    Start-Job -ArgumentList $PSScriptRoot, $root, $WorkingDirs[$k], $mine, $out, $TimeoutSec -ScriptBlock {
        param($lab, $root, $wd, $scripts, $out, $timeout)
        foreach ($s in $scripts) {
            $name = [IO.Path]::GetFileNameWithoutExtension($s)
            $ffnx = "$wd\FFNx.log"   # FFNx rewrites it at every launch: count after each run
            $o = & "$lab\run_test.ps1" -Script $s -TimeoutSec $timeout -WorkingDir $wd 2>&1 | Out-String
            $e1 = if (Test-Path $ffnx) { (Select-String -Path $ffnx -Pattern "Unhandled Exception").Count } else { 0 }
            Set-Content "$out\$name.log" $o -Encoding utf8
            New-Item -ItemType Directory -Force "$out\$name" | Out-Null
            Copy-Item "$wd\trainer\shots\*" "$out\$name\" -Force -ErrorAction SilentlyContinue
            $pass = $o -match "PASS marker"
            $to = ([regex]::Matches($o, "TIMEOUT")).Count
            $exc = $e1
            "{0,-28} {1,-6} timeouts {2}  exceptions {3}  ({4})" -f $name, $(if ($pass) { "PASS" } else { "-" }), $to, $exc, $wd
        }
    }
    Start-Sleep -Seconds 3   # stagger the launches
}
$jobs | Wait-Job | Receive-Job
