#Requires -Version 5.1
<#
.SYNOPSIS
    Windows equivalent of setup_sample_models.sh
    Lightweight model downloader (dx_app-style URL manifest, no Python).
    Reads scripts\model_manifest.json and downloads each "dxnn_url" straight
    into assets\models under its original filename (URL basename). No symlink.
#>
[CmdletBinding()]
param(
    [string]$Output            = "",
    [string]$SymlinkTargetPath = "",
    [string]$Manifest          = "",
    [switch]$Force,
    [switch]$Help
)

Set-StrictMode -Off
$ErrorActionPreference = "Stop"

$SCRIPT_DIR = $PSScriptRoot
if (-not $Output)   { $Output   = Join-Path $SCRIPT_DIR "..\assets\models" }
if (-not $Manifest) { $Manifest = Join-Path $SCRIPT_DIR "model_manifest.json" }

function Show-Help {
    param([string]$Type = "")
    Write-Host "Usage: $(Split-Path $PSCommandPath -Leaf) [OPTIONS]"
    Write-Host "Options:"
    Write-Host "  [-Output <path>]                 Output directory (default: ..\assets\models)"
    Write-Host "  [-SymlinkTargetPath <path>]      Ignored on Windows (kept for call-compatibility)"
    Write-Host "  [-Manifest <file>]               Model manifest json (default: scripts\model_manifest.json)"
    Write-Host "  [-Force]                         Re-download even if the file already exists"
    Write-Host "  [-Help]                          Show this help message"
    if ($Type -eq "error") { Write-Host "Error: Invalid or missing arguments." -ForegroundColor Red; exit 1 }
    exit 0
}
if ($Help) { Show-Help }

if (-not (Test-Path $Manifest)) {
    Write-Host "[DXDEMO] [ERROR] manifest not found: $Manifest" -ForegroundColor Red
    exit 1
}

# --- parse manifest (proper JSON parsing on Windows) ---
try {
    $entries = Get-Content -Raw -Encoding UTF8 $Manifest | ConvertFrom-Json
} catch {
    Write-Host "[DXDEMO] [ERROR] failed to parse manifest: $Manifest" -ForegroundColor Red
    exit 1
}

# --- resolve download dir ---
# Windows: download straight into the output dir (assets\models) as a real
# folder. No symlink (SymlinkTargetPath is accepted for call-compatibility but
# ignored on Windows, where symlinks need admin/developer mode).
$DlDir = $Output
New-Item -ItemType Directory -Force -Path $DlDir | Out-Null

# --- download ---
foreach ($e in $entries) {
    $url = $e.dxnn_url
    if (-not $url) {
        Write-Host "[DXDEMO] [ERROR] manifest entry needs dxnn_url." -ForegroundColor Red
        exit 1
    }
    $fn  = $url.Split('/')[-1]   # local filename = original name from the URL
    $dst = Join-Path $DlDir $fn
    if ((Test-Path $dst) -and (-not $Force)) {
        Write-Host "[DXDEMO] [INFO]  already exists, skip: $fn (use -Force to re-download)"
        continue
    }
    Write-Host "[DXDEMO] [INFO]  downloading $fn"
    Write-Host "[DXDEMO] [INFO]    <- $url"
    try {
        Invoke-WebRequest -Uri $url -OutFile "$dst.part" -UseBasicParsing
        Move-Item -Force "$dst.part" $dst
    } catch {
        Write-Host "[DXDEMO] [ERROR] download failed: $url" -ForegroundColor Red
        if (Test-Path "$dst.part") { Remove-Item -Force "$dst.part" }
        exit 1
    }
}

Write-Host "[DXDEMO] [INFO]  model setup complete -> $Output"
exit 0
