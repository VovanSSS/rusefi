# m74_9 execution speed: memory layout + flash accelerator (theoretical, 2026-11-04)

Five-point summary of what governs code execution speed on the AT32F435/437.
Grounded in the official CMSIS (`at32f435_437cx.h`), the ChibiOS-Contrib AT32
port (`hal_lld.h`/`hal_lld.c`), and the datasheet (`at32f.txt`).

## Register map (authoritative, CMSIS)

| Register | Bit | Name | Meaning |
|---|---|---|---|
| `FLASH->PSR` | 12 | `FLASH_PSR_NZW_BST` | non-zero-wait area boost (write) |
| `FLASH->PSR` | 13 | `FLASH_PSR_NZW_BST_STS` | boost status (read-only) |
| `FLASH->CONTR` | 31 | `FLASH_CONTR_FCONTR_EN` | continue-read (prefetch) enable |
| `FLASH->DIVR` | [1:0] | `FLASH_DIVR_FDIV` | flash divider (/2=0, /3=1, /4=2) |

There is **no separate prefetch bit**: the datasheet's "Prefetch in ON" is the
continue-read = `CONTR[31]`, already enabled. The only flash-accelerator
registers are `PSR`, `CONTR`, `DIVR`.

## Five points

| # | Point | Detail | Source |
|---|---|---|---|
| 1 | **Three memory-speed tiers** | ZW flash = 0 WS up to 288 MHz; NZW flash = non-zero WS; SRAM = 0 WS. m74_9 (1024-KB chip, EOPB0=0x05FA) = **256 KB ZW** (0x08000000–0x0803FFFF) + **768 KB NZW** (0x08040000+) + **384 KB SRAM**. | datasheet §4, §2.2.3 |
| 2 | **NZW_BST is a frequency trade-off, not a broken feature** | Enabling NZW_BST caps HCLK: 192 MHz @1.3 V / 160 @1.2 V / 136 @1.1 V / 108 @1.0 V; disabling it allows 288 / 240 / 192 / 144 MHz. m74_9 runs 288 MHz → NZW_BST **must** stay off. | `hal_lld.h` HCLK_MAX table; datasheet AHB freq table |
| 3 | **RAM code and ZW placement are the same speed** | `.fast_text` RAM copy (22.8 KB) and `.m749_fast_text` ZW placement (130 KB) both 0 WS → equal execution speed. RAM copy is the early workaround for code that sat in NZW; ZW placement is the clean route. Standard rusEFI needs no RAM copy (STM32 ART ≈ RAM). | `efifeatures.h`, `m749.ld` |
| 4 | **The "NZW_BST hang" is out-of-spec 288 MHz, not a hardware bug** | The 2026-08-21 "hangs the flash read path" observation (commit `004e88a148`) is NZW_BST=1 at HCLK=288 MHz, which exceeds the 192 MHz boost-mode limit. The correct enable sequence programs DIVR/NZW_BST while HCLK is still HICK, before the PLL switch (see `at32_clock_init`). | `hal_lld.c` comment + sequence; commit `004e88a148` |
| 5 | **No further flash lever is left on the table** | Only PSR/CONTR/DIVR exist — no separate prefetch bit to enable. continue-read is on, DIVR=/3 is set, ZW is used. The only knob left is the ZW/NZW/SRAM reconfig (EOPB0), bounded by the board's SRAM need. FPU/DSP/hardware divide are already enabled via `-mcpu=cortex-m4` + `USE_FPU=hard`. | CMSIS FLASH_TypeDef; `stm32f4/hw_ports.mk` |

## Bottom line

NZW flash is slow on m74_9 **by design choice**, not by bug: the board runs the
core at the maximum 288 MHz, which forces `NZW_BST` off (the boost mode tops
out at 192 MHz). Accelerating the 768 KB NZW region would require dropping the
core to ≤192 MHz — a ~33% clock reduction that is only worth it for
flash-bound code. For the hot trigger/spark/injection path the correct answer
is already in place: 0-WS ZW flash + 0-WS SRAM, no RAM-vs-flash penalty.
