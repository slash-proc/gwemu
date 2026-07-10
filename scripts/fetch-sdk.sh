#!/usr/bin/env bash
# Fetches the STM32CubeH7 SDK pieces relevant to STM32H7B0 (HAL driver
# source, device-specific CMSIS, core CMSIS headers) into ./sdk/, gitignored
# -- reference material for peripheral register behavior when writing
# device models (RCC, DMA2D, OCTOSPI, etc.), same role as STM32H7B0.svd and
# rm0455.pdf. Not a build dependency: this QEMU fork doesn't compile
# against it, it's just ground truth to read.
#
# Version pins match ../../game-and-watch-retro-go-sd/Makefile.common
# exactly, since that's the real toolchain G&W firmware builds against --
# keep these in sync with that file if it bumps versions.

set -euo pipefail
cd "$(dirname "$0")/.."

SDK_VERSION=v1.13.0
CMSIS_DEVICE_VERSION=v1.10.7
HAL_DRIVER_VERSION=v1.11.6

SDK_DIR=sdk
HAL_DRIVER_REPO=https://github.com/STMicroelectronics/stm32h7xx-hal-driver
CMSIS_DEVICE_REPO=https://github.com/STMicroelectronics/cmsis-device-h7
CUBE_RAW_URL=https://raw.githubusercontent.com/STMicroelectronics/STM32CubeH7

clone_pinned() {
    local repo="$1" tag="$2" dest="$3"
    if [ -d "$dest/.git" ]; then
        echo "[fetch-sdk] $dest already present, skipping clone"
        return
    fi
    echo "[fetch-sdk] cloning $repo @ $tag -> $dest"
    git clone --depth 1 --branch "$tag" "$repo" "$dest"
}

clone_pinned "$HAL_DRIVER_REPO" "$HAL_DRIVER_VERSION" "$SDK_DIR/stm32h7xx-hal-driver"
clone_pinned "$CMSIS_DEVICE_REPO" "$CMSIS_DEVICE_VERSION" "$SDK_DIR/cmsis-device-h7"

# Core ARM CMSIS headers (not device-specific) live in the STM32CubeH7
# umbrella repo, which is too large to clone whole just for a handful of
# headers -- fetch the same individual files
# game-and-watch-retro-go-sd/Makefile.common's SDK_HEADERS list pulls.
CMSIS_CORE_DIR="$SDK_DIR/cmsis-core/Drivers/CMSIS/Include"
mkdir -p "$CMSIS_CORE_DIR"
for f in cachel1_armv7.h cmsis_compiler.h cmsis_gcc.h cmsis_version.h core_cm7.h; do
    dest="$CMSIS_CORE_DIR/$f"
    if [ -f "$dest" ]; then
        continue
    fi
    echo "[fetch-sdk] wget $f"
    wget -q "$CUBE_RAW_URL/$SDK_VERSION/Drivers/CMSIS/Include/$f" -O "$dest"
done

echo "[fetch-sdk] done. SDK reference material in $SDK_DIR/ (gitignored)."
