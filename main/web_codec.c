#include "web_codec.h"
#include <stdio.h>
#include <string.h>
void jw_printf(json_writer *w, const char *fmt, ...) {
    if (w->failed || w->used >= w->capacity) { w->failed = true; return; }
    va_list args; va_start(args, fmt);
    int n = vsnprintf(w->data + w->used, w->capacity - w->used, fmt, args);
    va_end(args);
    if (n < 0 || (size_t)n >= w->capacity - w->used) { w->failed = true; return; }
    w->used += n;
}
void jw_string(json_writer *w, const char *s, size_t max_len) {
    jw_printf(w, "\"");
    for (size_t i = 0; i < max_len && s[i]; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') jw_printf(w, "\\%c", c);
        else if (c < 32) jw_printf(w, "\\u%04x", c);
        else jw_printf(w, "%c", c);
    }
    jw_printf(w, "\"");
}
