<#
Run one in-game test script without touching the user's desktop.

  run_test.ps1 -Script tests\smoke_newgame.txt [-TimeoutSec 600]

- The game is started on a private Windows desktop ("FF7Lab") with CreateProcess + STARTUPINFO.lpDesktop. Windows on
  another desktop cannot take focus or draw on the user's screen. There is deliberately no fallback launch path.
- Test mode (hook\src\testmode.cpp) injects the script's keys inside the game and ignores the physical keyboard.
- The foreground window is sampled the whole run; if the game ever becomes foreground the run is killed at once.
#>
param([Parameter(Mandatory)][string]$Script, [int]$TimeoutSec = 600,
      [string]$WorkingDir = $(if ($env:FF7_LAB_WORKINGDIR) { $env:FF7_LAB_WORKINGDIR } else { throw "pass -WorkingDir or set FF7_LAB_WORKINGDIR" }),
      [switch]$Visible)   # -Visible: user's desktop, window on screen (only when the user says they're off the PC)
$ErrorActionPreference = "Stop"
if (-not ("LabDesktop" -as [type])) {
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class LabDesktop {
  [DllImport("user32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
  static extern IntPtr CreateDesktopW(string name, IntPtr dev, IntPtr mode, uint flags, uint access, IntPtr sa);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
  struct STARTUPINFO { public int cb; public string lpReserved; public string lpDesktop; public string lpTitle;
    public int x, y, xs, ys, xc, yc, fill, flags; public short show, r2; public IntPtr r3, hin, hout, herr; }
  [StructLayout(LayoutKind.Sequential)] struct PI { public IntPtr hp, ht; public int pid, tid; }
  [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
  static extern bool CreateProcessW(string app, string cmd, IntPtr pa, IntPtr ta, bool inherit, uint flags,
    IntPtr env, string cwd, ref STARTUPINFO si, out PI pi);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
  static IntPtr desk;
  public static int Start(string exe, string cwd, string desktop) {
    if (desk == IntPtr.Zero) desk = CreateDesktopW(desktop, IntPtr.Zero, IntPtr.Zero, 0, 0x10000000u, IntPtr.Zero);
    if (desk == IntPtr.Zero) throw new Exception("CreateDesktop failed " + Marshal.GetLastWin32Error());
    var si = new STARTUPINFO(); si.cb = Marshal.SizeOf(si); si.lpDesktop = desktop;
    PI pi;
    if (!CreateProcessW(exe, "\"" + exe + "\"", IntPtr.Zero, IntPtr.Zero, false, 0, IntPtr.Zero, cwd, ref si, out pi))
      throw new Exception("CreateProcess failed " + Marshal.GetLastWin32Error());
    CloseHandle(pi.ht); CloseHandle(pi.hp);
    return pi.pid;
  }
  public static uint ForegroundPid() { uint p; GetWindowThreadProcessId(GetForegroundWindow(), out p); return p; }
}
"@
}
$root = Split-Path $PSScriptRoot
# One game per install (its log, script and saves are per working dir); other installs may run in parallel.
$exe = [IO.Path]::GetFullPath("$WorkingDir\ff7_en.exe")
if (Get-Process ff7_en -ErrorAction SilentlyContinue | Where-Object { $_.Path -ieq $exe }) { throw "ff7_en already running in $WorkingDir" }
Copy-Item (Join-Path $root $Script) "$WorkingDir\trainer\test_script.txt" -Force
Remove-Item "$WorkingDir\trainer\shots\*" -ErrorAction SilentlyContinue
$log = "$WorkingDir\trainer\trainer.log"
$mark = if (Test-Path $log) { (Get-Content $log).Count } else { 0 }
$fgBefore = [LabDesktop]::ForegroundPid()
$flag = "$WorkingDir\trainer\test_visible.flag"
try {
    if ($Visible) {
        Set-Content $flag "visible" -Encoding ascii
        $p = Start-Process -FilePath "$WorkingDir\ff7_en.exe" -WorkingDirectory $WorkingDir -PassThru
        $gamePid = $p.Id
        "started PID $gamePid VISIBLE on the user's desktop"
    } else {
        Remove-Item $flag -ErrorAction SilentlyContinue
        $gamePid = [LabDesktop]::Start("$WorkingDir\ff7_en.exe", $WorkingDir, "FF7Lab")
        $p = Get-Process -Id $gamePid
        "started PID $gamePid on private desktop FF7Lab (user foreground PID $fgBefore)"
    }
    $stolen = $false
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while (-not $p.HasExited -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 100
        if (-not $Visible -and [LabDesktop]::ForegroundPid() -eq [uint32]$gamePid) {
            $stolen = $true
            Stop-Process -Id $gamePid -Confirm:$false
            "ABORT: game became the foreground window; killed PID $gamePid"
            break
        }
    }
    if (-not $p.HasExited) { Stop-Process -Id $gamePid -Confirm:$false; "TIMEOUT after $TimeoutSec s: killed PID $gamePid" }
} finally {
    Remove-Item "$WorkingDir\trainer\test_script.txt" -ErrorAction SilentlyContinue   # never leave test mode armed
    Remove-Item $flag -ErrorAction SilentlyContinue
}
"foreground PID before: $fgBefore  after: $([LabDesktop]::ForegroundPid())  game ever foreground: $stolen"
"---- trainer.log (this run) ----"
Get-Content $log | Select-Object -Skip $mark
