//! Sidecar saves: one JSON file per FF7 save slot, next to the game's own saves.
//! Written to a temp file and renamed, so a crash mid-write never leaves a half file.

use crate::roster::Roster;
use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

pub const SAVE_VERSION: u32 = 1;

#[derive(Debug, Serialize, Deserialize)]
struct SaveFile {
    version: u32,
    slot: u32,
    roster: Roster,
}

/// Key for the between-saves autosave (captures, battles). Real save keys are file << 4 | slot <= 0xEE.
pub const AUTOSAVE: u32 = 0xFFFF;

pub fn path_for(dir: &Path, slot: u32) -> PathBuf {
    if slot == AUTOSAVE {
        dir.join("trainer_autosave.json")
    } else {
        dir.join(format!("trainer_slot{slot:02}.json"))
    }
}

/// Missing file = empty roster (a new game, or a save made before the mod was installed).
pub fn load(dir: &Path, slot: u32) -> Result<Roster, String> {
    let p = path_for(dir, slot);
    let text = match std::fs::read_to_string(&p) {
        Ok(t) => t,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(Roster::default()),
        Err(e) => return Err(format!("{}: {e}", p.display())),
    };
    let f: SaveFile = serde_json::from_str(&text).map_err(|e| format!("{}: {e}", p.display()))?;
    if f.version != SAVE_VERSION {
        return Err(format!("{}: save version {} (expected {SAVE_VERSION})", p.display(), f.version));
    }
    Ok(f.roster)
}

pub fn store(dir: &Path, slot: u32, roster: &Roster) -> Result<(), String> {
    std::fs::create_dir_all(dir).map_err(|e| format!("{}: {e}", dir.display()))?;
    let p = path_for(dir, slot);
    let tmp = p.with_extension("json.tmp");
    let body = serde_json::to_string_pretty(&SaveFile { version: SAVE_VERSION, slot, roster: roster.clone() })
        .map_err(|e| e.to_string())?;
    std::fs::write(&tmp, body).map_err(|e| format!("{}: {e}", tmp.display()))?;
    std::fs::rename(&tmp, &p).map_err(|e| format!("{}: {e}", p.display()))
}
