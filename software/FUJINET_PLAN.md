# FujiNet link plan — ESP32-S3 (UM FeatherS3) on the DMA board

Goal: serve `.img` files from TNFS/WiFi as SASI targets, reusing the SPI link
proven in `../../Victor9000-Development-Private/v9k_flop` (25 MHz SPI master →
FeatherS3 SPI slave, FujiBus RS232 protocol SLIP-framed over a 4-byte
transport header). Bench first on rev 1 (PGA2350 board); land on rev 2's
topology where possible.

## 1. Pin plan (bench, rev 1)

v9k_flop's pins (GP32–36, SPI0) are all taken here (IR_4, UART0 RX, IR_5,
IO_M), and every SPI0-capable GPIO is consumed by the 8088 bus — so the only
hardware-SPI option is **sharing SPI1 with the SD card**, which is exactly
what rev 2 does (`hardware/rev2/DESIGN.md:18-20`). All six wires come off the
**J2 "SPI Breakout" header** — no soldering.

| Signal | RP2350B | rev1 access | FeatherS3 | rev 2 |
|--------|---------|-------------|-----------|-------|
| SCK  | GP42 (SPI1 SCK, shared w/ SD) | J2 | IO36 (silk SCK) | same |
| MOSI | GP43 (SPI1 TX, shared) | J2 | IO35 (silk MO) | same |
| MISO | GP44 (SPI1 RX, shared) | J2 | IO37 (silk MI) | same |
| ESP_CS | **GP45** (plain GPIO out, idle high) | J2 | IO10 | **matches rev 2** |
| DRDY | **GP46** (GPIO in, pull-down) | J2 | IO11 | bench-only (rev 2: MCP23S17 GPA4) |
| GND  | — | J2/board GND | GND | — |

- FeatherS3 self-powers from its own USB-C; share ground only (v9k_flop wiring).
- SPI mode 0, MSB first, both devices. `spi_set_baudrate` before each owner
  switch: SD 25 MHz, ESP ≤25 MHz (bring-up at 12.5 MHz or below).
- CS is a plain GPIO, not PL022 hardware CS (PL022 drops CS between FIFO
  words; the ESP slave ends its transaction on any CS rise) —
  `v9k_flop/firmware/fuji_blkdev.c:26-28`.
- ESP slave already pulls CS↑ / SCK,MOSI↓ so a resetting master can't clock
  garbage; RP holds DRDY pull-down while the ESP boots.
- GP46 caution: the test executables (`benchmark_irq`, `test_clock_check`,
  `test_pio_simple`) drive GP46 as UART0 TX. Harmless to the main firmware
  (DRDY is an input) but don't run those builds expecting UART output there
  while the ESP is wired, and don't leave a bench UART adapter on GP46.
- Rev 2 keeps GP46 = UART0 TX (F11 AUX), with DRDY on the expander. Read DRDY
  through one accessor function so rev 2 swaps in an expander read.

## 2. ESP32-S3 side — zero changes

Reuse v9k_flop's build unchanged: fork of fujinet-firmware at base commit
`5ee28f603` + `v9k_flop/docs/fujinet-firmware-feathers3.patch`, tree at
`/root/fujinet-firmware`, target `fujinet-rs232-feathers3`. Its pinmap
(IO36/35/37 SPI, IO10 CS, IO11 DRDY) is already what we wire above. If this is
the same physical FeatherS3 from the flop bench it is already flashed and has
WiFi + TNFS host config (`tnfsd` at `/root/tnfsd`, UDP 16384).

SASI targets map onto FujiBus disk device IDs `0x31 + target` (already how
both the stale pico_v9k stub and the RS232 protocol address disks). Phase 1
uses target 0 / device 0x31 only.

## 3. Firmware work (pico_v9k)

### Phase A — port the proven transport
1. Copy `v9k_flop/src/fuji_proto.{c,h}` **verbatim** (portable SLIP/FujiBus
   core, no SDK deps, host-testable).
2. New `pico_fujinet/fuji_blkdev.{c,h}` adapted from
   `v9k_flop/firmware/fuji_blkdev.c`:
   - `spi0` → `spi1`; pins per table; keep the 4-byte transport header
     (magic 0xFB/0xFA, len, tag), DRDY gating, timeout/retry ladder verbatim.
   - Static SRAM buffers instead of the PSRAM carve-out (~10 KB: 2 KB request
     + 16×512 B + header reply). No PSRAM exists on this board.
   - `FUJI_SPI_HZ` compile-overridable; default 12.5 MHz until the shared-bus
     ladder passes, then try 25 MHz.
3. Delete the stale `pico_fujinet/spi.c/.h` (its pin map — MISO=40/CS=41/
   SCK=42/MOSI=43/HANDSHAKE=44 — collides with DEN and the SD card; protocol
   is obsolete) and remove the direct `fujinet_read_sector/write_sector`
   bypasses in `sasi.c:1093-1123` and `sasi.c:847-856`.

### Phase B — storage integration
4. Implement a real `storage_ops_t` backend and register
   `STORAGE_BACKEND_FUJINET` (the current stub never registered one). All I/O
   stays on core 1 exactly like SD; reads may block — DOS polls the status
   register, there is no floppy-style hard deadline. Start fully synchronous:
   per-sector `PUT` writes, `READ_MULTI` (≤16 sectors) batching for
   multi-sector SASI READs.
5. SPI1 arbitration: single-owner guard (pattern of `fatfs_guard.c`); switch
   owners only between complete transactions, never mid-SD-burst; set the
   owner's baud on acquire. Console-initiated FujiNet ops (core 0) defer to
   core 1 rather than touching SPI1 directly.
6. Mount UX: SD stays primary. Add diag-console commands mirroring v9k_flop
   (`fuji status`, `ls`, `mount <target> <file>`, WiFi setup), and keep the
   existing SD-init-failure fallback path, now through the vtable.

### Phase C — bench validation
0. With the ESP wired but idle, re-run the SD test suite — the J2 stubs hang
   off the SD's 25 MHz bus; this is the biggest new electrical risk.
1. Bring-up ladder (per v9k_flop): DRDY seen high → hello/status frame at
   6.25/12.5 MHz → raw sector read, CRC against a known image.
2. Mount a TNFS image as SASI target 1 alongside the SD boot drive → DOS sees
   it as a second drive.
3. `dma_vfy2` then `hd_stress` against the FujiNet-backed target; interleaved
   COPY/DEL churn (the v9k_flop data-loss regression case).
4. Measure transfer speeds; append to `bench_logs/TEST_REPORT.md`.

## 4. Risks / lessons carried over from v9k_flop

- **Shared-bus signal integrity** — new vs v9k_flop (which had a dedicated
  SPI0). Short leads, common ground at one point, lower ESP clock first,
  re-verify SD.
- **SD MISO release**: SD cards need dummy clocks after CS deassert to
  tri-state DO; verify the FatFS driver does this before the ESP gets the bus.
- Opening the ESP's USB-Serial-JTAG port can reset it even with DTR/RTS
  deasserted — treat any port-open as a possible ESP reboot.
- ESP boot MOUNT_ALL races WiFi/TNFS — the RP re-sends `FUJICMD_MOUNT_ALL`
  and wins.
- Never stall >200 ms mid-frame; always lead frames with 0xC0.
- Radio: `esp_wifi_set_max_tx_power(34)` already in the patch (full power =
  PA garbage on this board).
- No PSRAM ⇒ no write-back cache: synchronous writes will be slower than
  v9k_flop's deferred ring. If DOS write patterns time out on the bench,
  port `fuji_wq` (SPSC ring, SRAM-sized) as the escalation path.

## 5. Rev 2 alignment summary

Matches rev 2: shared SPI1 bus (GP42/43/44), ESP_CS = GP45, SPI-only link,
FujiBus protocol. Deviates: DRDY on GP46 (rev 2 routes it via MCP23S17 GPA4 —
isolated behind one accessor), and rev 2's onboard WROOM-1U-**N16R8** cannot
use IO35–37 (octal PSRAM pins), so its ESP pinmap moves to IO10=CS, IO11=MOSI,
IO12=SCK, IO13=MISO, IO9=DRDY per `DESIGN.md:80-82` — keep the ESP pinmap
header parameterized so both boards build from one tree.
