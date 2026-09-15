# Loam for Zed

Syntax highlighting (Tree-sitter) and error checking (`loam-lsp`).

## Install

1. From the Loam repo root: `make && make grammar`
2. One-time: `rustup target add wasm32-wasip2` (Zed compiles the extension to WebAssembly)
3. In Zed: command palette → **zed: install dev extension**
4. Choose `packages/tooling/editors/zed`

Zed clones the grammar from `packages/tooling/tree-sitter-yuga` via `file://`. That directory must be its own git repo (`make grammar` creates it). If install fails with **failed to compile grammar 'yuga'**:

```bash
rm -rf packages/tooling/editors/zed/grammars
make grammar
```

Then uninstall Loam in Zed and install the dev extension again.

Open a `.loam` file. You should see highlighting, and the status bar should show **Loam** / **Loam LSP**.

After `make` rebuilds `bin/loam-lsp`, restart the language server (**editor: restart language server**) so Zed picks up the new binary. The extension walks up from the worktree to `<repo>/bin/loam-lsp` first, then `PATH`.

For the `.loam` file icon: command palette → **theme selector: toggle icon theme** → **Loam**. Only `.loam` files get the YG badge; `.expected` and other types keep the normal icons. The badge uses Zed’s `fill="black"` convention, so it follows the UI theme (grey on dark, dark on light), same as TypeScript.

## After changing the grammar

```bash
make grammar
```

That regenerates the parser, commits the nested grammar repo, writes the new SHA into `extension.toml`, and deletes Zed’s cached clone. Then reinstall the dev extension.
