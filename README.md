# Windows 11 Keyboard-Only Taskbar Hider

A [Windhawk](https://windhawk.net) mod that hides the Windows 11 taskbar for every application and blocks Windows' own hover/edge auto-reveal. The taskbar comes back only when you press the Windows key.

## Why this exists

Windows' "Automatically hide the taskbar" option still slides the bar back in (and steals focus) the moment the pointer reaches the screen edge. That gets in the way of anything that lives along that edge: a timeline scrollbar, a horizontal scrollbar, a status bar.

This mod takes the hover path away entirely. On Windows 11 the taskbar you see is an expanded shell view, not just a shown window, so the mod blocks the mouse unhide and lets the Windows key run the shell's own unhide. That is the path that stays up for the whole time Start is open. The previous version did this only while chosen apps were running. It now stays on for every app.

## Features

- **Always on** — the taskbar is hidden no matter which application is in front. There is no process list and no focus check.
- **No hover reveal** — moving the pointer to the screen edge does not show the taskbar and does not steal focus.
- **Windows key shows it** — the taskbar appears as soon as the Windows key goes down (or on Ctrl+Esc) and stays up for the whole time Start or Search is open. It hides again shortly after Start closes.
- **Shortcut chords stay quiet** — Win+E, Win+Tab, and other Windows-key chords do not pop the bar up.
- **Full primary screen** — while the bar is hidden, the primary monitor's work area fills the screen. Turning the mod off puts the work area back.
- **Reversible** — the settings switch, or disabling the mod, restores the taskbar.

https://github.com/user-attachments/assets/08ac4170-572b-4d19-84e9-2347bd0806f4

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

- Leave Windows' own **Automatically hide the taskbar** setting **off**. While this mod is enabled it turns that shell behavior on itself, and it writes the previous state back when you disable the mod. The collapsed bar is cloaked, so the thin auto-hide line does not show.
- Press the **Windows key** by itself to show the taskbar and open Start. **Ctrl+Esc** does the same thing. The bar hides again shortly after Start/Search closes and the pointer leaves the taskbar.
- Win combined with another key (Win+E, Win+1, Win+Tab, and so on) does not show the taskbar.
- Secondary taskbars are hidden too. The full-screen work area is applied to the primary monitor, which is the only monitor `SPI_SETWORKAREA` can change.

## Known limitations

- The bar can take a fraction of a second to hide again after you dismiss Start or move the pointer off it.
- Notification toasts and other shell popups do not pull the taskbar out. The Windows key is the way to show it.
- If Start never produces a visible window the mod can see, the bar still hides again after a short grace period. Press the Windows key once more to bring it back.
- The mod turns shell auto-hide on while it is loaded. Disabling it, or unloading it, writes the previous taskbar state back.

## How it works (technical)

The mod runs inside `explorer.exe` and:

1. Turns on the shell's auto-hide state (restored on unload), expands the primary work area to the full monitor, and cloaks the collapsed taskbar so no sliver remains. It does not `SW_HIDE` the tray window. On Windows 11 that leaves the XAML taskbar collapsed, so it stays invisible while Start is open and only flashes as Start closes.
2. Blocks the mouse unhide only: tray timer id 3, and the Win11 view reasons for pointer-over and screen-edge. While a keyboard reveal is active, `ShouldTaskbarBeExpanded` stays true and the hide timer is swallowed, so the bar cannot collapse out from under Start.
3. A low-level keyboard hook watches for a Windows-key tap with no other key in the chord, and for Ctrl+Esc. On key-down, before the event reaches the shell, it marks the bar shown and removes the cloak. The hook lives on its own thread so the show/hide work cannot trip Windows' low-level hook timeout.
4. The shell then runs its real unhide. The bar stays up while the Windows key is down, while Start or Search has a visible window, or while the pointer is over the taskbar. After Start closes it is collapsed and cloaked again.
5. Disabling the mod uncloaks the taskbar, restores the previous auto-hide state, shows the taskbar, and writes the original work area back.

## Disclaimer

This mod hooks system APIs to alter shell behavior. It's provided as-is — test it on your system before relying on it, and disable it via Windhawk at any time to fully restore default behavior.

## License

MIT

## Author

KWWDCoding — [homepage](https://www.kwwdcoding.com)

Based on "Windows 11 Taskbar Hider and Full Screen Utilizer" by Jeremi Lorenti — [Github](https://github.com/JeremiLorenti/windows11-taskbar-hider)

## Support Katy

☕ Building these tools does take time so if you'd like to financially support me you can do so on Ko-Fi

<a href='https://ko-fi.com/W7W61F06CS' target='_blank'><img height='36' style='border:0px;height:36px;' src='https://storage.ko-fi.com/cdn/kofi4.png?v=6' border='0' alt='Buy Me a Coffee at ko-fi.com' /></a>

👍 Can't support me financially? That's fine, I know times are hard. But if you'd consider sharing this tool on your socials that would be fantastic!

⭐ You can also star the project which will help increase visibility and show appreciation of my work!

---
*Developed with ❤️ by [KWWDCoding](https://www.kwwdcoding.com)*
