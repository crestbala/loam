/**
 * threadcheck.c — the Send discipline for std:thread (language threads).
 *
 * Runs after typecheck, when calls are resolved and closure captures carry
 * types. The runtime is single-threaded by design (globals, arenas, signal
 * store, non-atomic vec refcounts, C-seam state), so a fn that will run on
 * its own OS thread must be statically provable to stay off all of it:
 *
 *   - the `thread.spawn` callback must be a closure or a fn name — its code
 *     must be statically visible (a fn value of unknown origin is rejected);
 *   - a closure's captures must be Send (`type_is_send`): plain data only —
 *     int/float/bool/string/Chan<T>/structs of those, never []T (non-atomic
 *     refcount), fn (unchecked body), Box, or a borrow;
 *   - the spawned fn and everything it calls touches no module global (no
 *     reading either — module state belongs to the UI thread) and no C-seam
 *     symbol outside the pure allowlist below. fn values are traced one hop
 *     through their initializer; anything else is rejected.
 *
 * The allowlist is the complete set of C symbols that are safe from any
 * thread: pure value plumbing in yuga_rt.h (string conversion, concat,
 * fmt writes), the monotonic clock and sleep, wrapping/saturating int ops,
 * the channel cell ops (they take their own locks), and `thread.spawn` /
 * `thread.running` themselves. Everything else in the seam — zeus platform
 * calls, net, sys, sig/fut cells, host entry points — touches shared state
 * and stays UI-thread-only.
 */
#include "threadcheck.h"
#include "diagnostics.h"
#include "sema/type.h"
#include <stdlib.h>
#include <string.h>

/* --- small pointer set (dce.c pattern) --------------------------------- */

typedef struct {
    AstNode **items;
    size_t n, cap;
} PtrSet;

static void set_add(PtrSet *s, AstNode *n) {
    if (!n) return;
    for (size_t i = 0; i < s->n; i++)
        if (s->items[i] == n) return;
    if (s->n >= s->cap) {
        s->cap = s->cap ? s->cap * 2 : 64;
        s->items = (AstNode **)realloc(s->items, s->cap * sizeof(AstNode *));
        if (!s->items) { /* keep going without dedup rather than dying */ s->n = 0; return; }
    }
    s->items[s->n++] = n;
}

static int set_has(const PtrSet *s, AstNode *n) {
    for (size_t i = 0; i < s->n; i++)
        if (s->items[i] == n) return 1;
    return 0;
}

/* --- the pass ----------------------------------------------------------- */

typedef struct {
    AstNode **globals;  /* sorted program-level AST_VAR_DECL pointers */
    size_t nglobals;
    PtrSet walked;      /* fn decl bodies already checked in worker mode */
    const char *spawn_file; /* the spawn site current errors are attributed to */
    int spawn_line;
} ThreadCheck;

static int global_cmp(const void *a, const void *b) {
    const AstNode *x = *(const AstNode *const *)a;
    const AstNode *y = *(const AstNode *const *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static int is_module_global(const ThreadCheck *tc, const AstNode *d) {
    if (!d || d->kind != AST_VAR_DECL) return 0;
    return bsearch(&d, tc->globals, tc->nglobals, sizeof(AstNode *), global_cmp) != NULL;
}

/** The C symbol a call resolves to, or NULL. */
static const char *callee_cname(AstNode *call) {
    if (!call || call->kind != AST_CALL) return NULL;
    AstNode *cal = call->as.call.callee;
    if (cal && cal->kind == AST_IDENT && cal->as.ident.resolved &&
        cal->as.ident.resolved->kind == AST_FN_DECL &&
        cal->as.ident.resolved->as.fn.cname)
        return cal->as.ident.resolved->as.fn.cname;
    if (cal && cal->kind == AST_FIELD && cal->as.access.resolved &&
        cal->as.access.resolved->kind == AST_FN_DECL &&
        cal->as.access.resolved->as.fn.cname)
        return cal->as.access.resolved->as.fn.cname;
    return NULL;
}

static int cname_in(const char *cname, const char *const *list) {
    for (int i = 0; list[i]; i++)
        if (strcmp(cname, list[i]) == 0) return 1;
    return 0;
}

/* C symbols a worker may call. Everything else in the seam stays on the UI
   thread. Channel cell ops and spawn/running are thread-safe by construction
   (yuga_rt.h); the rest is pure value plumbing. */
static const char *const worker_seam_ok[] = {
    "yuga_thread_spawn", "yuga_thread_running",
    "yuga_ch_alloc", "yuga_ch_send", "yuga_ch_try_send", "yuga_ch_recv",
    "yuga_ch_pop", "yuga_ch_ready",
    "yuga_fmt_write", "yuga_fmt_writeln", "yuga_fmt_write_int",
    "yuga_fmt_write_bool", "yuga_fmt_write_float", "yuga_fmt_eq",
    "yuga_str_of_int", "yuga_str_of_float", "yuga_str_of_bool",
    "yuga_str_of_string", "yuga_str_concat", "yuga_string_from_bytes",
    "yuga_async_now_ms", "yuga_async_sleep",
    "yuga_wrapping_add", "yuga_wrapping_shr", "yuga_wrapping_shl",
    "yuga_wrapping_or", "yuga_wrapping_and", "yuga_saturating_add",
    NULL,
};

/* Bodyless fns of the std seam (declared with an empty body in the ffi
   modules). Their bodies live in C; a worker calling one is calling shared
   C state, so only the allowlist above is reachable. */
static int decl_is_seam(const AstNode *fn) {
    return fn && fn->kind == AST_FN_DECL && fn->as.fn.is_intrinsic;
}

static void worker_walk(AstNode *n, ThreadCheck *tc);

static void report(const ThreadCheck *tc, SourceLoc loc, const char *fmt,
                   const char *a1, const char *a2) {
    char msg[512];
    if (tc->spawn_file)
        snprintf(msg, sizeof msg, "%s (thread.spawn at %s:%d)", fmt, tc->spawn_file,
                 tc->spawn_line);
    else
        snprintf(msg, sizeof msg, "%s", fmt);
    yuga_error(loc, msg, a1, a2);
}

/** Follow one CALL site in worker mode. */
static void worker_call(AstNode *call, ThreadCheck *tc) {
    AstNode *cal = call->as.call.callee;
    size_t i;

    /* Vec and interpolation builtins operate on worker-local storage only
       (module globals are banned above, so no shared vec can be reached). */
    if (call->as.call.is_vec_push || call->as.call.is_vec_pop || call->as.call.is_println ||
        call->as.call.num_builtin != NUMB_NONE) {
        for (i = 0; i < call->as.call.arg_count; i++)
            worker_walk(call->as.call.args[i], tc);
        return;
    }
    /* Pure value helpers wired by name in typecheck (yuga_str_of_*,
       string_from_bytes, concat, wrapping bit ops). */
    if (call->as.call.c_builtin) {
        const char *c = call->as.call.c_builtin;
        if (!cname_in(c, worker_seam_ok)) {
            char buf[128];
            snprintf(buf, sizeof buf, "spawned fn calls the C seam '%s'", c);
            report(tc, call->loc, buf, NULL, NULL);
        }
        for (i = 0; i < call->as.call.arg_count; i++)
            worker_walk(call->as.call.args[i], tc);
        return;
    }
    /* Unresolved magic names: the channel cells (__ch_*) are the sanctioned
       worker I/O; the future/signal cells are UI-thread state. */
    if (cal && cal->kind == AST_IDENT && !cal->as.ident.resolved &&
        cal->as.ident.name && strncmp(cal->as.ident.name, "__ch_", 5) == 0) {
        for (i = 0; i < call->as.call.arg_count; i++)
            worker_walk(call->as.call.args[i], tc);
        return;
    }
    if (cal && cal->kind == AST_IDENT && !cal->as.ident.resolved &&
        cal->as.ident.name && strncmp(cal->as.ident.name, "__fut_", 6) == 0) {
        report(tc, call->loc,
               "spawned fn uses a future cell (UI-thread state); workers return values "
               "through channel<T> only",
               NULL, NULL);
        return;
    }
    /* A closure called directly: its body runs on this thread. */
    if (cal && cal->kind == AST_CLOSURE) {
        worker_walk(cal->as.fn.body, tc);
        for (i = 0; i < call->as.call.arg_count; i++)
            worker_walk(call->as.call.args[i], tc);
        return;
    }
    /* A fn value call (local or param of proc type — typecheck sets this for
       anything that is not a direct fn-name/closure call). Trace the local's
       initializer one hop (let f = worker / let f = || {...}); params and
       deeper aliasing are rejected. */
    if (call->as.call.is_fn_val) {
        AstNode *res = NULL;
        if (cal && cal->kind == AST_IDENT) res = cal->as.ident.resolved;
        if (cal && cal->kind == AST_FIELD) res = cal->as.access.resolved;
        if (res && res->kind == AST_VAR_DECL && res->as.var.init) {
            AstNode *init = res->as.var.init;
            if (init->kind == AST_CLOSURE) {
                worker_walk(init->as.fn.body, tc);
                for (i = 0; i < call->as.call.arg_count; i++)
                    worker_walk(call->as.call.args[i], tc);
                return;
            }
            AstNode *fn = NULL;
            if (init->kind == AST_IDENT) fn = init->as.ident.resolved;
            if (init->kind == AST_FIELD) fn = init->as.access.resolved;
            if (fn && fn->kind == AST_FN_DECL && !fn->as.fn.tparam_count &&
                !decl_is_seam(fn)) {
                if (!set_has(&tc->walked, fn)) {
                    set_add(&tc->walked, fn);
                    worker_walk(fn->as.fn.body, tc);
                }
                for (i = 0; i < call->as.call.arg_count; i++)
                    worker_walk(call->as.call.args[i], tc);
                return;
            }
        }
        report(tc, call->loc,
               "spawned fn calls a fn value whose code is not statically visible (pass a fn "
               "name or call the closure directly)",
               NULL, NULL);
        for (i = 0; i < call->as.call.arg_count; i++)
            worker_walk(call->as.call.args[i], tc);
        return;
    }
    /* Resolved fn decls: intrinsic = C seam (allowlist), else walk the body. */
    {
        AstNode *res = NULL;
        if (cal && cal->kind == AST_IDENT) res = cal->as.ident.resolved;
        if (cal && cal->kind == AST_FIELD) res = cal->as.access.resolved;
        if (res && res->kind == AST_FN_DECL) {
            const char *cn = res->as.fn.cname;
            if (decl_is_seam(res)) {
                if (!cn || !cname_in(cn, worker_seam_ok)) {
                    char buf[160];
                    snprintf(buf, sizeof buf,
                             "spawned fn calls the C seam '%s', which is not allowed from a "
                             "worker (allowed: pure value helpers and channel ops)",
                             cn ? cn : "?");
                    report(tc, call->loc, buf, NULL, NULL);
                }
                return;
            }
            if (!res->as.fn.tparam_count) {
                if (!set_has(&tc->walked, res)) {
                    set_add(&tc->walked, res);
                    worker_walk(res->as.fn.body, tc);
                }
            }
            for (i = 0; i < call->as.call.arg_count; i++)
                worker_walk(call->as.call.args[i], tc);
            return;
        }
        if (cal && cal->kind == AST_IDENT && cal->as.ident.name &&
            strcmp(cal->as.ident.name, "push") == 0) {
            /* vec push on a local — same argument as is_vec_push above */
            for (i = 0; i < call->as.call.arg_count; i++)
                worker_walk(call->as.call.args[i], tc);
            return;
        }
    }
    report(tc, call->loc,
           "spawned fn calls a fn value whose code is not statically visible (pass a fn "
           "name or call the closure directly)",
           NULL, NULL);
    for (i = 0; i < call->as.call.arg_count; i++)
        worker_walk(call->as.call.args[i], tc);
}

static void worker_walk(AstNode *n, ThreadCheck *tc) {
    size_t i;
    if (!n) return;
    switch (n->kind) {
        case AST_CALL:
            worker_call(n, tc);
            break;
        case AST_IDENT:
            /* A read of a module global from worker code. */
            if (n->as.ident.resolved &&
                is_module_global(tc, n->as.ident.resolved)) {
                char buf[160];
                snprintf(buf, sizeof buf,
                         "spawned fn touches module state '%s'; workers may only use their "
                         "captures and channels",
                         n->as.ident.name ? n->as.ident.name : "?");
                report(tc, n->loc, buf, NULL, NULL);
            }
            break;
        case AST_FIELD:
            if (n->as.access.resolved &&
                is_module_global(tc, n->as.access.resolved)) {
                char buf[160];
                snprintf(buf, sizeof buf,
                         "spawned fn touches module state (a module-level `let`); workers may "
                         "only use their captures and channels");
                report(tc, n->loc, buf, NULL, NULL);
            }
            worker_walk(n->as.access.target, tc);
            worker_walk(n->as.access.index, tc);
            break;
        case AST_FN_DECL:
        case AST_CLOSURE:
            worker_walk(n->as.fn.body, tc);
            break;
        case AST_BLOCK:
            for (i = 0; i < n->as.block.stmt_count; i++)
                worker_walk(n->as.block.stmts[i], tc);
            break;
        case AST_IF:
            worker_walk(n->as.if_stmt.cond, tc);
            worker_walk(n->as.if_stmt.then_block, tc);
            worker_walk(n->as.if_stmt.else_block, tc);
            break;
        case AST_FOR:
            worker_walk(n->as.for_stmt.iter, tc);
            worker_walk(n->as.for_stmt.body, tc);
            break;
        case AST_WHILE:
            worker_walk(n->as.if_stmt.cond, tc);
            worker_walk(n->as.if_stmt.then_block, tc);
            break;
        case AST_MATCH:
            worker_walk(n->as.match_stmt.scrut, tc);
            for (i = 0; i < n->as.match_stmt.arm_count; i++)
                worker_walk(n->as.match_stmt.arms[i], tc);
            break;
        case AST_MATCH_ARM:
            worker_walk(n->as.match_arm.body, tc);
            break;
        case AST_RETURN:
            worker_walk(n->as.ret.expr, tc);
            break;
        case AST_EXPR_STMT:
            worker_walk(n->as.expr_stmt.expr, tc);
            break;
        case AST_VAR_DECL:
            worker_walk(n->as.var.init, tc);
            break;
        case AST_ASSIGN:
            worker_walk(n->as.assign.left, tc);
            worker_walk(n->as.assign.right, tc);
            break;
        case AST_BINARY:
            worker_walk(n->as.binary.left, tc);
            worker_walk(n->as.binary.right, tc);
            break;
        case AST_UNARY:
            worker_walk(n->as.unary.operand, tc);
            break;
        case AST_CAST:
            worker_walk(n->as.cast.expr, tc);
            break;
        case AST_INDEX:
        case AST_DEREF:
        case AST_ADDR:
            worker_walk(n->as.access.target, tc);
            worker_walk(n->as.access.index, tc);
            break;
        case AST_STRUCT_LIT:
            for (i = 0; i < n->as.struct_lit.field_count; i++)
                worker_walk(n->as.struct_lit.fields[i].init, tc);
            break;
        case AST_ARRAY_LIT:
        case AST_TUPLE:
            for (i = 0; i < n->as.array_lit.count; i++)
                worker_walk(n->as.array_lit.elems[i], tc);
            break;
        default:
            break; /* types, literals: nothing to check */
    }
}

/* --- root scan: find every thread.spawn call in the program ------------- */

static void scan_walk(AstNode *n, ThreadCheck *tc);

/** Validate one spawn call site and check its callback body in worker mode. */
static void scan_spawn(AstNode *call, ThreadCheck *tc) {
    AstNode *cb = call->as.call.arg_count > 0 ? call->as.call.args[0] : NULL;
    size_t i;
    tc->spawn_file = call->loc.file;
    tc->spawn_line = call->loc.line;
    if (!cb) {
        report(tc, call->loc, "thread.spawn expects a callback", NULL, NULL);
        return;
    }
    if (cb->kind == AST_CLOSURE) {
        for (i = 0; i < cb->as.fn.cap_count; i++) {
            Type *ct = cb->as.fn.cap_types[i];
            if (!ct || !type_is_send(ct)) {
                char buf[256];
                snprintf(buf, sizeof buf,
                         "thread.spawn callback captures '%s' (type %s), which is not Send: "
                         "workers may carry only plain data (no []T, fn, Box, or borrows)",
                         cb->as.fn.caps[i] ? cb->as.fn.caps[i] : "?", type_name(ct));
                report(tc, cb->loc, buf, NULL, NULL);
            }
        }
        worker_walk(cb->as.fn.body, tc);
        return;
    }
    AstNode *res = NULL;
    if (cb->kind == AST_IDENT) res = cb->as.ident.resolved;
    if (cb->kind == AST_FIELD) res = cb->as.access.resolved;
    if (res && res->kind == AST_FN_DECL) {
        if (res->as.fn.tparam_count) {
            report(tc, call->loc, "thread.spawn cannot take a generic fn", NULL, NULL);
            return;
        }
        if (decl_is_seam(res)) {
            report(tc, call->loc, "thread.spawn cannot run a C seam fn", NULL, NULL);
            return;
        }
        if (!set_has(&tc->walked, res)) {
            set_add(&tc->walked, res);
            worker_walk(res->as.fn.body, tc);
        }
        return;
    }
    report(tc, call->loc,
           "thread.spawn callback must be a closure or a fn name, not a fn value", NULL,
           NULL);
}

static void scan_walk(AstNode *n, ThreadCheck *tc) {
    size_t i;
    if (!n) return;
    switch (n->kind) {
        case AST_CALL: {
            AstNode *cal = n->as.call.callee;
            const char *cn = callee_cname(n);
            if (cn && strcmp(cn, "yuga_thread_spawn") == 0) {
                scan_spawn(n, tc);
                return;
            }
            scan_walk(cal, tc);
            for (i = 0; i < n->as.call.arg_count; i++)
                scan_walk(n->as.call.args[i], tc);
            break;
        }
        case AST_FIELD:
            scan_walk(n->as.access.target, tc);
            scan_walk(n->as.access.index, tc);
            break;
        case AST_FN_DECL:
            scan_walk(n->as.fn.body, tc);
            break;
        case AST_CLOSURE:
            scan_walk(n->as.fn.body, tc);
            break;
        case AST_BLOCK:
            for (i = 0; i < n->as.block.stmt_count; i++)
                scan_walk(n->as.block.stmts[i], tc);
            break;
        case AST_IF:
            scan_walk(n->as.if_stmt.cond, tc);
            scan_walk(n->as.if_stmt.then_block, tc);
            scan_walk(n->as.if_stmt.else_block, tc);
            break;
        case AST_FOR:
            scan_walk(n->as.for_stmt.iter, tc);
            scan_walk(n->as.for_stmt.body, tc);
            break;
        case AST_WHILE:
            scan_walk(n->as.if_stmt.cond, tc);
            scan_walk(n->as.if_stmt.then_block, tc);
            break;
        case AST_MATCH:
            scan_walk(n->as.match_stmt.scrut, tc);
            for (i = 0; i < n->as.match_stmt.arm_count; i++)
                scan_walk(n->as.match_stmt.arms[i], tc);
            break;
        case AST_MATCH_ARM:
            scan_walk(n->as.match_arm.body, tc);
            break;
        case AST_RETURN:
            scan_walk(n->as.ret.expr, tc);
            break;
        case AST_EXPR_STMT:
            scan_walk(n->as.expr_stmt.expr, tc);
            break;
        case AST_VAR_DECL:
            scan_walk(n->as.var.init, tc);
            break;
        case AST_ASSIGN:
            scan_walk(n->as.assign.left, tc);
            scan_walk(n->as.assign.right, tc);
            break;
        case AST_BINARY:
            scan_walk(n->as.binary.left, tc);
            scan_walk(n->as.binary.right, tc);
            break;
        case AST_UNARY:
            scan_walk(n->as.unary.operand, tc);
            break;
        case AST_CAST:
            scan_walk(n->as.cast.expr, tc);
            break;
        case AST_INDEX:
        case AST_DEREF:
        case AST_ADDR:
            scan_walk(n->as.access.target, tc);
            scan_walk(n->as.access.index, tc);
            break;
        case AST_STRUCT_LIT:
            for (i = 0; i < n->as.struct_lit.field_count; i++)
                scan_walk(n->as.struct_lit.fields[i].init, tc);
            break;
        case AST_ARRAY_LIT:
        case AST_TUPLE:
            for (i = 0; i < n->as.array_lit.count; i++)
                scan_walk(n->as.array_lit.elems[i], tc);
            break;
        default:
            break;
    }
}

/** Check every module (spawn sites anywhere, incl. dead fns — same rule as
 *  typecheck/IR covering everything). Returns the number of errors. */
int threadcheck_modules(YugaModule *mods, int nmods) {
    ThreadCheck tc;
    int errs_before = yuga_diag_count();
    int have_thread = 0;
    tc.nglobals = 0;
    tc.globals = NULL;
    tc.walked.n = tc.walked.cap = 0;
    tc.walked.items = NULL;
    tc.spawn_file = NULL;
    tc.spawn_line = 0;
    for (int m = 0; m < nmods; m++)
        if (mods[m].name && strcmp(mods[m].name, "thread") == 0) have_thread = 1;
    if (!have_thread) return 0;

    /* Module-level `let`s: the state a worker must not touch. */
    for (int m = 0; m < nmods; m++) {
        AstNode *p = mods[m].ast;
        if (!p) continue;
        for (size_t i = 0; i < p->as.program.decl_count; i++) {
            AstNode *d = p->as.program.decls[i];
            if (d->kind != AST_VAR_DECL) continue;
            if (tc.nglobals == 0 || tc.nglobals % 64 == 0)
                tc.globals = (AstNode **)realloc(tc.globals,
                                                 (tc.nglobals + 64) * sizeof(AstNode *));
            tc.globals[tc.nglobals++] = d;
        }
    }
    qsort(tc.globals, tc.nglobals, sizeof(AstNode *), global_cmp);

    /* Root scan: fn decls (incl. generic decls — mono instances reuse the
       same AST) and module-level inits. Each spawn found is validated and
       its callback body checked right there. */
    for (int m = 0; m < nmods; m++) {
        AstNode *p = mods[m].ast;
        if (!p) continue;
        for (size_t i = 0; i < p->as.program.decl_count; i++) {
            AstNode *d = p->as.program.decls[i];
            if (d->kind == AST_FN_DECL && !d->as.fn.is_intrinsic) {
                if (d->as.fn.body && d->as.fn.body->kind == AST_BLOCK)
                    scan_walk(d->as.fn.body, &tc);
            } else if (d->kind == AST_VAR_DECL) {
                scan_walk(d->as.var.init, &tc);
            }
        }
    }
    free(tc.globals);
    free(tc.walked.items);
    return yuga_diag_count() - errs_before;
}
