/*
 * dma_vfy2.c - Bench-safe DMA Transfer Verification Test (DMAVFY2.EXE)
 *
 * Bench-safe variant of dma_verify.c.  It talks to the same SASI/DMA
 * controller emulated at segment 0xEF30 and reuses the register protocol,
 * read_sector/write_sector state machine, CRC8, polling logic and
 * reset_board from the original.  The differences are all about NOT
 * relying on stale baked CRCs and NOT corrupting the only hard-disk image:
 *
 *   1. READ test  -> self-consistency: each LBA is read 3 times and the
 *                    three CRC8 values must be identical.  The original
 *                    baked expected_crc8 table is kept and printed as a
 *                    "ref-only" cross-check (LBAs 0, 2, 68), but a mismatch
 *                    against it is NOT a failure.
 *
 *   2. WRITE test -> save/restore: for each scratch LBA the original sector
 *                    is read and saved, the test pattern is written and
 *                    verified, then the ORIGINAL content is written back and
 *                    re-verified.  Any error still attempts a restore, and a
 *                    sector whose restore cannot be verified is reported
 *                    loudly.
 *
 *   3. 64K-boundary test -> a single-sector DMA read whose 512-byte target
 *                    buffer straddles a 64 KB physical address boundary
 *                    (the 20-bit DMA address must carry across e.g.
 *                    0x1FFFF->0x20000 or 0x2FFFF->0x30000 mid-transfer).
 *                    The straddling read's CRC8 is compared against the same
 *                    LBA read into a non-straddling buffer.  The buffer is a
 *                    far heap allocation we own, so this test never touches
 *                    DOS memory or the disk.  The actual 20-bit physical
 *                    start address and the boundary are always printed.
 *
 * main() runs read test, then boundary test, then write test, so that if the
 * write test wedges the board we still have the other results.  A final
 * "DMAVFY2 SUMMARY:" block lists counts per test.
 *
 * Build (known-good on this box):
 *   source /opt/watcom/owsetenv.sh
 *   export LIB=$WATCOM/lib286/dos
 *   wcc -bt=dos -ms -0 -os -zq -za99 -d0 -fo=dma_vfy2.obj dma_vfy2.c
 *   wlink format dos LIBPATH $WATCOM/lib286/dos LIBPATH $WATCOM/lib286 \
 *         name DMAVFY2.EXE file dma_vfy2.obj
 */

#include <conio.h>
#include <dos.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

/* DMA board register addresses */
#define DMA_BASE        0xEF30
#define DMA_REG(offset) ((volatile unsigned char far *)(MK_FP(DMA_BASE, offset)))

/* Register offsets */
#define REG_CONTROL     0x00
#define REG_DATA        0x10
#define REG_STATUS      0x20
#define REG_ADDR_L      0x80
#define REG_ADDR_M      0xA0
#define REG_ADDR_H      0xC0

/* Status register phase bits */
#define STATUS_PHASE_MASK   0x1F
#define PHASE_IDLE          0x00
#define PHASE_BSY           0x04
#define PHASE_COMMAND       0x0E    /* BSY|REQ|CTL */
#define PHASE_STATUS        0x0F    /* BSY|REQ|CTL|INP */
#define PHASE_MESSAGE       0x1F    /* BSY|REQ|CTL|INP|MSG */
#define PHASE_DATA_IN       0x06    /* BSY|INP (data from device) */

/* Control register bits */
#define CTRL_DMA_ENABLE     0x01
#define CTRL_DMA_LATCH      0x02
#define CTRL_DMA_WRITE      0x04    /* 1=write to memory (read from disk) */
#define CTRL_SELECT         0x10
#define CTRL_RESET          0x20

/* Poll limits */
#define MAX_POLL_ITERATIONS 50000U
#define SECTOR_SIZE         512

/* Test configuration */
#define NUM_TEST_SECTORS 10
#define NUM_WRITE_TEST_SECTORS 5
#define NUM_READS_PER_LBA 3

/*
 * Standard DMA scratch buffer address (expansion RAM), same as the original
 * dma_verify.c with USE_EXPANSION_RAM=1.  Used by the read and write tests.
 * The 64K-boundary test allocates its own owned far buffer instead.
 */
#define EXPANSION_RAM_ADDR 0x2FFFF

/* Write test uses scratch sectors that won't corrupt the file system */
static const uint16_t write_test_lbas[NUM_WRITE_TEST_SECTORS] = {
    200, 201, 202, 203, 204
};

/* LBA numbers to test - selected for varied content */
static const uint16_t test_lbas[NUM_TEST_SECTORS] = {
    0, 2, 3, 7, 11, 68, 79, 95, 100, 104
};

/*
 * Expected CRC8 values from the original disk image (ref-only cross-check).
 * These are NOT used to pass/fail; they are printed next to the measured CRC
 * so LBAs 0, 2 and 68 can be manually cross-checked against the original.
 */
static const uint8_t expected_crc8[NUM_TEST_SECTORS] = {
    0xF9,  /* LBA   0: drive label */
    0xDE,  /* LBA   2: volume label */
    0xFD,  /* LBA   3: FAT */
    0xFD,  /* LBA   7: FAT copy */
    0xDD,  /* LBA  11: root directory */
    0x01,  /* LBA  68: BIOS boot code start */
    0xB7,  /* LBA  79: boot code */
    0x76,  /* LBA  95: boot code */
    0x84,  /* LBA 100: boot code */
    0xC7,  /* LBA 104: boot code */
};

/* LBA used for the 64K-boundary test (stable boot-code sector) */
#define BOUNDARY_TEST_LBA 68

/* Local storage for the write test's save/restore (kept out of the stack) */
static uint8_t save_buffer[SECTOR_SIZE];

/* ---- Read test results -------------------------------------------------- */
typedef struct {
    uint16_t lba;
    uint8_t  crc[NUM_READS_PER_LBA];
    uint8_t  consistent;    /* all reads produced identical CRC */
    uint8_t  phase_error;   /* non-zero if any read failed its phases */
    uint16_t poll_count;
} read_result_t;

static read_result_t read_results[NUM_TEST_SECTORS];

/* ---- Write test results ------------------------------------------------- */
typedef struct {
    uint16_t lba;
    uint8_t  pattern_ok;        /* pattern read back correctly */
    uint8_t  restore_ok;        /* original content restored & verified */
    uint8_t  had_error;         /* any phase error occurred */
} write_result_t;

static write_result_t write_results[NUM_WRITE_TEST_SECTORS];

/*
 * CRC-8 calculation using polynomial 0x07 (x^8 + x^2 + x + 1).
 * (Verbatim from dma_verify.c.)
 */
static uint8_t crc8(const uint8_t *data, uint16_t len) {
    uint8_t crc = 0x00;
    uint16_t i;
    uint8_t j;

    for (i = 0; i < len; i++) {
        crc ^= data[i];
        for (j = 0; j < 8; j++) {
            if (crc & 0x80) {
                crc = (crc << 1) ^ 0x07;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

/* Direct register access functions (verbatim). */
static void dma_write(unsigned int offset, unsigned char value) {
    *DMA_REG(offset) = value;
}

static unsigned char dma_read(unsigned int offset) {
    return *DMA_REG(offset);
}

/*
 * Poll status register until expected phase or timeout. (Verbatim.)
 * Returns number of iterations, or 0xFFFF on timeout.
 */
static uint16_t poll_status(uint8_t expected) {
    uint16_t iterations = 0;
    uint8_t actual;

    while (iterations < MAX_POLL_ITERATIONS) {
        actual = dma_read(REG_STATUS) & STATUS_PHASE_MASK;
        iterations++;
        if (actual == expected) {
            return iterations;
        }
    }
    return 0xFFFF;  /* Timeout */
}

/*
 * Set up DMA address registers. (Verbatim.)
 * Address is a 20-bit physical address split across three registers.
 */
static void set_dma_address(uint32_t addr) {
    dma_write(REG_ADDR_L, (uint8_t)(addr & 0xFF));
    dma_write(REG_ADDR_M, (uint8_t)((addr >> 8) & 0xFF));
    dma_write(REG_ADDR_H, (uint8_t)((addr >> 16) & 0x0F));
}

/*
 * Physical-address memory access helpers.  A 20-bit physical address is
 * converted to seg:off with seg = phys>>4, off = phys&0x0F so the pointer is
 * recomputed per byte and correctly carries across 64K segment boundaries.
 */
static uint8_t read_phys_byte(uint32_t phys) {
    uint16_t seg = (uint16_t)(phys >> 4);
    uint16_t off = (uint16_t)(phys & 0x0F);
    volatile uint8_t far *p = (volatile uint8_t far *)MK_FP(seg, off);
    return *p;
}

static void write_phys_byte(uint32_t phys, uint8_t value) {
    uint16_t seg = (uint16_t)(phys >> 4);
    uint16_t off = (uint16_t)(phys & 0x0F);
    volatile uint8_t far *p = (volatile uint8_t far *)MK_FP(seg, off);
    *p = value;
}

/* CRC8 over SECTOR_SIZE bytes at a physical address. */
static uint8_t crc8_phys(uint32_t phys, uint16_t len) {
    uint8_t crc = 0x00;
    uint16_t i;
    uint8_t j;

    for (i = 0; i < len; i++) {
        crc ^= read_phys_byte(phys + i);
        for (j = 0; j < 8; j++) {
            if (crc & 0x80) {
                crc = (crc << 1) ^ 0x07;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

static void copy_phys_to_local(uint32_t phys, uint8_t *local, uint16_t len) {
    uint16_t i;
    for (i = 0; i < len; i++) {
        local[i] = read_phys_byte(phys + i);
    }
}

static void copy_local_to_phys(uint32_t phys, const uint8_t *local, uint16_t len) {
    uint16_t i;
    for (i = 0; i < len; i++) {
        write_phys_byte(phys + i, local[i]);
    }
}

/* Fill 512 bytes at phys with the deterministic per-LBA pattern. */
static void fill_pattern_phys(uint32_t phys, uint32_t lba) {
    uint16_t i;
    for (i = 0; i < SECTOR_SIZE; i++) {
        write_phys_byte(phys + i, (uint8_t)((lba + i) & 0xFF));
    }
}

/* Rolling CRC8 of the per-LBA pattern - no 512-byte stack buffer needed. */
static uint8_t crc8_pattern(uint32_t lba) {
    uint8_t crc = 0x00;
    uint16_t i;
    uint8_t j;

    for (i = 0; i < SECTOR_SIZE; i++) {
        crc ^= (uint8_t)((lba + i) & 0xFF);
        for (j = 0; j < 8; j++) {
            if (crc & 0x80) {
                crc = (crc << 1) ^ 0x07;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

/*
 * Issue a SASI READ(6) command to read one sector into dma_addr.
 * Returns 0 on success, non-zero on error.
 * (Body verbatim from dma_verify.c except the DMA target address is now a
 * parameter instead of get_buffer_address().)
 */
static int read_sector(uint32_t lba, uint32_t dma_addr, uint16_t *poll_count) {
    uint16_t polls;
    uint8_t status_byte;

    *poll_count = 0;

    /* Step 1: Configure DMA for write-to-memory mode */
    dma_write(REG_CONTROL, 0x08);           /* Clear DMA enable */
    dma_write(REG_CONTROL, 0x0C);           /* Set DMA write mode (device->memory) */

    /* Step 2: Set DMA address */
    set_dma_address(dma_addr);

    /* Step 3: Enable DMA */
    dma_write(REG_CONTROL, 0x09);           /* DMA enable + latch */
    dma_write(REG_CONTROL, 0x0D);           /* DMA enable + write mode */

    /* Step 4: Wait for bus idle */
    polls = poll_status(PHASE_IDLE);
    if (polls == 0xFFFF) {
        return 1;  /* Timeout waiting for idle */
    }
    *poll_count += polls;

    /* Step 5: Select target (target ID = 1) */
    dma_write(REG_DATA, 0x01);
    dma_write(REG_CONTROL, 0x1D);           /* Select + DMA enable + write */

    /* Step 6: Wait for BSY */
    polls = poll_status(PHASE_BSY);
    if (polls == 0xFFFF) {
        return 2;  /* Timeout waiting for BSY */
    }
    *poll_count += polls;

    /* Step 7: Drop select, keep DMA enabled */
    dma_write(REG_CONTROL, 0x0D);

    /* Step 8: Wait for command phase */
    polls = poll_status(PHASE_COMMAND);
    if (polls == 0xFFFF) {
        return 3;  /* Timeout waiting for command phase */
    }
    *poll_count += polls;

    /* Step 9: Send READ(6) CDB - 6 bytes */
    /* CDB[0]: opcode 0x08 = READ(6) */
    dma_write(REG_DATA, 0x08);
    polls = poll_status(PHASE_COMMAND);
    if (polls == 0xFFFF) return 4;
    *poll_count += polls;

    /* CDB[1]: LUN (bits 7-5) + LBA high (bits 4-0) */
    dma_write(REG_DATA, (uint8_t)((lba >> 16) & 0x1F));
    polls = poll_status(PHASE_COMMAND);
    if (polls == 0xFFFF) return 5;
    *poll_count += polls;

    /* CDB[2]: LBA middle byte */
    dma_write(REG_DATA, (uint8_t)((lba >> 8) & 0xFF));
    polls = poll_status(PHASE_COMMAND);
    if (polls == 0xFFFF) return 6;
    *poll_count += polls;

    /* CDB[3]: LBA low byte */
    dma_write(REG_DATA, (uint8_t)(lba & 0xFF));
    polls = poll_status(PHASE_COMMAND);
    if (polls == 0xFFFF) return 7;
    *poll_count += polls;

    /* CDB[4]: Transfer length = 1 sector */
    dma_write(REG_DATA, 0x01);
    polls = poll_status(PHASE_COMMAND);
    if (polls == 0xFFFF) return 8;
    *poll_count += polls;

    /* CDB[5]: Control byte = 0 */
    dma_write(REG_DATA, 0x00);

    /* Step 10: Wait for status phase (DMA transfer happens automatically) */
    polls = poll_status(PHASE_STATUS);
    if (polls == 0xFFFF) {
        return 9;  /* Timeout waiting for status (DMA may have failed) */
    }
    *poll_count += polls;

    /* Step 11: Read status byte (should be 0x00 = GOOD) */
    status_byte = dma_read(REG_DATA);
    if (status_byte != 0x00) {
        return 10;  /* Bad status */
    }

    /* Step 12: Wait for message phase */
    polls = poll_status(PHASE_MESSAGE);
    if (polls == 0xFFFF) {
        return 11;  /* Timeout waiting for message */
    }
    *poll_count += polls;

    /* Step 13: Read message byte (should be 0x00 = COMMAND COMPLETE) */
    status_byte = dma_read(REG_DATA);
    if (status_byte != 0x00) {
        return 12;  /* Bad message */
    }

    /* Success! Data should now be at dma_addr */
    return 0;
}

/*
 * Issue a SASI WRITE(6) command to write one sector from dma_addr.
 * Returns 0 on success, non-zero on error.
 * (Body verbatim from dma_verify.c except the DMA source address is now a
 * parameter instead of get_buffer_address().)
 */
static int write_sector(uint32_t lba, uint32_t dma_addr, uint16_t *poll_count) {
    uint16_t polls;
    uint8_t status_byte;

    *poll_count = 0;

    /* Step 1: Configure DMA for read-from-memory mode (host->device) */
    dma_write(REG_CONTROL, 0x08);           /* Clear DMA enable */
    dma_write(REG_CONTROL, 0x08);           /* DMA read mode (memory->device), no write bit */

    /* Step 2: Set DMA address */
    set_dma_address(dma_addr);

    /* Step 3: Enable DMA */
    dma_write(REG_CONTROL, 0x09);           /* DMA enable + latch */
    dma_write(REG_CONTROL, 0x09);           /* DMA enable, read mode */

    /* Step 4: Wait for bus idle */
    polls = poll_status(PHASE_IDLE);
    if (polls == 0xFFFF) {
        return 1;  /* Timeout waiting for idle */
    }
    *poll_count += polls;

    /* Step 5: Select target (target ID = 1) */
    dma_write(REG_DATA, 0x01);
    dma_write(REG_CONTROL, 0x19);           /* Select + DMA enable */

    /* Step 6: Wait for BSY */
    polls = poll_status(PHASE_BSY);
    if (polls == 0xFFFF) {
        return 2;  /* Timeout waiting for BSY */
    }
    *poll_count += polls;

    /* Step 7: Drop select, keep DMA enabled */
    dma_write(REG_CONTROL, 0x09);

    /* Step 8: Wait for command phase */
    polls = poll_status(PHASE_COMMAND);
    if (polls == 0xFFFF) {
        return 3;  /* Timeout waiting for command phase */
    }
    *poll_count += polls;

    /* Step 9: Send WRITE(6) CDB - 6 bytes */
    /* CDB[0]: opcode 0x0A = WRITE(6) */
    dma_write(REG_DATA, 0x0A);
    polls = poll_status(PHASE_COMMAND);
    if (polls == 0xFFFF) return 4;
    *poll_count += polls;

    /* CDB[1]: LUN (bits 7-5) + LBA high (bits 4-0) */
    dma_write(REG_DATA, (uint8_t)((lba >> 16) & 0x1F));
    polls = poll_status(PHASE_COMMAND);
    if (polls == 0xFFFF) return 5;
    *poll_count += polls;

    /* CDB[2]: LBA middle byte */
    dma_write(REG_DATA, (uint8_t)((lba >> 8) & 0xFF));
    polls = poll_status(PHASE_COMMAND);
    if (polls == 0xFFFF) return 6;
    *poll_count += polls;

    /* CDB[3]: LBA low byte */
    dma_write(REG_DATA, (uint8_t)(lba & 0xFF));
    polls = poll_status(PHASE_COMMAND);
    if (polls == 0xFFFF) return 7;
    *poll_count += polls;

    /* CDB[4]: Transfer length = 1 sector */
    dma_write(REG_DATA, 0x01);
    polls = poll_status(PHASE_COMMAND);
    if (polls == 0xFFFF) return 8;
    *poll_count += polls;

    /* CDB[5]: Control byte = 0 */
    dma_write(REG_DATA, 0x00);

    /* Step 10: Wait for status phase (DMA transfer happens automatically) */
    polls = poll_status(PHASE_STATUS);
    if (polls == 0xFFFF) {
        return 9;  /* Timeout waiting for status (DMA may have failed) */
    }
    *poll_count += polls;

    /* Step 11: Read status byte (should be 0x00 = GOOD) */
    status_byte = dma_read(REG_DATA);
    if (status_byte != 0x00) {
        return 10;  /* Bad status */
    }

    /* Step 12: Wait for message phase */
    polls = poll_status(PHASE_MESSAGE);
    if (polls == 0xFFFF) {
        return 11;  /* Timeout waiting for message */
    }
    *poll_count += polls;

    /* Step 13: Read message byte (should be 0x00 = COMMAND COMPLETE) */
    status_byte = dma_read(REG_DATA);
    if (status_byte != 0x00) {
        return 12;  /* Bad message */
    }

    /* Success! Data has been written to disk */
    return 0;
}

/*
 * Reset the DMA board to recover from errors. (Verbatim.)
 */
static void reset_board(void) {
    dma_write(REG_CONTROL, CTRL_RESET);
    dma_write(REG_CONTROL, 0x00);
    /* Small delay to let the board settle */
    {
        volatile uint16_t i;
        for (i = 0; i < 1000; i++) { }
    }
}

/*
 * TEST 1: READ self-consistency.
 * Read each LBA 3 times; PASS if all three CRC8 values are identical.
 * The baked expected_crc8 value is printed as ref-only.
 */
static void run_read_test(void) {
    int i, r;
    int err;
    uint16_t polls;
    uint32_t buf = EXPANSION_RAM_ADDR;

    printf("READ self-consistency test (%d sectors x %d reads)...\n\n",
           NUM_TEST_SECTORS, NUM_READS_PER_LBA);

    for (i = 0; i < NUM_TEST_SECTORS; i++) {
        read_results[i].lba = test_lbas[i];
        read_results[i].phase_error = 0;
        read_results[i].consistent = 0;
        read_results[i].poll_count = 0;

        for (r = 0; r < NUM_READS_PER_LBA; r++) {
            err = read_sector(test_lbas[i], buf, &polls);
            if (err != 0) {
                read_results[i].phase_error = (uint8_t)err;
                printf("  LBA %3u: PHASE ERROR %d on read %d (polls=%u)\n",
                       (unsigned)test_lbas[i], err, r + 1, polls);
                reset_board();
                break;
            }
            read_results[i].poll_count += polls;
            read_results[i].crc[r] = crc8_phys(buf, SECTOR_SIZE);
        }

        if (read_results[i].phase_error) {
            continue;
        }

        read_results[i].consistent =
            (read_results[i].crc[0] == read_results[i].crc[1] &&
             read_results[i].crc[1] == read_results[i].crc[2]) ? 1 : 0;

        if (read_results[i].consistent) {
            printf("  LBA %3u: PASS  crc=0x%02X  (ref-only expected=0x%02X)\n",
                   (unsigned)test_lbas[i], read_results[i].crc[0],
                   expected_crc8[i]);
        } else {
            printf("  LBA %3u: FAIL  inconsistent crc=0x%02X,0x%02X,0x%02X"
                   "  (ref-only expected=0x%02X)\n",
                   (unsigned)test_lbas[i],
                   read_results[i].crc[0], read_results[i].crc[1],
                   read_results[i].crc[2], expected_crc8[i]);
        }
    }
}

/*
 * TEST 2: 64K-boundary DMA straddle.
 * Returns 1=PASS, 0=FAIL, -1=SKIP.
 *
 * Allocates an owned far buffer, finds the next 64K multiple above its base,
 * and places a 512-byte straddling window at (boundary-256) so a single
 * sector read carries the 20-bit DMA address across the boundary mid-transfer.
 * A non-straddling reference read of the same LBA (at the buffer base) is
 * compared by CRC8.  Because the buffer is our own allocation, no DOS memory
 * or disk sector is ever touched.
 */
static int run_boundary_test(void) {
    /* Candidate sizes, largest first, to maximise the chance a 64K multiple
     * with room for the straddle window lands inside the buffer. */
    static const uint16_t sizes[] = { 0xE000, 0xC000, 0x8000, 0x4000, 0x2000 };
    const int nsizes = (int)(sizeof(sizes) / sizeof(sizes[0]));
    void far *buf = (void far *)0;
    uint16_t size = 0;
    uint32_t base = 0, top = 0, boundary = 0, straddle = 0, ref = 0;
    int found = 0;
    int i;
    int err;
    uint16_t polls;
    uint8_t crc_straddle, crc_ref;

    printf("\n64K-boundary DMA straddle test (LBA %u)...\n", BOUNDARY_TEST_LBA);

    for (i = 0; i < nsizes; i++) {
        size = sizes[i];
        buf = _fmalloc(size);
        if (buf == (void far *)0) {
            continue;
        }
        base = ((uint32_t)FP_SEG(buf) << 4) + (uint32_t)FP_OFF(buf);
        top = base + size;
        /* next 64K multiple strictly above base */
        boundary = (base & 0xF0000UL) + 0x10000UL;
        straddle = boundary - 256;   /* window [straddle, straddle+512) crosses at +256 */
        ref = base;                  /* reference window at buffer start */
        /* Require: straddle window fully inside buffer, and the reference
         * window at base is at least 512 below the boundary (so it does not
         * itself straddle). */
        if (straddle >= base &&
            (straddle + SECTOR_SIZE) <= top &&
            (base + SECTOR_SIZE) <= boundary) {
            found = 1;
            break;
        }
        _ffree(buf);
        buf = (void far *)0;
    }

    if (!found) {
        printf("  buffer base phys = 0x%05lX  size = 0x%04X\n",
               (unsigned long)base, (unsigned)size);
        printf("  next 64K boundary= 0x%05lX\n", (unsigned long)boundary);
        printf("  SKIP: no 64K multiple with room for a straddle window fell "
               "inside an owned buffer.\n");
        if (buf != (void far *)0) {
            _ffree(buf);
        }
        return -1;
    }

    printf("  buffer base phys = 0x%05lX  size = 0x%04X  top = 0x%05lX\n",
           (unsigned long)base, (unsigned)size, (unsigned long)top);
    printf("  64K boundary     = 0x%05lX\n", (unsigned long)boundary);
    printf("  straddle start   = 0x%05lX  (crosses boundary at +256)\n",
           (unsigned long)straddle);
    printf("  reference start  = 0x%05lX  (non-straddling)\n",
           (unsigned long)ref);

    /* Straddling read */
    err = read_sector(BOUNDARY_TEST_LBA, straddle, &polls);
    if (err != 0) {
        printf("  FAIL: straddling read PHASE ERROR %d (polls=%u)\n",
               err, polls);
        reset_board();
        _ffree(buf);
        return 0;
    }
    crc_straddle = crc8_phys(straddle, SECTOR_SIZE);

    /* Non-straddling reference read */
    err = read_sector(BOUNDARY_TEST_LBA, ref, &polls);
    if (err != 0) {
        printf("  FAIL: reference read PHASE ERROR %d (polls=%u)\n",
               err, polls);
        reset_board();
        _ffree(buf);
        return 0;
    }
    crc_ref = crc8_phys(ref, SECTOR_SIZE);

    _ffree(buf);

    if (crc_straddle == crc_ref) {
        printf("  PASS: straddling crc=0x%02X matches reference crc=0x%02X\n",
               crc_straddle, crc_ref);
        return 1;
    }

    printf("  FAIL: straddling crc=0x%02X != reference crc=0x%02X "
           "(possible 64K carry bug)\n", crc_straddle, crc_ref);
    return 0;
}

/*
 * TEST 3: WRITE save/restore.
 * For each scratch LBA: read+save original, write pattern, verify pattern,
 * write original back, verify restore.  Any error still attempts a restore.
 */
static void run_write_test(void) {
    int i;
    int err;
    uint16_t polls;
    uint32_t buf = EXPANSION_RAM_ADDR;
    uint8_t orig_crc, pat_crc_expected, pat_crc, rest_crc;

    printf("\nWRITE save/restore test (%d sectors, LBAs 200-204)...\n\n",
           NUM_WRITE_TEST_SECTORS);

    for (i = 0; i < NUM_WRITE_TEST_SECTORS; i++) {
        int had_error = 0;
        int pattern_ok = 0;
        int restore_ok = 0;

        write_results[i].lba = write_test_lbas[i];
        write_results[i].pattern_ok = 0;
        write_results[i].restore_ok = 0;
        write_results[i].had_error = 0;

        /* (a) Read and save the original sector content. */
        err = read_sector(write_test_lbas[i], buf, &polls);
        if (err != 0) {
            printf("  LBA %3u: ERROR - initial read failed %d (disk untouched)\n",
                   (unsigned)write_test_lbas[i], err);
            reset_board();
            write_results[i].had_error = 1;
            continue;   /* Nothing written yet, so nothing to restore. */
        }
        copy_phys_to_local(buf, save_buffer, SECTOR_SIZE);
        orig_crc = crc8(save_buffer, SECTOR_SIZE);

        /* (b) Write the test pattern. */
        fill_pattern_phys(buf, write_test_lbas[i]);
        pat_crc_expected = crc8_pattern(write_test_lbas[i]);
        err = write_sector(write_test_lbas[i], buf, &polls);
        if (err != 0) {
            printf("  LBA %3u: WRITE ERROR %d - will still attempt restore\n",
                   (unsigned)write_test_lbas[i], err);
            reset_board();
            had_error = 1;
        } else {
            /* (c) Read back and verify pattern CRC. */
            err = read_sector(write_test_lbas[i], buf, &polls);
            if (err != 0) {
                printf("  LBA %3u: pattern READBACK ERROR %d - will still "
                       "attempt restore\n",
                       (unsigned)write_test_lbas[i], err);
                reset_board();
                had_error = 1;
            } else {
                pat_crc = crc8_phys(buf, SECTOR_SIZE);
                pattern_ok = (pat_crc == pat_crc_expected) ? 1 : 0;
                if (!pattern_ok) {
                    printf("  LBA %3u: pattern mismatch wrote=0x%02X read=0x%02X\n",
                           (unsigned)write_test_lbas[i],
                           pat_crc_expected, pat_crc);
                }
            }
        }

        /* (d) Write the ORIGINAL content back (restore). */
        copy_local_to_phys(buf, save_buffer, SECTOR_SIZE);
        err = write_sector(write_test_lbas[i], buf, &polls);
        if (err != 0) {
            printf("  LBA %3u: RESTORE WRITE ERROR %d\n",
                   (unsigned)write_test_lbas[i], err);
            reset_board();
            had_error = 1;
        } else {
            /* (e) Read back and verify restore matches saved original. */
            err = read_sector(write_test_lbas[i], buf, &polls);
            if (err != 0) {
                printf("  LBA %3u: RESTORE READBACK ERROR %d\n",
                       (unsigned)write_test_lbas[i], err);
                reset_board();
                had_error = 1;
            } else {
                rest_crc = crc8_phys(buf, SECTOR_SIZE);
                restore_ok = (rest_crc == orig_crc) ? 1 : 0;
            }
        }

        write_results[i].pattern_ok = (uint8_t)pattern_ok;
        write_results[i].restore_ok = (uint8_t)restore_ok;
        write_results[i].had_error = (uint8_t)had_error;

        if (!restore_ok) {
            printf("  *** LBA %3u: RESTORE NOT VERIFIED - SECTOR MAY BE "
                   "CORRUPT ***\n", (unsigned)write_test_lbas[i]);
        }

        if (pattern_ok && restore_ok) {
            printf("  LBA %3u: PASS (pattern verified & original restored)\n",
                   (unsigned)write_test_lbas[i]);
        } else if (restore_ok) {
            printf("  LBA %3u: FAIL (pattern not verified, original restored)\n",
                   (unsigned)write_test_lbas[i]);
        }
    }
}

/*
 * Pre-flight check - verify DMA board is responding. (Verbatim logic.)
 */
static int preflight_check(void) {
    uint8_t test_val;

    printf("Pre-flight check...\n");

    dma_write(REG_ADDR_H, 0x07);
    test_val = dma_read(REG_ADDR_H);

    if ((test_val & 0x0F) == 0x07) {
        printf("  DMA board responding: OK\n");
        reset_board();
        printf("  Board reset: OK\n");
        return 1;
    }

    printf("  WARNING: DMA board may not be responding.\n");
    printf("  Wrote 0x07 to ADDR_H, read back 0x%02X\n", test_val);
    return 0;
}

/*
 * Print the final "DMAVFY2 SUMMARY:" block.
 */
static void print_summary(int boundary_result) {
    int i;
    int read_pass = 0, read_fail = 0, read_error = 0;
    int write_pass = 0, write_fail = 0, write_error = 0;
    int restore_unverified = 0;

    for (i = 0; i < NUM_TEST_SECTORS; i++) {
        if (read_results[i].phase_error) {
            read_error++;
        } else if (read_results[i].consistent) {
            read_pass++;
        } else {
            read_fail++;
        }
    }

    for (i = 0; i < NUM_WRITE_TEST_SECTORS; i++) {
        if (write_results[i].pattern_ok && write_results[i].restore_ok) {
            write_pass++;
        } else if (write_results[i].had_error) {
            write_error++;
        } else {
            write_fail++;
        }
        if (!write_results[i].restore_ok) {
            restore_unverified++;
        }
    }

    printf("\nDMAVFY2 SUMMARY:\n");
    printf("  READ self-consistency : %d PASS, %d FAIL, %d ERROR (of %d)\n",
           read_pass, read_fail, read_error, NUM_TEST_SECTORS);
    printf("  64K boundary straddle : %s\n",
           boundary_result > 0 ? "PASS" :
           boundary_result == 0 ? "FAIL" : "SKIP");
    printf("  WRITE save/restore    : %d PASS, %d FAIL, %d ERROR (of %d)\n",
           write_pass, write_fail, write_error, NUM_WRITE_TEST_SECTORS);
    if (restore_unverified > 0) {
        printf("  *** WARNING: %d scratch sector(s) may NOT have been "
               "restored! ***\n", restore_unverified);
    }
}

int main(void) {
    int boundary_result;

    printf("DMA Transfer Verification Test v2.0 (bench-safe)\n");
    printf("================================================\n\n");

    if (!preflight_check()) {
        printf("\nContinuing anyway...\n");
    }
    printf("\n");

    /* Order: read, boundary, write - so a write-test wedge preserves the
     * other results. */
    run_read_test();
    boundary_result = run_boundary_test();
    run_write_test();

    print_summary(boundary_result);

    printf("\nPress any key to exit...\n");
    getch();

    return 0;
}
