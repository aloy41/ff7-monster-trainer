//! The numbers. Reference curves are fitted to Cloud so monsters scale like party members
//! (HP 302 at Lv6 and ~9000 at Lv99; MP 54 at Lv6 and ~1000 at Lv99).

use crate::species::Species;

pub const MAX_LEVEL: u8 = 99;
pub const PARTY_SIZE: usize = 6;
/// Pokemon-style singles: Cloud commands one monster at a time (FF7's Manipulate link is one caster -> one actor).
pub const ACTIVE_SLOTS: usize = 1;
/// FF7's Manipulate menu (what Cloud sees when commanding a monster) lists 3 moves.
pub const EQUIPPED_MOVES: usize = 3;
/// Boss capture rate once the post-game rematch flag is set (story fights stay uncapturable).
pub const LEGENDARY_RATE: f32 = 0.08;
/// Benched party members get this share of battle XP.
pub const BENCH_XP_SHARE: f32 = 0.5;

pub fn hp_ref(level: u8) -> f32 {
    let l = level as f32;
    30.0 + 45.0 * l + 0.45 * l * l
}

pub fn mp_ref(level: u8) -> f32 {
    let l = level as f32;
    10.0 + 7.0 * l + 0.03 * l * l
}

pub fn stat_ref(level: u8) -> f32 {
    12.0 + 0.9 * level as f32
}

/// Total XP needed to *be* at `level` (Lv1 = 0). ~2.4M at Lv99, close to FF7's own curve.
pub fn xp_for_level(level: u8) -> u32 {
    if level <= 1 { 0 } else { (level as f64).powf(3.2) as u32 }
}

pub fn level_for_xp(xp: u32) -> u8 {
    let mut l = 1u8;
    while l < MAX_LEVEL && xp_for_level(l + 1) <= xp {
        l += 1;
    }
    l
}

/// Capture tiers: Capture / Greater Capture / Master Capture materia.
pub fn tier_multiplier(tier: u32) -> f32 {
    match tier {
        0 => 1.0,
        1 => 1.5,
        2 => 255.0, // master: always succeeds on anything capturable
        _ => 1.0,
    }
}

/// p = (1 - 2/3 * hp%) * species rate * tier * status bonus, clamped to [0, 1].
/// `status_bonus` is 1.0 normally, 1.5 for Sleep/Paralyze/Stop-type statuses, 2.0 for Stop + low HP combos.
pub fn capture_chance(species: &Species, hp: u32, hp_max: u32, status_bonus: f32, tier: u32, legendary_unlocked: bool) -> f32 {
    let rate = if species.boss {
        if legendary_unlocked { LEGENDARY_RATE } else { 0.0 }
    } else {
        species.capture_rate
    };
    if rate <= 0.0 {
        return 0.0;
    }
    let hp_frac = if hp_max == 0 { 1.0 } else { (hp.min(hp_max) as f32) / hp_max as f32 };
    ((1.0 - 2.0 / 3.0 * hp_frac) * rate * tier_multiplier(tier) * status_bonus.max(1.0)).clamp(0.0, 1.0)
}
