# crypto

`std:crypto`: password hashing, keyed hashing, constant-time comparison, and OS
random bytes for native Loam programs, through `#[c]`. No cryptography is written
in Loam. The base64 here only formats stored hashes.

`std/vendor/` holds `monocypher.c`, `monocypher.h`, and `LICENCE.md` from Monocypher
4.0.3, unmodified. Monocypher is CC0-1.0, or 2-clause BSD. Cure53 audited Monocypher
in June 2020, which predates this version.

- Source: https://monocypher.org/download/monocypher-4.0.3.tar.gz, identical to the
  GitHub release asset
- SHA-512 and BLAKE2b: checked against the files published beside the tarball

`argon2_shim.c` is ours. It only unpacks arguments, as `crypto_argon2` takes structs
by value and `#[c]` does not pass them.

To upgrade, replace the three vendored files, update this page, and run
`./bin/nob test crypto`.
