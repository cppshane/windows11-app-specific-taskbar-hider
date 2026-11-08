// ==WindhawkMod==
// @id              windows11-app-specific-taskbar-hider
// @name            Windows 11 Keyboard-Only Taskbar Hider
// @description     Always hides the taskbar and blocks mouse hover/edge reveal. Press the Windows key to show it.
// @version         2.0
// @author          KWWDCoding
// @github          https://github.com/kwwdcoding/windows11-app-specific-taskbar-hider
// @homepage        https://www.kwwd.co.uk
// @include         explorer.exe
// @architecture    x86-64
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Windows 11 Keyboard-Only Taskbar Hider

Hides the Windows 11 taskbar for **every application**, all the time, and
blocks Windows' hover/edge auto-reveal. Moving the pointer to the screen edge
does nothing. The taskbar is shown only when you press the Windows key (or
Ctrl+Esc, the same Start shortcut).

The bar stays up while Start or Search is open, while the Windows key is held,
and while the pointer is over the taskbar (so you can click pinned apps). It
hides again shortly after you leave it.

## How it works

The shell brings the taskbar back by calling `ShowWindow`, `SetWindowPos`, and
`SetForegroundWindow` on `Shell_TrayWnd`. This mod hooks those calls inside
`explorer.exe` and rejects every show attempt that did not come from the
Windows key. The window is never allowed to appear for a hover, so the edge
of the screen cannot pop it up or steal focus.

Pressing the Windows key on its own (a tap, not Win+another key) shows the
taskbar again without activating it, so Start keeps focus. The primary monitor's
work area is expanded to the full screen while the mod is hiding the bar, and
restored when the mod is turned off.

## Usage

1. Leave Windows' own "Automatically hide the taskbar" **off**. This mod hides
   the bar itself. Leaving the Windows setting on makes the two fight, and a
   thin auto-hide sliver can flash at the edge.
2. Enable the mod. The taskbar hides for every app. There is no process list.
3. Press the Windows key to show the taskbar and open Start. Ctrl+Esc does the
   same thing. Win+E, Win+Tab, and other chords do not show it.
4. Click a taskbar button, or dismiss Start. Once Start/Search is gone and the
   pointer is no longer on the taskbar, it hides again.

The master switch in settings turns this off without uninstalling the mod.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- hideTaskbar: true
  $name: Hide the taskbar
  $description: >-
    When enabled, the taskbar stays hidden for every application and mouse
    hover at the screen edge cannot reveal it. Press the Windows key to show
    it. Turn this off to restore normal taskbar behavior.
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <windhawk_utils.h>

#include <atomic>

// How long the bar stays up after a Windows-key tap so Start has time to take
// focus, and how long it lingers after the last real reason to stay up.
constexpr ULONGLONG kRevealGraceMs = 600;
constexpr ULONGLONG kLingerMs = 400;

std::atomic<bool> g_masterEnabled{false};
std::atomic<bool> g_forceHidden{false};
std::atomic<bool> g_revealedByKeyboard{false};
std::atomic<bool> g_revealRequested{false};
std::atomic<bool> g_stop{false};
std::atomic<int> g_winDownCount{0};
std::atomic<DWORD> g_keyboardThreadId{0};
std::atomic<bool> g_keyboardHookInstalled{false};

// Written on the worker thread only.
bool g_inUpdate = false;
ULONGLONG g_revealTick = 0;
ULONGLONG g_lastInteractionTick = 0;
bool g_savedWorkArea = false;
bool g_workAreaExpanded = false;
RECT g_originalWorkArea{};
bool g_fallbackWinDown = false;
bool g_fallbackCtrlEsc = false;
bool g_loggedAsHidden = false;

HANDLE g_wakeEvent = nullptr;
HANDLE g_workerThread = nullptr;
HANDLE g_keyboardThread = nullptr;
HHOOK g_keyboardHook = nullptr;

// Win-key edge state is touched only on the keyboard-hook thread.
bool g_lwinDown = false;
bool g_rwinDown = false;
bool g_winChord = false;

using ShowWindow_t = BOOL(WINAPI*)(HWND, int);
ShowWindow_t g_origShowWindow = nullptr;
ShowWindow_t g_origShowWindowAsync = nullptr;

using SetWindowPos_t = BOOL(WINAPI*)(HWND, HWND, int, int, int, int, UINT);
SetWindowPos_t g_origSetWindowPos = nullptr;

using SetForegroundWindow_t = BOOL(WINAPI*)(HWND);
SetForegroundWindow_t g_origSetForegroundWindow = nullptr;

// ---------------------------------------------------------------------
// Taskbar windows
// ---------------------------------------------------------------------

bool ClassIs(HWND hwnd, const wchar_t* name) {
    WCHAR cls[64];
    if (!hwnd || !GetClassNameW(hwnd, cls, ARRAYSIZE(cls))) return false;
    return _wcsicmp(cls, name) == 0;
}

bool IsTaskbarFrame(HWND hwnd) {
    return ClassIs(hwnd, L"Shell_TrayWnd") || ClassIs(hwnd, L"Shell_SecondaryTrayWnd");
}

// True for the taskbar itself, its overflow flyout, or a menu/thumbnail it owns.
bool IsTaskbarRelated(HWND hwnd) {
    if (!hwnd) return false;
    if (IsTaskbarFrame(hwnd)) return true;
    if (ClassIs(hwnd, L"NotifyIconOverflowWindow")) return true;
    if (ClassIs(hwnd, L"TopLevelWindowForOverflowXamlIsland")) return true;

    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (root && root != hwnd && IsTaskbarFrame(root)) return true;

    HWND owner = GetWindow(hwnd, GW_OWNER);
    if (owner && IsTaskbarFrame(owner)) return true;

    HWND rootOwner = GetAncestor(hwnd, GA_ROOTOWNER);
    return rootOwner && IsTaskbarFrame(rootOwner);
}

int CollectTaskbars(HWND* out, int capacity) {
    int count = 0;
    HWND primary = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (primary && count < capacity) out[count++] = primary;

    HWND after = nullptr;
    while (count < capacity) {
        HWND secondary = FindWindowExW(nullptr, after, L"Shell_SecondaryTrayWnd", nullptr);
        if (!secondary || secondary == after || secondary == primary) break;
        out[count++] = secondary;
        after = secondary;
    }
    return count;
}

bool IsShowCommand(int nCmdShow) {
    switch (nCmdShow) {
        case SW_SHOW:
        case SW_SHOWNA:
        case SW_SHOWNORMAL:
        case SW_SHOWNOACTIVATE:
        case SW_RESTORE:
        case SW_SHOWDEFAULT:
        case SW_MAXIMIZE:
        case SW_SHOWMAXIMIZED:
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------
// Work area. SPI_SETWORKAREA only affects the primary monitor.
// ---------------------------------------------------------------------

void RememberWorkAreaIfNeeded() {
    // Don't sample while our own full-screen override is in effect, or the
    // "original" we restore on disable would be the expanded rect.
    if (g_savedWorkArea || g_workAreaExpanded) return;
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &g_originalWorkArea, 0)) {
        g_savedWorkArea = true;
    }
}

void ForceFullWorkArea() {
    // Capture the user's work area the first time we are about to replace it,
    // which is after the tray window exists (so Explorer has already inset it).
    RememberWorkAreaIfNeeded();

    MONITORINFO mi = {sizeof(mi)};
    HMONITOR monitor = MonitorFromWindow(nullptr, MONITOR_DEFAULTTOPRIMARY);
    if (!GetMonitorInfo(monitor, &mi)) return;

    RECT current{};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &current, 0) &&
        EqualRect(&current, &mi.rcMonitor)) {
        g_workAreaExpanded = true;
        return;
    }

    RECT full = mi.rcMonitor;
    if (SystemParametersInfoW(SPI_SETWORKAREA, 0, &full, SPIF_SENDCHANGE)) {
        g_workAreaExpanded = true;
    }
}

void RestoreWorkArea() {
    if (!g_workAreaExpanded || !g_savedWorkArea) {
        g_workAreaExpanded = false;
        return;
    }

    MONITORINFO mi = {sizeof(mi)};
    HMONITOR monitor = MonitorFromWindow(nullptr, MONITOR_DEFAULTTOPRIMARY);
    // A saved full-screen rect means we never saw the taskbar inset (auto-hide
    // was already on, or we sampled too early). Showing the bar lets Explorer
    // reserve the strip itself; writing the full rect back would undo that.
    bool savedIsFull = GetMonitorInfo(monitor, &mi) &&
                       EqualRect(&g_originalWorkArea, &mi.rcMonitor);
    if (savedIsFull) {
        g_workAreaExpanded = false;
        g_savedWorkArea = false;
        return;
    }

    RECT current{};
    bool alreadyRestored = SystemParametersInfoW(SPI_GETWORKAREA, 0, &current, 0) &&
                           EqualRect(&current, &g_originalWorkArea);
    if (alreadyRestored ||
        SystemParametersInfoW(SPI_SETWORKAREA, 0, &g_originalWorkArea, SPIF_SENDCHANGE)) {
        // Drop the sample so the next enable picks up a moved taskbar.
        g_workAreaExpanded = false;
        g_savedWorkArea = false;
    }
}

// ---------------------------------------------------------------------
// When to keep a keyboard reveal on screen
// ---------------------------------------------------------------------

// Worker thread only. Caches the last pid so a hover poll does not reopen
// the same process twice.
bool ProcessImageEndsWith(DWORD pid, const wchar_t* exeName) {
    if (!pid || !exeName) return false;

    static DWORD cachedPid = 0;
    static WCHAR cachedBase[MAX_PATH] = {};

    if (pid != cachedPid) {
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process) return false;

        WCHAR full[MAX_PATH];
        DWORD size = MAX_PATH;
        cachedBase[0] = L'\0';
        if (QueryFullProcessImageNameW(process, 0, full, &size)) {
            const WCHAR* base = wcsrchr(full, L'\\');
            lstrcpynW(cachedBase, base ? base + 1 : full, MAX_PATH);
        }
        CloseHandle(process);
        cachedPid = pid;
    }

    return _wcsicmp(cachedBase, exeName) == 0;
}

bool IsLauncherProcess(DWORD pid) {
    return ProcessImageEndsWith(pid, L"StartMenuExperienceHost.exe") ||
           ProcessImageEndsWith(pid, L"SearchHost.exe");
}

bool IsLauncherWindow(HWND hwnd) {
    if (!hwnd) return false;
    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (!root) root = hwnd;

    DWORD pid = 0;
    GetWindowThreadProcessId(root, &pid);
    return IsLauncherProcess(pid);
}

bool IsWinPhysicallyDown() {
    if (g_winDownCount.load(std::memory_order_acquire) > 0) return true;
    if (g_keyboardHookInstalled.load(std::memory_order_acquire)) return false;
    return (GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000);
}

bool ShouldKeepRevealed() {
    ULONGLONG now = GetTickCount64();

    bool hardReason = IsWinPhysicallyDown();
    if (!hardReason) hardReason = IsLauncherWindow(GetForegroundWindow());

    if (!hardReason) {
        POINT pt;
        if (GetCursorPos(&pt)) {
            HWND under = WindowFromPoint(pt);
            hardReason = IsTaskbarRelated(under) || IsLauncherWindow(under);
        }
    }

    if (!hardReason) hardReason = IsTaskbarRelated(GetForegroundWindow());

    if (hardReason) {
        g_lastInteractionTick = now;
        return true;
    }
    if (now - g_revealTick < kRevealGraceMs) return true;
    if (now - g_lastInteractionTick < kLingerMs) return true;
    return false;
}

// ---------------------------------------------------------------------
// Apply visibility. Flags are updated by the caller before this runs, so
// nested shell calls made from inside ShowWindow hit the hooks and obey them.
// ---------------------------------------------------------------------

int SetTaskbarsHidden(bool hide, bool activateOnShow) {
    HWND bars[16];
    int count = CollectTaskbars(bars, ARRAYSIZE(bars));
    int cmd = hide ? SW_HIDE : (activateOnShow ? SW_SHOW : SW_SHOWNOACTIVATE);

    for (int i = 0; i < count; i++) {
        if (!IsWindow(bars[i])) continue;
        bool visible = IsWindowVisible(bars[i]) != FALSE;
        if (hide) {
            if (visible) ShowWindow(bars[i], SW_HIDE);
        } else if (!visible) {
            ShowWindow(bars[i], cmd);
        }
    }
    return count;
}

void UpdateTaskbarState() {
    if (g_inUpdate) return;
    g_inUpdate = true;

    bool freshReveal = g_revealRequested.exchange(false, std::memory_order_acq_rel);
    if (freshReveal) {
        ULONGLONG now = GetTickCount64();
        g_revealTick = now;
        g_lastInteractionTick = now;
        g_revealedByKeyboard.store(true, std::memory_order_release);
    }

    if (!g_masterEnabled.load(std::memory_order_acquire)) {
        g_forceHidden.store(false, std::memory_order_release);
        g_revealedByKeyboard.store(false, std::memory_order_release);
        if (g_loggedAsHidden || g_workAreaExpanded) {
            Wh_Log(L"Mod inactive, restoring taskbar");
        }
        g_loggedAsHidden = false;
        SetTaskbarsHidden(false, true);
        RestoreWorkArea();
        g_inUpdate = false;
        return;
    }

    // Don't arm the show-block until a tray window exists. Blocking ShowWindow
    // while Explorer is still creating the taskbar can leave it half-initialized.
    if (!FindWindowW(L"Shell_TrayWnd", nullptr) &&
        !FindWindowW(L"Shell_SecondaryTrayWnd", nullptr)) {
        // Leave show calls alone so a tray that is still being created can appear.
        g_forceHidden.store(false, std::memory_order_release);
        g_inUpdate = false;
        return;
    }

    bool keep = freshReveal ||
                (g_revealedByKeyboard.load(std::memory_order_acquire) && ShouldKeepRevealed());
    if (keep) {
        g_forceHidden.store(false, std::memory_order_release);
        g_revealedByKeyboard.store(true, std::memory_order_release);
        if (g_loggedAsHidden) Wh_Log(L"Showing taskbar for Windows key");
        g_loggedAsHidden = false;
        int count = SetTaskbarsHidden(false, false);
        if (count > 0) {
            ForceFullWorkArea();
            // A work-area change can make the shell hide the bar again.
            SetTaskbarsHidden(false, false);
        }
        g_inUpdate = false;
        return;
    }

    g_revealedByKeyboard.store(false, std::memory_order_release);
    g_forceHidden.store(true, std::memory_order_release);
    // Sample the inset BEFORE SW_HIDE. Hiding the bar makes Explorer drop
    // the inset, and sampling afterwards would restore a full-screen rect.
    if (FindWindowW(L"Shell_TrayWnd", nullptr) ||
        FindWindowW(L"Shell_SecondaryTrayWnd", nullptr)) {
        RememberWorkAreaIfNeeded();
    }
    int count = SetTaskbarsHidden(true, false);
    if (count > 0) {
        if (!g_loggedAsHidden) Wh_Log(L"Hiding taskbar");
        g_loggedAsHidden = true;
        ForceFullWorkArea();
    }
    g_inUpdate = false;
}

// ---------------------------------------------------------------------
// API hooks
// ---------------------------------------------------------------------

bool ShouldBlockTaskbarShow(HWND hwnd, int nCmdShow) {
    if (!IsShowCommand(nCmdShow) || !IsTaskbarFrame(hwnd)) return false;
    if (g_revealedByKeyboard.load(std::memory_order_acquire)) return false;
    return g_forceHidden.load(std::memory_order_acquire);
}

BOOL WINAPI Hook_ShowWindow(HWND hwnd, int nCmdShow) {
    if (ShouldBlockTaskbarShow(hwnd, nCmdShow)) return TRUE;
    if (nCmdShow == SW_HIDE && IsTaskbarFrame(hwnd) &&
        g_revealedByKeyboard.load(std::memory_order_acquire)) {
        return TRUE;
    }
    return g_origShowWindow(hwnd, nCmdShow);
}

BOOL WINAPI Hook_ShowWindowAsync(HWND hwnd, int nCmdShow) {
    if (ShouldBlockTaskbarShow(hwnd, nCmdShow)) return TRUE;
    if (nCmdShow == SW_HIDE && IsTaskbarFrame(hwnd) &&
        g_revealedByKeyboard.load(std::memory_order_acquire)) {
        return TRUE;
    }
    return g_origShowWindowAsync(hwnd, nCmdShow);
}

BOOL WINAPI Hook_SetWindowPos(HWND hwnd, HWND insertAfter, int x, int y,
                              int cx, int cy, UINT flags) {
    if (IsTaskbarFrame(hwnd)) {
        if (g_revealedByKeyboard.load(std::memory_order_acquire)) {
            flags &= ~SWP_HIDEWINDOW;
        } else if (g_forceHidden.load(std::memory_order_acquire)) {
            // Keep the bar hidden without letting a hover slide it off-screen,
            // or the next Windows-key show would come back in the wrong place.
            flags &= ~SWP_SHOWWINDOW;
            flags |= SWP_NOACTIVATE | SWP_HIDEWINDOW | SWP_NOMOVE;
        }
    }
    return g_origSetWindowPos(hwnd, insertAfter, x, y, cx, cy, flags);
}

BOOL WINAPI Hook_SetForegroundWindow(HWND hwnd) {
    if (IsTaskbarFrame(hwnd) &&
        g_forceHidden.load(std::memory_order_acquire) &&
        !g_revealedByKeyboard.load(std::memory_order_acquire)) {
        return FALSE;
    }
    return g_origSetForegroundWindow(hwnd);
}

bool InstallHooks() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return false;

    void* show = (void*)GetProcAddress(user32, "ShowWindow");
    void* showAsync = (void*)GetProcAddress(user32, "ShowWindowAsync");
    void* setPos = (void*)GetProcAddress(user32, "SetWindowPos");
    void* setForeground = (void*)GetProcAddress(user32, "SetForegroundWindow");

    if (!show || !Wh_SetFunctionHook(show, (void*)Hook_ShowWindow, (void**)&g_origShowWindow)) {
        return false;
    }
    if (showAsync &&
        !Wh_SetFunctionHook(showAsync, (void*)Hook_ShowWindowAsync, (void**)&g_origShowWindowAsync)) {
        return false;
    }
    if (!setPos || !Wh_SetFunctionHook(setPos, (void*)Hook_SetWindowPos, (void**)&g_origSetWindowPos)) {
        return false;
    }
    if (!setForeground ||
        !Wh_SetFunctionHook(setForeground, (void*)Hook_SetForegroundWindow,
                            (void**)&g_origSetForegroundWindow)) {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------
// Keyboard hook. Runs on its own thread so a slow show/hide cannot trip
// the system-wide low-level hook timeout and freeze the keyboard.
// A plain Windows-key tap (key up with no chord) requests a reveal.
// Win+E and other chords do not.
// ---------------------------------------------------------------------

void RequestReveal() {
    g_revealRequested.store(true, std::memory_order_release);
    if (g_wakeEvent) SetEvent(g_wakeEvent);
}

void NoteWinKey(bool left, bool down) {
    bool& flag = left ? g_lwinDown : g_rwinDown;
    if (flag == down) return;

    if (down) {
        if (!g_lwinDown && !g_rwinDown) g_winChord = false;
        flag = true;
        g_winDownCount.fetch_add(1, std::memory_order_release);
        return;
    }

    flag = false;
    int remaining = g_winDownCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (remaining < 0) {
        g_winDownCount.store(0, std::memory_order_release);
        remaining = 0;
    }
    if (remaining == 0 && !g_winChord) RequestReveal();
}

LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && !g_stop.load(std::memory_order_acquire)) {
        (void)wParam;
        const auto* info = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        UINT vk = info->vkCode;
        bool up = (info->flags & LLKHF_UP) != 0;

        if (vk == VK_LWIN || vk == VK_RWIN) {
            NoteWinKey(vk == VK_LWIN, !up);
        } else if (!up && vk == VK_ESCAPE && (GetAsyncKeyState(VK_CONTROL) & 0x8000)) {
            RequestReveal();
        } else if (!up && (g_lwinDown || g_rwinDown)) {
            g_winChord = true;
        }
    }
    return CallNextHookEx(g_keyboardHook, code, wParam, lParam);
}

void PollFallbackWinKey() {
    // The hook sees taps the poll would miss. This path is only for a failed hook.
    if (g_keyboardHookInstalled.load(std::memory_order_acquire)) return;

    bool down = (GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000);
    if (g_fallbackWinDown && !down) RequestReveal();
    g_fallbackWinDown = down;

    bool ctrlEsc = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_ESCAPE) & 0x8000);
    if (ctrlEsc && !g_fallbackCtrlEsc) RequestReveal();
    g_fallbackCtrlEsc = ctrlEsc;
}

DWORD WINAPI WorkerThreadProc(LPVOID) {
    while (!g_stop.load(std::memory_order_acquire)) {
        PollFallbackWinKey();
        UpdateTaskbarState();
        if (g_stop.load(std::memory_order_acquire)) break;
        WaitForSingleObject(g_wakeEvent, 100);
    }
    return 0;
}

DWORD WINAPI KeyboardThreadProc(LPVOID) {
    MSG msg;
    PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);
    g_keyboardThreadId.store(GetCurrentThreadId(), std::memory_order_release);

    if (!g_stop.load(std::memory_order_acquire)) {
        g_keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, nullptr, 0);
    }
    if (g_keyboardHook) {
        g_keyboardHookInstalled.store(true, std::memory_order_release);
    } else {
        Wh_Log(L"SetWindowsHookEx failed: %u", GetLastError());
    }

    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        DispatchMessageW(&msg);
    }

    g_keyboardHookInstalled.store(false, std::memory_order_release);
    if (g_keyboardHook) {
        UnhookWindowsHookEx(g_keyboardHook);
        g_keyboardHook = nullptr;
    }
    g_keyboardThreadId.store(0, std::memory_order_release);
    return 0;
}

void StopThreads() {
    g_stop.store(true, std::memory_order_release);
    if (g_wakeEvent) SetEvent(g_wakeEvent);

    if (g_workerThread) {
        WaitForSingleObject(g_workerThread, 5000);
        CloseHandle(g_workerThread);
        g_workerThread = nullptr;
    }

    if (g_keyboardThread) {
        for (int i = 0; i < 50; i++) {
            DWORD threadId = g_keyboardThreadId.load(std::memory_order_acquire);
            if (threadId && PostThreadMessageW(threadId, WM_QUIT, 0, 0)) break;
            Sleep(10);
        }
        WaitForSingleObject(g_keyboardThread, 5000);
        CloseHandle(g_keyboardThread);
        g_keyboardThread = nullptr;
    }
}

void LoadSettings() {
    g_masterEnabled.store(Wh_GetIntSetting(L"hideTaskbar") != 0, std::memory_order_release);
}

void RestoreTaskbarAndWorkArea() {
    g_revealedByKeyboard.store(false, std::memory_order_release);
    g_forceHidden.store(false, std::memory_order_release);
    g_loggedAsHidden = false;
    SetTaskbarsHidden(false, true);
    RestoreWorkArea();
}

BOOL Wh_ModInit() {
    Wh_Log(L"Initializing keyboard-only taskbar hider");

    LoadSettings();

    if (!InstallHooks()) {
        Wh_Log(L"Failed to install taskbar hooks");
        return FALSE;
    }

    // Hooks are applied by the engine after Wh_ModInit returns. Threads start
    // in Wh_ModAfterInit so ShowWindow is already detoured before the first hide.
    g_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_wakeEvent) return FALSE;

    return TRUE;
}

void Wh_ModAfterInit() {
    g_workerThread = CreateThread(nullptr, 0, WorkerThreadProc, nullptr, 0, nullptr);
    g_keyboardThread = CreateThread(nullptr, 0, KeyboardThreadProc, nullptr, 0, nullptr);
    if (!g_workerThread || !g_keyboardThread) {
        Wh_Log(L"Failed to start taskbar threads");
        StopThreads();
        RestoreTaskbarAndWorkArea();
    }
}

void Wh_ModBeforeUninit() {
    // Stop threads before the engine removes hooks. The worker calls
    // ShowWindow, and that has to finish while the detour is still installed.
    StopThreads();
    RestoreTaskbarAndWorkArea();
}

void Wh_ModUninit() {
    Wh_Log(L"Uninitializing keyboard-only taskbar hider");

    if (g_wakeEvent) {
        CloseHandle(g_wakeEvent);
        g_wakeEvent = nullptr;
    }
}

void Wh_ModSettingsChanged() {
    Wh_Log(L"Settings changed");
    LoadSettings();
    if (g_wakeEvent) SetEvent(g_wakeEvent);
}
