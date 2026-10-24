# Quota

Quota shows limits, usage, and cost. It reads the same local history as the T3 Code Usage panel.

![Quota](screenshot.png)

## Install

From the AUR:

```sh
yay -S quota
```

Or download the package from the latest release, then install it:

```sh
sudo pacman -U quota-*.pkg.tar.zst
```

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

Usage and cost use the T3 Code scan cache at `~/.t3/userdata`. Limits use the Claude, Codex, and Grok logins on this computer. The cost is an API estimate. It is not the subscription bill.

The T3 Code clone is at `~/Work/t3code`.

## Limits

Each limit shows how much is left and when it resets. The thin mark on the bar shows how much of the window has passed. When the fill runs past the mark, the limit is burning faster than it resets, and the tile says when it runs out at that pace.

## Screenshot

`build/quota --preview screenshot.png 960x1100` renders sample data to an image and quits.
