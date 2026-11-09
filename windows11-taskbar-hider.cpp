// ==WindhawkMod==
// @id              windows11-app-specific-taskbar-hider
// @name            Windows 11 Keyboard-Only Taskbar Hider
// @description     Always hides the taskbar and blocks mouse hover/edge reveal. Press the Windows key to show it.
// @version         3.0
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

Hides the Windows 11 taskbar for every application and blocks hover/edge
auto-reveal. The taskbar comes back when you press the Windows key (or
Ctrl+Esc), stays up while Start is open, and hides again when Start closes.

## How it works

The mod turns on the shell's own auto-hide while it is loaded, then blocks
only the mouse unhide path (the tray unhide timer, and on Windows 11 the
pointer-over / screen-edge expand). The Windows key is left alone, so the
shell runs its normal Start unhide and keeps the bar visible the whole time
Start is open. The collapsed bar is cloaked so the auto-hide sliver does not
show.

When the mod is unloaded or the master switch is turned off, the previous
auto-hide setting is written back.

## Usage

1. Leave Windows' own "Automatically hide the taskbar" **off**. The mod turns
   that on while it is loaded and restores it when unloaded.
2. Enable the mod. Hover at the screen edge does nothing.
3. Press the Windows key. Start opens and the taskbar stays visible with it.
4. Close Start. The taskbar hides again.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- hideTaskbar: true
  $name: Hide the taskbar
  $description: >-
    When enabled, the taskbar stays hidden and mouse hover cannot reveal it.
    Press the Windows key to show it. Turn this off to restore normal behavior.
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <windhawk_utils.h>

#include <atomic>

#ifndef DWMWA_CLOAK
#define DWMWA_CLOAK 13
#endif

// TrayUI mouse-unhide timer. The Windows key does not use this id.
constexpr UINT_PTR kTrayTimerUnhide = 3;
// ViewCoordinator::UpdateIsExpanded reasons that mean "the pointer did this".
constexpr int kReasonPointerOver = 7;
constexpr int kReasonEdgeStroke = 8;

std::atomic<bool> g_enabled{true};
std::atomic<bool> g_afterInit{false};

bool g_autoHideSaved = false;
UINT g_originalAppBarState = 0;
bool g_viewHooksReady = false;

using SetTimer_t = UINT_PTR(WINAPI*)(HWND, UINT_PTR, UINT, TIMERPROC);
SetTimer_t g_origSetTimer = nullptr;

using DwmSetWindowAttribute_t = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
DwmSetWindowAttribute_t g_origDwmSetWindowAttribute = nullptr;

using LoadLibraryExW_t = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);
LoadLibraryExW_t g_origLoadLibraryExW = nullptr;

using ViewIsExpanded_t = bool(WINAPI*)(void* pThis, HWND hwnd);
ViewIsExpanded_t g_origIsExpanded = nullptr;

using ViewUpdateExpanded_t = void(WINAPI*)(void* pThis, HWND hwnd, int reason);
ViewUpdateExpanded_t g_origUpdateExpanded = nullptr;

bool ClassIs(HWND hwnd, const wchar_t* name) {
    WCHAR cls[64];
    if (!hwnd || !GetClassNameW(hwnd, cls, ARRAYSIZE(cls))) return false;
    return _wcsicmp(cls, name) == 0;
}

bool IsTaskbarFrame(HWND hwnd) {
    return ClassIs(hwnd, L"Shell_TrayWnd") || ClassIs(hwnd, L"Shell_SecondaryTrayWnd");
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

void CloakWindow(HWND hwnd, BOOL cloak) {
    if (!IsWindow(hwnd)) return;
    if (g_origDwmSetWindowAttribute) {
        g_origDwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
    } else {
        DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
    }
}

void CloakAllTaskbars(BOOL cloak) {
    HWND bars[16];
    int count = CollectTaskbars(bars, ARRAYSIZE(bars));
    for (int i = 0; i < count; i++) CloakWindow(bars[i], cloak);
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
    Wh_Log(L"Enabled shell auto-hide");
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

void ApplyEnabledState() {
    if (g_enabled.load(std::memory_order_acquire)) {
        EnsureShellAutoHide();
        // Only cloak when we can uncloak again through ViewCoordinator.
        if (g_viewHooksReady) CloakAllTaskbars(TRUE);
    } else {
        CloakAllTaskbars(FALSE);
        RestoreShellAutoHide();
    }
}

UINT_PTR WINAPI Hook_SetTimer(HWND hwnd, UINT_PTR id, UINT elapse, TIMERPROC proc) {
    // Block the mouse unhide timer. The Windows key unhides through the shell,
    // not this timer.
    if (id == kTrayTimerUnhide && IsTaskbarFrame(hwnd) &&
        g_enabled.load(std::memory_order_acquire)) {
        return id ? id : 1;
    }
    return g_origSetTimer(hwnd, id, elapse, proc);
}

HRESULT WINAPI Hook_DwmSetWindowAttribute(HWND hwnd, DWORD attribute, LPCVOID data,
                                          DWORD size) {
    // Keep the collapsed bar cloaked: reject uncloak while we are hiding.
    if (attribute == DWMWA_CLOAK && IsTaskbarFrame(hwnd) && data && size >= sizeof(BOOL) &&
        g_enabled.load(std::memory_order_acquire) && !*(const BOOL*)data) {
        return S_OK;
    }
    return g_origDwmSetWindowAttribute(hwnd, attribute, data, size);
}

void WINAPI Hook_UpdateExpanded(void* pThis, HWND hwnd, int reason) {
    // Block pointer-over and screen-edge expand. Leave every other reason alone
    // so the Windows key / Start can expand the bar normally.
    if (g_enabled.load(std::memory_order_acquire) &&
        (reason == kReasonPointerOver || reason == kReasonEdgeStroke)) {
        return;
    }

    g_origUpdateExpanded(pThis, hwnd, reason);

    if (!g_enabled.load(std::memory_order_acquire) || !g_origIsExpanded) return;
    bool expanded = g_origIsExpanded(pThis, hwnd);
    CloakWindow(hwnd, expanded ? FALSE : TRUE);
}

bool InstallViewHooks(HMODULE module);
void TryInstallViewHooks();

HMODULE WINAPI Hook_LoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags) {
    HMODULE module = g_origLoadLibraryExW(name, file, flags);
    if (!module || !name || g_viewHooksReady) return module;

    const wchar_t* base = wcsrchr(name, L'\\');
    base = base ? base + 1 : name;
    if (_wcsicmp(base, L"Taskbar.View.dll") == 0 ||
        _wcsicmp(base, L"ExplorerExtensions.dll") == 0) {
        InstallViewHooks(module);
    }
    return module;
}

bool InstallViewHooks(HMODULE module) {
    if (!module || g_viewHooksReady) return g_viewHooksReady;

    WindhawkUtils::SYMBOL_HOOK hooks[] = {
        {
            {LR"(public: bool __cdecl winrt::Taskbar::implementation::ViewCoordinator::IsExpanded(unsigned __int64))"},
            &g_origIsExpanded,
            nullptr,
            true,
        },
        {
            {LR"(public: void __cdecl winrt::Taskbar::implementation::ViewCoordinator::UpdateIsExpanded(unsigned __int64,enum TaskbarTipTest::TaskbarExpandCollapseReason))"},
            &g_origUpdateExpanded,
            Hook_UpdateExpanded,
            true,
        },
    };

    if (!WindhawkUtils::HookSymbols(module, hooks, ARRAYSIZE(hooks))) {
        Wh_Log(L"ViewCoordinator symbols not found in module");
        return false;
    }

    if (!g_origUpdateExpanded) {
        Wh_Log(L"UpdateIsExpanded not resolved");
        return false;
    }

    g_viewHooksReady = true;
    Wh_Log(L"Hooked ViewCoordinator");
    if (g_afterInit.load(std::memory_order_acquire)) Wh_ApplyHookOperations();
    return true;
}

void TryInstallViewHooks() {
    if (g_viewHooksReady) return;
    HMODULE view = GetModuleHandleW(L"Taskbar.View.dll");
    if (view && InstallViewHooks(view)) return;
    HMODULE ext = GetModuleHandleW(L"ExplorerExtensions.dll");
    if (ext && ext != view) InstallViewHooks(ext);
}

bool InstallHooks() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return false;

    void* setTimer = (void*)GetProcAddress(user32, "SetTimer");
    if (!setTimer ||
        !Wh_SetFunctionHook(setTimer, (void*)Hook_SetTimer, (void**)&g_origSetTimer)) {
        return false;
    }

    HMODULE dwmapi = GetModuleHandleW(L"dwmapi.dll");
    if (!dwmapi) dwmapi = LoadLibraryW(L"dwmapi.dll");
    void* dwmAttr = dwmapi ? (void*)GetProcAddress(dwmapi, "DwmSetWindowAttribute") : nullptr;
    if (dwmAttr) {
        Wh_SetFunctionHook(dwmAttr, (void*)Hook_DwmSetWindowAttribute,
                           (void**)&g_origDwmSetWindowAttribute);
    }

    HMODULE kernelBase = GetModuleHandleW(L"kernelbase.dll");
    void* loadLib = kernelBase ? (void*)GetProcAddress(kernelBase, "LoadLibraryExW") : nullptr;
    if (loadLib) {
        Wh_SetFunctionHook(loadLib, (void*)Hook_LoadLibraryExW, (void**)&g_origLoadLibraryExW);
    }

    TryInstallViewHooks();
    return true;
}

void LoadSettings() {
    g_enabled.store(Wh_GetIntSetting(L"hideTaskbar") != 0, std::memory_order_release);
}

BOOL Wh_ModInit() {
    Wh_Log(L"Initializing keyboard-only taskbar hider");
    LoadSettings();
    if (!InstallHooks()) {
        Wh_Log(L"Failed to install hooks");
        return FALSE;
    }
    return TRUE;
}

void Wh_ModAfterInit() {
    g_afterInit.store(true, std::memory_order_release);
    TryInstallViewHooks();
    ApplyEnabledState();
}

void Wh_ModBeforeUninit() {
    CloakAllTaskbars(FALSE);
    RestoreShellAutoHide();
}

void Wh_ModUninit() {
    Wh_Log(L"Uninitialized");
}

void Wh_ModSettingsChanged() {
    LoadSettings();
    ApplyEnabledState();
}
