# Windows 11 Keyboard-Only Taskbar Hider

A [Windhawk](https://windhawk.net) mod that hides the Windows 11 taskbar for every application and blocks Windows' own hover/edge auto-reveal. The taskbar comes back only when you press the Windows key.

## Why this exists

Windows' "Automatically hide the taskbar" option still slides the bar back in (and steals focus) the moment the pointer reaches the screen edge. That gets in the way of anything that lives along that edge: a timeline scrollbar, a horizontal scrollbar, a status bar.

This mod takes the hover path away entirely. It turns on the shell's own auto-hide while loaded, then blocks only the mouse unhide. The Windows key is left alone, so Start opens with the taskbar and keeps it visible until Start closes.

## Features

- **Always on** — the taskbar is hidden no matter which application is in front.
- **No hover reveal** — moving the pointer to the screen edge does not show the taskbar.
- **Windows key shows it** — Start opens with the taskbar and the bar stays up until Start closes.
- **Reversible** — disabling the mod restores the previous auto-hide setting.

## Requirements

- Windows 11
- [Windhawk](https://windhawk.net) installed and running

## Installation

1. Install [Windhawk](https://windhawk.net) if you haven't already.
2. Open Windhawk and go to **Explore**, then search for this mod (or use **Create New Mod** and paste in the source from this repo if it's not yet published).
3. Click **Install**, then enable it. The only setting is the master switch.

## Settings

| Setting | Description | Default |
|---|---|---|
| **Hide the taskbar** | Master switch. On: the bar stays hidden and hover cannot reveal it. Off: normal Windows taskbar behavior. | `true` |

## Usage notes

- Leave Windows' own **Automatically hide the taskbar** setting **off**. While this mod is enabled it turns that shell behavior on itself, and it writes the previous state back when you disable the mod.
- Press the **Windows key** (or **Ctrl+Esc**) to open Start with the taskbar. Close Start and the bar hides again.
- Secondary taskbars are covered too.

## How it works (technical)

1. Turns on the shell's auto-hide state (`ABM_SETSTATE` / `ABS_AUTOHIDE`) while the mod is loaded, and restores the previous state on unload.
2. Hooks `SetTimer` and drops the tray mouse-unhide timer (id 3).
3. On Windows 11, hooks `ViewCoordinator::UpdateIsExpanded` and ignores the pointer-over / screen-edge reasons. The Windows key expand path is left alone.
4. Cloaks the collapsed bar so the thin auto-hide sliver does not show, and uncloaks it when the shell expands it.

## Disclaimer

This mod hooks system APIs to alter shell behavior. It's provided as-is — test it on your system before relying on it, and disable it via Windhawk at any time to fully restore default behavior.

## License

MIT

## Author

KWWDCoding — [homepage](https://www.kwwdcoding.com)

Based on "Windows 11 Taskbar Hider and Full Screen Utilizer" by Jeremi Lorenti — [Github](https://github.com/JeremiLorenti/windows11-taskbar-hider)

## Support Katy

☕ Building these tools does take time so if you'd like to financially support me you can do so on Ko-Fi

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/X8X41J6Y4H)
