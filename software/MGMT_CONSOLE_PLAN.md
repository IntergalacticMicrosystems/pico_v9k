# Management console + host control channel plan

Port v9k_flop's proven console/control stack to the DMA card: micro_tui
REPL + full-screen menu with a unified `sd:` / `fuji:` image namespace, a
register-based host control channel so the Victor 9000 detects and manages
the card from the boot ROM (F4) and DOS, and the ROM_NASM40 F4 menu entry.

Reference implementations (read these, don't reinvent):
- v9k_flop console: `../../Victor9000-Development-Private/v9k_flop/firmware/tui.c`,
  `tui_cmds.c`, `tui.h` (tui_ops_t seam), `via_con.c`
- Channel core: `v9k_flop/src/via_chan.{c,h}` (portable, host-tested SPSC)
- Victor clients: `viclibc2/viaterm/viaterm.c` (DOS),
  `viclibc2/viaterm_asm/viaterm.asm` + `gen_blob.py` (ROM-download VTASM),
  `ROM_NASM40/rom_menu.asm` (F2 flow to mirror for F4)
- micro_tui toolkit: `/root/sync/micro_tui` (external dep, same as v9k_flop)

## Console architecture (Stage 1)

- **USB CDC = interactive console** (REPL + full-screen menu), raw TinyUSB
  like v9k_flop (`pico_enable_stdio_usb` stays 0). **UART0 = diag/log
  stream at 115200** (was 230400; update `BAUD_RATE` in dma.h and all bench
  docs). Rev1 routes USB_DP/DM to the USB-C, so plug the bench host into
  the card's USB-C for the console.
- Port `tui.c`/`tui_cmds.c` with the same two-mode design (REPL default,
  `menu` enters full-screen; non-blocking `tui_poll()` from core 0 main
  loop; 100 ms bounded CDC writes; drop-when-no-DTR).
- **No PSRAM here**: the 2×80×25 cell buffers (~16 KB) go in static SRAM.
- **Frozen REPL contract** from v9k_flop: every reply ends `OK` or
  `ERR <reason>` — keep it exactly (bench scripts parse it).
- Command set v1: `ls` (SD + FujiNet, `sd:`/`fuji:` prefixed), `status`
  (targets 0–7: image, backend, capacity), `mount <t> <[sd:|fuji:]file>`
  (bare name = sd:), `eject <t>`, `wifi [...]`, `scan`, `host [...]` (TNFS
  host if the ESP supports it), `diag`, `menu`, `help`. v9k_flop's
  `create`/`copy` are deferred (HD images are large); `peek <t> <lba>` is
  cheap and useful — include it. SASI single-char debug console and the
  Phase B `F` shell are REMOVED (git history preserves them); `diag` is
  the escape hatch.
- **Absent-media behavior** (v9k_flop semantics): no SD → `ls` lists only
  `fuji:`, sd mounts fail with `ERR`; no ESP → fuji ops answer
  `ERR no FujiNet`, `ls` silently lists SD only; both absent → console
  still runs.
- **Core discipline**: unlike v9k_flop (storage on core 0), ALL storage/SPI
  work here lives on core 1. Generalize the Phase B fuji_console mailbox
  into the console-ops mailbox (mount/eject/ls/wifi/stat/peek), serviced
  from `defer_worker_main`. TUI handlers submit and poll non-blocking
  (mtui redraws while waiting; show a busy row) or with bounded waits.
- SD image listing: expose a `sd_storage_list_images(emit, ctx)` (the scan
  logic already exists in sd_storage init).

## Host control channel (Stage 2)

Same wire protocol as v9k_flop's VIATERM channel, different registers:
three virtual registers in the DMA card's 0xEF300 window. The PIO snoops
the whole 0xEF3xx page and firmware decodes offsets
(`register_irq_handlers.c:is_valid_reg_offset`), so NO PIO changes.

- Offsets (after `dma_mask_offset`, 16-byte cells below 0x80; ROM/DOS SASI
  drivers use only 0x00/0x10/0x20/0x30/0x80/0xA0/0xC0):
  - **0x40 CMD** — write: push command byte; read: ~last-written (complement echo)
  - **0x50 STATUS** — read: `0xD0 | flags`; write: soft reset
  - **0x60 RESP** — read: current response byte; write: ack/advance
- Signature nibble **0xD0** (flop card uses 0xB0 — clients must be able to
  tell them apart). Flags identical: b3 OVERFLOW, b2 CMD_FULL,
  b1 RESP_READY, b0 RESP_PHASE. Idle CMD echo 0xFF. Download command 0x1C,
  blob frame `A5 5A len_lo len_hi <payload> sum16`. Menu-exit CSI
  `ESC[86q`. Keep all constants byte-identical otherwise so client code
  ports mechanically.
- Port `via_chan.c` → `pico_victor/mgmt_chan.c` nearly verbatim (it is
  transport-agnostic SPSC with explicit fences). Integration differs:
  - Bus side = the register ISR (core 1). CMD/STATUS/RESP writes are
    handled SYNCHRONOUSLY in the write path (O(1): complement+store,
    flag set, counter bump) — the defer queue would let a fast
    write-then-read race the echo. Reads are already served from
    `cached->values[]`; the channel's read shadows ARE those cache cells.
  - Console side = core 0 (`mgmt_con_poll()` in the main loop), the only
    writer of the STATUS/RESP cache cells (single-writer per offset,
    fences per via_chan).
  - Extend `is_valid_reg_offset` with 0x40/0x50/0x60.
- `mgmt_con.c` = port of `via_con.c`: binds the SAME command table over an
  mtui transport whose write feeds the response FIFO; echo discipline,
  reset handling, shared menu ownership (CDC vs channel), 0x1C blob serve.
- VTASM blob: assemble a variant of `viaterm.asm` with base segment
  0xEF30, offsets 0x40/0x50/0x60, signature 0xD0 (parameterize the source
  with equates rather than forking it if the v9k_flop repo allows; else a
  patched copy under this repo). `gen_blob.py` → `vtasm_dma_blob.h`.

## Victor-side clients (Stage 3)

- **ROM (ROM_NASM40)**: in `rom_menu.asm`, after the flop-card probe, probe
  the DMA channel the same way (0x55/0xAA complement echo on 0xEF340, then
  STATUS&0xF0==0xD0). If present: `scale4` + `draw2x` the F4 glyphs
  (0x0F,0x04) and the hard-disk icon pair `left_hd`/`right_hd` (0x1A/0x1B,
  already in the font at `bt1iconc_nasm.asm:458-491`) to the right of the
  F2/disc block; blink F4 in the same loop as F2. Add
  `KB_F4_MAKE equ 084h` to the `.wait` cmp/je chain → `hd_load`, a
  parameterized copy of `rom_load` (share `vc_getb` by passing the
  register trio; ~1 KB pad free at 0x1905). Timeout/failure paths must
  all fall through to normal boot, like rom_load.
  Build: `./build` (nasm + add-checksum.py); test in MAME first
  (build copies test.rom into mame-files), EPROM-emu only with user OK.
  Per AGENTS.md: update its docs with the change.
- **DOS tool**: build a DMA-card variant of `viclibc2/viaterm/viaterm.c`
  (same code, register base 0xE F30:0x40/0x50/0x60, signature 0xD0 —
  parameterize with #defines). Detects card, runs the same VT100 terminal
  against the management console.

## Order of work

1. Stage 1a: micro_tui + CDC console + command table + console-ops mailbox
   generalization + 115200 + remove old debug console. Build clean.
2. Stage 1b: bench check (user approval): console over USB-C, sd:/fuji:
   mount flows, absent-media behavior.
3. Stage 2: mgmt_chan + mgmt_con + ISR hooks + VTASM blob. Host-testable
   like v9k_flop's test_via.c (port the test). Bench: DOS-side probe via
   dma_vfy2-style tool or the DOS terminal.
   **DONE (2026-07-14)**: `pico_victor/mgmt_chan.{c,h}` (portable SPSC, sig
   0xD0, cells 0x40/0x50/0x60), ISR write-path hooks in
   `register_irq_handlers.c` (synchronous CMD echo / reset flag / ack bump;
   0x40/0x50/0x60 added to `is_valid_reg_offset`; cells bound to the register
   cache via `mgmt_chan_bind_cells` in `register_cache.c`),
   `console/mgmt_con.{c,h}` (second console binding + 0x1C blob serve, polled
   from `dma_board.c`), `victor_client/` VTASM (`viaterm_dma.asm` on segment
   0xEF30 + `build.sh` → `console/vtasm_dma_blob.h`), host test
   `test/host/test_mgmt.c` (37 checks pass at both FIFO sizes). Firmware builds
   clean. Bench validation (DOS/ROM probe) still pending — Stage 3.
4. Stage 3: ROM F4 (MAME test, then EPROM-emu with user), DOS tool.

## Risks / notes

- ISR write-path additions must stay O(1) and branch-cheap; measure with
  the existing IRQ benchmark if in doubt.
- The register cache cells for 0x40/0x50/0x60 become cross-core shared:
  core 0 writes (STATUS/RESP shadows), ISR writes (CMD echo), host reads
  via prefetch. Single-writer-per-cell + fences, exactly like via_chan.
- Logging vs console: logs stay on UART0, console on CDC — no interleaving
  (same as v9k_flop). While the full-screen menu is owned by the ROM/DOS
  channel, CDC `menu` gets a busy guard (port that logic).
- `fuji:` mounts persist to the ESP's fnconfig (Config.save) — unchanged
  Phase B behavior, worth surfacing in `status` someday.
- DOS 3.1 SASI driver ground truth for "offsets 0x40-0x7F unused":
  `notes/MS-DOS 3.1 Listings/SASIDMA.LST` — verify before finalizing
  offsets (Stage 2 gate).
