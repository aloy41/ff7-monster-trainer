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
