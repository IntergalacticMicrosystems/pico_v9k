/* console_ops.c — core-1 execution mailbox for management-console ops. See the
 * header for the cross-core handshake. Generalized from the Phase B
 * pico_fujinet/fuji_console.c (same single-slot PENDING/DONE + __dmb() design
 * and refuse-while-busy guard), extended with typed result fields (ls entries,
 * peek buffer, status rows, wifi/scan) the TUI needs beyond a text blob. */
#include "console_ops.h"

#include "sd_storage.h"
#include "fuji_blkdev.h"

#include "pico/stdlib.h"
#include "hardware/sync.h"
#include <stdio.h>
#include <string.h>

#define CONOP_IDLE      0
#define CONOP_PENDING   1
#define CONOP_DONE      2

#define CONOP_ARG1_MAX  128     /* filename or ssid */
#define CONOP_ARG2_MAX  80      /* wifi password (<=63) */

/* ---- request slot (cross-core; single writer per side, ordered by __dmb) --- */
static volatile int       s_state = CONOP_IDLE;
static console_op_t       s_op;
static storage_backend_t  s_backend;
static uint8_t            s_target;
static uint32_t           s_lba;
static char               s_arg1[CONOP_ARG1_MAX];
static char               s_arg2[CONOP_ARG2_MAX];
static conop_result_t     s_res;

/* FujiNet mount names per target: the storage layer stores no display name, and
 * there is no fuji path getter (unlike sd_storage_get_image_path for SD), so we
 * track fuji: mount names here for STATUS. Updated by core 1 on mount/eject. */
static char s_fuji_name[STORAGE_MAX_TARGETS][CONOP_NAME_MAX];

/* Per-op core-1 execution budget (ms): how long core 0 blocks while core 1
 * talks to the SD card / ESP. Carried over from the Phase B mailbox. */
static uint32_t op_timeout_ms(console_op_t op) {
    switch (op) {
    case CONOP_PING:
    case CONOP_STATUS:
    case CONOP_WIFI_GET:
    case CONOP_LS_SD:
    case CONOP_PEEK:     return 5000;
    case CONOP_LS_FUJI:
    case CONOP_MOUNT:
    case CONOP_EJECT:    return 10000;
    case CONOP_WIFI_SET:
    case CONOP_SCAN:     return 20000;
    default:             return 10000;
    }
}

/* ---- core-1 op handlers: fill s_res, set s_res.ok ------------------------- */

/* ls emit: append one file entry (SD or FujiNet), capping the array. unsigned ==
 * uint32_t on this target, so this matches both sd_storage_list_images() and
 * fuji_dir_list() emit signatures. */
static void ls_emit(void *ctx, const char *name, unsigned size) {
    conop_result_t *r = (conop_result_t *)ctx;
    if (r->n_entries >= CONOP_MAX_ENTRIES) { r->entries_truncated = true; return; }
    conop_entry_t *e = &r->entries[r->n_entries++];
    snprintf(e->name, sizeof e->name, "%s", name);
    e->size = size;
}

static void do_ping(void) {
    bool up = false;
    s_res.link_alive = fuji_link_ping(&up);
    s_res.drdy = fuji_link_drdy();
    s_res.wifi_connected = up;
    s_res.ok = s_res.link_alive;
    if (!s_res.ok) snprintf(s_res.err, sizeof s_res.err, "no FujiNet");
}

static void do_ls_sd(void) {
    sd_storage_list_images(ls_emit, &s_res);
    s_res.ok = true;   /* absent SD -> zero entries, still OK (fuji-only ls) */
}

static void do_ls_fuji(void) {
    /* A failed walk (no ESP) silently lists nothing — the SD listing is still
     * valid, so ls does not ERR. */
    s_res.ok = fuji_dir_list(ls_emit, &s_res);
}

static void do_mount(void) {
    if (s_backend == STORAGE_BACKEND_FUJINET && !fuji_link_ping(NULL)) {
        s_res.ok = false;
        snprintf(s_res.err, sizeof s_res.err, "no FujiNet");
        return;
    }
    s_res.ok = storage_mount_on(s_backend, s_target, s_arg1, false);
    if (s_res.ok) {
        if (s_backend == STORAGE_BACKEND_FUJINET)
            snprintf(s_fuji_name[s_target], CONOP_NAME_MAX, "%s", s_arg1);
    } else {
        snprintf(s_res.err, sizeof s_res.err, "cannot open image (see 'ls')");
    }
}

static void do_eject(void) {
    s_res.ok = storage_unmount(s_target);
    if (s_target < STORAGE_MAX_TARGETS) s_fuji_name[s_target][0] = '\0';
    if (!s_res.ok) snprintf(s_res.err, sizeof s_res.err, "eject failed");
}

static void do_peek(void) {
    if (!storage_is_mounted(s_target) ||
        !storage_read_sector(s_target, s_lba, s_res.peek, 512)) {
        s_res.ok = false;
        snprintf(s_res.err, sizeof s_res.err, "cannot read sector");
        return;
    }
    s_res.ok = true;
}

static void do_status(void) {
    for (uint8_t t = 0; t < STORAGE_MAX_TARGETS; t++) {
        conop_status_row_t *row = &s_res.rows[t];
        row->backend  = (uint8_t)storage_get_target_backend(t);
        row->mounted  = storage_is_mounted(t);
        row->capacity = storage_get_capacity(t);
        row->name[0]  = '\0';
        if (row->backend == STORAGE_BACKEND_SDCARD) {
            const char *p = sd_storage_get_image_path(t);
            if (p) snprintf(row->name, sizeof row->name, "%s", p);
        } else if (row->backend == STORAGE_BACKEND_FUJINET) {
            snprintf(row->name, sizeof row->name, "%s", s_fuji_name[t]);
        }
    }
    s_res.ok = true;
}

static void do_wifi_get(void) {
    bool connected = false;
    const char *err = fuji_wifi_get(s_res.ssid, &connected);
    if (err) { s_res.ok = false; snprintf(s_res.err, sizeof s_res.err, "%s", err); return; }
    s_res.wifi_connected = connected;
    s_res.ok = true;
}

static void do_wifi_set(void) {
    const char *err = fuji_wifi_set(s_arg1, s_arg2);
    if (err) { s_res.ok = false; snprintf(s_res.err, sizeof s_res.err, "%s", err); return; }
    s_res.ok = true;
}

static void do_scan(void) {
    unsigned count = 0;
    const char *err = fuji_wifi_scan(&count);
    if (err) { s_res.ok = false; snprintf(s_res.err, sizeof s_res.err, "%s", err); return; }
    for (unsigned i = 0; i < count && s_res.n_scan < CONOP_MAX_SCAN; i++) {
        int rssi = 0;
        const char *e2 = fuji_wifi_scan_result(i, s_res.scan_ssid[s_res.n_scan], &rssi);
        if (e2) break;
        s_res.scan_rssi[s_res.n_scan] = rssi;
        s_res.n_scan++;
    }
    s_res.ok = true;
}

void console_op_service(void) {
    if (s_state != CONOP_PENDING) return;   /* O(1) idle path: one volatile load */
    __dmb();

    memset(&s_res, 0, sizeof s_res);
    switch (s_op) {
    case CONOP_PING:     do_ping();     break;
    case CONOP_LS_SD:    do_ls_sd();    break;
    case CONOP_LS_FUJI:  do_ls_fuji();  break;
    case CONOP_MOUNT:    do_mount();    break;
    case CONOP_EJECT:    do_eject();    break;
    case CONOP_PEEK:     do_peek();     break;
    case CONOP_WIFI_GET: do_wifi_get(); break;
    case CONOP_WIFI_SET: do_wifi_set(); break;
    case CONOP_SCAN:     do_scan();     break;
    case CONOP_STATUS:   do_status();   break;
    default:             s_res.ok = false;
                         snprintf(s_res.err, sizeof s_res.err, "unknown op"); break;
    }

    __dmb();
    s_state = CONOP_DONE;
}

const conop_result_t *console_op_submit(console_op_t op, storage_backend_t backend,
                                        uint8_t target, uint32_t lba,
                                        const char *arg1, const char *arg2) {
    /* Reclaim a slot core 1 completed after a prior timeout. */
    if (s_state == CONOP_DONE) s_state = CONOP_IDLE;
    /* A prior op that timed out may still be executing on core 1 — refuse to
     * clobber its args/result mid-run rather than trusting the caller to wait. */
    if (s_state != CONOP_IDLE) return NULL;

    s_op      = op;
    s_backend = backend;
    s_target  = target;
    s_lba     = lba;
    s_arg1[0] = '\0';
    s_arg2[0] = '\0';
    if (arg1) { strncpy(s_arg1, arg1, CONOP_ARG1_MAX - 1); s_arg1[CONOP_ARG1_MAX - 1] = '\0'; }
    if (arg2) { strncpy(s_arg2, arg2, CONOP_ARG2_MAX - 1); s_arg2[CONOP_ARG2_MAX - 1] = '\0'; }

    __dmb();
    s_state = CONOP_PENDING;
    __dmb();

    uint32_t timeout_ms = op_timeout_ms(op);
    uint32_t waited = 0;
    while (s_state != CONOP_DONE) {
        if (waited >= timeout_ms) {
            /* Leave PENDING: core 1 will still finish and mark DONE; the next
             * submit reclaims the slot. Caller sees NULL = "no result". */
            return NULL;
        }
        sleep_ms(1);
        waited++;
    }
    __dmb();
    s_state = CONOP_IDLE;
    return &s_res;
}
