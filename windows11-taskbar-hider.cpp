// ==WindhawkMod==
// @id              windows11-app-specific-taskbar-hider
// @name            Windows 11 Keyboard-Only Taskbar Hider
// @description     Always hides the taskbar and blocks mouse hover/edge reveal. Press the Windows key to show it.
// @version         2.2
// @compilerOptions -ldwmapi -lshell32
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

The bar is shown as soon as the Windows key goes down, and it stays up for the
whole time Start or Search is open (and while the pointer is still over the
bar). It hides again shortly after Start closes.

## How it works

On Windows 11 the pictures on the taskbar come from the shell's expand/collapse
state. Hiding `Shell_TrayWnd` with `ShowWindow` leaves that state collapsed, so
the bar stays invisible the whole time Start is open and only flashes when
Start closes. This mod does not do that.

While the mod is on it turns on the shell's own auto-hide (and puts the
previous setting back when the mod unloads), then blocks only the mouse unhide
path: the tray unhide timer, and the Win11 pointer-over / screen-edge expand.
The Windows key is left alone, so the shell runs its real unhide, the same one
that stays up for the whole time Start is open. The collapsed bar is cloaked so
the thin auto-hide sliver does not show.

The primary monitor's work area is expanded to the full screen while the mod
is hiding the bar, and restored when the mod is turned off.

## Usage

1. Leave Windows' own "Automatically hide the taskbar" **off**. The mod turns
   that behavior on while it is loaded and writes the previous state back when
   it unloads or you disable the switch.
2. Enable the mod. The taskbar hides for every app. There is no process list.
3. Press the Windows key. The taskbar appears with Start and stays visible
   while Start is open. Ctrl+Esc does the same thing.
4. Press the Windows key again, or click away, to close Start. The taskbar
   hides shortly after Start is gone, unless the pointer is still on it.

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
#include <dwmapi.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <windhawk_utils.h>

#include <atomic>

#ifndef DWMWA_CLOAK
#define DWMWA_CLOAK 13
#endif
#ifndef DWMWA_CLOAKED
#define DWMWA_CLOAKED 14
#endif

// TrayUI timer ids. 3 is the mouse unhide; the Windows key does not use it.
constexpr UINT_PTR kTrayTimerHide = 2;
constexpr UINT_PTR kTrayTimerUnhide = 3;
// ViewCoordinator::UpdateIsExpanded reasons that mean "the pointer did this".
constexpr int kReasonPointerOver = 7;
constexpr int kReasonEdgeStroke = 8;

// How long to keep the bar up while waiting for Start to actually open, and
// how long it lingers after Start has closed.
constexpr ULONGLONG kStartOpenGraceMs = 1500;
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
bool g_sawLauncher = false;
ULONGLONG g_revealTick = 0;
ULONGLONG g_lastInteractionTick = 0;

struct TaskbarPlacement {
    HWND hwnd;
    RECT rc;
};
TaskbarPlacement g_placement[16];
int g_placementCount = 0;
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

using SetTimer_t = UINT_PTR(WINAPI*)(HWND, UINT_PTR, UINT, TIMERPROC);
SetTimer_t g_origSetTimer = nullptr;

using DwmSetWindowAttribute_t = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
DwmSetWindowAttribute_t g_origDwmSetWindowAttribute = nullptr;

using LoadLibraryExW_t = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);
LoadLibraryExW_t g_origLoadLibraryExW = nullptr;

// ViewCoordinator lives in Taskbar.View.dll / ExplorerExtensions.dll.
// Calling the originals bypasses our mouse block.
using ViewIsExpanded_t = bool(WINAPI*)(void* pThis, HWND hwnd);
ViewIsExpanded_t g_origIsExpanded = nullptr;

using ViewPointerOver_t = void(WINAPI*)(void* pThis, HWND hwnd, bool over, int device);
ViewPointerOver_t g_origPointerOver = nullptr;

using ViewShouldExpand_t = bool(WINAPI*)(void* pThis, HWND hwnd, bool expanded);
ViewShouldExpand_t g_origShouldExpand = nullptr;

using ViewUpdateExpanded_t = void(WINAPI*)(void* pThis, HWND hwnd, int reason);
ViewUpdateExpanded_t g_origUpdateExpanded = nullptr;

// Set on the keyboard hook before the shell sees the Windows key, and held
// by the worker until Start has closed. View hooks read it.
std::atomic<bool> g_suppressReveal{false};
std::atomic<bool> g_controlExpand{false};
std::atomic<bool> g_afterInit{false};

// "this" captured from the view hooks. Touched under g_coordLock.
struct CoordinatorSlot {
    HWND hwnd;
    void* pThis;
};
CoordinatorSlot g_coordinators[8];
int g_coordinatorCount = 0;
CRITICAL_SECTION g_coordLock;
bool g_coordLockReady = false;

bool g_autoHideSaved = false;
UINT g_originalAppBarState = 0;
bool g_viewHooksReady = false;
HMODULE g_viewFailedModule = nullptr;

// Nonzero while this mod is inside the shell expand/collapse call. A mouse
// unhide that arrives on its own is blocked; one we requested is not.
std::atomic<int> g_shellDriveDepth{0};

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

bool RectIsOnScreen(const RECT& rc) {
    RECT inter{};
    HMONITOR monitor = MonitorFromRect(&rc, MONITOR_DEFAULTTONULL);
    if (!monitor) return false;
    MONITORINFO mi = {sizeof(mi)};
    if (!GetMonitorInfo(monitor, &mi)) return false;
    if (!IntersectRect(&inter, &rc, &mi.rcMonitor)) return false;
    return (inter.right - inter.left) >= 40 && (inter.bottom - inter.top) >= 16;
}

bool IsShownSurface(HWND hwnd) {
    if (!IsWindow(hwnd)) return false;

    DWORD cloaked = 0;
    HRESULT hr = DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (SUCCEEDED(hr)) {
        if (cloaked) return false;
    } else if (!IsWindowVisible(hwnd)) {
        return false;
    }

    RECT rc{};
    if (!GetWindowRect(hwnd, &rc)) return false;
    return RectIsOnScreen(rc);
}

bool IsLauncherSurfaceClass(HWND hwnd) {
    return ClassIs(hwnd, L"Windows.UI.Core.CoreWindow") ||
           ClassIs(hwnd, L"Windows.UI.Composition.DesktopWindowContentBridge") ||
           ClassIs(hwnd, L"XamlExplorerHostIslandWindow");
}

struct LauncherEnumCtx {
    DWORD startPid;
    DWORD searchPid;
    bool found;
};

bool IsLargeOnScreen(HWND hwnd) {
    if (!IsShownSurface(hwnd)) return false;
    RECT rc{};
    if (!GetWindowRect(hwnd, &rc)) return false;
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;
    return width >= 160 && height >= 160;
}

BOOL CALLBACK EnumLauncherWindow(HWND hwnd, LPARAM param) {
    auto* ctx = reinterpret_cast<LauncherEnumCtx*>(param);
    if (ctx->found) return FALSE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != ctx->startPid && pid != ctx->searchPid) return TRUE;

    // Search keeps the class filter. Start also counts any large uncloaked
    // window, because newer builds do not use CoreWindow for it.
    if (pid == ctx->startPid) {
        bool matched = (IsLauncherSurfaceClass(hwnd) && IsShownSurface(hwnd)) ||
                       IsLargeOnScreen(hwnd);
        if (!matched) return TRUE;
    } else if (!(IsLauncherSurfaceClass(hwnd) && IsShownSurface(hwnd))) {
        return TRUE;
    }

    ctx->found = true;
    return FALSE;
}

void FindLauncherPids(DWORD* startPid, DWORD* searchPid) {
    *startPid = 0;
    *searchPid = 0;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W entry = {sizeof(entry)};
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (!*startPid && _wcsicmp(entry.szExeFile, L"StartMenuExperienceHost.exe") == 0) {
                *startPid = entry.th32ProcessID;
            } else if (!*searchPid && _wcsicmp(entry.szExeFile, L"SearchHost.exe") == 0) {
                *searchPid = entry.th32ProcessID;
            }
        } while (Process32NextW(snapshot, &entry) && (!*startPid || !*searchPid));
    }
    CloseHandle(snapshot);
}

// Start's window is often not the foreground window, and IsWindowVisible stays
// true after it closes. An uncloaked, on-screen surface is the open menu.
bool IsLauncherActive() {
    if (IsLauncherWindow(GetForegroundWindow())) return true;

    POINT pt{};
    if (GetCursorPos(&pt)) {
        if (IsLauncherWindow(WindowFromPoint(pt))) return true;
    }

    const wchar_t* titles[] = {L"Start", L"Search"};
    for (const wchar_t* title : titles) {
        HWND hwnd = FindWindowW(L"Windows.UI.Core.CoreWindow", title);
        if (IsShownSurface(hwnd)) return true;
    }

    DWORD startPid = 0;
    DWORD searchPid = 0;
    FindLauncherPids(&startPid, &searchPid);
    if (!startPid && !searchPid) return false;

    LauncherEnumCtx ctx{startPid, searchPid, false};
    EnumWindows(EnumLauncherWindow, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

bool IsWinPhysicallyDown() {
    if (g_winDownCount.load(std::memory_order_acquire) > 0) return true;
    if (g_keyboardHookInstalled.load(std::memory_order_acquire)) return false;
    return (GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000);
}

bool IsPointerOverTaskbar() {
    if (IsTaskbarRelated(GetForegroundWindow())) return true;
    POINT pt{};
    if (!GetCursorPos(&pt)) return false;
    return IsTaskbarRelated(WindowFromPoint(pt));
}

bool ShouldKeepRevealed() {
    ULONGLONG now = GetTickCount64();
    bool launcher = IsLauncherActive();
    if (launcher) {
        if (!g_sawLauncher) Wh_Log(L"Start menu is open, holding taskbar");
        g_sawLauncher = true;
        g_lastInteractionTick = now;
        return true;
    }

    // Win+E and the other chords arm a reveal on the Windows key going down.
    // Drop it once another key makes it a chord, unless Start really opened.
    if (g_suppressReveal.load(std::memory_order_acquire)) return false;

    if (IsWinPhysicallyDown() || IsPointerOverTaskbar()) {
        g_lastInteractionTick = now;
        return true;
    }

    // Start was open and just closed (second Windows-key press). Stay up briefly.
    if (g_sawLauncher) return now - g_lastInteractionTick < kLingerMs;

    // Start has not appeared yet. Don't hide during the open animation.
    return now - g_revealTick < kStartOpenGraceMs;
}

// ---------------------------------------------------------------------
// Apply visibility. Flags are updated by the caller before this runs, so
// nested shell calls made from inside ShowWindow hit the hooks and obey them.
// ---------------------------------------------------------------------

void RememberPlacementIfOnScreen(HWND hwnd) {
    RECT rc{};
    if (!GetWindowRect(hwnd, &rc) || !RectIsOnScreen(rc)) return;

    for (int i = 0; i < g_placementCount; i++) {
        if (g_placement[i].hwnd == hwnd) {
            g_placement[i].rc = rc;
            return;
        }
    }
    if (g_placementCount < ARRAYSIZE(g_placement)) {
        g_placement[g_placementCount].hwnd = hwnd;
        g_placement[g_placementCount].rc = rc;
        g_placementCount++;
    }
}

bool LookupPlacement(HWND hwnd, RECT* out) {
    for (int i = 0; i < g_placementCount; i++) {
        if (g_placement[i].hwnd == hwnd) {
            *out = g_placement[i].rc;
            return true;
        }
    }
    return false;
}

void RestorePlacement(HWND hwnd) {
    RECT want{};
    if (!LookupPlacement(hwnd, &want)) {
        // No saved on-screen rect yet. Dock a normal-height bar to the bottom.
        HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi = {sizeof(mi)};
        if (!GetMonitorInfo(monitor, &mi)) return;
        int height = 48;
        want.left = mi.rcMonitor.left;
        want.right = mi.rcMonitor.right;
        want.bottom = mi.rcMonitor.bottom;
        want.top = want.bottom - height;
    }

    int width = want.right - want.left;
    int height = want.bottom - want.top;
    if (width < 40 || height < 16) return;

    // Bypass the hook so this snap is not treated as a shell slide.
    if (g_origSetWindowPos) {
        g_origSetWindowPos(hwnd, nullptr, want.left, want.top, width, height,
                           SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
    }
}

int SetTaskbarsHidden(bool hide, bool activateOnShow) {
    HWND bars[16];
    int count = CollectTaskbars(bars, ARRAYSIZE(bars));
    int cmd = hide ? SW_HIDE : (activateOnShow ? SW_SHOW : SW_SHOWNOACTIVATE);

    for (int i = 0; i < count; i++) {
        if (!IsWindow(bars[i])) continue;
        if (hide) RememberPlacementIfOnScreen(bars[i]);
        bool visible = IsWindowVisible(bars[i]) != FALSE;
        if (hide) {
            if (visible) ShowWindow(bars[i], SW_HIDE);
        } else if (!visible) {
            ShowWindow(bars[i], cmd);
        }
    }
    return count;
}

void CloakWindowRaw(HWND hwnd, BOOL cloak) {
    if (!IsWindow(hwnd)) return;
    if (g_origDwmSetWindowAttribute) {
        g_origDwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
    } else {
        DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
    }
}

bool IsCloaked(HWND hwnd) {
    DWORD cloaked = 0;
    HRESULT hr = DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    return SUCCEEDED(hr) && cloaked != 0;
}

void RememberCoordinator(HWND hwnd, void* pThis) {
    if (!hwnd || !pThis || !g_coordLockReady) return;
    EnterCriticalSection(&g_coordLock);
    for (int i = 0; i < g_coordinatorCount; i++) {
        if (g_coordinators[i].hwnd == hwnd) {
            g_coordinators[i].pThis = pThis;
            LeaveCriticalSection(&g_coordLock);
            return;
        }
    }
    if (g_coordinatorCount < ARRAYSIZE(g_coordinators)) {
        g_coordinators[g_coordinatorCount].hwnd = hwnd;
        g_coordinators[g_coordinatorCount].pThis = pThis;
        g_coordinatorCount++;
    }
    LeaveCriticalSection(&g_coordLock);
}

void CopyCoordinators(CoordinatorSlot* out, int* count) {
    *count = 0;
    if (!g_coordLockReady) return;
    EnterCriticalSection(&g_coordLock);
    *count = g_coordinatorCount;
    for (int i = 0; i < g_coordinatorCount; i++) out[i] = g_coordinators[i];
    LeaveCriticalSection(&g_coordLock);
}

// Ask the shell to expand or collapse the real taskbar. ShowWindow on
// Shell_TrayWnd does not do this on Windows 11: the XAML bar stays collapsed
// while Start is open, then paints for a moment as Start closes.
void DriveTaskbar(bool expand) {
    HWND bars[16];
    int count = CollectTaskbars(bars, ARRAYSIZE(bars));

    if (expand) {
        for (int i = 0; i < count; i++) {
            HWND hwnd = bars[i];
            if (!IsWindow(hwnd)) continue;
            if (!IsWindowVisible(hwnd) && g_origShowWindow) {
                g_origShowWindow(hwnd, SW_SHOWNOACTIVATE);
            }
            RECT current{};
            bool onScreen = GetWindowRect(hwnd, &current) && RectIsOnScreen(current) &&
                            IsWindowVisible(hwnd);
            if (!onScreen) RestorePlacement(hwnd);
            CloakWindowRaw(hwnd, FALSE);
            if (g_origSetTimer) g_origSetTimer(hwnd, kTrayTimerUnhide, 0, nullptr);
        }
    }

    // A Windows key can land while a collapse is already running. Collapsing
    // after that key is what left the bar invisible for the whole time Start
    // was open.
    if (!expand && g_revealedByKeyboard.load(std::memory_order_acquire)) return;

    CoordinatorSlot slots[8];
    int slotCount = 0;
    CopyCoordinators(slots, &slotCount);
    if (g_origPointerOver && slotCount > 0) {
        g_shellDriveDepth.fetch_add(1, std::memory_order_acq_rel);
        for (int i = 0; i < slotCount; i++) {
            if (!IsWindow(slots[i].hwnd)) continue;
            if (!expand && g_revealedByKeyboard.load(std::memory_order_acquire)) {
                g_shellDriveDepth.fetch_sub(1, std::memory_order_acq_rel);
                return;
            }
            if (expand) CloakWindowRaw(slots[i].hwnd, FALSE);
            g_origPointerOver(slots[i].pThis, slots[i].hwnd, expand, 0);
        }
        g_shellDriveDepth.fetch_sub(1, std::memory_order_acq_rel);
    }

    if (!expand) {
        if (g_revealedByKeyboard.load(std::memory_order_acquire)) return;
        for (int i = 0; i < count; i++) {
            if (!IsWindow(bars[i])) continue;
            if (g_origSetTimer) g_origSetTimer(bars[i], kTrayTimerHide, 0, nullptr);
            CloakWindowRaw(bars[i], TRUE);
        }
        for (int i = 0; i < slotCount; i++) {
            if (IsWindow(slots[i].hwnd)) CloakWindowRaw(slots[i].hwnd, TRUE);
        }
    }
}

bool TaskbarNeedsExpand() {
    HWND bars[16];
    int count = CollectTaskbars(bars, ARRAYSIZE(bars));
    for (int i = 0; i < count; i++) {
        if (!IsWindow(bars[i])) continue;
        if (IsCloaked(bars[i]) || !IsWindowVisible(bars[i])) return true;
        RECT current{};
        if (!GetWindowRect(bars[i], &current) || !RectIsOnScreen(current)) return true;
    }

    CoordinatorSlot slots[8];
    int slotCount = 0;
    CopyCoordinators(slots, &slotCount);
    if (g_origIsExpanded) {
        for (int i = 0; i < slotCount; i++) {
            if (!IsWindow(slots[i].hwnd)) continue;
            if (!g_origIsExpanded(slots[i].pThis, slots[i].hwnd)) return true;
        }
    }
    return false;
}

void EnsureShellAutoHide() {
    APPBARDATA abd = {sizeof(abd)};
    abd.hWnd = FindWindowW(L"Shell_TrayWnd", nullptr);
    UINT state = (UINT)SHAppBarMessage(ABM_GETSTATE, &abd);
    if (!g_autoHideSaved) {
        g_originalAppBarState = state;
        g_autoHideSaved = true;
        Wh_Log(L"Saved taskbar state 0x%X", state);
    }
    if (state & ABS_AUTOHIDE) return;

    abd.lParam = (LPARAM)(state | ABS_AUTOHIDE);
    SHAppBarMessage(ABM_SETSTATE, &abd);
    Wh_Log(L"Turned shell auto-hide on for the keyboard unhide path");
}

void RestoreShellAutoHide() {
    if (!g_autoHideSaved) return;
    APPBARDATA abd = {sizeof(abd)};
    abd.hWnd = FindWindowW(L"Shell_TrayWnd", nullptr);
    abd.lParam = (LPARAM)(g_originalAppBarState & (ABS_AUTOHIDE | ABS_ALWAYSONTOP));
    SHAppBarMessage(ABM_SETSTATE, &abd);
    Wh_Log(L"Restored taskbar state 0x%X", (UINT)abd.lParam);
    g_autoHideSaved = false;
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
        g_controlExpand.store(false, std::memory_order_release);
        g_forceHidden.store(false, std::memory_order_release);
        g_revealedByKeyboard.store(false, std::memory_order_release);
        if (g_loggedAsHidden || g_workAreaExpanded || g_autoHideSaved) {
            Wh_Log(L"Mod inactive, restoring taskbar");
        }
        g_loggedAsHidden = false;
        g_sawLauncher = false;
        DriveTaskbar(true);
        SetTaskbarsHidden(false, true);
        RestoreShellAutoHide();
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

    // Sample the inset before auto-hide, which makes Explorer drop it.
    RememberWorkAreaIfNeeded();
    if (!g_viewHooksReady) InstallViewHooks();
    EnsureShellAutoHide();
    if (!g_workAreaExpanded) ForceFullWorkArea();
    g_controlExpand.store(true, std::memory_order_release);

    // A Win chord (Win+E and the rest) sets suppress after the key-down reveal.
    // Don't keep that reveal unless Start actually opened.
    bool chord = g_suppressReveal.load(std::memory_order_acquire) && !g_sawLauncher &&
                 !IsLauncherActive();
    bool keep = !chord &&
                (freshReveal ||
                 (g_revealedByKeyboard.load(std::memory_order_acquire) && ShouldKeepRevealed()));
    if (keep) {
        if (freshReveal && !IsLauncherActive()) g_sawLauncher = false;
        g_forceHidden.store(false, std::memory_order_release);
        g_revealedByKeyboard.store(true, std::memory_order_release);
        if (g_loggedAsHidden) Wh_Log(L"Expanding taskbar for Windows key");
        g_loggedAsHidden = false;
        if (freshReveal || TaskbarNeedsExpand()) DriveTaskbar(true);
        g_inUpdate = false;
        return;
    }

    // Win is still down on a chord (suppress is set). That must not abort the hide.
    bool winHold = IsWinPhysicallyDown() && !g_suppressReveal.load(std::memory_order_acquire);
    if (g_revealRequested.load(std::memory_order_acquire) || winHold) {
        // A Windows-key press landed while this pass was deciding to hide.
        g_inUpdate = false;
        return;
    }

    g_sawLauncher = false;
    g_revealedByKeyboard.store(false, std::memory_order_release);
    g_forceHidden.store(true, std::memory_order_release);
    winHold = IsWinPhysicallyDown() && !g_suppressReveal.load(std::memory_order_acquire);
    if (g_revealRequested.load(std::memory_order_acquire) || winHold) {
        g_revealedByKeyboard.store(true, std::memory_order_release);
        g_forceHidden.store(false, std::memory_order_release);
        g_inUpdate = false;
        return;
    }

    // Collapse and cloak. Do not SW_HIDE the tray: that leaves the XAML bar
    // collapsed, so the next Windows key cannot paint it until Start closes.
    bool anyVisible = false;
    HWND bars[16];
    int barCount = CollectTaskbars(bars, ARRAYSIZE(bars));
    for (int i = 0; i < barCount; i++) {
        if (IsWindow(bars[i]) && !IsCloaked(bars[i])) anyVisible = true;
    }
    if (!g_loggedAsHidden || anyVisible) {
        if (!g_loggedAsHidden) Wh_Log(L"Collapsing taskbar");
        g_loggedAsHidden = true;
        DriveTaskbar(false);
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

            RECT current{};
            if (GetWindowRect(hwnd, &current)) {
                int nx = (flags & SWP_NOMOVE) ? current.left : x;
                int ny = (flags & SWP_NOMOVE) ? current.top : y;
                int nw = (flags & SWP_NOSIZE) ? (current.right - current.left) : cx;
                int nh = (flags & SWP_NOSIZE) ? (current.bottom - current.top) : cy;
                RECT dest{nx, ny, nx + nw, ny + nh};
                // The shell slides the bar off the edge when Start opens. Refuse
                // that move so the Windows-key show stays on screen.
                if (!RectIsOnScreen(dest)) flags |= SWP_NOMOVE | SWP_NOSIZE;
            }
        } else if (g_forceHidden.load(std::memory_order_acquire)) {
            // Drop a hover show. Do not force SWP_HIDEWINDOW: hiding the HWND
            // is what left the XAML bar collapsed for the whole time Start was open.
            flags &= ~SWP_SHOWWINDOW;
            flags |= SWP_NOACTIVATE;
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

UINT_PTR WINAPI Hook_SetTimer(HWND hwnd, UINT_PTR id, UINT elapse, TIMERPROC proc) {
    // Swallow the mouse unhide timer. The Windows key unhides through
    // TrayUI::Unhide / ViewCoordinator, not this timer.
    bool control = g_masterEnabled.load(std::memory_order_acquire) &&
                   g_controlExpand.load(std::memory_order_acquire);
    bool revealed = g_revealedByKeyboard.load(std::memory_order_acquire);
    if (IsTaskbarFrame(hwnd) && control) {
        // Mouse unhide. The Windows key does not use this timer.
        if (id == kTrayTimerUnhide && !revealed) return id ? id : 1;
        // Don't let the shell's hide timer pull the bar down while Start is open.
        if (id == kTrayTimerHide && revealed) return id ? id : 1;
    }
    return g_origSetTimer(hwnd, id, elapse, proc);
}

bool IsTrackedTaskbar(HWND hwnd) {
    if (IsTaskbarFrame(hwnd)) return true;
    if (!hwnd || !g_coordLockReady) return false;
    bool found = false;
    EnterCriticalSection(&g_coordLock);
    for (int i = 0; i < g_coordinatorCount; i++) {
        if (g_coordinators[i].hwnd == hwnd) {
            found = true;
            break;
        }
    }
    LeaveCriticalSection(&g_coordLock);
    return found;
}

HRESULT WINAPI Hook_DwmSetWindowAttribute(HWND hwnd, DWORD attribute, LPCVOID data, DWORD size) {
    if (attribute == DWMWA_CLOAK && IsTrackedTaskbar(hwnd) && data && size >= sizeof(BOOL) &&
        g_masterEnabled.load(std::memory_order_acquire) &&
        g_controlExpand.load(std::memory_order_acquire) &&
        !g_revealedByKeyboard.load(std::memory_order_acquire) &&
        !*(const BOOL*)data) {
        return S_OK;
    }
    return g_origDwmSetWindowAttribute(hwnd, attribute, data, size);
}

void WINAPI Hook_PointerOver(void* pThis, HWND hwnd, bool over, int device) {
    RememberCoordinator(hwnd, pThis);
    g_origPointerOver(pThis, hwnd, over, device);
}

bool WINAPI Hook_ShouldExpand(void* pThis, HWND hwnd, bool expanded) {
    RememberCoordinator(hwnd, pThis);
    if (!g_masterEnabled.load(std::memory_order_acquire) ||
        !g_controlExpand.load(std::memory_order_acquire)) {
        return g_origShouldExpand(pThis, hwnd, expanded);
    }
    // Hold the bar open for the whole keyboard reveal. Otherwise refuse, so a
    // hover cannot expand it.
    if (g_revealedByKeyboard.load(std::memory_order_acquire)) return true;
    return false;
}

void WINAPI Hook_UpdateExpanded(void* pThis, HWND hwnd, int reason) {
    RememberCoordinator(hwnd, pThis);
    bool revealed = g_revealedByKeyboard.load(std::memory_order_acquire);
    bool control = g_masterEnabled.load(std::memory_order_acquire) &&
                   g_controlExpand.load(std::memory_order_acquire);
    bool mouse = reason == kReasonPointerOver || reason == kReasonEdgeStroke;
    bool fromMod = g_shellDriveDepth.load(std::memory_order_acquire) > 0;
    if (control && !revealed && mouse && !fromMod) return;

    if (control && revealed) CloakWindowRaw(hwnd, FALSE);
    g_origUpdateExpanded(pThis, hwnd, reason);

    if (!control || !g_origIsExpanded) return;
    bool isExpanded = false;
    if (g_origIsExpanded) isExpanded = g_origIsExpanded(pThis, hwnd);
    if (isExpanded || revealed) CloakWindowRaw(hwnd, FALSE);
    else CloakWindowRaw(hwnd, TRUE);
}

bool RegisterViewHooks(HMODULE module) {
    if (!module || g_viewHooksReady || module == g_viewFailedModule) return g_viewHooksReady;

    WindhawkUtils::SYMBOL_HOOK symbolHooks[] = {
        {
            {LR"(public: bool __cdecl winrt::Taskbar::implementation::ViewCoordinator::IsExpanded(unsigned __int64))"},
            &g_origIsExpanded,
            nullptr,
            true,
        },
        {
            {LR"(public: void __cdecl winrt::Taskbar::implementation::ViewCoordinator::HandleIsPointerOverTaskbarFrameChanged(unsigned __int64,bool,enum winrt::WindowsUdk::UI::Shell::InputDeviceKind))"},
            &g_origPointerOver,
            Hook_PointerOver,
            true,
        },
        {
            {LR"(public: bool __cdecl winrt::Taskbar::implementation::ViewCoordinator::ShouldTaskbarBeExpanded(unsigned __int64,bool))"},
            &g_origShouldExpand,
            Hook_ShouldExpand,
            true,
        },
        {
            {LR"(public: void __cdecl winrt::Taskbar::implementation::ViewCoordinator::UpdateIsExpanded(unsigned __int64,enum TaskbarTipTest::TaskbarExpandCollapseReason))"},
            &g_origUpdateExpanded,
            Hook_UpdateExpanded,
            true,
        },
    };

    if (!WindhawkUtils::HookSymbols(module, symbolHooks, ARRAYSIZE(symbolHooks))) {
        Wh_Log(L"Taskbar view symbols were not found");
        g_viewFailedModule = module;
        return false;
    }

    g_viewHooksReady = g_origShouldExpand != nullptr && g_origUpdateExpanded != nullptr;
    if (!g_viewHooksReady) {
        Wh_Log(L"Taskbar view module has no ViewCoordinator symbols");
        g_viewFailedModule = module;
        return false;
    }

    Wh_Log(L"Hooked taskbar ViewCoordinator");
    if (g_afterInit.load(std::memory_order_acquire)) Wh_ApplyHookOperations();
    return true;
}

void InstallViewHooks() {
    if (g_viewHooksReady) return;
    HMODULE view = GetModuleHandleW(L"Taskbar.View.dll");
    if (view) RegisterViewHooks(view);
    if (g_viewHooksReady) return;
    HMODULE ext = GetModuleHandleW(L"ExplorerExtensions.dll");
    if (ext && ext != view) RegisterViewHooks(ext);
}

HMODULE WINAPI Hook_LoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags) {
    HMODULE module = g_origLoadLibraryExW(name, file, flags);
    if (!module || !name || g_viewHooksReady) return module;

    const wchar_t* base = wcsrchr(name, L'\\');
    base = base ? base + 1 : name;
    if (_wcsicmp(base, L"Taskbar.View.dll") == 0 ||
        _wcsicmp(base, L"ExplorerExtensions.dll") == 0) {
        RegisterViewHooks(module);
    }
    return module;
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

    void* setTimer = (void*)GetProcAddress(user32, "SetTimer");
    if (setTimer &&
        !Wh_SetFunctionHook(setTimer, (void*)Hook_SetTimer, (void**)&g_origSetTimer)) {
        Wh_Log(L"Failed to hook SetTimer");
    }

    HMODULE dwmapi = GetModuleHandleW(L"dwmapi.dll");
    if (!dwmapi) dwmapi = LoadLibraryW(L"dwmapi.dll");
    void* dwmAttr = dwmapi ? (void*)GetProcAddress(dwmapi, "DwmSetWindowAttribute") : nullptr;
    if (dwmAttr &&
        !Wh_SetFunctionHook(dwmAttr, (void*)Hook_DwmSetWindowAttribute,
                            (void**)&g_origDwmSetWindowAttribute)) {
        Wh_Log(L"Failed to hook DwmSetWindowAttribute");
    }

    HMODULE kernelBase = GetModuleHandleW(L"kernelbase.dll");
    void* loadLib = kernelBase ? (void*)GetProcAddress(kernelBase, "LoadLibraryExW") : nullptr;
    if (loadLib &&
        !Wh_SetFunctionHook(loadLib, (void*)Hook_LoadLibraryExW, (void**)&g_origLoadLibraryExW)) {
        Wh_Log(L"Failed to hook LoadLibraryExW");
    }

    InstallViewHooks();
    return true;
}

// ---------------------------------------------------------------------
// Keyboard hook. Runs on its own thread so a slow show/hide cannot trip
// the system-wide low-level hook timeout and freeze the keyboard.
// The Windows key must arm the reveal on key-down, before the event reaches
// the shell. Waiting for key-up lets Start open while shows are still blocked,
// which is why the bar stayed hidden until Start began to close.
// ---------------------------------------------------------------------

void UncloakTaskbarsNow() {
    // The shell paints the unhide as soon as it sees this key. Drop the cloak
    // first, or that paint happens on an invisible window.
    HWND bars[16];
    int count = CollectTaskbars(bars, ARRAYSIZE(bars));
    for (int i = 0; i < count; i++) CloakWindowRaw(bars[i], FALSE);
}

void ArmReveal() {
    // Publish "shown" before the shell handles this key, so its own unhide
    // is allowed and a hover block cannot swallow it.
    g_suppressReveal.store(false, std::memory_order_release);
    g_forceHidden.store(false, std::memory_order_release);
    g_revealedByKeyboard.store(true, std::memory_order_release);
    g_revealRequested.store(true, std::memory_order_release);
    UncloakTaskbarsNow();
    if (g_wakeEvent) SetEvent(g_wakeEvent);
}

void NoteWinKey(bool left, bool down) {
    bool& flag = left ? g_lwinDown : g_rwinDown;
    if (flag == down) return;

    if (down) {
        if (!g_lwinDown && !g_rwinDown) g_winChord = false;
        flag = true;
        g_winDownCount.fetch_add(1, std::memory_order_release);
        ArmReveal();
        return;
    }

    flag = false;
    int remaining = g_winDownCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (remaining < 0) {
        g_winDownCount.store(0, std::memory_order_release);
        remaining = 0;
    }
    if (remaining == 0 && !g_winChord) ArmReveal();
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
            ArmReveal();
        } else if (!up && (g_lwinDown || g_rwinDown)) {
            g_winChord = true;
            g_suppressReveal.store(true, std::memory_order_release);
            if (g_wakeEvent) SetEvent(g_wakeEvent);
        }
    }
    return CallNextHookEx(g_keyboardHook, code, wParam, lParam);
}

void PollFallbackWinKey() {
    // The hook sees taps the poll would miss. This path is only for a failed hook.
    if (g_keyboardHookInstalled.load(std::memory_order_acquire)) return;

    bool down = (GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000);
    if (down && !g_fallbackWinDown) ArmReveal();
    else if (g_fallbackWinDown && !down) ArmReveal();
    g_fallbackWinDown = down;

    bool ctrlEsc = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_ESCAPE) & 0x8000);
    if (ctrlEsc && !g_fallbackCtrlEsc) ArmReveal();
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
    g_controlExpand.store(false, std::memory_order_release);
    g_revealedByKeyboard.store(false, std::memory_order_release);
    g_forceHidden.store(false, std::memory_order_release);
    g_loggedAsHidden = false;
    g_sawLauncher = false;
    DriveTaskbar(true);
    SetTaskbarsHidden(false, true);
    RestoreShellAutoHide();
    RestoreWorkArea();
}

BOOL Wh_ModInit() {
    Wh_Log(L"Initializing keyboard-only taskbar hider");

    InitializeCriticalSection(&g_coordLock);
    g_coordLockReady = true;

    LoadSettings();

    if (!InstallHooks()) {
        Wh_Log(L"Failed to install taskbar hooks");
        DeleteCriticalSection(&g_coordLock);
        g_coordLockReady = false;
        return FALSE;
    }

    // Hooks are applied by the engine after Wh_ModInit returns. Threads start
    // in Wh_ModAfterInit so ShowWindow is already detoured before the first hide.
    g_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_wakeEvent) {
        DeleteCriticalSection(&g_coordLock);
        g_coordLockReady = false;
        return FALSE;
    }

    return TRUE;
}

void Wh_ModAfterInit() {
    g_afterInit.store(true, std::memory_order_release);
    InstallViewHooks();

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
    if (g_coordLockReady) {
        DeleteCriticalSection(&g_coordLock);
        g_coordLockReady = false;
    }
}

void Wh_ModSettingsChanged() {
    Wh_Log(L"Settings changed");
    LoadSettings();
    if (g_wakeEvent) SetEvent(g_wakeEvent);
}
