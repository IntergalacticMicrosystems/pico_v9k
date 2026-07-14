/* mgmt_chan.c — host management/control register channel. See mgmt_chan.h.
 *
 * Ported from v9k_flop's src/via_chan.c with the DMA card's offsets/signature
 * and one structural change: the three bus-side read shadows are not file-scope
 * statics but bound pointers (mgmt_chan_bind_cells) — in the firmware they are
 * the register-cache cells cached_regs.values[0x40/0x50/0x60] that the ISR
 * read-prefetch path serves, so a host read and the internal shadow are one and
 * the same byte. Until bound they point at internal defaults (the host test
 * exercises the read side that way).
 *
 * Single-writer-per-cell discipline (identical to via_chan):
 *   - the 0x40 CMD echo cell: written by core 1 on a CMD write, and by core 0
 *     on a reset re-arm (the protocol keeps the Victor read-only-polling between
 *     the reset write and the 0xFF ack, so the two never collide).
 *   - the 0x50 STATUS cell: core 0 only (status_store from pump/resp_write/reset).
 *   - the 0x60 RESP cell:    core 0 only (the publish step).
 * SPSC fences match via_chan.c's __atomic_thread_fence(__ATOMIC_SEQ_CST). */
#include "mgmt_chan.h"

#define MGMT_FENCE() __atomic_thread_fence(__ATOMIC_SEQ_CST)

/* Bounded spin for a full resp FIFO (~4M iters); the Victor's acks free slots. */
#define MGMT_SPIN_MAX  4000000u

/* Burst-drain up to MGMT_PUMP_BURST bytes per pump, spinning ~MGMT_PUMP_ACK_SPIN
 * for each byte's ack so a menu repaint isn't throttled to one byte per loop. */
#define MGMT_PUMP_BURST     64u
#define MGMT_PUMP_ACK_SPIN  2000u

/* ---- cmd ring: Victor -> emulator (SPSC core1 producer -> core0 consumer) -- */
#define CMD_RING_SZ 64u
static uint8_t           s_cmd[CMD_RING_SZ];
static volatile unsigned s_cmd_head;    /* producer (core1) */
static volatile unsigned s_cmd_tail;    /* consumer (core0) */

/* ---- resp FIFO: emulator -> Victor (core0-local end to end) ---------------- */
#ifndef MGMT_CHAN_RESP_SZ
#define MGMT_CHAN_RESP_SZ 1024u          /* host tests override for a tiny FIFO */
#endif
static uint8_t  s_resp[MGMT_CHAN_RESP_SZ];
static unsigned s_resp_head;            /* producer: mgmt_chan_resp_write */
static unsigned s_resp_tail;            /* consumer: the publish step */

/* ---- read shadows (bound to the register cache in firmware; internal here) - */
static volatile uint8_t s_resp_default;                     /* 0x60 idle = 0x00 */
static volatile uint8_t s_status_default = MGMT_CHAN_SIG;   /* 0xD0, no flags */
static volatile uint8_t s_cmd_default    = 0xFFu;           /* ~0x00 */
static volatile uint8_t *s_resp_byte   = &s_resp_default;   /* current 0x60 value */
static volatile uint8_t *s_status      = &s_status_default; /* current 0x50 value */
static volatile uint8_t *s_cmd_echo    = &s_cmd_default;    /* current 0x40 value */

/* ---- ack / reset handshakes ------------------------------------------------ */
static volatile uint32_t s_ack;         /* core1: bumped by the Victor's 0x60 write */
static uint32_t          s_served;      /* core0: bytes handed out (awaiting ack) */
static volatile uint8_t  s_reset_req;   /* core1 sets on 0x50 write; core0 clears */
static volatile uint32_t s_cmd_drop;    /* core1: bumped on a dropped (ring-full) push */
static uint32_t          s_cmd_drop_seen; /* core0's last-seen s_cmd_drop */

/* ---- core0-owned status flags (folded into the STATUS shadow) -------------- */
static bool s_overflow;                 /* sticky until reset */
static bool s_cmd_full;
static bool s_resp_ready;
static bool s_phase;

/* Rebuild the STATUS shadow from the core0-owned flags. Sole writer of the
 * STATUS cell; callers that just published a byte must have fenced first. */
static void status_store(void)
{
    uint8_t v = MGMT_CHAN_SIG;
    if (s_overflow)   v |= MGMT_CHAN_OVERFLOW;
    if (s_cmd_full)   v |= MGMT_CHAN_CMD_FULL;
    if (s_resp_ready) v |= MGMT_CHAN_RESP_READY;
    if (s_phase)      v |= MGMT_CHAN_RESP_PHASE;
    *s_status = v;
}

/* Publish step: if the Victor has acked the last byte, hand out the next one
 * (byte visible before status) or clear RESP_READY when the FIFO has drained. */
static void mgmt_chan_publish(void)
{
    if (s_served != s_ack) return;              /* last byte not acked yet */
    if (s_resp_tail != s_resp_head) {           /* FIFO non-empty: next byte */
        uint8_t b = s_resp[s_resp_tail];
        s_resp_tail = (s_resp_tail + 1u) % MGMT_CHAN_RESP_SZ;
        *s_resp_byte = b;
        MGMT_FENCE();                           /* byte visible before status */
        s_phase = !s_phase;
        s_resp_ready = true;
        s_served++;                             /* await the Victor's ack */
        status_store();
    } else if (s_resp_ready) {                  /* drained: drop READY, keep phase */
        s_resp_ready = false;
        status_store();
    }
}

bool mgmt_chan_reg_read(uint8_t off, uint8_t *out)
{
    switch (off) {
    case MGMT_CHAN_RESP:   *out = *s_resp_byte; return true;
    case MGMT_CHAN_STATUS: *out = *s_status;    return true;
    case MGMT_CHAN_CMD:    *out = *s_cmd_echo;  return true;
    default:               return false;
    }
}

bool mgmt_chan_reg_write(uint8_t off, uint8_t val)
{
    switch (off) {
    case MGMT_CHAN_CMD: {                       /* push a command byte + echo */
        unsigned h = s_cmd_head, next = (h + 1u) % CMD_RING_SZ;
        if (next == s_cmd_tail) {               /* ring full: signal core0, never block */
            __atomic_fetch_add(&s_cmd_drop, 1u, __ATOMIC_RELAXED);
        } else {
            s_cmd[h] = val;
            MGMT_FENCE();                        /* entry visible before the publish */
            s_cmd_head = next;
        }
        *s_cmd_echo = (uint8_t)~val;            /* detection echo (complement) */
        return true;
    }
    case MGMT_CHAN_RESP:                        /* ack/advance the response byte */
        __atomic_fetch_add(&s_ack, 1u, __ATOMIC_RELAXED);
        return true;
    case MGMT_CHAN_STATUS:                      /* soft reset */
        s_reset_req = 1u;
        return true;
    default:
        return false;
    }
}

bool mgmt_chan_cmd_pop(uint8_t *b)
{
    unsigned t = s_cmd_tail;
    if (t == s_cmd_head) return false;          /* empty */
    MGMT_FENCE();                               /* see the producer's byte store */
    *b = s_cmd[t];
    MGMT_FENCE();                               /* read done before freeing the slot */
    s_cmd_tail = (t + 1u) % CMD_RING_SZ;
    return true;
}

void mgmt_chan_resp_write(const uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (s_overflow) return;                 /* dropped: no per-call re-spin */
        unsigned next = (s_resp_head + 1u) % MGMT_CHAN_RESP_SZ;
        unsigned spins = 0;
        while (next == s_resp_tail) {           /* FIFO full: let the drain catch up */
            mgmt_chan_publish();                /* frees a slot as the Victor acks */
            if (next != s_resp_tail) break;
            if (++spins >= MGMT_SPIN_MAX) {     /* Victor not draining: give up */
                s_overflow = true;
                status_store();
                return;                         /* drop this byte and the rest */
            }
        }
        s_resp[s_resp_head] = buf[i];
        s_resp_head = next;
    }
}

bool mgmt_chan_blob_send(const uint8_t *buf, size_t len)
{
    /* Raw download path (0x1C program blob): stream bytes straight into the resp
     * FIFO with no text/LF translation, then block until the Victor has fully
     * received them. core0-only, like mgmt_chan_resp_write. Bail if the channel
     * is already unusable or a reset is pending — leave s_reset_req for
     * mgmt_chan_take_reset so the normal reset path runs on core0's next pass. */
    if (s_overflow || s_reset_req) return false;

    for (size_t i = 0; i < len; i++) {
        unsigned next = (s_resp_head + 1u) % MGMT_CHAN_RESP_SZ;
        unsigned spins = 0;
        while (next == s_resp_tail) {           /* FIFO full: let the drain catch up */
            if (s_reset_req) return false;      /* reset pending: leave it for take_reset */
            mgmt_chan_publish();                /* frees a slot as the Victor acks */
            if (next != s_resp_tail) break;
            if (++spins >= MGMT_SPIN_MAX) {     /* Victor not draining: give up */
                s_overflow = true;
                status_store();
                return false;
            }
        }
        s_resp[s_resp_head] = buf[i];
        s_resp_head = next;
    }

    /* Drain to fully delivered: FIFO empty AND the last byte acked. Patience
     * resets on each ack so a slow-but-progressing Victor never times out. */
    unsigned spins = 0;
    while (s_resp_tail != s_resp_head || s_served != s_ack) {
        if (s_reset_req) return false;          /* reset pending: leave it for take_reset */
        uint32_t acked = s_ack;
        mgmt_chan_publish();
        if (s_ack != acked) { spins = 0; continue; }
        if (++spins >= MGMT_SPIN_MAX) {         /* Victor stopped acking: give up */
            s_overflow = true;
            status_store();
            return false;
        }
    }
    return true;
}

bool mgmt_chan_take_reset(void)
{
    if (!s_reset_req) return false;             /* no reset pending */
    s_resp_head = s_resp_tail = 0;              /* flush both directions */
    s_served = s_ack;                           /* resync the ack baseline */
    s_cmd_tail = s_cmd_head;                    /* flush cmd ring (consumer side) */
    s_cmd_drop_seen = s_cmd_drop;
    s_overflow = false;
    s_cmd_full = false;
    s_resp_ready = false;
    s_reset_req = 0u;
    status_store();
    /* Reset ack: re-arm the echo to its initial 0xFF. The Victor polls the CMD
     * readback for this after writing STATUS — the flags alone can't tell it the
     * flush actually ran (they may have been clear already). core1 also writes
     * the echo cell, but only on a CMD write, and the protocol keeps the Victor
     * read-only-polling between the reset write and this ack. */
    *s_cmd_echo = 0xFFu;
    return true;
}

void mgmt_chan_pump(void)
{
    /* Fold the core1 drop counter into CMD_FULL; clear once the ring drains. */
    if (s_cmd_drop != s_cmd_drop_seen) { s_cmd_drop_seen = s_cmd_drop; s_cmd_full = true; }
    if (s_cmd_head == s_cmd_tail) s_cmd_full = false;

    /* Burst-drain: publish a byte, then bounded-spin for the Victor's ack so the
     * next byte can go out in this same call. Stops when nothing is outstanding
     * or the Victor didn't ack within the spin. */
    for (unsigned i = 0; i < MGMT_PUMP_BURST; i++) {
        mgmt_chan_publish();
        if (s_served == s_ack) break;           /* FIFO drained / nothing served */
        unsigned spins = 0;
        while (s_served != s_ack && ++spins < MGMT_PUMP_ACK_SPIN) { }
        if (s_served != s_ack) break;           /* Victor slow: leave the rest */
    }
    status_store();                             /* reflect any flag change */
}

void mgmt_chan_bind_cells(volatile uint8_t *cmd_cell,
                          volatile uint8_t *status_cell,
                          volatile uint8_t *resp_cell)
{
    /* Seed the new cells with the current shadow values so a read served from
     * the cache the instant after binding is correct, then repoint. */
    *cmd_cell    = *s_cmd_echo;
    *status_cell = *s_status;
    *resp_cell   = *s_resp_byte;
    s_cmd_echo  = cmd_cell;
    s_status    = status_cell;
    s_resp_byte = resp_cell;
}
