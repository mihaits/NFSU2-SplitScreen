@echo off
rem Builds build\AAChainTest.asi (the hook chaining test plugin, see tools/chain_test.sh).
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
cd /d "%~dp0..\.."
cl /nologo /O2 /MT /W3 /LD /Fobuild\ /Febuild\AAChainTest.asi tools\chain_test\ChainTest.cpp || exit /b 1
