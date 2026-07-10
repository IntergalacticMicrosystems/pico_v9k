# Code Review Findings — 2026-07-09

Inspection of the firmware C sources (`dma_board.c`, `pico_victor/dma.c`, `sasi.c`,
`pico_fujinet/spi.c`, `pico_storage/sd_storage.c`, `pico_victor/register_cache.c`) plus
the project docs. The code itself is careful — good fault handling, timeouts throughout,
explicit cross-core race notes. Findings below are mostly hygiene, documentation drift,
and a couple of behavioral concerns rather than crash bugs.

**Workflow:** test current firmware on hardware first, then apply changes and re-test.
Use the checkboxes to track what's been applied.

Priority order: **#2 (doc drift)** and **#3 (SPI_DEBUG)** are highest value, then
**#1 (.gitignore)** and **#4 (mount ordering)**.

---

## Repository hygiene

### 1. No `.gitignore` — build artifacts and editor cruft are loose in the tree
- [x] Applied (2026-07-09: repo-root `.gitignore` covering build/, .history/, ~*.lck, bench logs, watcom .obj)
- [x] Re-tested (n/a — repo hygiene only)

`git status` shows untracked: `software/build/`, KiCad lock files (`~*.kicad_*.lck`),
`hardware/.history/` (VS Code Local History), and `software/pico_sdk_import.cmake`.
There is no `.gitignore` anywhere.

**Fix:** add a `.gitignore` covering at least `build/`, `.history/`, and `~*.lck`.
The `.history/` directory is local editor state that should never reach the repo.

---

## Documentation drift (largest issue)

### 2. `software/CLAUDE.md` and `README.md` reference files that no longer exist
- [x] Applied (2026-07-09: both rewritten against the current tree; never-modify warning now names `dma_master.pio`/`board_registers.pio`; UART corrected to GPIO0/33 @ 230400; SD-primary)
- [x] Re-tested (n/a — docs)

These docs are the source of truth for anyone (human or AI) touching the project and are
now actively misleading:

| Referenced in docs | Reality |
|---|---|
| `dma_read_write.pio` (+ "**NEVER MODIFY**" warnings) | Gone — DMA PIO is now `dma_master.pio` |
| `extio_helper.pio` | Doesn't exist |
| `dma_ultra_fast.c/.h`, `dma_fast.c`, "ultra-fast handlers" | Don't exist |
| Branch `pico_rewire_oct_b_25` | Actual branch is `main` |

The only PIO files that exist are `board_registers.pio` and `dma_master.pio`. The strong
"DO NOT MODIFY `dma_read_write.pio`" guidance points at a nonexistent file, so the *real*
sensitive file (`dma_master.pio`) has no such guard.

**Fix:** reconcile CLAUDE.md against the current file set; move the "never modify" warning
to `dma_master.pio`; collapse the long dated "Current Status" history into a current summary.

---

## Behavioral concerns

### 3. `pico_fujinet/spi.c:11` — `#define SPI_DEBUG 1` on by default is a timing hazard
- [x] Applied (2026-07-09: default 0; per-sector Read/Write LBA prints gated under SPI_DEBUG; error prints stay unconditional)
- [x] Re-tested (build + full SD-path regression pass; FujiNet fallback not exercised — no FujiNet on bench)

Unlike every other subsystem (`SD_DEBUG_PRINTF 0`, `DMA_DEBUG_PRINTF 0`,
`SASI_DEBUG_PRINTF 0`), SPI debug ships enabled. With it on, `read_data_frame()` prints all
512 bytes as hex on every sector read — ~45 ms of blocking 115200-baud UART per sector —
which will blow the 5 s BIOS command timeout on the FujiNet backend.

**Fix:** default `SPI_DEBUG` to `0`. Also `fujinet_read_sector` / `fujinet_write_sector`
print one line *unconditionally* per sector even with SPI_DEBUG off — consider gating those
under `SPI_DEBUG` too.

### 4. `pico_victor/register_cache.c:201` — multi-disk mount order is nondeterministic
- [x] Applied (2026-07-09: case-insensitive insertion sort of `file_names` after the scan in `sd_storage.c`)
- [x] Re-tested (single-image bench: scan+mount+boot regression pass; true multi-image determinism check still needs a 2nd image staged — see TEST_REPORT.md open items)

The multi-disk mount assigns `sd_storage_get_image_name(t)` to target `t`, but that list
comes straight from `f_readdir()`, whose order FatFS does not guarantee. Which `.img` lands
on target 0 (normally the boot drive) depends on FAT directory layout, not any predictable
rule. With `data.img` + `boot.img` on a card, the host may boot the wrong one.

**Fix:** sort `sd_state->file_names` (e.g. alphabetically) after the scan in
`sd_storage_init()` so target assignment is stable and documentable.

---

## Minor / cosmetic

### 5. `pico_victor/dma.c:443` — stale comment
- [x] Applied (2026-07-09)

Comment says `// set as output to assert HOLD/` but the code is `gpio_set_dir(pin, GPIO_IN);`.
Leftover from the buffer-removal rework.

### 6. `dma_board.c:639` — dead variable
- [x] Applied (2026-07-09)

`uint64_t iterations = INT64_MAX;` is never read; the loop bound uses the literal directly.

### 7. `pico_victor/dma.c:331` (`dma_write_register`, `REG_STATUS` case) — dead store
- [x] Applied (2026-07-09)

`value = dma->status;` is discarded; a write to the status register has no effect.

### 8. `sasi.c:939` (`handle_request_sense`) — always returns "no error" sense data
- [ ] Applied

Returns all-zero sense data even when the preceding command reported CHECK CONDITION.
Fine for boot, but makes the CHECK CONDITION path inconsistent. Note only — revisit if/when
DOS error-reporting fidelity matters.

---

## Notes / observations (no action required)

- Error handling across `sd_storage.c` (retry + remount recovery + target disable
  thresholds) and the DMA timeout/abort paths in `dma.c` is genuinely solid.
- All the SASI phase transitions use the "cache-first" `sasi_bus_ctrl_set` pattern with
  documented rationale for the cross-core ISR race — well handled.
