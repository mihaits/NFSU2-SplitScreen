@echo off
rem Starts speed2.exe from the game folder: the nearest parent of this folder that holds it.
for %%P in ("%~dp0.." "%~dp0..\.." "%~dp0..\..\.." "%~dp0..\..\..\..") do (
    if exist "%%~P\speed2.exe" (
        cd /d "%%~P"
        start "" speed2.exe
        exit /b 0
    )
)
echo speed2.exe not found above %~dp0
exit /b 1
