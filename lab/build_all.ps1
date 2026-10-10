<# Build hook (MSVC x86) and core (Rust i686) and deploy to the lab. Stops on the first failure. #>
$ErrorActionPreference = "Continue"   # native tools write progress to stderr; we check exit codes
$root = Split-Path $PSScriptRoot
$out = cmd /c "$root\hook\build.cmd" 2>&1
if ($LASTEXITCODE -ne 0) { $out | Where-Object { $_ -match "error|warning" } ; throw "hook build failed" }
"hook: built"
# cargo from PATH, or $env:CARGO (full path to cargo.exe) when Rust is installed elsewhere
$cargo = if ($env:CARGO) { $env:CARGO } else { "cargo" }
Push-Location "$root\core"
& $cargo build -q --release --target i686-pc-windows-msvc 2>&1 | Out-String | Write-Output
$code = $LASTEXITCODE
Pop-Location
if ($code -ne 0) { throw "core build failed" }
"core: built"
& "$PSScriptRoot\deploy.ps1" | Out-Null
"deployed to lab"
