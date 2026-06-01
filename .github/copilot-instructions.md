# Copilot instructions for turmeric

Follow the repository-wide guidance in `../AGENTS.md` and `../CLAUDE.md`.

`CLAUDE.md` is the authoritative project guide. `AGENTS.md` exists so tools
that look for a generic agent instruction file still load the same repo rules.

Spice implementations are in `../turmeric-spices`. Do not create or scaffold
`./spices/` inside this repository.

<!-- rtk-instructions v2 -->
# RTK — Token-Optimized CLI

**rtk** is a CLI proxy that filters and compresses command outputs, saving 60-90% tokens.

## Rule

Always prefix shell commands with `rtk`:

```bash
# Instead of:              Use:
git status                 rtk git status
git log -10                rtk git log -10
cargo test                 rtk cargo test
docker ps                  rtk docker ps
kubectl get pods           rtk kubectl pods
```

## Meta commands (use directly)

```bash
rtk gain              # Token savings dashboard
rtk gain --history    # Per-command savings history
rtk discover          # Find missed rtk opportunities
rtk proxy <cmd>       # Run raw (no filtering) but track usage
```
<!-- /rtk-instructions -->