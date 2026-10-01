/*
 * M74.9 OpenBLT second-stage linker layout.
 *
 * OpenBLT runs behind the OEM resident loader, which boots it through the fixed
 * application contract: vectors at 0x08001000 (SP = 0x20020000, reset =
 * 0x08080001) and Thumb entry at 0x08080000. The OEM loader decides boot by
 * the validity marker only (see boot_activation.h), so OpenBLT is the "app"
 * it jumps to; OpenBLT then validates the main rusEFI image and either starts
 * it or stays in XCP mode.
 *
 * Segment 1 (0x08001000-0x0805FFFF) holds OpenBLT's vectors + rodata + data
 * LMA; the main app starts at 0x08010000 (see m749.ld). The activation
 * descriptor at 0x0805FFE0 is emitted by OpenBLT. Segment 2 (0x08080000+)
 * holds OpenBLT's code in its first 64 KiB; the main app code starts at
 * 0x08090000.
 */
MEMORY
{
    boot_intent (rw) : org = 0x20000000, len = 4
    mstack (rw) : org = 0x20020000 - __main_stack_size__, len = __main_stack_size__
    flash0 (rx) : org = 0x08001000, len = 0xF000
    activation (rx) : org = 0x0805FFE0, len = 32
    flash1 (rx) : org = 0x08080000, len = 0x10000
    flash2 (rx) : org = 0x00000000, len = 0
    flash3 (rx) : org = 0x00000000, len = 0
    flash4 (rx) : org = 0x00000000, len = 0
    flash5 (rx) : org = 0x00000000, len = 0
    flash6 (rx) : org = 0x00000000, len = 0
    flash7 (rx) : org = 0x00000000, len = 0
    ram0   (wx) : org = 0x20020000, len = 256k
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
REGION_ALIAS("HEAP_RAM", ram0);

/* Preserve the boot intent word across C runtime init (both OEM tokens). */
SECTIONS {
    .boot_intent (NOLOAD) : { KEEP(*(.boot_intent)) } > boot_intent
    .m749_startup : { KEEP(*(.m749_startup)) } > flash1
    .m749_activation : { KEEP(*(.m749_activation)) } > activation
}

INCLUDE rules.ld

ASSERT(ADDR(.vectors) == 0x08001000, "M74.9 OpenBLT vectors moved")
ASSERT(__main_stack_end__ == 0x20020000, "M74.9 OpenBLT initial SP moved")
ASSERT((Reset_Handler & ~1) == 0x08080000, "M74.9 OpenBLT reset entry moved")
ASSERT(SIZEOF(.boot_intent) == 4, "M74.9 boot intent must be reserved")
ASSERT(SIZEOF(.m749_activation) == 32, "M74.9 activation descriptor missing")
