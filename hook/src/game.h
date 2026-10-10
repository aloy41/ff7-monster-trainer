// FF7 1.02 US (classic ff7_en.exe as shipped in the 2026 Steam re-release) addresses and layouts.
// Sources: FFNx's resolution chains (ff7_data.h) reproduced offline against this exe, and our own
// disassembly of the scene loader. Every call site we patch is byte-checked at install time (patch.h), so a
// different exe build fails loudly instead of corrupting memory.
#pragma once
#include <stdint.h>

namespace ff7 {

// --- game mode -----------------------------------------------------------------------------------------
inline volatile uint16_t& mode() { return *reinterpret_cast<volatile uint16_t*>(0xCBF9DC); }
enum Mode : uint16_t {
    FIELD = 1, BATTLE = 2, WORLDMAP = 3, MENU = 5, BATTLE_MENU = 17, MAIN_MENU = 20, INTRO = 22, SWIRL = 23,
    GAMEOVER = 26, CREDITS = 27,
};
const char* mode_name(uint16_t m);

// --- battle: scene globals filled by the scene loader 0x5D1050 -----------------------------------------
constexpr uintptr_t FN_LOAD_SCENE = 0x5D1050;
constexpr uintptr_t CALL_LOAD_SCENE_A = 0x437F0D;  // both in battle init 0x437DB0
constexpr uintptr_t CALL_LOAD_SCENE_B = 0x4382BE;

inline volatile uint16_t& formation_id() { return *reinterpret_cast<volatile uint16_t*>(0x9AB0C8); }
inline uint16_t* scene_enemy_ids() { return reinterpret_cast<uint16_t*>(0x9A8748); }  // 3 x u16
constexpr uintptr_t SCENE_SETUP = 0x9A8750;          // 0x14, first u16 = location
constexpr uintptr_t SCENE_PLACEMENT = 0x9A8794;      // 6 x 16
constexpr uintptr_t SCENE_ENEMY_RECORDS = 0x9A8E9C;  // 3 x 0xB8
constexpr uintptr_t SCENE_ATTACKS = 0x9A90C4;        // 32 x 0x1C
inline uint16_t* scene_attack_ids() { return reinterpret_cast<uint16_t*>(0x9A9444); }  // 32 x u16
constexpr uintptr_t SCENE_ATTACK_NAMES = 0x9A9484;   // 32 x 32
constexpr uintptr_t SCENE_ENEMY_AI = 0x9A9CFC;       // 3 x u16 offsets (0xFFFF = no script), then scripts
inline uint16_t* scene_ai_offsets() { return reinterpret_cast<uint16_t*>(SCENE_ENEMY_AI); }
constexpr int SCENE_ATTACK_SLOTS = 32;

// Enemy record (scene.bin layout, 0xB8 bytes).
#pragma pack(push, 1)
struct EnemyRecord {
    uint8_t name[32];
    uint8_t level, speed, luck, evade, strength, defense, magic, mdef;
    uint8_t element_types[8];
    uint8_t element_rates[8];
    uint8_t anim_index[16];
    uint16_t attack_ids[16];
    uint16_t attack_cams[16];
    uint8_t item_rates[4];
    uint16_t item_ids[4];
    uint16_t manip_ids[3];
    uint16_t unk_9a;
    uint16_t mp, ap, morph;
    uint8_t back_mult, align;
    uint32_t hp, exp, gil, status_immunity;
    uint8_t unk_b4[4];
};
static_assert(sizeof(EnemyRecord) == 0xB8, "EnemyRecord");
#pragma pack(pop)
inline EnemyRecord* scene_enemy(int slot) { return reinterpret_cast<EnemyRecord*>(SCENE_ENEMY_RECORDS) + slot; }

// --- battle: live actors ---------------------------------------------------------------------------------
// battle_ai_context at 0x9AB0A0; actor_vars[10] at +0x3C, stride 0x68. Actors 0-2 party, 3 unused, 4-9 enemies.
constexpr uintptr_t BATTLE_CONTEXT = 0x9AB0A0;
#pragma pack(push, 1)
struct ActorVars {
    uint32_t status;          // 0x00
    uint32_t state_flags;     // 0x04
    uint8_t index;            // 0x08 party: character id
    uint8_t level;            // 0x09
    uint8_t unk_0a;
    uint8_t element_damage;   // 0x0B
    uint8_t character_id;     // 0x0C
    uint8_t phys_atk;         // 0x0D
    uint8_t mag_atk;          // 0x0E
    uint8_t p_evade;          // 0x0F
    uint8_t idle_anim, damage_anim, back_mult, size_scale;  // 0x10..0x13
    uint8_t dexterity;        // 0x14
    uint8_t luck;             // 0x15
    uint8_t idle_holder, last_covered;  // 0x16, 0x17
    uint16_t last_targets;    // 0x18
    uint16_t prev_attacker, phys_attacker, mag_attacker;  // 0x1A..0x1F
    uint16_t defense;         // 0x20
    uint16_t m_defense;       // 0x22
    uint16_t formation_id;    // 0x24 enemies: enemy id
    uint16_t absorbed_elements;  // 0x26
    uint16_t current_mp;      // 0x28
    uint16_t max_mp;          // 0x2A
    int32_t current_hp;       // 0x2C
    int32_t max_hp;           // 0x30
    uint8_t unk_34[0x10];     // 0x34..0x43
    uint32_t initial_status;  // 0x44
    uint8_t unk_48[5];        // 0x48..0x4C
    uint8_t m_evade;          // 0x4D
    uint8_t row;              // 0x4E
    uint8_t camera;           // 0x4F
    uint16_t gil_stolen;      // 0x50
    uint16_t item_stolen;     // 0x52
    uint8_t unk_54[2];
    uint8_t miss_anim;        // 0x56
    uint8_t ap_value;         // 0x57
    uint32_t gil_value;       // 0x58
    uint32_t exp_value;       // 0x5C
    uint8_t unused_60[8];     // 0x60..0x67
};
static_assert(sizeof(ActorVars) == 0x68, "ActorVars");
#pragma pack(pop)
inline ActorVars* actor(int i) { return reinterpret_cast<ActorVars*>(BATTLE_CONTEXT + 0x3C) + i; }
// Side masks in battle_context (bit per actor). Moving our monster's bit from the enemy side to the party side makes
// wild AI target it (verified: Mono Drive took hits) while it stays a manipulated enemy-type actor.
inline volatile uint16_t& party_actors_mask() { return *reinterpret_cast<volatile uint16_t*>(BATTLE_CONTEXT + 0x18); }
inline volatile uint16_t& enemy_actors_mask() { return *reinterpret_cast<volatile uint16_t*>(BATTLE_CONTEXT + 0x1A); }
inline volatile uint8_t& last_command() { return *reinterpret_cast<volatile uint8_t*>(BATTLE_CONTEXT + 0x00); }
inline volatile uint8_t& last_action() { return *reinterpret_cast<volatile uint8_t*>(BATTLE_CONTEXT + 0x01); }
inline volatile uint16_t& active_actor_id() { return *reinterpret_cast<volatile uint16_t*>(0xBE1170); }

// Battle tick: battle_main_loop 0x41BAB3 calls the battle menu update 0x6CE8B3 from 0x41BE1B every battle frame.
// Our per-frame battle work runs right after it, on the game thread. Measured dead ends: 0x41C4E0 (inside
// battle_loop, one battle state only) and 0x41BC7B (battle_loop itself: 3 calls at battle start, then never).
constexpr uintptr_t FN_BATTLE_MENU_UPDATE = 0x6CE8B3;
constexpr uintptr_t CALL_BATTLE_MENU_UPDATE = 0x41BE1B;

// Battle messages: the event processor 0x42CBF9 shows a queued text event with
// add_text_to_display_queue(u16 text_index, u8 wait, u8 frames, u16 x) = 0x5BDFB6 (FFNx replaces its body), called
// only from 0x42CDC0. text_index is a kernel2 battle-text entry (47 "Stole ...", 48 "Couldn't steal anything.").
constexpr uintptr_t FN_ADD_TEXT = 0x5BDFB6;
constexpr uintptr_t CALL_ADD_TEXT = 0x42CDC0;
constexpr uint16_t TEXT_STOLE = 47, TEXT_COULDNT_STEAL = 48;

// Player menu: the chosen action, read by dispatch_chosen_battle_action 0x6D86D2.
constexpr uintptr_t FN_DISPATCH_ACTION = 0x6D86D2;
inline volatile uint8_t& issued_command() { return *reinterpret_cast<volatile uint8_t*>(0xDC3C70); }
inline volatile uint16_t& issued_action() { return *reinterpret_cast<volatile uint16_t*>(0xDC3C78); }
inline volatile uint8_t& issued_target_type() { return *reinterpret_cast<volatile uint8_t*>(0xDC3C90); }
inline volatile uint8_t& issued_target_index() { return *reinterpret_cast<volatile uint8_t*>(0xDC3C94); }

// Battle end: spoils menu 0x6C9543 called from 0x6CDAA3; mode word 0xDC1300 (0 = won, 1 = exp/levels, 3 = gil).
constexpr uintptr_t FN_BATTLE_END_MENU = 0x6C9543;
constexpr uintptr_t CALL_BATTLE_END_MENU = 0x6CDAA3;
inline volatile uint16_t& battle_end_mode() { return *reinterpret_cast<volatile uint16_t*>(0xDC1300); }

// Formation placement (scene.bin layout), 6 per formation, copied for the chosen formation to SCENE_PLACEMENT.
// Placement k becomes enemy actor 4 + k.
#pragma pack(push, 1)
struct Placement {
    uint16_t enemy_id;  // 0xFFFF = empty
    int16_t x, y, z;
    uint16_t row, cover;
    uint32_t flags;
};
static_assert(sizeof(Placement) == 16, "Placement");
#pragma pack(pop)
inline Placement* placement(int k) { return reinterpret_cast<Placement*>(SCENE_PLACEMENT) + k; }

// Cloud's battle party record (party slot 0): byte = the enemy actor he is manipulating (0 = none). Found by diffing a
// real Manipulate; writing it + the target's MANIPULATE status hands that actor's turns to Cloud's menu.
inline volatile uint8_t& cloud_manip_target() { return *reinterpret_cast<volatile uint8_t*>(0x9A8DBE); }

// Cloud's ATB (u16, 0xFFFF = full; measured by scanning battle memory). It stays full from his command until that
// action has played out, then restarts from 0.
inline volatile uint16_t& cloud_atb() { return *reinterpret_cast<volatile uint16_t*>(0x9A8B14); }

// The character whose battle menu is open (FFNx: menu_objects 0xDC0FC0 + 0x2CBC), 0-2 = party slot.
inline volatile uint8_t& menu_party_slot() { return *reinterpret_cast<volatile uint8_t*>(0xDC0FC0 + 0x2CBC); }

// Commands (kernel section 0) and statuses we use.
enum Command : uint8_t { CMD_ATTACK = 1, CMD_MAGIC = 2, CMD_ITEM = 4, CMD_STEAL = 5, CMD_MORPH = 9, CMD_MANIP = 11,
                         CMD_CHANGE = 18, CMD_DEFEND = 19, CMD_ENEMY_ATTACK = 32 };
enum Status : uint32_t { ST_DEATH = 1u << 0, ST_SLEEP = 1u << 2, ST_STOP = 1u << 10, ST_PETRIFY = 1u << 14,
                         ST_MANIPULATE = 1u << 22, ST_PARALYSIS = 1u << 25 };

// Save / load (FFNx resolution for 1.02 US). FFNx itself hooks the save call on the Steam edition, so ours is installed
// late (first field frame) and chains onto whatever is there.
constexpr uintptr_t CALL_WRITE_SAVE = 0x6FEDB0 + 0x1096;  // BOOL write_save_file(char slot): file = slot >> 4
constexpr uintptr_t CALL_LOAD_SAVE = 0x7212FB + 0xE9D;    // int load_save_file(int)

// Battle init 0x437DB0 builds the battle party from the savemap with sub_5CB2CC, called from 0x437DF3 (game thread).
// Companions are taken out right before that call.
constexpr uintptr_t FN_BATTLE_PARTY_BUILD = 0x5CB2CC;
constexpr uintptr_t CALL_BATTLE_PARTY_BUILD = 0x437DF3;

// Field opcode table (writable data) and the four "restore HP/MP" opcodes inns and beds use
// (HMPMAX1 0x3C, HMPMAX2 0x3D, MHMMX 0x3E, HMPMAX3 0x3F; ids from Makou Reactor's opcode list).
constexpr uintptr_t FIELD_OPCODE_TABLE = 0x9055A0;
constexpr uint8_t OP_RESTORE_FIRST = 0x3C, OP_RESTORE_LAST = 0x3F;
inline uint8_t* field_script() { return *reinterpret_cast<uint8_t**>(0xCBF5E8); }
inline uint16_t* field_script_pos() { return reinterpret_cast<uint16_t*>(0xCC0CF8); }
inline uint8_t field_entity() { return *reinterpret_cast<volatile uint8_t*>(0xCC0964); }

// --- savemap ---------------------------------------------------------------------------------------------
constexpr uintptr_t SAVEMAP = 0xDBFD38;
inline uint8_t* party_members() { return reinterpret_cast<uint8_t*>(SAVEMAP + 0x4F8); }  // 3 x char id, 0xFF empty
inline volatile uint32_t& gil() { return *reinterpret_cast<volatile uint32_t*>(SAVEMAP + 0xB7C); }
// savemap_char (0x84 bytes) x 9 at +0x54; materia: 16 x u32 at +0x40 (8 weapon, 8 armor), id | AP << 8, ~0 = empty.
inline uint8_t* savemap_char(int i) { return reinterpret_cast<uint8_t*>(SAVEMAP + 0x54 + i * 0x84); }
inline uint32_t* char_materia(int i) { return reinterpret_cast<uint32_t*>(savemap_char(i) + 0x40); }
constexpr uint8_t MATERIA_STEAL = 36, MATERIA_ICE = 50, MATERIA_LIGHTNING = 52;
inline uint32_t* materia_inventory() { return reinterpret_cast<uint32_t*>(SAVEMAP + 0x77C); }  // 200 x u32
constexpr int MATERIA_INVENTORY = 200;
inline uint32_t* stolen_materia() { return reinterpret_cast<uint32_t*>(SAVEMAP + 0xA9C); }  // 48 x u32 (Yuffie, Wutai)
constexpr int STOLEN_MATERIA = 48;
// savemap_char fields (u16): hp +0x2C, mp +0x30, max_hp +0x38, max_mp +0x3A
inline uint16_t char_u16(int i, int off) { return *reinterpret_cast<volatile uint16_t*>(savemap_char(i) + off); }

// FF7 text (char - 0x20, 0xFF end) -> ASCII.
void ff_text(const uint8_t* src, int max, char* out);

}  // namespace ff7
