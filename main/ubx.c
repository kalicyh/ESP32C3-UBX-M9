#include "ubx.h"
#include <string.h>
uint16_t ubx_u16(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }
uint32_t ubx_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
int32_t ubx_i32(const uint8_t *p) {
    uint32_t u = ubx_u32(p);
    return u <= INT32_MAX ? (int32_t)u : -1 - (int32_t)(UINT32_MAX - u);
}
static void checksum(const uint8_t *b, size_t n, uint8_t *a, uint8_t *c) {
    *a = *c = 0;
    for (size_t i = 0; i < n; ++i) { *a += b[i]; *c += *a; }
}
size_t ubx_encode(uint8_t *o, size_t cap, uint8_t cls, uint8_t id,
                  const uint8_t *payload, size_t len) {
    if (len > UBX_MAX_PAYLOAD || cap < len + 8 || (len && !payload)) return 0;
    o[0] = 0xb5; o[1] = 0x62; o[2] = cls; o[3] = id;
    o[4] = len; o[5] = len >> 8;
    if (len) memcpy(o + 6, payload, len);
    checksum(o + 2, len + 4, o + len + 6, o + len + 7);
    return len + 8;
}
static void discard(ubx_parser *p, size_t n) {
    p->used -= n;
    memmove(p->buf, p->buf + n, p->used);
}
void ubx_feed(ubx_parser *p, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        p->buf[p->used++] = data[i];
        while (p->used) {
            if (p->buf[0] != 0xb5) { discard(p, 1); continue; }
            if (p->used < 2) break;
            if (p->buf[1] != 0x62) { discard(p, 1); continue; }
            if (p->used < 6) break;
            size_t payload = ubx_u16(p->buf + 4), total = payload + 8;
            if (payload > UBX_MAX_PAYLOAD) { ++p->oversized; discard(p, 1); continue; }
            if (p->used < total) break;
            uint8_t a, b;
            checksum(p->buf + 2, payload + 4, &a, &b);
            if (a == p->buf[total - 2] && b == p->buf[total - 1]) {
                if (p->callback) p->callback(p->buf, total, p->context);
                discard(p, total);
            } else { ++p->bad_checksum; discard(p, 1); }
        }
    }
}
