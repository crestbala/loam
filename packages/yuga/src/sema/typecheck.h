/**
 * typecheck.h — name resolution, types, auto-borrow, generics, closures.
 *
 * Pass 1 assigns C names. Empty bodies in std boundary modules
 * (`platform`, `fmt`, `maya`, `sys`, `net`; `http` if it has empty `fn`s) are link-boundary
 * FFI (`is_intrinsic`): declaration only, symbol `yuga_<mod>_<name>`.
 * Pass 2 checks bodies. Generic calls are monomorphized; codegen asks
 * for those instances via typecheck_mono_*.
 *
 * Capturing closures own a heap env (RAII, like Box) and may escape.
 */
#ifndef YUGA_TYPECHECK_H
#define YUGA_TYPECHECK_H

#include "../module.h"

/** Typecheck all modules. mods[0] is main. Returns 1 if any error. */
int typecheck_modules(YugaModule *mods, int nmods);

/** Free monomorphization tables and the type pool. */
void typecheck_cleanup(void);

int typecheck_mono_count(void);
AstNode *typecheck_mono_fn(int i);
const char *typecheck_mono_cname(int i);
Type **typecheck_mono_args(int i);
size_t typecheck_mono_nargs(int i);

int typecheck_struct_inst_count(void);
Type *typecheck_struct_inst(int i);

Type *typecheck_subst(Type *t, const char **names, Type **args, size_t n);

/** C name of a generic callee under the caller's type subst, recording a
    monomorphized instance when T becomes concrete. NULL if T is still open. */
const char *typecheck_callee_cname(AstNode *call, const char **names, Type **args, size_t n);

/** Module-level `let` bindings, visible as `name` in the same module and as
    `mod.name` from importers. */
int typecheck_global_count(void);
AstNode *typecheck_global_var(int i);
const char *typecheck_global_cname(int i);
int typecheck_global_mod(int i);

/** `Signal` from std:zeus, or NULL if that struct is not in this compile. */
Type *typecheck_signal_type(void);

#endif
