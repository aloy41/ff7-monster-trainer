use crate::rules::{self, EQUIPPED_MOVES, MAX_LEVEL};
use crate::species::{Dex, Species};
use serde::{Deserialize, Deserializer, Serialize};

/// A number from a save file, clamped into the field's range (a hand-edited 99999 HP must not reject the whole
/// roster; clamp_vitals then brings it down to the species' maximum). Negative and fractional values count too.
fn saturate<'de, D, T>(d: D) -> Result<T, D::Error>
where
    D: Deserializer<'de>,
    T: TryFrom<u64> + Bounded,
{
    let v = f64::deserialize(d)?;
    if v.is_nan() || v <= 0.0 {
        return Ok(T::MIN_VALUE);
    }
    Ok(T::try_from(v as u64).unwrap_or(T::MAX_VALUE))
}

trait Bounded {
    const MIN_VALUE: Self;
    const MAX_VALUE: Self;
}
macro_rules! bounded {
    ($($t:ty),*) => { $(impl Bounded for $t { const MIN_VALUE: Self = <$t>::MIN; const MAX_VALUE: Self = <$t>::MAX; })* };
}
bounded!(u8, u16, u32);

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Stats {
    pub hp_max: u16,
    pub mp_max: u16,
    pub str: u8,
    pub def: u8,
    pub mag: u8,
    pub mdef: u8,
    pub spd: u8,
    pub lck: u8,
    pub eva: u8,
}

impl Stats {
    pub fn of(species: &Species, level: u8) -> Stats {
        let f = &species.factors;
        let s = |factor: f32| (rules::stat_ref(level) * factor).round().clamp(1.0, 255.0) as u8;
        Stats {
            hp_max: (rules::hp_ref(level) * f.hp).round().clamp(1.0, 9999.0) as u16,
            mp_max: (rules::mp_ref(level) * f.mp).round().clamp(0.0, 999.0) as u16,
            str: s(f.str),
            def: s(f.def_),
            mag: s(f.mag),
            mdef: s(f.mdef),
            spd: s(f.spd),
            lck: s(f.lck),
            eva: s(f.eva),
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct Monster {
    pub uid: u32,
    pub species: u16,
    pub nickname: String,
    #[serde(deserialize_with = "saturate")]
    pub level: u8,
    #[serde(deserialize_with = "saturate")]
    pub xp: u32,
    #[serde(deserialize_with = "saturate")]
    pub hp: u16,
    #[serde(deserialize_with = "saturate")]
    pub mp: u16,
    /// Every move ever learned (move ids are FF7 attack ids).
    pub known: Vec<u16>,
    /// Up to 4 moves usable in battle, in menu order.
    pub equipped: Vec<u16>,
    pub caught_level: u8,
    pub caught_scene: u16,
}

/// What happened when XP or a level changed: shown in the battle-end summary.
#[derive(Debug, Default, Clone, PartialEq)]
pub struct Growth {
    pub levels_gained: u8,
    pub learned: Vec<u16>,
    pub evolved_into: Option<u16>,
}

impl Monster {
    /// A freshly caught monster at `level` (its native level for wild captures), full HP/MP.
    pub fn new(uid: u32, species: &Species, level: u8, scene: u16) -> Monster {
        let level = level.clamp(1, MAX_LEVEL);
        let mut m = Monster {
            uid,
            species: species.id,
            nickname: species.name.clone(),
            level,
            xp: rules::xp_for_level(level),
            hp: 0,
            mp: 0,
            known: Vec::new(),
            equipped: Vec::new(),
            caught_level: level,
            caught_scene: scene,
        };
        for l in &species.learnset {
            if l.level <= level {
                m.learn(l.move_id);
            }
        }
        let st = m.stats(species);
        m.hp = st.hp_max;
        m.mp = st.mp_max;
        m
    }

    pub fn stats(&self, species: &Species) -> Stats {
        Stats::of(species, self.level)
    }

    pub fn is_ko(&self) -> bool {
        self.hp == 0
    }

    /// Learn a move; a 5th equipped move pushes out the oldest equipped one (it stays in `known`).
    pub fn learn(&mut self, move_id: u16) -> bool {
        if self.known.contains(&move_id) {
            return false;
        }
        self.known.push(move_id);
        if self.equipped.len() >= EQUIPPED_MOVES {
            self.equipped.remove(0);
        }
        self.equipped.push(move_id);
        true
    }

    /// Drop moves the species data no longer offers (a roster saved by an older build), then make sure something
    /// is equipped: the moves it still knows, or its learnset up to its level.
    pub fn sanitize_moves(&mut self, species: &Species) {
        self.known.retain(|&id| species.move_by_id(id).is_some());
        let known = self.known.clone();
        self.equipped.retain(|id| known.contains(id));
        if self.known.is_empty() {
            for l in &species.learnset {
                if l.level <= self.level {
                    self.learn(l.move_id);
                }
            }
        }
        for &id in &self.known {
            if self.equipped.len() >= EQUIPPED_MOVES {
                break;
            }
            if !self.equipped.contains(&id) {
                self.equipped.push(id);
            }
        }
    }

    /// A hand-edited (or older) file may hold more HP/MP than the monster has.
    pub fn clamp_vitals(&mut self, species: &Species) {
        self.level = self.level.clamp(1, MAX_LEVEL);
        let st = self.stats(species);
        self.hp = self.hp.min(st.hp_max);
        self.mp = self.mp.min(st.mp_max);
    }

    pub fn heal(&mut self, species: &Species) {
        let st = self.stats(species);
        self.hp = st.hp_max;
        self.mp = st.mp_max;
    }

    /// Add XP; level up, learn moves at each new level, evolve when the line says so.
    /// HP/MP keep their current fraction-free behaviour: gains in max are added to current, like FF7 level-ups.
    pub fn gain_xp(&mut self, dex: &Dex, xp: u32) -> Growth {
        let mut g = Growth::default();
        if self.level >= MAX_LEVEL {
            return g;
        }
        let Some(mut species) = dex.get(self.species) else { return g };
        self.xp = self.xp.saturating_add(xp);
        let target = rules::level_for_xp(self.xp);
        while self.level < target {
            let before = self.stats(species);
            self.level += 1;
            g.levels_gained += 1;
            let level = self.level;
            for l in species.learnset.iter().filter(|l| l.level == level) {
                if self.learn(l.move_id) {
                    g.learned.push(l.move_id);
                }
            }
            if let Some(evo) = &species.evolves_to
                && self.level >= evo.level
                    && let Some(next) = dex.get(evo.species) {
                        if self.nickname == species.name {
                            self.nickname = next.name.clone();
                        }
                        self.species = next.id;
                        species = next;
                        g.evolved_into = Some(next.id);
                        for l in species.learnset.iter().filter(|l| l.level <= level) {
                            if self.learn(l.move_id) {
                                g.learned.push(l.move_id);
                            }
                        }
                    }
            let after = self.stats(species);
            if !self.is_ko() {
                self.hp = (self.hp + after.hp_max.saturating_sub(before.hp_max)).min(after.hp_max);
            }
            self.mp = (self.mp + after.mp_max.saturating_sub(before.mp_max)).min(after.mp_max);
        }
        g
    }
}
