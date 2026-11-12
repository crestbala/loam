# Loam for Cursor / VS Code

Syntax highlighting (TextMate + semantic tokens) and IntelliSense (`loam-lsp`: hover, go-to-definition, completion, diagnostics).

## Install

From the Loam repo root:

```
./nob.sh
./nob.sh install-editor
```

Then **Developer: Reload Window**. The status bar language should read **Loam**, not Plain Text / unknown.

`./nob.sh install-editor` copies this folder into Cursor and VS Code's extensions directory **and registers it** (a copy alone is not enough). It walks up from the workspace to `<repo>/bin/loam-lsp`, or uses `loam.lspPath`.

After `./nob.sh` rebuilds `bin/loam-lsp`, reload the window so the editor picks up the new binary.
