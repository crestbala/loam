# Loam compiler — C11, libc only. Generated programs are gnu99 (C99 + statement exprs).

CC      := cc
CFLAGS  := -std=c11 -Wall -Wextra -Wpedantic -O2 -g -MMD -MP
CFLAGS  += -DLOAM_RT_PATH=\"$(CURDIR)/packages/loam/runtime/loam_rt.h\"
CFLAGS  += -DLOAM_RUNTIME_DIR=\"$(CURDIR)/packages/loam/runtime\"
CFLAGS  += -DLOAM_STD_DIR=\"$(CURDIR)/packages/loam/std\"
CFLAGS  += -DLOAM_ZEUS_DIR=\"$(CURDIR)/packages/zeus\"
CFLAGS  += -DLOAM_RAYGUI_DIR=\"$(CURDIR)/packages/raygui\"
CFLAGS  += -DLOAM_PATH=\"$(CURDIR)/packages/zeus:$(CURDIR)/packages/http:$(CURDIR)/packages/maya:$(CURDIR)/packages/raygui\"

COMPILER_DIR := packages/loam
SRCDIR  := $(COMPILER_DIR)/src
TESTDIR := $(COMPILER_DIR)/tests
OBJDIR  := obj
BINDIR  := bin

ALL_C   := $(wildcard $(SRCDIR)/*.c) $(wildcard $(SRCDIR)/sema/*.c)
LIB_C   := $(filter-out $(SRCDIR)/driver.c $(SRCDIR)/lsp.c $(SRCDIR)/fmt.c,$(ALL_C))
LIB_O   := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(LIB_C))
LOAM_O := $(LIB_O) $(OBJDIR)/driver.o
LSP_O   := $(LIB_O) $(OBJDIR)/lsp.o

TARGET  := $(BINDIR)/loamc
LSP     := $(BINDIR)/loam-lsp
FMT     := $(BINDIR)/loam-fmt
ZELI    := $(BINDIR)/zeli

PASS    := $(sort $(wildcard $(TESTDIR)/compile_pass/*.loam))
FAIL    := $(sort $(wildcard $(TESTDIR)/compile_fail/*.loam))
# Headless DRAW-list goldens: a fixture's stdout must match its .txt byte
# for byte (deterministic default metrics; see zeus_plat.c measure_default).
GOLDRAW := $(sort $(wildcard $(TESTDIR)/draw_golden/*.loam))
# Phase 12: files whose `#[test]` fns `loam test` collects and runs.
INLANG  := $(sort $(wildcard $(TESTDIR)/inlang/*.loam))
# In-language tests that sit beside the module they cover.
ZTESTS  := $(sort $(wildcard packages/zeus/cli/*_tests.loam))
GOLDEN  := $(TESTDIR)/golden
LANGEX  := examples/language
ZEUSEX  := examples/zeus
EXBUILD := $(TESTDIR)/tmp/build
# An app's entry point is named after its directory; every other .loam beside
# it is a module that app imports, not a program to link. Full-stack examples
# (e.g. examples/zeus/counter) have no such file and are excluded on purpose.
ZEUSAPPS := $(foreach d,$(wildcard $(ZEUSEX)/*),$(wildcard $(d)/$(notdir $(d)).loam))
EXAMPLES:= $(sort $(filter-out $(LANGEX)/oob.loam,\
             $(wildcard $(GOLDEN)/*.loam) $(wildcard $(LANGEX)/*.loam) $(ZEUSAPPS)))
# `examples/language/raygui.loam` links raylib (and the vendored raygui header).
# Keep it out of the default test set when raylib is not installed so `make test`
# stays dependency-light; `./run.sh raygui` still builds and runs it.
RAYGUI_EX := $(LANGEX)/raygui.loam
HAVE_RAYLIB := $(shell pkg-config --exists raylib 2>/dev/null && echo 1)
ifeq ($(HAVE_RAYLIB),)
EXAMPLES := $(filter-out $(RAYGUI_EX),$(EXAMPLES))
endif

.PHONY: all clean test mkdirs lsp grammar grammar-check zed-grammar install-editor bench

all: mkdirs $(TARGET) $(LSP) $(FMT) $(ZELI)

lsp: mkdirs $(LSP)

grammar:
	cd packages/tooling/tree-sitter-loam && npx --yes tree-sitter-cli generate
	$(MAKE) grammar-check
	$(MAKE) zed-grammar

# The grammar is a second, hand-maintained parser and nothing in `make test`
# reads it, so it rots silently while loam moves on. Gate `make grammar` on it.
grammar-check:
	@bash packages/tooling/tree-sitter-loam/check.sh

# Zed clones this directory via file://, so it must be its own git repo with
# src/parser.c at the clone root. The nested .git is local-only (not committed).
zed-grammar:
	@cd packages/tooling/tree-sitter-loam && \
	  if [ ! -d .git ]; then git init; fi && \
	  git add -A && \
	  if git diff --cached --quiet && git rev-parse --verify HEAD >/dev/null 2>&1; then :; \
	  else git -c user.name=loam -c user.email=loam@local commit --quiet -m "loam grammar"; fi
	@rev=$$(git -C packages/tooling/tree-sitter-loam rev-parse HEAD); \
	  sed -i '' "s/^rev = \".*\"/rev = \"$$rev\"/" packages/tooling/editors/zed/extension.toml; \
	  echo "zed grammar rev $$rev"
	@# Pin the clone URL to this checkout too, not just the rev: a moved grammar
	@# directory otherwise leaves Zed cloning a path that no longer exists and
	@# failing with "failed to compile grammar 'loam'".
	@repo="file://$(CURDIR)/packages/tooling/tree-sitter-loam"; \
	  sed -i '' "s|^repository = \".*\"|repository = \"$$repo\"|" packages/tooling/editors/zed/extension.toml; \
	  echo "zed grammar repo $$repo"
	@rm -rf packages/tooling/editors/zed/grammars

install-editor:
	@python3 packages/tooling/editors/vscode/install.py

mkdirs:
	@mkdir -p $(OBJDIR) $(OBJDIR)/sema $(BINDIR) $(TESTDIR)/tmp $(EXBUILD)

$(TARGET): $(LOAM_O)
	$(CC) $(CFLAGS) $(LOAM_O) -o $@

$(LSP): $(LSP_O)
	$(CC) $(CFLAGS) $(LSP_O) -o $@

# Standalone: fmt.c has its own main and links nothing else.
$(FMT): $(SRCDIR)/fmt.c
	$(CC) $(CFLAGS) $< -o $@

# `bin/zeli` — the framework CLI, written in Loam and built by the compiler above
# it in this file. It is the only implementation there is: the TypeScript one it
# was ported from is gone, so nothing here needs Deno to run a CLI command.
$(ZELI): packages/zeus/cli/zeli.loam $(TARGET)
	./$(TARGET) packages/zeus/cli/zeli.loam -o $@

# CFLAGS in this file carry the build-time paths (LOAM_PATH, LOAM_*_DIR), so an
# edit to the Makefile must rebuild every object — `.d` files only track source
# includes, which is how a path change silently kept a stale LOAM_PATH.
$(OBJDIR)/%.o: $(SRCDIR)/%.c Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

-include $(LIB_O:.o=.d) $(OBJDIR)/driver.d $(OBJDIR)/lsp.d

clean:
	rm -rf $(OBJDIR) $(BINDIR) $(TESTDIR)/tmp $(COMPILER_DIR)/runtime/.obj

# Phase 10: arena size, layout time, and binary size with the 32-bit default
# vs --int64-compat. See packages/loam/tests/bench/.
bench: all
	@sh $(TESTDIR)/bench/run.sh

# The failure flag is a file, not a shell variable. A `#` comment inside the
# recipe below ends the logical command — a `\`-joined comment would instead
# swallow the code after it — so every stanza that follows a comment runs in a
# fresh shell. A variable set at the top would be gone by the time the last
# shell looked at it, and the suite would print FAIL lines and then report
# success. A path survives the split, and the sentinel is its own recipe line so
# that a later comment cannot swallow it either.
test: all
	@mkdir -p $(TESTDIR)/tmp $(EXBUILD)
	@export ZEUS_HEADLESS=1 MAYA_HEADLESS=1; \
	rm -f $(TESTDIR)/tmp/failed; \
	for f in $(PASS); do \
	  stem=$$(basename $$f .loam); \
	  if ! ./$(TARGET) $$f -o $(TESTDIR)/tmp/$$stem >$(TESTDIR)/tmp/$$stem.log 2>&1; then \
	    echo "FAIL compile $$f"; cat $(TESTDIR)/tmp/$$stem.log; echo 1 >$(TESTDIR)/tmp/failed; continue; \
	  fi; \
	  if ! $(TESTDIR)/tmp/$$stem >/dev/null; then \
	    echo "FAIL run $$f"; echo 1 >$(TESTDIR)/tmp/failed; continue; \
	  fi; \
	  echo "ok   $$f"; \
	done; \
	for f in $(FAIL); do \
	  stem=$$(basename $$f .loam); \
	  if ./$(TARGET) $$f -o $(TESTDIR)/tmp/$$stem >$(TESTDIR)/tmp/$$stem.log 2>&1; then \
	    echo "FAIL should-reject $$f"; echo 1 >$(TESTDIR)/tmp/failed; \
	  else \
	    echo "ok   $$f (rejected)"; \
	  fi; \
	done; \
	for f in $(EXAMPLES); do \
	  stem=$$(basename $$f .loam); \
	  if ! ./$(TARGET) $$f -o $(EXBUILD)/$$stem >$(TESTDIR)/tmp/ex_$$stem.log 2>&1; then \
	    echo "FAIL compile $$f"; cat $(TESTDIR)/tmp/ex_$$stem.log; echo 1 >$(TESTDIR)/tmp/failed; continue; \
	  fi; \
	  if ! $(EXBUILD)/$$stem >/dev/null; then \
	    echo "FAIL run $$f"; echo 1 >$(TESTDIR)/tmp/failed; continue; \
	  fi; \
	  echo "ok   $$f"; \
	done; \
	if ! ./$(TARGET) $(LANGEX)/oob.loam -o $(EXBUILD)/oob >$(TESTDIR)/tmp/ex_oob.log 2>&1; then \
	  echo "FAIL compile $(LANGEX)/oob.loam"; cat $(TESTDIR)/tmp/ex_oob.log; echo 1 >$(TESTDIR)/tmp/failed; \
	elif $(EXBUILD)/oob >$(TESTDIR)/tmp/ex_oob.out 2>$(TESTDIR)/tmp/ex_oob.err; then \
	  echo "FAIL should-trap $(LANGEX)/oob.loam"; echo 1 >$(TESTDIR)/tmp/failed; \
	elif ! grep -q "index out of bounds" $(TESTDIR)/tmp/ex_oob.err; then \
	  echo "FAIL trap message $(LANGEX)/oob.loam"; cat $(TESTDIR)/tmp/ex_oob.err; echo 1 >$(TESTDIR)/tmp/failed; \
	else \
	  echo "ok   $(LANGEX)/oob.loam (trapped)"; \
	fi; \
	for f in $(PASS) $(EXAMPLES); do \
	  stem=$$(basename $$f .loam); \
	  if ! ./$(TARGET) --emit-ir $$f -o $(TESTDIR)/tmp/ir_$$stem.ir >$(TESTDIR)/tmp/ir_$$stem.log 2>&1; then \
	    echo "FAIL ir $$f"; cat $(TESTDIR)/tmp/ir_$$stem.log; echo 1 >$(TESTDIR)/tmp/failed; continue; \
	  fi; \
	  if grep -q "PARTIAL" $(TESTDIR)/tmp/ir_$$stem.ir; then \
	    echo "warn ir $$f (partial lowering)"; \
	  fi; \
	done; \
	echo "ok   ir lowering + verify"; \
	for g in $(TESTDIR)/ir_golden/*.ir; do \
	  [ -e $$g ] || continue; \
	  stem=$$(basename $$g .ir); \
	  src=$(TESTDIR)/compile_pass/$$stem.loam; \
	  if [ ! -f $$src ]; then src=$(GOLDEN)/$$stem.loam; fi; \
	  if [ ! -f $$src ]; then echo "FAIL ir golden missing source $$stem"; echo 1 >$(TESTDIR)/tmp/failed; continue; fi; \
	  if ! ./$(TARGET) --emit-ir $$src -o $(TESTDIR)/tmp/golden_$$stem.ir >$(TESTDIR)/tmp/golden_$$stem.log 2>&1; then \
	    echo "FAIL ir golden emit $$stem"; cat $(TESTDIR)/tmp/golden_$$stem.log; echo 1 >$(TESTDIR)/tmp/failed; continue; \
	  fi; \
	  if ! diff -u $$g $(TESTDIR)/tmp/golden_$$stem.ir >$(TESTDIR)/tmp/golden_$$stem.diff; then \
	    echo "FAIL ir golden $$stem"; cat $(TESTDIR)/tmp/golden_$$stem.diff; echo 1 >$(TESTDIR)/tmp/failed; \
	  else \
	    echo "ok   ir golden $$stem"; \
	  fi; \
	done; \
	for g in $(GOLDRAW); do \
	  stem=$$(basename $$g .loam); \
	  if ! ./$(TARGET) $$g -o $(TESTDIR)/tmp/dg_$$stem >$(TESTDIR)/tmp/dg_$$stem.log 2>&1; then \
	    echo "FAIL compile $$g"; cat $(TESTDIR)/tmp/dg_$$stem.log; echo 1 >$(TESTDIR)/tmp/failed; continue; \
	  fi; \
	  if ! $(TESTDIR)/tmp/dg_$$stem >$(TESTDIR)/tmp/dg_$$stem.out 2>&1; then \
	    echo "FAIL run $$g"; echo 1 >$(TESTDIR)/tmp/failed; continue; \
	  fi; \
	  if ! diff -u $(TESTDIR)/draw_golden/$$stem.txt $(TESTDIR)/tmp/dg_$$stem.out >$(TESTDIR)/tmp/dg_$$stem.diff; then \
	    echo "FAIL draw golden $$stem"; cat $(TESTDIR)/tmp/dg_$$stem.diff; echo 1 >$(TESTDIR)/tmp/failed; \
	  else \
	    echo "ok   draw golden $$stem"; \
	  fi; \
	done; \
	# Command-line arguments: the emitted entry hands argv to the runtime, so
	# `sys.argc` / `sys.arg` / `sys.args` see exactly what the shell passed —
	# including an argument with a space in it, which must stay one argument.
	if ! ./$(TARGET) packages/loam/tests/argv/args.loam -o $(TESTDIR)/tmp/argv_args >$(TESTDIR)/tmp/argv_build.log 2>&1; then \
	  echo "FAIL argv (compile)"; cat $(TESTDIR)/tmp/argv_build.log; echo 1 >$(TESTDIR)/tmp/failed; \
	elif ! $(TESTDIR)/tmp/argv_args alpha "beta gamma" >$(TESTDIR)/tmp/argv.out 2>&1; then \
	  echo "FAIL argv (run)"; echo 1 >$(TESTDIR)/tmp/failed; \
	elif ! diff -u packages/loam/tests/argv/args.txt $(TESTDIR)/tmp/argv.out >$(TESTDIR)/tmp/argv.diff; then \
	  echo "FAIL argv (output)"; cat $(TESTDIR)/tmp/argv.diff; echo 1 >$(TESTDIR)/tmp/failed; \
	else \
	  echo "ok   sys args (argc / arg / args, spaces preserved)"; \
	fi; \
	# Filesystem queries: a scratch tree is created, listed (sorted, `.`/`..`
	# excluded), stat-ed, and removed again. The scratch path arrives as argv,
	# which is the first real use of `sys.arg(1)`.
	if ! ./$(TARGET) packages/loam/tests/fs/fs.loam -o $(TESTDIR)/tmp/fs_probe >$(TESTDIR)/tmp/fs_build.log 2>&1; then \
	  echo "FAIL fs (compile)"; cat $(TESTDIR)/tmp/fs_build.log; echo 1 >$(TESTDIR)/tmp/failed; \
	elif ! $(TESTDIR)/tmp/fs_probe $(TESTDIR)/tmp/fsscratch >$(TESTDIR)/tmp/fs.out 2>&1; then \
	  echo "FAIL fs (run)"; echo 1 >$(TESTDIR)/tmp/failed; \
	elif ! diff -u packages/loam/tests/fs/fs.txt $(TESTDIR)/tmp/fs.out >$(TESTDIR)/tmp/fs.diff; then \
	  echo "FAIL fs (output)"; cat $(TESTDIR)/tmp/fs.diff; echo 1 >$(TESTDIR)/tmp/failed; \
	else \
	  echo "ok   sys filesystem (exists / is_dir / list_dir / remove_path)"; \
	fi; \
	# The kernel watcher behind a change-driven reload. The probe drives itself — it
	# creates its own tree and makes its own edits — and each edit is chosen so that
	# only one of the two watches can see it: an in-place write is invisible to a
	# directory watch, and a new file is invisible to a file watch. Writes under
	# build/ and .zeus/ must wake nothing at all, because they sit inside a watched
	# root and an event there would make every rebuild start the next one.
	if ! ./$(TARGET) packages/loam/tests/watch/watch.loam -o $(TESTDIR)/tmp/watch_probe >$(TESTDIR)/tmp/watch_build.log 2>&1; then \
	  echo "FAIL sys watcher (compile)"; cat $(TESTDIR)/tmp/watch_build.log; echo 1 >$(TESTDIR)/tmp/failed; \
	else \
	  $(TESTDIR)/tmp/watch_probe $(TESTDIR)/tmp/watchscratch >$(TESTDIR)/tmp/watch.out 2>&1; \
	  wstat=$$?; \
	  if grep -q 'no kernel watcher on this host' $(TESTDIR)/tmp/watch.out; then \
	    echo "skip sys watcher (no kernel watcher on this host)"; \
	  elif [ $$wstat -ne 0 ]; then \
	    echo "FAIL sys watcher (exit $$wstat)"; cat $(TESTDIR)/tmp/watch.out; echo 1 >$(TESTDIR)/tmp/failed; \
	  elif ! grep -q '^  hit .*existing\.loam$$' $(TESTDIR)/tmp/watch.out; then \
	    echo "FAIL sys watcher (an in-place edit did not wake it)"; cat $(TESTDIR)/tmp/watch.out; echo 1 >$(TESTDIR)/tmp/failed; \
	  elif ! grep -q '^  hit .*new\.loam$$' $(TESTDIR)/tmp/watch.out; then \
	    echo "FAIL sys watcher (a new file did not wake it)"; cat $(TESTDIR)/tmp/watch.out; echo 1 >$(TESTDIR)/tmp/failed; \
	  elif ! grep -q '^  (none)$$' $(TESTDIR)/tmp/watch.out; then \
	    echo "FAIL sys watcher (a write outside the watch set woke it)"; cat $(TESTDIR)/tmp/watch.out; echo 1 >$(TESTDIR)/tmp/failed; \
	  elif grep -qE '/build/|/\.zeus/' $(TESTDIR)/tmp/watch.out; then \
	    echo "FAIL sys watcher (build output or the cache woke it)"; cat $(TESTDIR)/tmp/watch.out; echo 1 >$(TESTDIR)/tmp/failed; \
	  else \
	    echo "ok   sys watcher (in-place edit, new file, build output ignored)"; \
	  fi; \
	fi; \
	for f in $(INLANG); do \
	  stem=$$(basename $$f .loam); \
	  if ! ./$(TARGET) test $$f >$(TESTDIR)/tmp/inlang_$$stem.log 2>&1; then \
	    echo "FAIL in-language tests $$f"; cat $(TESTDIR)/tmp/inlang_$$stem.log; echo 1 >$(TESTDIR)/tmp/failed; \
	  else \
	    echo "ok   in-language $$f"; \
	  fi; \
	done; \
	# In-language tests that live beside what they test, so the coverage sits next to
	# the code it pins. They cover the pure pieces `zeli` commands are built from,
	# which an end-to-end gate cannot pin down on its own.
	for f in $(ZTESTS); do \
	  stem=$$(basename $$f .loam); \
	  if ! ./$(TARGET) test $$f >$(TESTDIR)/tmp/ztests_$$stem.log 2>&1; then \
	    echo "FAIL in-language $$f"; cat $(TESTDIR)/tmp/ztests_$$stem.log; echo 1 >$(TESTDIR)/tmp/failed; \
	  else \
	    echo "ok   in-language $$f"; \
	  fi; \
	done; \
	# The route table is regenerated from `routes/` and compared against the one
	# committed for the app, so the expectation is in the tree rather than in
	# another implementation. Then the app is compiled and run, which is the part
	# that only works if the generated table is what a router can actually build.
	if ! $(ZELI) routes packages/loam/tests/routes_app >$(TESTDIR)/tmp/routes_gen.log 2>&1; then \
	  echo "FAIL zeli routes generator"; cat $(TESTDIR)/tmp/routes_gen.log; echo 1 >$(TESTDIR)/tmp/failed; \
	elif ! git --no-optional-locks diff --exit-code -- packages/loam/tests/routes_app/app_routes.loam >$(TESTDIR)/tmp/routes_diff.log 2>&1; then \
	  echo "FAIL zeli routes (the generated table is not what is committed)"; cat $(TESTDIR)/tmp/routes_diff.log; echo 1 >$(TESTDIR)/tmp/failed; \
	elif ! ./$(TARGET) packages/loam/tests/routes_app/app.loam -o $(TESTDIR)/tmp/routes_app >$(TESTDIR)/tmp/routes_compile.log 2>&1; then \
	  echo "FAIL compile packages/loam/tests/routes_app/app.loam"; cat $(TESTDIR)/tmp/routes_compile.log; echo 1 >$(TESTDIR)/tmp/failed; \
	elif ! $(TESTDIR)/tmp/routes_app >$(TESTDIR)/tmp/routes_run.log 2>&1; then \
	  echo "FAIL run packages/loam/tests/routes_app/app.loam"; cat $(TESTDIR)/tmp/routes_run.log; echo 1 >$(TESTDIR)/tmp/failed; \
	else \
	  echo "ok   zeli routes (regenerates the committed table, and it runs)"; \
	fi; \
	if ! $(ZELI) build packages/loam/tests/routes_app --targets web,macos >$(TESTDIR)/tmp/zeus_build.log 2>&1; then \
	  echo "FAIL zeli build"; cat $(TESTDIR)/tmp/zeus_build.log; echo 1 >$(TESTDIR)/tmp/failed; \
	elif [ ! -f packages/loam/tests/routes_app/build/web/app.wasm ] || [ ! -f packages/loam/tests/routes_app/build/macos/app ]; then \
	  echo "FAIL zeli build artifacts"; cat $(TESTDIR)/tmp/zeus_build.log; echo 1 >$(TESTDIR)/tmp/failed; \
	else \
	  echo "ok   zeli build (web + macos artifacts)"; \
	fi; \
	if [ -f packages/loam/tests/routes_app/build/web/index.html ] && \
	   [ -f packages/loam/tests/routes_app/build/web/blog/index.html ] && \
	   [ -f packages/loam/tests/routes_app/build/web/blog/hello-world/index.html ] && \
	   grep -q "<title>Blog</title>" packages/loam/tests/routes_app/build/web/blog/index.html && \
	   grep -q "<title>hello-world</title>" packages/loam/tests/routes_app/build/web/blog/hello-world/index.html && \
	   grep -q "/blog/hello-world" packages/loam/tests/routes_app/build/web/sitemap.xml && \
	   grep -q "User-agent" packages/loam/tests/routes_app/build/web/robots.txt && \
	   [ "$$(grep -c '<canvas' packages/loam/tests/routes_app/build/web/blog/index.html)" = "1" ]; then \
	  echo "ok   zeus web shell (per-route head + sitemap + robots)"; \
	else \
	  echo "FAIL zeus web shell"; echo 1 >$(TESTDIR)/tmp/failed; \
	fi; \
	# `zeli pkg sync`: the vendored tree and the lock file it writes are the ones
	# committed under the fixture, so git itself is the expectation — the gate needs
	# no second implementation to compare against. Then the app that imports the
	# package is compiled and run, which is what `import "pkg:name"` has to resolve
	# to.
	if ! $(ZELI) pkg sync packages/loam/tests/pkg_app >$(TESTDIR)/tmp/pkg_sync.log 2>&1; then \
	  echo "FAIL zeli pkg sync"; cat $(TESTDIR)/tmp/pkg_sync.log; echo 1 >$(TESTDIR)/tmp/failed; \
	elif ! git --no-optional-locks diff --exit-code -- packages/loam/tests/pkg_app >$(TESTDIR)/tmp/pkg_diff.log 2>&1; then \
	  echo "FAIL zeli pkg sync (the vendored tree or the lock is not what is committed)"; cat $(TESTDIR)/tmp/pkg_diff.log; echo 1 >$(TESTDIR)/tmp/failed; \
	elif ! ./$(TARGET) packages/loam/tests/pkg_app/app.loam -o $(TESTDIR)/tmp/pkg_app >$(TESTDIR)/tmp/pkg_app.log 2>&1; then \
	  echo "FAIL compile pkg app"; cat $(TESTDIR)/tmp/pkg_app.log; echo 1 >$(TESTDIR)/tmp/failed; \
	elif ! $(TESTDIR)/tmp/pkg_app >$(TESTDIR)/tmp/pkg_run.log 2>&1; then \
	  echo "FAIL run pkg app"; cat $(TESTDIR)/tmp/pkg_run.log; echo 1 >$(TESTDIR)/tmp/failed; \
	else \
	  echo "ok   zeli pkg sync (vendors, locks, and the import resolves)"; \
	fi; \
	# `in.loam` must stay unformatted: it is the messy input the case is about, and
	# a `zeli fmt` over the repo would quietly make this check vacuous by rewriting
	# it. The last condition fails loudly if that has happened.
	if ./$(FMT) packages/loam/tests/fmt/in.loam >$(TESTDIR)/tmp/fmt.out 2>&1 && \
	   diff -u packages/loam/tests/fmt/want.loam $(TESTDIR)/tmp/fmt.out >/dev/null 2>&1 && \
	   ! cmp -s packages/loam/tests/fmt/in.loam $(TESTDIR)/tmp/fmt.out && \
	   ./$(FMT) packages/loam/tests/fmt/want.loam >$(TESTDIR)/tmp/fmt.idem 2>&1 && \
	   diff -u packages/loam/tests/fmt/want.loam $(TESTDIR)/tmp/fmt.idem >/dev/null 2>&1; then \
	  echo "ok   loam-fmt (canonical + idempotent)"; \
	else \
	  echo "FAIL loam-fmt"; diff -u packages/loam/tests/fmt/want.loam $(TESTDIR)/tmp/fmt.out; echo 1 >$(TESTDIR)/tmp/failed; \
	fi; \
	# `zeli fmt` over a tree: every `.loam` under it becomes canonical, and what a
	# build owns is left alone — the generated route table, anything under `build/`,
	# and a dotted directory, none of which is a source. The fixture goes two
	# directories deep and hides a file in each skipped place, so the walk rule is
	# exercised and not just the formatting.
	if ! rm -rf $(TESTDIR)/tmp/fmtapp || \
	   ! mkdir -p $(TESTDIR)/tmp/fmtapp/nested/deep $(TESTDIR)/tmp/fmtapp/build $(TESTDIR)/tmp/fmtapp/.hidden; then \
	  echo "FAIL zeli fmt (setup)"; echo 1 >$(TESTDIR)/tmp/failed; \
	else \
	  cp packages/loam/tests/fmt/in.loam $(TESTDIR)/tmp/fmtapp/ugly.loam; \
	  cp packages/loam/tests/fmt/in.loam $(TESTDIR)/tmp/fmtapp/nested/deep/inner.loam; \
	  printf '// Generated by `zeli routes`. Do not edit.\nfn x( ){ }\n' >$(TESTDIR)/tmp/fmtapp/app_routes.loam; \
	  printf 'fn y( ){ }\n' >$(TESTDIR)/tmp/fmtapp/build/skipped.loam; \
	  printf 'fn z( ){ }\n' >$(TESTDIR)/tmp/fmtapp/.hidden/dot.loam; \
	  $(ZELI) fmt $(TESTDIR)/tmp/fmtapp >$(TESTDIR)/tmp/fmt_cmd.log 2>&1; \
	  zstat=$$?; \
	  $(ZELI) fmt $(TESTDIR)/tmp/fmtapp >$(TESTDIR)/tmp/fmt_cmd2.log 2>&1; \
	  zstat2=$$?; \
	  if [ $$zstat -ne 0 ] || [ $$zstat2 -ne 0 ] || \
	     ! diff -u packages/loam/tests/fmt/want.loam $(TESTDIR)/tmp/fmtapp/ugly.loam >/dev/null 2>&1 || \
	     ! diff -u packages/loam/tests/fmt/want.loam $(TESTDIR)/tmp/fmtapp/nested/deep/inner.loam >/dev/null 2>&1; then \
	    echo "FAIL zeli fmt (not canonical, or it failed)"; cat $(TESTDIR)/tmp/fmt_cmd.log; diff -u packages/loam/tests/fmt/want.loam $(TESTDIR)/tmp/fmtapp/ugly.loam; echo 1 >$(TESTDIR)/tmp/failed; \
	  elif ! grep -q 'fn x( ){ }' $(TESTDIR)/tmp/fmtapp/app_routes.loam; then \
	    echo "FAIL zeli fmt (rewrote the generated route table)"; echo 1 >$(TESTDIR)/tmp/failed; \
	  elif ! grep -q 'fn y( ){ }' $(TESTDIR)/tmp/fmtapp/build/skipped.loam; then \
	    echo "FAIL zeli fmt (rewrote something under build/)"; echo 1 >$(TESTDIR)/tmp/failed; \
	  elif ! grep -q 'fn z( ){ }' $(TESTDIR)/tmp/fmtapp/.hidden/dot.loam; then \
	    echo "FAIL zeli fmt (rewrote something in a dotted directory)"; echo 1 >$(TESTDIR)/tmp/failed; \
	  elif ! grep -q '0 formatted' $(TESTDIR)/tmp/fmt_cmd2.log; then \
	    echo "FAIL zeli fmt (the second run is not a no-op)"; cat $(TESTDIR)/tmp/fmt_cmd2.log; echo 1 >$(TESTDIR)/tmp/failed; \
	  else \
	    echo "ok   zeli fmt (a tree: canonical, idempotent, generated and build output untouched)"; \
	  fi; \
	fi; \
	# The other app, whose tree is bigger: a group, a param, `meta` and `paths`.
	# Same expectation — the table committed for it is what regeneration has to
	# reproduce, byte for byte.
	if ! $(ZELI) routes examples/zeus/myapp >$(TESTDIR)/tmp/myapp_routes_gen.log 2>&1; then \
	  echo "FAIL zeli routes (myapp)"; cat $(TESTDIR)/tmp/myapp_routes_gen.log; echo 1 >$(TESTDIR)/tmp/failed; \
	elif ! git --no-optional-locks diff --exit-code -- examples/zeus/myapp/app_routes.loam >$(TESTDIR)/tmp/myapp_routes_diff.log 2>&1; then \
	  echo "FAIL zeli routes (myapp's table is not what is committed)"; cat $(TESTDIR)/tmp/myapp_routes_diff.log; echo 1 >$(TESTDIR)/tmp/failed; \
	else \
	  echo "ok   zeli routes (myapp: groups, params, meta and paths)"; \
	fi; \
	# `zeli new` against a frozen scaffold. The manifest carries every file's path,
	# size and bytes, so a changed template — or a changed substitution — is a diff
	# here. The name is dashed on purpose: the golden pins both where the app name
	# has to appear and that the literal `myapp` of the old templates is gone.
	# Then the scaffold is compiled, because the entry imports the generated route
	# table and a fresh app has to typecheck before any build touches it.
	if ! rm -rf $(TESTDIR)/tmp/newapp || ! mkdir -p $(TESTDIR)/tmp/newapp/empty; then \
	  echo "FAIL zeli new (setup)"; echo 1 >$(TESTDIR)/tmp/failed; \
	elif ! $(ZELI) new my-app $(TESTDIR)/tmp/newapp/empty >$(TESTDIR)/tmp/newapp.log 2>&1; then \
	  echo "FAIL zeli new"; cat $(TESTDIR)/tmp/newapp.log; echo 1 >$(TESTDIR)/tmp/failed; \
	elif ! (cd $(TESTDIR)/tmp/newapp/empty && find . -type f -print | sed -e 's|^\./||' | LC_ALL=C sort | while IFS= read -r f; do printf '=== %s %s\n' "$$f" "$$(wc -c < "$$f" | tr -d ' ')"; cat "$$f"; done) >$(TESTDIR)/tmp/newapp.manifest 2>&1; then \
	  echo "FAIL zeli new (manifest)"; cat $(TESTDIR)/tmp/newapp.manifest; echo 1 >$(TESTDIR)/tmp/failed; \
	elif ! diff -u packages/zeus/tests/scaffold.golden $(TESTDIR)/tmp/newapp.manifest; then \
	  echo "FAIL zeli new (the scaffold is not the frozen one)"; echo 1 >$(TESTDIR)/tmp/failed; \
	else \
	  $(ZELI) new my-app $(TESTDIR)/tmp/newapp/empty >$(TESTDIR)/tmp/newapp2.log 2>&1; \
	  nstat=$$?; \
	  if [ $$nstat -ne 2 ]; then \
	    echo "FAIL zeli new (a non-empty target must be refused)"; cat $(TESTDIR)/tmp/newapp2.log; echo 1 >$(TESTDIR)/tmp/failed; \
	  elif ! grep -q 'already exists and is not empty' $(TESTDIR)/tmp/newapp2.log; then \
	    echo "FAIL zeli new (the refusal does not say why)"; cat $(TESTDIR)/tmp/newapp2.log; echo 1 >$(TESTDIR)/tmp/failed; \
	  elif ! ./$(TARGET) $(TESTDIR)/tmp/newapp/empty/app.loam -o $(TESTDIR)/tmp/newapp/app >$(TESTDIR)/tmp/newapp_build.log 2>&1; then \
	    echo "FAIL zeli new (the scaffold does not compile)"; cat $(TESTDIR)/tmp/newapp_build.log; echo 1 >$(TESTDIR)/tmp/failed; \
	  else \
	    echo "ok   zeli new (matches the frozen scaffold, refuses a non-empty target, compiles)"; \
	  fi; \
	fi; \
	# `zeli build --base` over a copy of routes_app: the artifacts a browser and a
	# crawler are served, checked against what they have to say rather than against
	# another implementation. `--base` ends in a slash on purpose — it is the case
	# `strip_slash` exists for — and the dynamic route has to appear in the sitemap
	# at every URL its `paths()` names.
	if ! rm -rf $(TESTDIR)/tmp/buildapp; then \
	  echo "FAIL zeli build (setup)"; echo 1 >$(TESTDIR)/tmp/failed; \
	else \
	  mkdir -p $(TESTDIR)/tmp/buildapp/app; \
	  cp -R packages/loam/tests/routes_app/routes $(TESTDIR)/tmp/buildapp/app/routes; \
	  cp packages/loam/tests/routes_app/app.loam $(TESTDIR)/tmp/buildapp/app/app.loam; \
	  $(ZELI) build $(TESTDIR)/tmp/buildapp/app --targets web,macos --base https://example.com/ >$(TESTDIR)/tmp/buildapp.out 2>&1; \
	  bstat=$$?; \
	  W=$(TESTDIR)/tmp/buildapp/app/build/web; \
	  if [ $$bstat -ne 0 ] || \
	     [ ! -s $(TESTDIR)/tmp/buildapp/app/build/web/app.wasm ] || \
	     [ ! -x $(TESTDIR)/tmp/buildapp/app/build/macos/app ] || \
	     [ ! -f $$W/blog/index.html ] || \
	     ! grep -q 'rel="canonical" href="https://example.com/blog"' $$W/blog/index.html || \
	     ! grep -q 'https://example.com/blog/hello-world' $$W/sitemap.xml || \
	     ! grep -q 'Sitemap: https://example.com/sitemap.xml' $$W/robots.txt || \
	     [ "$$(grep -c '<canvas' $$W/blog/index.html)" != "1" ]; then \
	    echo "FAIL zeli build (--base, shells, sitemap, robots)"; cat $(TESTDIR)/tmp/buildapp.out; echo 1 >$(TESTDIR)/tmp/failed; \
	  else \
	    echo "ok   zeli build (--base: canonical URLs, sitemap, robots)"; \
	  fi; \
	fi; \
	# The exits a script around `zeli build` branches on: an unknown target is a
	# usage error, and an app that does not pass the compiler gate stops the build
	# before any target is attempted.
	if ! rm -rf $(TESTDIR)/tmp/buildbad; then \
	  echo "FAIL zeli build (failure setup)"; echo 1 >$(TESTDIR)/tmp/failed; \
	else \
	  mkdir -p $(TESTDIR)/tmp/buildbad/bad; \
	  printf 'fn main() {\n    let x: int = "no"\n}\n' >$(TESTDIR)/tmp/buildbad/bad/app.loam; \
	  $(ZELI) build $(TESTDIR)/tmp/buildbad/app --targets nope >$(TESTDIR)/tmp/buildbad/bad_target.out 2>&1; \
	  badstat=$$?; \
	  $(ZELI) build $(TESTDIR)/tmp/buildbad/bad --targets web >$(TESTDIR)/tmp/buildbad/type.out 2>&1; \
	  typestat=$$?; \
	  if [ $$badstat -ne 2 ] || [ $$typestat -ne 1 ] || \
	     ! grep -q "unknown target 'nope' (want web, macos, ios, android, server)" $(TESTDIR)/tmp/buildbad/bad_target.out || \
	     ! grep -q 'failed the compiler check' $(TESTDIR)/tmp/buildbad/type.out; then \
	    echo "FAIL zeli build (exit codes / messages)"; cat $(TESTDIR)/tmp/buildbad/bad_target.out; cat $(TESTDIR)/tmp/buildbad/type.out; echo 1 >$(TESTDIR)/tmp/failed; \
	  else \
	    echo "ok   zeli build (an unknown target and a type error exit as documented)"; \
	  fi; \
	fi; \
	# The partial-build branch: a target that fails while another succeeds. An
	# unwritable output directory is the only reliable way to reach it — it makes
	# `loamc` fail without making either CLI throw — and it is the branch a script
	# reads, because exit 0 means "something built". Only the first line and the
	# `built n/m` summary are compared: the compiler's error names a temporary
	# file, so the rest of the text is not reproducible. The `chmod` back runs
	# before the comparison, so a failure here cannot leave tmp unremovable.
	chmod -R u+w $(TESTDIR)/tmp/buildpart 2>/dev/null; \
	if ! rm -rf $(TESTDIR)/tmp/buildpart; then \
	  echo "FAIL zeli build (partial setup)"; echo 1 >$(TESTDIR)/tmp/failed; \
	else \
	  mkdir -p $(TESTDIR)/tmp/buildpart/build/web; \
	  cp packages/loam/tests/routes_app/app.loam $(TESTDIR)/tmp/buildpart/app.loam; \
	  cp -R packages/loam/tests/routes_app/routes $(TESTDIR)/tmp/buildpart/routes; \
	  chmod 500 $(TESTDIR)/tmp/buildpart/build/web; \
	  $(ZELI) build $(TESTDIR)/tmp/buildpart --targets web,macos >$(TESTDIR)/tmp/buildpart/part.out 2>&1; \
	  pstat=$$?; \
	  $(ZELI) build $(TESTDIR)/tmp/buildpart --targets web >$(TESTDIR)/tmp/buildpart/none.out 2>&1; \
	  nstat=$$?; \
	  chmod 700 $(TESTDIR)/tmp/buildpart/build/web; \
	  if [ $$pstat -ne 0 ] || [ $$nstat -ne 1 ] || \
	     ! grep -q '^zeli: web failed' $(TESTDIR)/tmp/buildpart/part.out || \
	     ! grep -q '^zeli: built 1/2 target(s)' $(TESTDIR)/tmp/buildpart/part.out || \
	     ! grep -q '^zeli: built 0/1 target(s)' $(TESTDIR)/tmp/buildpart/none.out; then \
	    echo "FAIL zeli build (a partial build must exit 0, a total failure 1)"; cat $(TESTDIR)/tmp/buildpart/part.out; cat $(TESTDIR)/tmp/buildpart/none.out; echo 1 >$(TESTDIR)/tmp/failed; \
	  else \
	    echo "ok   zeli build (one target fails, the rest still build)"; \
	  fi; \
	fi; \
	# `zeli serve --build-only`: the initial build, the regenerated route table and the
	# web shell in one command, with no server bound. The app is copied rather than
	# built in place, so the gate leaves no `build/` behind in a tracked tree.
	if ! rm -rf $(TESTDIR)/tmp/serveonly; then \
	  echo "FAIL zeli serve (setup)"; echo 1 >$(TESTDIR)/tmp/failed; \
	else \
	  mkdir -p $(TESTDIR)/tmp/serveonly/app; \
	  cp -R packages/loam/tests/routes_app/routes $(TESTDIR)/tmp/serveonly/app/routes; \
	  cp packages/loam/tests/routes_app/app.loam $(TESTDIR)/tmp/serveonly/app/app.loam; \
	  $(ZELI) serve $(TESTDIR)/tmp/serveonly/app --build-only >$(TESTDIR)/tmp/serveonly.out 2>&1; \
	  dstat=$$?; \
	  if [ $$dstat -ne 0 ] || \
	     [ ! -s $(TESTDIR)/tmp/serveonly/app/build/web/app.wasm ] || \
	     [ ! -f $(TESTDIR)/tmp/serveonly/app/build/web/index.html ] || \
	     [ ! -f $(TESTDIR)/tmp/serveonly/app/build/web/sitemap.xml ] || \
	     ! grep -q 'zeli: serve build ok (--build-only)$$' $(TESTDIR)/tmp/serveonly.out || \
	     ! grep -q 'zeli: routes -> app_routes.loam$$' $(TESTDIR)/tmp/serveonly.out; then \
	    echo "FAIL zeli serve (--build-only)"; cat $(TESTDIR)/tmp/serveonly.out; echo 1 >$(TESTDIR)/tmp/failed; \
	  else \
	    echo "ok   zeli serve --build-only (wasm + the web shell, no server bound)"; \
	  fi; \
	fi; \
	# `zeli serve --target macos` is the native half: a rebuild is "stop the process,
	# start the new one" rather than a page reload. The fixture is a plain Loam
	# program that stays up and appends its own pid to `$SERVE_LAUNCHES` per launch —
	# `serve` cannot tell it from a real app, and it opens no window. What has to
	# hold: the first launch is running, one edit starts exactly one replacement, the
	# process it replaced is gone, and the pids serve reported are the pids that ran.
	if ! rm -rf $(TESTDIR)/tmp/servenative; then \
	  echo "FAIL zeli serve native (setup)"; echo 1 >$(TESTDIR)/tmp/failed; \
	else \
	  mkdir -p $(TESTDIR)/tmp/servenative; \
	  rm -f $(TESTDIR)/tmp/serve_launches; \
	  cp packages/loam/tests/serve/loop.loam $(TESTDIR)/tmp/servenative/app.loam; \
	  SERVE_LAUNCHES=$(CURDIR)/$(TESTDIR)/tmp/serve_launches $(ZELI) serve $(TESTDIR)/tmp/servenative --target macos >$(TESTDIR)/tmp/serve_native.log 2>&1 & \
	  srvpid=$$!; \
	  i=0; \
	  while [ $$i -lt 300 ]; do \
	    if grep -q 'zeli: serve on macos (pid ' $(TESTDIR)/tmp/serve_native.log 2>/dev/null; then break; fi; \
	    if ! kill -0 $$srvpid 2>/dev/null; then break; fi; \
	    sleep 0.1; \
	    i=$$((i + 1)); \
	  done; \
	  if ! grep -q 'zeli: serve on macos (pid ' $(TESTDIR)/tmp/serve_native.log 2>/dev/null; then \
	    echo "FAIL zeli serve native (nothing came up)"; cat $(TESTDIR)/tmp/serve_native.log 2>/dev/null; echo 1 >$(TESTDIR)/tmp/failed; \
	    kill $$srvpid 2>/dev/null; \
	    wait $$srvpid 2>/dev/null; \
	  else \
	    first=$$(sed -n 's/.*zeli: serve on macos (pid \([0-9]*\)).*/\1/p' $(TESTDIR)/tmp/serve_native.log | head -1); \
	    printf '\n// touched by the serve gate\n' >>$(TESTDIR)/tmp/servenative/app.loam; \
	    j=0; \
	    while [ $$j -lt 400 ]; do \
	      if grep -q 'zeli: restarted (pid ' $(TESTDIR)/tmp/serve_native.log 2>/dev/null; then break; fi; \
	      sleep 0.1; \
	      j=$$((j + 1)); \
	    done; \
	    k=0; \
	    while [ $$k -lt 100 ]; do \
	      if [ "$$(wc -l <$(TESTDIR)/tmp/serve_launches 2>/dev/null)" -ge 2 ]; then break; fi; \
	      sleep 0.1; \
	      k=$$((k + 1)); \
	    done; \
	    second=$$(sed -n 's/.*zeli: restarted (pid \([0-9]*\)).*/\1/p' $(TESTDIR)/tmp/serve_native.log | tail -1); \
	    restarts=$$(grep -c 'zeli: restarted (pid ' $(TESTDIR)/tmp/serve_native.log 2>/dev/null); \
	    ran=$$(cat $(TESTDIR)/tmp/serve_launches 2>/dev/null | tr '\n' ' '); \
	    printed=$$(grep -c 'serve fixture up' $(TESTDIR)/tmp/servenative/build/macos/app.log 2>/dev/null); \
	    old_up=0; \
	    if [ -n "$$first" ] && kill -0 $$first 2>/dev/null; then old_up=1; fi; \
	    new_up=0; \
	    if [ -n "$$second" ] && kill -0 $$second 2>/dev/null; then new_up=1; fi; \
	    kill $$srvpid 2>/dev/null; \
	    wait $$srvpid 2>/dev/null; \
	    if [ -n "$$first" ]; then kill $$first 2>/dev/null; fi; \
	    if [ -n "$$second" ]; then kill $$second 2>/dev/null; fi; \
	    cat $(TESTDIR)/tmp/serve_native.log; \
	    echo "   launches ran=[$$ran] reported first=$$first second=$$second app-log-lines=$$printed"; \
	    if [ -z "$$first" ] || [ -z "$$second" ]; then \
	      echo "FAIL zeli serve native (a launch did not report a pid)"; echo 1 >$(TESTDIR)/tmp/failed; \
	    elif [ "$$restarts" -ne 1 ]; then \
	      echo "FAIL zeli serve native (one edit was $$restarts restarts - a self-triggered cascade?)"; echo 1 >$(TESTDIR)/tmp/failed; \
	    elif [ "$$old_up" -eq 1 ]; then \
	      echo "FAIL zeli serve native (the replaced process is still running)"; echo 1 >$(TESTDIR)/tmp/failed; \
	    elif [ "$$new_up" -ne 1 ]; then \
	      echo "FAIL zeli serve native (the replacement is not running)"; echo 1 >$(TESTDIR)/tmp/failed; \
	    elif [ "$$ran" != "$$first $$second " ]; then \
	      echo "FAIL zeli serve native (the processes that ran were [$ran], serve reported [$first $second])"; echo 1 >$(TESTDIR)/tmp/failed; \
	    elif [ "$$printed" -eq 0 ]; then \
	      echo "FAIL zeli serve native (the app's output never reached its log)"; echo 1 >$(TESTDIR)/tmp/failed; \
	    else \
	      echo "ok   zeli serve --target macos (one edit is one restart, old process gone)"; \
	    fi; \
	  fi; \
	fi; \
	# The serve watcher's policy — `zeli serve`'s own walk and filter, not a copy — on a
	# real tree. The probe makes its own edits, and each is chosen so only one kind
	# of watch can see it: the delete needs neither a file nor a directory event to
	# name it, only a re-walk of the registry.
	if ! ./$(TARGET) packages/zeus/cli/serve_watch_probe.loam -o $(TESTDIR)/tmp/serve_watch_probe >$(TESTDIR)/tmp/serve_watch_build.log 2>&1; then \
	  echo "FAIL serve watcher (compile)"; cat $(TESTDIR)/tmp/serve_watch_build.log; echo 1 >$(TESTDIR)/tmp/failed; \
	else \
	  $(TESTDIR)/tmp/serve_watch_probe $(TESTDIR)/tmp/servewatch >$(TESTDIR)/tmp/serve_watch.out 2>&1; \
	  swstat=$$?; \
	  if grep -q 'no kernel watcher on this host' $(TESTDIR)/tmp/serve_watch.out; then \
	    echo "skip serve watcher (no kernel watcher on this host)"; \
	  elif [ $$swstat -ne 0 ]; then \
	    echo "FAIL serve watcher (exit $$swstat)"; cat $(TESTDIR)/tmp/serve_watch.out; echo 1 >$(TESTDIR)/tmp/failed; \
	  elif ! grep -q '^  hit .*edited\.loam$$' $(TESTDIR)/tmp/serve_watch.out; then \
	    echo "FAIL serve watcher (an in-place edit did not wake it)"; cat $(TESTDIR)/tmp/serve_watch.out; echo 1 >$(TESTDIR)/tmp/failed; \
	  elif ! grep -q '^  hit .*created\.loam$$' $(TESTDIR)/tmp/serve_watch.out; then \
	    echo "FAIL serve watcher (a new file did not wake it)"; cat $(TESTDIR)/tmp/serve_watch.out; echo 1 >$(TESTDIR)/tmp/failed; \
	  elif ! grep -q '^  hit .*removed\.loam$$' $(TESTDIR)/tmp/serve_watch.out; then \
	    echo "FAIL serve watcher (a deleted file did not wake it)"; cat $(TESTDIR)/tmp/serve_watch.out; echo 1 >$(TESTDIR)/tmp/failed; \
	  elif ! grep -q '^  (none)$$' $(TESTDIR)/tmp/serve_watch.out; then \
	    echo "FAIL serve watcher (a write outside the watch set woke it)"; cat $(TESTDIR)/tmp/serve_watch.out; echo 1 >$(TESTDIR)/tmp/failed; \
	  elif grep -qE '/build/|/\.zeus/' $(TESTDIR)/tmp/serve_watch.out; then \
	    echo "FAIL serve watcher (build output or the cache woke it)"; cat $(TESTDIR)/tmp/serve_watch.out; echo 1 >$(TESTDIR)/tmp/failed; \
	  else \
	    echo "ok   serve watcher (in-place edit, new file, delete, build output ignored)"; \
	  fi; \
	fi; \
	# `zeli serve` over real sockets: a route, the live channel, a rebuild pushed to a
	# subscriber, and the gRPC-Web proxy against a stand-in backend and against one
	# that says nothing. The assertions are all in `serve_boot_probe.loam`; what has to
	# live here is the pair of servers, because a process that outlives the checks
	# cannot be started from inside them. A host that forbids binding a listener
	# says so in the server's own words, and the case is skipped rather than failed.
	if ! rm -rf $(TESTDIR)/tmp/serveboot || \
	   ! ./$(TARGET) packages/zeus/cli/serve_rpc_probe.loam -o $(TESTDIR)/tmp/serve_rpc_probe >$(TESTDIR)/tmp/serve_rpc_build.log 2>&1 || \
	   ! ./$(TARGET) packages/zeus/cli/serve_boot_probe.loam -o $(TESTDIR)/tmp/serve_boot_probe >$(TESTDIR)/tmp/serve_boot_build.log 2>&1; then \
	  echo "FAIL zeli serve boot (setup)"; cat $(TESTDIR)/tmp/serve_rpc_build.log 2>/dev/null; cat $(TESTDIR)/tmp/serve_boot_build.log 2>/dev/null; echo 1 >$(TESTDIR)/tmp/failed; \
	else \
	  mkdir -p $(TESTDIR)/tmp/serveboot/app; \
	  cp -R packages/loam/tests/routes_app/routes $(TESTDIR)/tmp/serveboot/app/routes; \
	  cp packages/loam/tests/routes_app/app.loam $(TESTDIR)/tmp/serveboot/app/app.loam; \
	  rm -f $(TESTDIR)/tmp/serve_boot_rpc.log $(TESTDIR)/tmp/serve_boot.log; \
	  $(TESTDIR)/tmp/serve_rpc_probe 5197 $(TESTDIR)/tmp/serve_boot_rpc.log >$(TESTDIR)/tmp/serve_boot_rpc.out 2>&1 & \
	  rpcpid=$$!; \
	  LOAM_RPC_ADDR=127.0.0.1:5197 $(ZELI) serve $(TESTDIR)/tmp/serveboot/app --port 5199 >$(TESTDIR)/tmp/serve_boot.log 2>&1 & \
	  srvpid=$$!; \
	  i=0; \
	  while [ $$i -lt 200 ]; do \
	    if grep -q 'zeli: serve on http' $(TESTDIR)/tmp/serve_boot.log 2>/dev/null; then break; fi; \
	    if ! kill -0 $$srvpid 2>/dev/null; then break; fi; \
	    sleep 0.1; \
	    i=$$((i + 1)); \
	  done; \
	  if ! kill -0 $$srvpid 2>/dev/null || ! grep -q 'zeli: serve on http' $(TESTDIR)/tmp/serve_boot.log 2>/dev/null; then \
	    kill $$rpcpid 2>/dev/null; \
	    wait $$rpcpid 2>/dev/null; \
	    if grep -q 'cannot bind' $(TESTDIR)/tmp/serve_boot.log 2>/dev/null; then \
	      echo "skip zeli serve boot (this host cannot bind a listening socket)"; \
	      sed -e 's/^/     /' $(TESTDIR)/tmp/serve_boot.log; \
	    else \
	      echo "FAIL zeli serve boot (the server never came up)"; cat $(TESTDIR)/tmp/serve_boot.log 2>/dev/null; cat $(TESTDIR)/tmp/serve_boot_rpc.out 2>/dev/null; echo 1 >$(TESTDIR)/tmp/failed; \
	    fi; \
	  else \
	    $(TESTDIR)/tmp/serve_boot_probe 5199 5197 $(TESTDIR)/tmp/serveboot/app $(TESTDIR)/tmp/serve_boot_rpc.log >$(TESTDIR)/tmp/serve_boot_probe.out 2>&1; \
	    dbstat=$$?; \
	    sleep 3; \
	    rebuilt=$$(grep -c 'zeli: rebuilt' $(TESTDIR)/tmp/serve_boot.log 2>/dev/null); \
	    kill $$srvpid 2>/dev/null; \
	    wait $$srvpid 2>/dev/null; \
	    kill $$rpcpid 2>/dev/null; \
	    wait $$rpcpid 2>/dev/null; \
	    cat $(TESTDIR)/tmp/serve_boot_probe.out; \
	    if [ $$dbstat -ne 0 ]; then \
	      echo "FAIL zeli serve boot"; sed -e 's/^/     /' $(TESTDIR)/tmp/serve_boot.log; echo 1 >$(TESTDIR)/tmp/failed; \
	    elif [ "$$rebuilt" -ne 1 ]; then \
	      echo "FAIL zeli serve boot (one edit was $$rebuilt rebuilds - a self-triggered cascade?)"; sed -e 's/^/     /' $(TESTDIR)/tmp/serve_boot.log; echo 1 >$(TESTDIR)/tmp/failed; \
	    else \
	      echo "ok   zeli serve boot (serves, rebuilds, pushes a frame, proxies)"; \
	    fi; \
	  fi; \
	fi; \
	# The pure pieces of `zeli serve` live in their own in-language tests now
	# (`serve_tests.loam`, above), so the TypeScript unit test that used to sit here
	# has nothing left to cover that the port does not: its request handler, source
	# filter and SSE frame are all asserted against the Loam implementation.
	if ./$(TARGET) --target=wasm32 packages/loam/tests/wasm_smoke/app.loam -o $(TESTDIR)/tmp/wasm_smoke.wasm >$(TESTDIR)/tmp/wasm_smoke_build.log 2>&1 && \
	   DENO_DIR=$(TESTDIR)/tmp/deno deno run --quiet --allow-read packages/zeus/hosts/web/wasm_smoke.ts $(TESTDIR)/tmp/wasm_smoke.wasm >$(TESTDIR)/tmp/wasm_smoke.log 2>&1; then \
	  echo "ok   wasm smoke (host entry points)"; \
	else \
	  echo "FAIL wasm smoke"; cat $(TESTDIR)/tmp/wasm_smoke.log 2>/dev/null; cat $(TESTDIR)/tmp/wasm_smoke_build.log; echo 1 >$(TESTDIR)/tmp/failed; \
	fi; \
	if $(ZELI) build examples/zeus/myapp >$(TESTDIR)/tmp/myapp_build.log 2>&1 && \
	   [ -f examples/zeus/myapp/build/macos/app ] && \
	   grep -q "<title>Blog</title>" examples/zeus/myapp/build/web/blog/index.html && \
	   grep -q "/blog/hello-world" examples/zeus/myapp/build/web/sitemap.xml; then \
	  echo "ok   zeus example myapp (barebone builds all targets)"; \
	else \
	  echo "FAIL zeus example myapp"; cat $(TESTDIR)/tmp/myapp_build.log 2>/dev/null; echo 1 >$(TESTDIR)/tmp/failed; \
	fi; \
	if DENO_DIR=$(TESTDIR)/tmp/deno deno run --quiet --allow-read packages/zeus/hosts/web/wasm_smoke.ts examples/zeus/myapp/build/web/app.wasm >$(TESTDIR)/tmp/myapp_wasm.log 2>&1; then \
	  echo "ok   zeus example myapp (wasm runs)"; \
	else \
	  echo "FAIL zeus example myapp wasm"; cat $(TESTDIR)/tmp/myapp_wasm.log 2>/dev/null; echo 1 >$(TESTDIR)/tmp/failed; \
	fi; \
	if ./$(TARGET) examples/zeus/myapp/tests/routes.loam -o $(TESTDIR)/tmp/myapp_routes >$(TESTDIR)/tmp/myapp_routes.log 2>&1 && \
	   $(TESTDIR)/tmp/myapp_routes >>$(TESTDIR)/tmp/myapp_routes.log 2>&1; then \
	  echo "ok   zeus example myapp (every route paints)"; \
	else \
	  echo "FAIL zeus example myapp routes"; cat $(TESTDIR)/tmp/myapp_routes.log 2>/dev/null; echo 1 >$(TESTDIR)/tmp/failed; \
	fi; \
	if LOAM_SERVER_SPLIT=1 ./$(TARGET) --emit-c packages/loam/tests/compile_pass/server_split.loam -o $(TESTDIR)/tmp/server_split.c >/dev/null 2>&1; then \
	  if grep -q "SERVER_SECRET_MARKER" $(TESTDIR)/tmp/server_split.c; then \
	    echo "FAIL server body leaked into client build"; echo 1 >$(TESTDIR)/tmp/failed; \
	  else \
	    echo "ok   server split (body excluded)"; \
	  fi; \
	else \
	  echo "FAIL server split emit"; echo 1 >$(TESTDIR)/tmp/failed; \
	fi; \
	if ! python3 $(TESTDIR)/lsp_smoke.py; then echo 1 >$(TESTDIR)/tmp/failed; fi
	@if [ -e $(TESTDIR)/tmp/failed ]; then echo "TESTS FAILED"; exit 1; fi
	@echo "ALL TESTS PASSED"
