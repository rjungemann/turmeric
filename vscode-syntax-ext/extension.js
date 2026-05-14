/* extension.js — Turmeric VS Code extension activation.
 *
 * Registers a document formatter that pipes the open file through
 * `tur format` (reading from stdin, writing formatted source to stdout).
 * The `tur` binary must be on PATH.
 */

'use strict';

const vscode = require('vscode');
const child_process = require('child_process');

/**
 * @param {vscode.ExtensionContext} context
 */
function activate(context) {
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

    /* Optional command palette entry */
    const cmd = vscode.commands.registerCommand(
        'turmeric.formatDocument',
        () => vscode.commands.executeCommand('editor.action.formatDocument')
    );
    context.subscriptions.push(cmd);
}

function deactivate() {}

module.exports = { activate, deactivate };
