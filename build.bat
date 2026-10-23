@echo off
rem Builds build\NFSU2SplitScreen.asi (32-bit). Needs VS 2022 Build Tools with the C++ workload.
setlocal
set VS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools
call "%VS%\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /O2 /MT /W3 /EHsc /LD /D_CRT_SECURE_NO_WARNINGS /Fobuild\ /Febuild\NFSU2SplitScreen.asi src\NFSU2SplitScreen.cpp src\XInputDevices.cpp src\DualSense.cpp user32.lib shlwapi.lib hid.lib setupapi.lib cfgmgr32.lib /link /SAFESEH:NO /MAP:build/NFSU2SplitScreen.map || exit /b 1
echo built build\NFSU2SplitScreen.asi
