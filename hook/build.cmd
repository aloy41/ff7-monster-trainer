@echo off
rem Build hook\build\DINPUT.dll (x86) with MSVC. Run from any shell; it sets up the x86 toolchain itself.
setlocal
rem Visual Studio (any edition with the C++ workload), found with vswhere; set VS yourself to override.
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
if not defined VS for /f "usebackq delims=" %%i in (`vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (echo Visual Studio with the C++ workload not found & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /std:c++20 /O2 /MT /W4 /WX /EHsc /permissive- /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
   src\dllmain.cpp src\trainer.cpp src\patch.cpp src\log.cpp src\game.cpp src\testmode.cpp src\mute.cpp src\text.cpp src\hud.cpp src\monsters.cpp ^
   /Fobuild\ /Fe:build\DINPUT.dll /LD ^
   /link /DEF:dinput.def /MACHINE:X86 /SAFESEH:NO /DYNAMICBASE:NO user32.lib kernel32.lib gdi32.lib ole32.lib || exit /b 1
echo built %~dp0build\DINPUT.dll
