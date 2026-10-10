// Trainer HUD: a small panel over the game's client area showing the monster in battle (name, level, HP bar) and the
// party in the field, plus the roster hotkeys. It is a click-through popup OWNED by the game window (never topmost):
// it sits above FF7 only, hides with it, never takes focus. Hotkeys are read only while FF7 is the foreground window.
//   F5  next lead monster (field only)      F6  show / hide the roster      F7  swap last party monster with the box
//   F8  in battle: Cloud's turn <-> the monster's turn (Catch needs Cloud's own menu)
#pragma once
#include <stdint.h>

namespace hud {

void start();                         // spawn the HUD thread (after the game window exists)
void* window();                       // HUD HWND (for test screenshots), or null

// Battle state the HUD shows; written by the trainer on the game thread.
void set_battle_monster(bool out, const char* name, unsigned level, int hp, int hp_max);

// The hotkey actions (5 next lead, 6 roster, 7 box swap, 8 trainer/monster turns); test scripts call these since they can't press real keys.
void action(int fkey);

}  // namespace hud
