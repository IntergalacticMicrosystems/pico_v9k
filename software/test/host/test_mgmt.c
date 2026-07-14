/* test_mgmt.c — host unit tests for the DMA-card host control channel
 * (pico_victor/mgmt_chan.c). Ported from v9k_flop's test/test_via.c with the
 * DMA card's offsets (0x40/0x50/0x60) and signature nibble (0xD0).
 * Build & run: make -C test/host  (or see the Makefile).
 *
 * Built twice: with the default 1024-byte resp FIFO (test_mgmt) and with an
 * 8-byte FIFO (test_mgmt_tiny, -DMGMT_CHAN_RESP_SZ=8) — the small build is the
 * tiny-FIFO no-loss case. The tests are size-agnostic: every response is driven
 * through a simulated Victor poll loop that reconstructs the exact byte stream.
 *
 * The "Victor" side here uses only the bus-facing register calls
 * (mgmt_chan_reg_read/write); the "emulator" side uses the core0 API
 * (resp_write/cmd_pop/pump) — the same split as the two firmware cores. */
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include "mgmt_chan.h"
#include "vtasm_dma_blob.h"    /* the raw 0x1C download payload */

/* Mirror mgmt_chan.c's default so this test knows the FIFO capacity (the -D that
 * shrinks the FIFO is applied to both files in one compile). */
#ifndef MGMT_CHAN_RESP_SZ
#define MGMT_CHAN_RESP_SZ 1024u
#endif

static int g_fail = 0, g_checks = 0;
#define CHECK(cond) do { g_checks++; if (!(cond)) { \
    printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

/* Soft-reset the channel to a known-clean state between tests. */
static void chan_reset(void)
{
    mgmt_chan_reg_write(MGMT_CHAN_STATUS, 0);  /* Victor's reset write */
    mgmt_chan_take_reset();                    /* core0 services it */
    mgmt_chan_pump();
}

/* Drive the emulator->Victor path for `len` bytes and collect what the Victor
 * receives into `recv`. Interleaved so it never blocks at any FIFO size: push
 * only while there is guaranteed FIFO room (so resp_write can't spin), pump,
 * then run one Victor poll (read STATUS; on READY+phase-change read RESP and
 * ack). Returns the number of bytes reconstructed. */
static size_t send_and_collect(const uint8_t *msg, size_t len, uint8_t *recv)
{
    /* Cap outstanding (pushed-but-unacked, incl. the byte in the shadow) below
     * the FIFO's usable depth so a 1-byte resp_write always fits without spin. */
    unsigned cap = (MGMT_CHAN_RESP_SZ > 2u) ? (MGMT_CHAN_RESP_SZ - 2u) : 1u;
    uint8_t st;
    mgmt_chan_reg_read(MGMT_CHAN_STATUS, &st);
    int last_phase = st & MGMT_CHAN_RESP_PHASE;
    size_t pushed = 0, got = 0;
    unsigned long guard = 0, guard_max = (unsigned long)len * 64u + 100000u;

    while ((pushed < len || got < len) && ++guard < guard_max) {
        while (pushed < len && (unsigned)(pushed - got) < cap) {
            mgmt_chan_resp_write(&msg[pushed], 1);
            pushed++;
        }
        mgmt_chan_pump();
        mgmt_chan_reg_read(MGMT_CHAN_STATUS, &st);
        if ((st & MGMT_CHAN_RESP_READY) &&
            (int)(st & MGMT_CHAN_RESP_PHASE) != last_phase) {
            uint8_t b;
            mgmt_chan_reg_read(MGMT_CHAN_RESP, &b);
            recv[got++] = b;
            mgmt_chan_reg_write(MGMT_CHAN_RESP, 0);   /* ack/advance */
            last_phase = st & MGMT_CHAN_RESP_PHASE;
        }
    }
    return got;
}

/* (a) Detection: two-point echo (~X for two X) + a stable 0xD signature, and the
 * probe sequence the ROM/DOS clients use (0x55->0xAA, 0xAA->0x55). */
static void test_detection(void)
{
    printf("detection:\n");
    chan_reset();
    uint8_t v;

    /* pre-command echo is ~0x00 = 0xFF (initialised so probing works cold). */
    CHECK(mgmt_chan_reg_read(MGMT_CHAN_CMD, &v) && v == 0xFFu);

    mgmt_chan_reg_write(MGMT_CHAN_CMD, 0x55);
    CHECK(mgmt_chan_reg_read(MGMT_CHAN_CMD, &v) && v == 0xAAu);   /* ~0x55 */
    mgmt_chan_reg_write(MGMT_CHAN_CMD, 0xAA);
    CHECK(mgmt_chan_reg_read(MGMT_CHAN_CMD, &v) && v == 0x55u);   /* ~0xAA */

    /* STATUS high nibble is the fixed 0xD signature on two consecutive reads. */
    uint8_t s1, s2;
    CHECK(mgmt_chan_reg_read(MGMT_CHAN_STATUS, &s1) && (s1 & 0xF0u) == MGMT_CHAN_SIG);
    CHECK(mgmt_chan_reg_read(MGMT_CHAN_STATUS, &s2) && (s2 & 0xF0u) == MGMT_CHAN_SIG);

    /* offsets outside the channel are not ours (caller falls back to 0xff). */
    CHECK(!mgmt_chan_reg_read(0x00, &v));
    CHECK(!mgmt_chan_reg_read(0x20, &v));   /* REG_STATUS: a real reg, not ours */
    CHECK(!mgmt_chan_reg_write(0x70, 0));   /* unused offset in the window */
}

/* (b) Full round trip: command bytes in via 0x40, drained with cmd_pop, then a
 * canned multi-line response reconstructed byte-for-byte by the Victor loop. */
static void test_roundtrip(void)
{
    printf("roundtrip:\n");
    chan_reset();

    const char *cmd = "status\r";
    for (const char *p = cmd; *p; p++) mgmt_chan_reg_write(MGMT_CHAN_CMD, (uint8_t)*p);

    uint8_t got[32]; int gi = 0;
    uint8_t x;
    while (mgmt_chan_cmd_pop(&x)) got[gi++] = x;
    CHECK(gi == (int)strlen(cmd) && memcmp(got, cmd, gi) == 0);

    const char *resp = "drive 0: disk.img\r\ndrive 1: (empty)\r\nOK\r\n";
    size_t n = strlen(resp);
    uint8_t recv[128];
    size_t r = send_and_collect((const uint8_t *)resp, n, recv);
    CHECK(r == n && memcmp(recv, resp, n) == 0);
}

/* (c) Small-FIFO no-loss under a slow-draining Victor. Message is 3x the FIFO
 * so it wraps many times and hits backpressure; the interleaved driver drains
 * one byte per cycle, and every byte must still arrive in order. */
static void test_smallfifo_noloss(void)
{
    printf("smallfifo_noloss:\n");
    chan_reset();

    uint8_t msg[3u * MGMT_CHAN_RESP_SZ];
    for (size_t i = 0; i < sizeof msg; i++) msg[i] = (uint8_t)(i * 31u + 7u);
    uint8_t recv[3u * MGMT_CHAN_RESP_SZ];
    size_t r = send_and_collect(msg, sizeof msg, recv);
    CHECK(r == sizeof msg && memcmp(recv, msg, sizeof msg) == 0);
}

/* (d) OVERFLOW on a never-acking Victor: a single resp_write past the FIFO must
 * latch OVERFLOW (bounded, no hang), and a SECOND write must return at once. */
static void test_overflow(void)
{
    printf("overflow:\n");
    chan_reset();

    static uint8_t big[MGMT_CHAN_RESP_SZ + 16u];
    memset(big, 'x', sizeof big);
    mgmt_chan_resp_write(big, sizeof big);     /* spins to MGMT_SPIN_MAX, then drops */

    uint8_t st;
    mgmt_chan_reg_read(MGMT_CHAN_STATUS, &st);
    CHECK(st & MGMT_CHAN_OVERFLOW);
    CHECK((st & 0xF0u) == MGMT_CHAN_SIG);      /* signature intact under overflow */

    /* Second write must short-circuit (no per-call re-spin) — if it re-spun the
     * test would still finish (bound), but it must also not clear OVERFLOW. */
    mgmt_chan_resp_write(big, sizeof big);
    mgmt_chan_reg_read(MGMT_CHAN_STATUS, &st);
    CHECK(st & MGMT_CHAN_OVERFLOW);

    /* OVERFLOW is sticky until reset. */
    chan_reset();
    mgmt_chan_reg_read(MGMT_CHAN_STATUS, &st);
    CHECK(!(st & MGMT_CHAN_OVERFLOW));
}

/* (e) Reset flushes both directions + clears flags, and detection still works. */
static void test_reset(void)
{
    printf("reset:\n");
    chan_reset();

    /* leave a command byte in the ring and a response byte in flight */
    mgmt_chan_reg_write(MGMT_CHAN_CMD, 'a');
    mgmt_chan_reg_write(MGMT_CHAN_CMD, 'b');
    mgmt_chan_resp_write((const uint8_t *)"hi", 2);
    mgmt_chan_pump();                           /* publishes 'h' into the shadow */

    uint8_t b;
    /* no reset pending: take_reset is a no-op and reports so */
    CHECK(!mgmt_chan_take_reset());

    /* soft reset — take_reset performs the flush, reports it, and re-arms the
     * CMD echo to 0xFF (the "reset done" ack the Victor polls for; the last
     * command write above left it at ~'b'). */
    mgmt_chan_reg_write(MGMT_CHAN_STATUS, 0);
    CHECK(mgmt_chan_reg_read(MGMT_CHAN_CMD, &b) && b == (uint8_t)~'b');
    CHECK(mgmt_chan_take_reset());
    CHECK(mgmt_chan_reg_read(MGMT_CHAN_CMD, &b) && b == 0xFFu);
    mgmt_chan_pump();

    CHECK(!mgmt_chan_cmd_pop(&b));               /* cmd ring flushed */

    uint8_t st;
    mgmt_chan_reg_read(MGMT_CHAN_STATUS, &st);
    CHECK((st & 0xF0u) == MGMT_CHAN_SIG);
    CHECK(!(st & MGMT_CHAN_OVERFLOW));
    CHECK(!(st & MGMT_CHAN_RESP_READY));

    /* detection still works after reset */
    mgmt_chan_reg_write(MGMT_CHAN_CMD, 0x55);
    CHECK(mgmt_chan_reg_read(MGMT_CHAN_CMD, &b) && b == 0xAAu);

    /* and a fresh response still round-trips */
    uint8_t recv[8];
    size_t r = send_and_collect((const uint8_t *)"OK\r\n", 4, recv);
    CHECK(r == 4 && memcmp(recv, "OK\r\n", 4) == 0);
}

/* Build the framed blob stream exactly as mgmt_con.c's mgmt_con_send_blob does:
 * A5 5A len_lo len_hi <vtasm_blob> sum_lo sum_hi, sum16 = LE sum of payload. */
static size_t build_blob_stream(uint8_t *out)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < VTASM_BLOB_LEN; i++) sum += vtasm_blob[i];
    size_t n = 0;
    out[n++] = 0xA5u; out[n++] = 0x5Au;
    out[n++] = (uint8_t)VTASM_BLOB_LEN; out[n++] = (uint8_t)(VTASM_BLOB_LEN >> 8);
    memcpy(&out[n], vtasm_blob, VTASM_BLOB_LEN); n += VTASM_BLOB_LEN;
    out[n++] = (uint8_t)sum; out[n++] = (uint8_t)(sum >> 8);
    return n;
}

/* blob_send is blocking (drains to fully-delivered), so it must run on its own
 * "core0" thread while the main "Victor" thread drains the FIFO — the same two-
 * core split the firmware uses (only volatile/atomic shadows cross the seam). */
struct blob_arg { const uint8_t *buf; size_t len; bool ret; };
static void *blob_send_thread(void *p)
{
    struct blob_arg *a = p;
    a->ret = mgmt_chan_blob_send(a->buf, a->len);
    return NULL;
}

/* (f) Full framed 0x1C stream: blob_send delivers the whole frame and the
 * emulated Victor reconstructs it byte-for-byte — magic, length, payload
 * identical to vtasm_blob, and correct sum16. */
static void test_blob_stream(void)
{
    printf("blob_stream:\n");
    chan_reset();

    static uint8_t stream[4u + VTASM_BLOB_LEN + 2u];
    size_t slen = build_blob_stream(stream);

    struct blob_arg arg = { stream, slen, false };
    pthread_t th;
    pthread_create(&th, NULL, blob_send_thread, &arg);

    static uint8_t recv[sizeof stream];
    size_t got = 0;
    uint8_t st;
    mgmt_chan_reg_read(MGMT_CHAN_STATUS, &st);
    int last_phase = st & MGMT_CHAN_RESP_PHASE;
    unsigned long guard = 0, guard_max = (unsigned long)slen * 4096u + 1000000u;
    while (got < slen && ++guard < guard_max) {
        mgmt_chan_reg_read(MGMT_CHAN_STATUS, &st);
        if ((st & MGMT_CHAN_RESP_READY) &&
            (int)(st & MGMT_CHAN_RESP_PHASE) != last_phase) {
            uint8_t b;
            mgmt_chan_reg_read(MGMT_CHAN_RESP, &b);
            recv[got++] = b;
            mgmt_chan_reg_write(MGMT_CHAN_RESP, 0);       /* ack/advance */
            last_phase = st & MGMT_CHAN_RESP_PHASE;
        }
    }
    pthread_join(th, NULL);

    CHECK(arg.ret == true);
    CHECK(got == slen);
    CHECK(recv[0] == 0xA5u && recv[1] == 0x5Au);
    unsigned rlen = recv[2] | ((unsigned)recv[3] << 8);
    CHECK(rlen == VTASM_BLOB_LEN);
    CHECK(memcmp(&recv[4], vtasm_blob, VTASM_BLOB_LEN) == 0);
    uint32_t sum = 0;
    for (size_t i = 0; i < VTASM_BLOB_LEN; i++) sum += vtasm_blob[i];
    unsigned rsum = recv[4 + VTASM_BLOB_LEN] | ((unsigned)recv[5 + VTASM_BLOB_LEN] << 8);
    CHECK(rsum == (sum & 0xFFFFu));
}

/* (g) Reset mid-send: the Victor drains a few bytes, then requests a soft reset
 * and stops acking. blob_send must abort false without consuming the reset, and
 * the channel must then settle normally (reset serviced, CMD echo 0xFF, REPL
 * responsive). */
static void test_blob_reset_abort(void)
{
    printf("blob_reset_abort:\n");
    chan_reset();

    static uint8_t stream[4u + VTASM_BLOB_LEN + 2u];
    size_t slen = build_blob_stream(stream);

    struct blob_arg arg = { stream, slen, true };
    pthread_t th;
    pthread_create(&th, NULL, blob_send_thread, &arg);

    /* Drain a handful of bytes, then pull the reset lever and stop draining. */
    uint8_t st;
    mgmt_chan_reg_read(MGMT_CHAN_STATUS, &st);
    int last_phase = st & MGMT_CHAN_RESP_PHASE;
    unsigned got = 0;
    unsigned long guard = 0;
    while (got < 8 && ++guard < 1000000u) {
        mgmt_chan_reg_read(MGMT_CHAN_STATUS, &st);
        if ((st & MGMT_CHAN_RESP_READY) &&
            (int)(st & MGMT_CHAN_RESP_PHASE) != last_phase) {
            uint8_t b;
            mgmt_chan_reg_read(MGMT_CHAN_RESP, &b);
            mgmt_chan_reg_write(MGMT_CHAN_RESP, 0);
            last_phase = st & MGMT_CHAN_RESP_PHASE;
            got++;
        }
    }
    mgmt_chan_reg_write(MGMT_CHAN_STATUS, 0);   /* request reset mid-send */
    pthread_join(th, NULL);

    CHECK(arg.ret == false);                    /* blob_send bailed */

    /* Reset was not consumed by blob_send: core0 services it now. */
    uint8_t b;
    CHECK(mgmt_chan_take_reset());              /* reset still pending -> serviced */
    CHECK(mgmt_chan_reg_read(MGMT_CHAN_CMD, &b) && b == 0xFFu);
    mgmt_chan_pump();

    mgmt_chan_reg_read(MGMT_CHAN_STATUS, &st);
    CHECK((st & 0xF0u) == MGMT_CHAN_SIG);
    CHECK(!(st & MGMT_CHAN_OVERFLOW));

    /* REPL responsive again: a fresh response round-trips. */
    uint8_t recv[8];
    size_t r = send_and_collect((const uint8_t *)"OK\r\n", 4, recv);
    CHECK(r == 4 && memcmp(recv, "OK\r\n", 4) == 0);
}

int main(void)
{
    printf("resp FIFO size = %u\n", (unsigned)MGMT_CHAN_RESP_SZ);
    test_detection();
    test_roundtrip();
    test_smallfifo_noloss();
    test_overflow();
    test_reset();
    test_blob_stream();
    test_blob_reset_abort();
    printf("\n%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
