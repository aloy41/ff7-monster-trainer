#include "trainer.h"
#include "monsters.h"
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include "game.h"
#include "log.h"
#include "patch.h"
#include "hud.h"
#include "text.h"

namespace trainer {

const char* BUILD = "phase3-sendout " __DATE__ " " __TIME__;
TrApi api = {};
bool core_ready = false;
volatile int test_layout = -1;
volatile bool test_sweep = false;

namespace {

HMODULE g_core = nullptr;

template <typename T>
bool bind(T& slot, const char* name) {
    slot = reinterpret_cast<T>(GetProcAddress(g_core, name));
    if (!slot) tlog("core: missing export %s", name);
    return slot != nullptr;
}

void core_error(const char* what) {
    char buf[256] = {};
    if (api.last_error) api.last_error(reinterpret_cast<uint8_t*>(buf), sizeof buf);
    tlog("core: %s failed: %s", what, buf);
}

// ---- send-out: the lead monster fights on Cloud's side as a manipulated actor ------------------------------

struct Puppet {
    bool planned = false;    // placement written at scene load
    bool live = false;       // stats/manipulation applied, still standing
    int actor = -1;          // enemy actor index 4..9
    uint32_t uid = 0;
    uint16_t species = 0;
    char name[32] = {};
    int cleared_ticks = -1;  // battle frames since the wild side was cleared (-1 = not yet)
    uint16_t moves[3] = {0xFFFF, 0xFFFF, 0xFFFF};  // what Cloud's Manipulate menu offers
    int sweep_used = 0;      // test_sweep: moves forced so far
    int sweep_finish = 0;    // test_sweep: turns since every forced move played out
};
Puppet g_puppet;
char g_battle_start_message[64] = {};  // shown on the first battle frame (scene load runs before the battle starts)
volatile bool g_trainer_turns = false;  // F8: Cloud acts himself (Catch!) while the monster waits
// Cloud's command is waiting to play out: the link it was given with stays until then. The engine ends a turn
// through the link, so switching it under a pending action left Cloud's ATB full forever with no menu (F8 soft-lock).
bool g_cloud_action_pending = false;
// Cloud's command to the monster was still queued when it fainted: the engine drops that action, and Cloud's TIME bar
// stays full with no menu for the rest of the battle (D12). Measured: it sometimes recovers on its own (~9 s), often
// never. settle_fallen_monster restarts his ATB after FALLEN_GRACE_MS.
bool g_orphaned = false;
bool g_face_pending = false;  // the monster takes Cloud's facing once his model has one (face_like_cloud)
int g_fallen_actor = -1;  // the fainted monster, until settle_fallen_monster has checked its death
DWORD g_fallen_at = 0;
constexpr DWORD FALLEN_GRACE_MS = 3000;
uint8_t g_held_link = 0;
constexpr int16_t SENDOUT_X = -600, SENDOUT_Z = 1100;  // in front of Cloud (party side is +z)
constexpr int RECALL_DELAY_TICKS = 120;  // ~2 s of battle frames between "wild side cleared" and the recall

// Make `species` available in the loaded scene. Returns its enemy-type slot (0-2), or -1.
// Uses a free type slot, or one the chosen formation doesn't place (scene types are shared by its 4 formations).
int ensure_species_loaded(const TrMonsterView& v) {
    uint16_t* ids = ff7::scene_enemy_ids();
    for (int k = 0; k < 3; k++)
        if (ids[k] == v.species) return k;
    bool placed[3] = {};
    for (int p = 0; p < 6; p++) {
        const uint16_t id = ff7::placement(p)->enemy_id;
        for (int k = 0; k < 3; k++)
            if (id != 0xFFFF && ids[k] == id) placed[k] = true;
    }
    int slot = -1;
    for (int k = 0; k < 3 && slot < 0; k++)
        if (ids[k] == 0xFFFF) slot = k;
    for (int k = 0; k < 3 && slot < 0; k++)
        if (!placed[k]) slot = k;
    if (slot < 0) {
        tlog("SENDOUT inject: no free enemy-type slot (formation places all 3 types)");
        return -1;
    }
    uint8_t rec[184];
    if (api.species_record(v.species, rec) != 184) {
        core_error("tr_species_record");
        return -1;
    }
    const uint16_t replaced = ids[slot];
    memcpy(ff7::scene_enemy(slot), rec, sizeof rec);
    ids[slot] = v.species;
    ff7::scene_ai_offsets()[slot] = 0xFFFF;  // manipulated: it never runs its own AI
    tlog("SENDOUT inject: species %u (%s) -> enemy-type slot %d (was %04X), record + no AI", v.species, v.name, slot,
         replaced);
    return slot;
}

// Put the monster's equipped moves where Cloud's Manipulate menu and the battle engine look for them.
void install_moves(const TrMonsterView& v, int type_slot) {
    ff7::EnemyRecord* e = ff7::scene_enemy(type_slot);
    uint16_t* ids = ff7::scene_attack_ids();
    // Only moves the species data still offers (a roster saved by an older build may hold a move that was dropped,
    // e.g. a kernel spell): anything without a record stays out of Cloud's menu.
    TrMoveRecord probe;
    for (int i = 0; i < 3; i++)
        e->manip_ids[i] = (v.moves[i] != 0xFFFF && api.move_record(v.moves[i], &probe) >= 0) ? v.moves[i] : 0xFFFF;
    for (int i = 0; i < 4; i++) {
        const uint16_t mv = v.moves[i];
        if (mv == 0xFFFF) continue;
        int have = -1, free = -1;
        for (int j = 0; j < ff7::SCENE_ATTACK_SLOTS; j++) {
            if (ids[j] == mv) have = j;
            if (ids[j] == 0xFFFF && free < 0) free = j;
        }
        if (have >= 0) continue;
        TrMoveRecord r;
        if (free < 0 || api.move_record(mv, &r) < 0) {
            tlog("SENDOUT move %u: %s", mv, free < 0 ? "attack table full" : "no record");
            continue;
        }
        memcpy(reinterpret_cast<uint8_t*>(ff7::SCENE_ATTACKS) + free * 28, r.record, 28);
        memcpy(reinterpret_cast<uint8_t*>(ff7::SCENE_ATTACK_NAMES) + free * 32, r.ff_name, 32);
        ids[free] = mv;
        char nm[33];
        ff7::ff_text(r.ff_name, 32, nm);
        tlog("SENDOUT move %u '%s' -> attack slot %d", mv, nm, free);
    }
}

void plan_send_out() {
    g_puppet = Puppet{};
    g_trainer_turns = false;
    g_cloud_action_pending = false;
    g_face_pending = false;
    g_orphaned = false;
    g_fallen_actor = -1;
    g_battle_start_message[0] = 0;
    hud::set_battle_monster(false, "", 0, 0, 0);
    if (!core_ready) return;
    int32_t act[2] = {-1, -1};
    if (api.active(act) <= 0 || act[0] < 0) {
        tlog("SENDOUT none: no conscious monster in the party");
        return;
    }
    TrMonsterView v;
    if (api.party_get(uint32_t(act[0]), &v) < 0) return;
    int free_place = -1;
    for (int k = 0; k < 6 && free_place < 0; k++)
        if (ff7::placement(k)->enemy_id == 0xFFFF) free_place = k;
    if (free_place < 0) {
        tlog("SENDOUT no free placement (formation uses all 6): Cloud fights alone");
        sprintf_s(g_battle_start_message, "%s can't join this battle!", v.name);
        return;
    }
    const int type_slot = ensure_species_loaded(v);
    if (type_slot < 0) {
        sprintf_s(g_battle_start_message, "%s can't join this battle!", v.name);
        return;
    }
    install_moves(v, type_slot);
    // Row and flags come from a real placement of this formation (placement 0 may be empty).
    const ff7::Placement* ref = nullptr;
    for (int k = 0; k < 6 && !ref; k++)
        if (ff7::placement(k)->enemy_id != 0xFFFF) ref = ff7::placement(k);
    const uint16_t row = ref ? ref->row : 1;
    const uint32_t flags = ref ? ref->flags : 0x9D;
    for (int k = free_place; k < 6; k++) {
        ff7::Placement* p = ff7::placement(k);
        if (p->enemy_id != 0xFFFF) continue;
        *p = ff7::Placement{v.species, SENDOUT_X, 0, SENDOUT_Z, row, 0, flags};
        g_puppet.planned = true;
        g_puppet.actor = 4 + k;
        // Forget whatever an earlier battle left in this actor, so apply_send_out only fires once it is rebuilt.
        ff7::actor(g_puppet.actor)->max_hp = 0;
        ff7::actor(g_puppet.actor)->formation_id = 0xFFFF;
        g_puppet.uid = v.uid;
        g_puppet.species = v.species;
        strncpy_s(g_puppet.name, v.name, _TRUNCATE);
        memcpy(g_puppet.moves, ff7::scene_enemy(type_slot)->manip_ids, sizeof g_puppet.moves);
        tlog("SENDOUT plan: %s L%u (uid %u) -> placement %d / actor %d at (%d, 0, %d)", v.name, v.level, v.uid, k,
             g_puppet.actor, SENDOUT_X, SENDOUT_Z);
        return;
    }
    tlog("SENDOUT no free placement (formation uses all 6)");
}

// The monster stands with Cloud, whatever the layout: its home spot (where it walks back to after acting) is set
// beside Cloud's, in his row, and it faces the way he does. The formation placement only got it into
// the scene; in a back or pincer attack that spot is among the enemies. The engine computes the party's homes
// (0x42AE73) and the enemies' (0x42B8D2) at battle start; this runs after each, so whichever comes last, it wins.
void __cdecl after_homes_set(uint32_t) {
    if (!(g_puppet.planned || g_puppet.live) || g_puppet.actor < 4) return;
    const int16_t* cloud = ff7::home_position(0);
    int16_t* home = ff7::home_position(g_puppet.actor);
    home[0] = int16_t(cloud[0] + (cloud[0] > 0 ? -900 : 900));
    home[1] = cloud[1];
    home[2] = cloud[2];  // in his row, like a party member (a step toward the enemies is among them in a back attack)
    int16_t* pos = ff7::model_position(g_puppet.actor);
    for (int i = 0; i < 3; i++) pos[i] = home[i];
    g_face_pending = true;
    tlog("SENDOUT layout %u: Cloud home (%d, %d, %d) -> %s home (%d, %d, %d)", ff7::battle_layout(), cloud[0], cloud[1],
         cloud[2], g_puppet.name, home[0], home[1], home[2]);
}

// Target selection for a monster move starts on Cloud (the menu sees the move from the puppet's seat, where the
// "other side" is Cloud's). A monster move only ever hits a wild enemy (before_dispatch), so whenever its cursor is on
// the party side it goes to the first living wild enemy instead. 0xDC3C70 = the command being targeted (0x20 =
// monster move); the cursor's actor is at 0xDC3C94, mirrored at 0xDC3C9C (measured: writing both moves the hand).
int first_living_wild();

void aim_at_wild_enemies() {
    if (!g_puppet.live || g_trainer_turns || ff7::menu_party_slot() != 0) return;
    if (ff7::issued_command() != ff7::CMD_ENEMY_ATTACK || ff7::issued_target_index() >= 4) return;
    const int t = first_living_wild();
    if (t < 0) return;
    *reinterpret_cast<volatile uint8_t*>(0xDC3C94) = uint8_t(t);
    *reinterpret_cast<volatile uint8_t*>(0xDC3C9C) = uint8_t(t);
}

// Battle tick: once Cloud's model has its facing, the monster takes the same one (also as the facing it returns to).
void face_like_cloud() {
    if (!g_face_pending || !g_puppet.live) return;
    const uint16_t* r = ff7::model_initial_rotation(0);
    if (!r[0] && !r[1] && !r[2] && !ff7::model_rotation(0)[1]) return;  // not set up yet
    for (int i = 0; i < 3; i++) {
        ff7::model_rotation(g_puppet.actor)[i] = ff7::model_rotation(0)[i];
        ff7::model_initial_rotation(g_puppet.actor)[i] = r[i];
    }
    g_face_pending = false;
    tlog("SENDOUT %s faces like Cloud (rot %u)", g_puppet.name, ff7::model_rotation(0)[1]);
}

// Runs on the monitor thread once the battle's actors exist.
void apply_send_out() {
    if (!g_puppet.planned || g_puppet.live) return;
    ff7::ActorVars* a = ff7::actor(g_puppet.actor);
    if (a->max_hp <= 0 || a->formation_id != g_puppet.species) return;  // not built yet
    TrMonsterView v;
    int32_t act[2] = {-1, -1};
    api.active(act);
    if (act[0] < 0 || api.party_get(uint32_t(act[0]), &v) < 0 || v.uid != g_puppet.uid) return;
    a->level = v.level;
    a->max_hp = v.hp_max;
    a->current_hp = v.hp;
    a->max_mp = v.mp_max;
    a->current_mp = v.mp;
    a->phys_atk = v.stats[0];
    a->defense = v.stats[1];
    a->mag_atk = v.stats[2];
    a->m_defense = v.stats[3];
    a->dexterity = v.stats[4];
    a->luck = v.stats[5];
    a->exp_value = 0;  // no rewards for "defeating" our own monster
    a->gil_value = 0;
    a->ap_value = 0;
    a->status |= ff7::ST_MANIPULATE;
    ff7::cloud_manip_target() = uint8_t(g_puppet.actor);
    const uint16_t bit = uint16_t(1u << g_puppet.actor);
    ff7::party_actors_mask() |= bit;
    ff7::enemy_actors_mask() &= uint16_t(~bit);
    g_puppet.planned = false;
    g_puppet.live = true;
    hud::set_battle_monster(true, v.name, v.level, a->current_hp, a->max_hp);
    char go[48];
    sprintf_s(go, "Go, %s!", v.name);
    text::show_battle_message(go);
    tlog("SENDOUT go: %s! actor[%d] L%u HP %d/%d MP %u/%u str %u def %u mag %u spd %u; Cloud commands it", v.name,
         g_puppet.actor, a->level, a->current_hp, a->max_hp, a->current_mp, a->max_mp, a->phys_atk, a->defense,
         a->mag_atk, a->dexterity);
}

bool is_wild_enemy(int i) {
    return i >= 4 && i <= 9 && i != g_puppet.actor && ff7::actor(i)->max_hp > 0;
}

int first_living_wild() {
    for (int i = 4; i <= 9; i++)
        if (is_wild_enemy(i) && ff7::actor(i)->current_hp > 0 && !(ff7::actor(i)->status & ff7::ST_DEATH)) return i;
    return -1;
}

// Battle bar text: the full wording, or the brief one when long names would run past the bar (MESSAGE_MAX).
template <size_t N>
void fit_message(char (&out)[N], const char* full, const char* brief, ...) {
    va_list a, b;
    va_start(a, brief);
    va_copy(b, a);
    vsprintf_s(out, full, a);
    if (strlen(out) > size_t(text::MESSAGE_MAX)) vsprintf_s(out, brief, b);
    va_end(b);
    va_end(a);
}

struct PartyNames {
    int n = 0;
    uint32_t uid[6] = {};
    char name[6][32] = {};
};

const char* before_name(const PartyNames& p, uint32_t uid) {
    for (int i = 0; i < p.n; i++)
        if (p.uid[i] == uid) return p.name[i];
    return "?";
}

bool find_party_uid(uint32_t uid, TrMonsterView* out) {
    for (int i = 0; i < api.party_len(); i++)
        if (api.party_get(uint32_t(i), out) >= 0 && out->uid == uid) return true;
    return false;
}

// Wild side cleared, or our monster went down: write its HP/MP back to the roster.
void puppet_watch() {
    if (!g_puppet.live) return;
    ff7::ActorVars* a = ff7::actor(g_puppet.actor);
    hud::set_battle_monster(true, g_puppet.name, a->level, a->current_hp, a->max_hp);
    if (a->current_hp <= 0) {
        api.set_vitals(g_puppet.uid, 0, a->current_mp);
        ff7::cloud_manip_target() = 0;
        g_puppet.live = false;
        g_orphaned = g_cloud_action_pending && g_held_link == uint8_t(g_puppet.actor) && ff7::cloud_atb() == 0xFFFF;
        g_fallen_actor = g_puppet.actor;
        g_fallen_at = GetTickCount();
        g_cloud_action_pending = false;
        tlog("SENDOUT %s fainted! (actor[%d])%s", g_puppet.name, g_puppet.actor,
             g_orphaned ? "; Cloud's command to it was still pending" : "");
        char msg[48];
        sprintf_s(msg, "%s fainted!", g_puppet.name);
        text::show_battle_message(msg);
        return;
    }
    // The engine drops the manipulation after the monster's turn once it sits on the party side; keep the trainer link
    // (status, Cloud's link byte, side masks) asserted for as long as the monster is out.
    const uint16_t bit = uint16_t(1u << g_puppet.actor);
    if (g_cloud_action_pending && ff7::cloud_atb() != 0xFFFF) {
        g_cloud_action_pending = false;  // his action played out: an F8 pressed meanwhile applies now
        tlog("SENDOUT Cloud's action played out (ATB %04X); link follows F8 again", ff7::cloud_atb());
    }
    const uint8_t want_link = g_cloud_action_pending ? g_held_link : g_trainer_turns ? 0 : uint8_t(g_puppet.actor);
    const bool lost = !(a->status & ff7::ST_MANIPULATE) || ff7::cloud_manip_target() != want_link ||
                      !(ff7::party_actors_mask() & bit) || (ff7::enemy_actors_mask() & bit);
    if (lost) {
        static DWORD last_note = 0;
        if (GetTickCount() - last_note > 2000) {
            tlog("SENDOUT re-link: status %08X link %u party %04X enemy %04X", a->status, ff7::cloud_manip_target(),
                 ff7::party_actors_mask(), ff7::enemy_actors_mask());
            last_note = GetTickCount();
        }
        a->status |= ff7::ST_MANIPULATE;
        ff7::cloud_manip_target() = want_link;
        ff7::party_actors_mask() |= bit;
        ff7::enemy_actors_mask() &= uint16_t(~bit);
    }
    if (test_sweep && g_puppet.cleared_ticks < 0) {
        // Nobody falls until every forced move has played out (a move that hangs never gets the monster another turn;
        // before_dispatch then downs the wild side, so moves that deal no damage end too).
        for (int i = 4; i <= 9; i++) {
            ff7::ActorVars* w = ff7::actor(i);
            if (!is_wild_enemy(i) || w->current_hp <= 0 || (w->status & ff7::ST_DEATH)) continue;
            if (!g_puppet.sweep_finish) w->current_hp = w->max_hp = 30000;
        }
        if (!g_puppet.sweep_finish) {
            a->current_hp = a->max_hp;
            ff7::actor(0)->current_hp = ff7::actor(0)->max_hp;
        }
    }
    if (g_puppet.cleared_ticks >= 0) {
        // Recall only once the last action has fully played out: killing the actor while an action that involves it
        // is still running leaves the battle waiting forever (measured: battle_mode 8 soft-lock).
        // Growth messages first: the battle ends once the monster leaves, and the rest of the queue would be lost.
        if (++g_puppet.cleared_ticks < RECALL_DELAY_TICKS || text::battle_messages_pending()) return;
        tlog("SENDOUT recall: %s leaves the battle", g_puppet.name);
        ff7::cloud_manip_target() = 0;
        a->status = (a->status & ~ff7::ST_MANIPULATE) | ff7::ST_DEATH;
        a->current_hp = 0;
        g_puppet.live = false;
        return;
    }
    if (first_living_wild() >= 0) return;
    g_puppet.cleared_ticks = 0;
    uint32_t xp = 0;
    for (int i = 4; i <= 9; i++)
        if (is_wild_enemy(i)) xp += ff7::actor(i)->exp_value;
    api.set_vitals(g_puppet.uid, uint16_t(a->current_hp), a->current_mp);
    // Names before the XP: an evolution renames the monster.
    PartyNames names;
    for (int i = 0; i < 6 && i < api.party_len(); i++) {
        TrMonsterView v;
        if (api.party_get(uint32_t(i), &v) < 0) continue;
        names.uid[names.n] = v.uid;
        strncpy_s(names.name[names.n++], v.name, _TRUNCATE);
    }
    TrGrowth grown[6] = {};
    int n = api.award_xp(xp, grown, 6);
    if (n > 6) n = 6;  // the core reports how many grew; only 6 records fit
    tlog("SENDOUT wild side cleared: %s returns with HP %d/%d; party XP +%u", g_puppet.name, a->current_hp, a->max_hp, xp);
    for (int i = 0; i < n; i++) {
        tlog("  GROWTH uid %u +%u level(s)%s, learned %u move(s)", grown[i].uid, grown[i].levels_gained,
             grown[i].evolved ? ", EVOLVED" : "", grown[i].learned_count);
        // The monster that fought gets the full story; the bench only announces evolutions (levels show in the roster).
        TrMonsterView after;
        if (!find_party_uid(grown[i].uid, &after)) continue;
        char msg[64];
        if (grown[i].uid != g_puppet.uid) {
            if (grown[i].evolved) {
                fit_message(msg, "%s evolved into %s!", "%s became %s!", before_name(names, grown[i].uid), after.name);
                text::show_battle_message(msg);
            }
            continue;
        }
        if (grown[i].evolved) {
            fit_message(msg, "%s evolved into %s!", "%s became %s!", g_puppet.name, after.name);
            text::show_battle_message(msg);
        }
        sprintf_s(msg, "%s reached Lv %u!", after.name, after.level);
        text::show_battle_message(msg);
        const int learned = grown[i].learned_count < 3 ? grown[i].learned_count : 3;
        for (int k = 0; k < learned; k++) {
            TrMoveRecord mv;
            char mvname[33] = "";
            if (api.move_record(grown[i].learned[k], &mv) >= 0) ff7::ff_text(mv.ff_name, 32, mvname);
            if (!mvname[0]) continue;
            fit_message(msg, "%s learned %s!", "%s got %s!", after.name, mvname);
            text::show_battle_message(msg);
        }
        if (grown[i].learned_count > 3) {  // only 3 move ids fit the growth record; the rest are in the roster
            const unsigned more = grown[i].learned_count - 3u;
            sprintf_s(msg, "%s learned %u more move%s!", after.name, more, more == 1 ? "" : "s");
            text::show_battle_message(msg);
        }
    }
    if (api.save_slot(TR_AUTOSAVE) < 0) core_error("tr_save_slot(autosave)");
    // Recall follows after RECALL_DELAY_TICKS (above): the battle only ends when every enemy-side actor is down.
}

// ---- battle events ---------------------------------------------------------------------------------------

void reset_battle_state();  // below

void __cdecl after_scene_loaded(uint32_t) {
    uint16_t* ids = ff7::scene_enemy_ids();
    tlog("SCENE formation=%u battle_id=%u location=%u enemies=[%04X %04X %04X]", ff7::formation_id(),
         *reinterpret_cast<uint16_t*>(0xCC0D8A), *reinterpret_cast<uint16_t*>(ff7::SCENE_SETUP), ids[0], ids[1],
         ids[2]);
    for (int i = 0; i < 3; i++) {
        if (ids[i] == 0xFFFF) continue;
        const ff7::EnemyRecord* e = ff7::scene_enemy(i);
        char name[33];
        ff7::ff_text(e->name, 32, name);
        char sp[64] = "";
        if (core_ready) api.species_name(ids[i], reinterpret_cast<uint8_t*>(sp), sizeof sp);
        tlog("  enemy[%d] id=%u '%s' L%u HP%u MP%u  dex:'%s'", i, ids[i], name, e->level, e->hp, e->mp,
             sp[0] ? sp : "(not capturable)");
    }
    int used = 0;
    for (int i = 0; i < ff7::SCENE_ATTACK_SLOTS; i++) used += ff7::scene_attack_ids()[i] != 0xFFFF;
    tlog("  attack table: %d of %d slots used", used, ff7::SCENE_ATTACK_SLOTS);
    // Catch is FF7's Steal underneath: take the stealable items out of every enemy (rate bit 0x80 = steal, else
    // drop), so a Catch never steals and Steal's result text is always ours. Drops are untouched.
    for (int i = 0; i < 3; i++) {
        if (ids[i] == 0xFFFF) continue;
        ff7::EnemyRecord* e = ff7::scene_enemy(i);
        for (int k = 0; k < 4; k++)
            if (e->item_rates[k] & 0x80) e->item_ids[k] = 0xFFFF;
    }
    if (test_layout >= 0) {
        tlog("  TEST layout %u -> %d", ff7::battle_layout(), test_layout);
        ff7::battle_layout() = uint8_t(test_layout);
    }
    reset_battle_state();
    plan_send_out();
}

// Cloud chose Catch on `target`; the roll happens when the Steal action actually executes (its result text).
struct CatchIntent {
    volatile bool armed = false;
    int target = -1;
};
CatchIntent g_catch;
char g_capture_message[64] = {};

void try_capture(int target);  // below

void __cdecl before_add_text(uint32_t* args) {
    const uint16_t idx = uint16_t(args[0]);
    tlog("TEXTEVT idx=%u wait=%u frames=%u x=%u", idx, args[1] & 0xFF, args[2] & 0xFF, args[3] & 0xFFFF);
    // No time limit: Cloud's action can wait 20 s and more behind enemy turns. An intent whose action never runs
    // (Cloud fell, the battle ended) is cleared when the next battle starts, or replaced by the next Catch.
    if (!g_catch.armed) return;
    // Only Steal's own result text: the action being shown is Cloud's Steal (an enemy's text never matches).
    if (ff7::last_command() != ff7::CMD_STEAL || ff7::active_actor_id() != 0) return;
    g_catch.armed = false;
    try_capture(g_catch.target);
    if (g_capture_message[0]) {
        args[0] = text::set_message(g_capture_message);
        tlog("TEXTEVT steal text %u -> our message '%s'", idx, g_capture_message);
    }
}

// A capture rolled at dispatch, applied once Cloud's Steal animation has played (monitor thread).
struct PendingCapture {
    volatile bool armed = false;
    int actor = -1;
    DWORD at = 0;
    char name[64] = {};
};
PendingCapture g_pending;

// New battle: nothing from the previous one may leak in.
void reset_battle_state() {
    g_pending = PendingCapture{};
    g_catch = CatchIntent{};
    g_capture_message[0] = 0;
    text::reset_battle_messages();
}

float status_bonus(uint32_t status) {
    if (status & (ff7::ST_STOP | ff7::ST_PARALYSIS | ff7::ST_PETRIFY)) return 2.0f;
    if (status & ff7::ST_SLEEP) return 1.5f;
    return 1.0f;
}

void try_capture(int target) {
    g_capture_message[0] = 0;
    if (g_puppet.live && target == g_puppet.actor) {
        tlog("CAPTURE skipped: actor[%d] is our own monster", target);
        strcpy_s(g_capture_message, "That's your monster!");
        return;
    }
    if (target < 4 || target > 9) {
        tlog("CAPTURE skipped: target %d is not an enemy", target);
        return;
    }
    const ff7::ActorVars* a = ff7::actor(target);
    if (a->current_hp <= 0 || (a->status & ff7::ST_DEATH)) {
        tlog("CAPTURE skipped: actor[%d] is already down", target);
        strcpy_s(g_capture_message, "There's nothing left to catch!");
        return;
    }
    const uint16_t species = a->formation_id;
    char name[64] = "";
    if (api.species_name(species, reinterpret_cast<uint8_t*>(name), sizeof name) < 0 || !name[0]) {
        // Not in the dex (it can't fight for Cloud): the game's own name for it.
        strcpy_s(name, "It");
        for (int k = 0; k < 3; k++)
            if (ff7::scene_enemy_ids()[k] == species) ff7::ff_text(ff7::scene_enemy(k)->name, 32, name);
    }
    const float bonus = status_bonus(a->status);
    const float chance = api.capture_chance(species, uint32_t(a->current_hp), uint32_t(a->max_hp), uint32_t(bonus * 100), 0);
    const uint16_t scene = *reinterpret_cast<uint16_t*>(0xCC0D8A);
    const int r = api.try_capture(species, a->level, uint32_t(a->current_hp), uint32_t(a->max_hp), uint32_t(bonus * 100), 0, scene);
    tlog("CAPTURE roll: actor[%d] %s (species %u) L%u HP %d/%d status %08X -> chance %.0f%% -> %s", target, name,
         species, a->level, a->current_hp, a->max_hp, a->status, chance * 100,
         r == TR_CAUGHT ? "CAUGHT (party)" : r == TR_CAUGHT_TO_BOX ? "CAUGHT (box)" : r == TR_FAILED ? "broke free" :
         r == TR_NOT_CAPTURABLE ? "not capturable" : "error");
    if (r == TR_CAUGHT) sprintf_s(g_capture_message, "Gotcha! %s was caught!", name);
    else if (r == TR_CAUGHT_TO_BOX) fit_message(g_capture_message, "Gotcha! %s was sent to the PC box.", "%s went to the PC box!", name);
    else if (r == TR_FAILED) sprintf_s(g_capture_message, "Oh no! %s broke free!", name);
    else if (r == TR_NOT_CAPTURABLE) sprintf_s(g_capture_message, "%s can't be caught!", name);
    if (r == TR_CAUGHT || r == TR_CAUGHT_TO_BOX) {
        strcpy_s(g_pending.name, name);
        g_pending.actor = target;
        g_pending.at = GetTickCount() - 1900;  // the roll is at Steal's result text: leave ~0.6 s for the message
        g_pending.armed = true;
        if (api.save_slot(TR_AUTOSAVE) < 0) core_error("tr_save_slot(autosave)");
        tlog("CAPTURE roster: party %d, box %d (autosaved)", api.party_len(), api.box_len());
    }
}

void __cdecl before_dispatch() {
    const uint8_t slot = ff7::menu_party_slot();
    const uint8_t chr = slot < 3 ? ff7::party_members()[slot] : 0xFF;
    const uint8_t cmd = ff7::issued_command();
    tlog("ACTION party slot %u (char %u) command=%u action=%u target_type=%u target_index=%u", slot, chr, cmd,
         ff7::issued_action(), ff7::issued_target_type(), ff7::issued_target_index());
    if (chr == 0 && g_puppet.live) {
        g_cloud_action_pending = true;
        g_held_link = ff7::cloud_manip_target();
    }
    // Cloud's Steal is the trainer's Catch; the roll waits until the action runs (before_add_text).
    if (core_ready && chr == 0 && cmd == ff7::CMD_STEAL) {
        g_catch.target = ff7::issued_target_index();
        g_catch.armed = true;
    }
    // Cloud commanding his monster: its moves default to "the other side" from the monster's point of view, which is
    // our party, and an all-target move (target type 6) takes in every enemy-type actor - the monster itself
    // included, which soft-locked the battle. Every monster move becomes a single hit on a living wild enemy
    // (type 0 + actor index; measured: index 0 = Cloud, 4..9 = enemy actors).
    if (chr == 0 && cmd == ff7::CMD_ENEMY_ATTACK && g_puppet.live) {
        if (test_sweep) {
            while (g_puppet.sweep_used < 3 && g_puppet.moves[g_puppet.sweep_used] == 0xFFFF) g_puppet.sweep_used++;
            // Another turn after the last forced move = it played out. The wild side drops to 1 HP, so this turn's
            // hit wins the battle; if the move deals no damage, the turn after downs them before its action runs
            // (downing an actor mid-action is what soft-locks). That action then has no wild target and lands on
            // Cloud, who gets the HP to take it.
            if (g_puppet.sweep_used >= 3 && g_puppet.sweep_finish < 2) {
                const bool down = ++g_puppet.sweep_finish == 2;
                for (int i = 4; i <= 9; i++)
                    if (is_wild_enemy(i) && ff7::actor(i)->current_hp > 0) {
                        ff7::actor(i)->current_hp = down ? 0 : 1;
                        if (down) ff7::actor(i)->status |= ff7::ST_DEATH;
                    }
                if (down) ff7::actor(0)->current_hp = ff7::actor(0)->max_hp = 9999;
                tlog("SWEEP %s: every move played out; the wild side %s", g_puppet.name, down ? "falls" : "drops to 1 HP");
            }
            // The issued action of an enemy attack is its slot in the scene's attack table, not the attack id.
            if (g_puppet.sweep_used < 3) {
                const uint16_t mv = g_puppet.moves[g_puppet.sweep_used++];
                for (int j = 0; j < ff7::SCENE_ATTACK_SLOTS; j++)
                    if (ff7::scene_attack_ids()[j] == mv) ff7::issued_action() = uint16_t(j);
                tlog("SWEEP %s (species %u) uses move %u (attack slot %u)", g_puppet.name, g_puppet.species, mv,
                     ff7::issued_action());
            }
        }
        const uint8_t type = ff7::issued_target_type(), idx = ff7::issued_target_index();
        const bool ok = type == 0 && is_wild_enemy(idx) && ff7::actor(idx)->current_hp > 0 &&
                        !(ff7::actor(idx)->status & ff7::ST_DEATH);
        const int wild = ok ? idx : first_living_wild();
        if (!ok && wild >= 0) {
            tlog("SENDOUT retarget: %s's attack slot %u (target type %u index %u) -> single wild actor[%d]", g_puppet.name,
                 ff7::issued_action(), type, idx, wild);
            ff7::issued_target_type() = 0;
            ff7::issued_target_index() = uint8_t(wild);
        }
    }
}

void __cdecl after_battle_end_menu(uint32_t) {
    static uint16_t last = 0xFFFF;
    const uint16_t m = ff7::battle_end_mode();
    if (m == last) return;
    last = m;
    tlog("BATTLE-END menu mode=%u formation=%u gil=%u", m, ff7::formation_id(), ff7::gil());
    if (m == 5) last = 0xFFFF;  // menu closes after 5; re-arm for the next battle
}

// ---- save / load: the roster lives next to each FF7 save --------------------------------------------------

// Which saveNN.ff7 holds the game that was just saved/loaded? The save/load calls only pass the slot within a file
// (measured: file 2 / slot 1 passed 0, file 1 / slot 2 passed 1), so we find the file by content: the live savemap
// appears byte-for-byte in the save file it was written to / read from. Returns the file number, or -1.
char g_game_dir[MAX_PATH] = {};

// saveNN.ff7 = 9-byte header + 15 slots of 0x10F4 bytes (one savemap each, checksum first).
constexpr size_t SAVE_HEADER = 9, SAVE_SLOT = 0x10F4;

char g_save_dir[MAX_PATH] = {};  // the game's save folder (FFNx save_path, relative to the game folder)

void resolve_save_dir() {
    // FFNx.toml: save_path = "save" (default). Relative paths are relative to the game folder.
    strcpy_s(g_save_dir, g_game_dir);
    strcat_s(g_save_dir, "\\save");
    char toml[MAX_PATH];
    sprintf_s(toml, "%s\\FFNx.toml", g_game_dir);
    FILE* f = nullptr;
    if (fopen_s(&f, toml, "r") != 0 || !f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char value[MAX_PATH] = {};
        if (sscanf_s(line, " save_path = \"%259[^\"]\"", value, unsigned(sizeof value)) == 1 && value[0]) {
            if (value[1] == ':' || value[0] == '\\' || value[0] == '/') strcpy_s(g_save_dir, value);
            else sprintf_s(g_save_dir, "%s\\%s", g_game_dir, value);
            break;
        }
    }
    fclose(f);
    tlog("SAVE folder: %s", g_save_dir);
}

// Every save slot whose bytes at `needle_offset` equal `needle`. Returns how many keys (file << 4 | slot) were found.
int find_in_save_files(const uint8_t* needle, size_t n, size_t needle_offset, int* keys, int cap) {
    static uint8_t buf[SAVE_HEADER + SAVE_SLOT * 15 + 64];
    int found = 0;
    for (int f = 0; f < 15; f++) {
        char path[MAX_PATH];
        sprintf_s(path, "%s\\save%02d.ff7", g_save_dir, f);
        FILE* fp = nullptr;
        if (fopen_s(&fp, path, "rb") != 0 || !fp) continue;
        const size_t got = fread(buf, 1, sizeof buf, fp);
        fclose(fp);
        for (int s = 0; s < 15; s++) {
            const size_t o = SAVE_HEADER + s * SAVE_SLOT + needle_offset;
            if (o + n <= got && memcmp(buf + o, needle, n) == 0 && found < cap) keys[found++] = f << 4 | s;
        }
    }
    return found;
}

// The save/load calls' own argument can't be trusted after the call (the callee reuses its stack slot: the same
// file-2 save was seen passing 00 and 10), so the roster key comes from content alone: the live savemap appears
// byte-for-byte in the slot it was just written to / read from. Almost the whole savemap is compared (skipping the
// checksum), play-time counter included, so two saves of the same story point made at different moments differ.
// All save slots holding this exact game: 0x54..0xB7F (characters, party, items, materia, gil). Excluded:
// 0x04..0x53 (save-screen preview header, rewritten for the file copy at save time) and 0xB80+ (timers keep
// ticking in memory) - both measured to break the match. Two slots can hold the same game (saved twice in a row).
int locate_save_slots(int* keys, int cap) {
    const uint8_t* sm = reinterpret_cast<const uint8_t*>(ff7::SAVEMAP);
    return find_in_save_files(sm + 0x54, 0xB80 - 0x54, 0x54, keys, cap);
}

bool roster_file_exists(int key) {
    char p[MAX_PATH];
    sprintf_s(p, "%s\\save\\trainer\\trainer_slot%02d.json", g_game_dir, key);
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
}

void __cdecl after_write_save(uint32_t ok, const uint32_t* args) {
    if (!core_ready) return;
    // The call argument can't be trusted after the call (same file-2 save seen passing 00 and 10), and the return
    // value isn't a success flag (0 while the file was written), so the slot comes from content only.
    int keys[16];
    const int n = locate_save_slots(keys, 16);
    tlog("SAVE write_save_file(arg %02X) returned %u; this game is in %d slot(s)", args[0] & 0xFF, ok, n);
    if (n == 0) {
        tlog("SAVE could not find this game in %s; roster NOT written (autosave keeps it)", g_save_dir);
        if (api.save_slot(TR_AUTOSAVE) < 0) core_error("tr_save_slot(autosave)");
        return;
    }
    api.heal_all();  // save points double as the Pokemon Center
    for (int i = 0; i < n; i++) {  // every slot holding this game gets its roster
        const int r = api.save_slot(uint32_t(keys[i]));
        tlog("SAVE file %d slot %d: monsters healed, roster %s (party %d, box %d)", keys[i] >> 4, keys[i] & 0xF,
             r < 0 ? "WRITE FAILED" : "written", api.party_len(), api.box_len());
        if (r < 0) core_error("tr_save_slot");
    }
}

// Set by a load on the title screen; leaving the title without one is a New Game.
volatile bool g_loaded_since_title = false;

void __cdecl after_load_save(uint32_t ret, const uint32_t* args) {
    tlog("LOAD load_save_file(%08X) -> %u", args[0], ret);
    g_loaded_since_title = true;
    if (!core_ready) return;
    int keys[16];
    const int n = locate_save_slots(keys, 16);
    int key = -1;
    for (int i = 0; i < n && key < 0; i++)
        if (roster_file_exists(keys[i])) key = keys[i];
    if (key < 0 && n > 0) key = keys[0];
    if (key < 0) {
        // Never fall back to the call argument: it could be another save's slot.
        tlog("LOAD could not find the loaded game in %s; starting with an empty roster", g_save_dir);
        api.new_game();
        return;
    }
    const int r = api.load_slot(uint32_t(key));
    if (r < 0) core_error("tr_load_slot");
    else tlog("LOAD roster for file %d slot %d: party %d, box %d", key >> 4, key & 0xF, api.party_len(), api.box_len());
}

void install_late_hooks() {
    static bool done = false;
    if (done) return;
    done = true;
    patch::hook_call_after_args(ff7::CALL_WRITE_SAVE, 0, after_write_save, "write-save");
    patch::hook_call_after_args(ff7::CALL_LOAD_SAVE, 0, after_load_save, "load-save");
}

// ---- companions: in the story, never in battle --------------------------------------------------------------
// Barret, Tifa & co stay in the savemap party (field scripts check it), but are taken out right before battle init
// builds the battle party (game thread), and put back when the game returns to the field or world map.
uint8_t g_stashed[2] = {0xFF, 0xFF};
volatile bool g_stash_active = false;
// Their character records as they left: after the battle the game writes its own copy of the pre-battle party back
// and copies the battle slots' stats into those characters' records - the empty slots, i.e. 0 HP / 0 MP (KO).
// They never fought, so their records go back exactly as they were.
uint8_t g_stashed_rec[2][0x84] = {};
int g_stashed_idx[2] = {-1, -1};

int char_record_index(uint8_t id) {
    for (int i = 0; i < 9; i++)
        if (ff7::savemap_char(i)[0] == id) return i;  // record byte 0 = character id (Young Cloud, Sephiroth share)
    return id < 9 ? id : -1;
}

void stash_companions() {
    uint8_t* pm = ff7::party_members();
    if (g_stash_active || pm[0] != 0) return;  // only when Cloud leads; otherwise the story party fights
    g_stashed[0] = pm[1];
    g_stashed[1] = pm[2];
    if (pm[1] == 0xFF && pm[2] == 0xFF) return;
    for (int k = 0; k < 2; k++) {
        g_stashed_idx[k] = g_stashed[k] == 0xFF ? -1 : char_record_index(g_stashed[k]);
        if (g_stashed_idx[k] >= 0) memcpy(g_stashed_rec[k], ff7::savemap_char(g_stashed_idx[k]), 0x84);
    }
    pm[1] = pm[2] = 0xFF;
    g_stash_active = true;
    tlog("PARTY companions %u,%u stay out of this battle", g_stashed[0], g_stashed[1]);
}

void restore_companions() {
    if (!g_stash_active) return;
    g_stash_active = false;
    uint8_t* pm = ff7::party_members();
    for (int k = 0; k < 2; k++) {
        if (g_stashed_idx[k] < 0) continue;
        uint8_t* rec = ff7::savemap_char(g_stashed_idx[k]);
        const uint16_t hp = *reinterpret_cast<uint16_t*>(rec + 0x2C), was = *reinterpret_cast<uint16_t*>(g_stashed_rec[k] + 0x2C);
        memcpy(rec, g_stashed_rec[k], 0x84);
        tlog("PARTY companion %u record restored (HP %u after the battle -> %u)", g_stashed[k], hp, was);
        g_stashed_idx[k] = -1;
    }
    // The game itself may already have put the pre-battle party back. Any other party = a script changed it: it wins.
    const bool ours = pm[0] == 0 && pm[1] == 0xFF && pm[2] == 0xFF;
    const bool games = pm[0] == 0 && pm[1] == g_stashed[0] && pm[2] == g_stashed[1];
    if (!ours && !games) {
        tlog("PARTY party changed during the battle (%u,%u,%u); companions not restored", pm[0], pm[1], pm[2]);
        return;
    }
    pm[1] = g_stashed[0];
    pm[2] = g_stashed[1];
    tlog("PARTY companions %u,%u back in the field party", pm[1], pm[2]);
}

void forget_companion_stash(const char* why) {
    if (!g_stash_active) return;
    g_stash_active = false;
    g_stashed_idx[0] = g_stashed_idx[1] = -1;
    tlog("PARTY stash dropped (%s): it belonged to a game that is gone", why);
}

void __cdecl before_battle_party_build() { stash_companions(); }

// Field mode: give Cloud the Catch materia once per game (flag saved with the roster).
bool has_steal_materia_anywhere() {
    auto is_steal = [](uint32_t m) { return m != 0xFFFFFFFF && (m & 0xFF) == ff7::MATERIA_STEAL; };
    for (int c = 0; c < 9; c++)
        for (int i = 0; i < 16; i++)
            if (is_steal(ff7::char_materia(c)[i])) return true;
    for (int i = 0; i < ff7::MATERIA_INVENTORY; i++)
        if (is_steal(ff7::materia_inventory()[i])) return true;
    for (int i = 0; i < ff7::STOLEN_MATERIA; i++)
        if (is_steal(ff7::stolen_materia()[i])) return true;
    return false;
}

int free_inventory_slot() {
    for (int i = 0; i < ff7::MATERIA_INVENTORY; i++)
        if (ff7::materia_inventory()[i] == 0xFFFFFFFF) return i;
    return -1;
}

void ensure_capture_kit() {
    if (api.kit_given() == 1) return;
    if (has_steal_materia_anywhere()) {  // e.g. an older save, or the player bought one
        api.mark_kit_given();
        return;
    }
    // Equip it in place of Cloud's Ice (or another equipped weapon materia: an occupied slot is a slot the weapon
    // really has). The materia it replaces, with its AP, goes to the inventory. If that isn't possible, Catch itself
    // goes to the inventory for the player to equip.
    uint32_t* mat = ff7::char_materia(0);
    int slot = -1;
    for (int i = 0; i < 8 && slot < 0; i++)
        if (mat[i] != 0xFFFFFFFF && (mat[i] & 0xFF) == ff7::MATERIA_ICE) slot = i;
    for (int i = 0; i < 8 && slot < 0; i++)
        if (mat[i] != 0xFFFFFFFF && (mat[i] & 0xFF) != ff7::MATERIA_LIGHTNING) slot = i;
    const int inv = free_inventory_slot();
    if (slot >= 0 && inv >= 0) {
        ff7::materia_inventory()[inv] = mat[slot];
        tlog("KIT Cloud weapon slot %d: %08X -> Catch (id %u); old materia moved to inventory slot %d", slot, mat[slot],
             ff7::MATERIA_STEAL, inv);
        mat[slot] = ff7::MATERIA_STEAL;
    } else if (inv >= 0) {
        ff7::materia_inventory()[inv] = ff7::MATERIA_STEAL;
        tlog("KIT Catch materia added to inventory slot %d (no weapon slot to swap)", inv);
    } else {
        tlog("KIT no room for the Catch materia (inventory full); will retry");
        return;
    }
    api.mark_kit_given();
}

// After a successful capture: once the Steal animation has had time to play, the monster leaves the battle.
void apply_pending_capture() {
    if (!g_pending.armed || GetTickCount() - g_pending.at < 2500) return;
    g_pending.armed = false;
    ff7::ActorVars* a = ff7::actor(g_pending.actor);
    tlog("CAPTURE apply: actor[%d] %s HP %d -> 0, status %08X -> +DEATH", g_pending.actor, g_pending.name,
         a->current_hp, a->status);
    a->current_hp = 0;
    a->status |= ff7::ST_DEATH;
}

// Field opcodes 0x3C-0x3F (restore HP/MP: inns, beds, story heals) -> the monsters rest too. Game thread.
using FieldOpcode = int(__cdecl*)();
FieldOpcode g_restore_orig[4] = {};

int __cdecl restore_opcode_hook() {
    const uint8_t op = ff7::field_script()[ff7::field_script_pos()[ff7::field_entity()]];
    const FieldOpcode orig = (op >= ff7::OP_RESTORE_FIRST && op <= ff7::OP_RESTORE_LAST)
                                 ? g_restore_orig[op - ff7::OP_RESTORE_FIRST] : nullptr;
    if (!orig) return 0;
    const int r = orig();
    if (r == 0 && core_ready) {
        api.heal_all();
        tlog("REST field opcode %02X restored the party: monsters healed", op);
    }
    return r;
}

void install_restore_hooks() {
    FieldOpcode* table = reinterpret_cast<FieldOpcode*>(ff7::FIELD_OPCODE_TABLE);
    DWORD old;
    VirtualProtect(&table[ff7::OP_RESTORE_FIRST], 4 * sizeof(FieldOpcode), PAGE_READWRITE, &old);
    for (int i = 0; i < 4; i++) {
        g_restore_orig[i] = table[ff7::OP_RESTORE_FIRST + i];
        table[ff7::OP_RESTORE_FIRST + i] = restore_opcode_hook;  // aligned dword write: atomic for the game thread
    }
    VirtualProtect(&table[ff7::OP_RESTORE_FIRST], 4 * sizeof(FieldOpcode), old, &old);
    tlog("hooked field opcodes 3C-3F (restore HP/MP) -> monsters heal at inns and beds");
}

}  // namespace

// Game thread, once per battle frame: everything that writes battle memory happens here, never on the monitor thread.
// FALLEN_GRACE_MS after the monster faints: if its death was never processed (HP poked or drained to 0 outside an
// action, or the action that would have run it was dropped), mark it fallen as the recall does, or the battle keeps
// it as a living target and never ends (D13). If Cloud's command to it was dropped with it, restart his TIME bar (D12).
void settle_fallen_monster() {
    if (g_fallen_actor < 0) return;
    if (g_orphaned && ff7::cloud_atb() != 0xFFFF) {
        tlog("SENDOUT Cloud's turn came back by itself (ATB %04X)", ff7::cloud_atb());
        g_orphaned = false;
    }
    if (GetTickCount() - g_fallen_at < FALLEN_GRACE_MS) return;
    ff7::ActorVars* a = ff7::actor(g_fallen_actor);
    if (a->current_hp <= 0 && !(a->status & ff7::ST_DEATH)) {
        a->status = (a->status & ~ff7::ST_MANIPULATE) | ff7::ST_DEATH;
        tlog("SENDOUT actor[%d]'s death was not processed: marked fallen (status %08X)", g_fallen_actor, a->status);
    }
    if (g_orphaned) {
        ff7::cloud_atb() = 0;
        tlog("SENDOUT the fainted monster's action was dropped: Cloud's TIME bar restarts");
        g_orphaned = false;
    }
    g_fallen_actor = -1;
}

void __cdecl battle_tick(uint32_t) {
    static unsigned calls = 0;
    calls++;
    if (calls <= 3 || calls % 3600 == 0)
        tlog("TICK #%u battle_mode=%u mode=%u", calls, *reinterpret_cast<volatile uint32_t*>(0x9AAD64), ff7::mode());
    if (!core_ready || ff7::mode() != ff7::BATTLE) return;
    if (g_battle_start_message[0]) {
        text::show_battle_message(g_battle_start_message);
        g_battle_start_message[0] = 0;
    }
    apply_pending_capture();
    apply_send_out();
    face_like_cloud();
    aim_at_wild_enemies();
    puppet_watch();
    settle_fallen_monster();
    text::pump_battle_messages();
}

void toggle_trainer_turns() {
    if (ff7::mode() != ff7::BATTLE || !g_puppet.live) return;
    g_trainer_turns = !g_trainer_turns;
    tlog("F8 %s", g_trainer_turns ? "trainer turns: Cloud acts himself (Catch, items)" : "monster turns: Cloud commands it");
}

bool trainer_turns() { return g_trainer_turns; }

void install_hooks() {
    int ok = 0, total = 0;
    total++; ok += patch::hook_call_after(ff7::CALL_LOAD_SCENE_A, ff7::FN_LOAD_SCENE, after_scene_loaded, "scene-load A");
    total++; ok += patch::hook_call_after(ff7::CALL_LOAD_SCENE_B, ff7::FN_LOAD_SCENE, after_scene_loaded, "scene-load B");
    static const uintptr_t dispatch_sites[] = {0x6D8A29, 0x6D8A68, 0x6DA823, 0x6DA926, 0x6DB5FC};
    for (uintptr_t site : dispatch_sites) {
        total++;
        ok += patch::hook_call_before(site, ff7::FN_DISPATCH_ACTION, before_dispatch, "dispatch-action");
    }
    total++; ok += patch::hook_call_after(ff7::CALL_BATTLE_END_MENU, ff7::FN_BATTLE_END_MENU, after_battle_end_menu, "battle-end-menu");
    total++; ok += patch::hook_call_before_args(ff7::CALL_ADD_TEXT, ff7::FN_ADD_TEXT, before_add_text, "battle-text");
    total++; ok += patch::hook_call_after(ff7::CALL_BATTLE_MENU_UPDATE, ff7::FN_BATTLE_MENU_UPDATE, battle_tick, "battle-tick");
    total++; ok += patch::hook_call_after(0x42A6C3, 0x42AE73, after_homes_set, "party-homes");
    total++; ok += patch::hook_call_after(0x429C50, 0x42B8D2, after_homes_set, "enemy-homes A");
    total++; ok += patch::hook_call_after(0x429F9B, 0x42B8D2, after_homes_set, "enemy-homes B");
    total++; ok += patch::hook_call_before(ff7::CALL_BATTLE_PARTY_BUILD, ff7::FN_BATTLE_PARTY_BUILD, before_battle_party_build, "battle-party-build");
    install_restore_hooks();
    monsters::install();
    tlog("hooks: %d of %d installed", ok, total);
}

void on_engine_ready(const char* game_dir) {
    static bool once = false;
    if (once) return;
    once = true;
    strcpy_s(g_game_dir, game_dir);
    resolve_save_dir();
    char path[MAX_PATH], species[MAX_PATH], saves[MAX_PATH];
    sprintf_s(path, "%s\\trainer\\trainer_core.dll", game_dir);
    sprintf_s(species, "%s\\trainer\\species.json", game_dir);
    sprintf_s(saves, "%s\\save\\trainer", game_dir);
    g_core = LoadLibraryA(path);
    if (!g_core) {
        tlog("core: LoadLibrary(%s) failed, error %lu", path, GetLastError());
        return;
    }
    bool all = bind(api.version, "tr_version") & bind(api.init, "tr_init") & bind(api.last_error, "tr_last_error") &
               bind(api.new_game, "tr_new_game") & bind(api.load_slot, "tr_load_slot") &
               bind(api.save_slot, "tr_save_slot") & bind(api.capture_chance, "tr_capture_chance") &
               bind(api.try_capture, "tr_try_capture") & bind(api.party_len, "tr_party_len") &
               bind(api.box_len, "tr_box_len") & bind(api.party_get, "tr_party_get") &
               bind(api.box_get, "tr_box_get") & bind(api.active, "tr_active") &
               bind(api.set_vitals, "tr_set_vitals") & bind(api.award_xp, "tr_award_xp") &
               bind(api.heal_all, "tr_heal_all") & bind(api.swap, "tr_swap") & bind(api.deposit, "tr_deposit") &
               bind(api.withdraw, "tr_withdraw") & bind(api.all_ko, "tr_all_ko") &
               bind(api.set_legendary_unlocked, "tr_set_legendary_unlocked") &
               bind(api.move_record, "tr_move_record") & bind(api.species_name, "tr_species_name") &
               bind(api.species_record, "tr_species_record") & bind(api.kit_given, "tr_kit_given") &
               bind(api.mark_kit_given, "tr_mark_kit_given") & bind(api.monster_detail, "tr_monster_detail") &
               bind(api.equip_move, "tr_equip_move");
    if (!all) return;
    if (api.version() != TR_VERSION) {
        tlog("core: version %u, hook expects %u", api.version(), TR_VERSION);
        return;
    }
    int n = api.init(species, saves, GetTickCount());
    if (n < 0) {
        core_error("tr_init");
        return;
    }
    core_ready = true;
    tlog("core: ready, %d species from %s", n, species);
    hud::start();
}

void monitor_loop() {
    // FFNx resolves the game's functions at start-up by READING call instructions (get_relative_call), including the
    // very call sites we redirect. Patching before that poisons FFNx's tables (it then derives pointers from bytes
    // inside our stubs and patches into them). So code hooks go in only once the game loop is running (first
    // non-zero mode = credits), when FFNx's resolution is finished and none of the hooked battle code is executing.
    while (ff7::mode() == 0) Sleep(5);
    install_hooks();
    install_late_hooks();  // save/load too: a Continue from the title screen loads before the first field frame
    uint16_t last_mode = 0xFFFF;
    int32_t hp[10] = {};
    uint8_t last_cmd = 0xFF, last_act = 0xFF;
    for (;;) {
        Sleep(16);
        uint16_t m = ff7::mode();
        if (core_ready && (m == ff7::FIELD || m == ff7::WORLDMAP)) monsters::keep_entry_open();
        if (m != last_mode) {
            tlog("MODE %u (%s) -> %u (%s)", last_mode, ff7::mode_name(last_mode), m, ff7::mode_name(m));
            if (last_mode == ff7::BATTLE && g_puppet.live) {
                // Battle ended with the monster still out (escape, scripted end): keep its HP/MP.
                const ff7::ActorVars* a = ff7::actor(g_puppet.actor);
                api.set_vitals(g_puppet.uid, uint16_t(a->current_hp > 0 ? a->current_hp : 0), a->current_mp);
                tlog("SENDOUT battle ended with %s out: HP %d/%d kept", g_puppet.name, a->current_hp, a->max_hp);
                g_puppet.live = false;
            }
            if (m == ff7::FIELD || m == ff7::WORLDMAP) restore_companions();
            if (m == ff7::GAMEOVER || m == ff7::MAIN_MENU || m == ff7::INTRO) forget_companion_stash(ff7::mode_name(m));
            if (m == ff7::MAIN_MENU) g_loaded_since_title = false;
            // Leaving the title screen: Continue went through load_save_file (game thread, before the mode changes);
            // anything else is a New Game, whether or not the intro mode (22) was caught by this poll.
            if (last_mode == ff7::MAIN_MENU && m != ff7::MAIN_MENU && core_ready && !g_loaded_since_title) {
                api.new_game();
                tlog("NEWGAME roster cleared (title -> %s, no save loaded)", ff7::mode_name(m));
            }
            last_mode = m;
            for (int32_t& h : hp) h = -1;
        }
        if (m == ff7::FIELD && core_ready) {
            ensure_capture_kit();
            install_late_hooks();
            text::rename_capture_kit();
        }
        if (m != ff7::BATTLE) continue;
        for (int i = 0; i < 10; i++) {
            if (i == 3) continue;
            const ff7::ActorVars* a = ff7::actor(i);
            if (a->max_hp <= 0 || a->current_hp == hp[i]) continue;
            if (hp[i] == -1) {
                tlog("  actor[%d] %s id=%u L%u HP %d/%d MP %u/%u", i, i < 3 ? "party" : "enemy",
                     i < 3 ? a->index : a->formation_id, a->level, a->current_hp, a->max_hp, a->current_mp, a->max_mp);
            } else {
                tlog("  actor[%d] HP %d -> %d", i, hp[i], a->current_hp);
            }
            hp[i] = a->current_hp;
        }
        if (ff7::last_command() != last_cmd || ff7::last_action() != last_act) {
            last_cmd = ff7::last_command();
            last_act = ff7::last_action();
            tlog("  last action: actor=%u command=%u action=%u", ff7::active_actor_id(), last_cmd, last_act);
        }
    }
}

}  // namespace trainer
