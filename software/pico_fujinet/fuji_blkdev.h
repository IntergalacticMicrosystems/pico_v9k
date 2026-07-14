/* fuji_blkdev.h — FujiNet disk transport over the shared SPI1 bus (rev1 bench).
 *
 * Serves .img files from an ESP32-S3 (FeatherS3) running the FujiNet
 * BUILD_RS232 firmware as SASI targets. The transport is SPI1 master, SHARED
 * with the SD card, on GP42 (SCK) / GP43 (MOSI) / GP44 (MISO) with a plain-GPIO
 * chip select on GP45 and the ESP's data-ready handshake on GP46. The wire
 * protocol (SLIP/FujiBus framing) lives in fuji_proto.c; the SPI transaction
 * framing + bus arbitration are in fuji_blkdev.c.
 *
 * Ported from v9k_flop/firmware/fuji_blkdev.c (which had a dedicated spi0 link
 * and PSRAM buffers); here the bus is shared with the SD card and buffers are
 * static SRAM. Phase A is transport-only: the storage_ops_t vtable binding is
 * Phase B and is NOT declared here.
 *
 * Execution contract: everything here BLOCKS on the SPI link. All disk I/O must
 * run on core 1 (same as SD). Console-initiated ops (the 'F' bench command) run
 * on core 0 and must NOT overlap disk I/O — fuji_bus_acquire()/release() guard
 * the shared SPI1 bus but only switch owners between complete transactions. */
#ifndef V9K_FUJI_BLKDEV_H
#define V9K_FUJI_BLKDEV_H

#include <stdint.h>
#include <stdbool.h>

/* Bring the FujiNet SPI link up: CS/DRDY GPIO + the bus-share mutex. The SPI
 * data pins (GP42/43/44) are already muxed GPIO_FUNC_SPI by the SD driver, so
 * this does NOT re-init them. Idempotent. */
void fuji_link_init(void);

/* Cheap link-liveness probe for bench bring-up: query the ESP's WiFi status
 * (FUJICMD_GET_WIFISTATUS, a one-byte reply). Returns true if the ESP answered
 * (link is alive); *wifi_up (may be NULL) reports whether the radio is joined. */
bool fuji_link_ping(bool *wifi_up);

/* Sample the DRDY handshake line (GP46). High means the ESP slave is armed /
 * present. GPIO-input read only, so safe from either core. */
bool fuji_link_drdy(void);

/* Read `sector_count` 512-byte sectors starting at `lba` from SASI `target`
 * (device 0x31 + target) into `buf`. Batches into READ_MULTI runs of up to 16
 * sectors — one request/reply per run. Blocks; returns false on any I/O error. */
bool fuji_read(uint8_t target, uint32_t lba, void *buf, uint32_t sector_count);

/* Write `sector_count` 512-byte sectors starting at `lba` to SASI `target` with
 * a per-sector PUT (no PSRAM write-back cache on this board). Blocks; returns
 * false on any I/O error. */
bool fuji_write(uint8_t target, uint32_t lba, const void *buf, uint32_t sector_count);

/* Ask the FujiNet control device (0x70) to mount all its configured disk slots
 * (FUJICMD_MOUNT_ALL). Retries with backoff; returns true if acknowledged.
 * Call from the mount path, never at boot, so a missing ESP32 never stalls. */
bool fuji_mount_all(void);

/* Select which server file the FujiNet's disk device `slot` serves (TNFS host
 * slot 0), by name. `slot` = SASI target (device ID 0x31+slot). `read_only`
 * sends access mode READ instead of WRITE. Blocks. Returns false if the name is
 * too long or the ESP doesn't ACK. NOTE: the ESP persists this selection to its
 * fnconfig, so it also changes what an unattended remote boot serves. */
bool fuji_select_image(uint8_t slot, const char *name, bool read_only);

/* Unmount whatever the FujiNet's disk device `slot` serves. Blocks; ACK->true. */
bool fuji_unmount_slot(uint8_t slot);

/* Stat one server file on TNFS host slot 0: fill *size with its byte length and
 * return NULL, else a short reason string ("FujiNet unreachable", "not found on
 * server", "name too long"). Blocks. Lets a remote mount auto-detect geometry
 * from the size instead of taking an ss/ds argument. */
const char *fuji_stat(const char *name, uint32_t *size);

/* List *.img files on the FujiNet's TNFS host slot 0, calling emit() with each
 * file's name and byte size. Blocks. Returns true on a clean directory walk
 * (zero files included), false if the remote can't be reached / errors. */
bool fuji_dir_list(void (*emit)(void *ctx, const char *name, unsigned size), void *ctx);

/* ---- FujiNet control: WiFi + TNFS host config (blocking) ----------------- */
/* Each runs the request/reply transaction synchronously and returns NULL on
 * success, else a short reason for a console ERR reply. They talk to the ESP's
 * Fuji control device (0x70), which dispatches everything — no ESP-side change
 * is needed. */

/* GET_SSID + GET_WIFISTATUS: fill `ssid` (NUL-terminated) with the stored
 * network name and `*connected` with the live link state. */
const char *fuji_wifi_get(char ssid[33], bool *connected);

/* SET_SSID: connect+persist `ssid`/`pass` on the ESP. The ESP joins the network
 * before replying, so this can block for SECONDS. Rejects an over-long ssid
 * (>32) / pass (>63). */
const char *fuji_wifi_set(const char *ssid, const char *pass);

/* SCAN_NETWORKS: trigger a live scan (blocks several seconds) and report the
 * network count in `*count`. Pair with fuji_wifi_scan_result() per index. */
const char *fuji_wifi_scan(unsigned *count);

/* GET_SCAN_RESULT: one scanned network by index -> `ssid` (NUL-terminated) and
 * `*rssi` (signed dBm). */
const char *fuji_wifi_scan_result(unsigned idx, char ssid[33], int *rssi);

#endif /* V9K_FUJI_BLKDEV_H */
