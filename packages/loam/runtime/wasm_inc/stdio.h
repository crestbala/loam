#ifndef LOAM_WASM_STDIO_H
#define LOAM_WASM_STDIO_H
/* Empty: wasm has no stdio. Text goes out through `write` (unistd.h), which
   is the `zeus.write` import. */
#endif
