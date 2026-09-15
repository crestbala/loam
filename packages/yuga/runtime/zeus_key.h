#ifndef ZEUS_KEY_H
#define ZEUS_KEY_H

#include <stdint.h>

#define ZEUS_MOD_SHIFT 1
#define ZEUS_MOD_CTRL 2
#define ZEUS_MOD_ALT 4
#define ZEUS_MOD_CMD 8

/* Printable keys are their own ASCII code; everything else lives above it.
   The arrow and page codes keep the values already shipped. */
#define ZEUS_K_BACK 8
#define ZEUS_K_TAB 9
#define ZEUS_K_ENTER 13
#define ZEUS_K_ESC 27
#define ZEUS_K_DEL 127
#define ZEUS_K_LEFT 1000
#define ZEUS_K_RIGHT 1001
#define ZEUS_K_UP 1002
#define ZEUS_K_DOWN 1003
#define ZEUS_K_PGUP 1004
#define ZEUS_K_PGDN 1005
#define ZEUS_K_HOME 1006
#define ZEUS_K_END 1007
#define ZEUS_K_F1 1010

/* Deepest focus chain a dispatch will walk. */
#define ZEUS_FOCUS_MAX 64

/* keymap */
int zeus_key_parse(const char *spec, int *k1, int *m1, int *k2, int *m2);
void zeus_key_map(const char *spec, const char *action, const char *ctx);
void zeus_key_map_ctx(const char *spec, const char *action, int ctx);
void zeus_key_remap(const char *spec, const char *action, const char *ctx);
int zeus_key_action_id(const char *name);
int zeus_key_context_id(const char *name);
int zeus_key_context_from_action(const char *action);

/* handlers + dispatch */
void zeus_key_reset_handlers(void);
void zeus_key_on_action(int nid, const char *action, int sig, int mode, int64_t value);
void zeus_key_on_range(int nid, const char *action, int sig, int64_t delta, int64_t lo,
                      int64_t hi);
int zeus_key_dispatch(int key, int mods);

/* provided by zeus_plat.c (Yuga engine trampolines) */
int zeus_focus_chain(int *nodes, int *ctxs, int max);
void zeus_key_apply(int sig, int mode, int64_t value, int64_t lo, int64_t hi);
int zeus_focus_captures_text(void);
int zeus_focus_step(int back);

#endif
