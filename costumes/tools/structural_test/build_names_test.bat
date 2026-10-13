@echo off
setlocal
cd /d "%~dp0"

rem Builds names_test.exe next to this file. Run: names_test.exe <game folder> [oracle.tsv]
set "VSDIR=C:\Program Files\Microsoft Visual Studio\2022\Community"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

if not exist obj mkdir obj
set "C=..\..\..\common"
cl /nologo /O2 /MT /w /c /Fo"obj\\" %C%\third_party\zstddeclib.c %C%\third_party\miniz.c
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /O2 /MT /EHa /W3 /D_CRT_SECURE_NO_WARNINGS /I"..\..\loader" /I"%C%" /I"%C%\third_party" /I"..\..\..\colormod\src" /Fo"obj\\" /Fe"names_test.exe" names_test.cpp "%C%\fighter_names.cpp" "%C%\pak.cpp" "..\..\loader\structural_msg.cpp" "..\..\..\colormod\src\fighters.cpp" obj\zstddeclib.obj obj\miniz.obj
if errorlevel 1 exit /b 1
echo built %~dp0names_test.exe
