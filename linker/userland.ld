OUTPUT_FORMAT(elf32-i386)
ENTRY(userland_entry)

SECTIONS
{
    . = 0x20000;

    .text : ALIGN(4)
    {
        KEEP(*(.entry))
        *(.text*)
    }

    .rodata : ALIGN(4)
    {
        *(.rodata*)
    }

    .data : ALIGN(4)
    {
        *(.data*)
    }

    .bss : ALIGN(4)
    {
        *(.bss*)
        *(COMMON)
    }

    /DISCARD/ :
    {
        *(.comment*)
        *(.eh_frame*)
    }
}
