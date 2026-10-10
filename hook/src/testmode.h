// Test mode: drive the game with a script and never touch the desktop.
// Active only when <game>\trainer\test_script.txt exists. Then:
//  - the game window is created off-screen and is never activated or brought to the front
//    (IAT hooks on CreateWindowExA / ShowWindow / SetForegroundWindow / SetActiveWindow / SetFocus /
//    BringWindowToTop in ff7_en.exe and AF3DN.P);
//  - the real keyboard is ignored; the script's keys are fed into the game's own DirectInput keyboard state
//    (vtable hook on the keyboard device's GetDeviceState/Acquire), so nothing needs focus;
//  - the game's audio is muted (its own audio session only);
//  - `shot` captures the game window with PrintWindow into trainer\shots\<name>.bmp.
//
// Script (one command per line, # comments):
//   wait <ms>                       sleep
//   press <key> [ms]                key down for ms (default 120), then up for 120 ms
//   hold <key> <ms>                 key down for ms
//   waitmode <mode> [timeout_ms]    until ff7 mode == <mode> (1 field, 2 battle, 5 menu, 20 main menu)
//   waitnotmode <mode> [timeout_ms]
//   whilemode <mode> <timeout_ms> ... end   repeat the block while mode == <mode> (nestable)
//   ffnx skip|speedup|speeddown|speedtoggle   FFNx Ctrl hotkeys, posted to the game window
//   dump <hexaddr> <len>                       hex dump into the log
//   write8|write16|write32|or32|andnot32 <hexaddr> <value>   poke game memory (experiments)
//   core load <slot> | core new                 roster from save\trainer\trainer_slotNN.json, or empty
//   hud f5|f6|f7                                the HUD hotkey actions (next lead / roster / box swap)
//   fieldop <hex bytes...>                      run one field opcode in the current field (49 = MENU, 60 = MAPJUMP)
//   shot <name>                                 '#' in the name = running number
//   sweep on|off                                each battle, the monster uses each of its moves once (forced),
//                                               nobody falls until all have played out, then the wild side falls
//   quitifmode <mode> | quitifnotmode <mode>    exit the game now if mode is (isn't) <mode> (a hung battle)
//   log <text>
//   quit                            flush the log and exit the game
// lab/sweep_driver.ps1 runs the full move sweep (every move of every catchable species) with these.
// Keys: ok cancel menu switch start up down left right pgup pgdn camera target, or 0xNN (DIK code).
#pragma once
#include <stdint.h>

namespace testmode {

bool enabled();
void init_early(const char* game_dir);           // DllMain: decide on test mode, install window IAT hooks
void wrap_dinput(void** direct_input);           // after the real DirectInputCreateA succeeded
void start_script_thread();                      // once the engine is up

}  // namespace testmode
