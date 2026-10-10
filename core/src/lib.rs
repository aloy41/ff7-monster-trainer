//! trainer_core: the rules half of FF7 Monster Trainer.
//!
//! Pure logic over `species.json` (built by `tools/build_species.py` from the player's own game files).
//! It never touches game memory; `hook/` translates between this and the running game through `ffi`.

pub mod ffi;
pub mod monster;
pub mod rng;
pub mod roster;
pub mod rules;
pub mod save;
pub mod species;

pub use monster::{Monster, Stats};
pub use roster::Roster;
pub use species::{Dex, Species};
