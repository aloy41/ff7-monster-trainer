//! C ABI for the hook DLL (x86, cdecl). Fixed-size structs, sizes asserted at compile time here and in
//! `hook/trainer_core.h`. All functions return 0 / a count on success and a negative code on error;
//! `tr_last_error` explains the last failure. State is a single global guarded by a mutex (the game is
//! single-threaded, the mutex only guards against our own reload thread).

use crate::monster::Monster;
use crate::rng::Rng;
use crate::roster::{CaptureResult, Roster};
use crate::species::Dex;
use crate::{rules, save};
use std::ffi::{CStr, c_char};
use std::path::PathBuf;
use std::sync::Mutex;

pub const TR_VERSION: u32 = 1;
pub const E_NOT_INIT: i32 = -1;
pub const E_BAD_ARG: i32 = -2;
pub const E_IO: i32 = -3;
pub const E_NOT_FOUND: i32 = -4;

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct TrMonsterView {
    pub uid: u32,
    pub species: u16,
    pub level: u8,
    pub ko: u8,
    pub hp: u16,
    pub hp_max: u16,
    pub mp: u16,
    pub mp_max: u16,
    /// str, def, mag, mdef, spd, lck, eva
    pub stats: [u8; 7],
    pub _pad: u8,
    /// FF7 element rate code per element (0xFF none, 2 weak, 4 half, 5 null, 6 absorb, 0 death, 7 full cure).
    pub element_rates: [u8; 16],
    pub status_immunity_mask: u32,
    /// Equipped move ids (FF7 attack ids), 0xFFFF = empty.
    pub moves: [u16; 4],
    /// Battle model id, two ASCII letters ("as" = Mono Drive), 0 = none.
    pub model: [u8; 2],
    pub _pad2: [u8; 2],
    /// UTF-8 nickname, NUL-terminated.
    pub name: [u8; 32],
}
const _: () = assert!(std::mem::size_of::<TrMonsterView>() == 88);

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct TrMoveRecord {
    /// The 28-byte FF7 attack record, ready to copy into a scene attack table.
    pub record: [u8; 28],
    /// The attack id the record belongs to.
    pub id: u16,
    pub _pad: u16,
    /// Display name in FF7 text encoding (0xFF-terminated, 32 bytes like scene.bin).
    pub ff_name: [u8; 32],
}
const _: () = assert!(std::mem::size_of::<TrMoveRecord>() == 64);

/// Per-battle-end growth for one monster, so the hook can print "Mono Drive grew to Lv 5".
#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct TrGrowth {
    pub uid: u32,
    pub levels_gained: u8,
    pub learned_count: u8,
    pub evolved: u8,
    pub _pad: u8,
    pub evolved_into: u16,
    pub learned: [u16; 3],
}
const _: () = assert!(std::mem::size_of::<TrGrowth>() == 16);

struct State {
    dex: Dex,
    save_dir: PathBuf,
    slot: u32,
    roster: Roster,
    rng: Rng,
}

static STATE: Mutex<Option<State>> = Mutex::new(None);
static LAST_ERROR: Mutex<String> = Mutex::new(String::new());

fn set_error(msg: impl Into<String>) {
    if let Ok(mut e) = LAST_ERROR.lock() {
        *e = msg.into();
    }
}

fn with_state<T>(f: impl FnOnce(&mut State) -> Result<T, i32>) -> Result<T, i32> {
    let mut guard = STATE.lock().map_err(|_| E_NOT_INIT)?;
    match guard.as_mut() {
        Some(s) => f(s),
        None => {
            set_error("tr_init has not been called");
            Err(E_NOT_INIT)
        }
    }
}

fn code(r: Result<i32, i32>) -> i32 {
    r.unwrap_or_else(|e| e)
}

unsafe fn cstr(p: *const c_char) -> Option<String> {
    if p.is_null() {
        return None;
    }
    unsafe { CStr::from_ptr(p) }.to_str().ok().map(str::to_owned)
}

/// ASCII -> FF7 text encoding (char - 0x20), 0xFF-terminated; unknown chars become spaces.
pub fn to_ff_text(s: &str) -> [u8; 32] {
    let mut out = [0xFFu8; 32];
    for (i, c) in s.chars().take(31).enumerate() {
        let b = c as u32;
        out[i] = if (0x20..0x80).contains(&b) { (b - 0x20) as u8 } else { 0 };
    }
    out
}

fn view(dex: &Dex, m: &Monster) -> TrMonsterView {
    let mut v = TrMonsterView {
        uid: m.uid,
        species: m.species,
        level: m.level,
        ko: m.is_ko() as u8,
        hp: m.hp,
        hp_max: 0,
        mp: m.mp,
        mp_max: 0,
        stats: [0; 7],
        _pad: 0,
        element_rates: [0xFF; 16],
        status_immunity_mask: 0,
        moves: [0xFFFF; 4],
        model: [0; 2],
        _pad2: [0; 2],
        name: [0; 32],
    };
    if let Some(s) = dex.get(m.species) {
        let st = m.stats(s);
        v.hp_max = st.hp_max;
        v.mp_max = st.mp_max;
        v.stats = [st.str, st.def, st.mag, st.mdef, st.spd, st.lck, st.eva];
        v.element_rates = s.element_rates();
        v.status_immunity_mask = s.status_immunity_mask;
        if let Some(model) = s.model.as_deref().map(str::as_bytes).filter(|b| b.len() == 2) {
            v.model = [model[0], model[1]];
        }
    }
    for (slot, id) in v.moves.iter_mut().zip(&m.equipped) {
        *slot = *id;
    }
    let name = m.nickname.as_bytes();
    let n = name.len().min(31);
    v.name[..n].copy_from_slice(&name[..n]);
    v
}

#[unsafe(no_mangle)]
pub extern "C" fn tr_version() -> u32 {
    TR_VERSION
}

/// Load species.json and remember where sidecar saves live. Safe to call again (Ctrl+T reload):
/// the current roster is kept.
///
/// # Safety
/// Both arguments must be NUL-terminated UTF-8 paths.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tr_init(species_json: *const c_char, save_dir: *const c_char, seed: u32) -> i32 {
    let (Some(sp), Some(dir)) = (unsafe { cstr(species_json) }, unsafe { cstr(save_dir) }) else {
        set_error("tr_init: null or non-UTF-8 path");
        return E_BAD_ARG;
    };
    let dex = match Dex::load(sp.as_ref()) {
        Ok(d) => d,
        Err(e) => {
            set_error(e);
            return E_IO;
        }
    };
    let Ok(mut guard) = STATE.lock() else { return E_NOT_INIT };
    let (roster, slot) = guard.take().map(|s| (s.roster, s.slot)).unwrap_or_default();
    let n = dex.len() as i32;
    *guard = Some(State { dex, save_dir: PathBuf::from(dir), slot, roster, rng: Rng::new(seed) });
    n
}

/// Copies the last error message (NUL-terminated, truncated to `len`). Returns its full length.
///
/// # Safety
/// `buf` must point to `len` writable bytes (or be null with len 0).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tr_last_error(buf: *mut u8, len: u32) -> u32 {
    let msg = LAST_ERROR.lock().map(|e| e.clone()).unwrap_or_default();
    if !buf.is_null() && len > 0 {
        let n = msg.len().min(len as usize - 1);
        unsafe {
            std::ptr::copy_nonoverlapping(msg.as_ptr(), buf, n);
            *buf.add(n) = 0;
        }
    }
    msg.len() as u32
}

/// Fresh roster (New Game).
#[unsafe(no_mangle)]
pub extern "C" fn tr_new_game() -> i32 {
    code(with_state(|s| {
        s.roster = Roster::default();
        Ok(0)
    }))
}

/// Load the sidecar for FF7 save slot `slot` (missing = empty roster). Returns party size.
#[unsafe(no_mangle)]
pub extern "C" fn tr_load_slot(slot: u32) -> i32 {
    code(with_state(|s| match save::load(&s.save_dir, slot) {
        Ok(mut r) => {
            // A species the dex no longer has (one that can't fight for Cloud) can't be sent out or shown.
            r.party.retain(|m| s.dex.get(m.species).is_some());
            r.storage.retain(|m| s.dex.get(m.species).is_some());
            for m in r.party.iter_mut().chain(r.storage.iter_mut()) {
                if let Some(sp) = s.dex.get(m.species) {
                    m.sanitize_moves(sp);
                    m.clamp_vitals(sp);
                }
            }
            s.roster = r;
            s.slot = slot;
            Ok(s.roster.party.len() as i32)
        }
        Err(e) => {
            set_error(e);
            Err(E_IO)
        }
    }))
}

#[unsafe(no_mangle)]
pub extern "C" fn tr_save_slot(slot: u32) -> i32 {
    code(with_state(|s| {
        s.slot = slot;
        save::store(&s.save_dir, slot, &s.roster).map(|_| 0).map_err(|e| {
            set_error(e);
            E_IO
        })
    }))
}

/// Capture odds in [0,1] for the HUD; status_bonus_x100 = 100 normally, 150 asleep/paralyzed.
#[unsafe(no_mangle)]
pub extern "C" fn tr_capture_chance(species: u16, hp: u32, hp_max: u32, status_bonus_x100: u32, tier: u32) -> f32 {
    with_state(|s| {
        let sp = s.dex.get(species).ok_or(E_NOT_FOUND)?;
        Ok(rules::capture_chance(sp, hp, hp_max, status_bonus_x100 as f32 / 100.0, tier, s.roster.trainer.legendary_unlocked))
    })
    .unwrap_or(0.0)
}

/// Roll a capture. Returns 1 caught (to party), 2 caught (party full, sent to box), 0 failed,
/// 3 not capturable (boss / unknown species), negative on error.
#[unsafe(no_mangle)]
pub extern "C" fn tr_try_capture(
    species: u16, level: u8, hp: u32, hp_max: u32, status_bonus_x100: u32, tier: u32, scene: u16,
) -> i32 {
    code(with_state(|s| {
        let r = s.roster.try_capture(&s.dex, &mut s.rng, species, level, hp, hp_max, status_bonus_x100 as f32 / 100.0, tier, scene);
        Ok(match r {
            CaptureResult::Caught { to_box: false, .. } => 1,
            CaptureResult::Caught { to_box: true, .. } => 2,
            CaptureResult::Failed { .. } => 0,
            CaptureResult::NotCapturable => 3,
        })
    }))
}

#[unsafe(no_mangle)]
pub extern "C" fn tr_party_len() -> i32 {
    code(with_state(|s| Ok(s.roster.party.len() as i32)))
}

#[unsafe(no_mangle)]
pub extern "C" fn tr_box_len() -> i32 {
    code(with_state(|s| Ok(s.roster.storage.len() as i32)))
}

/// # Safety
/// `out` must point to a writable TrMonsterView.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tr_party_get(index: u32, out: *mut TrMonsterView) -> i32 {
    if out.is_null() {
        return E_BAD_ARG;
    }
    code(with_state(|s| {
        let m = s.roster.party.get(index as usize).ok_or(E_NOT_FOUND)?;
        unsafe { *out = view(&s.dex, m) };
        Ok(0)
    }))
}

/// # Safety
/// `out` must point to a writable TrMonsterView.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tr_box_get(index: u32, out: *mut TrMonsterView) -> i32 {
    if out.is_null() {
        return E_BAD_ARG;
    }
    code(with_state(|s| {
        let m = s.roster.storage.get(index as usize).ok_or(E_NOT_FOUND)?;
        unsafe { *out = view(&s.dex, m) };
        Ok(0)
    }))
}

/// Party indices of the fighters, written to out[0..2] (-1 = empty; singles uses only out[0]).
///
/// # Safety
/// `out` must point to two writable i32s.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tr_active(out: *mut i32) -> i32 {
    if out.is_null() {
        return E_BAD_ARG;
    }
    code(with_state(|s| {
        let a = s.roster.active();
        for i in 0..2 {
            let v = a.get(i).copied().flatten().map(|x| x as i32).unwrap_or(-1);
            unsafe { *out.add(i) = v };
        }
        Ok(a.iter().filter(|x| x.is_some()).count() as i32)
    }))
}

/// After a battle (or a heal): write a monster's HP/MP back, clamped to its max.
#[unsafe(no_mangle)]
pub extern "C" fn tr_set_vitals(uid: u32, hp: u16, mp: u16) -> i32 {
    code(with_state(|s| {
        let State { dex, roster, .. } = s;
        let m = roster.find_mut(uid).ok_or(E_NOT_FOUND)?;
        let st = m.stats(dex.get(m.species).ok_or(E_NOT_FOUND)?);
        m.hp = hp.min(st.hp_max);
        m.mp = mp.min(st.mp_max);
        Ok(0)
    }))
}

/// Battle won. Fills up to `cap` growth records; returns how many monsters levelled.
///
/// # Safety
/// `out` must point to `cap` writable TrGrowth (or be null with cap 0).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tr_award_xp(xp: u32, out: *mut TrGrowth, cap: u32) -> i32 {
    code(with_state(|s| {
        let grown = s.roster.award_xp(&s.dex, xp);
        for (i, (uid, g)) in grown.iter().take(if out.is_null() { 0 } else { cap as usize }).enumerate() {
            let mut r = TrGrowth { uid: *uid, levels_gained: g.levels_gained, ..Default::default() };
            r.learned_count = g.learned.len().min(255) as u8;  // all of them; `learned` holds the first 3
            for (dst, src) in r.learned.iter_mut().zip(&g.learned) {
                *dst = *src;
            }
            if let Some(e) = g.evolved_into {
                r.evolved = 1;
                r.evolved_into = e;
            }
            unsafe { *out.add(i) = r };
        }
        Ok(grown.len() as i32)
    }))
}

#[unsafe(no_mangle)]
pub extern "C" fn tr_heal_all() -> i32 {
    code(with_state(|s| {
        s.roster.heal_all(&s.dex);
        Ok(0)
    }))
}

#[unsafe(no_mangle)]
pub extern "C" fn tr_swap(a: u32, b: u32) -> i32 {
    code(with_state(|s| if s.roster.swap(a as usize, b as usize) { Ok(0) } else { Err(E_BAD_ARG) }))
}

#[unsafe(no_mangle)]
pub extern "C" fn tr_deposit(index: u32) -> i32 {
    code(with_state(|s| if s.roster.deposit(index as usize) { Ok(0) } else { Err(E_BAD_ARG) }))
}

#[unsafe(no_mangle)]
pub extern "C" fn tr_withdraw(box_index: u32) -> i32 {
    code(with_state(|s| if s.roster.withdraw(box_index as usize) { Ok(0) } else { Err(E_BAD_ARG) }))
}

#[unsafe(no_mangle)]
pub extern "C" fn tr_all_ko() -> i32 {
    code(with_state(|s| Ok(s.roster.all_ko() as i32)))
}

/// 1 if the Catch materia was already granted in this game.
#[unsafe(no_mangle)]
pub extern "C" fn tr_kit_given() -> i32 {
    code(with_state(|s| Ok(s.roster.trainer.kit_given as i32)))
}

#[unsafe(no_mangle)]
pub extern "C" fn tr_mark_kit_given() -> i32 {
    code(with_state(|s| {
        s.roster.trainer.kit_given = true;
        Ok(0)
    }))
}

#[unsafe(no_mangle)]
pub extern "C" fn tr_set_legendary_unlocked(on: u32) -> i32 {
    code(with_state(|s| {
        s.roster.trainer.legendary_unlocked = on != 0;
        Ok(0)
    }))
}

/// The attack record + FF7-encoded name for a move id, for injecting into a scene's attack table.
///
/// # Safety
/// `out` must point to a writable TrMoveRecord.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tr_move_record(move_id: u16, out: *mut TrMoveRecord) -> i32 {
    if out.is_null() {
        return E_BAD_ARG;
    }
    code(with_state(|s| {
        let mv = s.dex.find_move(move_id).ok_or(E_NOT_FOUND)?;
        let record = mv.record().ok_or(E_BAD_ARG)?;
        unsafe { *out = TrMoveRecord { record, id: move_id, _pad: 0, ff_name: to_ff_text(&mv.name) } };
        Ok(0)
    }))
}

/// The species' original 184-byte enemy record (scene.bin layout), for injecting it into a battle scene.
///
/// # Safety
/// `out` must point to 184 writable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tr_species_record(species: u16, out: *mut u8) -> i32 {
    if out.is_null() {
        return E_BAD_ARG;
    }
    code(with_state(|s| {
        let rec = s.dex.get(species).ok_or(E_NOT_FOUND)?.record().ok_or(E_BAD_ARG)?;
        unsafe { std::ptr::copy_nonoverlapping(rec.as_ptr(), out, rec.len()) };
        Ok(rec.len() as i32)
    }))
}

/// Species name for display (UTF-8, NUL-terminated). Returns length or E_NOT_FOUND.
///
/// # Safety
/// `buf` must point to `len` writable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tr_species_name(species: u16, buf: *mut u8, len: u32) -> i32 {
    if buf.is_null() || len == 0 {
        return E_BAD_ARG;
    }
    code(with_state(|s| {
        let name = &s.dex.get(species).ok_or(E_NOT_FOUND)?.name;
        let n = name.len().min(len as usize - 1);
        unsafe {
            std::ptr::copy_nonoverlapping(name.as_ptr(), buf, n);
            *buf.add(n) = 0;
        }
        Ok(n as i32)
    }))
}

/// One move as the Monsters screen shows it.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct TrMoveLine {
    pub id: u16,
    pub mp: u16,
    pub power: u8,
    pub _pad: [u8; 3],
    /// UTF-8, NUL-terminated.
    pub name: [u8; 24],
    /// Elements, e.g. "Fire" or "Bolt/Wind"; empty for none (UTF-8, NUL-terminated).
    pub element: [u8; 24],
}
const _: () = assert!(std::mem::size_of::<TrMoveLine>() == 56);

/// Everything the Monsters screen shows about one party or box monster.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct TrMonsterDetail {
    pub view: TrMonsterView,
    pub xp: u32,
    /// Total XP at which the next level is reached (0 at the level cap).
    pub xp_next: u32,
    pub caught_level: u8,
    pub next_learn_level: u8,
    pub evolve_level: u8,
    pub equipped_count: u8,
    pub known_count: u8,
    pub _pad: [u8; 3],
    pub species_name: [u8; 24],
    /// "Earth/Water" (UTF-8, NUL-terminated).
    pub types: [u8; 32],
    /// "Fire, Ice" (UTF-8, NUL-terminated), empty if none.
    pub weak: [u8; 48],
    pub next_learn: [u8; 24],
    pub evolve_into: [u8; 24],
    /// Equipped moves first (menu order), then the other known moves.
    pub moves: [TrMoveLine; 12],
}
const _: () = assert!(std::mem::size_of::<TrMonsterDetail>() == 928);

fn put(dst: &mut [u8], s: &str) {
    let n = s.len().min(dst.len() - 1);
    dst[..n].copy_from_slice(&s.as_bytes()[..n]);
    dst[n] = 0;
}

fn title(s: &str) -> String {
    let mut c = s.chars();
    c.next().map(|f| f.to_uppercase().collect::<String>() + c.as_str()).unwrap_or_default()
}

fn detail(dex: &Dex, m: &Monster) -> TrMonsterDetail {
    let blank_move = TrMoveLine { id: 0xFFFF, mp: 0, power: 0, _pad: [0; 3], name: [0; 24], element: [0; 24] };
    let mut d = TrMonsterDetail {
        view: view(dex, m),
        xp: m.xp,
        xp_next: if m.level >= rules::MAX_LEVEL { 0 } else { rules::xp_for_level(m.level + 1) },
        caught_level: m.caught_level,
        next_learn_level: 0,
        evolve_level: 0,
        equipped_count: 0,
        known_count: 0,
        _pad: [0; 3],
        species_name: [0; 24],
        types: [0; 32],
        weak: [0; 48],
        next_learn: [0; 24],
        evolve_into: [0; 24],
        moves: [blank_move; 12],
    };
    let Some(s) = dex.get(m.species) else { return d };
    put(&mut d.species_name, &s.name);
    put(&mut d.types, &s.types.iter().map(|t| title(t)).collect::<Vec<_>>().join("/"));
    let weak = s.affinities.get("weak").map(|v| v.iter().map(|e| title(e)).collect::<Vec<_>>().join(", "));
    put(&mut d.weak, weak.as_deref().unwrap_or(""));
    let mut order: Vec<u16> = m.equipped.clone();
    order.extend(m.known.iter().filter(|id| !m.equipped.contains(id)));
    for (line, id) in d.moves.iter_mut().zip(order.iter()) {
        let Some(mv) = s.move_by_id(*id).or_else(|| dex.find_move(*id)) else { continue };
        line.id = *id;
        line.mp = mv.mp;
        line.power = mv.power;
        put(&mut line.name, &mv.name);
        put(&mut line.element, &mv.elements.iter().map(|e| title(e)).collect::<Vec<_>>().join("/"));
    }
    d.equipped_count = m.equipped.len().min(4) as u8;
    d.known_count = order.len().min(12) as u8;
    if let Some(l) = s.learnset.iter().filter(|l| l.level > m.level && !m.known.contains(&l.move_id)).min_by_key(|l| l.level) {
        d.next_learn_level = l.level;
        put(&mut d.next_learn, s.move_by_id(l.move_id).map(|mv| mv.name.as_str()).unwrap_or("?"));
    }
    if let Some(e) = &s.evolves_to {
        d.evolve_level = e.level;
        put(&mut d.evolve_into, dex.get(e.species).map(|t| t.name.as_str()).unwrap_or("?"));
    }
    d
}

/// The Monsters screen's view of party (`in_box` 0) or box (`in_box` 1) monster `index`.
///
/// # Safety
/// `out` must point to a writable TrMonsterDetail.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tr_monster_detail(in_box: u32, index: u32, out: *mut TrMonsterDetail) -> i32 {
    if out.is_null() {
        return E_BAD_ARG;
    }
    code(with_state(|s| {
        let list = if in_box != 0 { &s.roster.storage } else { &s.roster.party };
        let m = list.get(index as usize).ok_or(E_NOT_FOUND)?;
        unsafe { *out = detail(&s.dex, m) };
        Ok(0)
    }))
}

/// Equip known move `move_id` in battle-menu slot `slot` (0..3) of party/box monster `index`. If it is already
/// equipped elsewhere the two slots swap; `slot` past the end appends (up to 4).
#[unsafe(no_mangle)]
pub extern "C" fn tr_equip_move(in_box: u32, index: u32, slot: u32, move_id: u16) -> i32 {
    code(with_state(|s| {
        let list = if in_box != 0 { &mut s.roster.storage } else { &mut s.roster.party };
        let m = list.get_mut(index as usize).ok_or(E_NOT_FOUND)?;
        if !m.known.contains(&move_id) || slot >= 4 {
            return Err(E_BAD_ARG);
        }
        let slot = slot as usize;
        match m.equipped.iter().position(|&id| id == move_id) {
            Some(cur) if slot < m.equipped.len() => m.equipped.swap(cur, slot),
            Some(_) => {}
            None if slot < m.equipped.len() => m.equipped[slot] = move_id,
            None => m.equipped.push(move_id),
        }
        Ok(0)
    }))
}
