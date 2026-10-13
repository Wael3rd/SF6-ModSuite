@echo off
setlocal
cd /d "%~dp0"

rem Builds everything into build\
rem   core\sf6core_native.dll           the shared core (core\build.bat)
rem   colormod\SF6_ColorModNative.dll   the Color Mod (colormod\build.bat)
rem   amd_ags_x64.dll                  costume slots + stage slots: the DLL the game loads
rem   costumes_only\amd_ags_x64.dll    the costume loader alone (what the releases ship so far)
rem   SF6_CostumeSlotsNative.dll       REFramework plugin used during online matches
rem   costume_loader.exe paktool.exe loadtest.exe    costume tools
rem   SF6_SlotsClean.exe              removes the loader's runtime paks and state (shipped next to amd_ags_x64.dll)
rem   stagepak.exe stagetest.exe textest.exe         stage tools
rem "build.bat deps" rebuilds the archive decoders (build\archive_deps.lib) first.

rem Visual Studio or Build Tools with the C++ workload, wherever it is installed
set "VSDIR=C:\Program Files\Microsoft Visual Studio\2022\Community"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

set C=common
set CO=costumes\loader
set ST=stages\loader
set B=build
set O=build\obj
set CF=/nologo /std:c++17 /O2 /MT /EHa /W3 /D_CRT_SECURE_NO_WARNINGS /I"%C%" /I"%C%\third_party" /I"%CO%" /I"%ST%" /I"proxy"
set LK=/link /INCREMENTAL:NO
rem Libraries the archive decoders may import. All of them are already loaded by the game's own
rem executable, so importing them statically adds nothing at runtime.
set SYSLIBS=bcrypt.lib advapi32.lib shell32.lib ole32.lib user32.lib
if not exist %B%\costumes_only mkdir %B%\costumes_only

rem ---------------------------------------------------------------------------------------------
rem Archive decoders (7z: LZMA SDK, rar: UnRAR), built once into build\archive_deps.lib.
rem UnRAR is built single-threaded and without Crypt32: see common\third_party\unrar\SF6_PATCHES.txt.
rem ---------------------------------------------------------------------------------------------
if /i "%1"=="deps" if exist %B%\archive_deps.lib del %B%\archive_deps.lib
if exist %B%\archive_deps.lib goto :objects

echo === archive_deps.lib (LZMA SDK + UnRAR) ===
if exist %O%\deps rmdir /s /q %O%\deps
mkdir %O%\deps
set LZ=%C%\third_party\lzma
cl /nologo /O2 /MT /W1 /c /Fo%O%\deps\ %LZ%\7zAlloc.c %LZ%\7zArcIn.c %LZ%\7zBuf.c %LZ%\7zBuf2.c %LZ%\7zCrc.c %LZ%\7zCrcOpt.c %LZ%\7zDec.c %LZ%\7zFile.c %LZ%\7zStream.c %LZ%\Bcj2.c %LZ%\Bra.c %LZ%\Bra86.c %LZ%\BraIA64.c %LZ%\CpuArch.c %LZ%\Delta.c %LZ%\Lzma2Dec.c %LZ%\LzmaDec.c %LZ%\Ppmd7.c %LZ%\Ppmd7Dec.c
if errorlevel 1 goto :failed
set UR=%C%\third_party\unrar
set URDEF=/DRARDLL /DUNRAR /DSILENT /DSF6_LOADER_NO_SMP /DSF6_LOADER_NO_CRYPT32
cl /nologo /std:c++17 /O2 /MT /EHa /W1 %URDEF% /c /Fo%O%\deps\ %UR%\archive.cpp %UR%\arcread.cpp %UR%\blake2s.cpp %UR%\cmddata.cpp %UR%\consio.cpp %UR%\crc.cpp %UR%\crypt.cpp %UR%\dll.cpp %UR%\encname.cpp %UR%\errhnd.cpp %UR%\extinfo.cpp %UR%\extract.cpp %UR%\filcreat.cpp %UR%\file.cpp %UR%\filefn.cpp %UR%\filestr.cpp %UR%\find.cpp %UR%\getbits.cpp %UR%\global.cpp %UR%\hash.cpp %UR%\headers.cpp %UR%\isnt.cpp %UR%\largepage.cpp %UR%\match.cpp %UR%\motw.cpp %UR%\options.cpp %UR%\pathfn.cpp %UR%\qopen.cpp %UR%\rar.cpp %UR%\rarvm.cpp %UR%\rawread.cpp %UR%\rdwrfn.cpp %UR%\rijndael.cpp %UR%\rs.cpp %UR%\rs16.cpp %UR%\scantree.cpp %UR%\secpassword.cpp %UR%\sha1.cpp %UR%\sha256.cpp %UR%\smallfn.cpp %UR%\strfn.cpp %UR%\strlist.cpp %UR%\system.cpp %UR%\threadpool.cpp %UR%\timefn.cpp %UR%\ui.cpp %UR%\unicode.cpp %UR%\unpack.cpp %UR%\volume.cpp
if errorlevel 1 goto :failed
lib /nologo /OUT:%B%\archive_deps.lib %O%\deps\*.obj
if errorlevel 1 goto :failed

:objects
rem ---------------------------------------------------------------------------------------------
rem Shared objects, compiled once and linked into each target
rem ---------------------------------------------------------------------------------------------
for %%d in (core stage proxy proxy_costumes tools) do if not exist %O%\%%d mkdir %O%\%%d
echo === common + costume loader ===
cl %CF% /c /Fo%O%\core\ %C%\third_party\zstddeclib.c %C%\third_party\miniz.c %C%\pak.cpp %C%\archive.cpp %C%\fighter_names.cpp %CO%\patch.cpp %CO%\structural_scene.cpp %CO%\structural_msg.cpp %CO%\vanilla_inventory.cpp %CO%\loader_core.cpp
if errorlevel 1 goto :failed
echo === stage slots ===
cl %CF% /c /Fo%O%\stage\ %ST%\stage_log.cpp %ST%\stage_loader.cpp %ST%\preview_tex.cpp %ST%\stage_redirect.cpp
if errorlevel 1 goto :failed
set PAK=%O%\core\zstddeclib.obj %O%\core\miniz.obj %O%\core\pak.obj
set CORE=%PAK% %O%\core\archive.obj %O%\core\fighter_names.obj %O%\core\patch.obj %O%\core\structural_scene.obj %O%\core\structural_msg.obj %O%\core\vanilla_inventory.obj %O%\core\loader_core.obj

echo === amd_ags_x64.dll (costume slots + stage slots) ===
cl %CF% /c /Fo%O%\proxy\ proxy\slots_proxy.cpp
if errorlevel 1 goto :failed
link /nologo /DLL /INCREMENTAL:NO /MACHINE:X64 /OUT:%B%\amd_ags_x64.dll %O%\proxy\slots_proxy.obj %CORE% %O%\stage\stage_log.obj %O%\stage\stage_loader.obj %O%\stage\preview_tex.obj %O%\stage\stage_redirect.obj %B%\archive_deps.lib %SYSLIBS%
if errorlevel 1 goto :failed

echo === costumes_only\amd_ags_x64.dll ===
cl %CF% /DSLOTS_NO_STAGES /c /Fo%O%\proxy_costumes\ proxy\slots_proxy.cpp
if errorlevel 1 goto :failed
link /nologo /DLL /INCREMENTAL:NO /MACHINE:X64 /OUT:%B%\costumes_only\amd_ags_x64.dll %O%\proxy_costumes\slots_proxy.obj %CORE% %B%\archive_deps.lib %SYSLIBS%
if errorlevel 1 goto :failed

echo === costume tools ===
cl %CF% /Fo%O%\tools\ %CO%\costume_loader_main.cpp %CORE% /Fe%B%\costume_loader.exe %LK% %B%\archive_deps.lib %SYSLIBS%
if errorlevel 1 goto :failed
cl %CF% /Fo%O%\tools\ %C%\paktool.cpp %PAK% %O%\core\patch.obj /Fe%B%\paktool.exe %LK%
if errorlevel 1 goto :failed
cl %CF% /Fo%O%\tools\ %CO%\loadtest.cpp /Fe%B%\loadtest.exe %LK%
if errorlevel 1 goto :failed
cl %CF% /Fo%O%\tools\ costumes\tools\slots_clean.cpp %PAK% /Fe%B%\SF6_SlotsClean.exe %LK%
if errorlevel 1 goto :failed

echo === stage tools ===
cl %CF% /Fo%O%\tools\ stages\tools\stagepak.cpp %PAK% /Fe%B%\stagepak.exe %LK%
if errorlevel 1 goto :failed
cl %CF% /Fo%O%\tools\ stages\tools\textest.cpp %O%\stage\preview_tex.obj /Fe%B%\textest.exe %LK%
if errorlevel 1 goto :failed
cl %CF% /Fo%O%\tools\ stages\tools\stagetest.cpp %PAK% %O%\core\archive.obj %O%\core\patch.obj %O%\stage\stage_log.obj %O%\stage\stage_loader.obj %O%\stage\preview_tex.obj /Fe%B%\stagetest.exe %LK% %B%\archive_deps.lib %SYSLIBS%
if errorlevel 1 goto :failed

echo === SF6_CostumeSlotsNative.dll (REFramework plugin, API 1.5.8: costume and stage slots in game) ===
if not exist %O%\plugin mkdir %O%\plugin
rem SLOTS_LEGACY_UI_FALLBACK: compiles the direct implementations so the plugin
rem works without sf6core_native.dll.  Remove when the core becomes mandatory.
cl /nologo /std:c++20 /EHa /O2 /W4 /LD /DSLOTS_LEGACY_UI_FALLBACK /I plugin\include /I core\include /Fo%O%\plugin\ plugin\src\common.cpp plugin\src\costumes.cpp plugin\src\stages.cpp plugin\src\main.cpp plugin\src\slots_ui.cpp plugin\src\slots_ui_legacy.cpp plugin\src\perf.cpp plugin\src\wjournal.cpp /Fe%B%\SF6_CostumeSlotsNative.dll /link /opt:ref
if errorlevel 1 goto :failed

echo.
echo === core (sf6core_native.dll) ===
call core\build.bat
if errorlevel 1 goto :failed
echo === Color Mod (SF6_ColorModNative.dll) ===
call colormod\build.bat
if errorlevel 1 goto :failed

echo.
echo All OK: build\amd_ags_x64.dll, build\costumes_only\amd_ags_x64.dll, build\SF6_CostumeSlotsNative.dll,
echo         build\core\sf6core_native.dll, build\colormod\SF6_ColorModNative.dll and the tools
echo Replace the DLLs in the game folder only while the game is closed.
exit /b 0

:failed
echo FAILED
exit /b 1
