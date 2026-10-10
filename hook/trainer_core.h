// trainer_core C ABI (core/src/ffi.rs). x86 cdecl. Keep in lockstep with ffi.rs: the static_asserts below and the
// const asserts there must agree, or the build fails.
#pragma once
#include <stdint.h>

#define TR_VERSION 1
#define TR_E_NOT_INIT -1
#define TR_E_BAD_ARG -2
#define TR_E_IO -3
#define TR_E_NOT_FOUND -4

// tr_try_capture results
#define TR_CAUGHT 1
#define TR_CAUGHT_TO_BOX 2
#define TR_FAILED 0
#define TR_NOT_CAPTURABLE 3

#pragma pack(push, 1)
typedef struct TrMonsterView {
    uint32_t uid;
    uint16_t species;
    uint8_t level;
    uint8_t ko;
    uint16_t hp, hp_max, mp, mp_max;
    uint8_t stats[7];  // str, def, mag, mdef, spd, lck, eva
    uint8_t pad;
    uint8_t element_rates[16];  // FF7 rate codes: 0xFF none, 2 weak, 4 half, 5 null, 6 absorb, 0 death, 7 full cure
    uint32_t status_immunity_mask;
    uint16_t moves[4];  // FF7 attack ids, 0xFFFF = empty
    char model[2];      // battle model id ("as" = Mono Drive), 0 = none
    uint8_t pad2[2];
    char name[32];      // UTF-8, NUL-terminated
} TrMonsterView;

typedef struct TrMoveRecord {
    uint8_t record[28];  // FF7 attack record, as in scene.bin
    uint16_t id;
    uint16_t pad;
    uint8_t ff_name[32];  // FF7 text encoding, 0xFF-terminated
} TrMoveRecord;

typedef struct TrGrowth {
    uint32_t uid;
    uint8_t levels_gained, learned_count, evolved, pad;
    uint16_t evolved_into;
    uint16_t learned[3];  // the first 3 of learned_count (which counts every move learned)
} TrGrowth;

typedef struct TrMoveLine {
    uint16_t id, mp;
    uint8_t power, pad[3];
    char name[24];     // UTF-8
    char element[24];  // "Fire", "Bolt/Wind", "" = none
} TrMoveLine;

typedef struct TrMonsterDetail {
    TrMonsterView view;
    uint32_t xp, xp_next;  // xp_next: total XP for the next level, 0 at the cap
    uint8_t caught_level, next_learn_level, evolve_level, equipped_count, known_count, pad[3];
    char species_name[24];
    char types[32];   // "Earth/Water"
    char weak[48];    // "Fire, Ice", "" = none
    char next_learn[24];
    char evolve_into[24];
    TrMoveLine moves[12];  // equipped first (menu order), then the other known moves
} TrMonsterDetail;
#pragma pack(pop)

static_assert(sizeof(TrMonsterView) == 88, "TrMonsterView layout drifted from ffi.rs");
static_assert(sizeof(TrMoveRecord) == 64, "TrMoveRecord layout drifted from ffi.rs");
static_assert(sizeof(TrGrowth) == 16, "TrGrowth layout drifted from ffi.rs");
static_assert(sizeof(TrMoveLine) == 56, "TrMoveLine layout drifted from ffi.rs");
static_assert(sizeof(TrMonsterDetail) == 88 + 8 + 8 + 24 + 32 + 48 + 24 + 24 + 12 * 56, "TrMonsterDetail layout drifted");

// Function table, filled by LoadLibrary/GetProcAddress so trainer_core.dll can be reloaded (Ctrl+T)
// without restarting the game.
typedef struct TrApi {
    uint32_t (*version)(void);
    int32_t (*init)(const char* species_json, const char* save_dir, uint32_t seed);
    uint32_t (*last_error)(uint8_t* buf, uint32_t len);
    int32_t (*new_game)(void);
    int32_t (*load_slot)(uint32_t slot);
    int32_t (*save_slot)(uint32_t slot);
    float (*capture_chance)(uint16_t species, uint32_t hp, uint32_t hp_max, uint32_t status_bonus_x100, uint32_t tier);
    int32_t (*try_capture)(uint16_t species, uint8_t level, uint32_t hp, uint32_t hp_max, uint32_t status_bonus_x100,
                           uint32_t tier, uint16_t scene);
    int32_t (*party_len)(void);
    int32_t (*box_len)(void);
    int32_t (*party_get)(uint32_t index, TrMonsterView* out);
    int32_t (*box_get)(uint32_t index, TrMonsterView* out);
    int32_t (*active)(int32_t* out2);
    int32_t (*set_vitals)(uint32_t uid, uint16_t hp, uint16_t mp);
    int32_t (*award_xp)(uint32_t xp, TrGrowth* out, uint32_t cap);
    int32_t (*heal_all)(void);
    int32_t (*swap)(uint32_t a, uint32_t b);
    int32_t (*deposit)(uint32_t index);
    int32_t (*withdraw)(uint32_t box_index);
    int32_t (*all_ko)(void);
    int32_t (*set_legendary_unlocked)(uint32_t on);
    int32_t (*move_record)(uint16_t move_id, TrMoveRecord* out);
    int32_t (*species_name)(uint16_t species, uint8_t* buf, uint32_t len);
    int32_t (*species_record)(uint16_t species, uint8_t* out184);
    int32_t (*kit_given)(void);
    int32_t (*mark_kit_given)(void);
    int32_t (*monster_detail)(uint32_t in_box, uint32_t index, TrMonsterDetail* out);
    int32_t (*equip_move)(uint32_t in_box, uint32_t index, uint32_t slot, uint16_t move_id);
} TrApi;

#define TR_AUTOSAVE 0xFFFFu  // sidecar key for the between-saves autosave (real keys are file << 4 | slot)
