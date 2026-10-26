@echo off
rem Builds build\NFSU2SplitScreen.asi (32-bit). Needs VS 2022 Build Tools with the C++ workload.
setlocal
set VS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools
call "%VS%\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
rem libopus (third_party/opus, 1.5.2, float, no SIMD) as build\opus.lib, built once
if not exist build\opus.lib (
    if not exist build\opus mkdir build\opus
    cl /nologo /c /O2 /MT /W0 /DOPUS_BUILD /DUSE_ALLOCA /DPACKAGE_VERSION=\"1.5.2\" /Ithird_party\opus\include /Ithird_party\opus\celt /Ithird_party\opus\silk /Ithird_party\opus\silk\float /Ithird_party\opus /Fobuild\opus\ third_party\opus\celt\*.c third_party\opus\silk\*.c third_party\opus\silk\float\*.c third_party\opus\src\*.c || exit /b 1
    lib /nologo /out:build\opus.lib build\opus\*.obj || exit /b 1
)
cl /nologo /O2 /MT /W3 /EHsc /LD /D_CRT_SECURE_NO_WARNINGS /Ithird_party\opus\include /Fobuild\ /Febuild\NFSU2SplitScreen.asi src\NFSU2SplitScreen.cpp src\XInputDevices.cpp src\DualSense.cpp user32.lib shlwapi.lib hid.lib setupapi.lib cfgmgr32.lib build\opus.lib winmm.lib /link /SAFESEH:NO /MAP:build/NFSU2SplitScreen.map || exit /b 1
echo built build\NFSU2SplitScreen.asi
