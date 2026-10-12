#pragma once
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
void gnss_web_init(void);
/* Called on validated frames before the serial output queue. */
void gnss_web_receive(const uint8_t *frame, size_t len);
void gnss_web_stats(uint32_t checksum, uint32_t oversized, unsigned dropped,
                    unsigned io_errors, unsigned overflows);
/* Caller owns returned JSON (NULL on allocation/serialization failure). */
char *gnss_web_json(void);

char *gnss_snapshot_json(bool include_raw);
