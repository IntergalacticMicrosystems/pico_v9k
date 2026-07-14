/* mgmt_con.h — the management console, bound a SECOND time onto the host control
 * register channel (mgmt_chan) so the Victor 9000 can drive the same console the
 * USB CDC port offers. Core0-only; driven from the main loop next to tui_poll().
 *
 * Port of v9k_flop's firmware/via_con.{c,h}. */
#ifndef V9K_MGMT_CON_H
#define V9K_MGMT_CON_H

/* Bind the frozen REPL command table over the mgmt-channel response transport.
 * Call once, after tui_init(). */
void mgmt_con_init(void);

/* Drain the Victor's command bytes (with repl_key echo semantics) into the line
 * engine, then advance the channel. Call every core0 loop. */
void mgmt_con_poll(void);

#endif /* V9K_MGMT_CON_H */
