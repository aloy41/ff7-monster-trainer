# FINAL FANTASY VII: Monster Trainer

A mod for the classic PC FINAL FANTASY VII that turns Cloud into a monster trainer. Every regular enemy can be
caught, raised, levelled and evolved, then sent into battle, where it fights with its own moves, model and
animations. Up to two monsters fight beside Cloud as real party members, each with its own HP, MP and TIME bar.
Barret, Tifa and the rest stay in the story but step aside in battle.

> **Status: work in progress.** Catching, two-monster battles with their own party rows, EXP and levels, learned
> moves, evolution, saves, the Monsters page and the installer all work. Next: a third monster stepping in when
> one faints, and wider testing across the whole game.

## Features

- **Catch** any regular enemy with Cloud's Catch command. Weakened or disabled monsters are easier to catch; bosses
  can't be caught.
- **314 species** built from your own copy of the game: types from their elemental affinities, stats from their
  battle data, and move lists from their attacks.
- **Growth:** EXP, levels, moves learned by level, and 31 evolution lines (Mono Drive → Scrutin Eye,
  Guard Hound → Blood Taste, ...).
- **Party of 6** plus a PC box, saved alongside every game save and restored when that save loads.
- **Monsters page** in the main menu (where PHS was), drawn with the game's own windows and cursor and driven by
  its own buttons, so it works on keyboard and controller. It shows each monster's type, weaknesses, HP, MP, EXP,
  stats, moves, next move and evolution. You can reorder the party, trade monsters with the PC box, and choose
  which known moves each monster uses in battle.
- **Real party members:** the first two conscious monsters take party rows 2 and 3, with their own HP, MP, TIME
  bar and turns, and stand in the party's spots in every layout (back and pincer attacks too). Cloud keeps his own
  menu, with Catch, on his turns.
- **No game files in this repository.** The installer copies your Steam install into a separate folder and reads
  the monster data from it; your Steam install is never modified.

Controls and rules: [release/README.md](release/README.md).

## Install (players)

Requirements: FINAL FANTASY VII on Steam (the 2026 release), Windows 10/11, Python 3.10+.

1. Build the mod (below), then run `release/make-dist.ps1`. It assembles `release/dist/FF7-Monster-Trainer/`.
2. Run `Setup.cmd` from that folder. It finds your Steam install and makes a separate **FF7 Monster Trainer** copy
   next to your Steam library (about 3.5 GB). It downloads FFNx from its GitHub release and checks its SHA-256,
   builds the monster data, and adds a Desktop shortcut.
3. Start Steam, then launch **FF7 Monster Trainer**.

## Build (developers)

- **Hook** (`hook/`, C++20, x86): `hook\build.cmd` builds `DINPUT.dll`. It needs Visual Studio 2022 with the C++
  workload, which it finds automatically.
- **Rules engine** (`core/`, Rust): `cargo build --release --target i686-pc-windows-msvc`. Tests: `cargo test`.
- **Data tools** (`tools/`, Python): read `scene.bin` and `battle.lgp` from your game and write `species.json`.

### How it works

`DINPUT.dll` is a DirectInput proxy that the classic `ff7_en.exe` (1.02) loads at startup. It patches a handful of
byte-checked call sites in the engine: scene load, battle party build, battle tick, action dispatch, battle text,
the end-of-battle menu, and save/load. A monster in battle is an enemy-side actor (its own model, moves and
animation scripts) driven through FF7's Manipulate link. Its party row belongs to a hidden, untargetable stand-in
whose link points at it, so the stand-in's turns are the monster's turns. All game rules (catching, growth, evolution, the roster) live in the Rust core behind a small
C ABI. The 64-bit `FFVII.exe` launcher and its DRM are never used or touched.

### Testing

`lab/` runs scripted in-game tests (`tests/*.txt`; the commands are listed in `hook/src/testmode.h`). Each runs on
a private, muted Windows desktop, and input is fed inside the game process, so tests never take focus or sound
from whatever else is on screen. They run against a separate copy of the game, never the Steam install: pass
`-WorkingDir <copy>\ff7\workingdir` or set `FF7_LAB_WORKINGDIR`.

## Credits

- [FFNx](https://github.com/julianxhokaxhiu/FFNx) (GPL-3.0) runs the classic engine. The installer downloads it;
  it is not included here.
- FINAL FANTASY VII © Square Enix. This is an unofficial fan project, not affiliated with Square Enix, Nintendo or
  The Pokémon Company, and contains no game files.
