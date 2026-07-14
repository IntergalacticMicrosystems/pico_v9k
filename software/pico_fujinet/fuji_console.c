/* fuji_console.c — core-1 execution mailbox for FujiNet console ops. See header.
 *
 * Handshake (single slot): core 0 fills the arg fields, then publishes PENDING
 * behind a __dmb(); core 1's service() loads state (one volatile read when
 * idle), runs the op, writes the result buffer + success behind a __dmb(), then
 * publishes DONE; core 0 observes DONE, reads the result, and returns the slot
 * to IDLE. The __dmb() pairs order the field writes ahead of the state flag on
 * each side so the reader never sees PENDING/DONE before the data it guards.
 */
#include "fuji_console.h"
#include "fuji_blkdev.h"
#include "storage.h"
#include "pico/stdlib.h"
#include "hardware/sync.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#define FCON_IDLE       0
#define FCON_PENDING    1
#define FCON_DONE       2

#define FCON_ARG1_MAX   128     /* filename or ssid */
#define FCON_ARG2_MAX   80      /* wifi password (<=63) */
#define FCON_RESULT_MAX 2048
#define FCON_LS_MAX     40      /* cap directory listing lines */

static volatile int s_state = FCON_IDLE;
static fuji_console_op_t s_op;
static uint8_t  s_target;
static char     s_arg1[FCON_ARG1_MAX];
static char     s_arg2[FCON_ARG2_MAX];
static char     s_result[FCON_RESULT_MAX];
static volatile bool s_success;

// Bounded snprintf-append: no-ops once the buffer is full; clamps the offset so
// a truncated write can't push subsequent writes past the buffer.
static void fcon_appendf(char *buf, size_t cap, size_t *off, const char *fmt, ...) {
    if (*off >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(buf + *off, cap - *off, fmt, ap);
    va_end(ap);
    if (w > 0) *off += (size_t)w;
    if (*off > cap) *off = cap;
}

uint32_t fuji_console_op_timeout_ms(fuji_console_op_t op) {
    switch (op) {
    case FCON_PING:
    case FCON_STATUS:
    case FCON_WIFI_GET: return 5000;
    case FCON_LS:
    case FCON_MOUNT:
    case FCON_UNMOUNT:  return 10000;
    case FCON_WIFI_SET:
    case FCON_SCAN:     return 20000;
    default:            return 10000;
    }
}

static const char *backend_name(storage_backend_t b) {
    switch (b) {
    case STORAGE_BACKEND_SDCARD:  return "SD";
    case STORAGE_BACKEND_FUJINET: return "FujiNet";
    default:                      return "?";
    }
}

typedef struct {
    char  *buf;
    size_t cap;
    size_t off;
    int    count;
} ls_ctx_t;

static void ls_emit(void *vctx, const char *name, unsigned size) {
    ls_ctx_t *c = (ls_ctx_t *)vctx;
    if (c->count < FCON_LS_MAX) {
        fcon_appendf(c->buf, c->cap, &c->off, "    %-28s %u bytes\n", name, size);
    }
    c->count++;
}

// ---- core-1 op handlers: fill `out`, return success -----------------------

static bool do_ping(char *out, size_t cap) {
    bool up = false;
    bool ok = fuji_link_ping(&up);
    size_t off = 0;
    fcon_appendf(out, cap, &off, "  DRDY: %s\n", fuji_link_drdy() ? "HIGH (ESP armed)"
                                                                  : "LOW (ESP absent)");
    if (ok) {
        fcon_appendf(out, cap, &off, "  link: alive (WiFi %s)\n",
                     up ? "connected" : "disconnected");
    } else {
        fcon_appendf(out, cap, &off, "  link: no ACK from ESP\n");
    }
    return ok;
}

static bool do_status(char *out, size_t cap) {
    size_t off = 0;
    fcon_appendf(out, cap, &off, "  DRDY: %s\n", fuji_link_drdy() ? "HIGH (ESP armed)"
                                                                  : "LOW (ESP absent)");
    char ssid[33] = {0};
    bool connected = false;
    const char *werr = fuji_wifi_get(ssid, &connected);
    if (werr) {
        fcon_appendf(out, cap, &off, "  WiFi: %s\n", werr);
    } else {
        fcon_appendf(out, cap, &off, "  WiFi: %s (%s)\n",
                     ssid[0] ? ssid : "(none)",
                     connected ? "connected" : "disconnected");
    }

    fcon_appendf(out, cap, &off, "  Mounts:\n");
    int any = 0;
    for (uint8_t t = 0; t < STORAGE_MAX_TARGETS; t++) {
        storage_backend_t b = storage_get_target_backend(t);
        if (b == STORAGE_BACKEND_NONE) continue;
        any++;
        fcon_appendf(out, cap, &off, "    target %d: %-7s %s (%lu sectors)\n",
                     t, backend_name(b),
                     storage_is_mounted(t) ? "mounted" : "unmounted",
                     (unsigned long)storage_get_capacity(t));
    }
    if (!any) fcon_appendf(out, cap, &off, "    (none)\n");
    return werr == NULL;
}

static bool do_ls(char *out, size_t cap) {
    size_t off = 0;
    fcon_appendf(out, cap, &off, "  *.img on FujiNet TNFS host:\n");
    ls_ctx_t c = { out, cap, off, 0 };
    bool ok = fuji_dir_list(ls_emit, &c);
    off = c.off;
    if (ok && c.count == 0) {
        fcon_appendf(out, cap, &off, "    (none)\n");
    } else if (c.count > FCON_LS_MAX) {
        fcon_appendf(out, cap, &off, "    ... (%d more, listing capped)\n",
                     c.count - FCON_LS_MAX);
    }
    if (!ok) fcon_appendf(out, cap, &off, "    (directory read error)\n");
    return ok;
}

static bool do_mount(char *out, size_t cap) {
    size_t off = 0;
    bool ok = storage_mount_on(STORAGE_BACKEND_FUJINET, s_target, s_arg1, false);
    fcon_appendf(out, cap, &off, ok ? "  mounted '%s' on target %d\n"
                                    : "  mount '%s' on target %d FAILED\n",
                 s_arg1, s_target);
    return ok;
}

static bool do_unmount(char *out, size_t cap) {
    size_t off = 0;
    bool ok = storage_unmount(s_target);
    fcon_appendf(out, cap, &off, ok ? "  unmounted target %d\n"
                                    : "  unmount target %d FAILED\n", s_target);
    return ok;
}

static bool do_wifi_get(char *out, size_t cap) {
    size_t off = 0;
    char ssid[33] = {0};
    bool connected = false;
    const char *err = fuji_wifi_get(ssid, &connected);
    if (err) {
        fcon_appendf(out, cap, &off, "  %s\n", err);
        return false;
    }
    fcon_appendf(out, cap, &off, "  SSID:  %s\n  State: %s\n",
                 ssid[0] ? ssid : "(none)",
                 connected ? "connected" : "disconnected");
    return true;
}

static bool do_wifi_set(char *out, size_t cap) {
    size_t off = 0;
    const char *err = fuji_wifi_set(s_arg1, s_arg2);
    if (err) {
        fcon_appendf(out, cap, &off, "  %s\n", err);
        return false;
    }
    fcon_appendf(out, cap, &off, "  joined '%s'\n", s_arg1);
    return true;
}

static bool do_scan(char *out, size_t cap) {
    size_t off = 0;
    unsigned count = 0;
    const char *err = fuji_wifi_scan(&count);
    if (err) {
        fcon_appendf(out, cap, &off, "  %s\n", err);
        return false;
    }
    fcon_appendf(out, cap, &off, "  %u network(s):\n", count);
    for (unsigned i = 0; i < count && i < 32; i++) {
        char ssid[33] = {0};
        int rssi = 0;
        const char *e2 = fuji_wifi_scan_result(i, ssid, &rssi);
        if (e2) {
            fcon_appendf(out, cap, &off, "  [%u] %s\n", i, e2);
            break;
        }
        fcon_appendf(out, cap, &off, "  [%u] %-28s %d dBm\n", i, ssid, rssi);
    }
    return true;
}

void fuji_console_service(void) {
    if (s_state != FCON_PENDING) return;   // O(1) idle path: one volatile load
    __dmb();

    bool ok = false;
    s_result[0] = '\0';
    switch (s_op) {
    case FCON_PING:     ok = do_ping(s_result, FCON_RESULT_MAX);     break;
    case FCON_STATUS:   ok = do_status(s_result, FCON_RESULT_MAX);   break;
    case FCON_LS:       ok = do_ls(s_result, FCON_RESULT_MAX);       break;
    case FCON_MOUNT:    ok = do_mount(s_result, FCON_RESULT_MAX);    break;
    case FCON_UNMOUNT:  ok = do_unmount(s_result, FCON_RESULT_MAX);  break;
    case FCON_WIFI_GET: ok = do_wifi_get(s_result, FCON_RESULT_MAX); break;
    case FCON_WIFI_SET: ok = do_wifi_set(s_result, FCON_RESULT_MAX); break;
    case FCON_SCAN:     ok = do_scan(s_result, FCON_RESULT_MAX);     break;
    default:            snprintf(s_result, FCON_RESULT_MAX, "  unknown op\n"); break;
    }

    s_success = ok;
    __dmb();
    s_state = FCON_DONE;
}

void fuji_console_submit(fuji_console_op_t op, uint8_t target,
                         const char *arg1, const char *arg2, uint32_t timeout_ms) {
    // Reclaim a slot that core 1 completed after a prior timeout.
    if (s_state == FCON_DONE) s_state = FCON_IDLE;
    // A prior op that timed out may still be executing on core 1 — refuse to
    // clobber its args/result mid-run rather than trusting the user to wait.
    if (s_state != FCON_IDLE) {
        printf("fuji: previous op still running on core 1 -- try again shortly\n");
        return;
    }

    s_op = op;
    s_target = target;
    s_arg1[0] = '\0';
    s_arg2[0] = '\0';
    if (arg1) { strncpy(s_arg1, arg1, FCON_ARG1_MAX - 1); s_arg1[FCON_ARG1_MAX - 1] = '\0'; }
    if (arg2) { strncpy(s_arg2, arg2, FCON_ARG2_MAX - 1); s_arg2[FCON_ARG2_MAX - 1] = '\0'; }
    s_result[0] = '\0';
    s_success = false;

    __dmb();
    s_state = FCON_PENDING;
    __dmb();

    uint32_t waited = 0;
    while (s_state != FCON_DONE) {
        if (waited >= timeout_ms) {
            printf("fuji: timed out after %lu ms — core 1 may still be busy on "
                   "this op; wait before running further F commands\n",
                   (unsigned long)timeout_ms);
            return;   // leave PENDING: core 1 will still finish and mark DONE
        }
        sleep_ms(1);
        waited++;
    }
    __dmb();

    if (s_result[0]) printf("%s", s_result);
    printf(s_success ? "fuji: OK\n" : "fuji: FAIL\n");
    s_state = FCON_IDLE;
}
