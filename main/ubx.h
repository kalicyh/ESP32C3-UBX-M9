#pragma once
#include <stddef.h>
#include <stdint.h>
#define UBX_MAX_PAYLOAD 8192
#define UBX_MAX_FRAME (UBX_MAX_PAYLOAD + 8)
typedef void (*ubx_callback)(const uint8_t *, size_t, void *);
typedef struct {
    uint8_t buf[UBX_MAX_FRAME];
    size_t used;
    uint32_t bad_checksum, oversized;
    ubx_callback callback;
    void *context;
} ubx_parser;
void ubx_feed(ubx_parser *p, const uint8_t *data, size_t len);
size_t ubx_encode(uint8_t *out, size_t capacity, uint8_t cls, uint8_t id,
                  const uint8_t *payload, size_t len);
uint16_t ubx_u16(const uint8_t *p);
uint32_t ubx_u32(const uint8_t *p);
int32_t ubx_i32(const uint8_t *p);
