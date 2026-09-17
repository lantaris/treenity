/**
 * @file frame.c
 * @brief Frame and control-payload (de)serialisation.
 */
#include "frame.h"

#include <string.h>

#include "core/util.h"

/**
 * Nibble lookup table for CRC-16/CCITT-FALSE (polynomial 0x1021). Processing
 * four bits at a time keeps the code small and fast without a 256 entry table.
 */
static const uint16_t crc16_nibble[16] = {
    0x0000u, 0x1021u, 0x2042u, 0x3063u, 0x4084u, 0x50A5u, 0x60C6u, 0x70E7u,
    0x8108u, 0x9129u, 0xA14Au, 0xB16Bu, 0xC18Cu, 0xD1ADu, 0xE1CEu, 0xF1EFu
};

uint16_t tn_frame_crc(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];
        crc = (uint16_t)((crc << 4) ^
                         crc16_nibble[((crc >> 12) ^ (uint32_t)(b >> 4)) & 0x0Fu]);
        crc = (uint16_t)((crc << 4) ^
                         crc16_nibble[((crc >> 12) ^ (uint32_t)(b & 0x0Fu)) & 0x0Fu]);
    }
    return crc;
}

size_t tn_frame_encode(uint8_t *buf, size_t cap, const tn_frame_t *frame)
{
    size_t body = TN_FRAME_HDR + frame->payload_len;
    size_t total = body + TN_FRAME_TRAILER;
    if (total > cap || total > TREENET_MTU) {
        return 0;
    }

    buf[0] = (uint8_t)(((frame->version & 0x0Fu) << 4) | (frame->type & 0x0Fu));
    buf[1] = frame->flags;
    tn_put_u32(&buf[2], frame->src);
    tn_put_u32(&buf[6], frame->dst);
    tn_put_u16(&buf[10], frame->seq);
    tn_put_u16(&buf[12], frame->net_id);
    buf[14] = frame->hop_limit;
    buf[15] = frame->sec;
    tn_put_u32(&buf[TN_FRAME_OFF_PREV], frame->prev);

    if (frame->payload_len > 0 && frame->payload != NULL) {
        memcpy(&buf[TN_FRAME_HDR], frame->payload, frame->payload_len);
    }

#if TREENET_ENABLE_FRAME_CRC
    /* The CRC covers the header (including the freshly written previous-hop
     * field) and the payload, and is appended as a trailer. */
    tn_put_u16(&buf[body], tn_frame_crc(buf, body));
#endif
    return total;
}

bool tn_frame_decode(const uint8_t *buf, size_t len, tn_frame_t *out)
{
    if (buf == NULL || out == NULL || len < TN_FRAME_OVERHEAD) {
        return false;
    }

#if TREENET_ENABLE_FRAME_CRC
    /* Verify integrity before touching any field. */
    size_t body = len - TN_FRAME_TRAILER;
    if (tn_frame_crc(buf, body) != tn_get_u16(&buf[body])) {
        return false;
    }
#endif

    out->version = (uint8_t)((buf[0] >> 4) & 0x0Fu);
    out->type = (uint8_t)(buf[0] & 0x0Fu);
    if (out->version != TREENET_PROTOCOL_VERSION) {
        return false;
    }

    out->flags = buf[1];
    out->src = tn_get_u32(&buf[2]);
    out->dst = tn_get_u32(&buf[6]);
    out->seq = tn_get_u16(&buf[10]);
    out->net_id = tn_get_u16(&buf[12]);
    out->hop_limit = buf[14];
    out->sec = buf[15];
    out->prev = tn_get_u32(&buf[TN_FRAME_OFF_PREV]);

    /* Address sanity: a frame can never originate from the null or broadcast
     * address, and can never be addressed to the null address. */
    if (out->src == TREENET_ADDR_INVALID ||
        out->src == TREENET_ADDR_BROADCAST ||
        out->dst == TREENET_ADDR_INVALID) {
        return false;
    }

    out->payload = buf + TN_FRAME_HDR;
    out->payload_len = len - TN_FRAME_OVERHEAD;
    return true;
}


void tn_beacon_encode(uint8_t *out, const tn_beacon_payload_t *b)
{
    tn_put_u16(&out[0], b->rank);
    tn_put_u32(&out[2], b->parent);
    out[6] = b->flags;
    tn_put_u16(&out[7], b->interval_100ms);
}

bool tn_beacon_decode(const uint8_t *in, size_t len, tn_beacon_payload_t *b)
{
    if (in == NULL || b == NULL || len < TN_BEACON_PAYLOAD_LEN) {
        return false;
    }
    b->rank = tn_get_u16(&in[0]);
    b->parent = tn_get_u32(&in[2]);
    b->flags = in[6];
    b->interval_100ms = tn_get_u16(&in[7]);
    return true;
}

void tn_frag_encode(uint8_t *out, const tn_frag_hdr_t *f)
{
    tn_put_u16(&out[0], f->dgram_id);
    out[2] = f->index;
    out[3] = f->count;
}

bool tn_frag_decode(const uint8_t *in, size_t len, tn_frag_hdr_t *f)
{
    if (in == NULL || f == NULL || len < TN_FRAG_HDR_LEN) {
        return false;
    }
    f->dgram_id = tn_get_u16(&in[0]);
    f->index = in[2];
    f->count = in[3];
    return true;
}

void tn_dao_encode(uint8_t *out, const tn_dao_payload_t *d)
{
    tn_put_u32(&out[0], d->origin);
    out[4] = d->hops;
}

bool tn_dao_decode(const uint8_t *in, size_t len, tn_dao_payload_t *d)
{
    if (in == NULL || d == NULL || len < TN_DAO_PAYLOAD_LEN) {
        return false;
    }
    d->origin = tn_get_u32(&in[0]);
    d->hops = in[4];
    return true;
}
