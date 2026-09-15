#!/usr/bin/env python3
# pyright: basic
# Smoke-test yuga-lsp: diagnostics, hover, go-to-definition, completion.
from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path
from typing import Any, Optional

ROOT = Path(__file__).resolve().parent.parent
REPO = ROOT.parent.parent
LSP = REPO / "bin" / "yuga-lsp"
Json = dict[str, Any]


def rpc(obj: Json) -> bytes:
    body = json.dumps(obj).encode()
    return b"Content-Length: %d\r\n\r\n" % len(body) + body


def read_message(buf: bytes) -> tuple[Optional[Json], bytes]:
    while True:
        sep = buf.find(b"\r\n\r\n")
        if sep < 0:
            return None, buf
        headers = buf[:sep].decode("utf-8", "replace")
        length = None
        for line in headers.split("\r\n"):
            if line.lower().startswith("content-length:"):
                length = int(line.split(":", 1)[1].strip())
        if length is None:
            return None, buf[sep + 4 :]
        start = sep + 4
        if len(buf) < start + length:
            return None, buf
        raw = buf[start : start + length]
        msg = json.loads(raw.decode())
        if not isinstance(msg, dict):
            return None, buf[start + length :]
        return msg, buf[start + length :]


def collect(out: bytes) -> list[Json]:
    messages: list[Json] = []
    rest = out
    while True:
        msg, rest = read_message(rest)
        if msg is None:
            break
        messages.append(msg)
    return messages


def fail(msg: str, extra: object = None) -> int:
    print("FAIL lsp:", msg)
    if extra is not None:
        print(extra)
    return 1


def hover_text(result: object) -> str:
    if not isinstance(result, dict):
        return ""
    contents = result.get("contents")
    if isinstance(contents, dict):
        value = contents.get("value", "")
        return value if isinstance(value, str) else str(value)
    if isinstance(contents, str):
        return contents
    return str(contents) if contents is not None else ""


def main() -> int:
    if not LSP.is_file():
        return fail("missing " + str(LSP))

    yuga_path = ROOT / "tests" / "tmp" / "lsp_smoke.yuga"
    uri = yuga_path.resolve().as_uri()
    bad = "fn main() {\n    let x = y\n}\n"
    good = (
        "/// Adds two integers.\n"
        "fn add(a: int, b: int) -> int {\n"
        "    return a + b\n"
        "}\n"
        "fn main() {\n"
        "    let x = add(1, 2)\n"
        "}\n"
    )

    rich = (
        'import "std:fmt"\n'
        "fn add(a: int, b: int) -> int {\n"
        "    return a + b\n"
        "}\n"
        "fn main() {\n"
        "    let x = add(1, 2)\n"
        "    fmt.println(x)\n"
        "    let f = || { 1 }\n"
        "    fmt.\n"
        "}\n"
    )
    fmt_uri = (ROOT / "std" / "fmt.yuga").resolve().as_uri()

    async_src = (
        'import "std:async"\n'
        'import "std:fmt"\n'
        "\n"
        "async fn fetch_name() -> string {\n"
        "    let f = async.future_str()\n"
        "    async.spawn(|| { async.resolve(f, \"n\") })\n"
        "    let v = await f\n"
        "    return v\n"
        "}\n"
        "\n"
        "fn main() {\n"
        "    let s = fetch_name()\n"
        "    fmt.println(s)\n"
        "}\n"
    )
    async_uri = (ROOT / "tmp" / "lsp_async.yuga").resolve().as_uri()

    kv_src = (
        'import "std:kv"\n'
        "\n"
        "fn main() {\n"
        "    let s = kv.open_path(\"/tmp/yuga_lsp_kv.kv\")\n"
        "    kv.set(s, \"a\", \"1\")\n"
        "    kv.\n"
        "}\n"
    )
    kv_uri = (ROOT / "tmp" / "lsp_kv.yuga").resolve().as_uri()

    payload = (
        rpc({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"capabilities": {}}})
        + rpc({"jsonrpc": "2.0", "method": "initialized", "params": {}})
        + rpc(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didOpen",
                "params": {
                    "textDocument": {
                        "uri": uri,
                        "languageId": "yuga",
                        "version": 1,
                        "text": bad,
                    }
                },
            }
        )
        + rpc(
            {
                "jsonrpc": "2.0",
                "id": 2,
                "method": "textDocument/hover",
                "params": {"textDocument": {"uri": uri}, "position": {"line": 1, "character": 12}},
            }
        )
        + rpc(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": uri, "version": 2},
                    "contentChanges": [{"text": good}],
                },
            }
        )
        + rpc(
            {
                "jsonrpc": "2.0",
                "id": 3,
                "method": "textDocument/hover",
                "params": {"textDocument": {"uri": uri}, "position": {"line": 5, "character": 12}},
            }
        )
        + rpc(
            {
                "jsonrpc": "2.0",
                "id": 4,
                "method": "textDocument/definition",
                "params": {"textDocument": {"uri": uri}, "position": {"line": 5, "character": 12}},
            }
        )
        + rpc(
            {
                "jsonrpc": "2.0",
                "id": 5,
                "method": "textDocument/hover",
                "params": {"textDocument": {"uri": uri}, "position": {"line": 2, "character": 11}},
            }
        )
        + rpc(
            {
                "jsonrpc": "2.0",
                "id": 7,
                "method": "textDocument/hover",
                "params": {"textDocument": {"uri": uri}, "position": {"line": 1, "character": 7}},
            }
        )
        + rpc(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": uri, "version": 3},
                    "contentChanges": [{"text": rich}],
                },
            }
        )
        + rpc(
            {
                "jsonrpc": "2.0",
                "id": 8,
                "method": "textDocument/hover",
                "params": {"textDocument": {"uri": uri}, "position": {"line": 0, "character": 12}},
            }
        )
        + rpc(
            {
                "jsonrpc": "2.0",
                "id": 9,
                "method": "textDocument/hover",
                "params": {"textDocument": {"uri": uri}, "position": {"line": 6, "character": 4}},
            }
        )
        + rpc(
            {
                "jsonrpc": "2.0",
                "id": 10,
                "method": "textDocument/definition",
                "params": {"textDocument": {"uri": uri}, "position": {"line": 6, "character": 4}},
            }
        )
        + rpc(
            {
                "jsonrpc": "2.0",
                "id": 11,
                "method": "textDocument/hover",
                "params": {"textDocument": {"uri": uri}, "position": {"line": 7, "character": 12}},
            }
        )
        + rpc(
            {
                "jsonrpc": "2.0",
                "id": 12,
                "method": "textDocument/completion",
                "params": {"textDocument": {"uri": uri}, "position": {"line": 8, "character": 8}},
            }
        )
        + rpc(
            {
                "jsonrpc": "2.0",
                "id": 15,
                "method": "textDocument/semanticTokens/full",
                "params": {"textDocument": {"uri": uri}},
            }
        )
        + rpc(
            {
                "jsonrpc": "2.0",
                "id": 16,
                "method": "textDocument/completion",
                "params": {"textDocument": {"uri": uri}, "position": {"line": 4, "character": 0}},
            }
        )
        + rpc(
            {
                "jsonrpc": "2.0",
                "id": 13,
                "method": "textDocument/hover",
                "params": {"textDocument": {"uri": fmt_uri}, "position": {"line": 23, "character": 3}},
            }
        )
        + rpc(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didChange",
                "params": {
                    "textDocument": {"uri": uri, "version": 4},
                    "contentChanges": [
                        {
                            "range": {
                                "start": {"line": 5, "character": 8},
                                "end": {"line": 5, "character": 9},
                            },
                            "text": "count",
                        }
                    ],
                },
            }
        )
        + rpc(
            {
                "jsonrpc": "2.0",
                "id": 14,
                "method": "textDocument/hover",
                "params": {"textDocument": {"uri": uri}, "position": {"line": 5, "character": 8}},
            }
        )
        + rpc(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didOpen",
                "params": {
                    "textDocument": {
                        "uri": async_uri,
                        "languageId": "yuga",
                        "version": 1,
                        "text": async_src,
                    }
                },
            }
        )
        + rpc(
            {
                "jsonrpc": "2.0",
                "id": 17,
                "method": "textDocument/semanticTokens/full",
                "params": {"textDocument": {"uri": async_uri}},
            }
        )
        + rpc(
            {
                "jsonrpc": "2.0",
                "id": 18,
                "method": "textDocument/completion",
                "params": {"textDocument": {"uri": async_uri}, "position": {"line": 10, "character": 0}},
            }
        )
        + rpc(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didOpen",
                "params": {
                    "textDocument": {
                        "uri": async_uri,
                        "languageId": "yuga",
                        "version": 2,
                        "text": async_src,
                    }
                },
            }
        )
        + rpc(
            {
                "jsonrpc": "2.0",
                "id": 19,
                "method": "textDocument/hover",
                "params": {"textDocument": {"uri": uri}, "position": {"line": 5, "character": 16}},
            }
        )
        + rpc(
            {
                "jsonrpc": "2.0",
                "method": "textDocument/didOpen",
                "params": {
                    "textDocument": {
                        "uri": kv_uri,
                        "languageId": "yuga",
                        "version": 1,
                        "text": kv_src,
                    }
                },
            }
        )
        + rpc(
            {
                "jsonrpc": "2.0",
                "id": 20,
                "method": "textDocument/hover",
                "params": {"textDocument": {"uri": kv_uri}, "position": {"line": 0, "character": 12}},
            }
        )
        + rpc(
            {
                "jsonrpc": "2.0",
                "id": 21,
                "method": "textDocument/completion",
                "params": {"textDocument": {"uri": kv_uri}, "position": {"line": 5, "character": 7}},
            }
        )
        + rpc(
            {
                "jsonrpc": "2.0",
                "id": 22,
                "method": "textDocument/semanticTokens/full",
                "params": {"textDocument": {"uri": kv_uri}},
            }
        )
        + rpc(
            {
                "jsonrpc": "2.0",
                "id": 23,
                "method": "textDocument/completion",
                "params": {"textDocument": {"uri": kv_uri}, "position": {"line": 0, "character": 12}},
            }
        )
        + rpc({"jsonrpc": "2.0", "id": 6, "method": "shutdown", "params": None})
        + rpc({"jsonrpc": "2.0", "method": "exit"})
    )
    proc = subprocess.Popen(
        [str(LSP)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    out, err = proc.communicate(payload, timeout=10)
    messages = collect(out)

    inits = [m for m in messages if m.get("id") == 1 and "result" in m]
    if not inits:
        return fail("no initialize result", (out[:500], err[:500]))
    caps = inits[0]["result"]
    if not isinstance(caps, dict):
        return fail("initialize result is not an object", caps)
    caps = caps.get("capabilities", {})
    if not isinstance(caps, dict):
        return fail("unexpected capabilities", caps)
    if caps.get("textDocumentSync") != 1:
        return fail("unexpected capabilities", caps)
    if caps.get("hoverProvider") is not True:
        return fail("missing hoverProvider", caps)
    if caps.get("definitionProvider") is not True:
        return fail("missing definitionProvider", caps)
    comp = caps.get("completionProvider")
    if not isinstance(comp, dict) or "." not in (comp.get("triggerCharacters") or []):
        return fail("missing completionProvider triggerCharacters", caps)
    sem = caps.get("semanticTokensProvider")
    if not isinstance(sem, dict):
        return fail("missing semanticTokensProvider", caps)
    legend = sem.get("legend") if isinstance(sem, dict) else None
    types = legend.get("tokenTypes") if isinstance(legend, dict) else None
    if not isinstance(types, list) or "keyword" not in types:
        return fail("semanticTokens legend missing keyword", caps)

    diags_msgs = [m for m in messages if m.get("method") == "textDocument/publishDiagnostics"]
    if not diags_msgs:
        return fail("no publishDiagnostics", messages)
    params = diags_msgs[0].get("params", {})
    items = params.get("diagnostics", []) if isinstance(params, dict) else []
    if not isinstance(items, list) or not items:
        return fail("empty diagnostics for invalid source")
    first = items[0]
    if not isinstance(first, dict):
        return fail("diagnostic is not an object", first)
    rng = first.get("range", {})
    if not isinstance(rng, dict):
        return fail("missing range", rng)
    start = rng.get("start", {})
    end = rng.get("end", {})
    if not isinstance(start, dict) or not isinstance(end, dict):
        return fail("bad range", rng)
    if start.get("line") != 1 or start.get("character") != 12:
        return fail("unknown-ident start range", rng)
    if end.get("line") != 1 or end.get("character") != 13:
        return fail("unknown-ident end range (expected token span)", rng)

    hovers = {m.get("id"): m for m in messages if m.get("id") in (2, 3, 5) and "result" in m}
    if 3 not in hovers:
        return fail("no hover result for add()", messages)
    value = hover_text(hovers[3].get("result"))
    if "add" not in value or "fn(" not in value:
        return fail("hover on add() should show fn type", value)
    if "Adds two integers" not in value:
        return fail("hover on add() should include /// doc comment", value)

    if 5 not in hovers:
        return fail("no hover result for param a", messages)
    value_a = hover_text(hovers[5].get("result"))
    if "int" not in value_a:
        return fail("hover on param use should show int", value_a)

    defs = [m for m in messages if m.get("id") == 4 and "result" in m]
    if not defs:
        return fail("no definition result", messages)
    loc = defs[0].get("result")
    if not isinstance(loc, dict) or loc.get("uri") != uri:
        return fail("definition uri", loc)
    loc_range = loc.get("range") if isinstance(loc, dict) else None
    loc_start = loc_range.get("start") if isinstance(loc_range, dict) else None
    if not isinstance(loc_start, dict) or loc_start.get("line") != 1:
        return fail("definition should jump to fn add", loc)

    hovers.update({m.get("id"): m for m in messages if m.get("id") in (7, 8, 9, 11, 13) and "result" in m})
    if 7 not in hovers:
        return fail("no hover result for signature param a", messages)
    value_sig = hover_text(hovers[7].get("result"))
    if "a:" not in value_sig or "int" not in value_sig:
        return fail("hover on fn param name should show a: int", value_sig)

    if 8 not in hovers:
        return fail("no hover result for import", messages)
    value_imp = hover_text(hovers[8].get("result"))
    if "module" not in value_imp or "fmt" not in value_imp:
        return fail("hover on import should show module fmt", value_imp)

    if 9 not in hovers:
        return fail("no hover result for module prefix fmt", messages)
    value_fmt = hover_text(hovers[9].get("result"))
    if "module fmt" not in value_fmt:
        return fail("hover on fmt. should show module fmt", value_fmt)

    defs_fmt = [m for m in messages if m.get("id") == 10 and "result" in m]
    if not defs_fmt:
        return fail("no definition result for module fmt", messages)
    floc = defs_fmt[0].get("result")
    if not isinstance(floc, dict) or "fmt.yuga" not in str(floc.get("uri", "")):
        return fail("definition of fmt should jump to std/fmt.yuga", floc)

    if 11 not in hovers:
        return fail("no hover result for closure", messages)
    value_cl = hover_text(hovers[11].get("result"))
    if "fn(" not in value_cl:
        return fail("hover on || closure should show fn type", value_cl)

    comps = [m for m in messages if m.get("id") == 12 and "result" in m]
    if not comps:
        return fail("no completion result", messages)
    cres = comps[0].get("result")
    items_c = cres.get("items") if isinstance(cres, dict) else cres
    if not isinstance(items_c, list) or not items_c:
        return fail("empty completion after fmt.", cres)
    labels = [it.get("label") for it in items_c if isinstance(it, dict)]
    if "println" not in labels and "writeln" not in labels:
        return fail("completion after fmt. should list fmt functions", labels)

    tokens = [m for m in messages if m.get("id") == 15 and "result" in m]
    if not tokens:
        return fail("no semanticTokens/full result", messages)
    data = tokens[0].get("result", {}).get("data") if isinstance(tokens[0].get("result"), dict) else None
    if not isinstance(data, list) or len(data) < 5:
        return fail("semantic tokens empty", tokens[0].get("result"))
    tok_types = data[3::5]
    if 9 not in tok_types:
        return fail("semantic tokens should include keyword (type 9)", tok_types[:20])
    if 11 not in tok_types:
        return fail("semantic tokens should include string (type 11)", tok_types[:20])
    if 0 not in tok_types:
        return fail("semantic tokens should include namespace (type 0) for fmt.", tok_types[:40])
    if 4 not in tok_types:
        return fail("semantic tokens should include parameter (type 4)", tok_types[:40])
    if 8 not in tok_types:
        return fail("semantic tokens should include function (type 8)", tok_types[:40])

    kcomps = [m for m in messages if m.get("id") == 16 and "result" in m]
    if not kcomps:
        return fail("no keyword completion result", messages)
    kres = kcomps[0].get("result")
    kitems = kres.get("items") if isinstance(kres, dict) else kres
    klabels = [it.get("label") for it in kitems if isinstance(it, dict)] if isinstance(kitems, list) else []
    if "let" not in klabels or "enum" not in klabels:
        return fail("completion at line start should list keywords", klabels[:30])

    if 13 not in hovers:
        return fail("no hover result in imported std/fmt.yuga", messages)
    value_w = hover_text(hovers[13].get("result"))
    if "write" not in value_w:
        return fail("hover in imported module should find fn write", value_w)

    hovers.update({m.get("id"): m for m in messages if m.get("id") == 14 and "result" in m})
    if 14 not in hovers:
        return fail("no hover result after incremental didChange", messages)
    value_ren = hover_text(hovers[14].get("result"))
    if "count" not in value_ren or "int" not in value_ren:
        return fail("hover after incremental edit should show count: int", value_ren)

    # Contextual keywords `async fn` / `await` must tokenize as keywords, and
    # the `async.` prefix must read as a module namespace.
    atoks = [m for m in messages if m.get("id") == 17 and "result" in m]
    if not atoks:
        return fail("no semanticTokens/full result for async buffer", messages)
    adata = atoks[0].get("result", {}).get("data")
    if not isinstance(adata, list) or len(adata) < 5:
        return fail("async semantic tokens empty", atoks[0].get("result"))
    aline = acol = 0
    by_pos = {}
    for j in range(0, len(adata) - 4, 5):
        aline += adata[j]
        if adata[j] == 0:
            acol += adata[j + 1]
        else:
            acol = adata[j + 1]
        by_pos[(aline, acol, adata[j + 2])] = adata[j + 3]
    alines = async_src.split("\n")
    for lineno, needle in ((3, "async"), (6, "await")):
        col = alines[lineno].index(needle)
        if by_pos.get((lineno, col, len(needle))) != 9:
            return fail("%s should be keyword type 9" % needle, by_pos)
    ncol = alines[4].index("async.")
    if by_pos.get((4, ncol, 5)) != 0:
        return fail("async. prefix should be namespace type 0", by_pos)

    acomp = [m for m in messages if m.get("id") == 18 and "result" in m]
    if not acomp:
        return fail("no completion result for async buffer", messages)
    ares = acomp[0].get("result")
    aitems = ares.get("items") if isinstance(ares, dict) else ares
    alabels = [it.get("label") for it in aitems if isinstance(it, dict)] if isinstance(aitems, list) else []
    if "async" not in alabels or "await" not in alabels:
        return fail("completion should list async/await keywords", alabels[:40])

    # A redundant didOpen of an already-open buffer (same text) must be a
    # no-op, and the earlier document must still resolve after the async one
    # was activated: buffers switch per requested uri.
    back_hover = [m for m in messages if m.get("id") == 19 and "result" in m]
    if not back_hover:
        return fail("no hover result after redundant didOpen", messages)
    value_back = hover_text(back_hover[0].get("result"))
    if "add" not in value_back or "fn(" not in value_back:
        return fail("hover should switch back to the first doc's fn add", value_back)

    kv_hover = [m for m in messages if m.get("id") == 20 and "result" in m]
    if not kv_hover:
        return fail("no hover result for import std:kv", messages)
    value_kv = hover_text(kv_hover[0].get("result"))
    if "kv" not in value_kv.lower() and "module" not in value_kv:
        return fail("hover on import std:kv should show module kv", value_kv)

    kvcomp = [m for m in messages if m.get("id") == 21 and "result" in m]
    if not kvcomp:
        return fail("no completion result for kv.", messages)
    kvres = kvcomp[0].get("result")
    kvitems = kvres.get("items") if isinstance(kvres, dict) else kvres
    kvlabs = [it.get("label") for it in kvitems if isinstance(it, dict)] if isinstance(kvitems, list) else []
    if "get" not in kvlabs or "set" not in kvlabs:
        return fail("completion after kv. should list get/set", kvlabs[:40])

    kvtoks = [m for m in messages if m.get("id") == 22 and "result" in m]
    if not kvtoks:
        return fail("no semanticTokens/full result for kv buffer", messages)
    kdata = kvtoks[0].get("result", {}).get("data")
    if not isinstance(kdata, list) or len(kdata) < 5:
        return fail("kv semantic tokens empty", kvtoks[0].get("result"))
    kline = kcol = 0
    kpos = {}
    for j in range(0, len(kdata) - 4, 5):
        kline += kdata[j]
        if kdata[j] == 0:
            kcol += kdata[j + 1]
        else:
            kcol = kdata[j + 1]
        kpos[(kline, kcol, kdata[j + 2])] = kdata[j + 3]
    klines = kv_src.split("\n")
    kcol0 = klines[3].index("kv.")
    if kpos.get((3, kcol0, 2)) != 0:
        return fail("kv. prefix should be namespace type 0", kpos)

    icomp = [m for m in messages if m.get("id") == 23 and "result" in m]
    if not icomp:
        return fail("no completion result inside import string", messages)
    ires = icomp[0].get("result")
    iitems = ires.get("items") if isinstance(ires, dict) else ires
    ilabs = [it.get("label") for it in iitems if isinstance(it, dict)] if isinstance(iitems, list) else []
    if "std:kv" not in ilabs:
        return fail("import completion should list std:kv", ilabs[:40])

    print("ok   yuga-lsp (%d diagnostic(s), hover, definition, completion, semantic tokens)" % len(items))
    return 0


if __name__ == "__main__":
    sys.exit(main())
