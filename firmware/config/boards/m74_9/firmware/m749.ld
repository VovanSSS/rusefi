/*
 * M74.9 three-stage main-application linker layout.
 *
 * The main rusEFI application runs as the "user program" of the OpenBLT
 * second-stage bootloader (m749_openblt.ld). OpenBLT owns the fixed OEM-loader
 * application contract (vectors 0x08001000, entry 0x08080000) and the
 * activation descriptor at 0x0805FFE0; the main app is relocated above OpenBLT:
 *
 *   vectors + ctors            : 0x08010000-0x08010FFF (fixed 4 KiB page)
 *   hot code (.m749_fast_text)  : 0x08011000+, first section of segment 1
 *   rodata + data LMA + misc    : after the hot code in segment 1
 *   string literals             : 0x08090000+ (.m749_rodata_nzw, segment 2)
 *   code (TEXT)                 : 0x08090000-0x080FFFFB (segment 2)
 *
 * OpenBLT sets VTOR = 0x08010000 before jumping here, so this image uses the
 * standard ChibiOS reset handler. The initial SP stays 0x20020000 (OEM
 * contract), and low SRAM (0x20000000) keeps the boot-intent token reserved.
 *
 * The fixed .shared region (0x20000004, 16 bytes) holds the OpenBLT shared
 * parameters (reset counters, boot-intent flag) - it must be at the SAME
 * address in BOTH this script and m749_openblt.ld, and outside the app
 * data/bss: as an orphan section the app build placed it at 0x20020c80 while
 * the bootloader build placed it at 0x20020804, and the app's .data copy
 * (0x20020600..) overwrote the bootloader's copy every boot, killing the
 * reset-loop latch counters.
 *
 * The ChibiOS heap/memcore lives in the free low SRAM between the shared
 * params and the main stack (0x20000014-0x2001F000, ~124 KiB). It must NOT be
 * aliased to a sub-range of ram0: the app data/bss fills nearly all of ram0
 * (bss ends at ~0x2004FF04), so a heap region whose origin lies inside ram0
 * gets placed at that origin - INSIDE the live bss - and every heap
 * allocation (the Lua arena) silently corrupts static objects. That was the
 * 2026-10-02 'LOGBUF clobber' root cause: the Lua heap overwrote
 * buffers[0].m_writePtr and truncated the console log. The ASSERT at the
 * bottom guards the invariant.
 *
 * The AT32F435 zero-wait (ZW) flash window holds the hot code: with the
 * board-mandated 384 KiB SRAM option (EOPB0 = 0x05FA) the ZW region is
 * 0x08000000-0x0803FFFF (256 KiB), and the application's share of it is
 * 0x08010000-0x0803FFFF (192 KiB). .m749_fast_text captures interrupt,
 * scheduler, trigger and engine hot paths by function-section name and is
 * placed at the front of segment 1 so its instruction fetches stay in ZW;
 * everything else (the TEXT region) stays in the slower NZW part of flash.
 * The ASSERT at the bottom bounds the section to the ZW window. The board
 * cannot run with a smaller ZW config: its RAM map spans 0x20000000-
 * 0x2005FFFF, which already requires the 384 KiB SRAM option.
 *
 * The .fast_text section (trigger decode + event scheduling, see
 * efifeatures.h EFI_TRIGGER_IN_RAM) is copied to zero-wait SRAM at boot by
 * board.c __early_init, mirroring AT32F435ZMxx.ld. It stays on top of the
 * ZW placement: explicitly marked functions keep the proven RAM path while
 * the ZW window covers the rest of the hot set.
 */
MEMORY
{
    boot_intent (rw) : org = 0x20000000, len = 4
    shared (rw) : org = 0x20000004, len = 16
    ram_heap (wx) : org = 0x20000014, len = 0x1EFEC
    mstack (rw) : org = 0x20020000 - __main_stack_size__, len = __main_stack_size__
    vectors (rx) : org = 0x08010000, len = 0x1000
    flash0 (rx) : org = 0x08011000, len = 0x4E000
    flash1 (rx) : org = 0x08090000, len = 0x6FFFC
    flash2 (rx) : org = 0x00000000, len = 0
    flash3 (rx) : org = 0x00000000, len = 0
    flash4 (rx) : org = 0x00000000, len = 0
    flash5 (rx) : org = 0x00000000, len = 0
    flash6 (rx) : org = 0x00000000, len = 0
    flash7 (rx) : org = 0x00000000, len = 0
    ram0   (wx) : org = 0x20020000, len = 192k
    ram_fast (wx) : org = 0x20050000, len = 64k
    ram1   (wx) : org = 0x00000000, len = 0
    ram2   (wx) : org = 0x00000000, len = 0
    ram3   (wx) : org = 0x20020000, len = 0
    ram4   (wx) : org = 0x10000000, len = 0
    ram5   (wx) : org = 0x40024000, len = 0
    ram6   (wx) : org = 0x00000000, len = 0
    ram7   (wx) : org = 0x00000000, len = 0
}

REGION_ALIAS("VECTORS_FLASH", vectors);
REGION_ALIAS("VECTORS_FLASH_LMA", vectors);
REGION_ALIAS("XTORS_FLASH", vectors);
REGION_ALIAS("XTORS_FLASH_LMA", vectors);
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

/* OpenBLT shared params - fixed address, same as m749_openblt.ld. */
SECTIONS {
    .shared (NOLOAD) : { KEEP(*(.shared)) } > shared
}

/* Frequently executed control and interrupt code belongs in the AT32
   zero-wait window (see the header comment). Match function sections by
   name so this also works with LTO partitions and -ffunction-sections.
   The ZW bound is 0x08040000 (end of the 256 KiB ZW region); flash0
   capacity (up to 0x0805F000) is the other bound and is enforced by the
   region itself. Sections not matched here land in TEXT (NZW).

   The rodata classes moved to flash1 are cold by construction: string
   literal pools (plain .rodata.str* and the per-function .rodata.*.str1.x
   pools LTO emits) are only fetched by formatting/error paths, and the
   wideband firmware blob is only read when flashing the controller. This
   frees flash0 for the hot code; the linker errors when either region
   overflows, so the split stays honest.

   The section is padded (zero fill) to the next 4 KiB sector boundary.
   The OpenBLT app-update path erases whole 4 KiB sectors per XCP
   PROGRAM_CLEAR and the flasher erases each srec segment right before
   programming the next one: if two adjacent segments shared a sector (a
   sub-4-KiB gap, like the one this section would otherwise leave before
   .text), the later erase would wipe the already-programmed tail of the
   earlier segment (verify mismatch at the wideband blob tail,
   2026-10-10). Sector-aligning every segment boundary keeps the erases
   disjoint for ANY flasher version. */
SECTIONS {
    .m749_fast_text : ALIGN(4)
    {
        __m749_fast_text_start__ = .;
        *(.text.*Trigger*)
        *(.text.*trigger*)
        *(.text.*Shaft*)
        *(.text.*shaft*)
        *(.text.*EventQueue*)
        *(.text.*SingleTimer*)
        *(.text.*Pwm*)
        *(.text.*pwm*)
        *(.text.*Engine*)
        *(.text.*engine*)
        *(.text.*Injection*)
        *(.text.*injection*)
        *(.text.*Injector*)
        *(.text.*injector*)
        *(.text.*Fuel*)
        *(.text.*fuel*)
        *(.text.*Ignition*)
        *(.text.*ignition*)
        *(.text.*Spark*)
        *(.text.*spark*)
        *(.text.*Dwell*)
        *(.text.*dwell*)
        *(.text.*Advance*)
        *(.text.*advance*)
        *(.text.*Timing*)
        *(.text.*timing*)
        *(.text.*Map*)
        *(.text.*map*)
        *(.text.*Sensor*)
        *(.text.*sensor*)
        *(.text.*Adc*)
        *(.text.*adc*)
        *(.text.*Exti*)
        *(.text.*exti*)
        *(.text.*Scheduler*)
        *(.text.*scheduler*)
        *(.text.*Timer*)
        *(.text.*timer*)
        *(.text.*Limp*)
        *(.text.*limp*)
        *(.text.*WallFuel*)
        *(.text.*wallFuel*)
        *(.text.*Airmass*)
        *(.text.*airmass*)
        *(.text.*VeModel*)
        *(.text.*Gpio*)
        *(.text.*gpio*)
        *(.text.*writePad*)
        *(.text.*Brain*)
        *(.text.*Sniffer*)
        *(.text.*OutputPin*)
        *(.text.*action*)
        *(.text.*assertPin*)
        *(.text.*Wave*)
        *(.text.*wave*)
        *(.text.*Knock*)
        *(.text.*knock*)
        *(.text.*Idle*)
        *(.text.*idle*)
        *(.text.*Etb*)
        *(.text.*etb*)
        *(.text.*Throttle*)
        *(.text.*throttle*)
        *(.text.*Vvt*)
        *(.text.*vvt*)
        *(.text.*Fan*)
        *(.text.*fan*)
        *(.text.*Boost*)
        *(.text.*boost*)
        *(.text.*Alternator*)
        *(.text.*alternator*)
        *(.text.ch*)
        *(.text._port*)
        *(.text.Vector*)
        *(.text.*perfEvent*)
        *(.text.*interpol*)
        *(.text.*getTime*)
        *(.text.*Rpm*)
        *(.text.*rpm*)
        *(.text.*Angle*)
        *(.text.*angle*)
        *(.text.*Dma*)
        *(.text.*dma*)
        *(.text.*pal*)
        *(.text.*ScopePerf*)
        *(.text.__aeabi*)
        *(.text.__hard_fp*)
        *(.text.__gnu_*)
        *(.text.memset)
        *(.text.memcpy)
        *(.text.memmove)
        __m749_fast_text_end__ = .;
    } > flash0

    .m749_rodata_nzw : {
        *(.rodata.str*)
        *(.rodata.*.str1.1)
        *(.rodata.*.str1.4)
        *(.rodata.*wideband*)
        . = ALIGN(0x1000);
    } > flash1
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
ASSERT(ADDR(.shared) == 0x20000004 && SIZEOF(.shared) == 16, "M74.9 shared params must be fixed at 0x20000004")
ASSERT(SIZEOF(.vectors) + SIZEOF(.xtors) <= 0x1000, "M74.9 vectors/ctors exceed the 4 KiB vectors page")
ASSERT(__m749_fast_text_end__ <= 0x08040000, "M74.9 hot code exceeds the zero-wait flash window")
ASSERT((__heap_end__ <= __data_base__) || (__heap_base__ >= __bss_end__),
    "M74.9 heap overlaps data/bss - log-buffer clobber")
