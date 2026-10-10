# FINAL FANTASY VII: Monster Trainer

A mod for the classic PC FINAL FANTASY VII that turns Cloud into a monster trainer. Every regular enemy can be
caught, raised, levelled and evolved, then sent into battle, where it fights with its own moves, model and
animations. Barret, Tifa and the rest stay in the story but step aside in battle.

> **Status: work in progress.** Catching, sending out, EXP and levels, learned moves, evolution, saves and the
> installer all work. Being reworked right now: monsters as real party members (their own party row with HP, MP
> and time bar, up to two fighting beside Cloud), and a Monsters page in the main menu.

## Features

- **Catch** any regular enemy with Cloud's Catch command. Weakened or disabled monsters are easier to catch; bosses
  can't be caught.
- **314 species** built from your own copy of the game: types from their elemental affinities, stats from their
  battle data, and move lists from their attacks.
- **Growth:** EXP, levels, moves learned by level, and 31 evolution lines (Mono Drive → Scrutin Eye,
  Guard Hound → Blood Taste, ...).
- **Party of 6** plus a PC box, saved alongside every game save and restored when that save loads.
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
byte-checked call sites in the engine: scene load, battle tick, action dispatch, battle text, the end-of-battle
menu, and save/load. All game rules (catching, growth, evolution, the roster) live in the Rust core behind a small
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
