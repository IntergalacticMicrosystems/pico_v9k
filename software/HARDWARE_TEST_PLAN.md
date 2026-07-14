# Hardware Test Plan — Victor 9000 DMA-card firmware (`dma_board`)

Bench validation of the RP2350 (PGA2350) firmware that emulates the Victor 9000
DMA / SASI hard-disk expansion card, streaming 512-byte sectors from `.img`
files on an SD card into 8088 memory. See `BENCH.md` (private repo) for the rig
rules this plan builds on, and `REVIEW_FINDINGS.md` for open code concerns to
watch for during testing.

Prepared 2026-07-09.

---

## 0. Rig facts verified for THIS bench (read first — some differ from the docs)

| Interface | Path / value | Notes |
|---|---|---|
| **Diag UART (logs)** | `/dev/serial/by-id/usb-Raspberry_Pi_Debugprobe_on_RP2350-Zero__CMSI_BA8296921F814583-if01` (currently `ttyACM1`) | **115200 8N1** (was 230400). Wired to PGA2350 **GPIO0 (TX) / GPIO33 (RX)** = firmware `UART0`. **Logs only now** — the interactive console moved to USB CDC (row below). Auto stuck/trace dumps still appear here. |
| **Management console** | the card's **USB-C** (rev1 routes USB_DP/DM there), a CDC-ACM node (`/dev/serial/by-id/usb-IGM_Victor_9000_DMA_Hard-Disk_Emulator_*-if00`) | **micro_tui** REPL (`v9k> `, replies end `OK`/`ERR`) + `menu`. Commands: `ls status mount eject peek wifi diag menu help`. Raw TinyUSB (stdio_usb off). |
| **SWD / flash** | same debug probe, **CMSIS-DAP** interface (if00, no tty node) | `openocd 0.12.0` + `picotool` both installed. |
| **Remote control (Victor kbd/screen/reset)** | `/dev/serial/by-id/usb-IGM_Victor_9000_Remote_Control_2600A48D424FB2F8-if00` (`ttyACM0`) | Drive via `…/Victor9000-Development-Private/victor9k_remote_control/host/.venv/bin/python host/remotectl.py --dev <by-id> <cmd>`. Always pin `--dev` (analyzer board shares VID/PID). |
| **Storage** | SD card over **SPI1**, `*.img` files (case-insensitive), up to **8 SASI targets** | Boot drive = **target 0**. FujiNet-over-SPI is only a *fallback* if SD init fails. |
| **Victor drives** | HD = **A:**, floppy = **B:** (empty), MS-DOS 3.1 | |

**Three gotchas that will bite immediately**

1. **UART0 baud is 115200** (was 230400) and it is now a **log stream only** on
   the debug-probe's **if01** node. Never `cat` the port (hangs the shell,
   BENCH §2). The interactive console moved to the card's **USB-C CDC** (micro_tui
   REPL/menu) — drive it there, not on the UART.
2. **ONE reader per port, ever.** The diag console is both a log source and an
   interactive command interface — do NOT run a background `dd` logger and then
   open a second connection to send `h`/`t`/`p`. Two readers = the classic
   garbled-but-legible split-stream failure (BENCH §2). Use a single pyserial
   session per test that both logs and injects command characters (§2 below).
3. **Multi-disk boot order is nondeterministic** (`REVIEW_FINDINGS.md` #4 — mount
   order follows `f_readdir`, not a sorted list). For any test where the *boot*
   drive matters, **put a single `.img` on the card** so target 0 is unambiguous.
   Only add a second image once boot is proven.

Environment hygiene before every session: `pkill -9 openocd`; `fuser -v $DIAG`
to confirm no stale reader is stealing UART bytes.

**On-device test targets are silent on this bench:** `dma_board_tests`,
`benchmark_irq`, `test_clock_check` all hardcode `UART_TX_PIN 46` — the probe is
wired to GPIO0/33, so flashing them yields no console output without rewiring.
This plan uses only the `dma_board` firmware plus DOS-side test programs.

---

## 1. Build & flash

Clean build (confirm the ELF is actually fresh — a stale ELF once got flashed,
BENCH §6):

```bash
cd /root/sync/pico_v9k/software
rm -rf build && mkdir build && cd build
cmake .. && make dma_board -j
ls -l dma_board.elf dma_board.uf2      # check mtime/size
```

- Default build: `VERIFY_DMA_WRITES=OFF`. **Enable it for the write-path tests**
  (§5) so the `'r'` console command can dump per-sector DMA-write CRCs:
  `cmake -DVERIFY_DMA_WRITES=ON ..`
- Keep the flashed ELF and the on-disk ELF in sync — symbol addresses move every
  rebuild (needed if you drop to openocd `mdw`/`nm`).

Flash over CMSIS-DAP (no in-repo `flash.sh`; kill stray openocd first):

```bash
pkill -9 openocd
openocd -f interface/cmsis-dap.cfg -c "adapter speed 5000" -f target/rp2350.cfg \
        -c "init" \
        -c "program build/dma_board.elf verify" \
        -c "reset run" -c "shutdown"
```

> BENCH §4 warning: `reset_config srst_only` **hangs on this rig** — rely on the
> rp2350 sysresetreq path (`reset run`). If the first post-flash reset stalls,
> just reset again. Alternative flash path: BOOTSEL + `picotool load -f`.

---

## 2. Reset & logging discipline (applies to EVERY test)

Per BENCH §4, **a clean test resets BOTH the emulator and the Victor**, in order:

1. Flash / sysresetreq the emulator (RP2350). **Wait ~5 s** — the SD must mount
   and Core 1 must set `storage_ready` before the bus is driven, or boot wedges.
2. `remotectl … reset` the Victor (hard CPU reset via optocoupler; boot ≈ 20-25 s).
3. Emulator state (motor_on, cur_track, mounted targets, stuck flag) contaminates
   across Victor-only resets — never reuse a dirty emulator between test cases.

**Diag capture — single-owner logger.** One pyserial process owns the port for
the whole test case: it appends everything to the log file AND is the only path
for sending console commands (`h t p i c r f a`). Never open a second reader.

```bash
DIAG=$(ls /dev/serial/by-id/usb-Raspberry_Pi_Debugprobe_on_RP2350-Zero__CMSI_*-if01)
PY=/root/vidcap/host/.venv/bin/python3   # has pyserial (BENCH §7)
```

```python
# diaglog.py <dev> <logfile> — sole port owner (UART0 is log-only now)
import serial, sys, threading
s = serial.Serial(sys.argv[1], 115200, timeout=0.2)
f = open(sys.argv[2], 'ab', buffering=0)
def rx():
    while True:
        d = s.read(4096)
        if d: f.write(d); sys.stdout.buffer.write(d); sys.stdout.buffer.flush()
threading.Thread(target=rx, daemon=True).start()
for line in sys.stdin:                    # 'p' + Enter sends the p command
    s.write(line.strip()[:1].encode())
```

Non-interactive runs: drive it with a heredoc/pipe, or just let it log. To grab
an `Op timing` / stall snapshot at a checkpoint, send `p` **through this same
process** (the stats print only via the `p` dump or the auto-stuck dump — they
are NOT periodic).

Do **not** name a UART capture the same basename as a `remotectl grab` (grab
writes an OCR `.txt` sidecar that would clobber it, BENCH §2).

---

## 3. Phase 0 — Test-asset prep (host side, before any hardware time)

Everything Phases C–F need must be **staged into the disk image on the host**
first — typing content in via remotectl is slow and hits the known
space-eating-`type` bug (BENCH §3). One-time setup:

1. **Build the DOS test suite** in `test/dos_dma_test/` with OpenWatcom
   (`source /opt/watcom/owsetenv.sh && export LIB=$WATCOM/lib286/dos`; see the
   dir's Makefile/build.sh). The key tools, purpose-built for this card:
   - `dma_verify` — reads 10 sectors via DMA and CRC8-checks them (Phase C)
   - `hd_stress` — multi-phase INT 21h full-stack stress test (Phase F)
   - `sasi_perf_test` — replays 250 register ops from the MAME boot log and
     checks phase-transition behavior against MAME (register-fidelity oracle)
   - `addrtest` / `dmatest` — targeted address-register / register poking
2. **Stage the boot image** (mount the `.img` loopback on the host or use
   mtools): copy the test EXEs, a set of known-content files with recorded
   CRCs, one file **> 128 KB** (stress/boundary work), and `.BAT` loops for
   Phase F.
3. Record CRC/size manifest of every staged file (host side) for later compare.
4. **Operator step:** write the image(s) to the SD card and insert into the DMA
   board. (Nothing on the rig can swap SD contents remotely.)

---

## 3a. Phase A — Emulator bring-up smoke (before touching the Victor bus)

Goal: prove the firmware boots, sees the SD card, and idles clean.

| # | Step | Pass criteria |
|---|---|---|
| A1 | Flash + reset emulator, capture diag UART | `=== DMA Board Initialization ===` banner appears |
| A2 | Watch Core 1 SD init | `Core1: Initializing SD card storage backend…`, then `Auto-mounting '<name>' on target N` for each `.img`, and eventually `storage_ready` behavior (register_cache.c:248) |
| A3 | Image enumeration matches card | count + names printed = what's actually on the SD |
| A4 | Console alive | send `h` → help menu prints (see Appendix A) |
| A5 | Idle health | **no** `HARDFAULT`, `STUCK DETECTED`, or `STACK OVERFLOW` lines over ≥60 s idle |

Fail here ⇒ stop; do not reset the Victor onto a broken emulator.

---

## 4. Phase B — Boot integration (headline test)

Single bootable HD image on the SD card (deterministic target 0).

- **B1 Cold clean boot:** reset emulator → wait 5 s → reset Victor → `remotectl grab`.
  **Pass = the MS-DOS `A:\>` prompt, dir-verified** (`type 'dir{Enter}'` returns a
  real listing). A banner/partial screen is **not** a pass (BENCH §6).
- **B2 Boot consistency:** repeat B1 **5×**, resetting BOTH each iteration.
  Pass = **5/5**. Any failure ⇒ stop, capture (`c`,`t`,`i`,`p`), diagnose, then
  the counter restarts — **4/5 is not "solved"**; past "solved" boot claims were
  superseded twice (BENCH §6).
- **B3 Boot trace:** during one boot, capture diag UART; after the prompt
  appears send `p` to dump `Op timing (max us)` / stall counters, and confirm
  no timeout / abort / stuck fired. `t` (SASI trace) and `i` (register IRQ
  trace) on demand.
- **B4 Register fidelity (optional but cheap):** run `sasi_perf_test` from A: —
  it replays the MAME boot log's register sequence and flags divergence from
  the reference implementation.

---

## 5. Phase C — Read-path functional

From the `A:\>` prompt (`remotectl type '…{Enter}'`; Alt = Ctrl on Victor kbd,
ESC does not cancel — use `{Ctrl+c}`; wait ~3 s after full-screen prompts):

- **C1** `DIR A:` root and a subdirectory — listing correct, no hang.
- **C2** `TYPE <stagedfile>` — content matches the Phase-0 manifest.
- **C3** `CHKDSK A:` — reports expected geometry/capacity, **0 errors**.
- **C4** `dma_verify` (staged in Phase 0) — 10 DMA-read sectors, all CRC8s
  match. This is the direct data-integrity oracle; run it before and after C5.
- **C5** Multi-file / recursive: `CHKDSK` after a `DIR /S`-style walk; watch diag
  UART for SD read-retry or remount events under sustained reads.
- **C6 64 K-boundary / 20-bit carry (must PROVE the crossing happened):** the
  carry lives in the firmware's per-sector advance
  (`sasi.c:643 — local_dma_addr = (local_dma_addr + 512) & 0xFFFFF`), and
  whether a transfer straddles 0x?FFFF depends on **where DOS's buffer sits in
  RAM, not on file size** — `TYPE`/`COPY` of a big file may never cross one.
  Procedure: run large multi-sector reads (big `COPY`, `hd_stress` read phase),
  then dump the DMA CRC trace (`r`, needs `VERIFY_DMA_WRITES=ON` build) or SASI
  trace (`t`) and grep for a multi-sector transfer whose start-addr + length
  straddles a 64 KB multiple. **No crossing observed = test not performed** —
  extend `addrtest`/`dmatest` to set the address registers just below a 64 K
  multiple and issue a multi-sector read directly. This is CLAUDE.md gap-list
  #7 and the prime suspect region.

---

## 6. Phase D — Write-path functional  *(build with `VERIFY_DMA_WRITES=ON`)*

Writes must be proven **persistent to the SD image**, not just "command returned
GOOD" (BENCH §6: a copy "completed" while a whole track was blank).

- **D1** `COPY CON A:\T1.TXT` (type a line, `{Ctrl+z}{Enter}`) → file created.
- **D2** `MD A:\TDIR`, `COPY` a file in, `DEL`, `RD` — directory ops succeed.
- **D3 Large write:** `COPY` the staged >128 KB file to a new name, `TYPE`/
  compare it back. Send `r` to dump the DMA-write CRC trace — no mismatches,
  and check (as in C6) whether any transfer actually straddled a 64 K multiple;
  if none did, the boundary case remains untested and needs the direct-register
  harness.
- **D4 Persistence across reset:** after writes, reset **both** and reboot from
  the same image; re-`TYPE` / `CHKDSK` — data survived (proves it reached SD, not
  just cache).
- **D5 (optional, risky) FORMAT:** only on a correctly-sized DS image — FORMAT on
  an SS image "succeeds" and creates phantom capacity (BENCH §6). Verify cursor
  moved before committing full-screen FORMAT (BENCH §3). Do this last.

Deeper oracle if a write bug is suspected: pull the SD card and byte-compare the
`.img` on the host against expected sectors.

---

## 7. Phase E — Multi-disk

- **E1** Two `.img` on the card → both mount (diag UART shows targets 0 and 1);
  DOS sees A: and the second drive.
- **E2 Boot-order determinism (`REVIEW_FINDINGS.md` #4):** with two images whose
  names sort differently from their `f_readdir` order (e.g. `boot.img` +
  `data.img`), confirm which lands on target 0 and whether the intended image
  boots. If it's nondeterministic across reflashes, that confirms the finding —
  apply the sort fix, then re-test.

---

## 8. Phase F — Stress / soak

- **F1** `hd_stress` (staged in Phase 0) — the purpose-built multi-phase INT 21h
  stress tool — plus a `.BAT` loop of large `COPY`/`TYPE`/`CHKDSK`, ~10–15 min
  total. Pass = no `STUCK DETECTED`, no HardFault, no stack-canary trip; `p`
  snapshots at start/middle/end show `Op timing` maxima bounded (no monotonic
  creep) and stall count near zero; SD retry/remount counters stay low.
- **F2** Reset the Victor **mid-transfer** repeatedly → DMA abort/timeout path
  recovers and the next boot is clean (tests the stuck-detector + abort logic and
  cross-reset state hygiene).

---

## 9. Phase G — Fault injection / robustness (optional, last)

- **G1** Force a SASI error path and confirm DOS INT 24h handler doesn't hang the
  machine (b65d936 added this handler).
- **G2 (operator-assisted — requires physical hands at the bench):** SD
  retry/remount recovery: transiently interrupt the SD (reseat) during idle and
  confirm the firmware recovers per `sd_storage.c` retry logic — do NOT do this
  mid-write. Skip if no operator is present.

---

## 10. Recording & pass/fail

For each case record: git SHA + build flags, SD image set, N/pass, diag-UART log
filename, screen grab. On any anomaly, immediately dump `c` (fault info), `t`,
`i`, `p` before resetting, and attach the log. Escalate timing-sensitive failures
to an analyzer-board bus trace (`tools/analyze_*.py`) correlated against the diag
UART timeline (UART is ground truth; openocd halt reads are not — BENCH §4/§6).

**Gate order:** 0 → A → B → C → D → E → F → G. Do not advance a phase while an
earlier one is red.

---

## Appendix A — Diag-UART interactive commands (`handle_uart_command`)

| Key | Action |
|---|---|
| `h` / `?` | help |
| `t` | dump SASI trace |
| `f` | force SASI log flush |
| `a` | toggle automatic SASI log flush |
| `p` | dump PIO0 + pin state |
| `i` | dump register IRQ trace |
| `c` | dump fault info |
| `r` | dump DMA-write CRC trace *(only if built `VERIFY_DMA_WRITES=ON`)* |

Automatic emitters to watch for: `STUCK DETECTED`, `CORE1 STACK OVERFLOW`,
`=== HARDFAULT INFO ===`. Note: `Op timing (max us)` / `Stalls` / `Cmd timing`
are **not periodic** — they print only inside the `p` dump
(`dump_pio0_and_pin_state()`, dma_board.c:265) or the automatic stuck-detector
dump. Sample them by sending `p` at test checkpoints.

## Appendix B — Watch-list from code review (`REVIEW_FINDINGS.md`)

- #4 multi-disk mount order (Phase E) — highest test-visible risk.
- #3 `SPI_DEBUG=1` default — only matters if SD init fails and the FujiNet
  fallback engages (512-byte hex dump per sector ⇒ blows the 5 s BIOS timeout).
  If you ever see the FujiNet fallback path in the log, treat results as invalid
  until `SPI_DEBUG` is set to 0.
- CLAUDE.md/README are drifted (reference deleted `dma_read_write.pio`,
  `dma_ultra_fast.c`, etc.) — trust the code, not those docs, when triaging.
