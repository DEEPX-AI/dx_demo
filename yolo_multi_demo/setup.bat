@echo off
rem ---------------------------------------------------------------------------
rem setup.bat - double-clickable / cmd launcher for setup.ps1
rem
rem Downloads sample models (via scripts\model_manifest.json URLs) and videos.
rem Just double-click, or run from cmd:  setup.bat  [-Force] [-ForceRemoveModels] ...
rem Any arguments are forwarded to setup.ps1 as-is (PowerShell-style, e.g. -Force).
rem ---------------------------------------------------------------------------
setlocal

set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"

powershell -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%\scripts\setup.ps1" %*
set "RC=%ERRORLEVEL%"

if %RC% neq 0 (
    echo.
    echo [ERROR] setup failed with code %RC%.
) else (
    echo.
    echo [OK] setup done.
)

rem Keep the window open when launched by double-click from Explorer.
echo %CMDCMDLINE% | find /i "cmd /c" >nul && pause

endlocal & exit /b %RC%
