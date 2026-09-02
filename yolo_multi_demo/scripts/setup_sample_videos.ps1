#Requires -Version 5.1
<#
.SYNOPSIS
    Windows equivalent of setup_sample_videos.sh
    Lightweight sample-video downloader (no Python, no get_resource).
    Downloads the sample video archive and extracts it straight into
    assets\videos (real folder, no symlink). Requires tar.exe (Windows 10 1803+).
#>
[CmdletBinding()]
param(
    [string]$Output            = "",
    [string]$Url               = "https://sdk.deepx.ai/res/video/sample_videos.tar.gz",
    [string]$SymlinkTargetPath = "",   # accepted for call-compatibility; ignored on Windows
    [switch]$Force,
    [switch]$Help
)

Set-StrictMode -Off
$ErrorActionPreference = "Stop"

$SCRIPT_DIR = $PSScriptRoot
if (-not $Output) { $Output = Join-Path $SCRIPT_DIR "..\assets\videos" }

function Show-Help {
    param([string]$Type = "")
    Write-Host "Usage: $(Split-Path $PSCommandPath -Leaf) [OPTIONS]"
    Write-Host "Options:"
    Write-Host "  [-Output <path>]                 Output directory (default: ..\assets\videos)"
    Write-Host "  [-Url <url>]                     Archive URL (default: sample_videos.tar.gz)"
    Write-Host "  [-SymlinkTargetPath <path>]      Ignored on Windows (kept for call-compatibility)"
    Write-Host "  [-Force]                         Re-download/extract even if videos already exist"
    Write-Host "  [-Help]                          Show this help message"
    if ($Type -eq "error") { Write-Host "Error: Invalid or missing arguments." -ForegroundColor Red; exit 1 }
    exit 0
}
if ($Help) { Show-Help }

# tar.exe is required to extract the .tar.gz
if (-not (Get-Command tar.exe -ErrorAction SilentlyContinue)) {
    Write-Host "[DXDEMO] [ERROR] tar.exe not found (needs Windows 10 1803+ / build 17063+)." -ForegroundColor Red
    exit 1
}

# Skip if videos already present (unless -Force)
if ((Test-Path $Output) -and (-not $Force)) {
    $existing = @(Get-ChildItem -Path $Output -File -ErrorAction SilentlyContinue |
                  Where-Object { $_.Extension -in '.mov', '.mp4' })
    if ($existing.Count -gt 0) {
        Write-Host "[DXDEMO] [INFO]  videos already exist, skip: $Output (use -Force to re-download)"
        exit 0
    }
}

New-Item -ItemType Directory -Force -Path $Output | Out-Null

$fn      = $Url.Split('/')[-1]
$tmpArch = Join-Path ([System.IO.Path]::GetTempPath()) $fn

Write-Host "[DXDEMO] [INFO]  downloading $fn (this is large, ~1GB) ..."
Write-Host "[DXDEMO] [INFO]    <- $Url"
try {
    if (Get-Command curl.exe -ErrorAction SilentlyContinue) {
        & curl.exe -fL --retry 3 --connect-timeout 15 -o "$tmpArch" "$Url"
        if ($LASTEXITCODE -ne 0) { throw "curl exited with $LASTEXITCODE" }
    } else {
        Invoke-WebRequest -Uri $Url -OutFile "$tmpArch" -UseBasicParsing
    }
} catch {
    Write-Host "[DXDEMO] [ERROR] download failed: $Url" -ForegroundColor Red
    if (Test-Path "$tmpArch") { Remove-Item -Force "$tmpArch" }
    exit 1
}

Write-Host "[DXDEMO] [INFO]  extracting into $Output ..."
& tar.exe -xzf "$tmpArch" -C "$Output"
$rc = $LASTEXITCODE
Remove-Item -Force "$tmpArch" -ErrorAction SilentlyContinue
if ($rc -ne 0) {
    Write-Host "[DXDEMO] [ERROR] extraction failed (tar rc=$rc)." -ForegroundColor Red
    exit 1
}

Write-Host "[DXDEMO] [INFO]  video setup complete -> $Output"
exit 0
