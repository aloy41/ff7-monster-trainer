#include "testmode.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <atomic>
#include "game.h"
#include "log.h"
#include "hud.h"
#include "mute.h"
#include "trainer.h"

namespace testmode {
namespace {

bool g_enabled = false;
char g_dir[MAX_PATH];
char g_script[MAX_PATH];
std::atomic<HWND> g_hwnd{nullptr};

// Scripted key state, written by the script thread, read by the game thread inside GetDeviceState.
std::atomic<uint8_t> g_keys[256];

// ---- IAT patching --------------------------------------------------------------------------------------

bool patch_iat(HMODULE mod, const char* dll, const char* fn, void* replacement, void** original) {
    auto base = reinterpret_cast<uint8_t*>(mod);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    for (auto imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; imp++) {
        if (_stricmp(reinterpret_cast<char*>(base + imp->Name), dll) != 0) continue;
        auto names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, slots++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<char*>(by_name->Name), fn) != 0) continue;
            DWORD old;
            VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
            if (original && !*original) *original = reinterpret_cast<void*>(slots->u1.Function);
            slots->u1.Function = reinterpret_cast<ULONG_PTR>(replacement);
            VirtualProtect(&slots->u1.Function, sizeof(void*), old, &old);
            return true;
        }
    }
    return false;
}

// ---- window: off-screen, never active ------------------------------------------------------------------

using CreateWindowExA_t = HWND(WINAPI*)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
using ShowWindow_t = BOOL(WINAPI*)(HWND, int);
CreateWindowExA_t real_CreateWindowExA = nullptr;
ShowWindow_t real_ShowWindow = nullptr;
void* real_SetWindowPos = nullptr;

constexpr int OFFSCREEN_X = -4000, OFFSCREEN_Y = -4000;

HWND WINAPI my_CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR name, DWORD style, int x, int y, int w, int h, HWND parent,
                               HMENU menu, HINSTANCE inst, LPVOID param) {
    if (!parent) {
        x = OFFSCREEN_X;
        y = OFFSCREEN_Y;
        ex |= WS_EX_NOACTIVATE;
        ex &= ~DWORD(WS_EX_TOPMOST);
    }
    HWND h2 = real_CreateWindowExA(ex, cls, name, style, x, y, w, h, parent, menu, inst, param);
    if (!parent && h2 && !g_hwnd.load()) {
        g_hwnd = h2;
        tlog("TEST window %p created off-screen (%d,%d %dx%d), WS_EX_NOACTIVATE", h2, x, y, w, h);
    }
    return h2;
}

BOOL WINAPI my_ShowWindow(HWND h, int cmd) {
    if (h == g_hwnd.load()) cmd = SW_SHOWNOACTIVATE;
    return real_ShowWindow(h, cmd);
}

BOOL WINAPI my_SetWindowPos(HWND h, HWND after, int x, int y, int cx, int cy, UINT flags) {
    using Fn = BOOL(WINAPI*)(HWND, HWND, int, int, int, int, UINT);
    if (h == g_hwnd.load()) {
        flags |= SWP_NOACTIVATE;
        if (!(flags & SWP_NOMOVE)) { x = OFFSCREEN_X; y = OFFSCREEN_Y; }
        if (after == HWND_TOPMOST || after == HWND_TOP) flags |= SWP_NOZORDER;
    }
    return reinterpret_cast<Fn>(real_SetWindowPos)(h, after, x, y, cx, cy, flags);
}

// FFNx reads its Ctrl+key hotkeys (skip movie, speedhack) from WM_KEYDOWN plus GetKeyState(VK_CONTROL).
// The script posts the key to the game window and fakes Ctrl for AF3DN.P only, so no real keyboard is involved.
std::atomic<bool> g_fake_ctrl{false};
using GetKeyState_t = SHORT(WINAPI*)(int);
GetKeyState_t real_GetKeyState = nullptr;
SHORT WINAPI my_GetKeyState(int vk) {
    if (vk == VK_CONTROL || vk == VK_LCONTROL) return g_fake_ctrl.load() ? SHORT(-0x8000) : SHORT(0);  // high bit = down
    return real_GetKeyState(vk);
}

BOOL WINAPI no_SetForegroundWindow(HWND) { return TRUE; }
HWND WINAPI no_SetActiveWindow(HWND h) { return h; }
HWND WINAPI no_SetFocus(HWND h) { return h; }
BOOL WINAPI no_BringWindowToTop(HWND) { return TRUE; }

void hook_windows(HMODULE mod, const char* label) {
    int n = 0;
    n += patch_iat(mod, "USER32.dll", "CreateWindowExA", reinterpret_cast<void*>(my_CreateWindowExA), reinterpret_cast<void**>(&real_CreateWindowExA));
    n += patch_iat(mod, "USER32.dll", "ShowWindow", reinterpret_cast<void*>(my_ShowWindow), reinterpret_cast<void**>(&real_ShowWindow));
    n += patch_iat(mod, "USER32.dll", "SetWindowPos", reinterpret_cast<void*>(my_SetWindowPos), &real_SetWindowPos);
    n += patch_iat(mod, "USER32.dll", "SetForegroundWindow", reinterpret_cast<void*>(no_SetForegroundWindow), nullptr);
    n += patch_iat(mod, "USER32.dll", "SetActiveWindow", reinterpret_cast<void*>(no_SetActiveWindow), nullptr);
    n += patch_iat(mod, "USER32.dll", "SetFocus", reinterpret_cast<void*>(no_SetFocus), nullptr);
    n += patch_iat(mod, "USER32.dll", "BringWindowToTop", reinterpret_cast<void*>(no_BringWindowToTop), nullptr);
    if (strcmp(label, "AF3DN.P") == 0)
        n += patch_iat(mod, "USER32.dll", "GetKeyState", reinterpret_cast<void*>(my_GetKeyState), reinterpret_cast<void**>(&real_GetKeyState));
    tlog("TEST %s: %d window imports redirected", label, n);
}

// ---- DirectInput keyboard ------------------------------------------------------------------------------

// IDirectInputA vtable: 3 CreateDevice. IDirectInputDeviceA vtable: 7 Acquire, 9 GetDeviceState, 13 SetCooperativeLevel.
using CreateDevice_t = HRESULT(WINAPI*)(void* self, REFGUID guid, void** out, void* outer);
using GetDeviceState_t = HRESULT(WINAPI*)(void* self, DWORD size, void* data);
using Acquire_t = HRESULT(WINAPI*)(void* self);
using SetCoop_t = HRESULT(WINAPI*)(void* self, HWND, DWORD);
CreateDevice_t real_CreateDevice = nullptr;
GetDeviceState_t real_GetDeviceState = nullptr;
Acquire_t real_Acquire = nullptr;
SetCoop_t real_SetCoop = nullptr;
std::atomic<void*> g_keyboard{nullptr};
const GUID GUID_SysKeyboard_ = {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

void patch_vtable(void* obj, int index, void* fn, void** original) {
    void** vt = *reinterpret_cast<void***>(obj);
    if (vt[index] == fn) return;
    DWORD old;
    VirtualProtect(&vt[index], sizeof(void*), PAGE_READWRITE, &old);
    if (original && !*original) *original = vt[index];
    vt[index] = fn;
    VirtualProtect(&vt[index], sizeof(void*), old, &old);
}

HRESULT WINAPI my_GetDeviceState(void* self, DWORD size, void* data) {
    if (self != g_keyboard.load() || size != 256) return real_GetDeviceState(self, size, data);
    // Ignore the physical keyboard entirely in test mode: only the script types.
    auto* keys = static_cast<uint8_t*>(data);
    for (int i = 0; i < 256; i++) keys[i] = g_keys[i].load(std::memory_order_relaxed) ? 0x80 : 0;
    return 0;  // DI_OK
}

HRESULT WINAPI my_Acquire(void* self) {
    if (self == g_keyboard.load()) return 0;  // nothing to acquire: we are the keyboard
    return real_Acquire(self);
}

HRESULT WINAPI my_SetCoop(void* self, HWND h, DWORD flags) {
    if (self == g_keyboard.load()) flags = (flags & ~DWORD(0x4 | 0x1)) | 0x8 | 0x2;  // BACKGROUND | NONEXCLUSIVE
    return real_SetCoop(self, h, flags);
}

HRESULT WINAPI my_CreateDevice(void* self, REFGUID guid, void** out, void* outer) {
    HRESULT hr = real_CreateDevice(self, guid, out, outer);
    if (SUCCEEDED(hr) && out && *out && IsEqualGUID(guid, GUID_SysKeyboard_)) {
        g_keyboard = *out;
        patch_vtable(*out, 7, reinterpret_cast<void*>(my_Acquire), reinterpret_cast<void**>(&real_Acquire));
        patch_vtable(*out, 9, reinterpret_cast<void*>(my_GetDeviceState), reinterpret_cast<void**>(&real_GetDeviceState));
        patch_vtable(*out, 13, reinterpret_cast<void*>(my_SetCoop), reinterpret_cast<void**>(&real_SetCoop));
        tlog("TEST keyboard device %p: scripted input on, physical keyboard ignored", *out);
    }
    return hr;
}

// ---- script --------------------------------------------------------------------------------------------

int key_code(const char* k) {
    struct { const char* name; int dik; } map[] = {
        {"ok", 0x9C}, {"cancel", 0x52}, {"menu", 0x4E}, {"switch", 0x53}, {"start", 0x4C}, {"up", 0x48},
        {"down", 0x50}, {"left", 0x4B}, {"right", 0x4D}, {"pgup", 0x49}, {"pgdn", 0x51}, {"camera", 0x47},
        {"target", 0x4F}, {"assist", 0x4A},
    };
    for (auto& m : map)
        if (_stricmp(k, m.name) == 0) return m.dik;
    if (k[0] == '0' && (k[1] == 'x' || k[1] == 'X')) return int(strtol(k, nullptr, 16)) & 0xFF;
    return -1;
}

// The game's main window: recorded by our CreateWindowExA hook, or (visible runs, no window hooks) found by PID.
BOOL CALLBACK find_own_window(HWND w, LPARAM out) {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(w) && !GetWindow(w, GW_OWNER)) {
        *reinterpret_cast<HWND*>(out) = w;
        return FALSE;
    }
    return TRUE;
}

HWND game_window() {
    HWND h = g_hwnd.load();
    if (!h) {
        EnumWindows(find_own_window, reinterpret_cast<LPARAM>(&h));
        if (h) g_hwnd = h;
    }
    return h;
}

// ---- fieldop: run one field-script opcode in the current field (test mode only) -----------------------------
// The field dispatcher at 0x60CDAE is `call [opcode*4 + 0x9055A0]` (7 bytes). We redirect it once to fieldop_hook,
// which puts our opcode bytes at the running entity's script position, calls that opcode's handler, then restores the
// script bytes AND the entity's script position, so the entity's own script continues untouched. (FFNx's Field Debug
// warp uses the same dispatch point for MAPJUMP.)
constexpr uintptr_t FIELD_DISPATCH = 0x60CDAE;
constexpr uintptr_t OPCODE_TABLE = 0x9055A0;
uint8_t g_dispatch_orig[7];
uint8_t g_fieldop[32];
int g_fieldop_len = 0;
std::atomic<bool> g_fieldop_done{false};
int g_fieldop_result = 0;

void write_code(uintptr_t at, const void* src, size_t n) {
    DWORD old;
    VirtualProtect(reinterpret_cast<void*>(at), n, PAGE_EXECUTE_READWRITE, &old);
    memcpy(reinterpret_cast<void*>(at), src, n);
    VirtualProtect(reinterpret_cast<void*>(at), n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), n);
}

// The hook stays installed while our opcode is in flight and dispatches every other opcode normally. Opcodes like
// BATTLE return "wait" while their effect runs and must be re-executed at the same position until they complete
// (restoring the script after the first call made the field start the battle again, measured).
struct FieldOp {
    bool claimed = false;  // our bytes are in some entity's script
    uint8_t entity = 0;
    uint16_t at = 0;
    uint8_t saved[32] = {};
};
FieldOp g_op;
std::atomic<bool> g_fieldop_started{false};

int __cdecl fieldop_hook() {
    using Handler = int(__cdecl*)();
    Handler* table = reinterpret_cast<Handler*>(OPCODE_TABLE);
    uint8_t* script = *reinterpret_cast<uint8_t**>(0xCBF5E8);
    uint16_t* pos = reinterpret_cast<uint16_t*>(0xCC0CF8);
    const uint8_t e = *reinterpret_cast<uint8_t*>(0xCC0964);
    if (!g_op.claimed) {  // the first entity that runs gets our opcode at its current position
        g_op.claimed = true;
        g_op.entity = e;
        g_op.at = pos[e];
        memcpy(g_op.saved, script + g_op.at, g_fieldop_len);
        memcpy(script + g_op.at, g_fieldop, g_fieldop_len);
    }
    if (e != g_op.entity || pos[e] != g_op.at) return table[script[pos[e]]]();  // everyone else: normal dispatch
    const int r = table[g_fieldop[0]]();
    g_fieldop_result = r;
    g_fieldop_started = true;
    const bool completed = r == 0 || pos[e] != g_op.at || g_fieldop[0] == 0x60;  // 0x60 MAPJUMP leaves the field
    if (!completed) return r;  // still waiting (e.g. the battle is running): run it again next time
    if (g_fieldop[0] != 0x60) {
        memcpy(script + g_op.at, g_op.saved, g_fieldop_len);
        pos[e] = g_op.at;  // the entity's own instruction runs next frame
    }
    write_code(FIELD_DISPATCH, g_dispatch_orig, 7);
    g_op = FieldOp{};
    g_fieldop_done = true;
    return g_fieldop[0] == 0x60 ? r : 1;
}

// The dispatcher at 0x60CDAE runs many times per frame on the game thread while this (script) thread patches it.
// Write order keeps every intermediate state a valid instruction: 1) "EB FE" (jump-to-self, one aligned 16-bit write:
// a thread arriving now spins), 2) bytes 2..6, 3) the first two bytes, atomically.
void hot_patch_dispatch(const uint8_t* bytes7) {
    DWORD old;
    VirtualProtect(reinterpret_cast<void*>(FIELD_DISPATCH), 7, PAGE_EXECUTE_READWRITE, &old);
    auto* head = reinterpret_cast<volatile uint16_t*>(FIELD_DISPATCH);  // 0x60CDAE is 2-byte aligned
    const uint8_t spin_bytes[2] = {0xEB, 0xFE};  // jmp $ (little-endian EB FE)
    SHORT spin;
    memcpy(&spin, spin_bytes, 2);
    InterlockedExchange16(reinterpret_cast<volatile SHORT*>(head), spin);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(FIELD_DISPATCH), 7);
    memcpy(reinterpret_cast<uint8_t*>(FIELD_DISPATCH) + 2, bytes7 + 2, 5);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(FIELD_DISPATCH), 7);
    SHORT first;
    memcpy(&first, bytes7, 2);
    InterlockedExchange16(reinterpret_cast<volatile SHORT*>(head), first);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(FIELD_DISPATCH), 7);
    VirtualProtect(reinterpret_cast<void*>(FIELD_DISPATCH), 7, old, &old);
}

bool run_fieldop(const uint8_t* bytes, int n, int timeout_ms) {
    if (n <= 0 || n > 32) return false;
    if (ff7::mode() != ff7::FIELD) { tlog("TEST fieldop: not in the field (mode %u)", ff7::mode()); return false; }
    if (g_op.claimed) { tlog("TEST fieldop: previous opcode still in flight"); return false; }
    memcpy(g_fieldop, bytes, n);
    g_fieldop_len = n;
    g_fieldop_done = false;
    g_fieldop_started = false;
    memcpy(g_dispatch_orig, reinterpret_cast<void*>(FIELD_DISPATCH), 7);
    if (g_dispatch_orig[0] != 0xFF || g_dispatch_orig[1] != 0x14 || g_dispatch_orig[2] != 0x85) {
        tlog("TEST fieldop: dispatch site %08X is not call [eax*4+table] (%02X %02X %02X)", unsigned(FIELD_DISPATCH),
             g_dispatch_orig[0], g_dispatch_orig[1], g_dispatch_orig[2]);
        return false;
    }
    uint8_t patch[7] = {0x90, 0x90, 0xE8, 0, 0, 0, 0};
    const int32_t rel = int32_t(reinterpret_cast<uintptr_t>(&fieldop_hook) - (FIELD_DISPATCH + 2 + 5));
    memcpy(patch + 3, &rel, 4);
    hot_patch_dispatch(patch);
    const DWORD start = GetTickCount();
    while (!g_fieldop_started) {  // wait until it ran once; completion (e.g. after a battle) is handled by the hook
        if (int(GetTickCount() - start) > timeout_ms) {
            hot_patch_dispatch(g_dispatch_orig);
            tlog("TEST fieldop: no field script ran within %d ms; unpatched", timeout_ms);
            return false;
        }
        Sleep(5);
    }
    tlog("TEST fieldop %02X (%d bytes) started, handler returned %d%s", bytes[0], n, g_fieldop_result,
         g_fieldop_done ? " (done)" : " (in flight)");
    return true;
}

void shot(const char* name) {
    HWND h = game_window();
    RECT r;
    if (!h || !GetClientRect(h, &r) || r.right <= 0 || r.bottom <= 0) {
        tlog("SHOT %s: no window", name);
        return;
    }
    int w = r.right, hgt = r.bottom;
    HDC wdc = GetDC(h);
    HDC mdc = CreateCompatibleDC(wdc);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -hgt;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(wdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mdc, bmp);
    BOOL ok = PrintWindow(h, mdc, PW_CLIENTONLY | 2 /* PW_RENDERFULLCONTENT */);
    char path[MAX_PATH];
    sprintf_s(path, "%s\\trainer\\shots", g_dir);
    CreateDirectoryA(path, nullptr);
    sprintf_s(path, "%s\\trainer\\shots\\%s.bmp", g_dir, name);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") == 0 && f) {
        BITMAPFILEHEADER fh = {};
        DWORD img = DWORD(w) * hgt * 4;
        fh.bfType = 0x4D42;
        fh.bfOffBits = sizeof fh + sizeof bi.bmiHeader;
        fh.bfSize = fh.bfOffBits + img;
        fwrite(&fh, sizeof fh, 1, f);
        fwrite(&bi.bmiHeader, sizeof bi.bmiHeader, 1, f);
        fwrite(bits, img, 1, f);
        fclose(f);
    }
    SelectObject(mdc, old);
    DeleteObject(bmp);
    DeleteDC(mdc);
    ReleaseDC(h, wdc);
    tlog("SHOT %s %dx%d %s", path, w, hgt, ok ? "ok" : "PrintWindow failed");
    if (HWND hh = static_cast<HWND>(hud::window()); hh && IsWindowVisible(hh)) {
        RECT hr;
        GetClientRect(hh, &hr);
        HDC hdc = GetDC(hh), hm = CreateCompatibleDC(hdc);
        BITMAPINFO hb = {};
        hb.bmiHeader.biSize = sizeof hb.bmiHeader;
        hb.bmiHeader.biWidth = hr.right;
        hb.bmiHeader.biHeight = -hr.bottom;
        hb.bmiHeader.biPlanes = 1;
        hb.bmiHeader.biBitCount = 32;
        void* hbits = nullptr;
        HBITMAP hbmp = CreateDIBSection(hdc, &hb, DIB_RGB_COLORS, &hbits, nullptr, 0);
        HGDIOBJ ho = SelectObject(hm, hbmp);
        PrintWindow(hh, hm, PW_CLIENTONLY);
        char hpath[MAX_PATH];
        sprintf_s(hpath, "%s\\trainer\\shots\\%s_hud.bmp", g_dir, name);
        FILE* hf = nullptr;
        if (fopen_s(&hf, hpath, "wb") == 0 && hf) {
            BITMAPFILEHEADER fh2 = {};
            const DWORD img2 = DWORD(hr.right) * hr.bottom * 4;
            fh2.bfType = 0x4D42;
            fh2.bfOffBits = sizeof fh2 + sizeof hb.bmiHeader;
            fh2.bfSize = fh2.bfOffBits + img2;
            fwrite(&fh2, sizeof fh2, 1, hf);
            fwrite(&hb.bmiHeader, sizeof hb.bmiHeader, 1, hf);
            fwrite(hbits, img2, 1, hf);
            fclose(hf);
        }
        SelectObject(hm, ho);
        DeleteObject(hbmp);
        DeleteDC(hm);
        ReleaseDC(hh, hdc);
    }
}

bool wait_mode(int mode, bool equal, int timeout_ms) {
    DWORD start = GetTickCount();
    while ((ff7::mode() == mode) != equal) {
        if (timeout_ms > 0 && int(GetTickCount() - start) > timeout_ms) return false;
        Sleep(16);
    }
    return true;
}

DWORD WINAPI script_thread(void*) {
    FILE* f = nullptr;
    if (fopen_s(&f, g_script, "r") != 0 || !f) {
        tlog("TEST script %s: cannot open", g_script);
        return 0;
    }
    struct Line { int no; char text[256]; };
    static Line lines[8192];
    int n = 0, lineno = 0;
    char buf[512];
    while (n < 8192 && fgets(buf, sizeof buf, f)) {
        lineno++;
        char* s = buf;
        while (*s == ' ' || *s == '\t') s++;
        s[strcspn(s, "\r\n")] = 0;
        if (!*s || *s == '#') continue;
        lines[n].no = lineno;
        strncpy_s(lines[n].text, s, _TRUNCATE);
        n++;
    }
    fclose(f);
    // Matching `end` for each `whilemode`, and loop start times.
    static int match[8192];
    static DWORD loop_start[8192];
    int stack[64], sp = 0;
    for (int i = 0; i < n; i++) {
        match[i] = -1;
        if (!strncmp(lines[i].text, "whilemode", 9) && sp < 64) stack[sp++] = i;
        else if (!strcmp(lines[i].text, "end") && sp > 0) { int o = stack[--sp]; match[o] = i; match[i] = o; }
    }
    tlog("TEST script %s: start (%d commands)", g_script, n);
    for (int pc = 0; pc < n; pc++) {
        const char* s = lines[pc].text;
        const int no = lines[pc].no;
        char cmd[32] = {}, a1[128] = {}, a2[128] = {};
        sscanf_s(s, "%31s %127s %127s", cmd, 32, a1, 128, a2, 128);
        if (!strcmp(cmd, "wait")) {
            Sleep(atoi(a1));
        } else if (!strcmp(cmd, "press") || !strcmp(cmd, "hold")) {
            int k = key_code(a1);
            if (k < 0) { tlog("TEST line %d: unknown key %s", no, a1); continue; }
            int ms = a2[0] ? atoi(a2) : 120;
            g_keys[k] = 1;
            Sleep(ms);
            g_keys[k] = 0;
            if (!strcmp(cmd, "press")) Sleep(120);
        } else if (!strcmp(cmd, "waitmode") || !strcmp(cmd, "waitnotmode")) {
            bool eq = !strcmp(cmd, "waitmode");
            int timeout = a2[0] ? atoi(a2) : 0;
            if (!wait_mode(atoi(a1), eq, timeout))
                tlog("TEST line %d: TIMEOUT %s %s after %d ms (mode is %u)", no, cmd, a1, timeout, ff7::mode());
            else
                tlog("TEST line %d: %s %s ok", no, cmd, a1);
        } else if (!strcmp(cmd, "whilemode")) {
            // whilemode <mode> <timeout_ms> ... end : repeat the block while ff7 mode == <mode>
            if (match[pc] < 0) { tlog("TEST line %d: whilemode without end", no); break; }
            DWORD now = GetTickCount();
            if (!loop_start[pc]) loop_start[pc] = now;
            int timeout = a2[0] ? atoi(a2) : 0;
            bool timed_out = timeout > 0 && int(now - loop_start[pc]) > timeout;
            if (ff7::mode() != atoi(a1) || timed_out) {
                tlog("TEST line %d: whilemode %s done after %lu ms%s (mode is %u)", no, a1, now - loop_start[pc],
                     timed_out ? " - TIMEOUT" : "", ff7::mode());
                loop_start[pc] = 0;
                pc = match[pc];  // continue after `end`
            }
        } else if (!strcmp(cmd, "end")) {
            if (match[pc] >= 0) pc = match[pc] - 1;  // re-test the whilemode
        } else if (!strcmp(cmd, "ffnx")) {
            // ffnx skip | speedup | speeddown | speedtoggle   (FFNx Ctrl hotkeys)
            int vk = !strcmp(a1, "skip") ? 'S' : !strcmp(a1, "speedup") ? VK_UP : !strcmp(a1, "speeddown") ? VK_DOWN :
                     !strcmp(a1, "speedtoggle") ? VK_RIGHT : 0;
            HWND h = game_window();
            if (!vk || !h) { tlog("TEST line %d: ffnx %s: %s", no, a1, vk ? "no window" : "unknown hotkey"); continue; }
            g_fake_ctrl = true;
            PostMessageA(h, WM_KEYDOWN, WPARAM(vk), 1);
            Sleep(150);
            const DWORD up_bits = 0xC0000001;  // repeat 1, previous-state + transition bits set
            LPARAM up;
            memcpy(&up, &up_bits, sizeof up);
            PostMessageA(h, WM_KEYUP, WPARAM(vk), up);
            Sleep(100);
            g_fake_ctrl = false;
            tlog("TEST ffnx %s", a1);
        } else if (!strcmp(cmd, "fieldop")) {
            // fieldop <hex bytes...>   e.g. fieldop 49 00 0E 00 (MENU), fieldop 60 7C 00 00 00 00 00 00 00 00 (MAPJUMP 124)
            uint8_t bytes[32];
            int nb = 0;
            for (const char* q = s + 7; *q && nb < 32;) {
                while (*q == ' ') q++;
                if (!*q) break;
                bytes[nb++] = uint8_t(strtoul(q, const_cast<char**>(&q), 16));
            }
            run_fieldop(bytes, nb, 5000);
        } else if (!strcmp(cmd, "hud")) {
            // hud f5|f6|f7 : the HUD hotkey actions
            const int k = (a1[0] == 'f' || a1[0] == 'F') ? atoi(a1 + 1) : 0;
            hud::action(k);
            tlog("TEST hud %s", a1);
        } else if (!strcmp(cmd, "core")) {
            // core load <slot> | core new   (roster from a sidecar save, or empty)
            if (!trainer::core_ready) { tlog("TEST line %d: core not ready", no); continue; }
            // Let the monitor thread (16 ms poll) handle the mode change first: leaving the title screen resets
            // the roster (New Game), which would otherwise wipe a roster loaded in the same frame.
            Sleep(250);
            const int r = !strcmp(a1, "load") ? trainer::api.load_slot(uint32_t(atoi(a2))) : trainer::api.new_game();
            tlog("TEST core %s %s -> %d (party %d, box %d)", a1, a2, r, trainer::api.party_len(), trainer::api.box_len());
        } else if (!strcmp(cmd, "snap")) {
            // snap <name> : write the battle/menu memory regions to trainer\snaps\<name>_<addr>.bin for offline diffing
            static const struct { uintptr_t a; size_t n; } regions[] = {
                {0x9A8000, 0x8000}, {0xBE0000, 0x20000}, {0xDBA000, 0xA000}, {0x91E000, 0x2000}};
            char path[MAX_PATH];
            sprintf_s(path, "%s\\trainer\\snaps", g_dir);
            CreateDirectoryA(path, nullptr);
            for (auto& r : regions) {
                sprintf_s(path, "%s\\trainer\\snaps\\%s_%06X.bin", g_dir, a1, unsigned(r.a));
                FILE* sf = nullptr;
                if (fopen_s(&sf, path, "wb") == 0 && sf) {
                    fwrite(reinterpret_cast<const void*>(r.a), 1, r.n, sf);
                    fclose(sf);
                }
            }
            tlog("SNAP %s (4 regions)", a1);
        } else if (!strcmp(cmd, "layout")) {
            // layout <n> | layout off : force the next battles' layout (2 back attack, 4 pincer, ...)
            trainer::test_layout = !strcmp(a1, "off") ? -1 : int(strtol(a1, nullptr, 0));
            tlog("TEST layout %d", trainer::test_layout);
        } else if (!strcmp(cmd, "find")) {
            // find <ascii> -> every committed read/write address holding it (first 64), for reverse engineering
            const size_t flen = strlen(a1);
            int hits = 0;
            MEMORY_BASIC_INFORMATION mbi;
            for (uintptr_t p = 0x10000; p < 0x7FFF0000 && hits < 64 && VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof mbi);
                 p = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize) {
                if (mbi.State != MEM_COMMIT || !(mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) || (mbi.Protect & PAGE_GUARD)) continue;
                const char* b = static_cast<const char*>(mbi.BaseAddress);
                for (size_t o = 0; o + flen <= mbi.RegionSize && hits < 64; o++)
                    if (b[o] == a1[0] && !memcmp(b + o, a1, flen)) {
                        tlog("FIND '%s' at %08X", a1, unsigned(uintptr_t(b + o)));
                        hits++;
                    }
            }
            tlog("FIND '%s': %d hit(s)", a1, hits);
        } else if (!strcmp(cmd, "dump")) {
            // dump <hex addr> <len>  -> hex into the log (16 bytes per line)
            uintptr_t addr = strtoul(a1, nullptr, 16);
            int len = a2[0] ? int(strtol(a2, nullptr, 0)) : 16;
            for (int o = 0; o < len; o += 16) {
                char hex[16 * 3 + 1] = {};
                for (int b = 0; b < 16 && o + b < len; b++)
                    sprintf_s(hex + b * 3, 4, "%02X ", reinterpret_cast<const uint8_t*>(addr)[o + b]);
                tlog("DUMP %08X: %s", unsigned(addr + o), hex);
            }
        } else if (!strcmp(cmd, "write8") || !strcmp(cmd, "write16") || !strcmp(cmd, "write32") ||
                   !strcmp(cmd, "or32") || !strcmp(cmd, "andnot32")) {
            // write8|write16|write32|or32|andnot32 <hex addr> <value>   (experiments only; test mode only)
            uintptr_t addr = strtoul(a1, nullptr, 16);
            uint32_t v = strtoul(a2, nullptr, 0);
            if (!strcmp(cmd, "write8")) *reinterpret_cast<volatile uint8_t*>(addr) = uint8_t(v);
            else if (!strcmp(cmd, "write16")) *reinterpret_cast<volatile uint16_t*>(addr) = uint16_t(v);
            else if (!strcmp(cmd, "write32")) *reinterpret_cast<volatile uint32_t*>(addr) = v;
            else if (!strcmp(cmd, "or32")) *reinterpret_cast<volatile uint32_t*>(addr) |= v;
            else *reinterpret_cast<volatile uint32_t*>(addr) &= ~v;
            tlog("POKE %s %08X %08X -> now %08X", cmd, unsigned(addr), v, *reinterpret_cast<volatile uint32_t*>(addr));
        } else if (!strcmp(cmd, "shot")) {
            // a '#' in the name becomes a running number, so shots inside loops don't overwrite each other
            static int seq = 0;
            char nm[160];
            const char* hash = strchr(a1, '#');
            if (hash) sprintf_s(nm, "%.*s%03d%s", int(hash - a1), a1, ++seq, hash + 1);
            else strcpy_s(nm, a1);
            shot(nm);
        } else if (!strcmp(cmd, "log")) {
            tlog("TEST %s", s + 4);
        } else if (!strcmp(cmd, "sweep")) {
            // sweep on|off : the monster cycles through its moves each battle (trainer::test_sweep)
            trainer::test_sweep = !strcmp(a1, "on");
            tlog("TEST sweep %s", trainer::test_sweep ? "on" : "off");
        } else if (!strcmp(cmd, "quitifmode") || !strcmp(cmd, "quitifnotmode")) {
            // quitifmode <mode> : stop the run here if the game is still in that mode (a hung battle)
            // quitifnotmode <mode> : ... or if it isn't (a Game Over instead of the field)
            if ((ff7::mode() == unsigned(atoi(a1))) == !strcmp(cmd, "quitifmode")) {
                tlog("TEST quitifmode %s: still in mode %u, quitting", a1, ff7::mode());
                TerminateProcess(GetCurrentProcess(), 0);
            }
        } else if (!strcmp(cmd, "quit")) {
            tlog("TEST quit");
            TerminateProcess(GetCurrentProcess(), 0);
        } else {
            tlog("TEST line %d: unknown command '%s'", no, cmd);
        }
    }
    tlog("TEST script: end (game keeps running)");
    return 0;
}

DWORD WINAPI mute_thread(void*) {
    bool logged = false;
    for (;;) {
        const bool ok = mute::mute_process();
        if (ok && !logged) {
            tlog("TEST audio muted (this process's audio session only)");
            logged = true;
        }
        Sleep(ok ? 1000 : 100);
    }
}

}  // namespace

bool enabled() { return g_enabled; }

void init_early(const char* game_dir) {
    strcpy_s(g_dir, game_dir);
    sprintf_s(g_script, "%s\\trainer\\test_script.txt", game_dir);
    g_enabled = GetFileAttributesA(g_script) != INVALID_FILE_ATTRIBUTES;
    if (!g_enabled) return;
    tlog("TEST mode ON (%s)", g_script);
    CloseHandle(CreateThread(nullptr, 0, mute_thread, nullptr, 0, nullptr));
    char visible[MAX_PATH];
    sprintf_s(visible, "%s\\trainer\\test_visible.flag", game_dir);
    if (GetFileAttributesA(visible) != INVALID_FILE_ATTRIBUTES) {
        // Visible run (the user asked to watch): normal on-screen window; input is still scripted and in-process.
        tlog("TEST visible run: window stays on screen");
        HMODULE ffnx = GetModuleHandleA("AF3DN.P");
        if (ffnx)
            patch_iat(ffnx, "USER32.dll", "GetKeyState", reinterpret_cast<void*>(my_GetKeyState),
                      reinterpret_cast<void**>(&real_GetKeyState));
        return;
    }
    hook_windows(GetModuleHandleA(nullptr), "ff7_en.exe");
    if (HMODULE ffnx = GetModuleHandleA("AF3DN.P")) hook_windows(ffnx, "AF3DN.P");
    else tlog("TEST AF3DN.P not loaded yet; only the exe's window imports are redirected");
}

void wrap_dinput(void** direct_input) {
    if (!g_enabled || !direct_input || !*direct_input) return;
    patch_vtable(*direct_input, 3, reinterpret_cast<void*>(my_CreateDevice), reinterpret_cast<void**>(&real_CreateDevice));
}

void start_script_thread() {
    static bool once = false;
    if (!g_enabled || once) return;
    once = true;
    CloseHandle(CreateThread(nullptr, 0, script_thread, nullptr, 0, nullptr));
}

}  // namespace testmode
