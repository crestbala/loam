# Yuga compiler — C11, libc only. Generated programs are gnu99 (C99 + statement exprs).

CC      := cc
CFLAGS  := -std=c11 -Wall -Wextra -Wpedantic -O2 -g -MMD -MP
CFLAGS  += -DYUGA_RT_PATH=\"$(CURDIR)/packages/compiler/runtime/yuga_rt.h\"
CFLAGS  += -DYUGA_RUNTIME_DIR=\"$(CURDIR)/packages/compiler/runtime\"
CFLAGS  += -DYUGA_STD_DIR=\"$(CURDIR)/packages/compiler/std\"
CFLAGS  += -DYUGA_ZEUS_DIR=\"$(CURDIR)/packages/zeus\"

COMPILER_DIR := packages/compiler
SRCDIR  := $(COMPILER_DIR)/src
TESTDIR := $(COMPILER_DIR)/tests
OBJDIR  := obj
BINDIR  := bin

ALL_C   := $(wildcard $(SRCDIR)/*.c) $(wildcard $(SRCDIR)/sema/*.c)
LIB_C   := $(filter-out $(SRCDIR)/driver.c $(SRCDIR)/lsp.c,$(ALL_C))
LIB_O   := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(LIB_C))
YUGAC_O := $(LIB_O) $(OBJDIR)/driver.o
LSP_O   := $(LIB_O) $(OBJDIR)/lsp.o

TARGET  := $(BINDIR)/yugac
LSP     := $(BINDIR)/yuga-lsp

PASS    := $(sort $(wildcard $(TESTDIR)/compile_pass/*.yuga))
FAIL    := $(sort $(wildcard $(TESTDIR)/compile_fail/*.yuga))
# Headless DRAW-list goldens: a fixture's stdout must match its .txt byte
# for byte (deterministic default metrics; see zeus_plat.c measure_default).
GOLDRAW := $(sort $(wildcard $(TESTDIR)/draw_golden/*.yuga))
GOLDEN  := $(TESTDIR)/golden
LANGEX  := examples/language
ZEUSEX  := examples/zeus
EXBUILD := $(TESTDIR)/tmp/build
# An app's entry point is named after its directory; every other .yuga beside
# it is a module that app imports, not a program to link. Full-stack examples
# (e.g. examples/zeus/counter) have no such file and are excluded on purpose.
ZEUSAPPS := $(foreach d,$(wildcard $(ZEUSEX)/*),$(wildcard $(d)/$(notdir $(d)).yuga))
EXAMPLES:= $(sort $(filter-out $(LANGEX)/oob.yuga,\
             $(wildcard $(GOLDEN)/*.yuga) $(wildcard $(LANGEX)/*.yuga) $(ZEUSAPPS)))

.PHONY: all clean test mkdirs lsp grammar zed-grammar install-editor bench

all: mkdirs $(TARGET) $(LSP)

lsp: mkdirs $(LSP)

grammar:
	cd packages/tree-sitter-yuga && npx --yes tree-sitter-cli generate
	$(MAKE) zed-grammar

# Zed clones this directory via file://, so it must be its own git repo with
# src/parser.c at the clone root. The nested .git is local-only (not committed).
zed-grammar:
	@cd packages/tree-sitter-yuga && \
	  if [ ! -d .git ]; then git init; fi && \
	  git add -A && \
	  if git diff --cached --quiet && git rev-parse --verify HEAD >/dev/null 2>&1; then :; \
	  else git -c user.name=yuga -c user.email=yuga@local commit --quiet -m "yuga grammar"; fi
	@rev=$$(git -C packages/tree-sitter-yuga rev-parse HEAD); \
	  sed -i '' "s/^rev = \".*\"/rev = \"$$rev\"/" packages/editors/zed/extension.toml; \
	  echo "zed grammar rev $$rev"
	@rm -rf packages/editors/zed/grammars

install-editor:
	@python3 packages/editors/vscode/install.py

mkdirs:
	@mkdir -p $(OBJDIR) $(OBJDIR)/sema $(BINDIR) $(TESTDIR)/tmp $(EXBUILD)

$(TARGET): $(YUGAC_O)
	$(CC) $(CFLAGS) $(YUGAC_O) -o $@

$(LSP): $(LSP_O)
	$(CC) $(CFLAGS) $(LSP_O) -o $@

$(OBJDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

-include $(LIB_O:.o=.d) $(OBJDIR)/driver.d $(OBJDIR)/lsp.d

clean:
	rm -rf $(OBJDIR) $(BINDIR) $(TESTDIR)/tmp $(COMPILER_DIR)/runtime/.obj

# Phase 10: arena size, layout time, and binary size with the 32-bit default
# vs --int64-compat. See packages/compiler/tests/bench/.
bench: all
	@sh $(TESTDIR)/bench/run.sh

test: all
	@mkdir -p $(TESTDIR)/tmp $(EXBUILD)
	@export ZEUS_HEADLESS=1 MAYA_HEADLESS=1; \
	err=0; \
	for f in $(PASS); do \
	  stem=$$(basename $$f .yuga); \
	  if ! ./$(TARGET) $$f -o $(TESTDIR)/tmp/$$stem >$(TESTDIR)/tmp/$$stem.log 2>&1; then \
	    echo "FAIL compile $$f"; cat $(TESTDIR)/tmp/$$stem.log; err=1; continue; \
	  fi; \
	  if ! $(TESTDIR)/tmp/$$stem >/dev/null; then \
	    echo "FAIL run $$f"; err=1; continue; \
	  fi; \
	  echo "ok   $$f"; \
	done; \
	for f in $(FAIL); do \
	  stem=$$(basename $$f .yuga); \
	  if ./$(TARGET) $$f -o $(TESTDIR)/tmp/$$stem >$(TESTDIR)/tmp/$$stem.log 2>&1; then \
	    echo "FAIL should-reject $$f"; err=1; \
	  else \
	    echo "ok   $$f (rejected)"; \
	  fi; \
	done; \
	for f in $(EXAMPLES); do \
	  stem=$$(basename $$f .yuga); \
	  if ! ./$(TARGET) $$f -o $(EXBUILD)/$$stem >$(TESTDIR)/tmp/ex_$$stem.log 2>&1; then \
	    echo "FAIL compile $$f"; cat $(TESTDIR)/tmp/ex_$$stem.log; err=1; continue; \
	  fi; \
	  if ! $(EXBUILD)/$$stem >/dev/null; then \
	    echo "FAIL run $$f"; err=1; continue; \
	  fi; \
	  echo "ok   $$f"; \
	done; \
	if ! ./$(TARGET) $(LANGEX)/oob.yuga -o $(EXBUILD)/oob >$(TESTDIR)/tmp/ex_oob.log 2>&1; then \
	  echo "FAIL compile $(LANGEX)/oob.yuga"; cat $(TESTDIR)/tmp/ex_oob.log; err=1; \
	elif $(EXBUILD)/oob >$(TESTDIR)/tmp/ex_oob.out 2>$(TESTDIR)/tmp/ex_oob.err; then \
	  echo "FAIL should-trap $(LANGEX)/oob.yuga"; err=1; \
	elif ! grep -q "index out of bounds" $(TESTDIR)/tmp/ex_oob.err; then \
	  echo "FAIL trap message $(LANGEX)/oob.yuga"; cat $(TESTDIR)/tmp/ex_oob.err; err=1; \
	else \
	  echo "ok   $(LANGEX)/oob.yuga (trapped)"; \
	fi; \
	for f in $(PASS) $(EXAMPLES); do \
	  stem=$$(basename $$f .yuga); \
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
	  src=$(TESTDIR)/compile_pass/$$stem.yuga; \
	  if [ ! -f $$src ]; then src=$(GOLDEN)/$$stem.yuga; fi; \
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
	  stem=$$(basename $$g .yuga); \
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
	if ! python3 $(TESTDIR)/lsp_smoke.py; then err=1; fi; \
	if [ $$err -ne 0 ]; then echo "TESTS FAILED"; exit 1; fi; \
	echo "ALL TESTS PASSED"
