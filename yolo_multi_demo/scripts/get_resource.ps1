#Requires -Version 5.1
<#
.SYNOPSIS
    Windows equivalent of get_resource.sh
    Downloads a resource from https://sdk.deepx.ai/, optionally extracts it,
    and optionally creates a symbolic link at the output path.

.PARAMETER SrcPath
    Source path relative to base URL (e.g. res/models/models-2_2_0.tar.gz).

.PARAMETER Output
    Output directory path.

.PARAMETER SymlinkTargetPath
    If set, the actual files are stored here; Output becomes a symbolic link.

.PARAMETER Force
    Force overwrite even if the destination already exists.

.PARAMETER Extract
    Extract the downloaded archive after downloading.

.PARAMETER Help
    Show this help message.

.NOTES
    Requirements:
      - tar.exe  : built-in on Windows 10 build 17063+ (used for .tar.gz extraction)
      - curl.exe : built-in on Windows 10 1803+ (used for download)
    Symbolic link creation requires Administrator privileges OR Windows Developer Mode.
#>
[CmdletBinding()]
param(
    [string]$SrcPath           = "",
    [string]$Output            = "",
    [string]$SymlinkTargetPath = "",
    [switch]$Force,
    [switch]$Extract,
    [switch]$Help
)

Set-StrictMode -Off
$ErrorActionPreference = "Stop"

$BASE_URL    = "https://sdk.deepx.ai/"
$SCRIPT_DIR  = $PSScriptRoot
$DOWNLOAD_DIR = [System.IO.Path]::GetFullPath((Join-Path $SCRIPT_DIR "..\download"))

# --------------------------------------------------------------------------
# Helpers
# --------------------------------------------------------------------------
function Resolve-PathSafe {
    param([string]$Path)
    if (Test-Path $Path) {
        return (Resolve-Path $Path).Path
    }
    return [System.IO.Path]::GetFullPath($Path)
}

function Write-Info    { param([string]$m) Write-Host "[INFO] $m"    -ForegroundColor Cyan }
function Write-Ok      { param([string]$m) Write-Host "[OK] $m"      -ForegroundColor Green }
function Write-Err     { param([string]$m) Write-Host "[ERROR] $m"   -ForegroundColor Red }
function Write-Hint    { param([string]$m) Write-Host "[HINT] $m"    -ForegroundColor Yellow }
function Write-Msg     { param([string]$m) Write-Host $m }

function Exit-WithMessage {
    param([string]$Message)
    Write-Err $Message
    exit 1
}

# --------------------------------------------------------------------------
# Help
# --------------------------------------------------------------------------
function Show-Help {
    param([string]$Type = "")
    Write-Info "Usage: $(Split-Path $PSCommandPath -Leaf) -SrcPath <path> -Output <dir> [-SymlinkTargetPath <dir>] [-Extract] [-Force]"
    Write-Info "Example: .\get_resource.ps1 -SrcPath res/models/models-2_2_0.tar.gz -Output ..\assets\models -SymlinkTargetPath ..\workspace\models -Extract"
    Write-Info "Options:"
    Write-Info "  -SrcPath <path>               Source path on the file server"
    Write-Info "  -Output <path>                Output directory"
    Write-Info "  [-Extract]                    Extract the downloaded archive"
    Write-Info "  [-SymlinkTargetPath <path>]   Store real files here; Output becomes a symlink"
    Write-Info "  [-Force]                      Force overwrite existing files"
    Write-Info "  [-Help]                       Show this help message"
    if ($Type -eq "error") {
        Exit-WithMessage "Invalid or missing arguments."
    }
    exit 0
}

if ($Help) { Show-Help }

# --------------------------------------------------------------------------
# Validate inputs
# --------------------------------------------------------------------------
if (-not $SrcPath -or -not $Output) {
    Exit-WithMessage "SrcPath('$SrcPath') or Output('$Output') is not set."
}

# Resolve output to absolute path
$OUTPUT_DIR = Resolve-PathSafe $Output

# Guard: output must not be the current directory
$CURRENT_DIR = Resolve-PathSafe "."
if ($OUTPUT_DIR -eq $CURRENT_DIR) {
    Exit-WithMessage "'-Output' is the same as the current directory. Please specify a different directory."
}

$SYMLINK_TARGET_PATH = if ($SymlinkTargetPath) { Resolve-PathSafe $SymlinkTargetPath } else { "" }
$USE_FORCE   = $Force.IsPresent
$USE_EXTRACT = $Extract.IsPresent

Write-Msg "USE_EXTRACT($USE_EXTRACT)"
Write-Msg "USE_FORCE($USE_FORCE)"

# --------------------------------------------------------------------------
# Compute paths
# --------------------------------------------------------------------------
$FILENAME = [System.IO.Path]::GetFileName($SrcPath)

# Determine target directory name (strip .tar.gz / .tgz / last extension)
if ($FILENAME -match '\.tar\.gz$') {
    $TARGET_DIR_NAME = $FILENAME -replace '\.tar\.gz$', ''
} elseif ($FILENAME -match '\.tgz$') {
    $TARGET_DIR_NAME = $FILENAME -replace '\.tgz$', ''
} else {
    $TARGET_DIR_NAME = [System.IO.Path]::GetFileNameWithoutExtension($FILENAME)
}

if ($SYMLINK_TARGET_PATH) {
    $ARCHIVE_TARGET_DIR  = Join-Path $SYMLINK_TARGET_PATH "download"
    $ARCHIVE_TARGET_PATH = Join-Path $ARCHIVE_TARGET_DIR $FILENAME
    $OUTPUT_TARGET_PATH  = Join-Path $SYMLINK_TARGET_PATH $TARGET_DIR_NAME
} else {
    $ARCHIVE_TARGET_DIR  = Join-Path $OUTPUT_DIR "download"
    $ARCHIVE_TARGET_PATH = Join-Path $ARCHIVE_TARGET_DIR $FILENAME
    $OUTPUT_TARGET_PATH  = Join-Path $OUTPUT_DIR $TARGET_DIR_NAME
}

$DOWNLOAD_PATH = Join-Path $DOWNLOAD_DIR $FILENAME
$URL = "${BASE_URL}${SrcPath}"

# --------------------------------------------------------------------------
# Check for required tools
# --------------------------------------------------------------------------
function Assert-Tool {
    param([string]$Name, [string]$InstallHint)
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        Exit-WithMessage "$Name is not available. $InstallHint"
    }
}

Assert-Tool "curl.exe" "curl.exe is built into Windows 10 1803+. Please update Windows or install curl manually."
if ($USE_EXTRACT) {
    Assert-Tool "tar.exe" "tar.exe is built into Windows 10 build 17063+. Please update Windows or install it manually."
}

# --------------------------------------------------------------------------
# Check symlink privileges
# --------------------------------------------------------------------------
function Test-SymlinkPrivilege {
    # Returns $true if we can create symlinks (admin or developer mode)
    $isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
    if ($isAdmin) { return $true }

    try {
        $devMode = Get-ItemProperty -Path "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock" `
                                    -Name "AllowDevelopmentWithoutDevLicense" -ErrorAction SilentlyContinue
        if ($devMode -and $devMode.AllowDevelopmentWithoutDevLicense -eq 1) { return $true }
    } catch { }
    return $false
}

# --------------------------------------------------------------------------
# Download
# --------------------------------------------------------------------------
function Invoke-Download {
    Write-Info "=== Download Start ==="
    Write-Msg "--- Download path: $DOWNLOAD_PATH ---"
    Write-Msg "--- ARCHIVE_TARGET_PATH($ARCHIVE_TARGET_PATH) ---"

    # If archive already in target and not forced: skip, ensure download symlink exists
    if ((Test-Path $ARCHIVE_TARGET_PATH) -and -not $USE_FORCE) {
        Write-Msg "Archive already exists at '$ARCHIVE_TARGET_PATH'. Skipping download."
        if (-not (Test-Path $DOWNLOAD_PATH)) {
            New-Item -ItemType Directory -Path $DOWNLOAD_DIR -Force | Out-Null
            if (Test-SymlinkPrivilege) {
                New-Item -ItemType SymbolicLink -Path $DOWNLOAD_PATH -Target (Resolve-PathSafe $ARCHIVE_TARGET_PATH) | Out-Null
            } else {
                # Fall back to copy if no symlink privilege
                Copy-Item $ARCHIVE_TARGET_PATH $DOWNLOAD_PATH
            }
        }
        Write-Info "=== Download SKIP ==="
        return
    }

    # Remove broken symlink at archive target
    if (Test-Path $ARCHIVE_TARGET_PATH -PathType Any) {
        $item = Get-Item $ARCHIVE_TARGET_PATH -ErrorAction SilentlyContinue
        if ($item -and ($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint)) {
            Write-Msg "Broken/stale symlink at '$ARCHIVE_TARGET_PATH'. Removing."
            Remove-Item $ARCHIVE_TARGET_PATH -Force
        }
    }

    # Remove existing download if forced
    if ($USE_FORCE -and (Test-Path $DOWNLOAD_PATH)) {
        Write-Msg "'--Force' option is set. Removing '$DOWNLOAD_PATH'."
        Remove-Item $DOWNLOAD_PATH -Force -Recurse
    }

    # Remove broken symlink at download path
    if (Test-Path $DOWNLOAD_PATH -PathType Any) {
        $item = Get-Item $DOWNLOAD_PATH -ErrorAction SilentlyContinue
        if ($item -and ($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint)) {
            Write-Msg "Broken symlink at '$DOWNLOAD_PATH'. Removing."
            Remove-Item $DOWNLOAD_PATH -Force
        }
    }

    New-Item -ItemType Directory -Path $DOWNLOAD_DIR -Force | Out-Null

    Write-Msg "Downloading $FILENAME from $URL ..."
    try {
        curl.exe --fail --location --output $DOWNLOAD_PATH $URL
    } catch {
        if (Test-Path $DOWNLOAD_PATH) { Remove-Item $DOWNLOAD_PATH -Force }
        Exit-WithMessage "Download failed! Check URL or network connection. URL: $URL"
    }

    if ($LASTEXITCODE -ne 0) {
        if (Test-Path $DOWNLOAD_PATH) { Remove-Item $DOWNLOAD_PATH -Force }
        Exit-WithMessage "Download failed (curl exit code $LASTEXITCODE)! Check URL or network connection. URL: $URL"
    }

    Write-Ok "Download Complete"
}

# --------------------------------------------------------------------------
# Extract tar.gz
# --------------------------------------------------------------------------
function Expand-TarArchive {
    param(
        [string]$TarFile,
        [string]$TargetDir
    )

    # Detect if archive has a top-level directory
    $firstEntry = & tar.exe tf $TarFile | Select-Object -First 1
    if ($firstEntry -match '/') {
        Write-Msg "Detected top-level directory: using --strip-components=1"
        tar.exe xvfz $TarFile --strip-components=1 -C $TargetDir
    } else {
        Write-Msg "No top-level directory: extracting as-is"
        tar.exe xvfz $TarFile -C $TargetDir
    }

    if ($LASTEXITCODE -ne 0) {
        return $false
    }
    return $true
}

# --------------------------------------------------------------------------
# Generate output (move downloaded file, extract if needed)
# --------------------------------------------------------------------------
function Invoke-GenerateOutput {
    Write-Info "=== Generate output Start ==="
    $ACTION_TYPE = if ($USE_EXTRACT) { "Move and Extract" } else { "Move" }

    if ((Test-Path $ARCHIVE_TARGET_PATH) -and -not $USE_FORCE) {
        Write-Msg "Archive already at '$ARCHIVE_TARGET_PATH'. Skipping move."
        Write-Info "=== MOVE SKIP ==="
    } else {
        Write-Msg "Moving '$DOWNLOAD_PATH' to '$ARCHIVE_TARGET_PATH' ..."
        New-Item -ItemType Directory -Path $ARCHIVE_TARGET_DIR -Force | Out-Null

        try {
            Move-Item $DOWNLOAD_PATH $ARCHIVE_TARGET_PATH -Force
        } catch {
            if (Test-Path $DOWNLOAD_PATH)      { Remove-Item $DOWNLOAD_PATH      -Force }
            if (Test-Path $ARCHIVE_TARGET_PATH) { Remove-Item $ARCHIVE_TARGET_PATH -Force }
            Exit-WithMessage "${ACTION_TYPE} failed! Could not move file to '$ARCHIVE_TARGET_PATH'. Check permissions."
        }

        # Create a reverse symlink from download path to archive target
        if (Test-SymlinkPrivilege) {
            try {
                New-Item -ItemType SymbolicLink -Path $DOWNLOAD_PATH -Target (Resolve-PathSafe $ARCHIVE_TARGET_PATH) | Out-Null
                Write-Ok "=== MAKE SYMLINK SUCC ==="
            } catch {
                Write-Hint "Could not create symlink '$DOWNLOAD_PATH' -> '$ARCHIVE_TARGET_PATH'. Skipping (non-fatal)."
            }
        } else {
            Write-Hint "No symlink privileges. Skipping reverse symlink creation (non-fatal)."
        }
    }

    # Extract if requested
    if (-not $USE_EXTRACT) {
        Write-Msg "Skipping extraction (--Extract not specified)."
        return
    }

    if ((Test-Path $OUTPUT_TARGET_PATH) -and -not $USE_FORCE) {
        Write-Msg "Output path '$OUTPUT_TARGET_PATH' already exists. Skipping extraction."
        Write-Info "=== EXTRACT SKIP ==="
        return
    }

    Write-Msg "Extracting '$ARCHIVE_TARGET_PATH' to '$OUTPUT_TARGET_PATH' ..."

    if (Test-Path $OUTPUT_TARGET_PATH) { Remove-Item $OUTPUT_TARGET_PATH -Recurse -Force }
    New-Item -ItemType Directory -Path $OUTPUT_TARGET_PATH -Force | Out-Null

    $ok = Expand-TarArchive -TarFile $ARCHIVE_TARGET_PATH -TargetDir $OUTPUT_TARGET_PATH
    if (-not $ok) {
        if (Test-Path $DOWNLOAD_PATH)       { Remove-Item $DOWNLOAD_PATH       -Force }
        if (Test-Path $ARCHIVE_TARGET_PATH) { Remove-Item $ARCHIVE_TARGET_PATH -Force }
        Exit-WithMessage "${ACTION_TYPE} failed! Extraction error. Check file integrity or permissions."
    }

    Write-Ok "=== EXTRACT SUCC ==="
    Write-Msg "${ACTION_TYPE} complete."
    Write-Ok "=== Generate output Complete ==="
}

# --------------------------------------------------------------------------
# Make symbolic link  (Output -> OUTPUT_TARGET_PATH)
# --------------------------------------------------------------------------
function Invoke-MakeSymlink {
    Write-Info "=== Make Symbolic Link Start ==="

    if (-not $SYMLINK_TARGET_PATH) {
        Write-Msg "'-SymlinkTargetPath' not set. Skipping symlink creation."
        Write-Ok "=== Make Symbolic Link Complete ==="
        return
    }

    if (-not (Test-SymlinkPrivilege)) {
        Write-Hint "No symlink privileges (run as Administrator or enable Developer Mode)."
        Write-Hint "Falling back to copying files instead of creating a symlink."

        # Fallback: copy instead of symlink
        if (-not $USE_EXTRACT) {
            $OUTPUT_PATH = Join-Path $OUTPUT_DIR $FILENAME
            if (-not (Test-Path $OUTPUT_PATH) -or $USE_FORCE) {
                New-Item -ItemType Directory -Path $OUTPUT_DIR -Force | Out-Null
                Copy-Item $ARCHIVE_TARGET_PATH $OUTPUT_PATH -Force
                Write-Msg "Copied '$ARCHIVE_TARGET_PATH' -> '$OUTPUT_PATH'"
            } else {
                Write-Msg "Output file '$OUTPUT_PATH' already exists. Skipping copy."
            }
        } else {
            if (-not (Test-Path $OUTPUT_DIR) -or $USE_FORCE) {
                if (Test-Path $OUTPUT_DIR) { Remove-Item $OUTPUT_DIR -Recurse -Force }
                $OUTPUT_TARGET_REAL = Resolve-PathSafe $OUTPUT_TARGET_PATH
                New-Item -ItemType Directory -Path (Split-Path $OUTPUT_DIR) -Force | Out-Null
                Copy-Item $OUTPUT_TARGET_REAL $OUTPUT_DIR -Recurse -Force
                Write-Msg "Copied '$OUTPUT_TARGET_REAL' -> '$OUTPUT_DIR'"
            } else {
                Write-Msg "Output directory '$OUTPUT_DIR' already exists. Skipping copy."
            }
        }
        Write-Ok "=== Make Symbolic Link Complete (via copy) ==="
        return
    }

    if (-not $USE_EXTRACT) {
        # Copy archive file to output dir (not symlink for files in this branch)
        $OUTPUT_PATH = Join-Path $OUTPUT_DIR $FILENAME
        if (-not (Test-Path $OUTPUT_PATH) -or $USE_FORCE) {
            New-Item -ItemType Directory -Path $OUTPUT_DIR -Force | Out-Null
            Copy-Item $ARCHIVE_TARGET_PATH $OUTPUT_PATH -Force
            Write-Msg "Copied '$ARCHIVE_TARGET_PATH' -> '$OUTPUT_PATH'"
        } else {
            Write-Msg "Output file '$OUTPUT_PATH' already exists. Skipping."
        }
    } else {
        # Create symlink: OUTPUT_DIR -> OUTPUT_TARGET_PATH
        if ((Test-Path $OUTPUT_DIR) -and -not $USE_FORCE) {
            Write-Msg "Symlink/directory '$OUTPUT_DIR' already exists. Skipping."
        } else {
            if (Test-Path $OUTPUT_DIR) { Remove-Item $OUTPUT_DIR -Recurse -Force }

            New-Item -ItemType Directory -Path (Split-Path $OUTPUT_DIR) -Force | Out-Null
            $OUTPUT_TARGET_REAL = Resolve-PathSafe $OUTPUT_TARGET_PATH

            try {
                New-Item -ItemType SymbolicLink -Path $OUTPUT_DIR -Target $OUTPUT_TARGET_REAL | Out-Null
                Write-Msg "Created symbolic link: '$OUTPUT_DIR' -> '$OUTPUT_TARGET_REAL'"
            } catch {
                Exit-WithMessage "Failed to create symlink '$OUTPUT_DIR' -> '$OUTPUT_TARGET_REAL'. $_"
            }
        }
    }

    Write-Ok "=== Make Symbolic Link Complete ==="
}

# --------------------------------------------------------------------------
# Main
# --------------------------------------------------------------------------
Invoke-Download
Invoke-GenerateOutput
Invoke-MakeSymlink

exit 0
