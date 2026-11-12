# Loam for Zed

Syntax highlighting (Tree-sitter) and error checking (`loam-lsp`).

## Install

1. From the Loam repo root: `./nob.sh && ./nob.sh grammar`
2. One-time: `rustup target add wasm32-wasip2` (Zed compiles the extension to WebAssembly)
3. In Zed: command palette → **zed: install dev extension**
4. Choose `packages/tooling/editors/zed`

Zed clones a grammar from a git repo. The grammar is plain files in `packages/tooling/tree-sitter-loam`, so `./nob.sh grammar` copies them into a scratch repo, `out/zed-grammar`, and points `extension.toml` at it via `file://`. If install fails with **failed to compile grammar 'loam'**:

```bash
rm -rf packages/tooling/editors/zed/grammars
./nob.sh grammar
```

Then uninstall Loam in Zed and install the dev extension again.

Open a `.loam` file. You should see highlighting, and the status bar should show **Loam** / **Loam LSP**.

After `./nob.sh` rebuilds `bin/loam-lsp`, restart the language server (**editor: restart language server**) so Zed picks up the new binary. The extension walks up from the worktree to `<repo>/bin/loam-lsp` first, then `PATH`.

For the `.loam` file icon: command palette → **theme selector: toggle icon theme** → **Loam**. Only `.loam` files get the YG badge; `.expected` and other types keep the normal icons. The badge uses Zed’s `fill="black"` convention, so it follows the UI theme (grey on dark, dark on light), same as TypeScript.

## After changing the grammar

```bash
./nob.sh grammar
```

That regenerates the parser, rebuilds `out/zed-grammar`, writes its path and SHA into `extension.toml` (the SHA depends only on the grammar files), and deletes Zed’s cached clone. Commit the grammar and `extension.toml` together. Then reinstall the dev extension.
