# FINAL FANTASY VII: Monster Trainer

Cloud is a trainer. Every regular enemy in FF7 can be caught, raised, and sent into battle, where it fights with its
own moves, model, and animations. Up to two monsters fight beside Cloud, each with its own row (HP, MP, TIME) and its
own turns. Barret, Tifa and the rest stay in the story but never fight.

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
| **Your monsters fight** | The first two conscious monsters in your party are sent out automatically ("Go, Mono Drive!"). Each has its own party row with HP, MP and a TIME bar, and on its turn you pick one of its moves (up to 3). Every move hits one enemy, even moves that hit a whole group in the original game. Enemies attack them too. Cloud keeps his own menu (Attack, Magic, Catch, Item) on his turns. |
| **Monsters page** | Main menu → **Monsters** (where PHS was). See every monster's type, weaknesses, HP, MP, EXP, stats, moves, next move and evolution. **OK** on a monster picks it up, **OK** again puts it in a new place (reorder the party, or trade with the PC box: the first two fight). **Triangle** opens its moves: choose which known moves it uses in battle. Works with keyboard and controller. |
| **F5** | Next lead monster (outside battle). Also on the Monsters page. |
| **F6** | Show or hide the roster panel. |
| **F7** | Swap the last party monster with the next one in the PC box (outside battle). |
| **HUD** | Bottom right in the field: your lead monster (F6 for the full party). In battle it only appears to say why no monster is out. |

- **Party:** up to 6 monsters; more go to the PC box.
- **Growing:** the monsters that fought get the battle's EXP and the rest of the party half. Monsters learn their
  species' moves as they level and some evolve (Mono Drive becomes Scrutin Eye at Lv 15, Guard Hound becomes Blood
  Taste, and so on).
- **Healing:** saving the game, or resting at an inn or a bed, heals every monster, fainted ones included.
- **Fainting:** a monster at 0 HP faints and the others fight on. If the battle has room, your third conscious
  monster steps into its row ("Go, ...!"); otherwise its row goes down. The next conscious monsters fight in the
  next battle.
- **Game Over** only when Cloud and every monster out are down. If Cloud falls, his monsters can still win the
  battle for him.
- **No room:** a few battles have no space for another monster (all six spots, or all three enemy kinds, are
  taken). That monster sits the battle out ("... can't join this battle!").
- **Saves:** your monsters are saved together with each game save (`save\trainer\trainer_slotNN.json` next to
  the game's own saves) and come back when you load that save.
- The HUD hotkeys only react while the game window is active.

## Notes

- Runs the classic FF7 engine through **FFNx** 1.24.3 (as FFNx documents for this release); the 64-bit FFVII.exe
  launcher and its DRM are not used or touched.
- A log is written to `ff7\workingdir\trainer\trainer.log` if something goes wrong.
- Fan project for personal use. Not affiliated with Square Enix, Nintendo or The Pokemon Company. No game files are
  included; the monster data is built from your own copy of the game.
