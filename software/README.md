# Victor 9000 DMA Replacement Firmware

This project turns a Pimoroni PGA2350 (RP2350) board into a modern replacement for the Victor 9000/Sirius 1 DMA expansion card. The firmware emulates the original DMA registers, arbitrates the 8088 bus, and serves disk data from `.img` files on an SD card so the vintage system can boot and run without the original hard-disk controller. (A FujiNet-over-SPI backend exists as a fallback if SD init fails.)

## Who This Project Serves
- Victor 9000 and Sirius 1 owners who need a functional hard-disk subsystem
- Developers interested in RP2350 PIO/DMA techniques for vintage bus emulation

> **Status:** Boots MS-DOS 3.1 from SD-backed images and passes a phased hardware
> validation suite (boot consistency, byte-verified read/write round-trips, 64K-boundary
> DMA, stress, mid-transfer-reset recovery) — see `bench_logs/TEST_REPORT.md`.

## Hardware You Will Need
- Pimoroni PGA2350 (RP2350) board or equivalent RP2350 hardware running at 200 MHz
- Victor 9000/Sirius 1 host with access to the DMA expansion slot edge connector
- Micro-SD card (hardware SPI1) holding one or more Victor disk images (`*.img`)
- Wiring harness from the RP2350 GPIO to the Victor bus — `pico_victor/dma.h` is the
  authoritative pin map
- USB-serial (or debug probe UART) on GPIO0/GPIO33 for logging, and a CMSIS-DAP probe
  or BOOTSEL USB for flashing

The RP2350 is 5 V tolerant on the GPIOs used here, so no level shifting is required. Preserve the original DMA card pinout (address/data bus, ALE, RD/, WR/, HOLD/, HLDA, READY, XACK, EXTIO, …) exactly; incorrect mappings will prevent the 8088 from releasing the bus.

## Firmware Capabilities
- Mirrors the EF300h DMA register map expected by the Victor BIOS and MS-DOS 3.1
  (status readable at both `0xEF320` and `0xEF330`; some BIOS paths probe both)
- One PIO state machine snoops/services the register window (`board_registers.pio`);
  a second performs full 8088 bus-master DMA cycles (`dma_master.pio`)
- Emulates SASI control/status handshakes on behalf of the legacy Xebec 1410 controller
- Auto-mounts every `*.img` on the SD card to its own SASI target (up to 8), sorted
  case-insensitively so target 0 — the boot drive — is deterministic
- Robust storage path: FatFS with retry + remount recovery, write-sync with
  CHECK CONDITION reporting, DMA timeout/abort recovery (survives a host reset
  mid-transfer)
- Non-blocking diagnostics on **UART0, TX=GPIO0 / RX=GPIO33, 230400 baud**, with an
  interactive console: `h` help, `t` SASI trace, `p` PIO/pin/timing dump, `i` IRQ
  trace, `c` HardFault info, `f`/`a` log flush, `r` DMA CRC trace (VERIFY_DMA_WRITES builds)

## Building the Firmware
Prerequisites: CMake, arm-none-eabi toolchain, the Pico SDK, and
[no-OS-FatFS-SD-SDIO-SPI-RPi-Pico](https://github.com/carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico)
cloned at `$HOME/projects/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico`.

```bash
cd software
mkdir -p build && cd build
cmake ..                          # -DVERIFY_DMA_WRITES=ON adds DMA write read-back + CRC trace
make dma_board -j
```

Flash `build/dma_board.uf2` with picotool in BOOTSEL mode, or over SWD:

```bash
openocd -f interface/cmsis-dap.cfg -c "adapter speed 5000" -f target/rp2350.cfg \
        -c init -c "program build/dma_board.elf verify" -c "reset run" -c shutdown
```

## Installing and Bring-up
1. **Flash the PGA2350** with the firmware.
2. **Cable the Victor bus** to the RP2350 GPIOs per `pico_victor/dma.h`. Short runs;
   the 8 MHz multiplexed bus is unforgiving.
3. **Prepare the SD card**: copy one bootable Victor hard-disk image (`.img`) to the
   root. Additional images become additional SASI targets (alphabetical order).
4. **Power on.** The RP2350 mounts the SD, auto-mounts images, then serves the DMA
   register window at EF300h. Give it ~5 s after reset before booting the Victor.
5. **Monitor the UART** (230400) — you should see the init banner, SD scan,
   per-target mount lines, and `Core1: Storage ready`.

If the Victor fails to start DMA transfers, double-check HOLD/HLDA wiring and confirm READY is initialized with `pio_gpio_init()` (the PIO stalls forever otherwise).

## Operating Notes
- The firmware relies on BIOS/DOS status polling; host IRQ signaling is not implemented
  (validated unnecessary for boot and steady-state operation).
- 512-byte sectors are assumed throughout.
- Timing is tight — avoid adding blocking logging or USB work while the Victor bus is
  active. Use the debug queue / `sasi_printf` patterns already in the code.
- SD write latency can spike >100 ms during FAT updates (normal SD behavior); the
  worst observed full command time is ~0.4 s vs the BIOS's 5 s timeout.

## Testing
- `HARDWARE_TEST_PLAN.md` — phased bench validation plan (build/flash → boot
  consistency → read path → write path → stress/fault-injection)
- `bench_logs/TEST_REPORT.md` — latest full-pass session report and known findings
- `test/dos_dma_test/` — DOS-side tools (OpenWatcom): `dma_vfy2` (bench-safe CRC
  verify + 64K-boundary proof), `hd_stress` (INT 21h full-stack stress),
  `sasi_perf_test` (MAME boot-log register replay), and others
- `tools/` — host-side trace/timing analyzers

## Learning More
- `CLAUDE.md` — developer notes: architecture, PIO/GPIO rules, doc pointers
- `Technical Overview of the Project.md` — architectural background on the Victor DMA subsystem
- `notes/Manuals/` — scanned Victor 9000 technical manuals

Community feedback, trace captures, and bug reports are welcome. Please document wiring, SD card model, disk image provenance, and Victor ROM revisions when sharing results.
