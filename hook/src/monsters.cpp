#include "monsters.h"
#include <windows.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "game.h"
#include "log.h"
#include "trainer.h"

namespace monsters {
namespace {

// ---- the game's menu engine (ff7_en 1.02; FFNx names in brackets) -------------------------------------------------
// The main menu runs one submenu function per frame from a 16-entry table [menu_subs_call_table], indexed by the
// submenu id; its cursor maps entry -> id through 0x91AAF8 (PHS = entry 8 = id 9 = 0x700C90). Only the main menu
// goes through this table: a field script's PHS (menu type -> 0x6CBD43 -> 0x701EE4) calls 0x700C90 directly, so the
// story's party selection is untouched.
constexpr uintptr_t SUBMENU_TABLE = 0x91AB98;
constexpr int PHS_ID = 9;
constexpr uintptr_t FN_PHS = 0x700C90;
constexpr uintptr_t PHS_LABEL = 0x91A960;  // main-menu entry names, 20 bytes each, FF7 text
constexpr int LABEL_SIZE = 20;

struct Rect {
    int16_t x, y, w, h;
};
using SetRect = void(__cdecl*)(Rect*, int, int, int, int);
using DrawWindow = void(__cdecl*)(Rect*, float);
using DrawText = void(__cdecl*)(int x, int y, const uint8_t* text, int color, float z);
using Pressed = int(__cdecl*)(uint32_t mask);
using PlaySound = void(__cdecl*)(int id);
using Busy = int(__cdecl*)();
using Transition = void(__cdecl*)(int kind, int target);
using ResetCursor = void(__cdecl*)(int);
using Submenu = void(__cdecl*)(int);

const SetRect set_rect = reinterpret_cast<SetRect>(0x6E7BA1);
const DrawWindow draw_window = reinterpret_cast<DrawWindow>(0x6EDE1D);
const DrawText draw_text = reinterpret_cast<DrawText>(0x6F5B03);
using DrawHand = void(__cdecl*)(int x, int y, float z);
const DrawHand draw_hand = reinterpret_cast<DrawHand>(0x6EB3B8);  // the menus' pointing-hand cursor
const Pressed pressed = reinterpret_cast<Pressed>(0x41AB74);       // this frame's new presses
const Pressed repeating = reinterpret_cast<Pressed>(0x6F53F1);     // held with auto-repeat (cursor movement)
const PlaySound sound = reinterpret_cast<PlaySound>(0x74580A);
const Busy transitioning = reinterpret_cast<Busy>(0x6C9808);
const Transition transition = reinterpret_cast<Transition>(0x6C9812);
const ResetCursor reset_cursor = reinterpret_cast<ResetCursor>(0x6C6AEE);
// Every submenu calls one of these first, every frame: they run the open/close transition (and draw the page's title
// box at the top right from the main-menu label). Which one depends on the menu's mode flag 0xDC130C.
const Submenu run_transition = reinterpret_cast<Submenu>(0x6C98A6);
const Submenu run_transition_alt = reinterpret_cast<Submenu>(0x6C9D07);

// Menu buttons (PlayStation layout, as the game maps keyboard and pad).
constexpr uint32_t B_MENU = 0x10, B_OK = 0x20, B_CANCEL = 0x40, B_SWITCH = 0x80;
constexpr uint32_t B_L1 = 0x04, B_R1 = 0x08, B_UP = 0x1000, B_RIGHT = 0x2000, B_DOWN = 0x4000, B_LEFT = 0x8000;
constexpr int SND_CURSOR = 1, SND_ERROR = 3, SND_CANCEL = 4;

// Text colours (the game's palette indices).
constexpr int C_WHITE = 7, C_GREY = 0, C_CYAN = 5, C_YELLOW = 6, C_RED = 2, C_GREEN = 4;

// Savemap: main-menu entry visibility (u16) and lock (u16) masks, bit = entry.
inline volatile uint16_t& menu_visible() { return *reinterpret_cast<volatile uint16_t*>(0xDC08F8); }
inline volatile uint16_t& menu_locked() { return *reinterpret_cast<volatile uint16_t*>(0xDC08FA); }
constexpr uint16_t PHS_BIT = 1u << 8;

// ---- text -----------------------------------------------------------------------------------------------------------

struct FFText {
    uint8_t b[64];
    explicit FFText(const char* s) {
        int i = 0;
        for (; s[i] && i < 63; i++) {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            b[i] = (c >= 0x20 && c < 0x7F) ? uint8_t(c - 0x20) : 0;
        }
        b[i] = 0xFF;
    }
};

void text(int x, int y, int color, const char* fmt, ...) {
    char s[96];
    va_list a;
    va_start(a, fmt);
    vsprintf_s(s, fmt, a);
    va_end(a);
    FFText t(s);
    draw_text(x, y, t.b, color, 0.0f);
}

// The hand points at text drawn at (x, y): it sits left of it, as in the main menu.
void hand(int x, int y) { draw_hand(x - 40, y + 2, 0.0001f); }

void window(int x, int y, int w, int h) {
    Rect r;
    set_rect(&r, x, y, w, h);
    draw_window(&r, 0.0f);
}

// ---- state ----------------------------------------------------------------------------------------------------------

enum class Page { List, Moves };

struct State {
    Page page = Page::List;
    int cursor = 0;         // index into the combined list: party 0..n-1, then box
    int top = 0;            // first visible row
    int held = -1;          // combined index picked up with OK, waiting for its destination
    int move_slot = 0;      // Moves page: equipped slot being edited (0..3)
    int move_cursor = 0;    // Moves page: row in the known list
    bool editing = false;   // Moves page: a slot is picked, choosing what goes in it
    char note[64] = {};
    DWORD note_until = 0;
    DWORD last_frame = 0;
} g;

constexpr int LIST_ROWS = 14;

int party_len() { return trainer::core_ready ? trainer::api.party_len() : 0; }
int box_len() { return trainer::core_ready ? trainer::api.box_len() : 0; }
int total() { return party_len() + box_len(); }
bool in_box(int i) { return i >= party_len(); }
int list_index(int i) { return in_box(i) ? i - party_len() : i; }

bool get(int i, TrMonsterDetail& d) {
    return trainer::core_ready && trainer::api.monster_detail(in_box(i) ? 1 : 0, uint32_t(list_index(i)), &d) == 0;
}

void note(const char* s) {
    strcpy_s(g.note, s);
    g.note_until = GetTickCount() + 2500;
    tlog("MONSTERS %s", s);
}

// ---- actions --------------------------------------------------------------------------------------------------------

// Move the held monster to `to`: party<->party swaps order, party<->box trades places, box<->box swaps.
bool place(int from, int to) {
    if (from == to) return true;
    auto& api = trainer::api;
    const int pn = party_len();
    if (!in_box(from) && !in_box(to)) return api.swap(uint32_t(from), uint32_t(to)) == 0;
    if (in_box(from) && in_box(to)) {
        // box order isn't meaningful to the core's API; leave it
        return false;
    }
    const int p = in_box(from) ? to : from;     // party index
    const int b = in_box(from) ? from - pn : to - pn;  // box index
    // Trade: the box monster joins at the end, then takes the party monster's place; the party monster leaves.
    if (api.withdraw(uint32_t(b)) != 0) {
        // party full: deposit first, then withdraw (indices shift)
        if (api.deposit(uint32_t(p)) != 0) return false;
        if (api.withdraw(uint32_t(b)) != 0) return false;  // box index b unchanged (the deposit went to the end)
        const int last = party_len() - 1;
        for (int k = last; k > p; k--) api.swap(uint32_t(k), uint32_t(k - 1));
        return true;
    }
    const int last = party_len() - 1;  // the newcomer
    api.swap(uint32_t(p), uint32_t(last));
    return api.deposit(uint32_t(last)) == 0;
}

void list_input() {
    const int n = total();
    if (n == 0) return;
    if (repeating(B_UP)) { g.cursor = (g.cursor + n - 1) % n; sound(SND_CURSOR); }
    if (repeating(B_DOWN)) { g.cursor = (g.cursor + 1) % n; sound(SND_CURSOR); }
    if (repeating(B_L1) || repeating(B_LEFT)) { g.cursor = g.cursor - LIST_ROWS < 0 ? 0 : g.cursor - LIST_ROWS; sound(SND_CURSOR); }
    if (repeating(B_R1) || repeating(B_RIGHT)) { g.cursor = g.cursor + LIST_ROWS >= n ? n - 1 : g.cursor + LIST_ROWS; sound(SND_CURSOR); }
    if (pressed(B_OK)) {
        if (g.held < 0) {
            g.held = g.cursor;
            sound(SND_CURSOR);
        } else {
            TrMonsterDetail a, b;
            const bool ga = get(g.held, a), gb = get(g.cursor, b);
            if (place(g.held, g.cursor)) {
                sound(SND_CURSOR);
                char s[64];
                if (ga && gb && g.held != g.cursor) sprintf_s(s, "%s <-> %s", a.view.name, b.view.name);
                else s[0] = 0;
                if (s[0]) note(s);
            } else {
                sound(SND_ERROR);
                note(in_box(g.held) && in_box(g.cursor) ? "Pick a party monster to trade with"
                                                        : "Keep at least one monster that can fight");
            }
            g.held = -1;
        }
    }
    if (pressed(B_MENU) && g.held < 0) {  // Triangle: this monster's moves
        g.page = Page::Moves;
        g.move_slot = 0;
        g.move_cursor = 0;
        g.editing = false;
        sound(SND_CURSOR);
    }
}

void moves_input() {
    TrMonsterDetail d;
    if (!get(g.cursor, d)) { g.page = Page::List; return; }
    const int known = d.known_count;
    if (!g.editing) {
        const int slots = d.equipped_count < 4 ? d.equipped_count + (known > d.equipped_count ? 1 : 0) : 4;
        if (slots == 0) return;
        if (repeating(B_UP)) { g.move_slot = (g.move_slot + slots - 1) % slots; sound(SND_CURSOR); }
        if (repeating(B_DOWN)) { g.move_slot = (g.move_slot + 1) % slots; sound(SND_CURSOR); }
        if (pressed(B_OK)) {
            if (known <= 1) { sound(SND_ERROR); note("No other moves known yet"); return; }
            g.editing = true;
            g.move_cursor = 0;
            sound(SND_CURSOR);
        }
    } else {
        if (repeating(B_UP)) { g.move_cursor = (g.move_cursor + known - 1) % known; sound(SND_CURSOR); }
        if (repeating(B_DOWN)) { g.move_cursor = (g.move_cursor + 1) % known; sound(SND_CURSOR); }
        if (pressed(B_OK)) {
            const uint16_t id = d.moves[g.move_cursor].id;
            if (trainer::api.equip_move(in_box(g.cursor) ? 1 : 0, uint32_t(list_index(g.cursor)), uint32_t(g.move_slot), id) == 0) {
                sound(SND_CURSOR);
                char s[64];
                sprintf_s(s, "%s learned to use %s", d.view.name, d.moves[g.move_cursor].name);
                note(s);
            } else {
                sound(SND_ERROR);
            }
            g.editing = false;
        }
    }
}

// ---- drawing --------------------------------------------------------------------------------------------------------

void hp_line(int x, int y, const char* label, int v, int max) {
    text(x, y, C_CYAN, "%s", label);
    const int color = max > 0 && v * 4 <= max ? (v <= 0 ? C_RED : C_YELLOW) : C_WHITE;
    text(x + 50, y, color, "%4d", v);
    text(x + 100, y, C_WHITE, "/%4d", max);
}

void draw_detail(const TrMonsterDetail& d, int x, int y) {
    const TrMonsterView& v = d.view;
    text(x, y, C_WHITE, "%s", v.name);
    text(x + 230, y, C_CYAN, "LV");
    text(x + 262, y, C_WHITE, "%u", v.level);
    y += 26;
    if (strcmp(v.name, d.species_name) != 0) { text(x, y, C_GREY, "%s", d.species_name); y += 24; }
    text(x, y, C_CYAN, "Type");
    text(x + 70, y, C_WHITE, "%s", d.types[0] ? d.types : "Normal");
    y += 24;
    if (d.weak[0]) {
        text(x, y, C_CYAN, "Weak");
        text(x + 70, y, C_WHITE, "%s", d.weak);
        y += 24;
    }
    y += 4;
    hp_line(x, y, "HP", v.ko ? 0 : v.hp, v.hp_max);
    y += 24;
    hp_line(x, y, "MP", v.mp, v.mp_max);
    y += 24;
    text(x, y, C_CYAN, "EXP");
    text(x + 70, y, C_WHITE, "%u", d.xp);
    if (d.xp_next) {
        text(x + 180, y, C_CYAN, "Next");
        text(x + 240, y, C_WHITE, "%u", d.xp_next > d.xp ? d.xp_next - d.xp : 0);
    }
    y += 30;
    static const char* names[6] = {"Strength", "Defense", "Magic", "M.Defense", "Speed", "Luck"};
    for (int i = 0; i < 6; i++) {
        const int cx = x + (i % 2) * 180, cy = y + (i / 2) * 24;
        text(cx, cy, C_CYAN, "%s", names[i]);
        text(cx + 120, cy, C_WHITE, "%3u", v.stats[i]);
    }
    y += 3 * 24 + 8;
    text(x, y, C_CYAN, "Moves");
    y += 24;
    for (int i = 0; i < d.equipped_count && i < 4; i++) {
        const TrMoveLine& m = d.moves[i];
        text(x + 10, y, C_WHITE, "%s", m.name);
        if (m.mp) text(x + 200, y, C_WHITE, "%2u MP", m.mp);
        if (m.element[0]) text(x + 270, y, C_GREY, "%.10s", m.element);
        y += 22;
    }
    if (d.equipped_count == 0) { text(x + 10, y, C_GREY, "(none)"); y += 22; }
    y += 6;
    if (d.next_learn_level) { text(x, y, C_GREY, "Lv %u: learns %s", d.next_learn_level, d.next_learn); y += 22; }
    if (d.evolve_level) { text(x, y, C_YELLOW, "Lv %u: evolves into %s", d.evolve_level, d.evolve_into); y += 22; }
}

void draw_list_page() {
    const int pn = party_len(), n = total();
    if (g.cursor >= n) g.cursor = n ? n - 1 : 0;
    if (g.cursor < g.top) g.top = g.cursor;
    if (g.cursor >= g.top + LIST_ROWS) g.top = g.cursor - LIST_ROWS + 1;
    // header
    text(18, 14, C_CYAN, "Party");
    text(88, 14, C_WHITE, "%d/6", pn);
    text(160, 14, C_CYAN, "PC Box");
    text(250, 14, C_WHITE, "%d", box_len());
    // list
    int y = 64;
    if (n == 0) text(24, y, C_GREY, "None yet: Catch one!");
    for (int r = 0; r < LIST_ROWS && g.top + r < n; r++) {
        const int i = g.top + r;
        TrMonsterDetail d;
        if (!get(i, d)) continue;
        if (i == pn && pn > 0) { text(20, y, C_CYAN, "PC Box"); y += 22; }
        const int color = d.view.ko ? C_RED : (in_box(i) ? C_GREY : C_WHITE);
        if (i == g.cursor) hand(44, y);
        if (i == g.held) text(32, y, C_YELLOW, "*");
        text(44, y, color, "%.13s", d.view.name);
        text(180, y, C_WHITE, "%2u", d.view.level);
        if (!in_box(i) && i < 2) text(212, y, C_GREEN, i == 0 ? "1st" : "2nd");
        y += 22;
    }
    TrMonsterDetail cur;
    if (n > 0 && get(g.cursor, cur)) draw_detail(cur, 270, 64);
    // help
    const char* help = g.held >= 0 ? "OK: put it here    Cancel: drop it"
                                   : "OK: move/trade    Triangle: moves    Cancel: back";
    text(18, 446, C_GREY, "%s", help);
    if (GetTickCount() < g.note_until) text(18, 420, C_YELLOW, "%s", g.note);
    // windows (drawn after the text, as the game's own menus do)
    window(0, 0, 640, 46);
    window(0, 46, 256, 392);
    window(256, 46, 384, 392);
    window(0, 438, 640, 42);
}

void draw_moves_page() {
    TrMonsterDetail d;
    if (!get(g.cursor, d)) return;
    text(18, 14, C_WHITE, "%s's moves", d.view.name);
    text(44, 64, C_CYAN, "In battle");
    int y = 92;
    const int slots = d.equipped_count < 4 ? d.equipped_count + (d.known_count > d.equipped_count ? 1 : 0) : 4;
    for (int s = 0; s < slots; s++) {
        if (s == g.move_slot) { if (g.editing) text(30, y, C_YELLOW, "*"); else hand(44, y); }
        if (s < d.equipped_count) {
            text(44, y, C_WHITE, "%s", d.moves[s].name);
            if (d.moves[s].mp) text(200, y, C_WHITE, "%2u MP", d.moves[s].mp);
        } else {
            text(44, y, C_GREY, "(empty)");
        }
        y += 24;
    }
    text(300, 64, C_CYAN, "Known");
    y = 92;
    for (int k = 0; k < d.known_count; k++) {
        const TrMoveLine& m = d.moves[k];
        if (g.editing && k == g.move_cursor) hand(300, y);
        text(300, y, k < d.equipped_count ? C_WHITE : C_GREY, "%s", m.name);
        if (m.mp) text(470, y, C_WHITE, "%2u MP", m.mp);
        if (m.power) text(540, y, C_GREY, "Pw %u", m.power);
        y += 24;
    }
    text(18, 446, C_GREY, "%s", g.editing ? "OK: use this move    Cancel: back" : "OK: change this move    Cancel: back");
    if (GetTickCount() < g.note_until) text(18, 420, C_YELLOW, "%s", g.note);
    window(0, 0, 640, 46);
    window(0, 46, 256, 392);
    window(256, 46, 384, 392);
    window(0, 438, 640, 42);
}

void __cdecl monsters_submenu(int ctx) {
    const DWORD now = GetTickCount();
    if (now - g.last_frame > 500) {
        tlog("MONSTERS open (party %d, box %d)", party_len(), box_len());
        g.page = Page::List;
        g.held = -1;
        g.editing = false;
        g.note[0] = 0;
    }
    g.last_frame = now;
    (void)ctx;
    const int phase = *reinterpret_cast<volatile int*>(0xDC1210);
    if (*reinterpret_cast<volatile int*>(0xDC130C) == 1) run_transition_alt(phase);
    else run_transition(phase);
    if (!transitioning()) {
        if (pressed(B_CANCEL)) {
            sound(SND_CANCEL);
            if (g.page == Page::Moves && g.editing) g.editing = false;
            else if (g.page == Page::Moves) g.page = Page::List;
            else if (g.held >= 0) g.held = -1;
            else {
                transition(5, 0);  // back to the main menu, as every submenu's Cancel does
                reset_cursor(0);
            }
        } else if (g.page == Page::List) {
            list_input();
        } else {
            moves_input();
        }
    }
    if (g.page == Page::List) draw_list_page();
    else draw_moves_page();
}

}  // namespace

void install() {
    static bool once = false;
    if (once) return;
    once = true;
    auto* table = reinterpret_cast<uintptr_t*>(SUBMENU_TABLE);
    if (table[PHS_ID] != FN_PHS) {
        tlog("MONSTERS submenu table[%d] = %08X (expected %08X): not installed", PHS_ID, unsigned(table[PHS_ID]),
             unsigned(FN_PHS));
        return;
    }
    DWORD old;
    VirtualProtect(&table[PHS_ID], 4, PAGE_READWRITE, &old);
    table[PHS_ID] = reinterpret_cast<uintptr_t>(&monsters_submenu);
    VirtualProtect(&table[PHS_ID], 4, old, &old);
    // "PHS" -> "Monsters" in the main menu
    uint8_t* label = reinterpret_cast<uint8_t*>(PHS_LABEL);
    if (label[0] == 'P' - 0x20 && label[1] == 'H' - 0x20 && label[2] == 'S' - 0x20 && label[3] == 0xFF) {
        FFText t("Monsters");
        VirtualProtect(label, LABEL_SIZE, PAGE_READWRITE, &old);
        memcpy(label, t.b, 9);
        VirtualProtect(label, LABEL_SIZE, old, &old);
    }
    tlog("MONSTERS installed: main menu PHS -> Monsters page");
}

// Keep the entry selectable whatever the story has done to PHS (called from the field tick).
void keep_entry_open() {
    if (!(menu_visible() & PHS_BIT)) menu_visible() |= PHS_BIT;
    if (menu_locked() & PHS_BIT) menu_locked() &= uint16_t(~PHS_BIT);
}

}  // namespace monsters
