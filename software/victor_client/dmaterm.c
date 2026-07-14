/**
 * @file dmaterm.c
 * @brief Victor-side terminal to the DMA card's management console.
 *
 * DMA-card sibling of viclibc2/viaterm/viaterm.c. Same job, same VT100->VRAM
 * renderer, same keyboard rewriting - ONLY THE CHANNEL MOVED. The v9k_flop card
 * exposed its console through three VIA-A registers in the CRTC's 0xE800 I/O
 * segment; the DMA card exposes an identical channel through the DMA register
 * window at segment 0xEF30 (phys 0xEF340/50/60), with a 0xD0 signature nibble
 * instead of 0xB0 so a client can tell the two cards apart. Everything else -
 * the CRTC at 0xE800:0/1, VRAM at 0xF000, the DOS/Z-19 keyboard path - is the
 * same machine and is byte-identical to viaterm.c.
 *
 * The protocol and the renderer are SHARED with viaterm.c; the only differences
 * are the channel registers (segment/offsets) and the status signature nibble.
 * The VTASM sibling is victor_client/viaterm_dma.asm (same channel constants).
 *
 * The firmware's mtui speaks VT100; the response stream is rendered by writing
 * character cells straight into VRAM (igc-style, see igc/src/screen.c) - DOS CON
 * is bypassed entirely for output, which is what makes full-screen menu repaints
 * fast. Keyboard input still arrives through DOS (Z-19 sequences from CON), and
 * is rewritten into the VT100 the firmware's menu decoder expects. No serial
 * library needed - the channel is memory-mapped I/O.
 *
 * Register map (segment 0xEF30, the DMA-card control window):
 *   CMD    = ch[0x40]  read: complement echo (detection) ; write: input byte
 *   STATUS = ch[0x50]  read: 0xD0 sig | OVERFLOW b3 | CMD_FULL b2 |
 *                            RESP_READY b1 | RESP_PHASE b0 ; write: soft reset
 *   RESP   = ch[0x60]  read: current response byte ; write: ack/advance
 *
 * Build: see build_dos.sh (small-model DOS, no library). No arguments.
 */

#include <stdio.h>
#include <conio.h>
#include <i86.h>
#include <dos.h>

#define CH_SEG     0xEF30      /* DMA-card control-channel segment (phys 0xEF300) */

#define CMD_OFF    0x40
#define STATUS_OFF 0x50
#define RESP_OFF   0x60

#define ST_SIG     0xD0        /* high-nibble signature (DMA card; flop = 0xB0) */
#define ST_OVER    0x08        /* OVERFLOW   */
#define ST_READY   0x02        /* RESP_READY */
#define ST_PHASE   0x01        /* RESP_PHASE */

#define ESC        0x1B

/* Quit = typing the line "exit" (intercepted locally, never sent) or Ctrl+C.
 * The "exit" interception is disabled while mtui has the cursor hidden (menu
 * mode): there e/x/i/t/Enter are menu keys, not a quit request. Ctrl+C always
 * quits. Function keys (0xF1-0xFA) are dropped locally. */

static volatile unsigned char __far *via;

/* Cursor-visibility shadow, driven by ESC[?25l/h. Menu mode hides the cursor;
 * while hidden the local "exit"-line interception is off. */
static int cursor_hidden = 0;

/* --- Direct screen layer: VRAM + CRTC (after igc src/screen.c) ------------ */

#define VRAM_SEG   0xF000
#define CHAR_BASE  0x64        /* glyph = char + CHAR_BASE (char ROM offset) */
#define COLS       80
#define ROWS       25

#define A_REV      0x80        /* cell attribute bits (cell high byte) */
#define A_UL       0x20

#define CRTC_SEL_OFF      0x00 /* CRTC stays in the 0xE800 I/O segment; only  */
#define CRTC_DATA_OFF     0x01 /*   the control channel moved to 0xEF30       */
#define CRTC_CURSOR_START 10
#define CRTC_START_MSB    12
#define CRTC_START_LSB    13
#define CRTC_CURSOR_MSB   14
#define CRTC_CURSOR_LSB   15
#define CURSOR_SOLID      0x00
#define CURSOR_HIDDEN     0x20

#define BLANK_CELL ((unsigned short)(' ' + CHAR_BASE))

static unsigned short __far *vram;
static volatile unsigned char __far *crtc_sel;
static volatile unsigned char __far *crtc_data;

static unsigned char scr_x, scr_y;   /* cursor; HW cursor follows via scr_sync */
static unsigned char scr_attr;       /* attr bits for newly written cells */

static const unsigned short line_off[ROWS] = {
       0,   80,  160,  240,  320,
     400,  480,  560,  640,  720,
     800,  880,  960, 1040, 1120,
    1200, 1280, 1360, 1440, 1520,
    1600, 1680, 1760, 1840, 1920
};

/* CRTC needs a settle delay between register accesses. */
static void crtc_delay(void)
{
    volatile unsigned short i;
    for (i = 0; i < 50; i++) {
    }
}

static void crtc_write(unsigned char reg, unsigned char val)
{
    *crtc_sel = reg;
    crtc_delay();
    *crtc_data = val;
}

static unsigned char crtc_read(unsigned char reg)
{
    *crtc_sel = reg;
    crtc_delay();
    return *crtc_data;
}

static void scr_show_cursor(int show)
{
    unsigned char r;

    r = crtc_read(CRTC_CURSOR_START);
    r &= 0x1F;                             /* keep raster bits */
    r |= show ? CURSOR_SOLID : CURSOR_HIDDEN;
    crtc_write(CRTC_CURSOR_START, r);
}

/* Move the hardware cursor to the software cursor. Called once per drained
 * response burst, not per byte - CRTC register writes are slow. */
static void scr_sync(void)
{
    unsigned short pos = line_off[scr_y] + scr_x;

    crtc_write(CRTC_CURSOR_MSB, (unsigned char)((pos >> 8) & 0x3F));
    crtc_write(CRTC_CURSOR_LSB, (unsigned char)(pos & 0xFF));
}

static void scr_clear(void)
{
    unsigned short i;

    for (i = 0; i < ROWS * COLS; i++)
        vram[i] = BLANK_CELL;
    scr_x = 0;
    scr_y = 0;
}

static void scr_scroll(void)
{
    unsigned short i;

    for (i = 0; i < (ROWS - 1) * COLS; i++)
        vram[i] = vram[i + COLS];
    for (; i < ROWS * COLS; i++)
        vram[i] = BLANK_CELL;
}

/* Render one byte at the cursor. Column 80 clamps rather than wraps (the old
 * CON path ran in ESC w discard-at-EOL mode for the same reason): mtui
 * repositions every run explicitly, and a wrap after the bottom-right menu
 * cell would scroll the screen. */
static void scr_putc(unsigned char b)
{
    if (b == 0x0D) {
        scr_x = 0;
        return;
    }
    if (b == 0x0A) {
        if (scr_y < ROWS - 1)
            scr_y++;
        else
            scr_scroll();
        return;
    }
    if (b == 0x08) {
        if (scr_x > 0)
            scr_x--;
        return;
    }
    if (b < 0x20)
        return;                            /* BEL etc: swallow */
    vram[line_off[scr_y] + scr_x] =
        (unsigned short)(b + CHAR_BASE) | ((unsigned short)scr_attr << 8);
    if (scr_x < COLS - 1)
        scr_x++;
}

static void scr_puts(const char *s)
{
    while (*s)
        scr_putc((unsigned char)*s++);
}

/* Take the screen over from DOS: linear display start, blank page, home. */
static void scr_open(void)
{
    vram      = (unsigned short __far *)MK_FP(VRAM_SEG, 0);
    crtc_sel  = (volatile unsigned char __far *)MK_FP(0xE800, CRTC_SEL_OFF);
    crtc_data = (volatile unsigned char __far *)MK_FP(0xE800, CRTC_DATA_OFF);

    crtc_write(CRTC_START_MSB, 0);         /* reset display start: DOS may */
    crtc_write(CRTC_START_LSB, 0);         /*   have left it scrolled      */
    scr_attr = 0;
    scr_clear();
}

/* --- Output translator: VT100 (from mtui) -> VRAM ------------------------- */

/* Parser state. Persists across emit() calls because a VT100 sequence can be
 * split across firmware reads. */
static int  out_state;         /* 0=normal, 1=saw ESC, 2=inside CSI */
static int  csi_priv;          /* CSI had a '?' (private) marker */
static int  csi_seen;          /* CSI had any digit or ';' */
static int  csi_ni;            /* index of current param */
static int  csi_param[8];      /* decimal params (SGR with colors sends 6) */

#define OUT_NORMAL 0
#define OUT_ESC    1
#define OUT_CSI    2

/* Raw byte to CON via INT 21h AH=02 (DL=byte). Session boundaries only -
 * everything mid-session goes to VRAM. */
static void putb(unsigned char b)
{
    bdos(0x02, b, 0);
}

static void put_esc(unsigned char c)
{
    putb(ESC);
    putb(c);
}

/* Dispatch a completed CSI whose final byte is `f`. */
static void csi_dispatch(unsigned char f)
{
    int r, c, i;
    unsigned char a;

    switch (f) {
    case 'J':                                  /* ESC[2J -> clear + home */
        if (csi_param[0] == 2)
            scr_clear();
        break;
    case 'H':                                  /* home or absolute move */
        if (!csi_seen || (csi_ni == 0 && csi_param[0] == 0)) {
            scr_x = 0;
            scr_y = 0;
        } else {
            r = csi_param[0]; if (r < 1) r = 1; if (r > ROWS) r = ROWS;
            c = csi_param[1]; if (c < 1) c = 1; if (c > COLS) c = COLS;
            scr_y = (unsigned char)(r - 1);
            scr_x = (unsigned char)(c - 1);
        }
        break;
    case 'l':                                  /* ESC[?25l -> cursor hide */
        if (csi_priv && csi_param[0] == 25) {
            scr_show_cursor(0);
            cursor_hidden = 1;
        }
        break;
    case 'h':                                  /* ESC[?25h -> cursor show */
        if (csi_priv && csi_param[0] == 25) {
            scr_show_cursor(1);
            cursor_hidden = 0;
        }
        break;
    case 'm':                                  /* SGR -> reverse/underline.
                                                  mtui always resets-and-
                                                  rebuilds, so compute from
                                                  scratch; colors ignored */
        a = 0;
        for (i = 0; i <= csi_ni; i++) {
            if (csi_param[i] == 7) a |= A_REV;
            if (csi_param[i] == 4) a |= A_UL;
        }
        scr_attr = a;
        break;
    default:
        break;                                 /* any other CSI: swallow */
    }
}

/* Feed one firmware response byte through the translator. */
static void emit(unsigned char b)
{
    int i;

    switch (out_state) {
    case OUT_NORMAL:
        if (b == ESC)
            out_state = OUT_ESC;
        else
            scr_putc(b);                       /* ordinary byte: render */
        break;

    case OUT_ESC:
        if (b == '[') {
            out_state = OUT_CSI;
            csi_priv = 0; csi_seen = 0; csi_ni = 0;
            for (i = 0; i < 8; i++)
                csi_param[i] = 0;
        } else {
            out_state = OUT_NORMAL;            /* unknown ESC-x: swallow both */
        }
        break;

    case OUT_CSI:
        if (b >= 0x40 && b <= 0x7E) {          /* final byte */
            csi_dispatch(b);
            out_state = OUT_NORMAL;
        } else if (b >= '0' && b <= '9') {
            csi_param[csi_ni] = csi_param[csi_ni] * 10 + (b - '0');
            csi_seen = 1;
        } else if (b == ';') {
            if (csi_ni < 7) csi_ni++;
            csi_param[csi_ni] = 0;
            csi_seen = 1;
        } else if (b == '?') {
            csi_priv = 1;
        }
        /* other intermediates (0x20-0x2F): ignore, keep collecting */
        break;

    default:
        out_state = OUT_NORMAL;
        break;
    }
}

/* --- Session channel reset ------------------------------------------------ */

/* Soft-reset the channel and wait (bounded) for the firmware to process it,
 * then return the phase bit to seed the poll loop. Shared by session start and
 * overflow recovery. Only call after detection has passed - writes STATUS.
 *
 * "Reset processed" is signalled by the CMD echo re-arming to 0xFF (its
 * power-on value) - the flag bits alone can't distinguish "reset done" from
 * "flags happened to be clear already", and sending input before the flush has
 * run would get it eaten. The last CMD write is never 0x00 here, so the prior
 * echo is never 0xFF. */
static unsigned char session_settle(void)
{
    unsigned long spin;

    via[STATUS_OFF] = 0;                     /* any write = soft channel reset */
    for (spin = 0; spin < 200000UL; spin++) {
        if (via[CMD_OFF] == 0xFF)            /* echo re-armed = flush ran */
            break;
    }
    return (unsigned char)(via[STATUS_OFF] & ST_PHASE);
}

int main(void)
{
    unsigned char last_phase;
    unsigned char st;
    char line[6];              /* local shadow of the current input line */
    int llen = 0;              /* chars since last CR; >5 = "can't be exit" */
    int c, d, drained;

    via = (volatile unsigned char __far *)MK_FP(CH_SEG, 0);

    /* Detection - side-effect-free beyond CMD writes; never touch 0x50/0x60.
     * Two-point complement echo: a real 6522 SR returns the value written and
     * pre-feature firmware returns 0xFF; neither can complement both. */
    via[CMD_OFF] = 0x55;
    if (via[CMD_OFF] != 0xAA) { printf("No DMA card detected.\n"); return 1; }
    via[CMD_OFF] = 0xAA;
    if (via[CMD_OFF] != 0x55) { printf("No DMA card detected.\n"); return 1; }
    /* Stable 0xD signature nibble on two consecutive reads (a real 6522 T2C-H
     * would free-run and vary). */
    if ((via[STATUS_OFF] & 0xF0) != ST_SIG) { printf("No DMA card detected.\n"); return 1; }
    if ((via[STATUS_OFF] & 0xF0) != ST_SIG) { printf("No DMA card detected.\n"); return 1; }

    /* Session start: take the screen, then open the channel. */
    scr_open();
    scr_puts("v9k DMA terminal - type 'exit' to quit.");
    scr_putc(0x0D); scr_putc(0x0A);
    scr_sync();
    last_phase = session_settle();
    via[CMD_OFF] = 0x0D;                      /* CR -> firmware emits a prompt */

    for (;;) {
        if (kbhit()) {
            c = getch();
            if (c == 0) {                     /* extended prefix: never seen on
                                                 Victor DOS; eat the scancode */
                (void)getch();
            } else if (c == 0x03) {           /* Ctrl+C (Alt+c on the Victor) */
                break;
            } else if (c == ESC) {
                /* Z-19 keyboard escape. The two bytes of one keypress land in
                 * the typeahead buffer atomically, so kbhit() right now tells
                 * arrows/nav apart from a bare ESC key. Nav keys only mean
                 * something in the menu (cursor hidden); in line mode the
                 * engine has no cursor keys, and the translated '['+letter
                 * would type into the line - drop them there. */
                if (kbhit()) {
                    d = getch();
                    if (!cursor_hidden)
                        continue;                             /* REPL: drop nav */
                    if (d == 'A' || d == 'B' || d == 'C' || d == 'D') {
                        via[CMD_OFF] = ESC; via[CMD_OFF] = '[';
                        via[CMD_OFF] = (unsigned char)d;      /* arrows */
                    } else if (d == 'H') {
                        via[CMD_OFF] = ESC; via[CMD_OFF] = '[';
                        via[CMD_OFF] = 'H';                   /* CLR/HOME */
                    } else if (d == 'K') {
                        via[CMD_OFF] = ESC; via[CMD_OFF] = '[';
                        via[CMD_OFF] = 'F';                   /* ERASE-EOL -> End */
                    } else {
                        via[CMD_OFF] = ESC;
                        via[CMD_OFF] = (unsigned char)d;      /* ESC + raw byte */
                    }
                } else if (cursor_hidden) {
                    via[CMD_OFF] = ESC;                       /* bare ESC: menu back/exit */
                }
            } else if (c == 0xE4 || c == 0xE5) {              /* word keys -> PgUp/PgDn */
                if (cursor_hidden) {
                    via[CMD_OFF] = ESC; via[CMD_OFF] = '[';
                    via[CMD_OFF] = (unsigned char)(c == 0xE4 ? '5' : '6');
                    via[CMD_OFF] = '~';
                }
            } else if (c >= 0x80) {
                /* F-keys (0xF1-0xFA) and other high bytes: drop locally */
            } else {
                /* Plain byte. Shadow the line so a typed "exit" quits locally -
                 * but only when the cursor is visible (i.e. NOT in the menu,
                 * where e/x/i/t are menu keys). The chars were already
                 * sent+echoed; the unsent CR leaves "exit" in the firmware's
                 * line buffer, which the next session reset drops. */
                if (!cursor_hidden) {
                    if (c == 0x0D) {
                        if (llen == 4 && (line[0] | 0x20) == 'e' &&
                            (line[1] | 0x20) == 'x' && (line[2] | 0x20) == 'i' &&
                            (line[3] | 0x20) == 't')
                            break;
                        llen = 0;
                    } else if (c == 0x08 || c == 0x7F) {
                        if (llen > 0) llen--;
                    } else if (llen < 5) {
                        line[llen] = (char)c;
                        llen++;
                    }                          /* >5 chars: stays "not exit" */
                }
                via[CMD_OFF] = (unsigned char)c;   /* raw byte, no local echo */
            }
        }

        st = via[STATUS_OFF];                  /* one read decides everything */
        if (st & ST_OVER) {
            scr_puts("[overflow]");
            scr_putc(0x0D); scr_putc(0x0A);
            scr_sync();
            last_phase = session_settle();
            continue;
        }
        /* Drain greedily so output isn't throttled to one byte per keypoll.
         * Fresh STATUS read each pass; latch phase from the read we gated on. */
        drained = 0;
        while ((st & ST_READY) && ((st & ST_PHASE) != last_phase)) {
            d = via[RESP_OFF];                 /* grab the byte... */
            via[RESP_OFF] = 0;                 /* ...ack at once, so the firmware
                                                  stages the next byte while we
                                                  render this one */
            last_phase = (unsigned char)(st & ST_PHASE);
            emit((unsigned char)d);            /* VT100 -> VRAM */
            if (++drained >= 256)
                break;
            st = via[STATUS_OFF];
        }
        if (drained)
            scr_sync();
    }

    /* Hand the screen back to DOS. CON hardware-scrolls via a private screen
     * base + CRTC R12/13; scr_open zeroed the registers but CON's base is
     * still wherever the pre-session scrollback left it. ESC E (DOS 3.1
     * CO.PLM CLRSCR) blanks rows 1-24, homes, AND resets that base + R12/13
     * - the one escape that realigns CON with the physical frame we rendered
     * into. It preserves the 25th line, so blank VRAM ourselves first (the
     * menu's status bar lives there). Attributes are per-cell now, no ESC
     * q/1 needed. (DOS 2.11 ignores ESC E; there CON stays skewed until its
     * first scroll rewrites R12/13 - it self-heals.) */
    scr_show_cursor(1);
    scr_clear();
    put_esc('y'); putb('5');                   /* CON re-asserts its cursor */
    put_esc('E');                              /* CON clear+home+base reset */
    printf("bye.\n");
    return 0;
}
