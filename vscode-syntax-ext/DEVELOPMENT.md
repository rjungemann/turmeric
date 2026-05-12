# VSCode Extension Development Guide

## Quick Start

### Prerequisites
- VSCode 1.50 or later
- Node.js 12+ (for packaging)

### Setup

1. **Clone repository**
```bash
cd fith/vscode-syntax-ext
```

2. **Link for development** (macOS/Linux)
```bash
ln -s "$(pwd)" ~/.vscode/extensions/turmeric-syntax-dev
```

Or on Windows:
```powershell
cmd /c mklink /d "%USERPROFILE%\.vscode\extensions\turmeric-syntax-dev" "%cd%"
```

3. **Reload VSCode**
   - Press `Cmd+Shift+P` (macOS) or `Ctrl+Shift+P` (Windows/Linux)
   - Type "Reload Window" and press Enter

### Testing

1. **Open test file**
```bash
code test/test-syntax.tur
```

2. **Verify highlighting**
   - All keywords should be colored
   - Strings should be highlighted
   - Numbers should be colored
   - Comments should be visible but muted

### Making Changes

#### To modify syntax rules:
1. Edit `syntaxes/turmeric.tmLanguage.json`
2. Save the file
3. Reload VSCode window (Cmd+Shift+P → Reload Window)
4. Open test file to see changes

#### To modify language configuration:
1. Edit `language-configuration.json`
2. Reload VSCode window
3. Test indentation and bracket matching

#### To modify documentation:
1. Edit `README.md`
2. Changes apply immediately

### Common Debugging

**Syntax highlighting not updating?**
- Make sure VSCode is showing the test file
- Try `Reload Window` from Command Palette
- Check file association: Should show "Turmeric" in language mode

**Bracket matching not working?**
- Verify `language-configuration.json` has correct bracket pairs
- Check console for errors: `Help` → `Toggle Developer Tools`

**Scope names not matching theme?**
- Use Developer Tools → Console to inspect scopes
- Right-click on highlighted code → Inspect Tokens

### Publishing (Future)

To package as VSIX:
```bash
npm install -g @vscode/vsce
vsce package
```

## File Structure Reference

```
vscode-syntax-ext/
├── package.json              # Extension metadata and configuration
├── language-configuration.json # VSCode language behavior
├── syntaxes/
│   └── turmeric.tmLanguage.json # TextMate grammar rules
├── themes/                   # (Optional) Color themes
├── test/
│   └── test-syntax.tur      # Comprehensive test file
├── README.md                # User documentation
└── LICENSE                  # MIT License
```

## Useful VSCode Commands

| Action | Command |
|--------|---------|
| Open Command Palette | Cmd+Shift+P |
| Reload Window | Cmd+Shift+P → "Reload Window" |
| Open Developer Tools | Cmd+Shift+I |
| Inspect Tokens | Right-click → "Inspect Tokens" |
| Change Language Mode | Cmd+K M |
| Toggle Comment | Cmd+/ |

## Resources

- [VSCode Language Extension Docs](https://code.visualstudio.com/api/language-extensions/overview)
- [TextMate Grammar Reference](https://macromates.com/manual/en/language_grammars)
- [JSON Schema for TextMate](https://raw.githubusercontent.com/martinring/tmlanguage/master/tmlanguage.json)
- [VSCode Language Configuration](https://code.visualstudio.com/api/language-extensions/language-configuration-guide)

## Contributing

See README.md for contribution guidelines.
