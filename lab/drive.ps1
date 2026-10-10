<#
Drive the lab FF7 window: scancode key presses (DirectInput reads scancodes, not window messages) + screenshots.

  drive.ps1 -Steps "ok; wait 2000; up 1500; shot title"      # against the running ff7_en
  drive.ps1 -Launch -Steps "wait 20000; shot boot"            # start the lab exe first

Steps (separated by ;):
  ok | cancel | menu | switch | start   tap the FF7 button (default ff7input.cfg bindings)
  up|down|left|right <ms>               hold a direction for <ms> (default 150)
  key <hex> [ms] [ext]                  raw scancode (0x58 = F12)
  ctrl <hex> [ext]                      Ctrl+key (0x1F = Ctrl+S skip movie, 0x48 ext = Ctrl+Up speed)
  wait <ms>                             sleep
  shot <name>                           PrintWindow screenshot to $ShotDir\<name>.png
Input only goes out while the game window is in the foreground; the script refocuses before every key.
#>
param(
    [string]$Steps = "",
    [switch]$Launch,
    [string]$Exe = "$env:FF7_LAB_WORKINGDIR\ff7_en.exe",
    [string]$ShotDir = "$PSScriptRoot\..\tests\out\shots"
)

Add-Type -AssemblyName System.Drawing
if (-not ("FF7Drive" -as [type]) -or -not [FF7Drive].GetMethod("Combo")) {
Add-Type @"
using System; using System.Runtime.InteropServices; using System.Threading;
public static class FF7Drive {
  [StructLayout(LayoutKind.Sequential)] struct KEYBDINPUT { public ushort vk, scan; public uint flags, time; public IntPtr extra; }
  [StructLayout(LayoutKind.Sequential)] struct INPUT { public uint type; public KEYBDINPUT ki; public long pad; }
  [DllImport("user32.dll")] static extern uint SendInput(uint n, INPUT[] i, int size);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] static extern void keybd_event(byte vk, byte scan, uint flags, IntPtr extra);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  public struct RECT { public int L, T, R, B; }
  const uint SCAN = 0x8, UP = 0x2, EXT = 0x1;
  static void Send(ushort scan, bool ext, bool up) {
    var i = new INPUT[1]; i[0].type = 1; i[0].ki.scan = scan;
    i[0].ki.flags = SCAN | (ext ? EXT : 0) | (up ? UP : 0);
    SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
  }
  public static bool Focus(IntPtr h) {
    if (GetForegroundWindow() == h) return true;
    keybd_event(0x12, 0, 0, IntPtr.Zero); keybd_event(0x12, 0, 2, IntPtr.Zero); // Alt tap unlocks SetForegroundWindow
    ShowWindow(h, 9); SetForegroundWindow(h); Thread.Sleep(150);
    return GetForegroundWindow() == h;
  }
  public static void Hold(ushort scan, bool ext, int ms) { Send(scan, ext, false); Thread.Sleep(ms); Send(scan, ext, true); }
  public static void Combo(ushort mod, ushort scan, bool ext) {
    Send(mod, false, false); Thread.Sleep(40); Send(scan, ext, false); Thread.Sleep(80);
    Send(scan, ext, true); Thread.Sleep(40); Send(mod, false, true);
  }
}
"@
}

# DirectInput scancodes from the default ff7input.cfg (0x9C = numpad Enter = E0 1C)
$keys = @{
    ok = @(0x1C, $true); cancel = @(0x52, $false); menu = @(0x4E, $false); switch = @(0x53, $false)
    start = @(0x4C, $false); up = @(0x48, $false); down = @(0x50, $false); left = @(0x4B, $false); right = @(0x4D, $false)
    pgup = @(0x49, $false); pgdn = @(0x51, $false); camera = @(0x47, $false); target = @(0x4F, $false)
}

if ($Launch) {
    $wd = Split-Path $Exe
    $p = Start-Process -FilePath $Exe -WorkingDirectory $wd -PassThru
    Write-Output "launched PID=$($p.Id)"
} else {
    $p = Get-Process ff7_en -ErrorAction Stop | Select-Object -First 1
}
# IntPtr.Zero is truthy in PowerShell: compare explicitly
for ($i = 0; $i -lt 80 -and $p.MainWindowHandle -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 500; $p.Refresh() }
$h = $p.MainWindowHandle
if ($h -eq [IntPtr]::Zero) { throw "ff7_en has no main window" }
New-Item -ItemType Directory -Force $ShotDir | Out-Null

function Shot([string]$name) {
    $r = New-Object FF7Drive+RECT; [FF7Drive]::GetWindowRect($h, [ref]$r) | Out-Null
    $bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
    $g = [System.Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc()
    [FF7Drive]::PrintWindow($h, $dc, 2) | Out-Null; $g.ReleaseHdc($dc); $g.Dispose()
    $path = Join-Path $ShotDir "$name.png"; $bmp.Save($path); $bmp.Dispose()
    Write-Output "shot $path"
}

foreach ($raw in ($Steps -split ";")) {
    $s = $raw.Trim(); if (-not $s) { continue }
    $verb, $arg = $s -split "\s+", 2
    switch ($verb) {
        "wait" { Start-Sleep -Milliseconds ([int]$arg) }
        "shot" { Shot $arg }
        "ctrl" {
            # ctrl <hex scancode> [ext]   e.g. ctrl 0x1F = Ctrl+S (FFNx skip movie), ctrl 0x48 ext = Ctrl+Up (speed up)
            $a = $arg -split "\s+"
            if (-not [FF7Drive]::Focus($h)) { Write-Output "WARN: could not focus the game window"; continue }
            [FF7Drive]::Combo(0x1D, [uint16][Convert]::ToInt32($a[0], 16), ($a.Count -gt 1))
            Start-Sleep -Milliseconds 150
        }
        "key" {
            # key <hex scancode> [ms] [ext]   e.g. key 0x58 = F12 (FFNx DevTools toggle)
            $a = $arg -split "\s+"
            if (-not [FF7Drive]::Focus($h)) { Write-Output "WARN: could not focus the game window"; continue }
            $ms = if ($a.Count -gt 1) { [int]$a[1] } else { 120 }
            [FF7Drive]::Hold([uint16][Convert]::ToInt32($a[0], 16), ($a.Count -gt 2), $ms)
            Start-Sleep -Milliseconds 120
        }
        default {
            if (-not $keys.ContainsKey($verb)) { throw "unknown step: $s" }
            if (-not [FF7Drive]::Focus($h)) { Write-Output "WARN: could not focus the game window"; continue }
            $ms = if ($arg) { [int]$arg } else { 120 }
            [FF7Drive]::Hold([uint16]$keys[$verb][0], [bool]$keys[$verb][1], $ms)
            Start-Sleep -Milliseconds 120
        }
    }
}
Write-Output "PID=$($p.Id)"
