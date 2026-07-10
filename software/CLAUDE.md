# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is a Raspberry Pi Pico RP2350 project that replaces the Victor 9000 (1982, 8088-based) DMA hard-disk expansion card. In the legacy hardware that card spoke to a SASI Xebec 1410 controller fronting MFM drives. The firmware emulates the card's memory-mapped register block, arbitrates the 8088 bus (HOLD/HLDA), performs real DMA against 8088 memory, and services READ/WRITE(6) SASI commands from `.img` files on an SD card — so the vintage machine boots MS-DOS 3.1 from modern storage.

## Build Commands

```bash
cd software
mkdir -p build && cd build
cmake ..            # add -DVERIFY_DMA_WRITES=ON for DMA write read-back + CRC trace ('r' console cmd)
make dma_board -j
```

Requires the Pico SDK and the FatFS library at `$HOME/projects/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico`
(clone from carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico if missing). Clean build: `rm -rf build/*` first.

Flash over SWD (CMSIS-DAP debug probe): `openocd -f interface/cmsis-dap.cfg -c "adapter speed 5000" -f target/rp2350.cfg -c init -c "program build/dma_board.elf verify" -c "reset run" -c shutdown`, or `picotool load build/dma_board.uf2` in BOOTSEL mode.

## Architecture

### Source layout (current — trust this, not old notes)

- `dma_board.c` — main firmware: init, Core 0 loop (UART console, stuck detector, stack canary, HardFault capture)
- `pico_victor/board_registers.pio` — PIO0 SM0: snoops the 8088 bus for the EF300h register window, services register reads/writes
- `pico_victor/dma_master.pio` — PIO1 SM0: full 8088 DMA bus-master cycles (HOLD/HLDA, address/data, RD//WR/)
- `pico_victor/dma.c/.h` — register model, pin map (single source of truth for GPIO assignments), DMA transfer helpers
- `pico_victor/register_cache.c`, `register_irq_handlers.c`, `reg_queue_processor.c`, `fifo_helpers.c`, `dma_irq_handler.c` — Core 1 cached register ISR + deferred-work queue
- `sasi.c` — SASI command state machine (READ/WRITE(6), sense, mode select…), per-op timing, trace ring
- `pico_storage/` — storage abstraction: `sd_storage.c` (SD over **hardware SPI1**, FatFS, retry/remount recovery), `fatfs_guard.c` (FatFS mutex)
- `pico_fujinet/spi.c` — FujiNet SPI backend (fallback only, used if SD init fails)
- `logging.c`, `pico_victor/debug_queue.c` — non-blocking logging for timing-critical paths

### ⚠️ PIO files: NEVER modify `dma_master.pio` or `board_registers.pio` directly

They are cycle-accurate at the nanosecond level and have consumed weeks of debugging each time they were touched. Recommend courses of action only; changes require explicit discussion first, then re-validation on hardware (`HARDWARE_TEST_PLAN.md`).

### Key runtime facts

- **Dual-core**: Core 0 = main loop + diagnostics console; Core 1 = register IRQs, SASI processing, and ALL storage I/O (keeps storage and command handling on one core)
- **Registers** at `0xEF300`: `0x00` control (W), `0x10` data (R/W), `0x20` status (R; `0x30` alias probed by some BIOS paths), `0x80/0xA0/0xC0` DMA address low/mid/high(4 bits). MAME-compatible low-bit masking.
- **Storage**: every `*.img` in the SD root is auto-mounted to its own SASI target (up to 8), **sorted case-insensitively** so target 0 (the boot drive) is deterministic. Single image ⇒ target 0. Default fallback image name: `victor.img`.
- **Diag UART**: `uart0`, **TX GPIO0 / RX GPIO33, 230400 baud**, interactive console — `h` help, `t` SASI trace, `p` PIO/pin/timing dump (Op timing lines print ONLY here or in the auto-stuck dump), `i` IRQ trace, `c` HardFault info, `f`/`a` log flush, `r` DMA CRC trace (VERIFY_DMA_WRITES builds).
- **Bench gotcha**: the test executables (`dma_board_tests`, `benchmark_irq`, `test_clock_check`) print on **GPIO46**, not GPIO0 — silent on a probe wired for the main firmware.
- Host IRQ signaling is not implemented; DOS/BIOS poll the status register (validated working).
- 200 MHz system clock; RP2350 GPIOs are 5V-tolerant → direct 8088 bus connection, no level shifters.

### PIO GPIO Initialization Requirements (RP2350 — still current, hard-won)

- Output pins (PIO-driven): use `pio_gpio_init()` so the PIO owns direction.
- Input pins the PIO reads with `wait`/`in` (READY, HLDA, clocks, IR): **also require `pio_gpio_init()`** on RP2350 (despite SDK docs), then `gpio_set_dir(pin, GPIO_IN)`. Skipping this makes `wait` hang forever even though the ARM core reads the pin fine.
- Never call `gpio_init()` on a pin the PIO must read — it reroutes the pin to SIO.
- RP2350 silicon quirk: side-set `pindirs` does nothing until the SM executes one explicit `pindirs` instruction — after `pio_sm_init()`, run `pio_sm_exec(pio, sm, pio_encode_set(pio_pindirs, 0))` to unlock it.

## Testing

- **Host-side analysis tools**: `tools/` (bus-timing/trace analyzers, MAME log parser).
- **DOS-side test programs**: `test/dos_dma_test/` — build with OpenWatcom (`wcc -bt=dos -ms -0 -os -zq -za99 -fo=X.obj X.c` then `wlink format dos LIBPATH $WATCOM/lib286/dos LIBPATH $WATCOM/lib286 name X.exe file X.obj`). Highlights: `dma_vfy2.c` (bench-safe: self-consistency reads, save/restore writes, 64K-boundary straddle proof), `hd_stress.c` (full INT 21h stack stress), `sasi_perf_test.c` (replays MAME boot-log register ops). ⚠️ `dma_verify.c`'s write test assumes LBAs 200–204 are free — stale; prefer `dma_vfy2`.
- **Hardware validation**: `HARDWARE_TEST_PLAN.md` (phased bench plan) and `bench_logs/TEST_REPORT.md` (2026-07-09 session: all phases pass at 040c8d5+fixes). Bench rig manual: `../../Victor9000-Development-Private/BENCH.md`.

## Vintage Hardware Documentation

- **SASI and DMA Card Hardware**: `notes/Manuals/Victor 9000 Sirius 1 Hard Disk Subsystem.pdf` (SASI handshake §3.1.4.2, DMA state machine §2.1.3)
- **Hardware Reference**: `notes/Manuals/Victor 9000 Hardware Reference Rev 0 - 10.5.1983.pdf`; memory map, schematics, programmer's toolkits also under `notes/Manuals/`
- **Boot BIOS source**: `notes/Boot BIOS ASM source 3.6/` (see `BT1INFO.DOC`)
- **MS-DOS 3.1 listings**: `notes/MS-DOS 3.1 Listings/` — esp. `SASIDMA.LST` (register layout ground truth)
- **MAME reference implementation**: `notes/victor9k_hdc.cpp`; `notes/mame boot example.log` shows a full boot's register interaction sequence

## History

Dated debugging narratives that used to live here (GPIO0 UART conflicts, EXTIO isolation, bus-release races, buffer removal, the `dma_read_write.pio` rewrite—since replaced by `dma_master.pio`) are preserved in git history of this file. The current firmware boots MS-DOS 3.1 reliably; see `bench_logs/TEST_REPORT.md` for the latest validated state.
