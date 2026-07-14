/* tui.c — management console on the card's USB port. See tui.h.
 *
 * Two modes over one CDC-ACM interface:
 *   REPL (default) — line commands with machine-parseable OK/ERR replies.
 *   TUI            — full-screen menu + image picker (arrow keys), built on the
 *                    micro_tui widget toolkit (desktop-filling frameless windows).
 *
 * Poll-driven: tui_poll() pumps tud_task(); in REPL it drains input a byte at a
 * time (local echo + the mtui line engine); in TUI it steps the mtui app
 * (mtui_app_poll draws the diff then dispatches keys). Writes drop when no host
 * terminal is attached (DTR down) so an unplugged port costs nothing.
 *
 * Ported from v9k_flop/firmware/tui.c. Adaptations for the DMA card: 8 SASI
 * targets (no floppy SS/DS geometry), cell buffers in static SRAM (no PSRAM),
 * all storage/SPI work deferred to core 1 via the console-ops mailbox
 * (console_ops.h), and the create/copy/new-disk/host screens dropped. The menu
 * single-owner (CDC vs channel) logic is kept for Stage 2; only the CDC owner is
 * exercised here. */
#include "tui.h"
#include "tui_cmds.h"
#include "console_ops.h"
#include "storage.h"
#include "mtui/line.h"
#include "mtui/platform/pico_cdc.h"
#include "mtui/widget.h"
#include "tusb.h"
#include "pico/stdlib.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

/* Line-mode engine + its CDC transport (itf 0). The REPL command table and
 * banner live in tui_cmds.c; this file drives the engine and the full-screen
 * screens over the same CDC interface. */
static mtui_line_t      s_ln;
static mtui_transport_t s_tp;
static mtui_pico_cdc_t  s_cdc;

/* ---- CDC output --------------------------------------------------------- */

/* Write all of buf, pumping the USB task while the FIFO drains. Bounded: gives
 * up after 100 ms so a wedged host can't stall the main loop. */
static void cdc_write(const char *buf, unsigned len)
{
    if (!tud_cdc_connected()) return;
    absolute_time_t deadline = make_timeout_time_ms(100);
    while (len) {
        unsigned n = tud_cdc_write(buf, len);
        buf += n; len -= n;
        tud_cdc_write_flush();
        tud_task();
        if (time_reached(deadline)) return;
    }
    tud_cdc_write_flush();
}

static void cdc_puts(const char *s) { cdc_write(s, strlen(s)); }

/* ---- ANSI helpers ------------------------------------------------------- */
/* The full-screen screens are painted by the mtui terminal writer; only the
 * REPL<->TUI transitions still emit raw escapes (clear + cursor visibility). */

#define A_CLEAR   "\x1b[2J\x1b[H"
#define A_SHOWCUR "\x1b[?25h"
#define A_SGRRST  "\x1b[0m"          /* the last diff-painted cell is usually the
                                      * reverse-video selection; reset on exit */

/* ---- key decoding (non-blocking escape-sequence state machine) ---------- */
/* REPL only: the mtui line engine takes raw bytes, so the REPL decodes just the
 * keys it cares about (Enter/backspace/printables). The TUI screens decode via
 * mtui_input instead. */

enum { KEY_NONE = -1, KEY_UP = 1000, KEY_DOWN, KEY_ENTER, KEY_ESC };

static int      s_esc;              /* 0=idle 1=got ESC 2=got ESC [ */
static uint32_t s_esc_ms;           /* when the pending ESC arrived  */

/* Feed one raw byte; returns a key code or KEY_NONE if mid-sequence. */
static int key_feed(uint8_t ch)
{
    if (s_esc == 1) {
        if (ch == '[') { s_esc = 2; return KEY_NONE; }
        s_esc = 0;
        return KEY_ESC;             /* ESC followed by a non-CSI byte */
    }
    if (s_esc == 2) {
        s_esc = 0;
        if (ch == 'A') return KEY_UP;
        if (ch == 'B') return KEY_DOWN;
        return KEY_NONE;            /* other CSI final bytes: ignore */
    }
    if (ch == 0x1b) { s_esc = 1; s_esc_ms = to_ms_since_boot(get_absolute_time()); return KEY_NONE; }
    if (ch == '\r' || ch == '\n') return KEY_ENTER;
    return ch;
}

/* A bare ESC keypress has no follow-up byte; time it out into KEY_ESC. */
static int key_timeout(void)
{
    if (s_esc == 1 &&
        to_ms_since_boot(get_absolute_time()) - s_esc_ms > 30) {
        s_esc = 0;
        return KEY_ESC;
    }
    return KEY_NONE;
}

/* ---- shared state ------------------------------------------------------- */

enum { M_REPL, M_TUI };
static int  s_mode = M_REPL;
static bool s_connected;

/* Who owns the single full-screen engine while s_mode == M_TUI. OWNER_CDC is the
 * local USB console; OWNER_VIA is the Victor-side console (Stage 2), which drives
 * the same screen/app over its own transport (s_via_tp). Reset to CDC on exit. */
enum { OWNER_CDC, OWNER_VIA };
static int               s_tui_owner = OWNER_CDC;
static mtui_transport_t *s_via_tp;   /* the borrowing channel transport, while it owns */

static char s_msg[80];              /* result line shown on the main menu */

/* ---- TUI: mtui app + screens -------------------------------------------- */
/* One frameless, desktop-filling window is visible at a time; its controls are
 * rebuilt on every screen change. The listboxes emit a command id on Enter
 * (routed by on_cmd), and the per-screen key hooks (on_key) handle q/ESC/eject
 * and the password submit. Everything runs off the same CDC transport as the
 * REPL, driven non-blocking by mtui_app_poll from tui_poll. */

/* The 80x25 cell buffers (front+back = 16 KB) live in static SRAM on this card
 * (no PSRAM). CPU/core-0 access only (as this poll path is). */
static mtui_cell_t   s_cells[2 * 80 * 25];
static int16_t       s_dlo[25], s_dhi[25];
static mtui_screen_t s_scr;
static bool          s_tui_ok;
static mtui_term_t   s_term;
static uint8_t       s_term_buf[1024];
static mtui_app_t    s_app;
static mtui_window_t s_win;

/* Command ids emitted by the per-screen listboxes on Enter. */
enum { CMD_MENU = 1, CMD_PICK, CMD_FUJI, CMD_FUJIPICK };
/* Which screen the single window currently holds (drives on_key). */
enum { SC_MENU, SC_PICK, SC_INPUT, SC_FUJI, SC_FUJISCAN, SC_FUJIPICK };
static int s_screen;

/* Header labels (title/separator/heading) shared by every screen. */
static mtui_label_t s_lbl_title, s_lbl_sep, s_lbl_heading, s_lbl_msg, s_lbl_name;
static char s_heading[96];

/* Main menu: target 0 + target 1 rows + FujiNet setup / command mode / exit.
 * Target rows are rendered from a status snapshot taken when the menu is built. */
#define MENU_ROWS 5
#define MENU_TARGETS 2                    /* rows 0..1 map to SASI targets 0..1 */
static mtui_listbox_t s_menu_lb;
static char        s_menu_row[MENU_ROWS][80];
static const char *s_menu_ptr[MENU_ROWS];
static conop_status_row_t s_srows[STORAGE_MAX_TARGETS];   /* cached status snapshot */

/* Picker: (eject/cancel) + SD images + FujiNet remote files. Parallel arrays
 * carry each row's backend + bare name so mount needs no string re-parse. */
#define PICK_MAX (1 + CONOP_MAX_ENTRIES + CONOP_MAX_ENTRIES)
static mtui_listbox_t s_pick_lb;
static char        s_pick_row[PICK_MAX][80];
static const char *s_pick_ptr[PICK_MAX];
static storage_backend_t s_pick_backend[PICK_MAX];   /* NONE for the eject row */
static char        s_pick_name[PICK_MAX][CONOP_NAME_MAX];
static int         s_pick_n;
static int         s_pick_target;    /* which SASI target the picker mounts into */

/* Password entry (join a scanned network). */
static mtui_input_line_t s_il;
static char s_input[80];

/* FujiNet setup screen: one info line + a 2-row action listbox. */
static mtui_label_t   s_lbl_fwifi;
static char           s_fuji_wifi[80];
static mtui_listbox_t s_fuji_lb;
static const char    *s_fuji_ptr[2];

/* Scan picker: (cancel) + up to CONOP_MAX_SCAN networks. Raw SSIDs are kept for
 * the join step; the rows carry the '<ssid> (<rssi> dBm)' display strings. */
static mtui_listbox_t s_scan_lb;
static char        s_scan_row[CONOP_MAX_SCAN + 1][48];
static const char *s_scan_ptr[CONOP_MAX_SCAN + 1];
static char        s_scan_ssid[CONOP_MAX_SCAN][33];
static int         s_scan_n;
static char        s_fuji_ssid[33];   /* the chosen SSID awaiting a password */

static void build_menu(int sel);
static void build_input(void);
static void build_fuji_setup(void);
static void build_fuji_scan(void);

static uint32_t tui_now_ms(void)
{
    return to_ms_since_boot(get_absolute_time());
}

/* sd:/fuji: display name for a mounted target (empty -> "-"). */
static const char *disp_name(uint8_t backend, const char *name,
                             char *buf, unsigned cap)
{
    if (!name || !name[0]) return "-";
    const char *pfx = (backend == STORAGE_BACKEND_FUJINET) ? "fuji:" : "sd:";
    snprintf(buf, cap, "%s%s", pfx, name);
    return buf;
}

/* Start a fresh screen: a plain (default-colour, frameless) full-window with the
 * standard title/separator/heading block. Callers add the screen's controls. */
static void win_begin(const char *heading)
{
    mtui_window_init(&s_win, 0, 0, 80, 25, NULL);
    s_win.has_frame = false; s_win.shadow = false;
    s_win.fg = MTUI_DEFAULT_COLOR; s_win.bg = MTUI_DEFAULT_COLOR;
    snprintf(s_heading, sizeof s_heading, "%s", heading);
    mtui_label_init(&s_lbl_title, 1, 0,
                    "Victor 9000 DMA Hard-Disk Emulator - Disk Control");
    mtui_label_init(&s_lbl_sep, 1, 1,
                    "------------------------------------------------------------");
    mtui_label_init(&s_lbl_heading, 1, 2, s_heading);
    mtui_window_add(&s_win, &s_lbl_title.base);
    mtui_window_add(&s_win, &s_lbl_sep.base);
    mtui_window_add(&s_win, &s_lbl_heading.base);
}

/* Render one target row from the cached status snapshot: name + backend +
 * mounted state (or "(empty)"). */
static void menu_row_target(int i)
{
    const conop_status_row_t *row = &s_srows[i];
    if (row->backend != STORAGE_BACKEND_NONE) {
        char nb[CONOP_NAME_MAX + 8];
        snprintf(s_menu_row[i], sizeof s_menu_row[i],
                 "Target %d:  %s  [%s, %s, %lu sec]", i,
                 disp_name(row->backend, row->name, nb, sizeof nb),
                 row->backend == STORAGE_BACKEND_FUJINET ? "fuji" : "sd",
                 row->mounted ? "mounted" : "unmounted",
                 (unsigned long)row->capacity);
    } else {
        snprintf(s_menu_row[i], sizeof s_menu_row[i], "Target %d:  (empty)", i);
    }
}

static void build_menu(int sel)
{
    /* One status snapshot for the whole menu (mailbox op; bounded). */
    const conop_result_t *r = console_op_submit(CONOP_STATUS, 0, 0, 0, NULL, NULL);
    if (r) memcpy(s_srows, r->rows, sizeof s_srows);
    else   memset(s_srows, 0, sizeof s_srows);

    win_begin("Main menu");
    for (int i = 0; i < MENU_TARGETS; i++) menu_row_target(i);
    snprintf(s_menu_row[2], sizeof s_menu_row[2], "FujiNet setup...");
    snprintf(s_menu_row[3], sizeof s_menu_row[3], "Switch to command mode");
    snprintf(s_menu_row[4], sizeof s_menu_row[4], "Exit to boot");
    for (int i = 0; i < MENU_ROWS; i++) s_menu_ptr[i] = s_menu_row[i];
    mtui_listbox_init(&s_menu_lb, 1, 4, 60, MENU_ROWS, s_menu_ptr, MENU_ROWS);
    s_menu_lb.base.id = CMD_MENU;
    s_menu_lb.sel = (sel < 0 || sel >= MENU_ROWS) ? 0 : sel;
    mtui_window_add(&s_win, &s_menu_lb.base);
    mtui_label_init(&s_lbl_msg, 1, 12, s_msg);   /* result line; empty => blank */
    mtui_window_add(&s_win, &s_lbl_msg.base);
    s_app.status = "Up/Down move   Enter select   E eject   Q exit";
    s_screen = SC_MENU;
}

/* Copy one half of the picker (SD or FujiNet) from a fresh listing op. The
 * result pointer is valid only until the next submit, so entries are copied out
 * immediately (SD then FujiNet). */
static void pick_add_entries(console_op_t op, storage_backend_t backend,
                             const char *pfx)
{
    const conop_result_t *r = console_op_submit(op, 0, 0, 0, NULL, NULL);
    if (!r) return;
    for (int i = 0; i < r->n_entries && s_pick_n < PICK_MAX; i++) {
        s_pick_backend[s_pick_n] = backend;
        snprintf(s_pick_name[s_pick_n], CONOP_NAME_MAX, "%s", r->entries[i].name);
        snprintf(s_pick_row[s_pick_n], 80, "%s%-30s  %lu", pfx,
                 r->entries[i].name, (unsigned long)r->entries[i].size);
        s_pick_n++;
    }
}

static void build_pick(int target)
{
    s_pick_target = target;
    s_pick_n = 0;
    /* Row 0: eject. */
    s_pick_backend[0] = STORAGE_BACKEND_NONE;
    s_pick_name[0][0] = '\0';
    snprintf(s_pick_row[0], 80, "(eject - leave target empty)");
    s_pick_n = 1;
    pick_add_entries(CONOP_LS_SD, STORAGE_BACKEND_SDCARD, "sd:");
    pick_add_entries(CONOP_LS_FUJI, STORAGE_BACKEND_FUJINET, "fuji:");

    int n = s_pick_n;
    for (int i = 0; i < n; i++) s_pick_ptr[i] = s_pick_row[i];

    char heading[96];
    snprintf(heading, sizeof heading, "Select disk for target %d", target);
    win_begin(heading);
    int h = n < 19 ? n : 19;       /* scroll when the list is taller than this */
    if (h < 1) h = 1;
    mtui_listbox_init(&s_pick_lb, 1, 4, 72, h, s_pick_ptr, n);
    s_pick_lb.base.id = CMD_PICK;
    mtui_window_add(&s_win, &s_pick_lb.base);
    s_app.status = "Up/Down move   Enter select   Q back";
    s_screen = SC_PICK;
}

/* Password entry to join the chosen network. */
static void build_input(void)
{
    s_input[0] = 0;
    char heading[96];
    snprintf(heading, sizeof heading, "Join %s - type the password", s_fuji_ssid);
    win_begin(heading);
    mtui_label_init(&s_lbl_name, 1, 4, "Pass:");
    mtui_window_add(&s_win, &s_lbl_name.base);
    mtui_input_line_init(&s_il, 7, 4, 40, s_input, sizeof s_input);
    mtui_window_add(&s_win, &s_il.base);
    s_app.status = "Enter join   ESC cancel";
    s_screen = SC_INPUT;
}

/* Run the WiFi join for the typed password and return to the setup screen. */
static void input_submit(void)
{
    if (!s_input[0]) return;                    /* nothing typed yet */
    const conop_result_t *r = console_op_submit(CONOP_WIFI_SET, 0, 0, 0,
                                                s_fuji_ssid, s_input);
    if (!r)         snprintf(s_msg, sizeof s_msg, "ERR busy (core 1)");
    else if (r->ok) snprintf(s_msg, sizeof s_msg, "Joined %s", s_fuji_ssid);
    else            snprintf(s_msg, sizeof s_msg, "ERR %s", r->err);
    build_fuji_setup();
}

/* FujiNet setup: one info line (fetched now via the mailbox) + a 2-row action
 * listbox + the shared s_msg line. */
static void build_fuji_setup(void)
{
    const conop_result_t *r = console_op_submit(CONOP_WIFI_GET, 0, 0, 0, NULL, NULL);
    win_begin("FujiNet setup");
    if (!r)
        snprintf(s_fuji_wifi, sizeof s_fuji_wifi, "WiFi:  busy (core 1)");
    else if (!r->ok)
        snprintf(s_fuji_wifi, sizeof s_fuji_wifi, "WiFi:  %s", r->err);
    else
        snprintf(s_fuji_wifi, sizeof s_fuji_wifi, "WiFi:  %s  (%s)",
                 r->ssid[0] ? r->ssid : "-",
                 r->wifi_connected ? "connected" : "disconnected");
    mtui_label_init(&s_lbl_fwifi, 1, 4, s_fuji_wifi);
    mtui_window_add(&s_win, &s_lbl_fwifi.base);
    s_fuji_ptr[0] = "Scan + join a network";
    s_fuji_ptr[1] = "Back";
    mtui_listbox_init(&s_fuji_lb, 1, 6, 60, 2, s_fuji_ptr, 2);
    s_fuji_lb.base.id = CMD_FUJI;
    mtui_window_add(&s_win, &s_fuji_lb.base);
    mtui_label_init(&s_lbl_msg, 1, 10, s_msg);
    mtui_window_add(&s_win, &s_lbl_msg.base);
    s_app.status = "Up/Down move   Enter select   Q back";
    s_screen = SC_FUJI;
}

/* Scan then show a picker. mtui_app_poll draws THEN dispatches, so on_cmd runs
 * after this poll's paint — force one synchronous repaint of a "Scanning..."
 * screen here so the message is on the wire before the multi-second scan. On
 * error, bounce back to the setup screen with the reason. */
static void build_fuji_scan(void)
{
    win_begin("Scan + join a network");
    mtui_label_init(&s_lbl_msg, 1, 4, "Scanning for networks...");
    mtui_window_add(&s_win, &s_lbl_msg.base);
    s_app.status = "Please wait...";
    s_screen = SC_FUJISCAN;
    mtui_app_draw(&s_app);                       /* flush the message, then block */

    const conop_result_t *r = console_op_submit(CONOP_SCAN, 0, 0, 0, NULL, NULL);
    if (!r || !r->ok) {
        snprintf(s_msg, sizeof s_msg, "ERR %s", r ? r->err : "busy (core 1)");
        build_fuji_setup();
        return;
    }
    s_scan_n = r->n_scan;
    for (int i = 0; i < s_scan_n; i++)
        snprintf(s_scan_ssid[i], sizeof s_scan_ssid[i], "%s", r->scan_ssid[i]);

    int n = 0;
    snprintf(s_scan_row[n++], 48, "(cancel)");
    for (int i = 0; i < s_scan_n; i++)
        snprintf(s_scan_row[n++], 48, "%-24s  (%d dBm)", s_scan_ssid[i], r->scan_rssi[i]);
    for (int i = 0; i < n; i++) s_scan_ptr[i] = s_scan_row[i];

    win_begin("Scan + join a network");
    int h = n < 19 ? n : 19;
    if (h < 1) h = 1;
    mtui_listbox_init(&s_scan_lb, 1, 4, 60, h, s_scan_ptr, n);
    s_scan_lb.base.id = CMD_FUJIPICK;
    mtui_window_add(&s_win, &s_scan_lb.base);
    s_app.status = "Up/Down move   Enter select   Q back";
    s_screen = SC_FUJIPICK;
}

/* Leave a channel-owned menu: rebind the engine's terminal + input back onto CDC
 * so a later CDC `menu` works, and drop to REPL. `cleanup` sends the exit escapes
 * to the channel terminal (SGR reset + show cursor + clear) — skipped on a
 * channel reset, whose fresh session will clear anyway. */
static void via_menu_leave(bool cleanup)
{
    if (cleanup && s_via_tp)
        s_via_tp->write(s_via_tp->ctx,
                        (const uint8_t *)(A_SGRRST A_SHOWCUR A_CLEAR),
                        strlen(A_SGRRST A_SHOWCUR A_CLEAR));
    mtui_term_init(&s_term, &s_tp, s_term_buf, sizeof s_term_buf);  /* rebind to CDC */
    s_app.tp = &s_tp;
    s_via_tp = NULL;
    s_tui_owner = OWNER_CDC;
    s_mode = M_REPL;
}

static void tui_exit(void)
{
    if (s_tui_owner == OWNER_VIA) { via_menu_leave(true); return; }
    s_mode = M_REPL;
    cdc_puts(A_SGRRST A_SHOWCUR A_CLEAR);
    tui_repl_banner(&s_ln);
}

/* Listbox Enter routes here with the choice in the listbox's ->sel. */
static void on_cmd(mtui_app_t *a, int id)
{
    (void)a;
    if (id == CMD_MENU) {
        int sel = s_menu_lb.sel;
        s_msg[0] = 0;
        if (sel < MENU_TARGETS) build_pick(sel);
        else if (sel == 2)      build_fuji_setup();
        else if (sel == 3)      tui_exit();       /* Switch to command mode */
        else {                                    /* Exit to boot */
            /* Tell a Victor-side client to quit (private CSI, written while the
             * channel transport is still bound; Stage 2). On the CDC console
             * there is nothing to boot: plain exit. */
            if (s_tui_owner == OWNER_VIA && s_via_tp)
                s_via_tp->write(s_via_tp->ctx, (const uint8_t *)"\x1b[86q", 5);
            tui_exit();
        }
    } else if (id == CMD_FUJI) {
        int sel = s_fuji_lb.sel;
        if (sel == 0) build_fuji_scan();
        else          build_menu(2);
    } else if (id == CMD_FUJIPICK) {
        int sel = s_scan_lb.sel;
        if (sel == 0) { build_fuji_setup(); return; }        /* cancel */
        snprintf(s_fuji_ssid, sizeof s_fuji_ssid, "%s", s_scan_ssid[sel - 1]);
        s_msg[0] = 0;
        build_input();
    } else if (id == CMD_PICK) {
        int sel = s_pick_lb.sel, target = s_pick_target;
        if (sel == 0) {                          /* eject */
            console_op_submit(CONOP_EJECT, 0, (uint8_t)target, 0, NULL, NULL);
            build_menu(target);
            return;
        }
        const conop_result_t *r = console_op_submit(
            CONOP_MOUNT, s_pick_backend[sel], (uint8_t)target, 0,
            s_pick_name[sel], NULL);
        if (!r)         snprintf(s_msg, sizeof s_msg, "ERR busy (core 1)");
        else if (!r->ok) snprintf(s_msg, sizeof s_msg, "ERR %s", r->err);
        build_menu(target);
    }
}

/* Keys the focused control didn't consume: q/ESC/eject on the lists, and the
 * password submit/cancel (Enter/ESC bubble past the input line). */
static void on_key(mtui_app_t *a, const mtui_key_t *k)
{
    (void)a;
    bool q = (k->code == MTUI_KEY_ESC) ||
             (k->code == MTUI_KEY_CHAR && (k->ch == 'q' || k->ch == 'Q'));
    switch (s_screen) {
    case SC_MENU:
        if (q) tui_exit();
        else if (k->code == MTUI_KEY_CHAR && (k->ch == 'e' || k->ch == 'E') &&
                 s_menu_lb.sel < MENU_TARGETS) {
            int d = s_menu_lb.sel;
            console_op_submit(CONOP_EJECT, 0, (uint8_t)d, 0, NULL, NULL);
            build_menu(d);
        }
        break;
    case SC_PICK:
        if (q) build_menu(s_pick_target);
        break;
    case SC_FUJI:
        if (q) build_menu(2);
        break;
    case SC_FUJISCAN:                            /* transient; only for safety */
    case SC_FUJIPICK:
        if (q) build_fuji_setup();
        break;
    case SC_INPUT:
        if (k->code == MTUI_KEY_ENTER) input_submit();
        else if (k->code == MTUI_KEY_ESC) build_fuji_setup();
        break;
    }
}

/* Enter the full-screen menu: reset the terminal, rebuild the main menu, and
 * force a full repaint on the next poll (mtui_app_poll draws before it reads). */
static void tui_enter(void)
{
    if (s_mode == M_TUI) {           /* channel owns the one screen: can't co-open */
        cdc_puts("\r\nmenu busy (Victor terminal)\r\n");
        tui_repl_banner(&s_ln);
        return;
    }
    mtui_term_begin(&s_term);
    build_menu(0);
    mtui_screen_invalidate(&s_scr);
    s_mode = M_TUI;
}

static void repl_key(int key)
{
    mtui_line_t *ln = &s_ln;
    if (key == KEY_ENTER) {
        cdc_puts("\r\n");
        mtui_line_feed(ln, '\r');
    } else if (key == 0x7f || key == 0x08) {
        if (ln->len > 0) cdc_puts("\b \b");
        mtui_line_feed(ln, (uint8_t)key);
    } else if (key >= 32 && key < 127 && ln->len < 95) {
        /* Cap at 95 so the engine's 160-byte buffer never overflows. */
        char c = (char)key;
        cdc_write(&c, 1);
        mtui_line_feed(ln, (uint8_t)key);
    }
    /* arrows/ESC in REPL mode: ignored */

    if (ln->done) {                  /* the `menu` command asked to switch */
        ln->done = false;
        tui_enter();                 /* menu paints on the next poll */
    }
}

/* ---- public entry points -------------------------------------------------- */

void tui_init(void)
{
    tusb_init();
    mtui_pico_cdc_init(&s_cdc, &s_tp, 0);   /* CDC interface 0 = the console */
    tui_cmds_bind(&s_ln, &s_tp);

    /* Full-screen app: a terminal writer + one screen over the same transport.
     * The cell buffers sit in static SRAM (see the decl above). */
    mtui_term_init(&s_term, &s_tp, s_term_buf, sizeof s_term_buf);
    mtui_screen_init(&s_scr, &s_term, 80, 25, s_cells, s_cells + 80 * 25,
                     s_dlo, s_dhi);
    mtui_app_init(&s_app, &s_scr, &s_tp);
    mtui_input_set_clock(&s_app.in, tui_now_ms);
    s_app.desk_fg = MTUI_DEFAULT_COLOR;
    s_app.desk_bg = MTUI_DEFAULT_COLOR;
    s_app.desk_fill = ' ';
    s_app.on_cmd = on_cmd;
    s_app.on_key = on_key;
    mtui_app_add_window(&s_app, &s_win);   /* the single, reconfigured window */
    s_tui_ok = true;
}

void tui_poll(void)
{
    tud_task();

    bool up = tud_cdc_connected();
    if (up != s_connected) {
        s_connected = up;
        /* A channel-owned menu is independent of this USB link — never let a CDC
         * connect/detach clobber the shared s_mode while the channel owns it. */
        if (up) {                        /* fresh connection: reset the line, re-banner */
            if (!tui_menu_via_active()) s_mode = M_REPL;
            s_esc = 0;
            cdc_puts(A_SHOWCUR);
            tui_cmds_bind(&s_ln, &s_tp);
            tui_repl_banner(&s_ln);
        } else if (s_mode == M_TUI && s_tui_owner == OWNER_CDC) {
            s_mode = M_REPL;             /* host detached mid-TUI: reconnect re-banners */
        }
        return;
    }
    if (!up) return;

    if (s_mode == M_TUI && s_tui_owner == OWNER_CDC) {
        mtui_app_poll(&s_app);           /* draws the diff, then dispatches keys */
        return;
    }
    /* M_REPL, or M_TUI owned by the channel: the CDC side stays a REPL either way
     * (a CDC `menu` here hits the busy guard in tui_enter). */

    int key = key_timeout();
    while (key != KEY_NONE || tud_cdc_available()) {
        if (key == KEY_NONE) {
            uint8_t ch;
            if (tud_cdc_read(&ch, 1) != 1) break;
            key = key_feed(ch);
            if (key == KEY_NONE) continue;
        }
        repl_key(key);
        key = KEY_NONE;
        if (s_mode == M_TUI) break;      /* `menu` switched; the app draws next poll */
    }
}

bool tui_menu_active(void)
{
    return s_mode == M_TUI;              /* true for either owner (diag-quiet) */
}

/* ---- channel ownership of the shared menu (Stage 2 seams) ----------------- */

bool tui_menu_via_active(void)
{
    return s_mode == M_TUI && s_tui_owner == OWNER_VIA;
}

/* Take the one full-screen engine for the Victor console, rebinding both its
 * output (terminal) and input (app->tp) onto `tp`. Returns false if the menu is
 * already up (either owner). On success the caller emits nothing — the full menu
 * draws on the next tui_menu_poll_via(). */
bool tui_menu_enter_via(mtui_transport_t *tp)
{
    if (!s_tui_ok || s_mode == M_TUI) return false;
    s_via_tp = tp;
    mtui_term_init(&s_term, tp, s_term_buf, sizeof s_term_buf);  /* output -> channel */
    s_app.tp = tp;                       /* input reads -> channel (read() returns 0) */
    mtui_input_init(&s_app.in);          /* fresh decoder for the new stream */
    mtui_input_set_clock(&s_app.in, tui_now_ms);
    mtui_term_begin(&s_term);
    build_menu(0);
    mtui_screen_invalidate(&s_scr);
    s_tui_owner = OWNER_VIA;
    s_mode = M_TUI;
    return true;
}

/* Feed one channel input byte into the menu's key decoder and dispatch a
 * complete key. Bare-ESC/partial-CSI timeouts resolve through
 * tui_menu_poll_via (its mtui_app_poll reads the channel transport, gets 0, and
 * flushes on idle). */
void tui_menu_feed_via(uint8_t b)
{
    if (!tui_menu_via_active()) return;
    mtui_key_t k;
    if (mtui_input_feed(&s_app.in, b, &k))
        mtui_app_dispatch(&s_app, &k);
}

/* Paint one frame for the channel-owned menu. Damage-diff makes idle passes
 * cheap; the input drain inside reads nothing (channel transport read() == 0). */
void tui_menu_poll_via(void)
{
    if (!tui_menu_via_active()) return;
    mtui_app_poll(&s_app);
}

/* Force a channel-owned menu closed without emitting cleanup escapes (used on a
 * channel reset, where the FIFO was just flushed and a new session follows). */
void tui_menu_abort_via(void)
{
    if (tui_menu_via_active()) via_menu_leave(false);
}
