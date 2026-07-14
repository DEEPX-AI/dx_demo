#Requires -Version 5.1
<#
.SYNOPSIS
    Windows equivalent of setup_sample_models.sh
    Downloads and sets up sample model files for yolo_multi_demo.
#>
[CmdletBinding()]
param(
    [string]$SrcPath         = "res/models/models-2_2_0.tar.gz",
    [string]$Output          = "",
    [string]$SymlinkTargetPath = "",
    [switch]$Force,
    [switch]$Help
)

Set-StrictMode -Off
$ErrorActionPreference = "Stop"

$SCRIPT_DIR = $PSScriptRoot

if (-not $Output) {
    $Output = Join-Path $SCRIPT_DIR "..\assets\models"
}

function Show-Help {
    param([string]$Type = "")
    Write-Host "Usage: $(Split-Path $PSCommandPath -Leaf) [OPTIONS]"
    Write-Host "Options:"
    Write-Host "  [-Force]                         Force overwrite if the file already exists"
    Write-Host "  [-Output <path>]                 Output directory path"
    Write-Host "  [-SymlinkTargetPath <path>]      Symlink target path"
    Write-Host "  [-Help]                          Show this help message"
    if ($Type -eq "error") {
        Write-Host "Error: Invalid or missing arguments." -ForegroundColor Red
        exit 1
    }
    exit 0
}

if ($Help) { Show-Help }

$getResourceScript = Join-Path $SCRIPT_DIR "get_resource.ps1"

$params = @{
    SrcPath  = $SrcPath
    Output   = $Output
    Extract  = $true
}
if ($SymlinkTargetPath) { $params['SymlinkTargetPath'] = $SymlinkTargetPath }
if ($Force)             { $params['Force']             = $true }

Write-Host "Get Resources from remote server ..."
Write-Host "& '$getResourceScript' -SrcPath '$($params.SrcPath)' -Output '$($params.Output)'$(if ($Force) {' -Force'})$(if ($SymlinkTargetPath) {" -SymlinkTargetPath '$SymlinkTargetPath'"}) -Extract"

& $getResourceScript @params
if ($LASTEXITCODE -ne 0) {
    Write-Host "[ERROR] Get resource failed!" -ForegroundColor Red
    Write-Host "[HINT]  If the issue persists, try running PowerShell as Administrator with the -Force flag:" -ForegroundColor Yellow
    Write-Host "        .\scripts\setup_sample_models.ps1 -Force" -ForegroundColor Yellow
    exit 1
}

exit 0
