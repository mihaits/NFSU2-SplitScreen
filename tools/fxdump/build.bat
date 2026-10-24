@echo off
rem Builds fxdump.exe (32-bit: it loads SysWOW64\d3dx9_33.dll). Needs VS 2022 Build Tools with the C++ workload.
setlocal
set VS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools
call "%VS%\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /O2 /W3 /D_CRT_SECURE_NO_WARNINGS /Fobuild\ /Febuild\fxdump.exe fxdump.c || exit /b 1
