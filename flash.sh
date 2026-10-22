#!/bin/bash
# Flash a frank-castle build with picotool. Usage: ./flash.sh [target]  (default: frank-castle-diag)
TARGET=${1:-frank-castle-diag}
picotool load -f "build/${TARGET}.uf2" && picotool reboot
