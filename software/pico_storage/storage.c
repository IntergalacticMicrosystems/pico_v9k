/* storage.c
 * Storage abstraction layer implementation.
 *
 * Per-target backend routing: an SD boot drive and FujiNet drives coexist on
 * different SASI targets. Every target records which backend mounted it
 * (target_backend[]); all I/O dispatches on that entry, so target 0 can be SD
 * while target 1 is FujiNet. storage_init() still designates a primary/default
 * backend used by the plain storage_mount() (the boot auto-mount loop).
 */

#include <stdio.h>
#include <string.h>
#include "storage.h"

// Storage backend registry (indexed by storage_backend_t: NONE/FUJINET/SDCARD)
static const storage_ops_t *backend_ops[3] = {NULL, NULL, NULL};
static bool backend_inited[3] = {false, false, false};
static storage_backend_t active_backend = STORAGE_BACKEND_NONE;

// Which backend serves each SASI target (NONE = unmounted).
static storage_backend_t target_backend[STORAGE_MAX_TARGETS];

void storage_register_backend(storage_backend_t type, const storage_ops_t *ops) {
    if (type > STORAGE_BACKEND_NONE && type <= STORAGE_BACKEND_SDCARD) {
        backend_ops[type] = ops;
    }
}

// Run a backend's init() once (idempotent). Does NOT change the primary backend
// so a console-driven FujiNet mount cannot hijack the plain storage_mount path.
static bool backend_ensure_inited(storage_backend_t backend) {
    if (backend == STORAGE_BACKEND_NONE) {
        printf("Storage: no backend specified\n");
        return false;
    }
    if (backend_inited[backend]) {
        return true;
    }

    const storage_ops_t *ops = backend_ops[backend];
    if (!ops) {
        printf("Storage: backend %d not registered\n", backend);
        return false;
    }
    if (!ops->init) {
        printf("Storage: backend %d has no init function\n", backend);
        return false;
    }
    if (!ops->init()) {
        printf("Storage: failed to initialize backend %d\n", backend);
        return false;
    }

    backend_inited[backend] = true;
    printf("Storage: initialized backend %d\n", backend);
    return true;
}

bool storage_init(storage_backend_t backend) {
    if (!backend_ensure_inited(backend)) {
        return false;
    }
    // Designate this as the primary/default backend for plain storage_mount().
    active_backend = backend;
    return true;
}

storage_backend_t storage_get_backend(void) {
    return active_backend;
}

storage_backend_t storage_get_target_backend(uint8_t target_id) {
    if (target_id >= STORAGE_MAX_TARGETS) {
        return STORAGE_BACKEND_NONE;
    }
    return target_backend[target_id];
}

// Resolve the ops table serving a mounted target.
static const storage_ops_t *ops_for_target(uint8_t target_id) {
    if (target_id >= STORAGE_MAX_TARGETS) {
        return NULL;
    }
    storage_backend_t backend = target_backend[target_id];
    if (backend == STORAGE_BACKEND_NONE) {
        return NULL;
    }
    return backend_ops[backend];
}

bool storage_mount_on(storage_backend_t backend, uint8_t target_id,
                      const char *image_path, bool read_only) {
    if (target_id >= STORAGE_MAX_TARGETS) {
        return false;
    }
    if (!backend_ensure_inited(backend)) {
        return false;
    }

    const storage_ops_t *ops = backend_ops[backend];
    if (!ops || !ops->mount) {
        return false;
    }
    if (!ops->mount(target_id, image_path, read_only)) {
        return false;
    }

    target_backend[target_id] = backend;
    return true;
}

bool storage_mount(uint8_t target_id, const char *image_path, bool read_only) {
    return storage_mount_on(active_backend, target_id, image_path, read_only);
}

bool storage_unmount(uint8_t target_id) {
    const storage_ops_t *ops = ops_for_target(target_id);
    if (!ops || !ops->unmount) {
        return false;
    }
    bool ok = ops->unmount(target_id);
    target_backend[target_id] = STORAGE_BACKEND_NONE;
    return ok;
}

bool storage_read_sector(uint8_t target_id, uint32_t lba, uint8_t *buffer, size_t len) {
    const storage_ops_t *ops = ops_for_target(target_id);
    if (!ops || !ops->read_sector) {
        return false;
    }
    return ops->read_sector(target_id, lba, buffer, len);
}

bool storage_write_sector(uint8_t target_id, uint32_t lba, const uint8_t *buffer, size_t len) {
    const storage_ops_t *ops = ops_for_target(target_id);
    if (!ops || !ops->write_sector) {
        return false;
    }
    return ops->write_sector(target_id, lba, buffer, len);
}

bool storage_sync(uint8_t target_id) {
    const storage_ops_t *ops = ops_for_target(target_id);
    if (!ops) {
        return false;
    }
    if (!ops->sync) {
        return true;  // No sync needed for this backend
    }
    return ops->sync(target_id);
}

uint32_t storage_get_capacity(uint8_t target_id) {
    const storage_ops_t *ops = ops_for_target(target_id);
    if (!ops || !ops->get_capacity) {
        return 0;
    }
    return ops->get_capacity(target_id);
}

bool storage_is_mounted(uint8_t target_id) {
    const storage_ops_t *ops = ops_for_target(target_id);
    if (!ops || !ops->is_mounted) {
        return false;
    }
    return ops->is_mounted(target_id);
}
