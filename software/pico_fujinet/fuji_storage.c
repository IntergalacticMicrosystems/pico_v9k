/* fuji_storage.c
 * FujiNet storage backend (STORAGE_BACKEND_FUJINET). Mirrors sd_storage.c's
 * registration and vtable shape, but the media is a .img on the ESP32-S3's TNFS
 * host reached over the shared SPI1 link (fuji_blkdev.c).
 *
 * Execution contract: every vtable op BLOCKS on SPI1 and must run on core 1
 * (same as SD). Console-initiated ops reach here only via the core-1 mailbox
 * (console/console_ops.c) — never call these from core 0 directly. SASI target N maps
 * to ESP device slot N (device ID 0x31+N).
 */

#include <stdio.h>
#include <string.h>

#include "fuji_storage.h"
#include "fuji_blkdev.h"
#include "storage.h"

// Per-target state. capacity_sectors is informational only: sasi.c does not use
// storage_get_capacity() to bound FujiNet I/O (the ESP validates LBAs), so the
// boot-fallback mount with an unknown size (capacity 0) is fully functional.
typedef struct {
    bool     mounted;
    bool     read_only;
    uint32_t capacity_sectors;
} fuji_target_t;

static fuji_target_t s_targets[STORAGE_MAX_TARGETS];

static bool fuji_storage_init(void) {
    // Bring up the link and prove the ESP answers so callers report "no
    // FujiNet" cleanly instead of hanging every later transaction on timeouts.
    fuji_link_init();
    if (!fuji_link_ping(NULL)) {
        printf("FujiNet Storage: no ESP32 on the SPI link (ping failed)\n");
        return false;
    }
    printf("FujiNet Storage: link up\n");
    return true;
}

static bool fuji_storage_mount(uint8_t target_id, const char *image_path, bool read_only) {
    if (target_id >= STORAGE_MAX_TARGETS) {
        printf("FujiNet Storage: invalid target ID %d\n", target_id);
        return false;
    }

    uint32_t capacity = 0;

    if (image_path && image_path[0]) {
        // Explicit image: stat for capacity, then bind it to this device slot.
        uint32_t size = 0;
        const char *err = fuji_stat(image_path, &size);
        if (err) {
            printf("FujiNet Storage: stat '%s': %s\n", image_path, err);
            return false;
        }
        capacity = size / STORAGE_SECTOR_SIZE;

        if (!fuji_select_image(target_id, image_path, read_only)) {
            printf("FujiNet Storage: select '%s' on slot %d failed\n",
                   image_path, target_id);
            return false;
        }
    }
    // else: boot fallback — mount whatever the ESP already has configured for
    // its slots; capacity stays 0 (unused, see fuji_target_t above).

    if (!fuji_mount_all()) {
        printf("FujiNet Storage: MOUNT_ALL failed\n");
        return false;
    }

    s_targets[target_id].mounted = true;
    s_targets[target_id].read_only = read_only;
    s_targets[target_id].capacity_sectors = capacity;
    printf("FujiNet Storage: mounted target %d (%s%s)\n", target_id,
           (image_path && image_path[0]) ? image_path : "preconfigured slots",
           read_only ? ", read-only" : "");
    return true;
}

static bool fuji_storage_unmount(uint8_t target_id) {
    if (target_id >= STORAGE_MAX_TARGETS) {
        return false;
    }
    if (!s_targets[target_id].mounted) {
        return true;  // already unmounted
    }

    bool ok = fuji_unmount_slot(target_id);
    s_targets[target_id].mounted = false;
    s_targets[target_id].capacity_sectors = 0;
    printf("FujiNet Storage: unmounted target %d\n", target_id);
    return ok;
}

static bool fuji_storage_read_sector(uint8_t target_id, uint32_t lba, uint8_t *buffer, size_t len) {
    if (target_id >= STORAGE_MAX_TARGETS || !s_targets[target_id].mounted) {
        return false;
    }
    if (len != STORAGE_SECTOR_SIZE) {
        return false;  // one 512-byte sector per call (see header)
    }
    // Single-sector reads for now: multi-sector READ_MULTI batching through
    // sasi.c is deferred to a perf pass — the transport already supports it.
    return fuji_read(target_id, lba, buffer, 1);
}

static bool fuji_storage_write_sector(uint8_t target_id, uint32_t lba, const uint8_t *buffer, size_t len) {
    if (target_id >= STORAGE_MAX_TARGETS || !s_targets[target_id].mounted) {
        return false;
    }
    if (s_targets[target_id].read_only) {
        printf("FujiNet Storage: target %d is read-only\n", target_id);
        return false;
    }
    if (len != STORAGE_SECTOR_SIZE) {
        return false;
    }
    return fuji_write(target_id, lba, buffer, 1);
}

static bool fuji_storage_sync(uint8_t target_id) {
    // Writes are synchronous per-sector PUTs (no write-back cache), so there is
    // nothing to flush.
    (void)target_id;
    return true;
}

static uint32_t fuji_storage_get_capacity(uint8_t target_id) {
    if (target_id >= STORAGE_MAX_TARGETS || !s_targets[target_id].mounted) {
        return 0;
    }
    return s_targets[target_id].capacity_sectors;
}

static bool fuji_storage_is_mounted(uint8_t target_id) {
    if (target_id >= STORAGE_MAX_TARGETS) {
        return false;
    }
    return s_targets[target_id].mounted;
}

static const storage_ops_t fuji_ops = {
    .init = fuji_storage_init,
    .mount = fuji_storage_mount,
    .unmount = fuji_storage_unmount,
    .read_sector = fuji_storage_read_sector,
    .write_sector = fuji_storage_write_sector,
    .sync = fuji_storage_sync,
    .get_capacity = fuji_storage_get_capacity,
    .is_mounted = fuji_storage_is_mounted,
};

void fuji_storage_register(void) {
    storage_register_backend(STORAGE_BACKEND_FUJINET, &fuji_ops);
}
