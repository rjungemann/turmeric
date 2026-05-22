'use strict';

const vscode = require('vscode');
const child_process = require('child_process');
const { LanguageClient, TransportKind } = require('vscode-languageclient/node');

let client;

function activate(context) {
    /* --- Document formatter (existing: pipes through `tur format`) --- */
    const formatter = vscode.languages.registerDocumentFormattingEditProvider(
        'turmeric',
        {
            provideDocumentFormattingEdits(document) {
                const text = document.getText();
                let formatted;
                try {
                    formatted = child_process
                        .execSync('tur format', { input: text, timeout: 5000 })
                        .toString();
                } catch (err) {
                    vscode.window.showErrorMessage(
                        'Turmeric formatter failed: ' + (err.message || String(err))
                    );
                    return [];
                }
                const fullRange = new vscode.Range(
                    document.positionAt(0),
                    document.positionAt(text.length)
                );
                return [vscode.TextEdit.replace(fullRange, formatted)];
            }
        }
    );
    context.subscriptions.push(formatter);

    const cmd = vscode.commands.registerCommand(
        'turmeric.formatDocument',
        () => vscode.commands.executeCommand('editor.action.formatDocument')
    );
    context.subscriptions.push(cmd);

    /* --- LSP client (launches `tur lsp` as a child process) --- */
    const config = vscode.workspace.getConfiguration('turmeric');
    const serverPath = config.get('serverPath', 'tur');

    const serverOptions = {
        command: serverPath,
        args: ['lsp'],
        transport: TransportKind.stdio,
    };

    const clientOptions = {
        documentSelector: [{ scheme: 'file', language: 'turmeric' }],
        synchronize: {
            fileEvents: vscode.workspace.createFileSystemWatcher('**/*.tur'),
        },
    };

    client = new LanguageClient(
        'turmeric-lsp',
        'Turmeric Language Server',
        serverOptions,
        clientOptions,
    );

    client.start();
    context.subscriptions.push({ dispose: () => client.stop() });
}

function deactivate() {
    if (client) return client.stop();
}

module.exports = { activate, deactivate };
