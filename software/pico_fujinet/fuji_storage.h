/* fuji_storage.h
 * FujiNet storage backend: binds the SPI1 FujiNet transport (fuji_blkdev.c) to
 * the storage_ops_t vtable as STORAGE_BACKEND_FUJINET. Registered like the SD
 * backend; a target mounted here routes its read/write/sync through fuji_*.
 *
 * Execution contract: like SD, every op BLOCKS on the shared SPI1 bus and must
 * run on core 1. Registering touches no SPI (safe from either core, any time).
 */
#ifndef V9K_FUJI_STORAGE_H
#define V9K_FUJI_STORAGE_H

#ifdef __cplusplus
extern "C" {
#endif

// Register the FujiNet backend with the storage layer (no SPI touched).
// Call before storage_init(STORAGE_BACKEND_FUJINET) / storage_mount_on().
void fuji_storage_register(void);

#ifdef __cplusplus
}
#endif

#endif // V9K_FUJI_STORAGE_H
