// ==WindhawkMod==
// @id              winamp-titlebar-unload-guard
// @name            Winamp Title Bar unload guard (temporary)
// @description     Upgrade helper: pins comctl32 and any loaded Winamp Classic Title Bar build in memory, so replacing a build older than 0.11 can't crash running apps. Install, upgrade, then remove.
// @version         1.0
// @author          you
// @include         *
// @exclude         winamp.exe
// @compilerOptions -lpsapi
// ==/WindhawkMod==

// Builds before 0.11 could leave windows pointing into the old DLL (or into a
// comctl32 that only that DLL kept loaded) when Windhawk unloads them. Pinning
// both before the swap means the old code simply stays mapped: harmless.

#include <windows.h>
#include <psapi.h>

static void PinModules() {
    HMODULE mods[1024];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return;
    DWORD count = needed / sizeof(HMODULE);
    if (count > 1024) count = 1024;
    for (DWORD i = 0; i < count; i++) {
        wchar_t name[MAX_PATH];
        if (!GetModuleBaseNameW(GetCurrentProcess(), mods[i], name, MAX_PATH)) continue;
        if (_wcsicmp(name, L"comctl32.dll") != 0 &&
            _wcsnicmp(name, L"local@winamp-classic-titlebar_", 30) != 0) continue;
        HMODULE pinned;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                               (LPCWSTR)mods[i], &pinned))
            Wh_Log(L"pinned %s", name);
    }
}

BOOL Wh_ModInit() {
    PinModules();
    return TRUE;
}

void Wh_ModAfterInit() {
    PinModules();   // again, in case the title bar mod loaded after us
}
