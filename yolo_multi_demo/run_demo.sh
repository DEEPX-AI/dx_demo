#!/bin/bash
SCRIPT_DIR=$(realpath "$(dirname "$0")")
DX_DEMO_PATH=$(realpath -s "${SCRIPT_DIR}")

# color env settings
source "${DX_DEMO_PATH}/scripts/color_env.sh"
source "${DX_DEMO_PATH}/scripts/common_util.sh"

pushd $DX_DEMO_PATH

print_colored "DX_DEMO_PATH: $DX_DEMO_PATH" "INFO"

# Check if bin directory exists and contains files
if [ ! -d "./bin" ] || [ -z "$(ls -A ./bin 2>/dev/null)" ]; then
    print_colored "yolo_multi_demo is not built. Building first before running the demo." "INFO"
    ./build.sh
fi

check_valid_dir_or_symlink() {
    local path="$1"
    if [ -d "$path" ] || { [ -L "$path" ] && [ -d "$(readlink -f "$path")" ]; }; then
        return 0
    else
        return 1
    fi
}

if check_valid_dir_or_symlink "./assets/models" && check_valid_dir_or_symlink "./assets/videos"; then
    print_colored "Models and Videos directory already exists. Skipping download." "INFO"
else
    print_colored "Models and Videos not found. Downloading now via setup.sh..." "INFO"
    ./setup.sh --force
fi

EXAMPLE="${DX_DEMO_PATH}/config"

BIN="${DX_DEMO_PATH}/bin/yolo_multi_demo"

# 이 스크립트에 준 인자는 그대로 데모에 전달된다.
#   예) ./run_demo.sh --profile            -> profile_linux_<N>ch_<시각>.log 생성
#       ./run_demo.sh --profile my.log --profile_period 2000
EXTRA_ARGS=("$@")

print_colored "Press ESC or Q to stop the demo." "INFO"

echo "0: Multi-Channel Object Detection (YOLOv5)"
echo "1: Multi-Channel Object Detection With PPU (YOLOv5-512)"
echo "2: Multi-Channel 100ch Object Detection With PPU (YOLOv5-512)"

prompt="Which demo do you want to run? (default:0): "
printf "%s" "$prompt"

for ((i=20; i>0; i--)); do
    read -t 0.1 -n 1 input 2>/dev/null
    if [ $? -eq 0 ]; then
        read -r rest_input
        select="$input$rest_input"
        break
    fi
    printf "\r%s(%ds) \033[K" "$prompt" "$i"
    sleep 0.9
done

if [ -z "$select" ]; then
    printf "\r%s(timeout) \033[K\n" "$prompt"
    select=0
    echo "Using default: 0"
fi

case $select in
    0) "$BIN" -c "${EXAMPLE}/yolo_multi_demo.json" "${EXTRA_ARGS[@]}";;
    1) "$BIN" -c "${EXAMPLE}/ppu_yolo_multi_demo.json" "${EXTRA_ARGS[@]}";;
    2) "$BIN" -c "${EXAMPLE}/ppu_yolo_multi_100channel_demo.json" "${EXTRA_ARGS[@]}";;
    *) print_colored "Invalid selection: $select" "ERROR"; exit 1;;
esac

popd > /dev/null
