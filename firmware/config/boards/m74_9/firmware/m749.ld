/*
 * M74.9 three-stage main-application linker layout.
 *
 * The main rusEFI application runs as the "user program" of the OpenBLT
 * second-stage bootloader (m749_openblt.ld). OpenBLT owns the fixed OEM-loader
 * application contract (vectors 0x08001000, entry 0x08080000) and the
 * activation descriptor at 0x0805FFE0; the main app is relocated above OpenBLT:
 *
 *   vectors + rodata + data LMA : 0x08010000-0x0805FFDF (segment 1)
 *   code (TEXT)                 : 0x08090000-0x080FFFFB (segment 2)
 *
 * OpenBLT sets VTOR = 0x08010000 before jumping here, so this image uses the
 * standard ChibiOS reset handler. The initial SP stays 0x20020000 (OEM
 * contract), and low SRAM (0x20000000) keeps the boot-intent token reserved.
 *
 * The .fast_text section (trigger decode + event scheduling, see
 * efifeatures.h EFI_TRIGGER_IN_RAM) is copied to zero-wait SRAM at boot by
 * board.c __early_init, mirroring AT32F435ZMxx.ld.
 */
MEMORY
{
    boot_intent (rw) : org = 0x20000000, len = 4
    mstack (rw) : org = 0x20020000 - __main_stack_size__, len = __main_stack_size__
    flash0 (rx) : org = 0x08010000, len = 0x4F000
    flash1 (rx) : org = 0x08090000, len = 0x6FFFC
    flash2 (rx) : org = 0x00000000, len = 0
    flash3 (rx) : org = 0x00000000, len = 0
    flash4 (rx) : org = 0x00000000, len = 0
    flash5 (rx) : org = 0x00000000, len = 0
    flash6 (rx) : org = 0x00000000, len = 0
    flash7 (rx) : org = 0x00000000, len = 0
    ram0   (wx) : org = 0x20020000, len = 192k
    ram_fast (wx) : org = 0x20050000, len = 64k
    ram_heap (wx) : org = 0x20020000, len = 128k
    ram1   (wx) : org = 0x00000000, len = 0
    ram2   (wx) : org = 0x00000000, len = 0
    ram3   (wx) : org = 0x20020000, len = 0
    ram4   (wx) : org = 0x10000000, len = 0
    ram5   (wx) : org = 0x40024000, len = 0
    ram6   (wx) : org = 0x00000000, len = 0
    ram7   (wx) : org = 0x00000000, len = 0
}

REGION_ALIAS("VECTORS_FLASH", flash0);
REGION_ALIAS("VECTORS_FLASH_LMA", flash0);
REGION_ALIAS("XTORS_FLASH", flash0);
REGION_ALIAS("XTORS_FLASH_LMA", flash0);
REGION_ALIAS("TEXT_FLASH", flash1);
REGION_ALIAS("TEXT_FLASH_LMA", flash1);
REGION_ALIAS("RODATA_FLASH", flash0);
REGION_ALIAS("RODATA_FLASH_LMA", flash0);
REGION_ALIAS("VARIOUS_FLASH", flash0);
REGION_ALIAS("VARIOUS_FLASH_LMA", flash0);
REGION_ALIAS("RAM_INIT_FLASH_LMA", flash0);
REGION_ALIAS("MAIN_STACK_RAM", mstack);
REGION_ALIAS("PROCESS_STACK_RAM", ram0);
REGION_ALIAS("DATA_RAM", ram0);
REGION_ALIAS("DATA_RAM_LMA", flash0);
REGION_ALIAS("BSS_RAM", ram0);
REGION_ALIAS("HEAP_RAM", ram_heap);

/* Preserve the boot intent word across C runtime init (both OEM tokens). */
SECTIONS {
    .boot_intent (NOLOAD) : { KEEP(*(.boot_intent)) } > boot_intent
}

INCLUDE rules.ld

/* Trigger fast path: copied from flash to zero-wait SRAM at boot (board.c). */
SECTIONS
{
    .fast_text : ALIGN(4)
    {
        . = ALIGN(4);
        __fast_text_start__ = .;
        *(.fast_text)
        *(.fast_text.*)
        . = ALIGN(4);
        __fast_text_end__ = .;
    } > ram_fast AT > flash0
}

__fast_text_load__ = LOADADDR(.fast_text);
__fast_text_size__ = __fast_text_end__ - __fast_text_start__;

ASSERT(ADDR(.vectors) == 0x08010000, "M74.9 main app vectors moved")
ASSERT(__main_stack_end__ == 0x20020000, "M74.9 main app initial SP moved")
ASSERT(SIZEOF(.boot_intent) == 4, "M74.9 boot intent must be reserved")
