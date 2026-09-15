"use strict";

const vscode = require("vscode");
const { spawn } = require("child_process");
const fs = require("fs");
const path = require("path");

let proc = null;
let buf = Buffer.alloc(0);
let nextId = 1;
const pending = new Map();
let ready = Promise.resolve();

function findLsp(folder) {
  const configured = vscode.workspace.getConfiguration("yuga").get("lspPath");
  if (configured && fs.existsSync(configured)) return configured;
  let dir = folder || "";
  while (dir && dir !== path.dirname(dir)) {
    const candidate = path.join(dir, "bin", "yuga-lsp");
    if (fs.existsSync(candidate)) return candidate;
    dir = path.dirname(dir);
  }
  return "yuga-lsp";
}

function send(obj) {
  if (!proc || !proc.stdin.writable) return;
  const body = Buffer.from(JSON.stringify(obj), "utf8");
  proc.stdin.write(`Content-Length: ${body.length}\r\n\r\n`);
  proc.stdin.write(body);
}

function request(method, params) {
  const id = nextId++;
  send({ jsonrpc: "2.0", id, method, params });
  return new Promise((resolve, reject) => {
    pending.set(id, { resolve, reject });
  });
}

function notify(method, params) {
  send({ jsonrpc: "2.0", method, params });
}

function readMessages() {
  for (;;) {
    const sep = buf.indexOf("\r\n\r\n");
    if (sep < 0) return;
    const header = buf.slice(0, sep).toString("utf8");
    const match = /content-length:\s*(\d+)/i.exec(header);
    if (!match) {
      buf = buf.slice(sep + 4);
      continue;
    }
    const len = parseInt(match[1], 10);
    const start = sep + 4;
    if (buf.length < start + len) return;
    const raw = buf.slice(start, start + len).toString("utf8");
    buf = buf.slice(start + len);
    let msg;
    try {
      msg = JSON.parse(raw);
    } catch {
      continue;
    }
    if (msg.id != null && pending.has(msg.id)) {
      const { resolve } = pending.get(msg.id);
      pending.delete(msg.id);
      resolve(msg);
    } else if (msg.method === "textDocument/publishDiagnostics") {
      const uri = vscode.Uri.parse(msg.params.uri);
      const diags = (msg.params.diagnostics || []).map((d) => {
        const r = d.range || { start: { line: 0, character: 0 }, end: { line: 0, character: 1 } };
        return new vscode.Diagnostic(
          new vscode.Range(r.start.line, r.start.character, r.end.line, r.end.character),
          d.message || "",
          vscode.DiagnosticSeverity.Error
        );
      });
      diagnosticCollection.set(uri, diags);
    }
  }
}

function pos(p) {
  return { line: p.line, character: p.character };
}

function asRange(r) {
  if (!r || !r.start) return undefined;
  return new vscode.Range(r.start.line, r.start.character, r.end.line, r.end.character);
}

function hoverFrom(result) {
  if (!result) return null;
  const c = result.contents;
  let text = "";
  if (typeof c === "string") text = c;
  else if (c && typeof c.value === "string") text = c.value;
  if (!text) return null;
  return new vscode.Hover(new vscode.MarkdownString(text));
}

let diagnosticCollection;

function activate(context) {
  diagnosticCollection = vscode.languages.createDiagnosticCollection("yuga");
  context.subscriptions.push(diagnosticCollection);

  const folder = vscode.workspace.workspaceFolders && vscode.workspace.workspaceFolders[0]
    ? vscode.workspace.workspaceFolders[0].uri.fsPath
    : "";
  const cmd = findLsp(folder);
  proc = spawn(cmd, [], { stdio: ["pipe", "pipe", "pipe"] });
  proc.on("error", (err) => {
    vscode.window.showErrorMessage(`yuga-lsp failed to start (${cmd}): ${err.message}`);
  });
  proc.stdout.on("data", (chunk) => {
    buf = Buffer.concat([buf, chunk]);
    readMessages();
  });
  proc.stderr.on("data", (chunk) => {
    console.log("[yuga-lsp]", chunk.toString());
  });
  proc.on("exit", () => {
    proc = null;
  });

  ready = request("initialize", {
    processId: process.pid,
    rootUri: folder ? vscode.Uri.file(folder).toString() : null,
    capabilities: {
      textDocument: {
        hover: { contentFormat: ["markdown", "plaintext"] },
        completion: { completionItem: { documentationFormat: ["markdown"] } },
        semanticTokens: { requests: { full: true } },
      },
    },
  }).then((msg) => {
    notify("initialized", {});
    vscode.workspace.textDocuments.filter((d) => d.languageId === "yuga").forEach(openDoc);
    return msg;
  });

  function openDoc(doc) {
    if (doc.languageId !== "yuga") return;
    notify("textDocument/didOpen", {
      textDocument: {
        uri: doc.uri.toString(),
        languageId: "yuga",
        version: doc.version,
        text: doc.getText(),
      },
    });
  }

  context.subscriptions.push(
    vscode.workspace.onDidOpenTextDocument(openDoc),
    vscode.workspace.onDidChangeTextDocument((e) => {
      if (e.document.languageId !== "yuga") return;
      notify("textDocument/didChange", {
        textDocument: { uri: e.document.uri.toString(), version: e.document.version },
        contentChanges: [{ text: e.document.getText() }],
      });
    }),
    vscode.workspace.onDidCloseTextDocument((doc) => {
      if (doc.languageId !== "yuga") return;
      notify("textDocument/didClose", { textDocument: { uri: doc.uri.toString() } });
      diagnosticCollection.delete(doc.uri);
    }),
    vscode.languages.registerHoverProvider("yuga", {
        async provideHover(doc, position) {
        await ready;
        const msg = await request("textDocument/hover", {
          textDocument: { uri: doc.uri.toString() },
          position: pos(position),
        });
        return hoverFrom(msg && msg.result);
      },
    }),
    vscode.languages.registerDefinitionProvider("yuga", {
        async provideDefinition(doc, position) {
        await ready;
        const msg = await request("textDocument/definition", {
          textDocument: { uri: doc.uri.toString() },
          position: pos(position),
        });
        const loc = msg && msg.result;
        if (!loc || !loc.uri) return null;
        return new vscode.Location(vscode.Uri.parse(loc.uri), asRange(loc.range) || new vscode.Position(0, 0));
      },
    }),
    vscode.languages.registerCompletionItemProvider(
      "yuga",
      {
        async provideCompletionItems(doc, position) {
          await ready;
          const msg = await request("textDocument/completion", {
            textDocument: { uri: doc.uri.toString() },
            position: pos(position),
          });
          const result = msg && msg.result;
          const items = (result && result.items) || result || [];
          return items.map((it) => {
            const c = new vscode.CompletionItem(it.label, it.kind || vscode.CompletionItemKind.Text);
            c.detail = it.detail;
            if (it.documentation && it.documentation.value) {
              c.documentation = new vscode.MarkdownString(it.documentation.value);
            }
            return c;
          });
        },
      },
      "."
    )
  );

  const legendTypes = [
    "namespace", "type", "enum", "struct", "parameter", "variable", "property",
    "enumMember", "function", "keyword", "comment", "string", "number", "operator",
    "modifier", "punctuation",
  ];
  const legend = new vscode.SemanticTokensLegend(legendTypes, ["declaration"]);
  context.subscriptions.push(
    vscode.languages.registerDocumentSemanticTokensProvider(
      "yuga",
      {
        async provideDocumentSemanticTokens(doc) {
          await ready;
          const msg = await request("textDocument/semanticTokens/full", {
            textDocument: { uri: doc.uri.toString() },
          });
          const data = msg && msg.result && msg.result.data;
          if (!data || !data.length) return new vscode.SemanticTokens(new Uint32Array());
          return new vscode.SemanticTokens(Uint32Array.from(data));
        },
      },
      legend
    )
  );

  context.subscriptions.push({
    dispose() {
      if (proc) {
        try {
          request("shutdown", null).then(() => notify("exit"));
        } catch {
          /* ignore */
        }
        proc.kill();
        proc = null;
      }
    },
  });
}

function deactivate() {
  if (proc) {
    proc.kill();
    proc = null;
  }
}

module.exports = { activate, deactivate };
