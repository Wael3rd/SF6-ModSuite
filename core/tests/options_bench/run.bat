@echo off
setlocal
:: run.bat -- build and run the option engine bench (no game). Output: build\core\options_bench\
cd /d "%~dp0"
set "VS_PATH=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
if not exist "%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat" for /f "usebackq delims=" %%i in (`"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_PATH=%%i"
where cl >nul 2>&1 || call "%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set OUT=..\..\..\build\core\options_bench
if not exist %OUT% mkdir %OUT%
cl /nologo /std:c++20 /EHsc /O2 /W4 /Fo%OUT%\ /Fe:%OUT%\options_bench.exe options_bench.cpp ..\..\src\options\options_core.cpp
if errorlevel 1 exit /b 1
%OUT%\options_bench.exe
