# m74_9 execution speed: memory layout + flash accelerator (theoretical, 2026-11-04)

Five-point summary of what governs code execution speed on the AT32F435/437,
and what is still on the table. Grounded in the datasheet (`at32f.txt`) and the
in-tree ChibiOS AT32 port; see the commit log / `hal_lld.c` for the flash setup.

| # | Point | Detail | Source |
|---|---|---|---|
| 1 | **Three memory-speed tiers** | ZW flash = 0 wait-state up to 288 MHz; NZW flash = non-zero wait; SRAM = 0 WS. m74_9 (1024-KB chip, EOPB0=0x05FA) = **256 KB ZW** (0x08000000–0x0803FFFF) + **768 KB NZW** (0x08040000+) + **384 KB SRAM**. | datasheet §4 memory map, §2.2.3 |
| 2 | **The flash accelerator exists** | `CONTR[31]` = continue-read (prefetch), enabled in `hal_lld.c`. `NZW_BST` (PSR[12]) = non-zero-wait boost — the datasheet documents it as a valid mode (separate `disabled`/`enabled` rows in the AHB frequency table; EMC tested at `NZW_BST=1`). | datasheet §5.3.10, `hal_lld.c` |
| 3 | **RAM code and ZW placement are the same speed** | `.fast_text` RAM copy (22.8 KB) and `.m749_fast_text` ZW placement (130 KB) both run 0 WS → equivalent execution speed. The RAM copy is the early workaround for code that sat in slow NZW; ZW placement is the clean route. Standard rusEFI needs no RAM copy because STM32 flash (ART/prefetch) ≈ RAM. | `efifeatures.h`, `m749.ld` |
| 4 | **NZW_BST "hang" is unverified, not a datasheet fact** | The "hangs the flash read path" claim is the author's own 2026-08-21 observation (commit `004e88a148`, 3–4 occurrences), never cross-checked against the RM. This is the single biggest unused lever: it would accelerate the entire 768 KB NZW region. | `hal_lld.c` comment, commit `004e88a148` |
| 5 | **Remaining levers** | (a) a separate prefetch bit possibly distinct from continue-read — confirm in the RM and the required "set before clock/bus prescale" order; (b) ZW/NZW/SRAM reconfig via EOPB0 (e.g. 512 KB ZW for the 1024-KB chip, at the cost of SRAM); (c) FPU / DSP / hardware divide / single-cycle MAC are already enabled by `-mcpu=cortex-m4` + `USE_FPU=hard` — nothing to gain there. | datasheet §2.1, §2.2.1; `stm32f4/hw_ports.mk` |
