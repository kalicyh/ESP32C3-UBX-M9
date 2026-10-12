#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>
typedef struct { char *data; size_t capacity, used; bool failed; } json_writer;
void jw_printf(json_writer *w, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void jw_string(json_writer *w, const char *s, size_t max_len);
