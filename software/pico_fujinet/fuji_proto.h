/* fuji_proto.h — FujiNet RS232 (FujiBus) wire protocol, host-portable.
 *
 * The framing/checksum/build/parse used to talk to an ESP32-S3 running the
 * FujiNet BUILD_RS232 firmware. Derived byte-for-byte from the reference
 * (fujinet-firmware lib/bus/rs232/FujiBusPacket.cpp + rs232.cpp): SLIP framing,
 * a 6-byte little-endian header, and a parameter-descriptor scheme.
 *
 * This layer is pure C with no Pico SDK dependency, so it links into the host
 * unit tests. The UART transport + block backend live in
 * firmware/fuji_blkdev.c. */
#ifndef V9K_FUJI_PROTO_H
#define V9K_FUJI_PROTO_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SLIP special bytes. */
#define FUJI_SLIP_END      0xC0u
#define FUJI_SLIP_ESC      0xDBu
#define FUJI_SLIP_ESC_END  0xDCu
#define FUJI_SLIP_ESC_ESC  0xDDu

/* Device / command IDs used by the disk backend (fujiDeviceID.h / .h). */
#define FUJI_DEVICEID_DISK     0x31u   /* D1 */
#define FUJI_DEVICEID_FUJINET  0x70u   /* Fuji control device */
#define FUJICMD_ACK            0x06u
#define FUJICMD_NAK            0x15u
#define FUJICMD_MOUNT_ALL      0xD7u
#define FUJICMD_GET_SSID          0xFEu   /* no param -> SSIDConfig (97B) */
#define FUJICMD_SCAN_NETWORKS     0xFDu   /* no param -> u8 count (live scan) */
#define FUJICMD_GET_SCAN_RESULT   0xFCu   /* 1 param: index -> SSIDInfo (34B) */
#define FUJICMD_SET_SSID          0xFBu   /* 1 param: save + SSIDConfig (97B) */
#define FUJICMD_GET_WIFISTATUS    0xFAu   /* no param -> u8: 3=up 6=down */
#define FUJICMD_READ_HOST_SLOTS   0xF4u   /* no param -> 8x32 host names (256B) */
#define FUJICMD_READ_DEVICE_SLOTS 0xF2u   /* no param -> 8x(hostSlot,mode,name[36]) (304B) */
#define FUJICMD_WRITE_HOST_SLOTS  0xF3u   /* no param + 8x32 host names (256B) */
#define FUJICMD_SET_DEVICE_FULLPATH 0xE2u  /* deviceSlot,hostSlot,mode + 256B name */
#define FUJICMD_UNMOUNT_IMAGE  0xE9u   /* 1 param: deviceSlot */
#define FUJICMD_MOUNT_HOST     0xF9u   /* 1 param: hostSlot (idempotent on ESP) */
#define FUJICMD_OPEN_DIRECTORY 0xF7u   /* 1 param: hostSlot + 256B path[/pattern] */
#define FUJICMD_READ_DIR_ENTRY 0xF6u   /* 2 params: maxlen,addtl */
#define FUJICMD_CLOSE_DIRECTORY 0xF5u  /* no params */
#define DISKCMD_READ           0x52u   /* 'R' */
#define DISKCMD_READ_MULTI     0x72u   /* 'r': one request, N sectors back-to-back */
#define DISKCMD_PUT            0x50u   /* 'P' */

/* Exact payload size SET_DEVICE_FULLPATH / OPEN_DIRECTORY require: the ESP does
 * transaction_get(tmp, 256) and errors if fewer bytes are available. */
#define FUJI_PATH_LEN          256u

#define FUJI_SECTOR_SIZE       512u
#define FUJI_MULTI_SECTORS     16u     /* max sectors per READ_MULTI transaction */
#define FUJI_HEADER_LEN        6u      /* device,command,length(2),checksum,descr */

/* Largest decoded packet (header + one u32 param + a 16-sector READ_MULTI
 * payload) and its SLIP worst case (every byte escaped, plus two END markers). */
#define FUJI_DEC_MAX  (FUJI_HEADER_LEN + 4u + FUJI_MULTI_SECTORS * FUJI_SECTOR_SIZE) /* 8202 */
#define FUJI_ENC_MAX  (FUJI_DEC_MAX * 2u + 2u)                    /* 16406 */

/* 8-bit end-around-carry checksum over the decoded packet (checksum byte
 * zeroed). Matches rs232_checksum / FujiBusPacket::calcChecksum. */
uint8_t fuji_checksum(const uint8_t *buf, size_t len);

/* SLIP-encode `len` bytes of `in` into `out` (cap bytes), bracketed by a
 * leading and trailing END. Returns encoded length, or 0 if it won't fit. */
size_t fuji_slip_encode(const uint8_t *in, size_t len, uint8_t *out, size_t cap);

/* SLIP-decode one frame from `in`: skip to the first END, decode to the next
 * END. Returns decoded length, or 0 on no-frame / truncation / overflow. */
size_t fuji_slip_decode(const uint8_t *in, size_t len, uint8_t *out, size_t cap);

/* Build a FujiBus request frame (SLIP-encoded, ready to write) into `out`
 * (cap bytes). `has_param` adds a single u32 little-endian parameter; a
 * non-NULL `payload` of `payload_len` bytes follows the parameters. Returns the
 * encoded length, or 0 on overflow. */
size_t fuji_build_frame(uint8_t device, uint8_t command,
                        bool has_param, uint32_t param,
                        const uint8_t *payload, size_t payload_len,
                        uint8_t *out, size_t cap);

/* Like fuji_build_frame but with `nparams` (1..4) single-byte parameters — the
 * descriptor byte is `nparams` (fieldCount=nparams, fieldSize=1). The FujiNet
 * control commands (UNMOUNT/SET_DEVICE_FULLPATH/OPEN_DIRECTORY/READ_DIR_ENTRY)
 * take their params numerically, so u8 fields serialize fine. Same static-buffer
 * non-reentrancy as fuji_build_frame. Returns the encoded length, or 0. */
size_t fuji_build_frame_u8(uint8_t device, uint8_t command,
                           const uint8_t *params, unsigned nparams,
                           const uint8_t *payload, size_t payload_len,
                           uint8_t *out, size_t cap);

/* A parsed frame. `payload` points into the caller-supplied scratch buffer. */
typedef struct {
    uint8_t        device;
    uint8_t        command;
    uint32_t       param[4];
    unsigned       nparam;
    const uint8_t *payload;      /* NULL if no payload */
    size_t         payload_len;
} fuji_frame_t;

/* Parse a SLIP-encoded frame in `in` (len bytes). `scratch` (scap bytes) holds
 * the decoded bytes the result's payload pointer indexes into. Returns true on
 * a well-formed, length-consistent, checksum-valid frame. */
bool fuji_parse_frame(const uint8_t *in, size_t len,
                      uint8_t *scratch, size_t scap, fuji_frame_t *out);

#ifdef __cplusplus
}
#endif

#endif /* V9K_FUJI_PROTO_H */
