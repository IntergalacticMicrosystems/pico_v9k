/* sd_storage.h
 * SD Card storage backend for Victor 9000 DMA board firmware
 * Uses no-OS-FatFS-SD-SDIO-SPI-RPi-Pico library for SD card access
 */
#ifndef SD_STORAGE_H
#define SD_STORAGE_H

#include <stdint.h>
#include <stdbool.h>
#include "storage.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the SD card storage backend and register it with the storage layer
// This should be called early in main() before storage_init()
void sd_storage_register(void);

// Get the file path for a mounted target (for debugging)
const char* sd_storage_get_image_path(uint8_t target_id);

// Get the number of discovered disk images on the SD card
int sd_storage_get_image_count(void);

// Get the filename of a discovered disk image by index
const char* sd_storage_get_image_name(int index);

// Walk the SD root directory for *.img files, invoking emit(ctx, name, size)
// for each (size in bytes). No-op if the SD backend is not initialized (no
// card): the console then lists FujiNet images only. Blocks on FatFS/SPI, so
// call only on core 1 (the storage core). See console/console_ops.c.
void sd_storage_list_images(void (*emit)(void *ctx, const char *name, unsigned size),
                            void *ctx);

#ifdef __cplusplus
}
#endif

#endif // SD_STORAGE_H
