#
# frank-castle — Castlevania: Symphony of the Night port for RP2350
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-castle
#
# Licensed under the GNU Affero General Public License v3.0 or later.
# See LICENSE for the full license text.
#

cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD frank_m2_rp2350b CACHE STRING "Pico board type")
set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/boards)
set(BOARD_VARIANT "M2" CACHE STRING "Board variant (only M2 for now)")
set(CPU_SPEED "504" CACHE STRING "CPU clock in MHz: 252, 378, 504")
set(PSRAM_SPEED "133" CACHE STRING "PSRAM max frequency in MHz: 84, 100, 133, 166")
set(FLASH_SPEED "66" CACHE STRING "Flash chip max frequency in MHz")
# Native USB is always the keyboard host; debug console is UART0 (GPIO0/1).
set(USB_HID_ENABLED ON)

if(CPU_SPEED GREATER_EQUAL 504)
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_65")
elseif(CPU_SPEED GREATER_EQUAL 378)
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_60")
else()
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_50")
endif()

set(SD_PIN_SCK 6)
set(SD_PIN_MOSI 7)
set(SD_PIN_MISO 4)
set(SD_PIN_CS 5)
set(I2S_DATA_PIN 9)
set(I2S_CLK_PIN 10)

include(pico_sdk_import.cmake)

project(frank-castle C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

file(READ "${CMAKE_CURRENT_LIST_DIR}/version.txt" CASTLE_VERSION_RAW)
string(STRIP "${CASTLE_VERSION_RAW}" CASTLE_VERSION_RAW)
string(REGEX REPLACE "[ \t]+" ";" CASTLE_VERSION_PARTS "${CASTLE_VERSION_RAW}")
list(GET CASTLE_VERSION_PARTS 0 CASTLE_VERSION_MAJOR)
list(GET CASTLE_VERSION_PARTS 1 CASTLE_VERSION_MINOR)
if(CASTLE_VERSION_MINOR LESS 10)
    set(CASTLE_VERSION "${CASTLE_VERSION_MAJOR}.0${CASTLE_VERSION_MINOR}")
else()
    set(CASTLE_VERSION "${CASTLE_VERSION_MAJOR}.${CASTLE_VERSION_MINOR}")
endif()

pico_sdk_init()

set(SOTN_DIR ${CMAKE_CURRENT_LIST_DIR}/third_party/sotn-decomp)

# ---------------------------------------------------------------------
# Drivers (shared with the other FRANK ports)
# ---------------------------------------------------------------------
add_subdirectory(drivers/fatfs)
add_subdirectory(drivers/sdcard)
add_subdirectory(drivers/usbhid)
add_subdirectory(drivers/audio)

add_library(drivers STATIC
    drivers/psram_init.c
    drivers/psram_allocator.c
    drivers/psram_heap.c
    drivers/dlmalloc.c
    drivers/hdmi/psx_video.c
    drivers/hdmi/hstx_packet.c
    drivers/hdmi/hstx_data_island_queue.c
)
target_include_directories(drivers PUBLIC drivers drivers/hdmi)
target_link_libraries(drivers PUBLIC pico_stdlib hardware_dma hardware_irq hardware_clocks hardware_gpio)
set_source_files_properties(drivers/dlmalloc.c PROPERTIES COMPILE_DEFINITIONS
    "ONLY_MSPACES=1;HAVE_MMAP=0;HAVE_MORECORE=0;NO_MALLOC_STATS=1;USE_LOCKS=0;ABORT_ON_ASSERT_FAILURE=0;MALLOC_FAILURE_ACTION=;LACKS_SYS_MMAN_H=1;LACKS_UNISTD_H=1;LACKS_SYS_PARAM_H=1"
)
target_compile_definitions(drivers PUBLIC
    PSRAM_MAX_FREQ_MHZ=${PSRAM_SPEED}
    PSXV_PIN_BASE=12
)

set(CASTLE_DEFINES
    BOARD_${BOARD_VARIANT}
    CPU_CLOCK_MHZ=${CPU_SPEED}
    CPU_VOLTAGE=${CPU_VOLTAGE}
    PSRAM_MAX_FREQ_MHZ=${PSRAM_SPEED}
    FLASH_MAX_FREQ_MHZ=${FLASH_SPEED}
    PICO_AUDIO_I2S_DATA_PIN=${I2S_DATA_PIN}
    PICO_AUDIO_I2S_CLOCK_PIN_BASE=${I2S_CLK_PIN}
    CASTLE_VERSION="${CASTLE_VERSION}"
)

# Platform code shared by the diag and game firmware
add_library(castle_platform STATIC
    src/common/font6x8.c
    src/common/fbcon.c
    ${SOTN_DIR}/src/pc/cdimage.c
    src/rp2350/platform.c
    src/rp2350/cd_fatfs.c
)
target_include_directories(castle_platform PUBLIC src/common src/rp2350 ${SOTN_DIR}/src/pc)
target_compile_definitions(castle_platform PUBLIC ${CASTLE_DEFINES})
target_link_libraries(castle_platform PUBLIC
    pico_stdlib pico_multicore hardware_clocks hardware_vreg hardware_watchdog
    drivers fatfs sdcard usbhid audio_driver)

# ---------------------------------------------------------------------
# Phase 0: hardware bring-up firmware
# ---------------------------------------------------------------------
add_executable(frank-castle-diag src/diag/diag_main.c)
target_link_libraries(frank-castle-diag castle_platform)
target_link_options(frank-castle-diag PRIVATE -Wl,--wrap=panic)
pico_set_linker_script(frank-castle-diag ${CMAKE_CURRENT_SOURCE_DIR}/memmap_psram.ld)
pico_enable_stdio_usb(frank-castle-diag 0)
pico_enable_stdio_uart(frank-castle-diag 1)
pico_add_extra_outputs(frank-castle-diag)
