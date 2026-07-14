/* console_ops.h — core-1 execution mailbox for management-console operations.
 *
 * Generalized from pico_fujinet/fuji_console.{c,h} (Phase B). ALL storage / SPI1
 * work on this board lives on core 1 (SD + FujiNet share the SPI1 bus, and the
 * SD driver does not take the FujiNet bus mutex), so the CDC console handlers on
 * core 0 must NOT call storage_ / fuji_ functions that touch the bus directly.
 * Instead each
 * handler fills a single request slot and hands the op to core 1, which runs it
 * and fills a typed result buffer.
 *
 * Handshake (single slot, single outstanding request): core 0 fills the arg
 * fields, publishes PENDING behind a __dmb(); core 1's service() runs the op,
 * writes the result behind a __dmb(), publishes DONE; core 0 observes DONE and
 * returns the slot to IDLE. The __dmb() pairs order the field writes ahead of the
 * state flag so a reader never sees PENDING/DONE before the data it guards.
 *
 * Pure state getters that read only memory (storage_get_target_backend,
 * storage_is_mounted, storage_get_capacity) touch no SPI and may be called
 * from core 0 directly — those do not need this mailbox.
 */
#ifndef V9K_CONSOLE_OPS_H
#define V9K_CONSOLE_OPS_H

#include <stdint.h>
#include <stdbool.h>

#include "storage.h"   /* storage_backend_t, STORAGE_MAX_TARGETS */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CONOP_PING = 0,     /* FujiNet link liveness + DRDY */
    CONOP_LS_SD,        /* list SD *.img -> entries[] */
    CONOP_LS_FUJI,      /* list FujiNet *.img -> entries[] */
    CONOP_MOUNT,        /* backend + target + arg1=name */
    CONOP_EJECT,        /* target */
    CONOP_PEEK,         /* target + lba -> peek[512] */
    CONOP_WIFI_GET,     /* -> ssid + wifi_connected */
    CONOP_WIFI_SET,     /* arg1=ssid, arg2=password */
    CONOP_SCAN,         /* -> scan_ssid[]/scan_rssi[] */
    CONOP_STATUS,       /* per-target backend/mounted/capacity/name -> rows[] */
} console_op_t;

#define CONOP_MAX_ENTRIES 32
#define CONOP_MAX_SCAN    16
#define CONOP_NAME_MAX    64
#define CONOP_ERR_MAX     64

/* One image-file listing entry (LS_SD / LS_FUJI). name is the bare file name
 * (no sd:/fuji: prefix); size is the byte length. */
typedef struct {
    char     name[CONOP_NAME_MAX];
    uint32_t size;
} conop_entry_t;

/* One STATUS row per SASI target. name is the raw image name (no prefix) — the
 * console renders it with an sd:/fuji: prefix per backend. */
typedef struct {
    uint8_t  backend;   /* storage_backend_t */
    bool     mounted;
    uint32_t capacity;  /* sectors */
    char     name[CONOP_NAME_MAX];
} conop_status_row_t;

/* Typed result buffer, filled by core 1. Only the fields relevant to the op are
 * meaningful. Valid from the console_op_submit() return until the NEXT submit. */
typedef struct {
    bool ok;
    char err[CONOP_ERR_MAX];        /* short reason on failure (for ERR replies) */

    conop_entry_t entries[CONOP_MAX_ENTRIES];   /* LS_SD / LS_FUJI */
    int  n_entries;
    bool entries_truncated;

    uint8_t peek[512];              /* PEEK */

    conop_status_row_t rows[STORAGE_MAX_TARGETS];   /* STATUS */

    char ssid[33];                  /* WIFI_GET */
    bool wifi_connected;

    int  n_scan;                    /* SCAN */
    char scan_ssid[CONOP_MAX_SCAN][33];
    int  scan_rssi[CONOP_MAX_SCAN];

    bool drdy;                      /* PING */
    bool link_alive;
} conop_result_t;

/* Core 0: fill the request slot and block (bounded by the op's Phase B timeout)
 * until core 1 completes it. Returns a pointer to the shared result buffer on
 * completion, or NULL if the mailbox is busy with a prior op or the op timed out.
 * The returned pointer is valid only until the next console_op_submit() call.
 * arg1/arg2 may be NULL; backend/target/lba are ignored by ops that don't use
 * them. */
const conop_result_t *console_op_submit(console_op_t op, storage_backend_t backend,
                                        uint8_t target, uint32_t lba,
                                        const char *arg1, const char *arg2);

/* Core 1: run a pending request, if any. O(1) when idle (single volatile load).
 * Call from the core-1 defer worker loop (defer_worker_main). */
void console_op_service(void);

#ifdef __cplusplus
}
#endif

#endif /* V9K_CONSOLE_OPS_H */
