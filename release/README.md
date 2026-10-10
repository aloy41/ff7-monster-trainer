# FINAL FANTASY VII: Monster Trainer

Cloud is a trainer. Every regular enemy in FF7 can be caught, raised, and sent into battle, where it fights with its
own moves, model, and animations while Cloud commands it. Barret, Tifa and the rest stay in the story but never fight.

## Install

1. Own **FINAL FANTASY VII** on Steam (the 2026 release) and have **Python 3.10+** installed (used once, to read
   your game's monster data).
2. Double-click **Setup.cmd**. It makes a separate copy of the game called **FF7 Monster Trainer** (about 3.5 GB,
   next to your Steam library) with a copy of this README, and a Desktop shortcut. Your Steam install is never
   changed.
3. Start Steam, then launch **FF7 Monster Trainer** from the Desktop.

To uninstall, delete the FF7 Monster Trainer folder and the shortcut.

## How to play

| | |
|---|---|
| **Catch** | Cloud gets the **Catch** materia once per game (it takes Ice's slot; Ice moves to your materia list). In battle, choose **Catch** on an enemy. Catch never steals items. Weakened monsters are easier: at full HP a low-level monster is about 30%, at low HP about 3 times that. Sleep, Stop or Paralysis help more. Bosses can't be caught, and neither can Eye, Yang and Grosspanzer·Mobile (their moves can't be used by a monster fighting for Cloud). |
| **Your monster fights** | The lead monster is sent out automatically ("Go, Mono Drive!"). On Cloud's turn you choose the monster's move (up to 3). Every move hits one enemy, even moves that hit a whole group in the original game. Enemies attack it too. |
| **F8** | Switch Cloud's turn between **commanding the monster** and **Cloud's own menu** (Catch, items, Defend). Pressed while a command is still playing out, it takes effect right after. |
| **F5** | Next lead monster (outside battle). |
| **F6** | Show or hide the roster panel. |
| **F7** | Swap the last party monster with the next one in the PC box (outside battle). |
| **HUD** | Top right in battle: the monster's level and HP. Bottom right in the field: your lead monster (F6 for the full party). |

- **Party:** up to 6 monsters; more go to the PC box.
- **Growing:** the monster that fought gets the battle's EXP and the rest of the party half. Monsters learn their
  species' moves as they level and some evolve (Mono Drive becomes Scrutin Eye at Lv 15, Guard Hound becomes Blood
  Taste, and so on).
- **Healing:** saving the game, or resting at an inn or a bed, heals every monster, fainted ones included.
- **Fainting:** a monster at 0 HP faints and Cloud fights on alone; the next conscious monster leads the next
  battle.
- **If Cloud falls, it's Game Over,** even if the monster still stands. Keep Cloud alive (Defend, Potions).
- **No room:** a few battles have no space for another monster (all six spots, or all three enemy kinds, are
  taken). Cloud fights alone there ("... can't join this battle!").
- **Saves:** your monsters are saved together with each game save (`save\trainer\trainer_slotNN.json` next to
  the game's own saves) and come back when you load that save.
- The HUD hotkeys only react while the game window is active.

## Notes

- Runs the classic FF7 engine through **FFNx** 1.24.3 (as FFNx documents for this release); the 64-bit FFVII.exe
  launcher and its DRM are not used or touched.
- A log is written to `ff7\workingdir\trainer\trainer.log` if something goes wrong.
- Fan project for personal use. Not affiliated with Square Enix, Nintendo or The Pokemon Company. No game files are
  included; the monster data is built from your own copy of the game.
