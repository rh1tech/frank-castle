#!/bin/bash
# frank-castle — reflash over USB without touching the board.
# The running firmware reboots into BOOTSEL on "~BOOT~" (UART), then
# picotool loads the UF2 and starts it.
# Usage: tools/usb_flash.sh [build/frank-castle.uf2]
# SPDX-License-Identifier: AGPL-3.0-or-later
set -e
UF2=${1:-build/frank-castle.uf2}
cd "$(dirname "$0")/.."
if ! picotool info >/dev/null 2>&1; then
    python3 tools/uart.py bootsel
    for i in $(seq 1 50); do
        picotool info >/dev/null 2>&1 && break
        sleep 0.2
    done
fi
picotool info >/dev/null 2>&1 || { echo "ERROR: board did not enter BOOTSEL" >&2; exit 1; }
picotool load -x "$UF2" 2>&1 | grep -v "%" 
