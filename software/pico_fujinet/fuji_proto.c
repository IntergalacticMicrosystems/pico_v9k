/* fuji_proto.c — FujiNet RS232 (FujiBus) wire protocol core. See fuji_proto.h.
 *
 * A faithful C port of the reference serialize/parse in fujinet-firmware
 * lib/bus/rs232/FujiBusPacket.cpp. The on-wire header is:
 *   [0] device  [1] command  [2..3] length(u16 LE, total decoded length incl.
 *   header)  [4] checksum  [5] first descriptor
 * followed by parameter bytes (little-endian, laid out per descriptor) and then
 * the raw payload. A single u32 parameter is descriptor 0x07 (numFields=1,
 * fieldSize=4). */
#include "fuji_proto.h"
#include <string.h>

uint8_t fuji_checksum(const uint8_t *buf, size_t len)
{
    uint16_t chk = 0;
    for (size_t i = 0; i < len; i++) {
        chk += buf[i];
        chk = (uint16_t)((chk >> 8) + (chk & 0xFF));   /* fold carry */
    }
    return (uint8_t)chk;
}

size_t fuji_slip_encode(const uint8_t *in, size_t len, uint8_t *out, size_t cap)
{
    size_t o = 0;
    if (cap < 2) return 0;
    out[o++] = FUJI_SLIP_END;
    for (size_t i = 0; i < len; i++) {
        uint8_t v = in[i];
        if (v == FUJI_SLIP_END || v == FUJI_SLIP_ESC) {
            if (o + 2 > cap) return 0;
            out[o++] = FUJI_SLIP_ESC;
            out[o++] = (v == FUJI_SLIP_END) ? FUJI_SLIP_ESC_END : FUJI_SLIP_ESC_ESC;
        } else {
            if (o + 1 > cap) return 0;
            out[o++] = v;
        }
    }
    if (o + 1 > cap) return 0;
    out[o++] = FUJI_SLIP_END;
    return o;
}

size_t fuji_slip_decode(const uint8_t *in, size_t len, uint8_t *out, size_t cap)
{
    size_t i = 0;
    while (i < len && in[i] != FUJI_SLIP_END) i++;   /* skip to frame start */
    if (i == len) return 0;
    size_t o = 0;
    for (++i; i < len; i++) {
        uint8_t v = in[i];
        if (v == FUJI_SLIP_END) return o;            /* closing END */
        if (v == FUJI_SLIP_ESC) {
            if (++i >= len) return 0;                /* truncated escape */
            v = in[i];
            if (v == FUJI_SLIP_ESC_END)      { if (o >= cap) return 0; out[o++] = FUJI_SLIP_END; }
            else if (v == FUJI_SLIP_ESC_ESC) { if (o >= cap) return 0; out[o++] = FUJI_SLIP_ESC; }
            /* else: malformed escape, ignored (matches reference) */
        } else {
            if (o >= cap) return 0;
            out[o++] = v;
        }
    }
    return 0;                                         /* no closing END */
}

/* Append `payload`, stamp the u16 length + end-around-carry checksum (the byte
 * at [4] must already be 0), and SLIP-encode `dec[0..n)` into `out`. */
static size_t fuji_finish(uint8_t *dec, size_t deccap, size_t n,
                          const uint8_t *payload, size_t payload_len,
                          uint8_t *out, size_t cap)
{
    if (payload && payload_len) {
        if (n + payload_len > deccap) return 0;
        memcpy(dec + n, payload, payload_len);
        n += payload_len;
    }
    dec[2] = (uint8_t)(n & 0xFF);
    dec[3] = (uint8_t)((n >> 8) & 0xFF);
    dec[4] = fuji_checksum(dec, n);                   /* placeholder was 0 */
    return fuji_slip_encode(dec, n, out, cap);
}

size_t fuji_build_frame(uint8_t device, uint8_t command,
                        bool has_param, uint32_t param,
                        const uint8_t *payload, size_t payload_len,
                        uint8_t *out, size_t cap)
{
    /* Static (not on the stack): FUJI_DEC_MAX is ~4 KB with READ_MULTI. The
     * proto layer is single-context — transact() in fuji_blkdev.c already uses
     * static frame/rx/scratch buffers, so this non-reentrancy is pre-existing. */
    static uint8_t dec[FUJI_DEC_MAX];
    size_t n;

    dec[0] = device;
    dec[1] = command;
    /* dec[2..3] length filled below */
    dec[4] = 0;                                       /* checksum placeholder */
    dec[5] = has_param ? 0x07u : 0x00u;               /* one u32 field, or none */
    n = FUJI_HEADER_LEN;

    if (has_param) {
        dec[n++] = (uint8_t)(param & 0xFF);
        dec[n++] = (uint8_t)((param >> 8) & 0xFF);
        dec[n++] = (uint8_t)((param >> 16) & 0xFF);
        dec[n++] = (uint8_t)((param >> 24) & 0xFF);
    }

    return fuji_finish(dec, sizeof(dec), n, payload, payload_len, out, cap);
}

size_t fuji_build_frame_u8(uint8_t device, uint8_t command,
                           const uint8_t *params, unsigned nparams,
                           const uint8_t *payload, size_t payload_len,
                           uint8_t *out, size_t cap)
{
    /* Own static scratch (see fuji_build_frame's note); only ever carries a
     * <=256-byte control payload, so it stays small. */
    static uint8_t dec[FUJI_HEADER_LEN + 4u + FUJI_PATH_LEN];
    size_t n;

    if (nparams < 1 || nparams > 4) return 0;
    dec[0] = device;
    dec[1] = command;
    /* dec[2..3] length filled below */
    dec[4] = 0;                                       /* checksum placeholder */
    dec[5] = (uint8_t)nparams;                        /* nparams u8 fields */
    n = FUJI_HEADER_LEN;
    for (unsigned i = 0; i < nparams; i++) dec[n++] = params[i];

    return fuji_finish(dec, sizeof(dec), n, payload, payload_len, out, cap);
}

/* Descriptor field tables (FujiBusPacket.cpp). Index = descriptor & 0x07. */
static const uint8_t k_field_size[8]  = {0, 1, 1, 1, 1, 2, 2, 4};
static const uint8_t k_field_count[8] = {0, 1, 2, 3, 4, 1, 2, 1};

bool fuji_parse_frame(const uint8_t *in, size_t len,
                      uint8_t *scratch, size_t scap, fuji_frame_t *out)
{
    size_t dl = fuji_slip_decode(in, len, scratch, scap);
    if (dl < FUJI_HEADER_LEN) return false;

    uint16_t plen = (uint16_t)(scratch[2] | (scratch[3] << 8));
    if (plen != dl) return false;                     /* length must be exact */

    uint8_t ck1 = scratch[4];
    scratch[4] = 0;
    if (fuji_checksum(scratch, dl) != ck1) return false;

    out->device  = scratch[0];
    out->command = scratch[1];
    out->nparam  = 0;

    /* Descriptors: the first is in the header; more follow while bit7 is set. */
    size_t off = FUJI_HEADER_LEN;
    uint8_t descrs[8];
    int nd = 0;
    uint8_t dsc = scratch[5];
    descrs[nd++] = dsc;
    while (dsc & 0x80u) {
        if (off >= dl) return false;
        dsc = scratch[off++];
        if (nd < (int)sizeof(descrs)) descrs[nd++] = dsc;
        else return false;
    }

    for (int d = 0; d < nd; d++) {
        unsigned fd = descrs[d] & 0x07u;
        unsigned fc = k_field_count[fd];
        unsigned fs = k_field_size[fd];
        for (unsigned k = 0; k < fc; k++) {
            if (off + fs > dl) return false;
            uint32_t v = 0;
            for (unsigned b = 0; b < fs; b++)
                v |= (uint32_t)scratch[off + b] << (8 * b);
            if (out->nparam < 4) out->param[out->nparam++] = v;
            off += fs;
        }
    }

    if (off < dl) { out->payload = scratch + off; out->payload_len = dl - off; }
    else          { out->payload = NULL;          out->payload_len = 0; }
    return true;
}
