@echo off
rem Builds build\screen_pacing.exe (64-bit: Desktop Duplication).
setlocal
set VS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0.."
if not exist build\pacing mkdir build\pacing
cl /nologo /O2 /EHsc /Fobuild\pacing\ /Febuild\screen_pacing.exe tools\screen_pacing.cpp user32.lib
