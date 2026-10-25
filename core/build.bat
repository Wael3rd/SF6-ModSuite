@echo off
setlocal enabledelayedexpansion

:: build.bat -- Build sf6core_native.dll and sf6core_test.dll
:: Usage: core\build.bat   (works from any directory)
:: Output: build\core\ (sf6core_native.dll and the two test clients)
:: Never copies anything to the game folder.

:: Always run from the core's folder (this script lives in <repo>\core\)
cd /d "%~dp0"

:: ---- Locate MSVC ----
set "VS_PATH=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
if not exist "%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat" (
    :: Try vswhere as fallback
    set "VSWHERE=C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
    if exist "!VSWHERE!" (
        for /f "usebackq delims=" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_PATH=%%i"
    )
)
if not exist "%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat" (
    echo ERROR: vcvars64.bat not found. Install MSVC Build Tools 2022.
    exit /b 1
)

:: ---- Set up MSVC environment ----
where cl >nul 2>&1 || call "%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

:: ---- Create output directory ----
if not exist ..\build\core mkdir ..\build\core

set CFLAGS=/nologo /MP /std:c++20 /EHa /O2 /LD /W4 /I include /I third_party\reframework\include /Fo..\build\core\
set ERRORS=0

:: ---- Step 1: Build Lua 5.4.3 static library ----
echo.
echo === Building Lua 5.4.3 (lua.lib) ===
rem Third-party and never edited: kept once built (delete build\core\lua.lib to rebuild it)
if exist ..\build\core\lua.lib echo OK: ..\build\core\lua.lib (kept)& goto :lua_done
set LUA_SRC=third_party\lua
set LUA_OBJS=
for %%f in (%LUA_SRC%\*.c) do (
    set "NAME=%%~nf"
    if /i not "!NAME!"=="lua" if /i not "!NAME!"=="luac" if /i not "!NAME!"=="onelua" (
        cl /nologo /c /O2 /W3 /I"%LUA_SRC%" /Fo"..\build\core\lua_%%~nf.obj" "%%f" >nul 2>&1
        if errorlevel 1 (
            echo ERROR: Failed to compile %%~nf.c
            set ERRORS=1
        )
        set "LUA_OBJS=!LUA_OBJS! ..\build\core\lua_%%~nf.obj"
    )
)
lib /nologo /out:..\build\core\lua.lib !LUA_OBJS!
if errorlevel 1 (
    echo ERROR: Failed to create lua.lib
    set ERRORS=1
) else (
    echo OK: ..\build\core\lua.lib
)

:lua_done
:: ---- Build sf6core_native.dll (core + display + lua_bridge + lua.lib) ----
echo.
echo === Building sf6core_native.dll ===
cl %CFLAGS% /DSF6CORE_EXPORTS /I third_party\lua /Fe:..\build\core\sf6core_native.dll ^
    src\sf6core.cpp src\display.cpp src\lua_bridge.cpp src\discovery.cpp src\perf.cpp ^
    src\options\options_service.cpp src\options\options.cpp src\options\options_core.cpp ^
    src\options\guid_text.cpp src\options\guid_table.cpp src\selection.cpp ^
    src\pausemenu\pm_service.cpp src\pausemenu\pausemenu.cpp src\pausemenu\pm_core.cpp ^
    /link /DLL ..\build\core\lua.lib rpcrt4.lib winmm.lib
if errorlevel 1 (
    echo ERROR: sf6core_native.dll build failed
    set ERRORS=1
) else (
    echo OK: ..\build\core\sf6core_native.dll
)

:: ---- Build sf6core_test.dll ----
echo.
echo === Building sf6core_test.dll ===
cl %CFLAGS% /Fe:..\build\core\sf6core_test.dll tests\test_client\sf6core_test.cpp /link /DLL
if errorlevel 1 (
    echo ERROR: sf6core_test.dll build failed
    set ERRORS=1
) else (
    echo OK: ..\build\core\sf6core_test.dll
)

:: ---- Build SF6core_preinit_test.dll (uppercase stem!) ----
echo.
echo === Building SF6core_preinit_test.dll ===
cl %CFLAGS% /Fe:..\build\core\SF6core_preinit_test.dll tests\preinit_client\sf6core_preinit_test.cpp /link /DLL
if errorlevel 1 (
    echo ERROR: SF6core_preinit_test.dll build failed
    set ERRORS=1
) else (
    echo OK: ..\build\core\SF6core_preinit_test.dll
)

:: ---- Clean up obj/lib/exp in repo root ----
if exist sf6core.obj del sf6core.obj
if exist sf6core_native.lib del sf6core_native.lib
if exist sf6core_native.exp del sf6core_native.exp
if exist sf6core_test.obj del sf6core_test.obj
if exist sf6core_test.lib del sf6core_test.lib
if exist sf6core_test.exp del sf6core_test.exp
if exist sf6core_preinit_test.obj del sf6core_preinit_test.obj
if exist SF6core_preinit_test.lib del SF6core_preinit_test.lib
if exist SF6core_preinit_test.exp del SF6core_preinit_test.exp
if exist display.obj del display.obj
if exist lua_bridge.obj del lua_bridge.obj
if exist discovery.obj del discovery.obj

:: ---- Summary ----
echo.
if %ERRORS%==0 (
    echo BUILD SUCCEEDED: 0 errors
) else (
    echo BUILD FAILED
)
exit /b %ERRORS%
