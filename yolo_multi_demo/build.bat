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

if not defined DEEPX_SDK_DIR (
    echo [ERROR] DEEPX_SDK_DIR is not set. Point it at the DEEPX SDK install directory.
    exit /b 1
)
if not defined OpenCV_DIR (
    echo [ERROR] OpenCV_DIR is not set. Point it at the OpenCV CMake config directory.
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
    echo [INFO] Build done (%BUILD_TYPE%). Binary in: %BIN_DIR%\
    dir /b "%BIN_DIR%"
) else (
    echo [ERROR] Build failed - bin\ not created.
    exit /b 1
)

endlocal
