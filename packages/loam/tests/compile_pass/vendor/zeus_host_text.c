/* A host's text input, as the Cocoa and web hosts send it: a borrowed
   `loam_str` view over a buffer the host reuses right after the call. */
#include <stdint.h>
#include <string.h>

typedef struct {
    const char *ptr;
    int64_t len;
    int64_t own;
} host_text_view;

int32_t loam_zeus_engine_insert(host_text_view text);

static char host_text_buf[8];

int32_t host_text_insert_then_reuse(void) {
    int32_t consumed;
    memcpy(host_text_buf, "abc", 4);
    consumed = loam_zeus_engine_insert((host_text_view){host_text_buf, 3, 0});
    memcpy(host_text_buf, "XYZ", 4);
    return consumed;
}
