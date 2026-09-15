use std::path::PathBuf;
use zed_extension_api::{self as zed, Result};

struct YugaExtension;

fn yuga_lsp_path(worktree: &zed::Worktree) -> String {
    let mut dir = PathBuf::from(worktree.root_path());
    loop {
        let candidate = dir.join("bin").join("yuga-lsp");
        if candidate.is_file() {
            return candidate.to_string_lossy().into_owned();
        }
        if !dir.pop() {
            break;
        }
    }
    worktree
        .which("yuga-lsp")
        .unwrap_or_else(|| format!("{}/bin/yuga-lsp", worktree.root_path()))
}

impl zed::Extension for YugaExtension {
    fn new() -> Self {
        Self
    }

    fn language_server_command(
        &mut self,
        _language_server_id: &zed::LanguageServerId,
        worktree: &zed::Worktree,
    ) -> Result<zed::Command> {
        Ok(zed::Command {
            command: yuga_lsp_path(worktree),
            args: vec![],
            env: Default::default(),
        })
    }
}

zed::register_extension!(YugaExtension);
