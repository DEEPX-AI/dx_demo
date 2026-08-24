@echo off
setlocal enabledelayedexpansion

rem ---------------------------------------------------------------------------
rem MSVC build script for yolo_multi_demo.
rem
rem Required environment variables (relative paths are accepted):
rem   DEEPX_SDK_DIR   DEEPX SDK install dir (include/, lib/x64/dxrt.lib, bin/x64/*.dll)
rem   OpenCV_DIR      OpenCV CMake config dir (e.g. vcpkg_installed\x64-windows\share\opencv4)
rem
rem Usage: build.bat [--clean] [--verbose] [--type Release|Debug|RelWithDebInfo]
rem ---------------------------------------------------------------------------

set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"

set "BUILD_TYPE=Release"
set "CLEAN_BUILD=false"
set "VERBOSE=false"

:parse_args
if "%~1"=="" goto args_done
if /i "%~1"=="--help"    goto help
if /i "%~1"=="--clean"   ( set "CLEAN_BUILD=true" & shift & goto parse_args )
if /i "%~1"=="--verbose" ( set "VERBOSE=true" & shift & goto parse_args )
if /i "%~1"=="--type"    ( set "BUILD_TYPE=%~2" & shift & shift & goto parse_args )
echo Unknown argument: %~1
exit /b 1
:args_done

rem Defaults so you don't have to set the environment manually.
rem Override by exporting DEEPX_SDK_DIR / OpenCV_DIR before running this script.
if not defined DEEPX_SDK_DIR set "DEEPX_SDK_DIR=C:\Program Files\DEEPX\DX_SDK_20260630"
if not defined OpenCV_DIR   set "OpenCV_DIR=%SCRIPT_DIR%\vcpkg_installed\x64-windows\share\opencv4"

echo [INFO] DEEPX_SDK_DIR = %DEEPX_SDK_DIR%
echo [INFO] OpenCV_DIR    = %OpenCV_DIR%

if not exist "%DEEPX_SDK_DIR%" (
    echo [ERROR] DEEPX_SDK_DIR not found: %DEEPX_SDK_DIR%
    echo         Set it to your DEEPX SDK install dir, e.g. set "DEEPX_SDK_DIR=C:\Program Files\DEEPX\DX_SDK_XXXXXXXX"
    exit /b 1
)
rem If OpenCV isn't present yet, install project deps via vcpkg (manifest mode).
if not exist "%OpenCV_DIR%" call :install_opencv
if not exist "%OpenCV_DIR%" (
    echo [ERROR] OpenCV_DIR still not found: %OpenCV_DIR%
    echo         vcpkg install may have failed, or OpenCV_DIR points to the wrong place.
    exit /b 1
)

set "BUILD_DIR=%SCRIPT_DIR%\build_x64"
set "BIN_DIR=%SCRIPT_DIR%\bin"

if "%CLEAN_BUILD%"=="true" (
    echo [INFO] Cleaning build directories...
    if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
    if exist "%BIN_DIR%"   rmdir /s /q "%BIN_DIR%"
)

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

cmake -S "%SCRIPT_DIR%" -B "%BUILD_DIR%" ^
    -G "Visual Studio 17 2022" -A x64 ^
    -DCMAKE_INSTALL_PREFIX="%SCRIPT_DIR%" ^
    -DDEEPX_SDK_DIR="%DEEPX_SDK_DIR%" ^
    -DOpenCV_DIR="%OpenCV_DIR%" ^
    -DCMAKE_VERBOSE_MAKEFILE=%VERBOSE%
if errorlevel 1 (
    echo [ERROR] CMake configuration failed.
    exit /b 1
)

cmake --build "%BUILD_DIR%" --config %BUILD_TYPE% --target install
if errorlevel 1 (
    echo [ERROR] CMake build failed.
    exit /b 1
)

if exist "%BIN_DIR%" (
    echo [INFO] Build done - %BUILD_TYPE%. Binary in: %BIN_DIR%
    dir /b "%BIN_DIR%"
) else (
    echo [ERROR] Build failed - bin not created.
    exit /b 1
)

endlocal
exit /b 0

rem ---------------------------------------------------------------------------
rem Install project dependencies (OpenCV, etc.) via vcpkg in manifest mode.
rem Reads vcpkg.json in this directory and populates vcpkg_installed\.
rem ---------------------------------------------------------------------------
:install_opencv
echo [INFO] OpenCV not found. Installing dependencies via vcpkg ^(manifest mode^)...
echo        This can take a while the first time ^(OpenCV may build from source^).

set "VCPKG_EXE="
if defined VCPKG_ROOT if exist "%VCPKG_ROOT%\vcpkg.exe" set "VCPKG_EXE=%VCPKG_ROOT%\vcpkg.exe"
if not defined VCPKG_EXE if exist "%SCRIPT_DIR%\vcpkg\vcpkg.exe" set "VCPKG_EXE=%SCRIPT_DIR%\vcpkg\vcpkg.exe"
if not defined VCPKG_EXE (
    where vcpkg.exe >nul 2>nul
    if not errorlevel 1 set "VCPKG_EXE=vcpkg"
)
if not defined VCPKG_EXE (
    echo [ERROR] vcpkg not found. Install vcpkg and set VCPKG_ROOT,
    echo         e.g. set "VCPKG_ROOT=C:\vcpkg"   ^(folder containing vcpkg.exe^)
    goto :eof
)
if not exist "%SCRIPT_DIR%\vcpkg.json" (
    echo [ERROR] %SCRIPT_DIR%\vcpkg.json not found - cannot run manifest-mode install.
    goto :eof
)

echo [INFO] Using vcpkg: %VCPKG_EXE%
pushd "%SCRIPT_DIR%"
rem vcpkg rejects a manifest that uses "overrides" without a "builtin-baseline".
rem Stamp the current vcpkg checkout's HEAD into vcpkg.json if it isn't there yet.
findstr /c:"builtin-baseline" vcpkg.json >nul 2>nul
if errorlevel 1 (
    echo [INFO] vcpkg.json has no builtin-baseline - adding one from the vcpkg checkout.
    "%VCPKG_EXE%" x-update-baseline --add-initial-baseline
)
"%VCPKG_EXE%" install --triplet x64-windows
set "VCPKG_ERR=%errorlevel%"
popd
if not "%VCPKG_ERR%"=="0" echo [ERROR] vcpkg install failed with code %VCPKG_ERR%.
goto :eof

:help
echo Usage: build.bat [--clean] [--verbose] [--type Release^|Debug^|RelWithDebInfo]
echo   Env: DEEPX_SDK_DIR, OpenCV_DIR (auto-defaulted; OpenCV auto-installed via vcpkg if missing)
endlocal
exit /b 0
