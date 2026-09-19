# Loam's build and test now live in `nob.loam`. This file is only the bootstrap
# `nob.loam` cannot be: `nob` is a Loam program, so something that is not Loam
# has to build the first `bin/loamc`. Everything after that is forwarded.
#
#   make            -> nob build   bin/loamc, bin/loam-lsp, bin/loam-fmt, bin/zeli
#   make test       -> nob test    language suites + host checks, -j4 by default
#   make test NOB_ARGS=-j8         change the parallel pool size
#   make lsp        -> nob lsp
#   make bench      -> nob bench
#
# The `zeli`-driven suite is not part of `test`: run `./bin/nob integration`
# (see nob.loam) for routes / build / pkg / fmt / serve.
# `make grammar` / `make install-editor` are developer tooling, not build steps,
# so they stay here.
#
# The flags below build the *seed* compiler in one shot. Keep them in sync with
# `cflags()` in nob.loam, which stamps the same value into `obj/.cflags` and owns
# the incremental object build from then on.

CC      := cc
CFLAGS  := -std=c11 -Wall -Wextra -Wpedantic -O2 -g
CFLAGS  += -DLOAM_RT_PATH='"$(CURDIR)/packages/loam/runtime/loam_rt.h"'
CFLAGS  += -DLOAM_RUNTIME_DIR='"$(CURDIR)/packages/loam/runtime"'
CFLAGS  += -DLOAM_STD_DIR='"$(CURDIR)/packages/loam/std"'
CFLAGS  += -DLOAM_ZEUS_DIR='"$(CURDIR)/packages/zeus"'
CFLAGS  += -DLOAM_RAYGUI_DIR='"$(CURDIR)/packages/raygui"'
CFLAGS  += -DLOAM_PATH='"$(CURDIR)/packages/zeus:$(CURDIR)/packages/zeus-components:$(CURDIR)/packages/http:$(CURDIR)/packages/maya:$(CURDIR)/packages/raygui"'

# driver.c is loamc's main; lsp.c and fmt.c carry their own and are built apart.
LOAMC_SRC := $(filter-out packages/loam/src/lsp.c packages/loam/src/fmt.c,\
             $(wildcard packages/loam/src/*.c) $(wildcard packages/loam/src/sema/*.c))

NOB := bin/nob

.PHONY: all test lsp bench clean grammar grammar-check zed-grammar install-editor

all: $(NOB)
	@./$(NOB) build

test: $(NOB)
	@./$(NOB) test $(NOB_ARGS)

lsp: $(NOB)
	@./$(NOB) lsp

bench: $(NOB)
	@./$(NOB) bench

$(NOB): nob.loam bin/loamc
	@mkdir -p bin
	./bin/loamc nob.loam -o $@

bin/loamc: $(LOAMC_SRC)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(LOAMC_SRC) -o $@

clean:
	rm -rf obj bin packages/loam/tests/tmp packages/loam/runtime/.obj

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
