# Quota

Quota shows limits, usage, and cost. It reads the same local history as the T3 Code Usage panel.

![Quota](screenshot.png)

## Install

Install the package from the latest release:

```sh
sudo pacman -U https://github.com/vekhyat/quota/releases/download/v0.2.0/quota-0.2.0-1-x86_64.pkg.tar.zst
```

It conflicts with `quota-tools`, since both install `/usr/bin/quota`.

Or build and install from source with `bin/install`. It needs `qmake6`, `make`, and Qt 6 (`qt6-base`, `qt6-declarative`, `qt6-wayland`).

## Open

Press Super + Space, then type Quota.

## Keys

| Key | Action |
| --- | --- |
| ? | Show the shortcut list |
| Q | Quit |
| Space | Refresh |
| C | Show cost |
| T | Show tokens |
| L | Scroll to limits |
| 1 | Past 24 hours |
| 2 | Past 7 days |
| 3 | Past 30 days |
| 4 | Past 90 days |

## Data

Usage and cost come from the Claude Code, Codex, Pi, and Grok history on this computer. T3 Code is not needed. If T3 Code is installed, its scan cache adds providers Quota does not read, such as Cursor or OpenCode. Prices come from the LiteLLM price list, with a built-in copy for offline use.

Quota finds every config: `~/.claude`, `~/.codex`, and siblings such as `~/.claude-work` or `~/.codex2`, plus `CLAUDE_CONFIG_DIR` and `CODEX_HOME`. List any other config in `~/.config/quota/sources`, one per line:

```
claude ~/work/claude-config
codex /mnt/data/codex
``` Limits use the Claude, Codex, and Grok logins on this computer. The cost is an API estimate. It is not the subscription bill.


## Limits

Each limit shows how much is left and when it resets. The thin mark on the bar shows how much of the window has passed. When the fill runs past the mark, the limit is burning faster than it resets, and the tile says when it runs out at that pace.

## Screenshot

`build/quota --preview screenshot.png 960x1100` renders sample data to an image and quits.
