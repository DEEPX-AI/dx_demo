#Requires -Version 5.1
<#
.SYNOPSIS
    Windows equivalent of setup.sh - Sets up sample models and videos for yolo_multi_demo.

.PARAMETER Force
    Force overwrite if the file already exists.

.PARAMETER ForceRemoveModels
    Force remove models if they exist.

.PARAMETER ForceRemoveVideos
    Force remove videos if they exist.

.PARAMETER Verbose
    Enable verbose (debug) logging.

.PARAMETER SymbolicLinkTargetPath
    Set symlink target path (default: ..\workspace).

.PARAMETER Help
    Show this help message.

.NOTES
    Symlink creation requires Administrator privileges or Windows Developer Mode enabled.
    tar.exe (Windows 10 build 17063+) is required for archive extraction.
#>
[CmdletBinding()]
param(
    [switch]$Force,
    [switch]$ForceRemoveModels,
    [switch]$ForceRemoveVideos,
    [switch]$Help,
    [string]$SymbolicLinkTargetPath = ""
)

Set-StrictMode -Off
$ErrorActionPreference = "Stop"

# This script lives in <demo>\scripts. SCRIPTS_DIR = its own dir; SCRIPT_DIR =
# the demo root (parent), so all the ..\assets / ..\workspace paths below stay
# relative to the demo root as before.
$SCRIPTS_DIR = $PSScriptRoot
$SCRIPT_DIR  = Split-Path $PSScriptRoot -Parent

# Resolve paths (may not exist yet)
function Resolve-PathSafe {
    param([string]$Path)
    if (Test-Path $Path) {
        return (Resolve-Path $Path).Path
    }
    return [System.IO.Path]::GetFullPath($Path)
}

$RUNTIME_PATH = Resolve-PathSafe (Join-Path $SCRIPT_DIR "..\..")
$DX_AS_PATH   = Resolve-PathSafe (Join-Path $RUNTIME_PATH "..")

# --- Initialize variables ---
$script:ENABLE_DEBUG_LOGS = if ($PSBoundParameters.ContainsKey('Verbose') -and $VerbosePreference -ne 'SilentlyContinue') { 1 } else { 0 }
$script:FORCE_FLAG         = $Force.IsPresent
$script:FORCE_REMOVE_MODELS = $ForceRemoveModels.IsPresent
$script:FORCE_REMOVE_VIDEOS = $ForceRemoveVideos.IsPresent
$script:SYMLINK_TARGET_PATH = if ($SymbolicLinkTargetPath) { $SymbolicLinkTargetPath } else { Resolve-PathSafe (Join-Path $SCRIPT_DIR "..\workspace") }

# --------------------------------------------------------------------------
# Color output helpers
# --------------------------------------------------------------------------
function Write-Colored {
    param(
        [string]$Message,
        [string]$Level = ""
    )

    if ($Level -eq "DEBUG" -and $script:ENABLE_DEBUG_LOGS -ne 1) {
        return
    }

    switch ($Level) {
        "ERROR"   { Write-Host "[ERROR] $Message"   -ForegroundColor Red }
        "SUCCESS" { Write-Host "[SUCCESS] $Message" -ForegroundColor Green }
        "OK"      { Write-Host "[OK] $Message"      -ForegroundColor Green }
        "FAIL"    { Write-Host "[FAIL] $Message"    -ForegroundColor Red }
        "INFO"    { Write-Host "[INFO] $Message"    -ForegroundColor Cyan }
        "WARNING" { Write-Host "[WARNING] $Message" -ForegroundColor Yellow }
        "DEBUG"   { Write-Host "[DEBUG] $Message"   -ForegroundColor Yellow }
        "GREEN"   { Write-Host $Message             -ForegroundColor Green }
        "YELLOW"  { Write-Host $Message             -ForegroundColor Yellow }
        default   { Write-Host $Message }
    }
}

# --------------------------------------------------------------------------
# Help
# --------------------------------------------------------------------------
function Show-Help {
    param(
        [string]$Type = "",
        [string]$ErrorMessage = ""
    )

    Write-Colored "Usage: $(Split-Path $PSCommandPath -Leaf) [OPTIONS]" "YELLOW"
    Write-Colored "Options:" "GREEN"
    Write-Colored "  [-Force]                                  Force overwrite if the file already exists" "GREEN"
    Write-Colored "  [-ForceRemoveModels]                      Force remove models if they exist" "GREEN"
    Write-Colored "  [-ForceRemoveVideos]                      Force remove videos if they exist" "GREEN"
    Write-Colored "  [-Verbose]                                Enable verbose (debug) logging" "GREEN"
    Write-Colored "  [-SymbolicLinkTargetPath <path>]          Set symlink target path (default: ..\workspace)" "GREEN"
    Write-Colored "  [-Help]                                   Show this help message" "GREEN"

    if ($Type -eq "error") {
        $msg = if ($ErrorMessage) { $ErrorMessage } else { "Invalid or missing arguments." }
        Write-Colored $msg "ERROR"
        exit 1
    }
    exit 0
}

if ($Help) {
    Show-Help
}

Write-Colored "======== PATH INFO =========" "DEBUG"
Write-Colored "RUNTIME_PATH($RUNTIME_PATH)" "DEBUG"
Write-Colored "DX_AS_PATH($DX_AS_PATH)" "DEBUG"

# --------------------------------------------------------------------------
# Setup assets
# --------------------------------------------------------------------------
function Invoke-SetupAssets {
    $MODEL_PATH = ".\assets\models"
    $VIDEO_PATH = ".\assets\videos"

    # ---- Models ----
    Write-Colored " MODEL_PATH: $MODEL_PATH" "INFO"
    $MODEL_REAL_PATH = Resolve-PathSafe $MODEL_PATH

    $forceModel = $script:FORCE_FLAG
    if ($script:FORCE_REMOVE_MODELS) { $forceModel = $true }

    if (-not (Test-Path $MODEL_REAL_PATH) -or $forceModel) {
        Write-Colored " models directory not found. Running setup models script... ($MODEL_REAL_PATH)" "INFO"

        # Windows: download straight into assets\models (real folder, no symlink).
        $modelParams = @{
            Output = $MODEL_PATH
        }
        if ($forceModel) { $modelParams['Force'] = $true }

        & "$SCRIPTS_DIR\setup_sample_models.ps1" @modelParams
        if ($LASTEXITCODE -ne 0) {
            Write-Colored "Setup models script failed." "ERROR"
            if (Test-Path $MODEL_PATH) { Remove-Item -Recurse -Force $MODEL_PATH }
            exit 1
        }
    } else {
        Write-Colored " models directory found. ($MODEL_REAL_PATH)" "INFO"
    }

    # ---- Videos ----
    Write-Colored "VIDEO_PATH: $VIDEO_PATH" "INFO"
    $VIDEO_REAL_PATH = Resolve-PathSafe $VIDEO_PATH

    $forceVideo = $script:FORCE_FLAG
    if ($script:FORCE_REMOVE_VIDEOS) { $forceVideo = $true }

    if (-not (Test-Path $VIDEO_REAL_PATH) -or $forceVideo) {
        Write-Colored " Video directory not found. Running setup videos script... ($VIDEO_REAL_PATH)" "INFO"

        # Windows: download straight into assets\videos (real folder, no symlink).
        $videoParams = @{
            Output = $VIDEO_PATH
        }
        if ($forceVideo) { $videoParams['Force'] = $true }

        & "$SCRIPTS_DIR\setup_sample_videos.ps1" @videoParams
        if ($LASTEXITCODE -ne 0) {
            Write-Colored "Setup videos script failed." "ERROR"
            if (Test-Path $VIDEO_PATH) { Remove-Item -Recurse -Force $VIDEO_PATH }
            exit 1
        }
    } else {
        Write-Colored " Video directory found. ($VIDEO_REAL_PATH)" "INFO"
    }

    Write-Colored "[OK] Sample models and videos setup complete" "INFO"
}

# --------------------------------------------------------------------------
# Main
# --------------------------------------------------------------------------
Push-Location $SCRIPT_DIR
try {
    Invoke-SetupAssets
} finally {
    Pop-Location
}
