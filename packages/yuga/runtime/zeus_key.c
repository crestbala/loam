/* Keyboard: chords, contexts, named actions, and dispatch.
 *
 * Split from the old C tree because keymap and node arena share nothing: a
 * keymap is a lookup table over interned names, not a tree.
 *
 * The chain is deliberately two-stage. A chord resolves to a named *action*
 * through the keymap, and the action reaches state through a handler that
 * writes a signal. Nothing binds a chord straight to an effect, which is what
 * lets a user remap Cmd+S without the component that saves knowing it moved.
 */

#include "zeus_key.h"

#include <stdlib.h>
#include <string.h>

/* --- interned names ------------------------------------------------------ */

static char **names;
static int nnames, namecap;

/* Name 0 is the empty name, so a zero ctx field reads as "any context"
   without a sentinel. */
static int intern(const char *s) {
    if (!s || !*s) return 0;
    for (int i = 1; i < nnames; i++)
        if (strcmp(names[i], s) == 0) return i;
    if (nnames >= namecap) {
        int nc = namecap ? namecap * 2 : 16;
        char **p = (char **)realloc(names, (size_t)nc * sizeof(char *));
        if (!p) abort();
        names = p;
        namecap = nc;
        if (nnames == 0) {
            names[0] = (char *)"";
            nnames = 1;
        }
    }
    size_t n = strlen(s);
    char *c = (char *)malloc(n + 1);
    if (!c) abort();
    memcpy(c, s, n + 1);
    names[nnames] = c;
    return nnames++;
}

/* --- chord parsing ------------------------------------------------------- */

static const struct {
    const char *name;
    int code;
} SPECIALS[] = {
    {"escape", ZEUS_K_ESC},    {"esc", ZEUS_K_ESC},       {"enter", ZEUS_K_ENTER},
    {"return", ZEUS_K_ENTER},  {"tab", ZEUS_K_TAB},       {"backspace", ZEUS_K_BACK},
    {"delete", ZEUS_K_DEL},    {"space", ' '},           {"left", ZEUS_K_LEFT},
    {"right", ZEUS_K_RIGHT},   {"up", ZEUS_K_UP},         {"down", ZEUS_K_DOWN},
    {"pageup", ZEUS_K_PGUP},   {"pagedown", ZEUS_K_PGDN}, {"home", ZEUS_K_HOME},
    {"end", ZEUS_K_END},       {"f1", ZEUS_K_F1},         {"f2", ZEUS_K_F1 + 1},
    {"f3", ZEUS_K_F1 + 2},     {"f4", ZEUS_K_F1 + 3},     {"f5", ZEUS_K_F1 + 4},
    {"f6", ZEUS_K_F1 + 5},     {"f7", ZEUS_K_F1 + 6},     {"f8", ZEUS_K_F1 + 7},
    {"f9", ZEUS_K_F1 + 8},     {"f10", ZEUS_K_F1 + 9},    {"f11", ZEUS_K_F1 + 10},
    {"f12", ZEUS_K_F1 + 11},   {NULL, 0},
};

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* One token: modifier words joined by '-' or '+', then a key name. Both
   separators are accepted because "cmd+s" is what people write. A letter is
   stored lowercased with shift as a separate bit, so "cmd-S" and "cmd-shift-s"
   cannot resolve to two different bindings. */
static int parse_one(const char *s, int len, int *key, int *mods) {
    char buf[32];
    int m = 0, start = 0;
    *key = 0;
    *mods = 0;
    for (int i = 0; i <= len; i++) {
        if (i != len && s[i] != '-' && s[i] != '+') continue;
        int n = i - start;
        if (n <= 0 || n >= (int)sizeof buf) return 0;
        for (int j = 0; j < n; j++) buf[j] = (char)lower(s[start + j]);
        buf[n] = '\0';
        start = i + 1;
        if (i != len) {
            if (!strcmp(buf, "cmd") || !strcmp(buf, "super")) m |= ZEUS_MOD_CMD;
            else if (!strcmp(buf, "ctrl") || !strcmp(buf, "control")) m |= ZEUS_MOD_CTRL;
            else if (!strcmp(buf, "alt") || !strcmp(buf, "opt")) m |= ZEUS_MOD_ALT;
            else if (!strcmp(buf, "shift")) m |= ZEUS_MOD_SHIFT;
            else return 0;
            continue;
        }
        for (int k = 0; SPECIALS[k].name; k++)
            if (!strcmp(buf, SPECIALS[k].name)) {
                *key = SPECIALS[k].code;
                *mods = m;
                return 1;
            }
        if (n == 1 && buf[0] >= 32 && buf[0] < 127) {
            *key = buf[0];
            *mods = m;
            return 1;
        }
        return 0;
    }
    return 0;
}

int zeus_key_parse(const char *spec, int *k1, int *m1, int *k2, int *m2) {
    *k1 = *m1 = *k2 = *m2 = 0;
    if (!spec) return 0;
    const char *sp = strchr(spec, ' ');
    if (!sp) return parse_one(spec, (int)strlen(spec), k1, m1);
    if (!parse_one(spec, (int)(sp - spec), k1, m1)) return 0;
    while (*sp == ' ') sp++;
    return parse_one(sp, (int)strlen(sp), k2, m2);
}

/* --- keymap -------------------------------------------------------------- */

typedef struct {
    int key, mods;
    int key2, mods2; /* second chord of a sequence; key2 == 0 for a single */
    int ctx;         /* 0 = any */
    int action;
    int user; /* a user entry outranks a default at the same context */
} ZeusBind;

static ZeusBind *binds;
static int nbinds, bindcap;

static void add_bind_id(const char *spec, const char *action, int c, int user) {
    int k1, m1, k2, m2;
    if (!zeus_key_parse(spec, &k1, &m1, &k2, &m2)) return;
    int a = intern(action);
    for (int i = 0; i < nbinds; i++) {
        ZeusBind *b = &binds[i];
        if (b->key == k1 && b->mods == m1 && b->key2 == k2 && b->mods2 == m2 &&
            b->ctx == c && b->user == user) {
            b->action = a;
            return;
        }
    }
    if (nbinds >= bindcap) {
        int nc = bindcap ? bindcap * 2 : 32;
        ZeusBind *p = (ZeusBind *)realloc(binds, (size_t)nc * sizeof(ZeusBind));
        if (!p) abort();
        binds = p;
        bindcap = nc;
    }
    binds[nbinds++] = (ZeusBind){k1, m1, k2, m2, c, a, user};
}

static void add_bind(const char *spec, const char *action, const char *ctx, int user) {
    add_bind_id(spec, action, intern(ctx), user);
}

void zeus_key_map(const char *spec, const char *action, const char *ctx) {
    add_bind(spec, action, ctx, 0);
}

void zeus_key_map_ctx(const char *spec, const char *action, int ctx) {
    add_bind_id(spec, action, ctx, 0);
}

void zeus_key_remap(const char *spec, const char *action, const char *ctx) {
    add_bind(spec, action, ctx, 1);
}

/* --- handlers ------------------------------------------------------------ */

typedef struct {
    int action;
    int nid; /* 0 = global */
    int sig, mode;
    int64_t value;
    int64_t lo, hi; /* mode 4 only: the range the step is clamped to */
} ZeusHandler;

static ZeusHandler *handlers;
static int nhandlers, handlercap;

void zeus_key_on_range(int nid, const char *action, int sig, int64_t delta, int64_t lo,
                      int64_t hi) {
    zeus_key_on_action(nid, action, sig, 4, delta);
    handlers[nhandlers - 1].lo = lo;
    handlers[nhandlers - 1].hi = hi;
}

void zeus_key_on_action(int nid, const char *action, int sig, int mode, int64_t value) {
    if (nhandlers >= handlercap) {
        int nc = handlercap ? handlercap * 2 : 32;
        ZeusHandler *p = (ZeusHandler *)realloc(handlers, (size_t)nc * sizeof(ZeusHandler));
        if (!p) abort();
        handlers = p;
        handlercap = nc;
    }
    handlers[nhandlers++] = (ZeusHandler){intern(action), nid, sig, mode, value, 0, 0};
}

void zeus_key_reset_handlers(void) { nhandlers = 0; }

int zeus_key_action_id(const char *name) { return intern(name); }
int zeus_key_context_id(const char *name) { return intern(name); }

int zeus_key_context_from_action(const char *action) {
    const char *dot;
    int n;
    char buf[64];
    if (!action || !*action) return 0;
    dot = strchr(action, '.');
    if (!dot) return intern(action);
    n = (int)(dot - action);
    if (n <= 0) return intern(action);
    if (n >= (int)sizeof buf) n = (int)sizeof buf - 1;
    memcpy(buf, action, (size_t)n);
    buf[n] = '\0';
    return intern(buf);
}

/* --- dispatch ------------------------------------------------------------ */

/* The first chord of a sequence, held until the next key decides it. */
static int pending_key, pending_mods;

/* Does this chord open a sequence in any context on the focus chain? */
static int opens_sequence(int key, int mods, const int *chain, int depth) {
    for (int i = 0; i < nbinds; i++) {
        ZeusBind *b = &binds[i];
        if (b->key2 == 0 || b->key != key || b->mods != mods) continue;
        if (b->ctx == 0) return 1;
        for (int d = 0; d < depth; d++)
            if (chain[d] == b->ctx) return 1;
    }
    return 0;
}

/* Innermost matching context wins; a user entry outranks a default within the
   same context, and a context-scoped entry outranks an unscoped one. */
static int resolve(int k1, int m1, int k2, int m2, const int *chain, int depth) {
    for (int d = 0; d < depth; d++) {
        int best = 0, best_user = -1;
        for (int i = 0; i < nbinds; i++) {
            ZeusBind *b = &binds[i];
            if (b->ctx != chain[d]) continue;
            if (b->key != k1 || b->mods != m1) continue;
            if (b->key2 != k2 || b->mods2 != m2) continue;
            if (b->user > best_user) {
                best = b->action;
                best_user = b->user;
            }
        }
        if (best) return best;
    }
    int best = 0, best_user = -1;
    for (int i = 0; i < nbinds; i++) {
        ZeusBind *b = &binds[i];
        if (b->ctx != 0) continue;
        if (b->key != k1 || b->mods != m1) continue;
        if (b->key2 != k2 || b->mods2 != m2) continue;
        if (b->user > best_user) {
            best = b->action;
            best_user = b->user;
        }
    }
    return best;
}

/* Fire the innermost handler for `action`: focused node, then each ancestor,
   then global. The first one to run consumes the event. */
static int fire(int action, const int *nodes, int depth) {
    for (int d = 0; d < depth; d++)
        for (int i = 0; i < nhandlers; i++)
            if (handlers[i].action == action && handlers[i].nid == nodes[d]) {
                zeus_key_apply(handlers[i].sig, handlers[i].mode, handlers[i].value, handlers[i].lo, handlers[i].hi);
                return 1;
            }
    for (int i = 0; i < nhandlers; i++)
        if (handlers[i].action == action && handlers[i].nid == 0) {
            zeus_key_apply(handlers[i].sig, handlers[i].mode, handlers[i].value, handlers[i].lo, handlers[i].hi);
            return 1;
        }
    return 0;
}

int zeus_key_dispatch(int key, int mods) {
    int typed = (key >= 32 && key < 127) || key == ZEUS_K_BACK || key == ZEUS_K_DEL ||
                key == ZEUS_K_TAB || key == ZEUS_K_ENTER || key == 10;
    if (typed && !(mods & ~ZEUS_MOD_SHIFT) && zeus_focus_captures_text())
        return 0;

    int nodes[ZEUS_FOCUS_MAX], ctxs[ZEUS_FOCUS_MAX];
    int depth = zeus_focus_chain(nodes, ctxs, ZEUS_FOCUS_MAX);

    if (pending_key) {
        int a = resolve(pending_key, pending_mods, key, mods, ctxs, depth);
        pending_key = pending_mods = 0;
        if (a) return fire(a, nodes, depth);
        return 1; /* the sequence died here; the key is still spent */
    }
    int a = resolve(key, mods, 0, 0, ctxs, depth);
    if (a && fire(a, nodes, depth)) return 1;
    if (opens_sequence(key, mods, ctxs, depth)) {
        pending_key = key;
        pending_mods = mods;
        return 1;
    }
    return 0;
}
