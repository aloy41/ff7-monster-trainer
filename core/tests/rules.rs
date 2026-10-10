//! Headless tests against the real species.json built from the lab install (tests skip if it's missing,
//! so a clean checkout without game files still builds; CI-style runs set TRAINER_SPECIES to require it).

use trainer_core::rng::Rng;
use trainer_core::roster::CaptureResult;
use trainer_core::rules::{self, PARTY_SIZE};
use trainer_core::{save, Dex, Monster, Roster};

fn dex() -> Option<Dex> {
    let path = std::env::var("TRAINER_SPECIES")
        .unwrap_or_else(|_| concat!(env!("CARGO_MANIFEST_DIR"), "/../out/species.json").to_string());
    match Dex::load(path.as_ref()) {
        Ok(d) => Some(d),
        Err(e) if std::env::var("TRAINER_SPECIES").is_ok() => panic!("{e}"),
        Err(_) => None,
    }
}

fn id(dex: &Dex, name: &str) -> u16 {
    dex.iter().find(|s| s.name == name).unwrap_or_else(|| panic!("{name} missing")).id
}

#[test]
fn reference_curves_match_cloud() {
    assert!((rules::hp_ref(6) - 302.0).abs() < 20.0);
    assert!((rules::hp_ref(99) - 9000.0).abs() < 300.0);
    assert!((rules::mp_ref(6) - 54.0).abs() < 3.0);
    assert_eq!(rules::level_for_xp(rules::xp_for_level(42)), 42);
    assert_eq!(rules::level_for_xp(rules::xp_for_level(42) - 1), 41);
    assert_eq!(rules::level_for_xp(u32::MAX), 99);
}

#[test]
fn every_species_is_usable() {
    let Some(dex) = dex() else { return };
    assert!(dex.len() >= 300, "only {} species", dex.len());
    for s in dex.iter() {
        let m = Monster::new(1, s, s.native_level, 0);
        assert!(!m.equipped.is_empty(), "{} has no starting moves", s.name);
        assert!(m.equipped.len() <= 3, "the Manipulate menu holds 3 moves");
        let rec = s.record().unwrap_or_else(|| panic!("{} has no 184-byte enemy record", s.name));
        assert!(u16::from_le_bytes([rec[0xA4], rec[0xA5]]) as u32 | (u16::from_le_bytes([rec[0xA6], rec[0xA7]]) as u32) << 16 > 0, "{}: record HP is 0", s.name);
        for mv in &m.known {
            let rec = s.move_by_id(*mv).and_then(|x| x.record());
            assert!(rec.is_some(), "{}: move {mv} has no 28-byte record", s.name);
        }
        let st = m.stats(s);
        assert!(st.hp_max >= 1 && st.hp_max <= 9999, "{} hp {}", s.name, st.hp_max);
        assert_eq!(m.hp, st.hp_max);
        if let Some(e) = &s.evolves_to {
            assert!(dex.get(e.species).is_some(), "{} evolves into missing {}", s.name, e.species);
            assert!(e.level >= s.native_level, "{} evolves backwards", s.name);
        }
    }
}

#[test]
fn no_species_offers_a_kernel_spell() {
    let Some(dex) = dex() else { return };
    for s in dex.iter() {
        for l in &s.learnset {
            assert!(l.move_id >= 256, "{} learns kernel spell {}", s.name, l.move_id);
        }
    }
}

#[test]
fn old_rosters_drop_moves_the_species_no_longer_has() {
    let Some(dex) = dex() else { return };
    let s = dex.get(id(&dex, "Scrutin Eye")).unwrap();
    let mut m = Monster::new(1, s, 15, 0);
    let good = m.known.clone();
    m.known = vec![27, 31, 28];  // Fire, Ice2, Fire2 from an older build
    m.equipped = vec![27, 31, 28];
    m.sanitize_moves(s);
    assert!(m.known.iter().all(|&id| id >= 256), "{:?}", m.known);
    assert!(!m.equipped.is_empty() && m.equipped.len() <= 3);
    assert!(m.equipped.iter().all(|id| good.contains(id)), "{:?} vs {:?}", m.equipped, good);
}

#[test]
fn species_whose_moves_hang_the_battle_are_not_in_the_dex() {
    let Some(dex) = dex() else { return };
    // Found by the lab move sweep (every move of every species forced once): Eye, Yang, Grosspanzer-Mobile.
    for id in [15u16, 152, 293] {
        assert!(dex.get(id).is_none(), "species {id} soft-locks the battle when sent out");
    }
    assert!(dex.get(292).is_some(), "Grosspanzer-Small is fine");
}

#[test]
fn loaded_vitals_never_exceed_the_maximum() {
    let Some(dex) = dex() else { return };
    let s = dex.get(id(&dex, "Mono Drive")).unwrap();
    let mut m = Monster::new(1, s, 15, 0);
    let st = m.stats(s);
    m.hp = 15246;
    m.mp = 9999;
    m.clamp_vitals(s);
    assert_eq!((m.hp, m.mp), (st.hp_max, st.mp_max));
    m.hp = 3;
    m.clamp_vitals(s);
    assert_eq!(m.hp, 3, "a hurt monster stays hurt");
}

#[test]
fn out_of_range_numbers_in_a_save_are_clamped_not_rejected() {
    let json = r#"{"uid": 1, "species": 3, "nickname": "X", "level": 300, "xp": -5, "hp": 99999, "mp": 1.5e9,
                   "known": [], "equipped": [], "caught_level": 5, "caught_scene": 0}"#;
    let m: Monster = serde_json::from_str(json).unwrap();
    assert_eq!((m.level, m.xp, m.hp, m.mp), (255, 0, 65535, 65535));
    let Some(dex) = dex() else { return };
    let s = dex.get(id(&dex, "Mono Drive")).unwrap();
    let mut m = m;
    m.clamp_vitals(s);
    assert_eq!(m.level, 99);
    assert_eq!(m.hp, m.stats(s).hp_max);
}

#[test]
fn mono_drive_is_what_the_dex_says() {
    let Some(dex) = dex() else { return };
    let s = dex.get(id(&dex, "Mono Drive")).unwrap();
    let names: Vec<_> = s.learnset.iter().map(|l| s.move_by_id(l.move_id).unwrap().name.as_str()).collect();
    assert!(names.contains(&"Drilldrive"), "{names:?}");
    assert!(!names.contains(&"Fire"), "kernel spells soft-lock the battle when a monster casts them: {names:?}");
    assert_eq!(s.types, vec!["earth"]);
    let rates = s.element_rates();
    assert_eq!(rates[3], 5, "earth null");
    assert_eq!(rates[7], 2, "weak to wind");
    assert!(!s.boss);
}

#[test]
fn capture_odds_follow_hp_and_bosses_are_locked() {
    let Some(dex) = dex() else { return };
    let mono = dex.get(id(&dex, "Mono Drive")).unwrap();
    let full = rules::capture_chance(mono, 100, 100, 1.0, 0, false);
    let low = rules::capture_chance(mono, 1, 100, 1.0, 0, false);
    assert!(low > full * 2.5, "low HP should be ~3x full: {full} vs {low}");
    assert!(rules::capture_chance(mono, 1, 100, 1.5, 0, false) > low);
    assert_eq!(rules::capture_chance(mono, 100, 100, 1.0, 2, false), 1.0, "master tier always works");

    let boss = dex.get(id(&dex, "Air Buster")).unwrap();
    assert!(boss.boss);
    assert_eq!(rules::capture_chance(boss, 1, 100, 2.0, 2, false), 0.0, "story bosses can't be caught");
    assert!(rules::capture_chance(boss, 1, 100, 1.0, 0, true) > 0.0, "legendary rematch unlocks them");
}

#[test]
fn capture_fills_party_then_box() {
    let Some(dex) = dex() else { return };
    let mono = id(&dex, "Mono Drive");
    let mut r = Roster::default();
    let mut rng = Rng::new(7);
    let mut caught = 0;
    for _ in 0..200 {
        if let CaptureResult::Caught { to_box, .. } = r.try_capture(&dex, &mut rng, mono, 2, 1, 28, 1.0, 0, 147) {
            caught += 1;
            assert_eq!(to_box, caught > PARTY_SIZE);
        }
        if caught == 8 {
            break;
        }
    }
    assert_eq!(caught, 8);
    assert_eq!(r.party.len(), PARTY_SIZE);
    assert_eq!(r.storage.len(), 2);
    assert_eq!(r.trainer.captures, 8);
    let uids: std::collections::HashSet<_> = r.party.iter().chain(&r.storage).map(|m| m.uid).collect();
    assert_eq!(uids.len(), 8, "uids are unique");
    assert_eq!(r.try_capture(&dex, &mut rng, id(&dex, "Air Buster"), 15, 1, 1200, 1.0, 2, 177), CaptureResult::NotCapturable);
}

#[test]
fn xp_levels_learns_and_evolves() {
    let Some(dex) = dex() else { return };
    // Pick a line with an evolution and a later move to learn.
    let s = dex.iter().find(|s| s.name == "Guard Hound").unwrap();
    let evo = s.evolves_to.as_ref().expect("Guard Hound evolves");
    let mut m = Monster::new(1, s, s.native_level, 0);
    let g = m.gain_xp(&dex, rules::xp_for_level(evo.level));
    assert!(g.levels_gained >= evo.level - s.native_level);
    assert_eq!(g.evolved_into, Some(evo.species));
    assert_eq!(m.species, evo.species);
    assert_eq!(m.nickname, dex.get(evo.species).unwrap().name, "default nickname follows the evolution");
    assert!(m.equipped.len() <= 3);
    let st = m.stats(dex.get(m.species).unwrap());
    assert!(m.hp <= st.hp_max && m.hp > 0);
}

#[test]
fn active_skips_ko_and_xp_share() {
    let Some(dex) = dex() else { return };
    let s = dex.get(id(&dex, "Mono Drive")).unwrap();
    let mut r = Roster::default();
    for i in 0..3 {
        r.party.push(Monster::new(i + 1, s, 2, 0));
    }
    r.party[0].hp = 0;
    assert_eq!(r.active(), [Some(1), Some(2)], "the first two conscious monsters fight");
    r.party.push(Monster::new(9, s, 2, 0));
    let xp = rules::xp_for_level(10);
    r.award_xp(&dex, xp);
    assert_eq!(r.party[0].level, 2, "KO'd monsters gain nothing");
    assert!(r.party[1].level > r.party[3].level, "bench gets half: {} vs {}", r.party[1].level, r.party[3].level);
    assert!(!r.deposit(99));
    r.party[1].hp = 0;
    r.party[2].hp = 0;
    assert!(!r.deposit(3), "can't box the last conscious monster");
    r.heal_all(&dex);
    assert!(r.party.iter().all(|m| !m.is_ko()));
}

#[test]
fn sidecar_save_round_trips_atomically() {
    let Some(dex) = dex() else { return };
    let dir = std::env::temp_dir().join(format!("trainer_test_{}", std::process::id()));
    let mut r = Roster::default();
    let mut rng = Rng::new(3);
    while r.party.is_empty() {
        r.try_capture(&dex, &mut rng, id(&dex, "Mono Drive"), 2, 1, 28, 1.0, 0, 147);
    }
    r.party[0].nickname = "Drilly".into();
    save::store(&dir, 3, &r).unwrap();
    assert!(!save::path_for(&dir, 3).with_extension("json.tmp").exists());
    assert_eq!(save::load(&dir, 3).unwrap(), r);
    assert_eq!(save::load(&dir, 4).unwrap(), Roster::default(), "missing slot = empty roster");
    std::fs::write(save::path_for(&dir, 5), "{not json").unwrap();
    assert!(save::load(&dir, 5).is_err(), "corrupt sidecar is an error, never silently empty");
    // The autosave never collides with a real save key (file << 4 | slot, at most 0xEE).
    save::store(&dir, save::AUTOSAVE, &r).unwrap();
    assert!(dir.join("trainer_autosave.json").exists());
    assert_ne!(save::path_for(&dir, save::AUTOSAVE), save::path_for(&dir, 0x63));
    // Old sidecars without kit_given still load (serde default).
    let old = r#"{"version":1,"slot":7,"roster":{"next_uid":0,"party":[],"box":[],"trainer":{"captures":0,"battles":0,"legendary_unlocked":false}}}"#;
    std::fs::write(save::path_for(&dir, 7), old).unwrap();
    assert!(!save::load(&dir, 7).unwrap().trainer.kit_given);
    let _ = std::fs::remove_dir_all(&dir);
}
