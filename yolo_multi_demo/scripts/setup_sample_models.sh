#!/bin/bash
# Lightweight model downloader (dx_app-style URL manifest, no Python).
# Reads scripts/model_manifest.json and downloads each "dxnn_url" into the
# output dir under its "filename" (the name the demo configs expect).
SCRIPT_DIR=$(realpath "$(dirname "$0")")

# color env settings (optional)
source ${SCRIPT_DIR}/color_env.sh 2>/dev/null || true
source ${SCRIPT_DIR}/common_util.sh 2>/dev/null || true

MANIFEST="${SCRIPT_DIR}/model_manifest.json"
OUTPUT_DIR="${SCRIPT_DIR}/../assets/models"
SYMLINK_TARGET_PATH=""
FORCE=0

log()  { echo "[DXDEMO] [INFO]  $*"; }
err()  { echo "[DXDEMO] [ERROR] $*" >&2; }

show_help() {
    echo "Usage: $(basename "$0") [OPTIONS]"
    echo "Options:"
    echo "  [--output=<dir>]                 Output directory (default: ../assets/models)"
    echo "  [--symlink_target_path=<dir>]    Download here, then symlink output -> here"
    echo "  [--manifest=<file>]              Model manifest json (default: scripts/model_manifest.json)"
    echo "  [--force]                        Re-download even if the file already exists"
    echo "  [--help]                         Show this help message"
    [ "$1" == "error" ] && exit 1
    exit 0
}

# --- parse args ---
for _ in "$@"; do
    case "$1" in
        --output=*)               OUTPUT_DIR="${1#*=}" ;;
        --symlink_target_path=*)   SYMLINK_TARGET_PATH="${1#*=}" ;;
        --manifest=*)              MANIFEST="${1#*=}" ;;
        --force)                   FORCE=1 ;;
        --help)                    show_help ;;
        "")                        ;;
        *)                         echo "Unknown option: $1"; show_help "error" ;;
    esac
    shift
done

[ -f "$MANIFEST" ] || { err "manifest not found: $MANIFEST"; exit 1; }

# --- pick a downloader ---
if command -v curl >/dev/null 2>&1; then
    fetch() { curl -fL --retry 3 --connect-timeout 15 -o "$1" "$2"; }
elif command -v wget >/dev/null 2>&1; then
    fetch() { wget -q -O "$1" "$2"; }
else
    err "neither curl nor wget is available."; exit 1
fi

# --- parse manifest (dxnn_url list; local filename = URL basename) ---
mapfile -t URLS < <(grep -oE '"dxnn_url"[[:space:]]*:[[:space:]]*"[^"]*"' "$MANIFEST" | sed -E 's/.*"([^"]*)"[[:space:]]*$/\1/')

if [ "${#URLS[@]}" -eq 0 ]; then
    err "manifest parse failed: no dxnn_url found in $MANIFEST"
    exit 1
fi

# --- resolve download dir ---
# When a symlink target is given, download into a dedicated <target>/models
# subdir (mirrors videos' <target>/sample_videos), so assets/models -> that
# subdir contains ONLY the model files (not the shared workspace root).
if [ -n "$SYMLINK_TARGET_PATH" ]; then
    DL_DIR="$SYMLINK_TARGET_PATH/models"
else
    DL_DIR="$OUTPUT_DIR"
fi
mkdir -p "$DL_DIR" || { err "cannot create $DL_DIR"; exit 1; }

# --- download ---
for i in "${!URLS[@]}"; do
    url="${URLS[$i]}"
    fn="${url##*/}"          # local filename = original name from the URL
    dst="${DL_DIR}/${fn}"
    if [ -f "$dst" ] && [ "$FORCE" -ne 1 ]; then
        log "already exists, skip: $fn (use --force to re-download)"
        continue
    fi
    log "downloading $fn"
    log "  <- $url"
    if ! fetch "${dst}.part" "$url"; then
        err "download failed: $url"
        rm -f "${dst}.part"
        exit 1
    fi
    mv -f "${dst}.part" "$dst"
done

# --- symlink output -> download dir (share models across demos, keep clean) ---
ABS_DL="$(readlink -f "$DL_DIR")"
ABS_OUTPUT="$(readlink -f "$OUTPUT_DIR" 2>/dev/null || echo "$OUTPUT_DIR")"
if [ "$ABS_DL" != "$ABS_OUTPUT" ]; then
    { [ -L "$OUTPUT_DIR" ] || [ -d "$OUTPUT_DIR" ]; } && rm -rf "$OUTPUT_DIR"
    mkdir -p "$(dirname "$OUTPUT_DIR")"
    ln -s "$ABS_DL" "$OUTPUT_DIR"
    log "linked: $OUTPUT_DIR -> $ABS_DL"
fi

log "model setup complete -> $(readlink -f "$OUTPUT_DIR")"
exit 0
