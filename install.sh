#!/bin/sh
# Install script for /MNT Tensor Konsole Visualiser
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
INSTALL_DIR="${HOME}/.local/bin"

echo "Checking build dependencies..."
if ! command -v g++ >/dev/null 2>&1; then
    echo "Error: g++ is required but was not found. Install a C++20-capable compiler (e.g. 'sudo pacman -S gcc' or 'sudo apt install g++') and re-run this script." >&2
    exit 1
fi
if ! command -v pactl >/dev/null 2>&1 || ! command -v parec >/dev/null 2>&1; then
    echo "Error: 'pactl' and 'parec' are required. Install pulseaudio-utils or the PipeWire-Pulse compatibility layer and re-run this script." >&2
    exit 1
fi

echo "Building mnt_tensor_visualizer..."
g++ -O3 -pthread -std=c++20 -o "${SCRIPT_DIR}/mnt_tensor_visualizer" "${SCRIPT_DIR}/mnt_tensor_visualizer.cpp"

mkdir -p "${INSTALL_DIR}"
cp "${SCRIPT_DIR}/mnt_tensor_visualizer" "${INSTALL_DIR}/mnt_tensor_visualizer"
chmod +x "${INSTALL_DIR}/mnt_tensor_visualizer"

echo ""
echo "Installed to ${INSTALL_DIR}/mnt_tensor_visualizer"
case ":$PATH:" in
    *":${INSTALL_DIR}:"*)
        echo "Run it with: mnt_tensor_visualizer"
        ;;
    *)
        echo "Add ${INSTALL_DIR} to your PATH, or run it directly with: ${INSTALL_DIR}/mnt_tensor_visualizer"
        ;;
esac
