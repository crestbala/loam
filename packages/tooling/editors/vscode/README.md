# Yuga for Cursor / VS Code

Syntax highlighting (TextMate + semantic tokens) and IntelliSense (`yuga-lsp`: hover, go-to-definition, completion, diagnostics).

## Install

From the Yuga repo root:

```
make
make install-editor
```

Then **Developer: Reload Window**. The status bar language should read **Yuga**, not Plain Text / unknown.

`make install-editor` copies this folder into Cursor and VS Code's extensions directory **and registers it** (a copy alone is not enough). It walks up from the workspace to `<repo>/bin/yuga-lsp`, or uses `yuga.lspPath`.

After `make` rebuilds `bin/yuga-lsp`, reload the window so the editor picks up the new binary.
