/* buscap.c — one-shot bus capture using PIO2 as a logic analyzer.
 *
 * Diagnostic for the host register-read corruption (low data nibble reads as a
 * constant float pattern while the high nibble tracks the served value; every
 * SASI status byte reads 0x02 = CHECK CONDITION, costing a REQUEST SENSE after
 * every command). PIO0/PIO1 are untouched; PIO2 is otherwise unused.
 *
 * Samples GPIO0-31 every 2 sys clocks (10 ns @ 200 MHz) into a DMA buffer,
 * armed by a trigger: wait for XACK (GPIO26, active low) to be deasserted then
 * asserted, i.e. the start of the next 0xEF3xx register access. The capture
 * window (4096 samples = ~41 us) covers a whole XACK-stretched 8088 read.
 * Decode prints one line per edge of the interesting signals.
 *
 * Run from the REPL while the Victor spins on a register read (MGSPIN.COM).
 */
#include "buscap.h"

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"

#include "mtui/line.h"

#define CAP_PIO        pio2
#define CAP_SM         0
#define CAP_SAMPLES    4096u
#define CAP_CLKDIV     2u          /* 10 ns/sample @ 200 MHz sysclk */
#define CAP_TRIG_GPIO  26          /* XACK/ — asserted (low) during EF3xx access */

/* Signal bits inside a sample word (GPIO0-31). */
#define BIT(n) (1u << (n))
#define MASK_BD      0x000001FEu   /* GPIO1-8 = BD0-7 */
#define MASK_RD      BIT(21)
#define MASK_WR      BIT(22)
#define MASK_DTR     BIT(23)
#define MASK_ALE     BIT(24)
#define MASK_HOLD    BIT(25)
#define MASK_XACK    BIT(26)
#define MASK_EXTIO   BIT(27)
#define MASK_READY   BIT(28)
#define MASK_HLDA    BIT(29)

static uint32_t s_buf[CAP_SAMPLES];

/* Print the raw edge list for samples [lo, hi) — helper for anomaly detail. */
static void print_edges(mtui_line_t *ln, uint32_t lo, uint32_t hi, uint32_t ns_per)
{
    uint32_t watch = MASK_BD | MASK_RD | MASK_WR | MASK_ALE |
                     MASK_XACK | MASK_EXTIO | MASK_READY |
                     MASK_DTR | MASK_HOLD | MASK_HLDA;
    uint32_t prev = ~s_buf[lo];
    int lines = 0;
    for (uint32_t i = lo; i < hi && lines < 40; i++) {
        uint32_t v = s_buf[i];
        if (((v ^ prev) & watch) == 0) continue;
        prev = v;
        lines++;
        mtui_line_printf(ln, "  %7luns BD=%02lX RD=%lu WR=%lu ALE=%lu XACK=%lu EXTIO=%lu RDY=%lu DTR=%lu HLD=%lu HLDA=%lu\n",
                         (unsigned long)(i * ns_per),
                         (unsigned long)((v & MASK_BD) >> 1),
                         (unsigned long)((v & MASK_RD) ? 1 : 0),
                         (unsigned long)((v & MASK_WR) ? 1 : 0),
                         (unsigned long)((v & MASK_ALE) ? 1 : 0),
                         (unsigned long)((v & MASK_XACK) ? 1 : 0),
                         (unsigned long)((v & MASK_EXTIO) ? 1 : 0),
                         (unsigned long)((v & MASK_READY) ? 1 : 0),
                         (unsigned long)((v & MASK_DTR) ? 1 : 0),
                         (unsigned long)((v & MASK_HOLD) ? 1 : 0),
                         (unsigned long)((v & MASK_HLDA) ? 1 : 0));
    }
}

bool buscap_run(mtui_line_t *ln, uint32_t skip, uint32_t timeout_ms, bool wide,
                int expect)
{
    /* wide mode: 40 ns/sample, ~164 us window (~30 back-to-back accesses),
     * decoded as one summary line per XACK-framed access instead of edges. */
    const uint32_t clkdiv = wide ? 8u : CAP_CLKDIV;
    const uint32_t ns_per = 5u * clkdiv;
    /* Hand-assembled capture program:
     *   pull block        ; 32-bit skip count supplied via the TX FIFO
     *   out x, 32
     * skiploop:
     *   wait 1 gpio 26    ; ensure XACK idle (deasserted)
     *   wait 0 gpio 26    ; XACK asserted = register access begins
     *   jmp x--, skiploop ; falls through after skip+1 assertions... so the
     *                     ; LAST awaited edge is the one we capture from
     * loop:
     *   in pins, 32       ; autopush; wraps back to loop
     * With skip=N the capture starts at the (N+1)th EF3xx access.
     */
    uint16_t prog[6] = {
        (uint16_t)pio_encode_pull(false, true),
        (uint16_t)pio_encode_out(pio_x, 32),
        (uint16_t)pio_encode_wait_gpio(true,  CAP_TRIG_GPIO),
        (uint16_t)pio_encode_wait_gpio(false, CAP_TRIG_GPIO),
        (uint16_t)pio_encode_jmp_x_dec(0),      /* patched to skiploop below */
        (uint16_t)pio_encode_in(pio_pins, 32),
    };
    struct pio_program p = { .instructions = prog, .length = 6, .origin = -1 };

    if (!pio_can_add_program(CAP_PIO, &p)) {
        mtui_line_write(ln, "ERR pio2 program space\n");
        return false;
    }
    uint off = pio_add_program(CAP_PIO, &p);
    /* Patch the skip-loop jmp target now that the load offset is known
     * (pio_add_program does not relocate jmp targets). */
    CAP_PIO->instr_mem[off + 4] = pio_encode_jmp_x_dec(off + 2);
    pio_sm_claim(CAP_PIO, CAP_SM);

    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_in_pins(&c, 0);
    sm_config_set_in_shift(&c, true /*right*/, true /*autopush*/, 32);
    sm_config_set_out_shift(&c, true /*right*/, false /*autopull*/, 32);
    sm_config_set_clkdiv_int_frac(&c, clkdiv, 0);
    sm_config_set_wrap(&c, off + 5, off + 5);   /* loop on the in instruction */
    pio_sm_init(CAP_PIO, CAP_SM, off, &c);
    pio_sm_put(CAP_PIO, CAP_SM, skip);          /* consumed by the pull */

    int ch = dma_claim_unused_channel(false);
    if (ch < 0) {
        pio_sm_unclaim(CAP_PIO, CAP_SM);
        pio_remove_program(CAP_PIO, &p, off);
        mtui_line_write(ln, "ERR no free DMA channel\n");
        return false;
    }
    dma_channel_config dc = dma_channel_get_default_config((uint)ch);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
    channel_config_set_read_increment(&dc, false);
    channel_config_set_write_increment(&dc, true);
    channel_config_set_dreq(&dc, pio_get_dreq(CAP_PIO, CAP_SM, false));
    dma_channel_configure((uint)ch, &dc, s_buf,
                          &CAP_PIO->rxf[CAP_SM], CAP_SAMPLES, true);

    pio_sm_set_enabled(CAP_PIO, CAP_SM, true);
    mtui_line_write(ln, "armed: waiting for XACK edge (run MGSPIN on the Victor)\n");

    absolute_time_t dl = make_timeout_time_ms(timeout_ms);
    bool done = true;
    while (dma_channel_is_busy((uint)ch)) {
        if (time_reached(dl)) { done = false; break; }
        sleep_ms(1);
    }

    pio_sm_set_enabled(CAP_PIO, CAP_SM, false);
    if (!done) dma_channel_abort((uint)ch);
    dma_channel_unclaim((uint)ch);
    pio_sm_unclaim(CAP_PIO, CAP_SM);
    pio_remove_program(CAP_PIO, &p, off);

    if (!done) {
        mtui_line_write(ln, "ERR trigger timeout (no EF3xx access seen)\n");
        return false;
    }

    if (wide) {
        /* One line per XACK-framed access: assert time, XACK duration, data
         * bus at mid-access and at the sample just before RD's rising edge
         * (= what the 8088 latches), plus whether RD/WR were seen asserted. */
        uint32_t i = 0;
        int acc = 0;
        while (i < CAP_SAMPLES && !(s_buf[i] & MASK_XACK)) i++;  /* sync to idle */
        while (i < CAP_SAMPLES && acc < 40) {
            while (i < CAP_SAMPLES && (s_buf[i] & MASK_XACK)) i++;
            if (i >= CAP_SAMPLES) break;
            uint32_t start = i;
            bool rd_low = false, wr_low = false;
            uint32_t rd_rise = 0;
            while (i < CAP_SAMPLES && !(s_buf[i] & MASK_XACK)) {
                if (!(s_buf[i] & MASK_RD)) rd_low = true;
                else if (rd_low && !rd_rise) rd_rise = i;
                if (!(s_buf[i] & MASK_WR)) wr_low = true;
                i++;
            }
            uint32_t end = i;
            /* RD can rise shortly after XACK release — look a bit further. */
            for (uint32_t j = end; j < CAP_SAMPLES && j < end + 64 && rd_low && !rd_rise; j++) {
                if (s_buf[j] & MASK_RD) rd_rise = j;
            }
            if (end >= CAP_SAMPLES) break;   /* truncated access */
            uint32_t mid = (start + end) / 2;
            uint32_t latch = rd_rise ? rd_rise - 1 : end - 1;
            uint32_t bd_latch = (s_buf[latch] & MASK_BD) >> 1;
            mtui_line_printf(ln, "#%02d t=%7luns dur=%5luns %s%s mid=%02lX latch=%02lX e=%lu r=%lu\n",
                             acc,
                             (unsigned long)(start * ns_per),
                             (unsigned long)((end - start) * ns_per),
                             rd_low ? "R" : "-", wr_low ? "W" : "-",
                             (unsigned long)((s_buf[mid] & MASK_BD) >> 1),
                             (unsigned long)bd_latch,
                             (unsigned long)((s_buf[latch] & MASK_EXTIO) ? 1 : 0),
                             (unsigned long)((s_buf[latch] & MASK_READY) ? 1 : 0));
            if (expect >= 0 && rd_low && bd_latch != (uint32_t)expect) {
                uint32_t lo = start > 12 ? start - 12 : 0;
                uint32_t hi = (rd_rise ? rd_rise : end) + 64;
                if (hi > CAP_SAMPLES) hi = CAP_SAMPLES;
                print_edges(ln, lo, hi, ns_per);
            }
            acc++;
        }
        mtui_line_printf(ln, "%d accesses in %luus\n", acc,
                         (unsigned long)(CAP_SAMPLES * ns_per / 1000u));
        return true;
    }

    /* Decode: print a line whenever an interesting signal changes. */
    uint32_t watch = MASK_BD | MASK_RD | MASK_WR | MASK_ALE |
                     MASK_XACK | MASK_EXTIO | MASK_READY |
                     MASK_DTR | MASK_HOLD | MASK_HLDA;
    uint32_t prev = ~s_buf[0];
    int lines = 0;
    for (uint32_t i = 0; i < CAP_SAMPLES && lines < 120; i++) {
        uint32_t v = s_buf[i];
        if (((v ^ prev) & watch) == 0) continue;
        prev = v;
        lines++;
        mtui_line_printf(ln, "%5luns BD=%02lX RD=%lu WR=%lu ALE=%lu XACK=%lu EXTIO=%lu RDY=%lu DTR=%lu HLD=%lu HLDA=%lu\n",
                         (unsigned long)(i * ns_per),
                         (unsigned long)((v & MASK_BD) >> 1),
                         (unsigned long)((v & MASK_RD) ? 1 : 0),
                         (unsigned long)((v & MASK_WR) ? 1 : 0),
                         (unsigned long)((v & MASK_ALE) ? 1 : 0),
                         (unsigned long)((v & MASK_XACK) ? 1 : 0),
                         (unsigned long)((v & MASK_EXTIO) ? 1 : 0),
                         (unsigned long)((v & MASK_READY) ? 1 : 0),
                         (unsigned long)((v & MASK_DTR) ? 1 : 0),
                         (unsigned long)((v & MASK_HOLD) ? 1 : 0),
                         (unsigned long)((v & MASK_HLDA) ? 1 : 0));
    }
    if (lines >= 120) mtui_line_write(ln, "... (edge list capped)\n");
    return true;
}
