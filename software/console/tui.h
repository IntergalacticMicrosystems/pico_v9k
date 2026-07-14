/* tui.h — management console on the card's USB port (CDC-ACM).
 *
 * Default mode is a line-based REPL whose replies always end with a line "OK" or
 * "ERR <reason>" — trivially parseable, so automated processes (test scripts,
 * LLMs) can drive mounts/ejects. The `menu` command switches to a full-screen
 * ANSI menu for humans. Everything is poll-driven from the core-0 main loop;
 * nothing here blocks longer than one screen repaint (plus the bounded
 * console-ops mailbox wait for a core-1 storage op).
 *
 * Ported from v9k_flop/firmware/tui.{c,h}. Adaptations for the DMA card: 8 SASI
 * targets (no floppy SS/DS geometry), all storage/SPI work deferred to core 1
 * through the console-ops mailbox (console_ops.h), cell buffers in static SRAM
 * (no PSRAM), and the create/copy/new-disk screens dropped.
 */
#ifndef V9K_TUI_H
#define V9K_TUI_H

#include <stdbool.h>
#include <stdint.h>

#include "mtui/transport.h"

/* Bring up TinyUSB + the CDC console (REPL + full-screen menu). No storage is
 * touched here (that all lives on core 1); call after core 1 is running. */
void tui_init(void);
void tui_poll(void);                 /* pump USB + console; call every loop */
bool tui_menu_active(void);          /* full-screen menu currently up (either owner)? */

/* ---- channel ownership of the shared full-screen menu (Stage 2 seams) ------
 * The one mtui engine (screen + app + cell buffers) is owner-aware: at most one
 * console drives the menu at a time. These let a Victor-side console (mgmt_con,
 * Stage 2) borrow it over its own transport. Present and wired but only the CDC
 * owner is exercised in Stage 1a. */
bool tui_menu_enter_via(mtui_transport_t *tp);  /* take the menu for the channel; false if busy */
void tui_menu_feed_via(uint8_t b);              /* feed one channel input byte to the menu */
void tui_menu_poll_via(void);                   /* paint one frame (call every loop while channel owns) */
bool tui_menu_via_active(void);                 /* menu up AND owned by the channel? */
void tui_menu_abort_via(void);                  /* force-exit a channel menu, no cleanup escapes */

#endif /* V9K_TUI_H */
