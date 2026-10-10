#include "text.h"
#include <string.h>
#include "game.h"
#include "log.h"

namespace text {
namespace {

using Kernel2GetText = char*(__cdecl*)(uint32_t section_base, uint32_t string_id, uint32_t section_offset);
const Kernel2GetText kernel2_get_text = reinterpret_cast<Kernel2GetText>(0x419457);

int ff_len(const uint8_t* s) {
    int n = 0;
    while (n < 256 && s[n] != 0xFF) n++;
    return n;
}

void encode(uint8_t* dst, int cap, const char* s) {
    int n = 0;
    for (; s[n] && n < cap; n++) {
        const unsigned char c = static_cast<unsigned char>(s[n]);
        dst[n] = (c >= 0x20 && c < 0x80) ? uint8_t(c - 0x20) : 0;
    }
    for (int i = n; i < cap; i++) dst[i] = 0xFF;  // terminate and blank any leftover of the old string
    if (cap >= 0) dst[n < cap ? n : cap] = 0xFF;
}

// The game's battle text queue (add_text_to_display_queue fills it, 0x5BE052 drains it): 64 entries of 6 bytes,
// text id -1 = free. Entry 0 is the one on screen; it stays queued while shown, and the bar draws its kernel2 string
// every frame, so a slot must not be rewritten while any entry still names it.
constexpr uintptr_t TEXT_QUEUE = 0xBF1EB8;
constexpr int TEXT_QUEUE_LEN = 64;

bool slot_in_use(uint16_t slot) {
    for (int i = 0; i < TEXT_QUEUE_LEN; i++) {
        const int16_t id = *reinterpret_cast<const int16_t*>(TEXT_QUEUE + i * 6);
        if (id == -1) break;  // the queue is packed from entry 0
        if (uint16_t(id) == slot) return true;
    }
    return false;
}

}  // namespace

bool rewrite(uint32_t section, uint16_t id, const char* s) {
    uint8_t* p = reinterpret_cast<uint8_t*>(kernel2_get_text(section, id, 0));
    if (!p) return false;
    const int cap = ff_len(p);
    if (cap == 0) return false;
    char before[64];
    ff7::ff_text(p, cap < 63 ? cap : 63, before);
    encode(p, cap, s);
    tlog("TEXT kernel2[%u][%u] '%s' -> '%s'", section, id, before, s);
    return true;
}

// Real capacity of a message slot = the length of the string the game shipped there (measured once, before we
// ever write it; later lengths would only reflect our own shorter messages). Never more than our compile-time cap.
int slot_capacity(uint16_t slot, int max) {
    static int cap55 = -1, cap58 = -1;
    int& cap = slot == MESSAGE_SLOT ? cap55 : cap58;
    if (cap < 0) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(kernel2_get_text(BATTLE_TEXT, slot, 0));
        cap = p ? ff_len(p) : 0;
        if (cap > max) cap = max;
        tlog("TEXT message slot %u holds %d characters", slot, cap);
    }
    return cap;
}

uint16_t set_message(const char* s) {
    // Not over a message of ours that is still waiting on the bar, if the short slot is free and fits.
    const bool second = slot_in_use(MESSAGE_SLOT) && !slot_in_use(MESSAGE_SLOT_2) && strlen(s) <= size_t(MESSAGE_MAX_2);
    const uint16_t slot = second ? MESSAGE_SLOT_2 : MESSAGE_SLOT;
    const int cap = slot_capacity(slot, second ? MESSAGE_MAX_2 : MESSAGE_MAX);
    uint8_t* p = reinterpret_cast<uint8_t*>(kernel2_get_text(BATTLE_TEXT, slot, 0));
    if (p && cap > 0) encode(p, cap, s);
    return slot;
}

namespace {

// Show `s` in a slot nothing on the bar still uses; false = both busy (try again next frame). `force`: use the
// long slot regardless (the game's queue has not moved for a long time).
bool show_now(const char* s, bool force) {
    using AddText = void(__cdecl*)(uint32_t idx, uint32_t wait, uint32_t frames, uint32_t x);
    uint16_t slot = MESSAGE_SLOT;
    int max = MESSAGE_MAX;
    if (!force && slot_in_use(MESSAGE_SLOT)) {
        if (strlen(s) > size_t(MESSAGE_MAX_2) || slot_in_use(MESSAGE_SLOT_2)) return false;
        slot = MESSAGE_SLOT_2;  // short messages may go through the second slot
        max = MESSAGE_MAX_2;
    }
    const int cap = slot_capacity(slot, max);
    uint8_t* p = cap > 0 ? reinterpret_cast<uint8_t*>(kernel2_get_text(BATTLE_TEXT, slot, 0)) : nullptr;
    if (!p) return true;  // nowhere to show it: drop it rather than block the queue
    encode(p, cap, s);
    // Same arguments the game uses for its own result texts (Steal: wait 1, frames 0, x 0).
    reinterpret_cast<AddText>(ff7::FN_ADD_TEXT)(slot, 1, 0, 0);
    tlog("MSG [%u] %s (queued: %s)", slot, s, slot_in_use(slot) ? "yes" : "NO");
    return true;
}

// Our messages, shown one at a time: the bar reads a slot's string while it shows it, not when it is queued, so a
// burst (evolved / reached Lv / learned x2) through two slots overwrote messages before they were seen. Each one
// waits for a slot the game's queue no longer names (and at least GAP_FRAMES after the one before).
constexpr int QUEUE = 16;
constexpr int GAP_FRAMES = 100;     // ~1.7 s per message
constexpr int BLOCKED_MAX = 600;    // ~10 s waiting on the game's queue: show it anyway, never stall the battle
char g_queue[QUEUE][64];
int g_head = 0, g_count = 0, g_since = GAP_FRAMES, g_blocked = 0;

}  // namespace

void show_battle_message(const char* s) {
    if (g_count == QUEUE) {
        tlog("MSG queue full, dropped: %s", s);
        return;
    }
    strncpy_s(g_queue[(g_head + g_count) % QUEUE], s, _TRUNCATE);
    g_count++;
}

void pump_battle_messages() {
    if (g_since < GAP_FRAMES) g_since++;
    if (g_count == 0 || g_since < GAP_FRAMES) return;
    if (!show_now(g_queue[g_head], g_blocked >= BLOCKED_MAX)) {
        g_blocked++;
        return;
    }
    if (g_blocked >= BLOCKED_MAX) tlog("MSG the bar's queue did not move for %d frames", g_blocked);
    g_blocked = 0;
    g_head = (g_head + 1) % QUEUE;
    g_count--;
    g_since = 0;
}

bool battle_messages_pending() { return g_count > 0 || g_since < GAP_FRAMES; }

void reset_battle_messages() {
    g_head = g_count = g_blocked = 0;
    g_since = GAP_FRAMES;
}

void rename_capture_kit() {
    // kernel2 must be loaded; retry on a later field frame if it isn't yet. The game reloads kernel2 on the way back
    // to the title (Game Over), restoring "Steal", so check what is there every time instead of latching once.
    const uint8_t* cur = reinterpret_cast<const uint8_t*>(kernel2_get_text(COMMAND_NAMES, ff7::CMD_STEAL, 0));
    if (!cur) return;
    char name[16];
    const int n = ff_len(cur);
    ff7::ff_text(cur, n < 15 ? n : 15, name);
    if (strcmp(name, "Catch") == 0) return;
    rewrite(COMMAND_NAMES, ff7::CMD_STEAL, "Catch");
    rewrite(MATERIA_NAMES, ff7::MATERIA_STEAL, "Catch");
    // In-place rewrites can't grow: "Steal item" has room for 10 characters, the materia help for 22.
    rewrite(COMMAND_DESCRIPTIONS, ff7::CMD_STEAL, "Catch foe");
    rewrite(MATERIA_DESCRIPTIONS, ff7::MATERIA_STEAL, "Equips Catch command");
}

}  // namespace text
