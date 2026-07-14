/* mgmt_chan.h — host management/control register channel for the DMA card.
 *
 * Ported nearly verbatim from v9k_flop's src/via_chan.{c,h}: a transport-
 * agnostic SPSC channel with explicit SEQ_CST fences, no Pico SDK includes, so
 * it links into the host unit test (test/host/test_mgmt.c). Only the register
 * offsets and the signature nibble differ from the floppy card's VIATERM
 * channel (which uses 0xB0 + VIA-A offsets), so a client can tell the two apart.
 *
 * The Victor drives the disk-control console over three virtual registers in the
 * DMA card's 0xEF300 window (16-byte cells, below 0x80, per dma_mask_offset):
 *   0x40 CMD     read  = ~last-written byte (detection echo; idle/reset 0xFF)
 *                write = push a command byte into the cmd ring (Victor -> emu)
 *   0x50 STATUS  read  = 0xD0 | flags (below)
 *                write = soft reset (flush both directions, clear flags; the
 *                        CMD echo re-arms to 0xFF as the "reset done" ack)
 *   0x60 RESP    read  = current response byte (emu -> Victor)
 *                write = ack/advance (bumps the ack counter)
 *
 * STATUS byte: high nibble 0xD (fixed signature) plus
 *   b3 OVERFLOW (sticky until reset), b2 CMD_FULL, b1 RESP_READY, b0 RESP_PHASE
 *   (toggled per published byte so the Victor tells "new byte" from "reread").
 *
 * Concurrency (the whole point):
 *   - bus side = the register ISR (core 1): a byte written to 0x40 is pushed
 *     into the cmd ring (producer), a 0x50 write sets a reset-request flag, a
 *     0x60 write bumps the ack counter. It only ever writes the 0x40 echo cell.
 *   - console side = core 0 (mgmt_con_poll): the sole writer of the 0x50/0x60
 *     cells, owns the resp FIFO end to end, publishes one byte at a time.
 *
 * The three read "shadows" are the register-cache cells the ISR read-prefetch
 * path serves (cached_regs.values[0x40/0x50/0x60]). mgmt_chan writes them
 * through bound pointers; before mgmt_chan_bind_cells() is called they point at
 * internal statics (that is how the host test exercises the read side).
 */
#ifndef V9K_MGMT_CHAN_H
#define V9K_MGMT_CHAN_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* fw register offsets (after dma_mask_offset, in the 0xEF300 window). */
#define MGMT_CHAN_CMD        0x40u
#define MGMT_CHAN_STATUS     0x50u
#define MGMT_CHAN_RESP       0x60u

/* STATUS layout. */
#define MGMT_CHAN_SIG        0xD0u   /* fixed signature nibble (flop card = 0xB0) */
#define MGMT_CHAN_OVERFLOW   0x08u
#define MGMT_CHAN_CMD_FULL   0x04u
#define MGMT_CHAN_RESP_READY 0x02u
#define MGMT_CHAN_RESP_PHASE 0x01u

/* ---- bus-side (core 1 / ISR) entrypoints: single loads / O(1) pushes only -- */

/* Read one of the three channel registers into *out. Returns false for any
 * other offset (the caller falls back to its default). Used by the host test;
 * the firmware ISR serves the same values straight from the register cache. */
bool mgmt_chan_reg_read(uint8_t off, uint8_t *out);

/* Handle a write to one of the three channel registers. Returns false for any
 * other offset. O(1), safe from ISR context. */
bool mgmt_chan_reg_write(uint8_t off, uint8_t val);

/* ---- console-side (core 0) API -------------------------------------------- */

/* Pop one command byte from the ring. Returns false when empty. */
bool mgmt_chan_cmd_pop(uint8_t *b);

/* Enqueue response bytes for the Victor. Bounded-blocks on a full FIFO while the
 * Victor drains it; on timeout it latches OVERFLOW and drops the rest (and every
 * later call then drops immediately, no re-spin). */
void mgmt_chan_resp_write(const uint8_t *buf, size_t len);

/* Stream raw bytes into the resp FIFO (no text/LF translation) and bounded-block
 * until the Victor has FULLY received them. Returns false without sending
 * further bytes if OVERFLOW is latched, a soft reset is pending, or the Victor
 * stops draining. core0-only. Used for the raw 0x1C program-blob download. */
bool mgmt_chan_blob_send(const uint8_t *buf, size_t len);

/* Service a pending soft reset: flush both directions, clear flags, re-arm the
 * CMD echo to 0xFF. Returns true if a reset was performed (so the caller can
 * drop its own per-session state). Call BEFORE draining the cmd ring each loop. */
bool mgmt_chan_take_reset(void);

/* Advance the channel: publish the next response byte if the last was acked.
 * Call every core0 loop (after take_reset + the drain). */
void mgmt_chan_pump(void);

/* Point the bus-side read shadows at externally owned cells (the register-cache
 * cells cached_regs.values[0x40/0x50/0x60]). Copies the current shadow values
 * into the new cells first, so a read served straight from the cache is always
 * correct. Call once on core 1 after the cache is initialized; before this the
 * shadows live in internal statics. Not needed by the host test. */
void mgmt_chan_bind_cells(volatile uint8_t *cmd_cell,
                          volatile uint8_t *status_cell,
                          volatile uint8_t *resp_cell);

#endif /* V9K_MGMT_CHAN_H */
