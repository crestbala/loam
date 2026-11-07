# sqlite

`sqlite`: SQLite for native Loam programs, through `#[c]`. Server builds do not
depend on the machine: the amalgamation is vendored and compiled with the program.

`std/vendor/` holds `sqlite3.c` and `sqlite3.h` from SQLite 3.54.1, unmodified.
SQLite is in the public domain.

- Source: https://sqlite.org/2026/sqlite-amalgamation-3540100.zip
- SHA3-256: `92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30`

To upgrade, replace both files, update this page, and run `./bin/nob test sqlite`.
The first build of a program compiles SQLite, which takes a while. Later builds
reuse the object.
