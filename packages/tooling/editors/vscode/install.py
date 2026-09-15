#!/usr/bin/env python3
"""Copy the Yuga editor extension into Cursor/VS Code and register it."""
from __future__ import annotations

import json
import shutil
import time
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parent
HOME = Path.home()
EXT_ID = "yuga.yuga"
VERSION = "0.1.0"
FOLDER = f"{EXT_ID}-{VERSION}"


def dest_dirs() -> list[Path]:
    dirs = [HOME / ".cursor" / "extensions", HOME / ".vscode" / "extensions"]
    return [d for d in dirs if d.parent.is_dir()]


def copy_ext(parent: Path) -> Path:
    dest = parent / FOLDER
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    skip = {".git", "__pycache__", "install.py"}
    for src in ROOT.rglob("*"):
        if src.is_dir() or src.name in skip:
            continue
        rel = src.relative_to(ROOT)
        out = dest / rel
        out.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, out)
    return dest


def register(parent: Path, dest: Path) -> None:
    index = parent / "extensions.json"
    entries = []
    if index.is_file():
        try:
            entries = json.loads(index.read_text())
        except json.JSONDecodeError:
            entries = []
    if not isinstance(entries, list):
        entries = []
    entries = [
        e
        for e in entries
        if not (
            isinstance(e, dict)
            and isinstance(e.get("identifier"), dict)
            and e["identifier"].get("id") == EXT_ID
        )
    ]
    loc = str(dest)
    entries.append(
        {
            "identifier": {"id": EXT_ID, "uuid": "8a0d4c2e-6f31-4b9a-9c11-2e7b1d4a90f3"},
            "version": VERSION,
            "location": {
                "$mid": 1,
                "fsPath": loc,
                "external": dest.as_uri(),
                "path": loc,
                "scheme": "file",
            },
            "relativeLocation": FOLDER,
            "metadata": {
                "installedTimestamp": int(time.time() * 1000),
                "source": "vsix",
                "isApplicationScoped": False,
                "isMachineScoped": False,
                "isBuiltin": False,
                "publisherDisplayName": "Yuga",
                "publisherId": str(uuid.UUID("8a0d4c2e-6f31-4b9a-9c11-2e7b1d4a90f3")),
            },
        }
    )
    index.write_text(json.dumps(entries))


def main() -> None:
    n = 0
    for parent in dest_dirs():
        parent.mkdir(parents=True, exist_ok=True)
        dest = copy_ext(parent)
        register(parent, dest)
        print("installed", dest)
        n += 1
    if n == 0:
        raise SystemExit("no Cursor/VS Code extensions directory found")
    print("Reload the editor window. Status bar should show Yuga, not Plain Text.")


if __name__ == "__main__":
    main()
