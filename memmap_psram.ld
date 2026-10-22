/*
 * frank-castle — Castlevania: Symphony of the Night port for RP2350
 * Linker script taken from frank-fodder (GPL-3.0-or-later), flash raised to 16 MB.
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://github.com/rh1tech/frank-castle
 */

/* Custom linker script for RP2350 with PSRAM sections
 * Based on Quake port approach - large BSS arrays go to PSRAM
 * This extends the default pico-sdk linker script
 */

/* Reserve 16KB at end of RAM for stack - Duke3D needs more than 4KB */
__main_stack_size__ = 32K;

MEMORY
{
    FLASH(rx) : ORIGIN = 0x10000000, LENGTH = 16384k
    RAM(rwx) : ORIGIN =  0x20000000, LENGTH = 512k - 32k
    STACK_RAM(rwx) : ORIGIN = 0x20078000, LENGTH = 32k
    SCRATCH_X(rwx) : ORIGIN = 0x20080000, LENGTH = 4k
    SCRATCH_Y(rwx) : ORIGIN = 0x20081000, LENGTH = 4k
    PSRAM(rwx) : ORIGIN = 0x11000000, LENGTH = 8192k
}

ENTRY(_entry_point)

SECTIONS
{
    .flash_begin : {
        __flash_binary_start = .;
    } > FLASH

    .text : {
        __logical_binary_start = .;
        KEEP (*(.vectors))
        KEEP (*(.binary_info_header))
        __binary_info_header_end = .;
        KEEP (*(.embedded_block))
        __embedded_block_end = .;
        KEEP (*(.reset))
        *(.init)
        *libgcc.a:cmse_nonsecure_call.o
        *(EXCLUDE_FILE(*libgcc.a: *libc.a:*lib_a-mem*.o *libm.a:) .text*)
        *(.fini)
        *crtbegin.o(.ctors)
        *crtbegin?.o(.ctors)
        *(EXCLUDE_FILE(*crtend?.o *crtend.o) .ctors)
        *(SORT(.ctors.*))
        *(.ctors)
        *crtbegin.o(.dtors)
        *crtbegin?.o(.dtors)
        *(EXCLUDE_FILE(*crtend?.o *crtend.o) .dtors)
        *(SORT(.dtors.*))
        *(.dtors)

        . = ALIGN(4);
        PROVIDE_HIDDEN (__preinit_array_start = .);
        KEEP(*(SORT(.preinit_array.*)))
        KEEP(*(.preinit_array))
        PROVIDE_HIDDEN (__preinit_array_end = .);

        . = ALIGN(4);
        PROVIDE_HIDDEN (__init_array_start = .);
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        PROVIDE_HIDDEN (__init_array_end = .);

        . = ALIGN(4);
        PROVIDE_HIDDEN (__fini_array_start = .);
        *(SORT(.fini_array.*))
        *(.fini_array)
        PROVIDE_HIDDEN (__fini_array_end = .);

        *(.eh_frame*)
        . = ALIGN(4);
    } > FLASH

    .boot2 : {
        __boot2_start__ = .;
        *(.boot2)
        __boot2_end__ = .;
    } > FLASH

    ASSERT(__boot2_end__ - __boot2_start__ <= 256,
        "ERROR: Pico second stage bootloader must be no more than 256 bytes in size")

    .rodata : {
        *(EXCLUDE_FILE(*libgcc.a: *libc.a:*lib_a-mem*.o *libm.a:) .rodata*)
        *(.srodata*)
        . = ALIGN(4);
        *(SORT_BY_ALIGNMENT(SORT_BY_NAME(.flashdata*)))
        . = ALIGN(4);
    } > FLASH

    .ARM.extab :
    {
        *(.ARM.extab* .gnu.linkonce.armextab.*)
    } > FLASH

    __exidx_start = .;
    .ARM.exidx :
    {
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
    } > FLASH
    __exidx_end = .;

    . = ALIGN(4);
    __binary_info_start = .;
    .binary_info :
    {
        KEEP(*(.binary_info.keep.*))
        *(.binary_info.*)
    } > FLASH
    __binary_info_end = .;
    . = ALIGN(4);

    .ram_vector_table (NOLOAD): {
        *(.ram_vector_table)
    } > RAM

    .uninitialized_data (NOLOAD): {
        . = ALIGN(4);
        *(.uninitialized_data*)
    } > RAM

    .data : {
        __data_start__ = .;
        *(vtable)

        *(.time_critical*)

        *(.text*)
        . = ALIGN(4);
        *(.rodata*)
        . = ALIGN(4);

        *(.data*)
        *(.sdata*)

        . = ALIGN(4);
        *(.after_data.*)
        . = ALIGN(4);
        PROVIDE_HIDDEN (__mutex_array_start = .);
        KEEP(*(SORT(.mutex_array.*)))
        KEEP(*(.mutex_array))
        PROVIDE_HIDDEN (__mutex_array_end = .);

        *(.jcr)
        . = ALIGN(4);
    } > RAM AT> FLASH

    .tdata : {
        . = ALIGN(4);
		*(.tdata .tdata.* .gnu.linkonce.td.*)
        __tdata_end = .;
    } > RAM AT> FLASH
    PROVIDE(__data_end__ = .);

    __etext = LOADADDR(.data);

    .tbss (NOLOAD) : {
        . = ALIGN(4);
        __bss_start__ = .;
        __tls_base = .;
        *(.tbss .tbss.* .gnu.linkonce.tb.*)
        *(.tcommon)

        __tls_end = .;
    } > RAM

    .bss (NOLOAD) : {
        . = ALIGN(4);
        __tbss_end = .;

        /* Only keep platform/SDK BSS in SRAM. Game BSS goes to psram_bss below. */
        *libgcc*(.bss .bss.*)
        *libc*(.bss .bss.*)
        /* libstdc++ locale/guard variables use atomics (ldrex/strex) —
           these MUST be in SRAM, not PSRAM which lacks exclusive access.
           Match by archive name AND by individual .o names from libstdc++. */
        *libstdc++*(.bss .bss.*)
        *locale*(.bss .bss.*)
        *ctype*(.bss .bss.*)
        *codecvt*(.bss .bss.*)
        *collate*(.bss .bss.*)
        *numpunct*(.bss .bss.*)
        *moneypunct*(.bss .bss.*)
        *timepunct*(.bss .bss.*)
        *messages*(.bss .bss.*)
        *ios_init*(.bss .bss.*)
        *cow-locale_init*(.bss .bss.*)
        *cow-stdexcept*(.bss .bss.*)
        *pico_*(.bss .bss.*)
        *hardware_*(.bss .bss.*)
        *HDMI*(.bss .bss.*)
        *psram*(.bss .bss.*)
        *sdcard*(.bss .bss.*)
        *ps2*(.bss .bss.*)
        *usbhid*(.bss .bss.*)
        *tinyusb*(.bss .bss.*)
        *tusb*(.bss .bss.*)
        *dcd_*(.bss .bss.*)
        *usbd*(.bss .bss.*)
        *cdc_*(.bss .bss.*)
        *fatfs*(.bss .bss.*)
        *ff.c.o(.bss .bss.*)
        *ffsystem*(.bss .bss.*)
        *main.c.o(.bss .bss.*)
        *welcome*(.bss .bss.*)
        *system_rp2350*(.bss .bss.*)
        *video_rp2350*(.bss .bss.*)
        *rp2350layer*(.bss.frame_buf)
        /* view.cpp back in PSRAM to save SRAM for larger stack */
        *sound_rp2350*(.bss .bss.*)
        *blood_rp2350*(.bss .bss.*)
        *compat_rp2350*(.bss .bss.*)
        *fatfs_stdio*(.bss .bss.*)
        /* Game objects that need fast/reliable SRAM access */
        *globals*(.bss .bss.*)
        *gui.cpp*(.bss .bss.*)
        *resource*(.bss .bss.*)
        /* Global shared_ptrs and game core objects — need SRAM for atomics */
        *stdafx*(.bss .bss.*)
        *qheap*(.bss .bss.*)
        *config*(.bss .bss.*)
        *blood.cpp*(.bss .bss.*)
        *key*(.bss .bss.*)
        *error*(.bss .bss.*)
        *debug4g*(.bss .bss.*)
        *cache1d*(.bss .bss.*)
        *a_asm*(.bss .bss.*)
        *video_font*(.bss .bss.*)
        /* MACT library - keep in SRAM (small, needs fast access from flash text) */
        *control*(.bss .bss.*)
        *keyboard*(.bss .bss.*)
        *scriplib*(.bss .bss.*)
        *tokenlib*(.bss .bss.*)
        *animlib*(.bss .bss.*)
        *file_lib*(.bss .bss.*)
        *util_lib*(.bss .bss.*)
        *mathutil*(.bss .bss.*)
        /* Audiolib - keep in SRAM */
        *audio*(.bss .bss.*)
        *multivoc*(.bss .bss.*)
        *fx_man*(.bss .bss.*)
        *midi*(.bss .bss.*)
        *music*(.bss .bss.*)
        *blaster*(.bss .bss.*)
        *task_man*(.bss .bss.*)
        *al_midi*(.bss .bss.*)
        *pitch*(.bss .bss.*)
        /* Catch-all: keep ALL remaining BSS in SRAM.
         * PSRAM reads are unreliable for small variables (corrupt pointers,
         * ldrex/strex failures). Total game BSS is small enough for SRAM. */
        *(.bss .bss.*)
        *(COMMON)
        PROVIDE(__global_pointer$ = . + 2K);
        *(.sbss*)
        . = ALIGN(4);
        __bss_end__ = .;
    } > RAM

    .heap (NOLOAD):
    {
        __end__ = .;
        end = __end__;
        KEEP(*(.heap*))
    } > RAM
    __HeapLimit = ORIGIN(RAM) + LENGTH(RAM);

    /* ============ PSRAM sections ============ */
    /* Data that needs initialization from flash */
    .psram_data : {
        __psram_data_start__ = .;
        *(.psram_data*)
        . = ALIGN(4);
        __psram_data_end__ = .;
    } > PSRAM AT> FLASH
    __psram_data_load__ = LOADADDR(.psram_data);

    /* Zero-initialized data in PSRAM */
    .psram_bss (NOLOAD) : {
        __psram_bss_start__ = .;
        *(SORT_BY_ALIGNMENT(SORT_BY_NAME(.psram_bss*)))
        . = ALIGN(4);
        __psram_bss_end__ = .;
    } > PSRAM

    /* Heap in PSRAM for psram_malloc */
    .psram_heap (NOLOAD) : {
        __psram_heap_start__ = .;
        KEEP(*(.psram_heap*))
    } > PSRAM
    /* ============ End PSRAM sections ============ */

    .scratch_x : {
        __scratch_x_start__ = .;
        *(.scratch_x.*)
        . = ALIGN(4);
        __scratch_x_end__ = .;
    } > SCRATCH_X AT > FLASH
    __scratch_x_source__ = LOADADDR(.scratch_x);

    .scratch_y : {
        __scratch_y_start__ = .;
        *(.scratch_y.*)
        . = ALIGN(4);
        __scratch_y_end__ = .;
    } > SCRATCH_Y AT > FLASH
    __scratch_y_source__ = LOADADDR(.scratch_y);

    .stack1_dummy (NOLOAD):
    {
        *(.stack1*)
    } > SCRATCH_X
    
    /* Main stack - use larger STACK_RAM region instead of tiny SCRATCH_Y */
    .stack_dummy (NOLOAD):
    {
        . = . + __main_stack_size__;
    } > STACK_RAM

    .flash_end : {
        KEEP(*(.embedded_end_block*))
        PROVIDE(__flash_binary_end = .);
    } > FLASH =0xaa

    __StackLimit = ORIGIN(STACK_RAM);
    __StackOneTop = ORIGIN(SCRATCH_X) + LENGTH(SCRATCH_X);
    __StackTop = ORIGIN(STACK_RAM) + LENGTH(STACK_RAM);
    __StackOneBottom = __StackOneTop - SIZEOF(.stack1_dummy);
    __StackBottom = __StackLimit;
    PROVIDE(__stack = __StackTop);

    PROVIDE (__heap_start = __end__);
    PROVIDE (__heap_end = __HeapLimit);
    PROVIDE( __tls_align = MAX(ALIGNOF(.tdata), ALIGNOF(.tbss)) );
    PROVIDE( __tls_size_align = (__tls_size + __tls_align - 1) & ~(__tls_align - 1));
    PROVIDE( __arm32_tls_tcb_offset = MAX(8, __tls_align) );

    PROVIDE (_end = __end__);
    PROVIDE (__llvm_libc_heap_limit = __HeapLimit);

    ASSERT(__StackLimit >= __HeapLimit, "region RAM overflowed")
    ASSERT( __binary_info_header_end - __logical_binary_start <= 1024, "Binary info must be in first 1024 bytes of the binary")
    ASSERT( __embedded_block_end - __logical_binary_start <= 4096, "Embedded block must be in first 4096 bytes of the binary")
}
