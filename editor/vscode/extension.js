// SynthGraph language client: launches `synthc lsp` (the compiler's
// built-in language server) and wires it to .synth documents.
const fs = require("fs");
const path = require("path");
const vscode = require("vscode");
const { LanguageClient } = require("vscode-languageclient/node");

let client;

// The synthc executable: the explicit setting, then a build/synthc in a
// workspace folder (the layout `cmake -B build` produces), then PATH.
function findSynthc() {
  const configured = vscode.workspace
    .getConfiguration("synthgraph")
    .get("synthcPath");
  if (configured) return configured;
  for (const folder of vscode.workspace.workspaceFolders || []) {
    const candidate = path.join(folder.uri.fsPath, "build", "synthc");
    if (fs.existsSync(candidate)) return candidate;
    if (fs.existsSync(candidate + ".exe")) return candidate + ".exe";
  }
  return "synthc";
}

function activate(context) {
  const command = findSynthc();
  client = new LanguageClient(
    "synthgraph",
    "SynthGraph",
    { command, args: ["lsp"] },
    { documentSelector: [{ language: "synth" }] }
  );
  client.start().catch((err) => {
    vscode.window.showErrorMessage(
      `SynthGraph: could not start "${command} lsp" (${err.message}). ` +
        "Build synthc and set synthgraph.synthcPath if it is not on PATH " +
        "or in <workspace>/build/."
    );
  });
  context.subscriptions.push({ dispose: () => client && client.stop() });
}

function deactivate() {
  return client ? client.stop() : undefined;
}

module.exports = { activate, deactivate };
