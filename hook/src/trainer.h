#pragma once
#include "../trainer_core.h"

namespace trainer {

extern const char* BUILD;
extern TrApi api;          // filled once trainer_core.dll is loaded; check `core_ready` first
extern bool core_ready;

void install_hooks();                     // DllMain: patch call sites (memory writes only)
void on_engine_ready(const char* game_dir);  // first DirectInputCreateA: load trainer_core.dll + species.json
void monitor_loop();                      // background thread: mode changes, actor HP, actions (read only)

// F8 in battle: Cloud's turns go to the trainer's own menu (Attack / Magic / Catch / Item) instead of the monster.
// The monster stays out, idle, still manipulated (its AI never runs). Toggle back to command it again.
void toggle_trainer_turns();
bool trainer_turns();

// Test mode only (lab/sweep_moves.ps1): each turn the monster uses its next equipped move, and the battle can't end
// until every one of them has run once, so one battle exercises the monster's whole move list.
extern volatile bool test_sweep;

}  // namespace trainer
