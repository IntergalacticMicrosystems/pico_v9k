/* buscap.h — one-shot PIO2 bus capture (see buscap.c). Bench diagnostic. */
#ifndef V9K_BUSCAP_H
#define V9K_BUSCAP_H

#include <stdint.h>
#include <stdbool.h>

typedef struct mtui_line mtui_line_t;

/* Arm PIO2, skip `skip` EF3xx accesses (XACK pulses), capture from the next
 * one, and print through `ln`. Normal mode: ~41 us at 10 ns resolution, edge
 * list. Wide mode: ~164 us at 40 ns, one summary line per access (XACK
 * duration + data bus at mid-access and at the RD latch point).
 * Runs on core 0; touches only PIO2 + one DMA channel. */
/* expect: -1 = none, else 0-255 — in wide mode, any read access whose latched
 * data byte differs gets a full 40 ns edge dump appended. */
bool buscap_run(mtui_line_t *ln, uint32_t skip, uint32_t timeout_ms, bool wide,
                int expect);

#endif
