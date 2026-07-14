/* fuji_blkdev.c — SPI1-master transport + block ops for a FujiNet disk.
 * See fuji_blkdev.h. The protocol core (SLIP/framing/checksum/build/parse) is
 * in fuji_proto.c; this file is only the RP2350 SPI glue, the shared-bus
 * arbitration, and the request/reply transaction loop.
 *
 * Ported from v9k_flop/firmware/fuji_blkdev.c. Two things changed for this
 * board: (1) the link moved from a dedicated spi0 to spi1 SHARED with the SD
 * card, so every transaction is bracketed by fuji_bus_acquire()/release(); and
 * (2) the ~43 KB of transaction buffers moved from a PSRAM carve-out (there is
 * no PSRAM chip here) to static SRAM. The transport framing/timing below is
 * kept verbatim from v9k_flop.
 *
 * Transport: the ESP32-S3 is an SPI slave (SPISlaveChannel in the fujinet
 * feathers3 patch). DRDY (ESP -> RP) is high exactly while the slave has a
 * transaction armed, so the master only ever asserts CS under DRDY-high.
 * Every transaction starts with a 4-byte header:
 *     [magic][len_lo][len_hi][tag]
 * magic = 0xFB master->slave, 0xFA slave->master; len = SLIP frame bytes that
 * follow; the reply echoes the request's tag, so a stale armed reply from a
 * desynced exchange can never be taken for the current one. Requests are padded
 * to a 4-byte multiple for the slave's RX DMA; replies are read to their exact
 * length. */
#include "fuji_blkdev.h"
#include "fuji_proto.h"
#include "pico/stdlib.h"
#include "pico/mutex.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include <stdio.h>
#include <string.h>

/* SPI1 -> ESP32-S3, SHARED with the SD card. The three data pins come off the
 * J2 "SPI Breakout" header and are ALREADY muxed GPIO_FUNC_SPI by the SD driver
 * (pico_storage/hw_config.c) — we must NOT re-init or re-mux them here or we
 * fight the SD driver for the pads. CS is a plain GPIO: the PL022 hardware CS
 * can drop between FIFO words, and the ESP slave ends its transaction on any CS
 * rise, so we drive CS by hand across the whole request/reply. */
#define FUJI_SPI        spi1
#define FUJI_PIN_SCK    42     /* SPI1 SCK -> ESP IO36 (silk SCK), SD driver muxes */
#define FUJI_PIN_MOSI   43     /* SPI1 TX  -> ESP IO35 (silk MO),  SD driver muxes */
#define FUJI_PIN_MISO   44     /* SPI1 RX  <- ESP IO37 (silk MI),  SD driver muxes */
#define FUJI_PIN_CS     45     /* GPIO out -> ESP IO10, idle high             */
#define FUJI_PIN_DRDY   46     /* GPIO in  <- ESP IO11, pull-down             */

#ifndef FUJI_SPI_HZ
#define FUJI_SPI_HZ     12500000  /* bring-up rate on the shared bus. The plan
                                     ladders up to 25 MHz once the shared-bus
                                     signal integrity is re-verified; compile
                                     with -DFUJI_SPI_HZ to override. The ESP
                                     slave takes its clock from us, no config on
                                     that side needs to match. */
#endif

/* Transport header (must match SPISlaveChannel.h in the feathers3 patch). */
#define FUJI_SPI_MAGIC_REQ  0xFBu
#define FUJI_SPI_MAGIC_RSP  0xFAu
#define FUJI_SPI_HDR        4u
#define FUJI_SPI_REQ_MAX    2048u  /* slave RX buffer: header + frame + pad */

/* Reply timing. The ESP arms its reply once its TNFS reads are done; a cold
 * 16-sector READ_MULTI is ~40 ms of ESP fread time (plus a TNFS retry), so
 * 250 ms of DRDY budget stays generous (>6x). MOUNT_ALL can take seconds (the
 * ESP mounts each slot over the network) so it gets a longer wait. */
#define FUJI_FIRST_US       250000u
#define FUJI_MOUNT_FIRST_US 2000000u
/* SET_SSID connects to the AP before replying and SCAN_NETWORKS runs a live
 * scan first — both block for several seconds — so they wait a long time for the
 * reply and use a SINGLE attempt (a retry resend mid-connect/mid-scan would just
 * queue a duplicate operation on the ESP). */
#define FUJI_WIFI_FIRST_US  15000000u
#define FUJI_DRDY_FALL_US   2000u
#define FUJI_RETRIES        3

static bool    s_inited;
static uint8_t s_tag;

/* Shared-bus guard. SPI1 is shared with the SD card; s_bus_mutex serialises
 * ownership and fuji_bus_acquire/release only ever switch owners between
 * complete transactions (never mid-burst). Acquire sets FUJI_SPI_HZ and saves
 * the caller's baud so release can restore it (the SD driver runs at 25 MHz). */
auto_init_mutex(s_bus_mutex);          /* boot-time init: no racy lazy mutex_init */
static uint    s_saved_baud;

/* Transaction buffers. v9k_flop carved these from PSRAM; this board has no
 * PSRAM chip, so they live in static SRAM (~43 KB total — RP2350 has 520 KB).
 * fuji_bus_acquire() is taken BEFORE frames are built into these buffers (see
 * transact/transact_u8), so concurrent callers on core 0 (console) and core 1
 * (storage, Phase B) can't clobber each other's frames. Sizes mirror v9k_flop (2 KB request; the reply/frame
 * buffers hold a 16-sector READ_MULTI ACK frame at SLIP worst case). */
static uint8_t s_tx[FUJI_SPI_REQ_MAX];    /* request out (header + frame + pad) */
static uint8_t s_rx[FUJI_ENC_MAX];        /* reply in                          */
static uint8_t s_scratch[FUJI_DEC_MAX];   /* reply decode                      */
static uint8_t s_frame[FUJI_ENC_MAX];     /* request build                     */

/* [fuji-t] SPI transport timing (measurement only, for the bench speed runs).
 * *_cur hold the current transaction's phase totals (reset per attempt in
 * transact); *_sum aggregate over 128 successful transactions and flush as one
 * printf. No per-transaction prints — printf in the hot path costs multiple ms
 * on this rig. */
static uint32_t s_ft_n;
static uint64_t s_ft_total_sum, s_ft_drdy_sum, s_ft_xfer_sum;
static uint64_t s_ft_drdy_cur, s_ft_xfer_cur;

/* DRDY read accessor. rev2 routes DRDY through an MCP23S17 expander (GPA4)
 * rather than a direct GPIO — swap this one function's body for the expander
 * read there and the rest of the transport is unchanged. */
static inline bool fuji_drdy(void)
{
    return gpio_get(FUJI_PIN_DRDY);
}

void fuji_link_init(void)
{
    if (s_inited) return;
    /* Do NOT touch GP42/43/44: the SD driver owns those pads (GPIO_FUNC_SPI)
     * and spi_init() on the shared peripheral. We only own CS + DRDY. */
    gpio_init(FUJI_PIN_CS);
    gpio_put(FUJI_PIN_CS, 1);
    gpio_set_dir(FUJI_PIN_CS, GPIO_OUT);
    gpio_init(FUJI_PIN_DRDY);
    gpio_set_dir(FUJI_PIN_DRDY, GPIO_IN);
    gpio_pull_down(FUJI_PIN_DRDY);       /* floats while the ESP (re)boots */
    s_inited = true;
}

/* Take the shared SPI1 bus for one FujiNet transaction: serialise against the
 * SD owner, then set our baud (saving the SD driver's so release restores it).
 * Format is SPI mode 0 / MSB-first for both devices, so it needs no switch. */
static void fuji_bus_acquire(void)
{
    mutex_enter_blocking(&s_bus_mutex);
    s_saved_baud = spi_get_baudrate(FUJI_SPI);
    spi_set_baudrate(FUJI_SPI, FUJI_SPI_HZ);
}

static void fuji_bus_release(void)
{
    spi_set_baudrate(FUJI_SPI, s_saved_baud);
    mutex_exit(&s_bus_mutex);
}

/* Wait for DRDY to reach `level`. A 0 budget is a single sample. */
static bool wait_drdy(bool level, uint32_t budget_us)
{
    uint64_t t0 = time_us_64();          /* [fuji-t] count DRDY-high waits only */
    absolute_time_t dl = make_timeout_time_us(budget_us);
    while (fuji_drdy() != level) {
        if (time_reached(dl)) { if (level) s_ft_drdy_cur += time_us_64() - t0; return false; }
        tight_loop_contents();
    }
    if (level) s_ft_drdy_cur += time_us_64() - t0;
    return true;
}

/* Clock one request transaction out under DRDY-high. `ready_us` bounds the
 * wait for the slave's armed RX. */
static bool send_request(const uint8_t *frame, size_t flen, uint8_t tag,
                         uint32_t ready_us)
{
    uint8_t *tx = s_tx;
    size_t total = (FUJI_SPI_HDR + flen + 3u) & ~3u;   /* slave RX DMA pads */

    if (!flen || total > FUJI_SPI_REQ_MAX) return false;
    tx[0] = FUJI_SPI_MAGIC_REQ;
    tx[1] = (uint8_t)(flen & 0xFF);
    tx[2] = (uint8_t)(flen >> 8);
    tx[3] = tag;
    memcpy(tx + FUJI_SPI_HDR, frame, flen);
    memset(tx + FUJI_SPI_HDR + flen, 0, total - FUJI_SPI_HDR - flen);

    if (!wait_drdy(true, ready_us)) return false;
    uint64_t x0 = time_us_64();          /* [fuji-t] CS-asserted clocking */
    gpio_put(FUJI_PIN_CS, 0);
    spi_write_blocking(FUJI_SPI, tx, total);
    gpio_put(FUJI_PIN_CS, 1);
    s_ft_xfer_cur += time_us_64() - x0;
    /* post-trans ISR drops DRDY in ~µs; best-effort — the reply header's
     * magic/tag check catches a desynced slave either way. */
    wait_drdy(false, FUJI_DRDY_FALL_US);
    return true;
}

/* Clock one reply transaction in: header first, then exactly `len` bytes in
 * the same CS window. Returns the frame length, or 0 on timeout / junk. */
static size_t recv_reply(uint8_t *buf, size_t cap, uint8_t tag, uint32_t first_us)
{
    uint8_t hdr[FUJI_SPI_HDR];

    if (!wait_drdy(true, first_us)) return 0;
    uint64_t x0 = time_us_64();          /* [fuji-t] CS-asserted clocking */
    gpio_put(FUJI_PIN_CS, 0);
    spi_read_blocking(FUJI_SPI, 0x00, hdr, sizeof(hdr));
    size_t len = (size_t)hdr[1] | ((size_t)hdr[2] << 8);
    if (hdr[0] != FUJI_SPI_MAGIC_RSP || hdr[3] != tag || !len || len > cap) {
        gpio_put(FUJI_PIN_CS, 1);        /* junk (e.g. we clocked an armed RX) */
        s_ft_xfer_cur += time_us_64() - x0;
        wait_drdy(false, FUJI_DRDY_FALL_US);
        return 0;
    }
    spi_read_blocking(FUJI_SPI, 0x00, buf, len);
    gpio_put(FUJI_PIN_CS, 1);
    s_ft_xfer_cur += time_us_64() - x0;
    wait_drdy(false, FUJI_DRDY_FALL_US);
    return len;
}

/* [fuji-t] Fold one successful transaction's phase totals into the aggregate;
 * every 128 emit and reset. drdy = DRDY-high wait (request + reply), xfer =
 * CS-asserted clocking, total = whole transact call; the remainder is main-loop
 * cadence + framing. */
static void fuji_t_account(uint64_t total_us)
{
    s_ft_n++;
    s_ft_total_sum += total_us;
    s_ft_drdy_sum  += s_ft_drdy_cur;
    s_ft_xfer_sum  += s_ft_xfer_cur;
    if (s_ft_n >= 128) {
        printf("[fuji-t] n=%u avg_us total=%u drdy=%u xfer=%u\n", s_ft_n,
               (unsigned)(s_ft_total_sum / s_ft_n),
               (unsigned)(s_ft_drdy_sum  / s_ft_n),
               (unsigned)(s_ft_xfer_sum  / s_ft_n));
        s_ft_n = 0;
        s_ft_total_sum = s_ft_drdy_sum = s_ft_xfer_sum = 0;
    }
}

/* Send a prebuilt (SLIP-encoded) request frame and parse the reply. Returns
 * true on ACK. On ACK with a payload, copies up to `rx_cap` bytes into
 * `rx_payload` and reports the count in `rx_len`. Retries on timeout / NAK /
 * malformed reply. Caller must hold the bus (fuji_bus_acquire) across the
 * whole exchange — frame build included — so the baud stays set across resends
 * and the shared static buffers stay single-owner. */
static bool transact_frame_locked(const uint8_t *frame, size_t flen,
                                  uint8_t *rx_payload, size_t rx_cap, size_t *rx_len,
                                  int retries, uint32_t first_us)
{
    uint8_t *rx      = s_rx;
    uint8_t *scratch = s_scratch;
    bool     ok      = false;

    if (!flen) return false;

    for (int attempt = 0; attempt < retries; attempt++) {
        s_ft_drdy_cur = 0;                             /* [fuji-t] this attempt */
        s_ft_xfer_cur = 0;
        uint64_t t0 = time_us_64();
        uint8_t tag = ++s_tag;
        if (!send_request(frame, flen, tag, FUJI_FIRST_US)) continue;

        size_t rlen = recv_reply(rx, FUJI_ENC_MAX, tag, first_us);
        if (!rlen) continue;                          /* timeout / desync */

        fuji_frame_t f;
        if (!fuji_parse_frame(rx, rlen, scratch, FUJI_DEC_MAX, &f))
            continue;                                 /* garbage / bad checksum */
        if (f.command != FUJICMD_ACK) continue;       /* NAK or unexpected */

        if (rx_payload) {
            size_t cp = f.payload_len < rx_cap ? f.payload_len : rx_cap;
            if (f.payload && cp) memcpy(rx_payload, f.payload, cp);
            if (rx_len) *rx_len = cp;
        }
        fuji_t_account(time_us_64() - t0);            /* [fuji-t] aggregate */
        ok = true;
        break;
    }
    return ok;
}

/* Build a u32-param request and run it. */
static bool transact(uint8_t device, uint8_t command, bool has_param, uint32_t param,
                     const uint8_t *tx_payload, size_t tx_payload_len,
                     uint8_t *rx_payload, size_t rx_cap, size_t *rx_len,
                     int retries, uint32_t first_us)
{
    fuji_bus_acquire();
    size_t flen = fuji_build_frame(device, command, has_param, param,
                                   tx_payload, tx_payload_len, s_frame, FUJI_ENC_MAX);
    bool ok = transact_frame_locked(s_frame, flen, rx_payload, rx_cap, rx_len,
                                    retries, first_us);
    fuji_bus_release();
    return ok;
}

/* Build a request with `nparams` u8 params and run it (FujiNet control cmds). */
static bool transact_u8(uint8_t device, uint8_t command,
                        const uint8_t *params, unsigned nparams,
                        const uint8_t *tx_payload, size_t tx_payload_len,
                        uint8_t *rx_payload, size_t rx_cap, size_t *rx_len,
                        int retries, uint32_t first_us)
{
    /* Control frames only: header + params + a <=FUJI_PATH_LEN payload, SLIP
     * worst case — no need for a second FUJI_ENC_MAX buffer. Static, so it is
     * built under the bus mutex like the other shared buffers. */
    static uint8_t frame[(FUJI_HEADER_LEN + 4u + FUJI_PATH_LEN) * 2u + 2u];
    fuji_bus_acquire();
    size_t flen = fuji_build_frame_u8(device, command, params, nparams,
                                      tx_payload, tx_payload_len, frame, sizeof(frame));
    bool ok = transact_frame_locked(frame, flen, rx_payload, rx_cap, rx_len,
                                    retries, first_us);
    fuji_bus_release();
    return ok;
}

/* Cheap liveness probe: FUJICMD_GET_WIFISTATUS is a no-param request with a
 * one-byte reply (3=up, 6=down), so an ACK proves the SPI link is alive without
 * touching TNFS. */
bool fuji_link_ping(bool *wifi_up)
{
    fuji_link_init();
    uint8_t st = 0; size_t got = 0;
    if (!transact(FUJI_DEVICEID_FUJINET, FUJICMD_GET_WIFISTATUS, false, 0, NULL, 0,
                  &st, 1, &got, FUJI_RETRIES, FUJI_MOUNT_FIRST_US) || got < 1)
        return false;
    if (wifi_up) *wifi_up = (st == 3);
    return true;
}

bool fuji_read(uint8_t target, uint32_t lba, void *buf, uint32_t sector_count)
{
    fuji_link_init();
    uint8_t device = FUJI_DEVICEID_DISK + target;   /* 0x31 + target */
    uint8_t *p = buf;
    /* Issue READ_MULTI in runs of up to FUJI_MULTI_SECTORS: one request/reply
     * per run instead of per sector. param = (count<<24)|sector. */
    for (uint32_t done = 0; done < sector_count; ) {
        uint32_t sector = lba + done;
        uint32_t count  = sector_count - done;
        if (count > FUJI_MULTI_SECTORS) count = FUJI_MULTI_SECTORS;
        uint32_t param  = (count << 24) | (sector & 0x00FFFFFFu);
        size_t   want   = count * FUJI_SECTOR_SIZE;
        size_t   got    = 0;
        if (!transact(device, DISKCMD_READ_MULTI, true, param, NULL, 0,
                      p + done * FUJI_SECTOR_SIZE, want, &got,
                      FUJI_RETRIES, FUJI_FIRST_US)
            || got != want)
            return false;
        done += count;
    }
    return true;
}

bool fuji_write(uint8_t target, uint32_t lba, const void *buf, uint32_t sector_count)
{
    fuji_link_init();
    uint8_t device = FUJI_DEVICEID_DISK + target;   /* 0x31 + target */
    const uint8_t *p = buf;
    for (uint32_t s = 0; s < sector_count; s++) {
        uint32_t sector = lba + s;
        if (!transact(device, DISKCMD_PUT, true, sector,
                      p + s * FUJI_SECTOR_SIZE, FUJI_SECTOR_SIZE, NULL, 0, NULL,
                      FUJI_RETRIES, FUJI_FIRST_US))
            return false;
    }
    return true;
}

bool fuji_mount_all(void)
{
    fuji_link_init();
    for (int attempt = 0; attempt < FUJI_RETRIES; attempt++) {
        if (transact(FUJI_DEVICEID_FUJINET, FUJICMD_MOUNT_ALL, false, 0,
                     NULL, 0, NULL, 0, NULL, 1, FUJI_MOUNT_FIRST_US))
            return true;
        sleep_ms(100u * (unsigned)(attempt + 1));     /* backoff */
    }
    return false;
}

/* Point the FujiNet's disk device 0 at server file `name` (TNFS host slot 0):
 * UNMOUNT the slot, then SET_DEVICE_FULLPATH with the name in a 256-byte buffer.
 * NOTE: the ESP persists this selection to its fnconfig (Config.save), so it
 * also changes what an unattended remote boot serves. Returns true iff both ACK. */
bool fuji_select_image(const char *name)
{
    fuji_link_init();
    size_t len = strlen(name);
    if (len >= FUJI_PATH_LEN) return false;

    uint8_t slot0 = 0;
    if (!transact_u8(FUJI_DEVICEID_FUJINET, FUJICMD_UNMOUNT_IMAGE, &slot0, 1,
                     NULL, 0, NULL, 0, NULL, FUJI_RETRIES, FUJI_MOUNT_FIRST_US))
        return false;

    uint8_t path[FUJI_PATH_LEN];
    memset(path, 0, sizeof(path));
    memcpy(path, name, len);
    uint8_t params[3] = { 0, 0, 2 };   /* deviceSlot, hostSlot, DISK_ACCESS_MODE_WRITE */
    return transact_u8(FUJI_DEVICEID_FUJINET, FUJICMD_SET_DEVICE_FULLPATH, params, 3,
                       path, sizeof(path), NULL, 0, NULL,
                       FUJI_RETRIES, FUJI_MOUNT_FIRST_US);
}

/* List *.img on the FujiNet's TNFS host slot 0. OPEN_DIRECTORY "/" with a
 * "*.img" pattern, READ_DIR_ENTRY until the 0x7F 0x7F end marker (safety-capped),
 * CLOSE_DIRECTORY always. Each entry: u32 LE size at offset 6, NUL-padded name
 * at offset 12; directory entries (flags bit0) are skipped. emit() gets each
 * file's name + size. Returns false if OPEN or any READ fails (zero files = ok). */
bool fuji_dir_list(void (*emit)(void *ctx, const char *name, unsigned size), void *ctx)
{
    fuji_link_init();

    uint8_t hostslot = 0;
    uint8_t path[FUJI_PATH_LEN];
    memset(path, 0, sizeof(path));
    path[0] = '/'; path[1] = 0;                 /* dir NUL-terminated, then pattern */
    memcpy(path + 2, "*.img", 5);
    if (!transact_u8(FUJI_DEVICEID_FUJINET, FUJICMD_OPEN_DIRECTORY, &hostslot, 1,
                     path, sizeof(path), NULL, 0, NULL,
                     FUJI_RETRIES, FUJI_MOUNT_FIRST_US))
        return false;

    bool ok = true;
    for (int i = 0; i < 64; i++) {              /* safety cap */
        uint8_t params[2] = { 128, 0x80 };      /* maxlen, addtl */
        uint8_t ent[128];
        size_t got = 0;
        if (!transact_u8(FUJI_DEVICEID_FUJINET, FUJICMD_READ_DIR_ENTRY, params, 2,
                         NULL, 0, ent, sizeof(ent), &got,
                         FUJI_RETRIES, FUJI_MOUNT_FIRST_US) || got < 12) {
            ok = false; break;
        }
        if (ent[0] == 0x7Fu && ent[1] == 0x7Fu) break;   /* end of directory */
        if (ent[10] & 0x01u) continue;                   /* skip directories */
        uint32_t size = (uint32_t)ent[6] | ((uint32_t)ent[7] << 8) |
                        ((uint32_t)ent[8] << 16) | ((uint32_t)ent[9] << 24);
        char name[128 - 12 + 1];
        size_t nl = got - 12;
        if (nl > sizeof(name) - 1) nl = sizeof(name) - 1;
        memcpy(name, ent + 12, nl);
        name[nl] = 0;                            /* force termination */
        emit(ctx, name, size);
    }

    transact(FUJI_DEVICEID_FUJINET, FUJICMD_CLOSE_DIRECTORY, false, 0,
             NULL, 0, NULL, 0, NULL, FUJI_RETRIES, FUJI_FIRST_US);
    return ok;
}

/* Stat one server file: OPEN_DIRECTORY "/" with the filename itself as the
 * search pattern, one READ_DIR_ENTRY, CLOSE_DIRECTORY always. On a matching
 * entry, *size gets the u32 LE byte size at offset 6; the 0x7F 0x7F end marker
 * as the first entry means no such file. See fuji_stat() in fuji_blkdev.h. */
const char *fuji_stat(const char *name, uint32_t *size)
{
    fuji_link_init();
    /* -2: the pattern sits after "/\0", so the name must fit at path[2..]. */
    if (strlen(name) >= FUJI_PATH_LEN - 2) return "name too long";

    uint8_t hostslot = 0;
    uint8_t path[FUJI_PATH_LEN];
    memset(path, 0, sizeof(path));
    path[0] = '/'; path[1] = 0;                 /* dir NUL-terminated, then pattern */
    memcpy(path + 2, name, strlen(name));
    if (!transact_u8(FUJI_DEVICEID_FUJINET, FUJICMD_OPEN_DIRECTORY, &hostslot, 1,
                     path, sizeof(path), NULL, 0, NULL,
                     FUJI_RETRIES, FUJI_MOUNT_FIRST_US))
        return "FujiNet unreachable";

    const char *err = NULL;
    uint8_t params[2] = { 128, 0x80 };          /* maxlen, addtl */
    uint8_t ent[128];
    size_t got = 0;
    if (!transact_u8(FUJI_DEVICEID_FUJINET, FUJICMD_READ_DIR_ENTRY, params, 2,
                     NULL, 0, ent, sizeof(ent), &got,
                     FUJI_RETRIES, FUJI_MOUNT_FIRST_US) || got < 12)
        err = "FujiNet unreachable";
    else if (ent[0] == 0x7Fu && ent[1] == 0x7Fu)
        err = "not found on server";
    else
        *size = (uint32_t)ent[6] | ((uint32_t)ent[7] << 8) |
                ((uint32_t)ent[8] << 16) | ((uint32_t)ent[9] << 24);

    transact(FUJI_DEVICEID_FUJINET, FUJICMD_CLOSE_DIRECTORY, false, 0,
             NULL, 0, NULL, 0, NULL, FUJI_RETRIES, FUJI_FIRST_US);
    return err;
}

/* ---- FujiNet control: WiFi + TNFS host config --------------------------- */
/* Wire structs (fujiDevice.h): SSIDConfig = char ssid[33] + char password[64];
 * SSIDInfo = char ssid[33] + uint8_t rssi. */

const char *fuji_wifi_get(char ssid[33], bool *connected)
{
    fuji_link_init();
    uint8_t rep[97];                                  /* SSIDConfig */
    size_t got = 0;
    if (!transact(FUJI_DEVICEID_FUJINET, FUJICMD_GET_SSID, false, 0, NULL, 0,
                  rep, sizeof(rep), &got, FUJI_RETRIES, FUJI_MOUNT_FIRST_US) || got < 33)
        return "FujiNet unreachable";
    memcpy(ssid, rep, 33); ssid[32] = 0;              /* password (rep[33..]) ignored */

    uint8_t st = 0; got = 0;
    if (!transact(FUJI_DEVICEID_FUJINET, FUJICMD_GET_WIFISTATUS, false, 0, NULL, 0,
                  &st, 1, &got, FUJI_RETRIES, FUJI_MOUNT_FIRST_US) || got < 1)
        return "FujiNet unreachable";
    *connected = (st == 3);                            /* 3=connected, 6=disconnected */
    return NULL;
}

const char *fuji_wifi_set(const char *ssid, const char *pass)
{
    fuji_link_init();
    if (strlen(ssid) > 32) return "network name too long (max 32)";
    if (strlen(pass) > 63) return "password too long (max 63)";

    uint8_t cfg[97];                                  /* ssid[33] + password[64] */
    memset(cfg, 0, sizeof(cfg));
    memcpy(cfg,      ssid, strlen(ssid));
    memcpy(cfg + 33, pass, strlen(pass));
    uint8_t save = 1;
    /* Single attempt, long reply wait: the ESP connects before it acks. */
    if (!transact_u8(FUJI_DEVICEID_FUJINET, FUJICMD_SET_SSID, &save, 1,
                     cfg, sizeof(cfg), NULL, 0, NULL, 1, FUJI_WIFI_FIRST_US))
        return "join failed (bad password?)";
    return NULL;
}

const char *fuji_wifi_scan(unsigned *count)
{
    fuji_link_init();
    uint8_t n = 0; size_t got = 0;
    /* Single attempt, long reply wait: the ESP runs a live scan before it acks. */
    if (!transact(FUJI_DEVICEID_FUJINET, FUJICMD_SCAN_NETWORKS, false, 0, NULL, 0,
                  &n, 1, &got, 1, FUJI_WIFI_FIRST_US) || got < 1)
        return "FujiNet unreachable";
    *count = n;
    return NULL;
}

const char *fuji_wifi_scan_result(unsigned idx, char ssid[33], int *rssi)
{
    fuji_link_init();
    uint8_t p = (uint8_t)idx;
    uint8_t rep[34]; size_t got = 0;                  /* SSIDInfo */
    if (!transact_u8(FUJI_DEVICEID_FUJINET, FUJICMD_GET_SCAN_RESULT, &p, 1,
                     NULL, 0, rep, sizeof(rep), &got, FUJI_RETRIES, FUJI_MOUNT_FIRST_US)
        || got < 34)
        return "FujiNet unreachable";
    memcpy(ssid, rep, 33); ssid[32] = 0;
    *rssi = (int)(int8_t)rep[33];                      /* signed dBm in a uint8 */
    return NULL;
}
