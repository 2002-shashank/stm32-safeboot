/*
 * Application, linked for one slot. SB_APP_ORIGIN (slot base + 0x200, after
 * the image header) is passed with --defsym by the build: slot A 0x08020200,
 * slot B 0x08080200. Values mirror core/include/sb/layout.h.
 */
MEMORY
{
    FLASH (rx)  : ORIGIN = SB_APP_ORIGIN, LENGTH = 384K - 512
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 124K   /* top 4 KB reserved: RTT + RAM functions */
}

_estack = ORIGIN(RAM) + LENGTH(RAM);
_min_stack = 4K;

SECTIONS
{
    .isr_vector : { KEEP(*(.isr_vector)) } > FLASH
    .text : {
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > FLASH
    .ARM.exidx : { *(.ARM.exidx*) } > FLASH

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

    /* The app has no RAM functions; startup copies an empty range. */
    _siramfunc = 0;
    _sramfunc = 0;
    _eramfunc = 0;

    /* Image must end on a word boundary (image_size % 4 == 0). */
    _app_end = LOADADDR(.data) + SIZEOF(.data);
    ASSERT(_ebss + _min_stack <= _estack, "app RAM: not enough room for the stack")
}
