/**
 * dce.c — reachability analysis over resolved AST call graph.
 *
 * Roots (everything else is pruned unless reachable):
 *   - module 0 (the entry program) is emitted whole: every fn decl.
 *   - module-level `let mut` initializers are compiled into per-module
 *     `__init` functions that codegen emits unconditionally, so every
 *     init expression in every module is a root.
 *   - the fixed `loam_zeus_*` entry points the C runtime and hosts call
 *     (engine_*, `on_action*`, `get`/`set`/`signal`, ...).
 *
 * Reachability: worklist over fn decl bodies (incl. parameter defaults,
 * which are cloned into reachable call sites). A call or fn-value use
 * resolves through `ident.resolved` / `access.resolved` (typecheck fills
 * these). Generic decls are monomorphized per call site; when a generic
 * decl is *not* reachable but its body contains closures, those closures
 * still get emitted from the IR with concrete substitutions, so the body
 * is walked (without keeping the decl) to keep their callees alive.
 *
 * Intrinsic decls (empty std hooks that map to C symbols) and the
 * `is_main` entry are handled by codegen's own loops; this pass only
 * answers loam_dce_keep() for regular fn decls.
 */
#include "dce.h"
#include "sema/typecheck.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    AstNode **items;
    size_t n;
    size_t cap;
} DceSet;

static DceSet kept;   /* fn decls whose C is emitted */
static DceSet todo;   /* fn decls still to process (body walk) */
static DceSet done;   /* fn decls already processed */
static int ran;       /* loam_dce_run completed (LOAM_NO_DCE skips it) */
static int dce_test_mode;

void loam_dce_set_test_mode(int on) {
    dce_test_mode = on;
}

static int in_set(const DceSet *s, const AstNode *d) {
    for (size_t i = 0; i < s->n; i++)
        if (s->items[i] == d) return 1;
    return 0;
}

static void add_set(DceSet *s, AstNode *d) {
    if (!d || in_set(s, d)) return;
    if (s->n >= s->cap) {
        s->cap = s->cap ? s->cap * 2 : 256;
        s->items = (AstNode **)realloc(s->items, s->cap * sizeof(AstNode *));
        if (!s->items) {
            /* Keep compiling without DCE rather than dying mid-emit. */
            s->n = 0;
            return;
        }
    }
    s->items[s->n++] = d;
}

/** C symbols the runtime/hosts may reference, by module prefix
 *  (`loam_zeus_*` from zeus_rt.h/zeus_plat.c/hosts, `loam_maya_*` from
 *  maya_rt.h/maya_plat.c/maya_mac.m). Entries that no longer exist in the
 *  module are ignored. Keep in sync when hosts grow new entry points. */
static const char *const c_roots_zeus[] = {
    "engine_layout", "engine_paint", "engine_step", "engine_step_dt",
    "engine_set_reduced_motion", "engine_next_ms",
    "engine_click", "engine_scroll", "engine_scroll_step", "engine_scroll_smooth", "engine_drag",
    "engine_hover", "engine_mouseup", "engine_over_button", "engine_cursor",
    "engine_a11y_dump", "engine_a11y_bytes", "engine_layout_gen", "engine_signals_dump", "engine_state_load",
    "engine_key_apply", "engine_fill_focus", "engine_focus_depth",
    "engine_focus_node", "engine_focus_ctx", "engine_focus_step",
    "engine_focus_captures_text", "engine_key", "engine_key_up",
    "engine_set_mods", "engine_insert", "engine_marked", "engine_picked_image",
    "on_action", "on_action_global", "on_range", "map_key", "remap_key",
    "get", "set", "signal",
    NULL,
};

static const char *const c_roots_maya[] = {
    "engine_cam_orbit", "engine_cam_pan", "engine_cam_reset",
    "engine_fixed_update", "engine_frame", "engine_hud", "engine_input_key",
    "engine_cam_zoom", "engine_fb_at", "engine_fb_h", "engine_fb_w",
    "engine_is_map", "engine_note_present", "engine_set_viewport",
    "engine_sprite_count", "engine_sprite_is_sun", "engine_sprite_name",
    "engine_sprite_orbit_r", "engine_sprite_r", "engine_sprite_rgb",
    "engine_sprite_x", "engine_sprite_y",
    NULL,
};

/* `packages/zeus/std/zeuscore/metrics.loam` is not named `zeus`, but the web host calls its
   `bind` from C (`zeus_font_set` → `loam_metrics_bind`), so it must survive DCE
   even when no app call to `zeus.use_font` reaches it. */
static const char *const c_roots_metrics[] = {
    "bind",
    NULL,
};

/* `std:router` hosts call `engine_open_url` (a deep link) and
   `engine_history_back` (an OS back button) from C, so they must survive DCE
   even when the app never calls them directly. */
static const char *const c_roots_router[] = {
    "engine_open_url", "engine_history_back",
    NULL,
};

static void mark_fn(AstNode *fn) {
    if (!fn || fn->kind != AST_FN_DECL) return;
    add_set(&kept, fn);
    add_set(&todo, fn);
}

static void walk(AstNode *n);

static void walk_params(AstNode *fn) {
    for (size_t i = 0; i < fn->as.fn.param_count; i++)
        if (fn->as.fn.params[i].def) walk(fn->as.fn.params[i].def);
}

static void walk(AstNode *n) {
    if (!n) return;
    switch (n->kind) {
        case AST_IDENT:
            if (n->as.ident.resolved && n->as.ident.resolved->kind == AST_FN_DECL)
                mark_fn(n->as.ident.resolved);
            break;
        case AST_FIELD:
            if (n->as.access.resolved && n->as.access.resolved->kind == AST_FN_DECL)
                mark_fn(n->as.access.resolved);
            walk(n->as.access.target);
            walk(n->as.access.index);
            break;
        case AST_FN_DECL:
            walk(n->as.fn.body);
            walk_params(n);
            break;
        case AST_CLOSURE:
            walk(n->as.fn.body);
            walk_params(n);
            break;
        case AST_BLOCK:
            for (size_t i = 0; i < n->as.block.stmt_count; i++)
                walk(n->as.block.stmts[i]);
            break;
        case AST_IF:
            walk(n->as.if_stmt.cond);
            walk(n->as.if_stmt.then_block);
            walk(n->as.if_stmt.else_block);
            break;
        case AST_FOR:
            walk(n->as.for_stmt.iter);
            walk(n->as.for_stmt.body);
            break;
        case AST_WHILE:
            walk(n->as.if_stmt.cond);
            walk(n->as.if_stmt.then_block);
            break;
        case AST_MATCH:
            walk(n->as.match_stmt.scrut);
            for (size_t i = 0; i < n->as.match_stmt.arm_count; i++)
                walk(n->as.match_stmt.arms[i]);
            break;
        case AST_MATCH_ARM:
            for (size_t i = 0; i < n->as.match_arm.pat_count; i++)
                walk(n->as.match_arm.pats[i]);
            walk(n->as.match_arm.body);
            break;
        case AST_RETURN:
            walk(n->as.ret.expr);
            break;
        case AST_EXPR_STMT:
            walk(n->as.expr_stmt.expr);
            break;
        case AST_VAR_DECL:
            walk(n->as.var.init);
            break;
        case AST_ASSIGN:
            walk(n->as.assign.left);
            walk(n->as.assign.right);
            break;
        case AST_BINARY:
            walk(n->as.binary.left);
            walk(n->as.binary.right);
            break;
        case AST_UNARY:
            walk(n->as.unary.operand);
            break;
        case AST_CAST:
            walk(n->as.cast.expr);
            break;
        case AST_CALL:
            walk(n->as.call.callee);
            for (size_t i = 0; i < n->as.call.arg_count; i++)
                walk(n->as.call.args[i]);
            break;
        case AST_INDEX:
        case AST_DEREF:
        case AST_ADDR:
            walk(n->as.access.target);
            walk(n->as.access.index);
            break;
        case AST_STRUCT_LIT:
            for (size_t i = 0; i < n->as.struct_lit.field_count; i++)
                walk(n->as.struct_lit.fields[i].init);
            break;
        case AST_ARRAY_LIT:
        case AST_TUPLE:
            for (size_t i = 0; i < n->as.array_lit.count; i++)
                walk(n->as.array_lit.elems[i]);
            break;
        default:
            break; /* types, literals, enums, structs: nothing to call */
    }
}

/** 1 if `n` contains any closure node (i.e. a generic decl may emit
 *  substituted closure bodies from the IR even when the decl is pruned). */
static int has_closure(AstNode *n) {
    if (!n) return 0;
    if (n->kind == AST_CLOSURE) return 1;
    switch (n->kind) {
        case AST_FN_DECL:
            return has_closure(n->as.fn.body);
        case AST_BLOCK:
            for (size_t i = 0; i < n->as.block.stmt_count; i++)
                if (has_closure(n->as.block.stmts[i])) return 1;
            return 0;
        case AST_IF:
            return has_closure(n->as.if_stmt.cond) ||
                   has_closure(n->as.if_stmt.then_block) ||
                   has_closure(n->as.if_stmt.else_block);
        case AST_FOR:
            return has_closure(n->as.for_stmt.iter) ||
                   has_closure(n->as.for_stmt.body);
        case AST_WHILE:
            return has_closure(n->as.if_stmt.cond) ||
                   has_closure(n->as.if_stmt.then_block);
        case AST_MATCH: {
            int any = has_closure(n->as.match_stmt.scrut);
            for (size_t i = 0; i < n->as.match_stmt.arm_count && !any; i++)
                any = has_closure(n->as.match_stmt.arms[i]);
            return any;
        }
        case AST_MATCH_ARM:
            return has_closure(n->as.match_arm.body);
        case AST_RETURN:
            return has_closure(n->as.ret.expr);
        case AST_EXPR_STMT:
            return has_closure(n->as.expr_stmt.expr);
        case AST_VAR_DECL:
            return has_closure(n->as.var.init);
        case AST_ASSIGN:
            return has_closure(n->as.assign.left) ||
                   has_closure(n->as.assign.right);
        case AST_BINARY:
            return has_closure(n->as.binary.left) ||
                   has_closure(n->as.binary.right);
        case AST_UNARY:
            return has_closure(n->as.unary.operand);
        case AST_CAST:
            return has_closure(n->as.cast.expr);
        case AST_CALL:
            if (has_closure(n->as.call.callee)) return 1;
            for (size_t i = 0; i < n->as.call.arg_count; i++)
                if (has_closure(n->as.call.args[i])) return 1;
            return 0;
        case AST_INDEX:
        case AST_DEREF:
        case AST_ADDR:
            return has_closure(n->as.access.target) ||
                   has_closure(n->as.access.index);
        case AST_STRUCT_LIT:
            for (size_t i = 0; i < n->as.struct_lit.field_count; i++)
                if (has_closure(n->as.struct_lit.fields[i].init)) return 1;
            return 0;
        case AST_ARRAY_LIT:
        case AST_TUPLE:
            for (size_t i = 0; i < n->as.array_lit.count; i++)
                if (has_closure(n->as.array_lit.elems[i])) return 1;
            return 0;
        default:
            return 0;
    }
}

/** Which generic decls were instantiated anywhere (mono records hold the
 *  original decl, so a decl with an instance is easy to spot). */
static int has_mono_instance(AstNode *decl) {
    int n = typecheck_mono_count();
    for (int i = 0; i < n; i++)
        if (typecheck_mono_fn(i) == decl) return 1;
    return 0;
}

void loam_dce_run(LoamModule *mods, int nmods) {
    kept.n = todo.n = done.n = 0;
    ran = 1;

    /* Roots: module 0 fn decls, every module's var inits, C entry points. */
    for (int m = 0; m < nmods; m++) {
        AstNode *p = mods[m].ast;
        if (!p) continue;
        /* `loam test` calls test.begin/ok/summary from generated C. */
        int test_mod = dce_test_mode && mods[m].name && strcmp(mods[m].name, "test") == 0;
        for (size_t i = 0; i < p->as.program.decl_count; i++) {
            AstNode *d = p->as.program.decls[i];
            if (test_mod && d->kind == AST_FN_DECL) {
                mark_fn(d);
                continue;
            }
            if (d->kind == AST_VAR_DECL) {
                walk(d->as.var.init);
                continue;
            }
            if (d->kind != AST_FN_DECL) continue;
            /* Phase 12: a `#[test]` fn is reachable from the generated test
               runner in every module, not just the entry module. */
            if (d->as.fn.is_test) {
                mark_fn(d);
                continue;
            }
            if (m == 0) {
                mark_fn(d);
                continue;
            }
            if (!d->as.fn.cname || !mods[m].name) continue;
            {
                const char *const *roots = NULL;
                if (strcmp(mods[m].name, "zeus") == 0)
                    roots = c_roots_zeus;
                else if (strcmp(mods[m].name, "maya") == 0)
                    roots = c_roots_maya;
                else if (strcmp(mods[m].name, "metrics") == 0)
                    roots = c_roots_metrics;
                else if (strcmp(mods[m].name, "router") == 0)
                    roots = c_roots_router;
                if (roots) {
                    for (int r = 0; roots[r]; r++) {
                        char want[128];
                        snprintf(want, sizeof want, "loam_%s_%s", mods[m].name, roots[r]);
                        if (strcmp(d->as.fn.cname, want) == 0) {
                            mark_fn(d);
                            break;
                        }
                    }
                }
            }
        }
    }

    /* Generic decls pruned but instantiated: their substituted bodies are
     * emitted from the IR, not walked above (a generic call resolves to the
     * instance, not the decl), so mark every callee of every instantiated
     * generic or the instance links against a dropped callee. */
    for (int m = 0; m < nmods; m++) {
        AstNode *p = mods[m].ast;
        if (!p) continue;
        for (size_t i = 0; i < p->as.program.decl_count; i++) {
            AstNode *d = p->as.program.decls[i];
            if (d->kind != AST_FN_DECL || !d->as.fn.tparam_count) continue;
            if (in_set(&kept, d)) continue;
            if (!has_mono_instance(d)) continue;
            walk(d->as.fn.body);
            walk_params(d);
        }
    }

    /* Worklist: process each kept decl's body once, to a fixed point (the set
     * grows while it is walked). Runs *after* the instantiated-generic pass so
     * the callees that pass marks are walked too: a pruned generic's body can
     * call a non-generic (`arena.key_count` from `kfor_refresh`) which itself
     * calls others, and the emitted instance needs all of them. */
    for (size_t i = 0; i < todo.n; i++) {
        AstNode *fn = todo.items[i];
        if (in_set(&done, fn)) continue;
        add_set(&done, fn);
        if (fn->kind == AST_FN_DECL) {
            walk(fn->as.fn.body);
            walk_params(fn);
        }
    }
}

int loam_dce_keep(const AstNode *fn_decl) {
    if (!fn_decl) return 0;
    if (!ran) return 1; /* pass skipped: emit the full monolith as before */
    return in_set(&kept, fn_decl);
}
