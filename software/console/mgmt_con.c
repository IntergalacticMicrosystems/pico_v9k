/* mgmt_con.c — second console binding: the management REPL over the host control
 * register channel (mgmt_chan). Port of v9k_flop's firmware/via_con.c.
 *
 * The mtui line engine is transport-abstracted, so this reuses the exact frozen
 * command table + OK/ERR protocol (tui_cmds_bind) that the USB CDC console runs
 * — only the transport differs: replies go out through mgmt_chan_resp_write. The
 * engine doesn't echo, so this reproduces tui.c:repl_key's local echo through
 * the same response channel before feeding each byte to the engine. The full-
 * screen `menu` borrows the shared engine via tui.c's OWNER_VIA seams. */
#include "pico/stdlib.h"   /* defines __in_flash so the blob stays in flash (copy_to_ram) */
#include "mgmt_con.h"
#include "pico_victor/mgmt_chan.h"
#include "tui.h"
#include "tui_cmds.h"
#include "mtui/line.h"
#include "vtasm_dma_blob.h"

static mtui_line_t      s_mc_ln;
static mtui_transport_t s_mc_tp;

/* Transport write = push the reply bytes into the response FIFO. Always reports
 * the full length written (the engine expects an all-or-nothing transport; a
 * full/overflowed channel drops inside mgmt_chan_resp_write). */
static int mc_tp_write(void *ctx, const uint8_t *buf, size_t len)
{
    (void)ctx;
    mgmt_chan_resp_write(buf, len);
    return (int)len;
}

/* Input never arrives via the transport (it comes through the cmd ring), so
 * read() is a no-op that reports "no data". */
static int mc_tp_read(void *ctx, uint8_t *buf, size_t len)
{
    (void)ctx; (void)buf; (void)len;
    return 0;
}

void mgmt_con_init(void)
{
    s_mc_tp.write = mc_tp_write;
    s_mc_tp.read  = mc_tp_read;
    s_mc_tp.flush = NULL;
    s_mc_tp.ctx   = NULL;
    tui_cmds_bind(&s_mc_ln, &s_mc_tp);
}

/* True while the previous poll had a channel-owned menu up; drives the falling-
 * edge prompt re-emission when the user exits the menu (q / Exit row). */
static bool s_was_menu;

static void mc_line_reset(mtui_line_t *ln)
{
    ln->len = 0;
    ln->overflow = false;
    ln->last_cr = false;
}

/* Raw program-blob download for the boot-ROM F4 loader: on the 0x1C command it
 * reads back A5 5A len_lo len_hi <payload> sum_lo sum_hi (sum16 = little-endian
 * sum of the payload bytes, mod 65536). Streamed raw via mgmt_chan_blob_send —
 * bypasses the line engine and its LF translation. Bails silently on any channel
 * failure (reset/overflow); the loader's own timeout handles recovery. */
static void mgmt_con_send_blob(void)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < VTASM_BLOB_LEN; i++) sum += vtasm_blob[i];
    uint8_t hdr[4] = { 0xA5u, 0x5Au,
                       (uint8_t)VTASM_BLOB_LEN, (uint8_t)(VTASM_BLOB_LEN >> 8) };
    uint8_t trailer[2] = { (uint8_t)sum, (uint8_t)(sum >> 8) };
    if (!mgmt_chan_blob_send(hdr, sizeof hdr)) return;
    if (!mgmt_chan_blob_send(vtasm_blob, VTASM_BLOB_LEN)) return;
    mgmt_chan_blob_send(trailer, sizeof trailer);
}

void mgmt_con_poll(void)
{
    mtui_line_t *ln = &s_mc_ln;
    uint8_t b;
    /* A channel reset must also drop the half-typed line: the DOS tool's
     * detection probe pushes 0x55 ('U') into the cmd ring before it resets, and
     * that byte may already have been fed into the engine's line buffer. */
    if (mgmt_chan_take_reset()) {
        mc_line_reset(ln);
        tui_menu_abort_via();            /* kill a stale menu (relaunched VTASM) */
        s_was_menu = false;              /* reset-abort: no unsolicited prompt */
    }

    if (tui_menu_via_active()) {         /* menu owns the channel: bytes are keys */
        while (mgmt_chan_cmd_pop(&b)) tui_menu_feed_via(b);  /* no echo, no engine */
        tui_menu_poll_via();             /* paint one frame */
        s_was_menu = true;
        mgmt_chan_pump();
        return;
    }
    if (s_was_menu) {                    /* menu just exited: back to the prompt */
        s_was_menu = false;
        mc_line_reset(ln);
        mgmt_chan_resp_write((const uint8_t *)"v9k> ", 5);
    }

    while (mgmt_chan_cmd_pop(&b)) {
        if (b == 0x1C) { mgmt_con_send_blob(); continue; }  /* raw blob download (boot-ROM F4 loader) */
        /* Mirror tui.c:repl_key exactly, echoing through the response channel. */
        if (b == '\r' || b == '\n') {
            mgmt_chan_resp_write((const uint8_t *)"\r\n", 2);
            mtui_line_feed(ln, '\r');
        } else if (b == 0x7f || b == 0x08) {
            if (ln->len > 0) mgmt_chan_resp_write((const uint8_t *)"\b \b", 3);
            mtui_line_feed(ln, b);
        } else if (b >= 32 && b < 127 && ln->len < 95) {
            mgmt_chan_resp_write(&b, 1);
            mtui_line_feed(ln, b);
        }
        /* other bytes ignored (as repl_key does) */

        if (ln->done) {                  /* the `menu` command: borrow the screen */
            ln->done = false;
            if (tui_menu_enter_via(&s_mc_tp)) break;   /* engine painted; keys next poll */
            mgmt_chan_resp_write((const uint8_t *)"ERR menu busy\r\n", 15);
            mgmt_chan_resp_write((const uint8_t *)"v9k> ", 5);
        }
    }
    mgmt_chan_pump();
}
