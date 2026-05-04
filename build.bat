@echo off
REM ===================================================================
REM  build.bat -- Mewjector chainloader (version.dll)
REM
REM  Compiles the public sources in this directory into version.dll
REM  (the chainloader / Mewjector API host) using Microsoft's C
REM  compiler (cl.exe).  The .NET host bridge, the managed SDKs, and
REM  the dev-only probe DLL are not part of the public github tree --
REM  they live in the private workspace.  This build.bat only handles
REM  what the public tree actually contains.
REM
REM  Requirements:
REM    Visual Studio 2019 or 2022 ^(Community is fine^) with the
REM    "Desktop development with C++" workload installed.
REM    (Build Tools for Visual Studio also works -- it's the same
REM    cl.exe + Windows SDK.)
REM    The script auto-locates the install via vswhere.exe; you do
REM    NOT need to launch a "Developer Command Prompt" first.
REM
REM  Usage:
REM    build.bat            -- build version.dll into dist\.
REM    build.bat clean      -- delete dist\ and intermediate files.
REM
REM  Output:
REM    dist\version.dll
REM
REM  Deployment:
REM    Drop dist\version.dll next to Mewgenics.exe in the game
REM    install directory, alongside chainloader.ini.
REM ===================================================================

setlocal enableextensions enabledelayedexpansion

REM ---- Resolve where this script lives so it works from any cwd ----
set "WS=%~dp0"
set "WS=%WS:~0,-1%"
set "DIST=%WS%\dist"

if /I "%~1"=="clean" goto :clean

if not "%~1"=="" if /I not "%~1"=="version" if /I not "%~1"=="all" (
    echo Unknown target: %~1
    echo.
    echo Usage: build.bat [version^|clean]
    echo.
    pause
    exit /b 1
)

REM ---- Locate Visual Studio via vswhere ---------------------------
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "!VSWHERE!" (
    echo ERROR: vswhere.exe not found at:
    echo   !VSWHERE!
    echo Install Visual Studio 2019 or 2022 ^(Community is fine^) with
    echo the "Desktop development with C++" workload, then re-run.
    pause
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo ERROR: vswhere did not report a Visual Studio installation.
    echo Make sure the C++ workload is installed.
    pause
    exit /b 1
)

set "VCVARS=!VSDIR!\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "!VCVARS!" (
    echo ERROR: vcvarsall.bat not found at:
    echo   !VCVARS!
    echo The C++ build tools may not be installed in this VS instance.
    pause
    exit /b 1
)

echo Setting up x64 MSVC environment...
call "!VCVARS!" x64 >nul 2>&1
if errorlevel 1 (
    echo ERROR: vcvarsall.bat returned !ERRORLEVEL!.
    pause
    exit /b 1
)

REM ---- Prepare dist\ ----------------------------------------------
if not exist "!DIST!" mkdir "!DIST!"

REM ---- Common cl flags --------------------------------------------
set "CFLAGS=/nologo /LD /O2 /GS- /W3 /D_CRT_SECURE_NO_WARNINGS"

REM ---- Build version.dll ------------------------------------------
echo.
echo ====================================================
echo  Building version.dll ^(chainloader^)
echo ====================================================
pushd "!WS!"
cl !CFLAGS! version.c /Fe:"!DIST!\version.dll" ^
    /Fo"!DIST!\version.obj" ^
    /link /nologo /DEF:version.def ^
          /IMPLIB:"!DIST!\version.lib"
set "RC=!ERRORLEVEL!"
popd
if not "!RC!"=="0" (
    echo.
    echo version.dll build FAILED ^(cl returned !RC!^).
    pause
    exit /b !RC!
)

REM ---- Stage chainloader.ini --------------------------------------
echo.
echo Staging chainloader.ini...
copy /Y "!WS!\chainloader.ini" "!DIST!\chainloader.ini" >nul
if errorlevel 1 (
    echo WARNING: could not copy chainloader.ini -- you can copy it
    echo manually from the source tree.
)

REM ---- Clean up cl intermediates ----------------------------------
del /Q "!DIST!\*.obj" "!DIST!\*.exp" "!DIST!\*.lib" 2>nul

REM ---- Summary ----------------------------------------------------
echo.
echo ====================================================
echo  Build succeeded.
echo ====================================================
echo.
echo Output directory: !DIST!
echo.
dir /B "!DIST!"
echo.
echo Drop the contents of !DIST!\ next to Mewgenics.exe in the game
echo install directory to deploy.
echo.
pause
endlocal
exit /b 0

:clean
echo Cleaning...
if exist "!DIST!" rmdir /S /Q "!DIST!"
echo Clean complete.
endlocal
exit /b 0
