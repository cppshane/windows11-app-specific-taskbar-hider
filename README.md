# Windows 11 App Specific Taskbar Hider

A [Windhawk](https://windhawk.net) mod that hides the Windows 11 taskbar and reclaims the full screen while chosen applications are running — and actually blocks Windows own hover/edge auto-reveal, so you can't accidentally bring the taskbar back by moving your mouse too close to the bottom of the screen.

## Why this exists

Windows built-in "Automatically hide the taskbar" option still pops the taskbar back up (and steals focus) the moment your cursor gets near the screen edge — which is a problem if your app has UI elements down there too, like a timeline scrollbar in DaVinci Resolve, or a horizontal scrollbar in Photoshop/InDesign. One accidental drag too far and you're fighting the taskbar instead of your project.

This mod fixes that by intercepting the Windows APIs the shell uses to show and focus the taskbar (`ShowWindow`, `SetWindowPos`, `SetForegroundWindow`), and simply blocking those calls while your chosen app is active. The taskbar never gets a chance to appear or take focus in the first place.

## Features

- **Per-app targeting** — only hides the taskbar while specific applications (by EXE name) are running, e.g. `Resolve.exe`, `Photoshop.exe`, `FL64.exe`, `InDesign.exe`.
- **Optional focus requirement** — choose whether hiding requires the target app to be the *active* window, or just running in the background.
- **True hover-blocking** — unlike a simple hide-on-a-timer approach, this stops Windows own auto-reveal at the source, so hovering into the taskbar's screen area does nothing while active.
- **Reversible** — disable the mod (or switch away from the target app) and the taskbar and full-screen work area are restored automatically.
- **Start menu still works** — pressing the Win key still opens the Start menu even while the taskbar is hidden.


https://github.com/user-attachments/assets/08ac4170-572b-4d19-84e9-2347bd0806f4

## Requirements

- Windows 11
- [Windhawk](https://windhawk.net) installed and running

## Installation

1. Install [Windhawk](https://windhawk.net) if you haven't already.
2. Open Windhawk and go to **Explore**, then search for this mod (or use **Create New Mod** and paste in the source from this repo if it's not yet published).
3. Click **Install**, then open the mod's **Settings** tab.

## Settings

| Setting | Description | Default |
|---|---|---|
| **Enable taskbar auto-hiding** | Master switch for the mod. | `true` |
| **Target processes** | List of EXE filenames (no path) that should trigger hiding, e.g. `Resolve.exe`, `Photoshop.exe`. | `Resolve.exe, Photoshop.exe, FL64.exe, InDesign.exe` |
| **Require focus (not just running)** | If enabled, the taskbar only hides while a target app is the active window — switching away (e.g. to your browser) brings it back immediately. If disabled, it stays hidden as long as the target app is running at all. | `true` |

You can find an app's real process name in **Task Manager → Details** tab. A few common ones:

- DaVinci Resolve → `Resolve.exe`
- Adobe Photoshop → `Photoshop.exe`
- Adobe InDesign → `InDesign.exe`
- FL Studio (64-bit) → `FL64.exe`

## Usage notes

- Enable Windows own "Automatically hide the taskbar" setting to ensure that the taskbar is hidden on all apps. If you have this setting off then the default Windows behaviour is to reserve space at the bottom of the screen in order to render the taskbar and you could be left with a unsuable gap.
- The mod checks the running/focused process a few times per second, so there's a short delay (well under a second) when switching apps before the hide state updates. The hover-blocking itself is instant, since it intercepts the show attempt directly rather than reacting after the fact.

## Known limitations

- Some applications use secondary top-level windows for palettes, popped-out panels, or plugin editors, which can briefly count as "not focused" if focus mode is enabled. If you notice the taskbar flickering back while you're clearly still using the app, this is the likely cause — switching off "Require focus" avoids it entirely.
- Since this hooks common `user32.dll` functions process-wide within `explorer.exe`, it's possible (though not observed in testing) that it could interact with other shell features that try to show or focus the taskbar for legitimate reasons (e.g. notification flyouts). If you notice odd behavior elsewhere in the shell while the mod is active, please open an issue.

## How it works (technical)

The mod runs inside `explorer.exe` (as all Windhawk mods targeting shell behavior do) and:

1. Polls the running process list (or current foreground window) to determine if a target app is running/focused.
2. Sets a shared "should be hidden" flag based on that check.
3. Hooks `ShowWindow`, `SetWindowPos`, and `SetForegroundWindow` so that, whenever the taskbar window (`Shell_TrayWnd`) is the target of a show/activate/reveal call while the flag is set, the call is silently swallowed instead of passed through.
4. When the condition ends, hiding is lifted and the taskbar is shown and the work area restored normally.

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
