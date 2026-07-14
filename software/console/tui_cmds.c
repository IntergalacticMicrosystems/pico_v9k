/* tui_cmds.c — REPL command layer on the mtui line engine. See tui_cmds.h.
 *
 * Ported from v9k_flop/firmware/tui_cmds.c. The reply text is a frozen bench API
 * (scripts read lines until OK/ERR): every REPL reply ends with a line "OK" or
 * "ERR <reason>". Output goes through mtui_line_write/printf, which translate
 * '\n' to CRLF — hence handlers write "...\n".
 *
 * Adaptations for the DMA card: 8 SASI targets (no floppy SS/DS geometry), and
 * every storage/SPI touch is deferred to core 1 through the console-ops mailbox
 * (console_ops.h). The pure GPIO/memory getters used by `diag` (fuji_link_drdy,
 * storage_get_target_backend) touch no SPI and run on core 0 directly, so `diag`
 * stays a reliable escape hatch even if core 1 is wedged. There is no
 * create/copy/bootmount/host here (deferred; our fuji_blkdev has no TNFS-host
 * setter). */
#include "tui_cmds.h"
#include "console_ops.h"
#include "storage.h"
#include "fuji_blkdev.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

/* dma_board.c: dump captured HardFault info to the diag UART (memory-only). */
void dma_board_dump_fault_info(void);
#include <stdlib.h>

/* ---- helpers ------------------------------------------------------------- */

static const char *backend_name(uint8_t b) {
    switch (b) {
    case STORAGE_BACKEND_SDCARD:  return "sd";
    case STORAGE_BACKEND_FUJINET: return "fuji";
    default:                      return "-";
    }
}

/* Display name for a mounted target: SD names get an "sd:" prefix, FujiNet names
 * "fuji:"; an empty name renders as "-". (v9k_flop disp_name convention.) */
static const char *disp_name(uint8_t backend, const char *name,
                             char *buf, unsigned cap) {
    if (!name || !name[0]) return "-";
    const char *pfx = (backend == STORAGE_BACKEND_FUJINET) ? "fuji:" : "sd:";
    snprintf(buf, cap, "%s%s", pfx, name);
    return buf;
}

/* Parse a "0".."7" SASI target token; -1 if malformed / out of range. */
static int parse_target(const char *s) {
    if (!s || !s[0] || s[1] || s[0] < '0' || s[0] > '7') return -1;
    return s[0] - '0';
}

/* One `status` row for a target with a backend. */
static void status_row(mtui_line_t *ln, int t, const conop_status_row_t *row) {
    char nb[CONOP_NAME_MAX + 8];
    mtui_line_printf(ln, "%d %s %s %s %lu\n", t,
                     disp_name(row->backend, row->name, nb, sizeof nb),
                     backend_name(row->backend),
                     row->mounted ? "mounted" : "unmounted",
                     (unsigned long)row->capacity);
}

/* ---- command handlers ---------------------------------------------------- */

static void cmd_help(mtui_line_t *ln, int argc, char **argv) {
    (void)argc; (void)argv;
    mtui_line_write(ln,
        "Commands (every reply ends with a line 'OK' or 'ERR <reason>'):\n"
        "  ls                 list images: 'sd:<name> <bytes>' then 'fuji:<name> <bytes>'\n"
        "  status             per target 0-7 with a backend: '<t> <name> <backend> <state> <sectors>'\n"
        "  mount <t> <[sd:|fuji:]file>  insert an image on SASI target t (bare name = sd:)\n"
        "  eject <t>          leave SASI target t empty\n"
        "  peek <t> <lba>     hex-dump one 512-byte sector of a mounted target\n"
        "  wifi               ESP32 WiFi: '<ssid|-> <connected|disconnected>'\n"
        "  wifi scan          list visible networks: '<rssi-dBm> <ssid>' per line\n"
        "  wifi <ssid> <password>  join + persist (SSID may contain spaces; the password can't)\n"
        "  diag               diag-UART liveness ping + DRDY + per-target backends\n"
        "  menu               full-screen menu for humans (q returns)\n"
        "  help               this text\n"
        "OK\n");
}

static void cmd_ls(mtui_line_t *ln, int argc, char **argv) {
    (void)argc; (void)argv;
    /* SD images first, then FujiNet — matching the sd:/fuji: namespace. A busy
     * mailbox or an absent backend simply contributes no rows (no ERR): the
     * other half's listing is still valid. */
    const conop_result_t *r = console_op_submit(CONOP_LS_SD, 0, 0, 0, NULL, NULL);
    if (r)
        for (int i = 0; i < r->n_entries; i++)
            mtui_line_printf(ln, "sd:%s %lu\n", r->entries[i].name,
                             (unsigned long)r->entries[i].size);
    r = console_op_submit(CONOP_LS_FUJI, 0, 0, 0, NULL, NULL);
    if (r)
        for (int i = 0; i < r->n_entries; i++)
            mtui_line_printf(ln, "fuji:%s %lu\n", r->entries[i].name,
                             (unsigned long)r->entries[i].size);
    mtui_line_write(ln, "OK\n");
}

static void cmd_status(mtui_line_t *ln, int argc, char **argv) {
    (void)argc; (void)argv;
    const conop_result_t *r = console_op_submit(CONOP_STATUS, 0, 0, 0, NULL, NULL);
    if (!r) { mtui_line_write(ln, "ERR busy (core 1)\n"); return; }
    for (int t = 0; t < STORAGE_MAX_TARGETS; t++)
        if (r->rows[t].backend != STORAGE_BACKEND_NONE)
            status_row(ln, t, &r->rows[t]);
    mtui_line_write(ln, "OK\n");
}

static void cmd_mount_eject(mtui_line_t *ln, int argc, char **argv) {
    bool is_mount = (argv[0][0] == 'm');
    int t = parse_target(argc > 1 ? argv[1] : NULL);
    if (t < 0) { mtui_line_write(ln, "ERR target must be 0-7\n"); return; }

    if (!is_mount) {
        const conop_result_t *r = console_op_submit(CONOP_EJECT, 0, (uint8_t)t,
                                                    0, NULL, NULL);
        if (!r)            mtui_line_write(ln, "ERR busy (core 1)\n");
        else if (r->ok)    mtui_line_write(ln, "OK\n");
        else               mtui_line_printf(ln, "ERR %s\n", r->err);
        return;
    }

    const char *name = argc > 2 ? argv[2] : NULL;
    if (!name) { mtui_line_write(ln, "ERR usage: mount <t> <[sd:|fuji:]file>\n"); return; }

    /* Prefix parse exactly like v9k_flop: fuji: -> FujiNet backend; sd:/bare -> SD. */
    storage_backend_t backend;
    const char *img;
    if (!strncmp(name, "fuji:", 5)) { backend = STORAGE_BACKEND_FUJINET; img = name + 5; }
    else if (!strncmp(name, "sd:", 3)) { backend = STORAGE_BACKEND_SDCARD; img = name + 3; }
    else                               { backend = STORAGE_BACKEND_SDCARD; img = name; }
    if (!*img) { mtui_line_write(ln, "ERR usage: mount <t> <[sd:|fuji:]file>\n"); return; }

    const conop_result_t *r = console_op_submit(CONOP_MOUNT, backend, (uint8_t)t,
                                                0, img, NULL);
    if (!r)            mtui_line_write(ln, "ERR busy (core 1)\n");
    else if (r->ok)    mtui_line_write(ln, "OK\n");
    else               mtui_line_printf(ln, "ERR %s\n", r->err);
}

static void cmd_peek(mtui_line_t *ln, int argc, char **argv) {
    int t = parse_target(argc > 1 ? argv[1] : NULL);
    const char *lba_s = argc > 2 ? argv[2] : NULL;
    if (t < 0 || !lba_s) { mtui_line_write(ln, "ERR usage: peek <t> <lba>\n"); return; }
    const conop_result_t *r = console_op_submit(CONOP_PEEK, 0, (uint8_t)t,
                                                (uint32_t)strtoul(lba_s, NULL, 0),
                                                NULL, NULL);
    if (!r)        { mtui_line_write(ln, "ERR busy (core 1)\n"); return; }
    if (!r->ok)    { mtui_line_printf(ln, "ERR %s\n", r->err); return; }
    for (int row = 0; row < 512; row += 16) {
        mtui_line_printf(ln, "%03X:", row);
        for (int j = 0; j < 16; j++) mtui_line_printf(ln, " %02X", r->peek[row + j]);
        mtui_line_write(ln, "\n");
    }
    mtui_line_write(ln, "OK\n");
}

static void cmd_wifi(mtui_line_t *ln, int argc, char **argv) {
    /* `wifi` -> status; `wifi scan` -> network list; `wifi <ssid...> <pass>` ->
     * join. The line engine collapses whitespace, so an SSID with interior
     * spaces is argv[1..argc-2] rejoined with single spaces; the password is
     * always the last token (so it can't itself contain a space). */
    if (argc == 1) {
        const conop_result_t *r = console_op_submit(CONOP_WIFI_GET, 0, 0, 0, NULL, NULL);
        if (!r)     { mtui_line_write(ln, "ERR busy (core 1)\n"); return; }
        if (!r->ok) { mtui_line_printf(ln, "ERR %s\n", r->err); return; }
        mtui_line_printf(ln, "%s %s\n", r->ssid[0] ? r->ssid : "-",
                         r->wifi_connected ? "connected" : "disconnected");
        mtui_line_write(ln, "OK\n");
    } else if (argc == 2 && !strcasecmp(argv[1], "scan")) {
        const conop_result_t *r = console_op_submit(CONOP_SCAN, 0, 0, 0, NULL, NULL);
        if (!r)     { mtui_line_write(ln, "ERR busy (core 1)\n"); return; }
        if (!r->ok) { mtui_line_printf(ln, "ERR %s\n", r->err); return; }
        for (int i = 0; i < r->n_scan; i++)
            mtui_line_printf(ln, "%d %s\n", r->scan_rssi[i], r->scan_ssid[i]);
        mtui_line_write(ln, "OK\n");
    } else if (argc >= 3) {
        char ssid[64]; size_t n = 0;
        for (int i = 1; i < argc - 1 && n < sizeof(ssid) - 1; i++) {
            int w = snprintf(ssid + n, sizeof(ssid) - n, "%s%s",
                             i > 1 ? " " : "", argv[i]);
            if (w < 0) break;
            n += (size_t)w;
            if (n >= sizeof(ssid)) n = sizeof(ssid) - 1;  /* clamp on truncation */
        }
        const conop_result_t *r = console_op_submit(CONOP_WIFI_SET, 0, 0, 0,
                                                    ssid, argv[argc - 1]);
        if (!r)         mtui_line_write(ln, "ERR busy (core 1)\n");
        else if (r->ok) mtui_line_write(ln, "OK\n");
        else            mtui_line_printf(ln, "ERR %s\n", r->err);
    } else {
        mtui_line_write(ln, "ERR usage: wifi | wifi scan | wifi <ssid> <password>\n");
    }
}

/* Emit a known line to the diag UART (printf routes there) plus live state, then
 * reply OK on the console. Reads only GPIO (fuji_link_drdy) and memory getters
 * (storage_get_target_backend) — no SPI — so it works from core 0 even when the
 * core-1 storage worker is wedged (the escape hatch that replaced the old 'F'
 * bench shell and single-char debug console). */
static void cmd_diag(mtui_line_t *ln, int argc, char **argv) {
    (void)argc; (void)argv;
    bool drdy = fuji_link_drdy();
    printf("diag: console liveness ping (FujiNet DRDY %s)\n", drdy ? "HIGH" : "LOW");
    dma_board_dump_fault_info();    /* to the diag UART; memory-only read */
    mtui_line_printf(ln, "DRDY %s\n", drdy ? "high" : "low");
    for (int t = 0; t < STORAGE_MAX_TARGETS; t++) {
        storage_backend_t b = storage_get_target_backend((uint8_t)t);
        if (b != STORAGE_BACKEND_NONE)
            mtui_line_printf(ln, "%d %s %s\n", t, backend_name((uint8_t)b),
                             storage_is_mounted((uint8_t)t) ? "mounted" : "unmounted");
    }
    mtui_line_write(ln, "OK\n");
}

/* `menu` only requests the switch; the firmware glue reacts to ln->done by
 * entering the full-screen TUI. mtui suppresses the post-dispatch prompt. */
static void cmd_menu(mtui_line_t *ln, int argc, char **argv) {
    (void)argc; (void)argv;
    ln->done = true;
}

static void on_unknown(mtui_line_t *ln, const char *name) {
    (void)name;
    mtui_line_write(ln, "ERR unknown command (try 'help')\n");
}

static const mtui_line_cmd_t k_cmds[] = {
    { "help",   NULL, cmd_help },
    { "ls",     NULL, cmd_ls },
    { "status", NULL, cmd_status },
    { "mount",  NULL, cmd_mount_eject },
    { "eject",  NULL, cmd_mount_eject },
    { "peek",   NULL, cmd_peek },
    { "wifi",   NULL, cmd_wifi },
    { "diag",   NULL, cmd_diag },
    { "menu",   NULL, cmd_menu },
};

void tui_cmds_bind(mtui_line_t *ln, mtui_transport_t *tp) {
    mtui_line_init(ln, tp, k_cmds, (int)(sizeof k_cmds / sizeof k_cmds[0]));
    ln->prompt      = "v9k> ";
    ln->no_builtins = true;         /* the frozen table owns the whole surface */
    ln->on_unknown  = on_unknown;
}

void tui_repl_banner(mtui_line_t *ln) {
    mtui_line_write(ln,
        "\nVictor 9000 DMA hard-disk emulator — disk control console\n"
        "Type 'help' for commands, 'menu' for the interactive menu.\n");
    const conop_result_t *r = console_op_submit(CONOP_STATUS, 0, 0, 0, NULL, NULL);
    if (r)
        for (int t = 0; t < STORAGE_MAX_TARGETS; t++)
            if (r->rows[t].backend != STORAGE_BACKEND_NONE)
                status_row(ln, t, &r->rows[t]);
    mtui_line_write(ln, ln->prompt);
}
