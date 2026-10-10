use serde::Deserialize;
use std::collections::HashMap;
use std::path::Path;

/// Elements in FF7's bit order (attack element mask, enemy element tables).
pub const ELEMENTS: [&str; 16] = [
    "fire", "ice", "bolt", "earth", "poison", "gravity", "water", "wind", "holy", "restorative", "cut", "hit",
    "punch", "shoot", "shout", "hidden",
];

#[derive(Debug, Clone, Deserialize)]
pub struct Factors {
    pub hp: f32,
    pub mp: f32,
    pub str: f32,
    pub def_: f32,
    pub mag: f32,
    pub mdef: f32,
    pub spd: f32,
    pub lck: f32,
    pub eva: f32,
}

#[derive(Debug, Clone, Deserialize)]
pub struct Move {
    pub name: String,
    pub mp: u16,
    pub power: u8,
    pub elements: Vec<String>,
    /// The 28-byte FF7 attack record, hex: injected into a scene's attack table at battle load.
    pub raw: String,
}

pub fn hex_bytes(s: &str) -> Option<Vec<u8>> {
    (0..s.len()).step_by(2).map(|i| u8::from_str_radix(s.get(i..i + 2)?, 16).ok()).collect()
}

impl Move {
    pub fn record(&self) -> Option<[u8; 28]> {
        hex_bytes(&self.raw)?.try_into().ok()
    }
}

#[derive(Debug, Clone, Deserialize)]
pub struct Learn {
    pub level: u8,
    #[serde(rename = "move")]
    pub move_id: u16,
}

#[derive(Debug, Clone, Deserialize)]
pub struct Evolution {
    pub species: u16,
    pub level: u8,
}

#[derive(Debug, Clone, Deserialize)]
pub struct Species {
    pub id: u16,
    pub name: String,
    pub native_level: u8,
    pub boss: bool,
    pub types: Vec<String>,
    /// "weak" / "half" / "null" / "absorb" / "death" / "full-cure" -> element names.
    pub affinities: HashMap<String, Vec<String>>,
    pub status_immunity_mask: u32,
    pub factors: Factors,
    pub exp_yield: u32,
    pub ap_yield: u16,
    pub gil: u32,
    pub capture_rate: f32,
    pub model: Option<String>,
    /// The original 184-byte scene.bin enemy record, hex: injected into a battle scene when sent out.
    pub enemy_record: String,
    pub moves: HashMap<String, Move>,
    pub learnset: Vec<Learn>,
    #[serde(default)]
    pub evolves_to: Option<Evolution>,
}

impl Species {
    pub fn record(&self) -> Option<[u8; 184]> {
        hex_bytes(&self.enemy_record)?.try_into().ok()
    }

    pub fn move_by_id(&self, id: u16) -> Option<&Move> {
        self.moves.get(&id.to_string())
    }

    /// FF7 element rate byte per element, as the battle engine stores them for an actor:
    /// 0xFF none, 0x02 weak, 0x04 half, 0x05 null, 0x06 absorb, 0x00 death, 0x07 full cure.
    pub fn element_rates(&self) -> [u8; 16] {
        let mut out = [0xFFu8; 16];
        for (rate, code) in [("weak", 2u8), ("half", 4), ("null", 5), ("absorb", 6), ("death", 0), ("full-cure", 7)] {
            for e in self.affinities.get(rate).into_iter().flatten() {
                if let Some(i) = ELEMENTS.iter().position(|x| x == e) {
                    out[i] = code;
                }
            }
        }
        out
    }
}

#[derive(Debug, Deserialize)]
struct DexFile {
    version: u32,
    species: Vec<Species>,
}

#[derive(Debug, Default)]
pub struct Dex {
    by_id: HashMap<u16, Species>,
    /// Move id -> owning species, so a move's record can be found without knowing who knows it.
    move_owner: HashMap<u16, u16>,
}

impl Dex {
    pub fn load(path: &Path) -> Result<Dex, String> {
        let text = std::fs::read_to_string(path).map_err(|e| format!("{}: {e}", path.display()))?;
        Dex::from_json(&text)
    }

    pub fn from_json(text: &str) -> Result<Dex, String> {
        let file: DexFile = serde_json::from_str(text).map_err(|e| format!("species.json: {e}"))?;
        if file.version != 1 {
            return Err(format!("species.json version {} (expected 1)", file.version));
        }
        let mut dex = Dex::default();
        for s in file.species {
            for k in s.moves.keys() {
                if let Ok(id) = k.parse::<u16>() {
                    dex.move_owner.entry(id).or_insert(s.id);
                }
            }
            dex.by_id.insert(s.id, s);
        }
        Ok(dex)
    }

    pub fn get(&self, id: u16) -> Option<&Species> {
        self.by_id.get(&id)
    }

    pub fn find_move(&self, move_id: u16) -> Option<&Move> {
        self.get(*self.move_owner.get(&move_id)?)?.move_by_id(move_id)
    }

    pub fn len(&self) -> usize {
        self.by_id.len()
    }

    pub fn is_empty(&self) -> bool {
        self.by_id.is_empty()
    }

    pub fn iter(&self) -> impl Iterator<Item = &Species> {
        self.by_id.values()
    }
}
