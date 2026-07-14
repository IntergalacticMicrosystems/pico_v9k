/* tui_cmds.h — the REPL command layer for the management console.
 *
 * The line commands (ls/status/mount/eject/peek/wifi/diag/menu/help) and the
 * connect banner, on the mtui line-mode engine. Ported from v9k_flop's
 * tui_cmds.{c,h}; adapted to the DMA card's 8 SASI targets and its console-ops
 * mailbox (all storage/SPI work runs on core 1). The full-screen TUI screens
 * live in tui.c.
 */
#ifndef V9K_TUI_CMDS_H
#define V9K_TUI_CMDS_H

#include "mtui/line.h"
#include "mtui/transport.h"

/* Bind `ln` to the frozen REPL command table over transport `tp`, with the
 * "v9k> " prompt and the OK/ERR unknown-command reply. Re-callable: each DTR
 * connect re-binds to reset line state. */
void tui_cmds_bind(mtui_line_t *ln, mtui_transport_t *tp);

/* The connect banner: banner text + a status line per mounted target + prompt. */
void tui_repl_banner(mtui_line_t *ln);

#endif /* V9K_TUI_CMDS_H */
