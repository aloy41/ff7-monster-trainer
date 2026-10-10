// In-game text: rename kernel strings and show our own battle messages.
// FFNx owns the kernel2 text sections (its kernel2_get_text replaces the game's at 0x419457) and keeps them in its own
// heap; we look strings up through that function and rewrite them in place, never longer than the original.
#pragma once
#include <stdint.h>

namespace text {

// kernel2 section indices (kernel.bin sections 9..26 -> 0..17)
enum Section : uint32_t { COMMAND_DESCRIPTIONS = 0, MATERIA_DESCRIPTIONS = 6, COMMAND_NAMES = 8, MATERIA_NAMES = 14,
                          BATTLE_TEXT = 16 };

// Battle-text entry we own for custom messages ("Select colors for each corner of the window": 43 chars, config help
// that the battle never displays).
constexpr uint16_t MESSAGE_SLOT = 55;
constexpr int MESSAGE_MAX = 43;

// Rewrite string `id` of `section` with ASCII `s` (truncated to the original's length). Returns false if missing.
bool rewrite(uint32_t section, uint16_t id, const char* s);

// Second slot ("Save cursor when window is closed?": 34 chars) so two messages can be queued back to back.
constexpr uint16_t MESSAGE_SLOT_2 = 58;
constexpr int MESSAGE_MAX_2 = 34;

// Put `s` into MESSAGE_SLOT (truncated to MESSAGE_MAX). Returns MESSAGE_SLOT.
uint16_t set_message(const char* s);

// Queue `s` for the battle message bar (game thread, battle only). Messages are shown one at a time by
// pump_battle_messages (call once per battle frame), alternating between the two slots.
void show_battle_message(const char* s);
void pump_battle_messages();
bool battle_messages_pending();  // something queued or still on screen
void reset_battle_messages();    // new battle

// Steal -> Catch everywhere the player reads it. Idempotent; call once kernel2 is loaded (first field frame).
void rename_capture_kit();

}  // namespace text
