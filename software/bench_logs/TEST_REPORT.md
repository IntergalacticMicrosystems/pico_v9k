# Hardware Test Report — dma_board firmware on the Victor 9000 bench

**Date:** 2026-07-09 evening session
**Firmware under test:** `dma_board` @ git `040c8d5` (main), clean rebuild, `VERIFY_DMA_WRITES=OFF` (default), Release.
**Bench:** per `HARDWARE_TEST_PLAN.md` §0. Single SD image `vidhc_paul.img` (59,075 sectors) on target 0. Diag UART 230400 on debugprobe if01. All artifacts (logs, PNG grabs) in `software/bench_logs/`.

> Build caveat: the FatFS library (`no-OS-FatFS-SD-SDIO-SPI-RPi-Pico`) was not present
> on the bench host and was cloned at master `d5e4534`. If the dev machine pins an
> older revision, the flashed firmware may differ from previous builds (ELF size
> 1,827,336 vs 1,860,580 in the last synced build/). No behavioral issues observed.

## Verdict

**All executed phases PASS. 8/8 Victor boots successful. Zero data-integrity failures
across ~3.7 MB of byte-verified transfers. No HardFaults, no stack-canary trips, no
BIOS timeouts, no FIFO drops.** Two findings (below) are diagnostics-quality issues,
not functional bugs.

| Phase | Result | Evidence |
|---|---|---|
| 0. Asset prep | PASS | All 10 DOS test EXEs built (OpenWatcom); staged to A: via bootload/putfile |
| A. Bring-up smoke | PASS | `phaseA_flash_boot.log`: banner, SD mount, console `h` answered, ~2 min idle, 0 faults |
| B. Boot integration | **PASS 5/5** | `boot1..5_dir.png` all dir-verified `A:\>`; 5 banners, 0 faults in `phaseB_boots.log` |
| B3 timing | PASS | `p`-dump: defer 1740/0 drops, cmd max 234 ms, sd_rd max 3.2 ms, 0 stalls |
| C. Read path | PASS | DIR/TYPE/CHKDSK clean; 37-file `copy dos\*.exe nul` sweep; round-trip getfile byte-identical |
| C4 CRC oracle | **PASS 10/10** | DMAVFY2 read self-consistency 3× per LBA; LBA 104 also matches May-2025 ref CRC |
| C6 64K boundary | **PASS, proven** | DMAVFY2: straddle @ phys 0x0FF00 crossing 0x10000 (+256), CRC == non-straddling ref (`c4_dmavfy2_a.png`) |
| D. Write path | PASS | COPY CON + MD/COPY/DEL/RD; 256 KB random putfile/getfile md5-identical (`6a0c6fd1…`) |
| D4 persistence | PASS | Same 256 KB file md5-identical after emulator+Victor dual reset |
| D5 FORMAT | SKIPPED | deliberate — sole boot image, optional test |
| E. Multi-disk | BLOCKED | single image on SD; needs operator to stage a second (REVIEW #4 mount-order test still open) |
| F1. hdstress | **PASS** | 25×10 K + 800 K, 1,075,200 B written + verified, 0 errors, 0 DOS critical errors (`f1_final.png`) |
| F2. Mid-write reset | **PASS** | Victor reset mid-putfile; booted clean from the *un-reset* emulator; CHKDSK clean; no orphan entries; neighboring 256 KB file still md5-identical |
| G. Fault injection | SKIPPED | G2 needs physical operator; G1 deferred |

Boots: B1–B5, D4, F2 (recovery), FINAL = **8/8** (7 dir-verified, F2 prompt-verified).

## Findings

1. **Stuck-detector false positives during long SD writes** (3 events: 1× Phase C,
   2× Phase F). Signature each time: `STUCK DETECTED` while Core 1 was inside a slow
   SD write/sync (`sd_wr` max 107 ms @ LBA 12, `sync` 82 ms), defer heartbeat already
   advancing 20–34 ms before the dump printed, canary 32/32, drops 0, all DOS ops
   completed. The detector's gate checks `!sasi_in_dma_transfer` but not
   storage-op-in-progress. **Suggested fix:** add an `in_storage_op` flag to the gate
   (or suppress while a SASI command is open). Purely diagnostic noise today, but it
   will cry wolf in every future long-write session.
2. **SD write latency spikes to ~107 ms** (plus 71 ms, and 82 ms sync) under
   directory-update workloads — typical SD wear-leveling. Ample margin vs the 5 s
   BIOS timeout (worst full command 434 ms), but worth remembering if timeouts are
   ever tightened.
3. Observation: one auto-dump sampled `CLK5: 0,0,0 STUCK` mid-stress while the system
   demonstrably kept running — three CPU-paced samples of a 5 MHz clock can alias.
   Treat that dump line as advisory, not proof.
4. Operational (bench, not firmware): `remotectl type` interprets `\t` as TAB — a
   path like `tdir\t2.txt` corrupts into a TAB-split command (cost one recovery
   here). Always escape as `\\` in typed paths. Also `putfile` mangles >8.3 source
   names (`DMA_VERI.E`) — always pass an explicit remote name.
5. `test/dos_dma_test/dma_verify.c`'s write test targets LBAs 200–204 with a stale
   "safe scratch" assumption — unsafe on a live image. Superseded on this bench by
   **`dma_vfy2.c` / `DMAVFY2.EXE`** (new, checked into the working tree): triple-read
   self-consistency, save/restore writes, and an explicit 64 K-boundary straddle test
   that prints the physical addresses as proof.

## Assets now staged on the Victor (A:)

`FTXSERV.EXE` (file server), `DMAVFY2.EXE`, `DMAVERI.EXE`, `HDSTRESS.EXE`,
`SASIPERF.EXE`, `ADDRTEST.EXE`, `T1.TXT`, `BIGD3.BIN` (256 KB known-md5 payload,
`6a0c6fd1366661104684baffb9804bec`). These make future sessions far faster: FTXSERV
enables direct file transfer; BIGD3.BIN is a ready-made integrity probe.

## Regression pass after fixes (same session, later)

All REVIEW_FINDINGS items 1–7 plus the stuck-detector fix (finding #1: extend
`sasi_in_dma_transfer` through the WRITE(6) `storage_sync`) were applied, rebuilt,
and reflashed. Regression results (`regression.log`, `reg_boot.png`):

- Emulator boot + SD mount + storage ready: clean
- Victor boot dir-verified: PASS
- 256 KB putfile/getfile round-trip: byte-identical
- **STUCK false positives: 0** under the identical write-burst workload that fired
  3 events on the pre-fix firmware (and the flag now provably spans the sync window)
- Timing snapshot: sd_wr max 32.6 ms, sync max 3.9 ms, cmd max 239 ms, 0 stalls

## Transfer-speed measurements (post-fix firmware, same session)

Method: DOS `TIME` bracketing (hundredth-second stamps) and host-clock OCR
polling of a cleared screen; hd_stress progress markers timed against the host
clock. Artifacts: `speed_read2.png`, `hs_speed/`, poll transcripts.

| Workload | Result |
|---|---|
| `COPY` 256 KB → NUL (pure sequential read) | 4.0 s DOS-clock / ~2.8–3.3 s host-clock ⇒ **~65–90 KB/s** |
| `COPY` 256 KB file→file (read+write, 512 KB total I/O) | ~4.6 s ⇒ **~110 KB/s aggregate**; the write half added only ~1.8 s over read-only ⇒ write ≳ 100 KB/s |
| hd_stress write phase (800 K) | 4.9 KB/s — **CPU-bound**, not disk: the 8088 pattern-fill loop dominates |
| hd_stress read+verify phase (800 K) | 4.5 KB/s — same reason (byte-compare loop ≈ 5 KB/s on a 5 MHz 8088) |

Cross-check: firmware per-sector maxima (SD read ≤3.2 ms + DMA-to-RAM ≤0.8 ms)
imply a ~125 KB/s order-of-magnitude ceiling — consistent with the measured
~0.1 MB/s DOS-level band. These rates are comparable to or better than the
original Xebec/MFM subsystem, and boot-to-prompt is ~20–25 s.

Caveat: hd_stress is a correctness tool, not a benchmark — its KB/s reflects
the host CPU. A future firmware-side sector counter in the `p` dump would give
exact sustained rates for free.

## Still open / next session

- **Phase E:** stage a 2nd `.img` (operator) → verify multi-target mount and expose
  the REVIEW #4 mount-order nondeterminism, then apply the sort fix and re-test.
- **SASIPERF** (MAME register-fidelity replay) staged but not yet run.
- Longer soak (F1 was ~6 min of sustained I/O; a 30–60 min loop would chase the
  BENCH "degrades after ~15+ openocd cycles" environmental flakiness).
- Apply REVIEW_FINDINGS fixes (now safe: "test first" precondition satisfied) and
  finding #1 above; re-run Phases A–B as regression.
