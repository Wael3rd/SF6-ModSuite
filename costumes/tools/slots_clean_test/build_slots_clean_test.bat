@echo off
setlocal
cd /d "%~dp0"

rem Builds slots_clean_test.exe next to this file. Run: slots_clean_test.exe <SF6_SlotsClean.exe> <work dir>
rem (build.bat builds build\SF6_SlotsClean.exe; the work dir is any scratch folder, the test fills it with fake game folders)
set "VSDIR=C:\Program Files\Microsoft Visual Studio\2022\Community"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

if not exist obj mkdir obj
set C=..\..\..\common
cl /nologo /std:c++17 /O2 /MT /EHa /W3 /D_CRT_SECURE_NO_WARNINGS /I"%C%" /I"%C%\third_party" /Fo"obj\\" /Fe"slots_clean_test.exe" slots_clean_test.cpp "%C%\pak.cpp" "%C%\third_party\zstddeclib.c" "%C%\third_party\miniz.c"
if errorlevel 1 exit /b 1
echo built %~dp0slots_clean_test.exe
