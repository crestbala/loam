# Loader cases

Entry files for the module loader's package rules, not programs to run: a
vendored package with a good and a bad lock checksum, one the lock does not
list, `pkg:` imports, workspaces (multi-line, inline, and an unterminated
`members` quote), a `[package]` with an `entry`, `name:module` imports, a `.loa`
file, an import cycle, a missing file, and a user module named like a std one.

`nob integration loader_cases` (part of `nob test`) runs `loamc check` on
every `.loam` file here and compares what it prints and how it exits with
`expected.txt` (`NOB_UPDATE_EXPECTED=1` rewrites it). `vendor/good`'s
checksum in `app1/loam.lock` is the tree hash of that directory: edit a file in
it and the lock must be updated (`zeli pkg sync`), or `ok.loam` reports a
mismatch.
