// trainer.dll, shipped as DINPUT.dll next to ff7_en.exe (the game imports DINPUT.dll!DirectInputCreateA, and
// DINPUT isn't a KnownDLL, so the copy beside the exe loads first). We forward that one export to the real
// system dinput.dll and use the load to install our hooks.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "game.h"
#include "log.h"
#include "patch.h"
#include "testmode.h"
#include "trainer.h"

namespace {

HMODULE g_self = nullptr;
char g_dir[MAX_PATH];  // ff7_en.exe's folder

using DirectInputCreateA_t = HRESULT(WINAPI*)(HINSTANCE, DWORD, void**, void*);
DirectInputCreateA_t g_real_create = nullptr;

bool load_real_dinput() {
    if (g_real_create) return true;
    char sys[MAX_PATH];
    GetSystemDirectoryA(sys, MAX_PATH);  // a 32-bit process gets SysWOW64 through WOW64 redirection
    strcat_s(sys, "\\dinput.dll");
    HMODULE real = LoadLibraryA(sys);
    if (!real) {
        tlog("FATAL: could not load real %s (error %lu)", sys, GetLastError());
        return false;
    }
    g_real_create = reinterpret_cast<DirectInputCreateA_t>(GetProcAddress(real, "DirectInputCreateA"));
    tlog("real dinput: %s -> DirectInputCreateA %p", sys, g_real_create);
    return g_real_create != nullptr;
}

DWORD WINAPI monitor_thread(void*) {
    trainer::monitor_loop();
    return 0;
}

}  // namespace

extern "C" HRESULT WINAPI Proxy_DirectInputCreateA(HINSTANCE inst, DWORD version, void** out, void* outer) {
    // Safe point (outside the loader lock, game engine initialising): bring up the rules engine.
    trainer::on_engine_ready(g_dir);
    if (!load_real_dinput()) return E_FAIL;
    HRESULT hr = g_real_create(inst, version, out, outer);
    if (SUCCEEDED(hr)) testmode::wrap_dinput(out);
    testmode::start_script_thread();
    return hr;
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = mod;
        DisableThreadLibraryCalls(mod);
        GetModuleFileNameA(nullptr, g_dir, MAX_PATH);
        if (char* slash = strrchr(g_dir, '\\')) *slash = 0;
        char dir[MAX_PATH], logpath[MAX_PATH];
        sprintf_s(dir, "%s\\trainer", g_dir);
        CreateDirectoryA(dir, nullptr);
        sprintf_s(logpath, "%s\\trainer.log", dir);
        tlog_open(logpath);
        tlog("==== trainer.dll %s loaded into %s ====", trainer::BUILD, g_dir);
        testmode::init_early(g_dir);
        // Code hooks are installed later by the monitor thread (see monitor_loop): FFNx reads these call sites
        // during its own start-up and must see the original instructions.
        CloseHandle(CreateThread(nullptr, 0, monitor_thread, nullptr, 0, nullptr));
    }
    return TRUE;
}
