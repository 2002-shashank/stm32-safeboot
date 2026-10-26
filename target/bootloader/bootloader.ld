/* Bootloader: S0-S1 (32 KB) at 0x08000000. Values mirror core/include/sb/layout.h. */
MEMORY
{
    FLASH   (rx)  : ORIGIN = 0x08000000, LENGTH = 32K - 256
    API     (r)   : ORIGIN = 0x08007F00, LENGTH = 256
    RAM     (rwx) : ORIGIN = 0x20000000, LENGTH = 124K
    RTT     (rw)  : ORIGIN = 0x2001F000, LENGTH = 3K
    RAMFUNC (rwx) : ORIGIN = 0x2001FC00, LENGTH = 1K
}

_estack = ORIGIN(RAM) + LENGTH(RAM);
_min_stack = 8K;

SECTIONS
{
    .isr_vector : { KEEP(*(.isr_vector)) } > FLASH
    .text : {
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > FLASH
    .ARM.exidx : { *(.ARM.exidx*) } > FLASH

    .ramfunc : {
        _sramfunc = .;
        *(.ramfunc*)
        . = ALIGN(4);
        _eramfunc = .;
    } > RAMFUNC AT > FLASH
    _siramfunc = LOADADDR(.ramfunc);

    .data : {
        _sdata = .;
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > FLASH
    _sidata = LOADADDR(.data);

    .bss (NOLOAD) : {
        _sbss = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM

    /* Not zeroed by startup: survives a system reset (not a power cycle). */
    .noinit (NOLOAD) : { *(.noinit*) } > RAM
    _ebss_noinit = .;

    .sb_api : { KEEP(*(.sb_api)) } > API

    ASSERT(_ebss_noinit + _min_stack <= _estack, "bootloader RAM: not enough room for the stack")
    ASSERT(LOADADDR(.data) + SIZEOF(.data) <= ORIGIN(API), "bootloader exceeds 32 KB budget")
}
