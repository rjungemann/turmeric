// Turmeric debugger -- VS Code extension stub.
//
// Wires VS Code's debug UI to the Turmeric DAP server (`tur dap`, debugger
// Phase 3).  The adapter itself lives in the compiler; this extension only
// declares the debug type and spawns `tur dap` as the adapter executable.
//
// See docs/artifacts/debugger-dap-phase3.md.

const vscode = require('vscode');

// Returns the adapter executable: `tur dap`, speaking DAP on its stdio.
class TurmericAdapterFactory {
  createDebugAdapterDescriptor(session) {
    const cfg = session.configuration || {};
    const tur = cfg.tur || 'tur';
    return new vscode.DebugAdapterExecutable(tur, ['dap']);
  }
}

// Fills in a sensible default config so "Run > Start Debugging" works on a .tur
// file with no launch.json.
const configProvider = {
  resolveDebugConfiguration(_folder, config) {
    if (!config.type && !config.request && !config.name) {
      config.type = 'turmeric';
      config.request = 'launch';
      config.name = 'Debug current Turmeric file';
      config.program = '${file}';
      config.stopOnEntry = true;
    }
    if (!config.program) {
      config.program = '${file}';
    }
    return config;
  }
};

function activate(context) {
  context.subscriptions.push(
    vscode.debug.registerDebugAdapterDescriptorFactory(
      'turmeric', new TurmericAdapterFactory()),
    vscode.debug.registerDebugConfigurationProvider(
      'turmeric', configProvider)
  );
}

function deactivate() {}

module.exports = { activate, deactivate };
