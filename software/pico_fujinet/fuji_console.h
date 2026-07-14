/* fuji_console.h
 * Core-1 execution mailbox for console-initiated FujiNet operations.
 *
 * Why a mailbox: ALL SPI1 traffic must run on core 1. The SD driver does not
 * take the FujiNet bus mutex, so a core-0 FujiNet transaction could interleave
 * with an in-flight SD burst on the shared bus. The 'F' console shell runs on
 * core 0; it hands each op to core 1 through a single request slot and blocks
 * until core 1 has run it and filled the result buffer.
 *
 * Single-slot, single-outstanding-request: the shell submits one op at a time.
 */
#ifndef V9K_FUJI_CONSOLE_H
#define V9K_FUJI_CONSOLE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FCON_PING = 0,
    FCON_STATUS,
    FCON_LS,
    FCON_MOUNT,     // arg1 = filename, target = SASI target
    FCON_UNMOUNT,   // target = SASI target
    FCON_WIFI_GET,
    FCON_WIFI_SET,  // arg1 = ssid, arg2 = password
    FCON_SCAN,
} fuji_console_op_t;

// Recommended core-1 execution budget for an op (ms): how long the submitter
// must be willing to block while core 1 talks to the ESP. Cross-core contract.
uint32_t fuji_console_op_timeout_ms(fuji_console_op_t op);

// Core 0: fill the request slot, block until core 1 completes it (or timeout),
// then print the result buffer + pass/fail. On timeout, warn that core 1 may
// still be busy and further F commands should wait. arg1/arg2 may be NULL.
void fuji_console_submit(fuji_console_op_t op, uint8_t target,
                         const char *arg1, const char *arg2, uint32_t timeout_ms);

// Core 1: run a pending request, if any. O(1) when idle (single volatile load).
// Call from the core-1 defer worker loop.
void fuji_console_service(void);

#ifdef __cplusplus
}
#endif

#endif // V9K_FUJI_CONSOLE_H
