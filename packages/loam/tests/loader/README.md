# Loader cases

Entry files for the module loader's package rules, not programs to run: a
vendored package with a good and a bad lock checksum, one the lock does not
list, `pkg:` imports, workspaces (multi-line, inline, and an unterminated
`members` quote), a `[package]` with an `entry`, `name:module` imports, a `.loa`
file, an import cycle, a missing file, and a user module named like a std one.

`nob integration selfhost_load` loads every `.loam` file in the repo with both
loaders (`loamc --dump-modules` and `compiler/moddump.loam`) and diffs the module
lists and diagnostics, so these cases are checked there. `vendor/good`'s
checksum in `app1/loam.lock` is the tree hash of that directory: edit a file in
it and the lock must be updated (`zeli pkg sync`), or `ok.loam` reports a
mismatch.
