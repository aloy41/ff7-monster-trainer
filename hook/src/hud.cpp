#include "hud.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "game.h"
#include "log.h"
#include "trainer.h"

namespace hud {
namespace {

SRWLOCK g_lock = SRWLOCK_INIT;  // static init: usable before start() (no init-order hazard)
HWND g_hud = nullptr;
HWND g_game = nullptr;

struct BattleMon {
    bool out = false;
    char name[32] = {};
    unsigned level = 0;
    int hp = 0, hp_max = 0;
} g_battle;

bool g_roster_open = false;

bool battle_out() {
    AcquireSRWLockExclusive(&g_lock);
    const bool out = g_battle.out;
    ReleaseSRWLockExclusive(&g_lock);
    return out;
}
char g_toast[96] = {};
DWORD g_toast_until = 0;

constexpr COLORREF BG = RGB(18, 30, 92);        // FF7 menu blue
constexpr COLORREF FRAME = RGB(200, 205, 220);
constexpr COLORREF TEXT_FG = RGB(240, 240, 245);
constexpr COLORREF DIM = RGB(160, 170, 205);
constexpr COLORREF HP_OK = RGB(90, 200, 120), HP_MID = RGB(230, 190, 70), HP_LOW = RGB(220, 80, 70);

BOOL CALLBACK find_game(HWND w, LPARAM out) {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid == GetCurrentProcessId() && w != g_hud && IsWindowVisible(w) && !GetWindow(w, GW_OWNER)) {
        *reinterpret_cast<HWND*>(out) = w;
        return FALSE;
    }
    return TRUE;
}

void toast(const char* s) {
    AcquireSRWLockExclusive(&g_lock);
    strcpy_s(g_toast, s);
    g_toast_until = GetTickCount() + 2500;
    ReleaseSRWLockExclusive(&g_lock);
    tlog("HUD %s", s);
}

bool in_field() {
    const uint16_t m = ff7::mode();
    return m == ff7::FIELD || m == ff7::WORLDMAP || m == ff7::MENU;
}

// ---- roster actions (field only; the core is mutex-protected, safe from this thread) ----------------------

// F5: the party rotates (the lead goes to the back, everyone moves up one) until a conscious monster leads, so
// repeated presses cycle through every monster in party order.
void next_lead() {
    if (!trainer::core_ready || !in_field()) return;
    const int n = trainer::api.party_len();
    if (n < 2) { toast(n ? "No other conscious monster to lead" : "No monsters yet: Catch one!"); return; }
    for (int turn = 1; turn < n; turn++) {
        for (int j = 0; j + 1 < n; j++) trainer::api.swap(uint32_t(j), uint32_t(j + 1));
        TrMonsterView v;
        if (trainer::api.party_get(0, &v) == 0 && !v.ko) {
            char s[80];
            sprintf_s(s, "Lead: %s (Lv %u)", v.name, v.level);
            toast(s);
            return;
        }
    }
    // Every other monster is fainted: one more rotation restores the original order.
    for (int j = 0; j + 1 < n; j++) trainer::api.swap(uint32_t(j), uint32_t(j + 1));
    toast("No other conscious monster to lead");
}

void rotate_box() {
    if (!trainer::core_ready || !in_field()) return;
    const int party = trainer::api.party_len(), box = trainer::api.box_len();
    if (box == 0) { toast("The PC box is empty"); return; }
    TrMonsterView in;
    trainer::api.box_get(0, &in);
    if (party < 6) {
        trainer::api.withdraw(0);
        char s[80];
        sprintf_s(s, "%s joined the party", in.name);
        toast(s);
        return;
    }
    TrMonsterView out;
    trainer::api.party_get(uint32_t(party - 1), &out);
    if (trainer::api.deposit(uint32_t(party - 1)) == 0 && trainer::api.withdraw(0) == 0) {
        char s[96];
        sprintf_s(s, "%s -> box, %s -> party", out.name, in.name);
        toast(s);
    } else {
        toast("Can't swap: keep one conscious monster in the party");
    }
}

// ---- drawing ------------------------------------------------------------------------------------------------

void bar(HDC dc, int x, int y, int w, int h, int v, int max) {
    HBRUSH back = CreateSolidBrush(RGB(10, 14, 40));
    RECT r{x, y, x + w, y + h};
    FillRect(dc, &r, back);
    DeleteObject(back);
    if (max <= 0) return;
    const int scaled = int(static_cast<long long>(w) * v / max);
    const int fill = v <= 0 ? 0 : (scaled > 1 ? scaled : 1);
    const COLORREF c = v * 4 <= max ? HP_LOW : v * 2 <= max ? HP_MID : HP_OK;
    HBRUSH b = CreateSolidBrush(c);
    RECT f{x, y, x + fill, y + h};
    FillRect(dc, &f, b);
    DeleteObject(b);
}

HFONT hud_font() {
    static HFONT f = CreateFontA(-14, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    return f;
}

int text_width(HDC dc, const char* s) {
    SIZE sz{};
    GetTextExtentPoint32A(dc, s, int(strlen(s)), &sz);
    return sz.cx;
}

// Layout runs twice: once to measure (the window grows to fit its longest line), once to draw at that width.
struct Pen {
    HDC dc;
    bool draw;
    int W;       // final width (draw pass)
    int need;    // width the content needs (measure pass)
    void fit(int right) { if (right + 12 > need) need = right + 12; }
    void text(int x, int y, COLORREF c, const char* s) {
        fit(x + text_width(dc, s));
        if (!draw) return;
        SetTextColor(dc, c);
        TextOutA(dc, x, y, s, int(strlen(s)));
    }
};

int layout(Pen& p, const BattleMon& b, bool toast_on, const char* t) {
    const int line = 18;
    int y = 6;
    char s[128];
    if (ff7::mode() == ff7::BATTLE) {
        if (b.out) {
            // Monsters are out: their own party rows show them.
        } else {
            // Why nobody is out: no room in this battle, everyone fainted, or no monsters at all.
            int32_t act[2] = {-1, -1};
            const bool ready = trainer::core_ready && trainer::api.active(act) > 0 && act[0] >= 0;
            const bool any = trainer::core_ready && trainer::api.party_len() > 0;
            p.text(10, y, DIM, ready ? "No room for a monster here" : any ? "Monsters need rest: inn or bed"
                                                                         : "No monster out: Catch one!");
            y += line;
        }
    } else if (trainer::core_ready) {
        const int n = trainer::api.party_len(), box = trainer::api.box_len();
        const int show = g_roster_open ? n : (n > 0 ? 1 : 0);
        if (n == 0) {
            p.text(10, y, DIM, "No monsters yet: Catch one in battle");
            y += line;
        }
        for (int i = 0; i < show; i++) {
            TrMonsterView v;
            if (trainer::api.party_get(uint32_t(i), &v) != 0) continue;
            sprintf_s(s, "%s%s  Lv %u", i == 0 ? "> " : "  ", v.name, v.level);
            p.text(10, y, v.ko ? HP_LOW : TEXT_FG, s);
            p.fit(10 + text_width(p.dc, s) + 16 + 80);  // the HP bar sits right of the longest name
            if (p.draw) bar(p.dc, p.W - 92, y + 6, 80, 6, v.hp, v.hp_max);
            y += line;
        }
        if (g_roster_open) {
            sprintf_s(s, "PC box: %d   party: %d/6", box, n);
            p.text(10, y, DIM, s);
            y += line;
            p.text(10, y, DIM, "F5 lead   F7 box swap   F6 hide");
            y += line;
        } else if (n > 0) {
            p.text(10, y, DIM, "F6 roster");
            y += line;
        }
    }
    if (toast_on) {
        p.text(10, y, HP_MID, t);
        y += line;
    }
    return y + 6;
}

// measure_only: returns the size the content needs. Otherwise draws it into `width` (the window's client width).
SIZE paint(HDC dc, bool measure_only, int width = 0) {
    BattleMon b;
    AcquireSRWLockExclusive(&g_lock);
    b = g_battle;
    const bool toast_on = GetTickCount() < g_toast_until;
    char t[96];
    strcpy_s(t, g_toast);
    ReleaseSRWLockExclusive(&g_lock);
    HGDIOBJ oldf = SelectObject(dc, hud_font());
    SetBkMode(dc, TRANSPARENT);
    Pen p{dc, false, 0, 220};
    int h = layout(p, b, toast_on, t);
    if (!measure_only) {
        p.draw = true;
        p.W = width > 0 ? width : p.need;
        h = layout(p, b, toast_on, t);
    }
    SelectObject(dc, oldf);
    return SIZE{p.need, h};
}

LRESULT CALLBACK proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(w, &ps);
            RECT rc;
            GetClientRect(w, &rc);
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
            HGDIOBJ oldb = SelectObject(mem, bmp);
            HBRUSH bg = CreateSolidBrush(BG);
            FillRect(mem, &rc, bg);
            DeleteObject(bg);
            HPEN pen = CreatePen(PS_SOLID, 2, FRAME);
            HGDIOBJ oldp = SelectObject(mem, pen);
            HGDIOBJ oldbr = SelectObject(mem, GetStockObject(NULL_BRUSH));
            RoundRect(mem, 1, 1, rc.right - 1, rc.bottom - 1, 8, 8);
            SelectObject(mem, oldbr);
            SelectObject(mem, oldp);
            DeleteObject(pen);
            paint(mem, false, rc.right);
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, oldb);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(w, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
    }
    return DefWindowProcA(w, msg, wp, lp);
}

bool key_pressed(int vk, bool& was_down) {
    const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool edge = down && !was_down;
    was_down = down;
    return edge;
}

DWORD WINAPI hud_thread(void*) {
    for (int i = 0; i < 600 && !g_game; i++) {  // wait up to 60 s for the game window
        EnumWindows(find_game, reinterpret_cast<LPARAM>(&g_game));
        if (!g_game) Sleep(100);
    }
    if (!g_game) {
        tlog("HUD no game window found; HUD disabled");
        return 0;
    }
    WNDCLASSA wc = {};
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "FF7TrainerHUD";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassA(&wc);
    // Owned popup (owner = game window): above FF7 only, never topmost, never activated, click-through.
    g_hud = CreateWindowExA(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, wc.lpszClassName,
                            "FF7 Trainer", WS_POPUP, 0, 0, 270, 60, g_game, nullptr, wc.hInstance, nullptr);
    if (!g_hud) {
        tlog("HUD CreateWindowEx failed (%lu)", GetLastError());
        return 0;
    }
    SetLayeredWindowAttributes(g_hud, 0, 225, LWA_ALPHA);
    tlog("HUD window %p over game window %p", g_hud, g_game);
    bool f5 = false, f6 = false, f7 = false, f8 = false;
    bool shown = false;
    for (;;) {
        MSG m;
        while (PeekMessageA(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageA(&m);
        }
        // Hotkeys only while FF7 itself is the active window (never react to keys meant for other apps).
        if (GetForegroundWindow() == g_game) {
            if (key_pressed(VK_F5, f5)) next_lead();
            if (key_pressed(VK_F6, f6)) g_roster_open = !g_roster_open;
            if (key_pressed(VK_F7, f7)) rotate_box();
            if (key_pressed(VK_F8, f8)) trainer::toggle_trainer_turns();
        } else {
            f5 = f6 = f7 = f8 = false;
        }
        RECT rc{};
        POINT tl{0, 0};
        const bool visible = IsWindowVisible(g_game) && !IsIconic(g_game) && GetClientRect(g_game, &rc) &&
                             ClientToScreen(g_game, &tl) && ff7::mode() != 0;
        const uint16_t mode = ff7::mode();
        const bool relevant = mode == ff7::BATTLE || mode == ff7::FIELD || mode == ff7::WORLDMAP;
        if (visible && relevant && !(mode == ff7::BATTLE && battle_out())) {
            HDC dc = GetDC(g_hud);
            const SIZE sz = paint(dc, true);
            ReleaseDC(g_hud, dc);
            const int x = tl.x + rc.right - sz.cx - 8;
            const int y = tl.y + (mode == ff7::BATTLE ? 8 : rc.bottom - sz.cy - 8);
            SetWindowPos(g_hud, nullptr, x, y, sz.cx, sz.cy, SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
            InvalidateRect(g_hud, nullptr, FALSE);
            shown = true;
        } else if (shown) {
            ShowWindow(g_hud, SW_HIDE);
            shown = false;
        }
        Sleep(100);
    }
}

}  // namespace

void start() {
    static bool once = false;
    if (once) return;
    once = true;
    CloseHandle(CreateThread(nullptr, 0, hud_thread, nullptr, 0, nullptr));
}

void* window() { return g_hud; }

void set_battle_monster(bool out, const char* name, unsigned level, int hp, int hp_max) {
    AcquireSRWLockExclusive(&g_lock);
    g_battle.out = out;
    if (name) strncpy_s(g_battle.name, name, _TRUNCATE);
    g_battle.level = level;
    g_battle.hp = hp;
    g_battle.hp_max = hp_max;
    ReleaseSRWLockExclusive(&g_lock);
}

// Test-mode entry points for the hotkeys (the script can't press real keys).
void action(int fkey) {
    if (fkey == 5) next_lead();
    else if (fkey == 6) g_roster_open = !g_roster_open;
    else if (fkey == 7) rotate_box();
    else if (fkey == 8) trainer::toggle_trainer_turns();
}

}  // namespace hud
