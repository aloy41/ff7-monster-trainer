// The Monsters page of the main menu: it takes over the PHS entry (companions never fight in this mod, so choosing a
// battle party has nothing left to do). Drawn with the game's own menu windows and text, driven by the game's own
// menu input, so it looks native and works with the keyboard and a controller alike. The story's field scripts that
// open PHS (party selection) call it directly and still get the original.
#pragma once

namespace monsters {

void install();          // once the engine is up (patches the main menu's submenu table and the "PHS" label)
void keep_entry_open();  // field tick: the Monsters entry stays visible and unlocked

}  // namespace monsters
