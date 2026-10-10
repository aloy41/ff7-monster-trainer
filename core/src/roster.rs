use crate::monster::{Growth, Monster};
use crate::rng::Rng;
use crate::rules::{self, ACTIVE_SLOTS, BENCH_XP_SHARE, PARTY_SIZE};
use crate::species::Dex;
use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Default, Serialize, Deserialize, PartialEq)]
pub struct Trainer {
    pub captures: u32,
    pub battles: u32,
    /// Set by the post-game rematch: bosses become capturable as legendaries.
    pub legendary_unlocked: bool,
    /// The Catch materia was granted in this game (never grant it twice). Older sidecars default to false.
    #[serde(default)]
    pub kit_given: bool,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, PartialEq)]
pub struct Roster {
    pub next_uid: u32,
    pub party: Vec<Monster>,
    #[serde(rename = "box")]
    pub storage: Vec<Monster>,
    pub trainer: Trainer,
}

#[derive(Debug, Clone, PartialEq)]
pub enum CaptureResult {
    Caught { uid: u32, to_box: bool },
    Failed { chance: f32 },
    NotCapturable,
}

impl Roster {
    /// The monster(s) that fight: the first ACTIVE_SLOTS party members that aren't KO'd, in party order.
    pub fn active(&self) -> [Option<usize>; ACTIVE_SLOTS] {
        let mut out = [None; ACTIVE_SLOTS];
        let mut it = self.party.iter().enumerate().filter(|(_, m)| !m.is_ko()).map(|(i, _)| i);
        for slot in out.iter_mut() {
            *slot = it.next();
        }
        out
    }

    pub fn find(&self, uid: u32) -> Option<&Monster> {
        self.party.iter().chain(self.storage.iter()).find(|m| m.uid == uid)
    }

    pub fn find_mut(&mut self, uid: u32) -> Option<&mut Monster> {
        self.party.iter_mut().chain(self.storage.iter_mut()).find(|m| m.uid == uid)
    }

    /// Roll a capture. On success the monster joins the party, or the box when the party is full.
    #[allow(clippy::too_many_arguments)]
    pub fn try_capture(
        &mut self, dex: &Dex, rng: &mut Rng, species_id: u16, level: u8, hp: u32, hp_max: u32, status_bonus: f32,
        tier: u32, scene: u16,
    ) -> CaptureResult {
        let Some(species) = dex.get(species_id) else { return CaptureResult::NotCapturable };
        let chance = rules::capture_chance(species, hp, hp_max, status_bonus, tier, self.trainer.legendary_unlocked);
        if chance <= 0.0 {
            return CaptureResult::NotCapturable;
        }
        if rng.next_f32() >= chance {
            return CaptureResult::Failed { chance };
        }
        self.next_uid += 1;
        let m = Monster::new(self.next_uid, species, level, scene);
        let uid = m.uid;
        let to_box = self.party.len() >= PARTY_SIZE;
        if to_box { self.storage.push(m) } else { self.party.push(m) }
        self.trainer.captures += 1;
        CaptureResult::Caught { uid, to_box }
    }

    /// Battle won: active fighters still standing get full XP, the rest of the party half.
    pub fn award_xp(&mut self, dex: &Dex, xp: u32) -> Vec<(u32, Growth)> {
        let active = self.active();
        let mut out = Vec::new();
        for (i, m) in self.party.iter_mut().enumerate() {
            if m.is_ko() {
                continue;
            }
            let share = if active.contains(&Some(i)) { xp } else { (xp as f32 * BENCH_XP_SHARE) as u32 };
            let g = m.gain_xp(dex, share);
            if g.levels_gained > 0 {
                out.push((m.uid, g));
            }
        }
        self.trainer.battles += 1;
        out
    }

    pub fn heal_all(&mut self, dex: &Dex) {
        for m in self.party.iter_mut().chain(self.storage.iter_mut()) {
            if let Some(s) = dex.get(m.species) {
                m.heal(s);
            }
        }
    }

    pub fn swap(&mut self, a: usize, b: usize) -> bool {
        if a < self.party.len() && b < self.party.len() {
            self.party.swap(a, b);
            true
        } else {
            false
        }
    }

    /// Party -> box. The last conscious party member can't be deposited.
    pub fn deposit(&mut self, i: usize) -> bool {
        if i >= self.party.len() {
            return false;
        }
        let conscious_left = self.party.iter().enumerate().filter(|(j, m)| *j != i && !m.is_ko()).count();
        if conscious_left == 0 && !self.party[i].is_ko() && self.party.len() > 1 {
            return false;
        }
        let m = self.party.remove(i);
        self.storage.push(m);
        true
    }

    pub fn withdraw(&mut self, box_index: usize) -> bool {
        if box_index >= self.storage.len() || self.party.len() >= PARTY_SIZE {
            return false;
        }
        let m = self.storage.remove(box_index);
        self.party.push(m);
        true
    }

    pub fn all_ko(&self) -> bool {
        self.party.iter().all(|m| m.is_ko())
    }
}
