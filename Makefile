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
YUGAC_O := $(LIB_O) $(OBJDIR)/driver.o
LSP_O   := $(LIB_O) $(OBJDIR)/lsp.o

TARGET  := $(BINDIR)/yugac
LSP     := $(BINDIR)/yuga-lsp
FMT     := $(BINDIR)/yugafmt
ZEUS    := $(BINDIR)/zeus

PASS    := $(sort $(wildcard $(TESTDIR)/compile_pass/*.loam))
FAIL    := $(sort $(wildcard $(TESTDIR)/compile_fail/*.loam))
# Headless DRAW-list goldens: a fixture's stdout must match its .txt byte
# for byte (deterministic default metrics; see zeus_plat.c measure_default).
GOLDRAW := $(sort $(wildcard $(TESTDIR)/draw_golden/*.loam))
# Phase 12: files whose `#[test]` fns `yugac test` collects and runs.
INLANG  := $(sort $(wildcard $(TESTDIR)/inlang/*.loam))
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

all: mkdirs $(TARGET) $(LSP) $(FMT) $(ZEUS)

lsp: mkdirs $(LSP)

grammar:
	cd packages/tooling/tree-sitter-yuga && npx --yes tree-sitter-cli generate
	$(MAKE) grammar-check
	$(MAKE) zed-grammar

# The grammar is a second, hand-maintained parser and nothing in `make test`
# reads it, so it rots silently while yugac moves on. Gate `make grammar` on it.
grammar-check:
	@bash packages/tooling/tree-sitter-yuga/check.sh

# Zed clones this directory via file://, so it must be its own git repo with
# src/parser.c at the clone root. The nested .git is local-only (not committed).
zed-grammar:
	@cd packages/tooling/tree-sitter-yuga && \
	  if [ ! -d .git ]; then git init; fi && \
	  git add -A && \
	  if git diff --cached --quiet && git rev-parse --verify HEAD >/dev/null 2>&1; then :; \
	  else git -c user.name=yuga -c user.email=yuga@local commit --quiet -m "yuga grammar"; fi
	@rev=$$(git -C packages/tooling/tree-sitter-yuga rev-parse HEAD); \
	  sed -i '' "s/^rev = \".*\"/rev = \"$$rev\"/" packages/tooling/editors/zed/extension.toml; \
	  echo "zed grammar rev $$rev"
	@# Pin the clone URL to this checkout too, not just the rev: a moved grammar
	@# directory otherwise leaves Zed cloning a path that no longer exists and
	@# failing with "failed to compile grammar 'yuga'".
	@repo="file://$(CURDIR)/packages/tooling/tree-sitter-yuga"; \
	  sed -i '' "s|^repository = \".*\"|repository = \"$$repo\"|" packages/tooling/editors/zed/extension.toml; \
	  echo "zed grammar repo $$repo"
	@rm -rf packages/tooling/editors/zed/grammars

install-editor:
	@python3 packages/tooling/editors/vscode/install.py

mkdirs:
	@mkdir -p $(OBJDIR) $(OBJDIR)/sema $(BINDIR) $(TESTDIR)/tmp $(EXBUILD)

$(TARGET): $(YUGAC_O)
	$(CC) $(CFLAGS) $(YUGAC_O) -o $@

$(LSP): $(LSP_O)
	$(CC) $(CFLAGS) $(LSP_O) -o $@

# Standalone: fmt.c has its own main and links nothing else.
$(FMT): $(SRCDIR)/fmt.c
	$(CC) $(CFLAGS) $< -o $@

# `bin/zeus` — run the framework CLI without spelling out the deno invocation.
$(ZEUS): packages/zeus/cli/zeus.ts
	@printf '#!/bin/sh\nexec deno run --quiet --allow-read --allow-write --allow-run --allow-env --allow-net "%s/packages/zeus/cli/zeus.ts" "$$@"\n' "$(CURDIR)" > $@
	@chmod +x $@

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

test: all
	@mkdir -p $(TESTDIR)/tmp $(EXBUILD)
	@export ZEUS_HEADLESS=1 MAYA_HEADLESS=1; \
	err=0; \
	for f in $(PASS); do \
	  stem=$$(basename $$f .loam); \
	  if ! ./$(TARGET) $$f -o $(TESTDIR)/tmp/$$stem >$(TESTDIR)/tmp/$$stem.log 2>&1; then \
	    echo "FAIL compile $$f"; cat $(TESTDIR)/tmp/$$stem.log; err=1; continue; \
	  fi; \
	  if ! $(TESTDIR)/tmp/$$stem >/dev/null; then \
	    echo "FAIL run $$f"; err=1; continue; \
	  fi; \
	  echo "ok   $$f"; \
	done; \
	for f in $(FAIL); do \
	  stem=$$(basename $$f .loam); \
	  if ./$(TARGET) $$f -o $(TESTDIR)/tmp/$$stem >$(TESTDIR)/tmp/$$stem.log 2>&1; then \
	    echo "FAIL should-reject $$f"; err=1; \
	  else \
	    echo "ok   $$f (rejected)"; \
	  fi; \
	done; \
	for f in $(EXAMPLES); do \
	  stem=$$(basename $$f .loam); \
	  if ! ./$(TARGET) $$f -o $(EXBUILD)/$$stem >$(TESTDIR)/tmp/ex_$$stem.log 2>&1; then \
	    echo "FAIL compile $$f"; cat $(TESTDIR)/tmp/ex_$$stem.log; err=1; continue; \
	  fi; \
	  if ! $(EXBUILD)/$$stem >/dev/null; then \
	    echo "FAIL run $$f"; err=1; continue; \
	  fi; \
	  echo "ok   $$f"; \
	done; \
	if ! ./$(TARGET) $(LANGEX)/oob.loam -o $(EXBUILD)/oob >$(TESTDIR)/tmp/ex_oob.log 2>&1; then \
	  echo "FAIL compile $(LANGEX)/oob.loam"; cat $(TESTDIR)/tmp/ex_oob.log; err=1; \
	elif $(EXBUILD)/oob >$(TESTDIR)/tmp/ex_oob.out 2>$(TESTDIR)/tmp/ex_oob.err; then \
	  echo "FAIL should-trap $(LANGEX)/oob.loam"; err=1; \
	elif ! grep -q "index out of bounds" $(TESTDIR)/tmp/ex_oob.err; then \
	  echo "FAIL trap message $(LANGEX)/oob.loam"; cat $(TESTDIR)/tmp/ex_oob.err; err=1; \
	else \
	  echo "ok   $(LANGEX)/oob.loam (trapped)"; \
	fi; \
	for f in $(PASS) $(EXAMPLES); do \
	  stem=$$(basename $$f .loam); \
	  if ! ./$(TARGET) --emit-ir $$f -o $(TESTDIR)/tmp/ir_$$stem.ir >$(TESTDIR)/tmp/ir_$$stem.log 2>&1; then \
	    echo "FAIL ir $$f"; cat $(TESTDIR)/tmp/ir_$$stem.log; err=1; continue; \
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
	  if [ ! -f $$src ]; then echo "FAIL ir golden missing source $$stem"; err=1; continue; fi; \
	  if ! ./$(TARGET) --emit-ir $$src -o $(TESTDIR)/tmp/golden_$$stem.ir >$(TESTDIR)/tmp/golden_$$stem.log 2>&1; then \
	    echo "FAIL ir golden emit $$stem"; cat $(TESTDIR)/tmp/golden_$$stem.log; err=1; continue; \
	  fi; \
	  if ! diff -u $$g $(TESTDIR)/tmp/golden_$$stem.ir >$(TESTDIR)/tmp/golden_$$stem.diff; then \
	    echo "FAIL ir golden $$stem"; cat $(TESTDIR)/tmp/golden_$$stem.diff; err=1; \
	  else \
	    echo "ok   ir golden $$stem"; \
	  fi; \
	done; \
	for g in $(GOLDRAW); do \
	  stem=$$(basename $$g .loam); \
	  if ! ./$(TARGET) $$g -o $(TESTDIR)/tmp/dg_$$stem >$(TESTDIR)/tmp/dg_$$stem.log 2>&1; then \
	    echo "FAIL compile $$g"; cat $(TESTDIR)/tmp/dg_$$stem.log; err=1; continue; \
	  fi; \
	  if ! $(TESTDIR)/tmp/dg_$$stem >$(TESTDIR)/tmp/dg_$$stem.out 2>&1; then \
	    echo "FAIL run $$g"; err=1; continue; \
	  fi; \
	  if ! diff -u $(TESTDIR)/draw_golden/$$stem.txt $(TESTDIR)/tmp/dg_$$stem.out >$(TESTDIR)/tmp/dg_$$stem.diff; then \
	    echo "FAIL draw golden $$stem"; cat $(TESTDIR)/tmp/dg_$$stem.diff; err=1; \
	  else \
	    echo "ok   draw golden $$stem"; \
	  fi; \
	done; \
	for f in $(INLANG); do \
	  stem=$$(basename $$f .loam); \
	  if ! ./$(TARGET) test $$f >$(TESTDIR)/tmp/inlang_$$stem.log 2>&1; then \
	    echo "FAIL in-language tests $$f"; cat $(TESTDIR)/tmp/inlang_$$stem.log; err=1; \
	  else \
	    echo "ok   in-language $$f"; \
	  fi; \
	done; \
	if ! DENO_DIR=$(TESTDIR)/tmp/deno deno run --quiet --allow-read --allow-write packages/zeus/cli/zeus.ts routes packages/loam/tests/routes_app >$(TESTDIR)/tmp/routes_gen.log 2>&1; then \
	  echo "FAIL zeus routes generator"; cat $(TESTDIR)/tmp/routes_gen.log; err=1; \
	elif ! ./$(TARGET) packages/loam/tests/routes_app/app.loam -o $(TESTDIR)/tmp/routes_app >$(TESTDIR)/tmp/routes_compile.log 2>&1; then \
	  echo "FAIL compile packages/loam/tests/routes_app/app.loam"; cat $(TESTDIR)/tmp/routes_compile.log; err=1; \
	elif ! $(TESTDIR)/tmp/routes_app >$(TESTDIR)/tmp/routes_run.log 2>&1; then \
	  echo "FAIL run packages/loam/tests/routes_app/app.loam"; cat $(TESTDIR)/tmp/routes_run.log; err=1; \
	else \
	  echo "ok   zeus routes app"; \
	fi; \
	if ! DENO_DIR=$(TESTDIR)/tmp/deno deno run --quiet --allow-read --allow-write --allow-run --allow-env packages/zeus/cli/zeus.ts build packages/loam/tests/routes_app --targets web,macos >$(TESTDIR)/tmp/zeus_build.log 2>&1; then \
	  echo "FAIL zeus build"; cat $(TESTDIR)/tmp/zeus_build.log; err=1; \
	elif [ ! -f packages/loam/tests/routes_app/build/web/app.wasm ] || [ ! -f packages/loam/tests/routes_app/build/macos/app ]; then \
	  echo "FAIL zeus build artifacts"; cat $(TESTDIR)/tmp/zeus_build.log; err=1; \
	else \
	  echo "ok   zeus build (web + macos artifacts)"; \
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
	  echo "FAIL zeus web shell"; err=1; \
	fi; \
	if ! DENO_DIR=$(TESTDIR)/tmp/deno deno run --quiet --allow-read --allow-write --allow-run --allow-env packages/zeus/cli/zeus.ts pkg sync packages/loam/tests/pkg_app >$(TESTDIR)/tmp/pkg_sync.log 2>&1; then \
	  echo "FAIL zeus pkg sync"; cat $(TESTDIR)/tmp/pkg_sync.log; err=1; \
	elif ! ./$(TARGET) packages/loam/tests/pkg_app/app.loam -o $(TESTDIR)/tmp/pkg_app >$(TESTDIR)/tmp/pkg_app.log 2>&1; then \
	  echo "FAIL compile pkg app"; cat $(TESTDIR)/tmp/pkg_app.log; err=1; \
	elif ! $(TESTDIR)/tmp/pkg_app >$(TESTDIR)/tmp/pkg_run.log 2>&1; then \
	  echo "FAIL run pkg app"; cat $(TESTDIR)/tmp/pkg_run.log; err=1; \
	else \
	  echo "ok   zeus pkg sync + import pkg:name"; \
	fi; \
	if ./$(FMT) packages/loam/tests/fmt/in.loam >$(TESTDIR)/tmp/fmt.out 2>&1 && \
	   diff -u packages/loam/tests/fmt/want.loam $(TESTDIR)/tmp/fmt.out >/dev/null 2>&1 && \
	   ./$(FMT) packages/loam/tests/fmt/want.loam >$(TESTDIR)/tmp/fmt.idem 2>&1 && \
	   diff -u packages/loam/tests/fmt/want.loam $(TESTDIR)/tmp/fmt.idem >/dev/null 2>&1; then \
	  echo "ok   yugafmt (canonical + idempotent)"; \
	else \
	  echo "FAIL yugafmt"; diff -u packages/loam/tests/fmt/want.loam $(TESTDIR)/tmp/fmt.out; err=1; \
	fi; \
	if ! DENO_DIR=$(TESTDIR)/tmp/deno deno run --quiet --allow-read --allow-write --allow-run --allow-env packages/zeus/cli/zeus.ts dev packages/loam/tests/routes_app --build-only >$(TESTDIR)/tmp/zeus_dev.log 2>&1; then \
	  echo "FAIL zeus dev"; cat $(TESTDIR)/tmp/zeus_dev.log; err=1; \
	else \
	  echo "ok   zeus dev --build-only"; \
	fi; \
	if ! DENO_DIR=$(TESTDIR)/tmp/deno deno run --quiet --allow-read --allow-write packages/zeus/cli/dev_test.ts >$(TESTDIR)/tmp/dev_test.log 2>&1; then \
	  echo "FAIL zeus dev handler"; cat $(TESTDIR)/tmp/dev_test.log; err=1; \
	else \
	  echo "ok   zeus dev handler"; \
	fi; \
	if ./$(TARGET) --target=wasm32 packages/loam/tests/wasm_smoke/app.loam -o $(TESTDIR)/tmp/wasm_smoke.wasm >$(TESTDIR)/tmp/wasm_smoke_build.log 2>&1 && \
	   DENO_DIR=$(TESTDIR)/tmp/deno deno run --quiet --allow-read packages/zeus/hosts/web/wasm_smoke.ts $(TESTDIR)/tmp/wasm_smoke.wasm >$(TESTDIR)/tmp/wasm_smoke.log 2>&1; then \
	  echo "ok   wasm smoke (host entry points)"; \
	else \
	  echo "FAIL wasm smoke"; cat $(TESTDIR)/tmp/wasm_smoke.log 2>/dev/null; cat $(TESTDIR)/tmp/wasm_smoke_build.log; err=1; \
	fi; \
	if DENO_DIR=$(TESTDIR)/tmp/deno deno run --quiet --allow-read --allow-write --allow-run --allow-env packages/zeus/cli/zeus.ts build examples/zeus/myapp >$(TESTDIR)/tmp/myapp_build.log 2>&1 && \
	   [ -f examples/zeus/myapp/build/macos/app ] && \
	   grep -q "<title>Blog</title>" examples/zeus/myapp/build/web/blog/index.html && \
	   grep -q "/blog/hello-world" examples/zeus/myapp/build/web/sitemap.xml; then \
	  echo "ok   zeus example myapp (barebone builds all targets)"; \
	else \
	  echo "FAIL zeus example myapp"; cat $(TESTDIR)/tmp/myapp_build.log 2>/dev/null; err=1; \
	fi; \
	if DENO_DIR=$(TESTDIR)/tmp/deno deno run --quiet --allow-read packages/zeus/hosts/web/wasm_smoke.ts examples/zeus/myapp/build/web/app.wasm >$(TESTDIR)/tmp/myapp_wasm.log 2>&1; then \
	  echo "ok   zeus example myapp (wasm runs)"; \
	else \
	  echo "FAIL zeus example myapp wasm"; cat $(TESTDIR)/tmp/myapp_wasm.log 2>/dev/null; err=1; \
	fi; \
	if ./$(TARGET) examples/zeus/myapp/tests/routes.loam -o $(TESTDIR)/tmp/myapp_routes >$(TESTDIR)/tmp/myapp_routes.log 2>&1 && \
	   $(TESTDIR)/tmp/myapp_routes >>$(TESTDIR)/tmp/myapp_routes.log 2>&1; then \
	  echo "ok   zeus example myapp (every route paints)"; \
	else \
	  echo "FAIL zeus example myapp routes"; cat $(TESTDIR)/tmp/myapp_routes.log 2>/dev/null; err=1; \
	fi; \
	if LOAM_SERVER_SPLIT=1 ./$(TARGET) --emit-c packages/loam/tests/compile_pass/server_split.loam -o $(TESTDIR)/tmp/server_split.c >/dev/null 2>&1; then \
	  if grep -q "SERVER_SECRET_MARKER" $(TESTDIR)/tmp/server_split.c; then \
	    echo "FAIL server body leaked into client build"; err=1; \
	  else \
	    echo "ok   server split (body excluded)"; \
	  fi; \
	else \
	  echo "FAIL server split emit"; err=1; \
	fi; \
	if ! python3 $(TESTDIR)/lsp_smoke.py; then err=1; fi; \
	if [ $$err -ne 0 ]; then echo "TESTS FAILED"; exit 1; fi; \
	echo "ALL TESTS PASSED"
