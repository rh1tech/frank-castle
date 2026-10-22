#!/bin/bash
#
# frank-castle — Castlevania: Symphony of the Night port for RP2350
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-castle
#
# Licensed under the GNU Affero General Public License v3.0 or later.
# See LICENSE for the full license text.
#
# Usage: ./build.sh [M2] [CPU_MHz] [PSRAM_MHz] [FLASH_MHz]
# Defaults: M2, 504 MHz CPU, 133 MHz PSRAM, 66 MHz flash.
# USB is always the keyboard host; the debug console is UART0 on GPIO0/1.

set -e

BOARD=${1:-M2}
CPU=${2:-504}
PSRAM=${3:-133}
FLASH=${4:-66}

if [ -z "$PICO_SDK_PATH" ] || [ ! -f "$PICO_SDK_PATH/pico_sdk_init.cmake" ]; then
    export PICO_SDK_PATH="$HOME/pico/pico-sdk"
fi

echo "Building frank-castle: Board=$BOARD CPU=${CPU}MHz PSRAM=${PSRAM}MHz Flash=${FLASH}MHz"

rm -rf ./build
mkdir build
cd build
cmake -DPICO_PLATFORM=rp2350 \
      -DBOARD_VARIANT=$BOARD \
      -DCPU_SPEED=$CPU \
      -DPSRAM_SPEED=$PSRAM \
      -DFLASH_SPEED=$FLASH \
      ..
make -j8
