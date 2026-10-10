"""dex.json (raw enemy data) -> species.json (what trainer_core loads).

    python tools/build_species.py out/dex.json out/species.json

Design decisions, all applied here so the Rust core stays data-driven:
- Real moves only: drop blank names, no-target records and scripting verbs; dedupe by name.
- Starting moves = the enemy's Manipulate list (Square's own player-usable pick), else its first 2 real moves.
  The rest are learned at levels spread from the native level to native + 30.
- Types = elements the species absorbs, then nullifies (not gravity/hidden), at most 2; none = "normal". The full elemental chart is kept and is
  what the battle engine applies.
- Stats are expressed as factors against a player reference curve, fitted to Cloud's (HP 302 at Lv6, ~9000 at
  Lv99), so captured monsters scale like party members instead of carrying raw enemy numbers (Ruby Weapon has
  800 000 HP).
- Capture rate falls with native level; bosses are 0 until a post-game rematch flag lifts it.
- Evolution: species sharing a rig (identical skeleton + animation pack) form a line, ordered by native level.
"""
import json
import math
import sys
from collections import defaultdict
from pathlib import Path

SCRIPT_VERBS = {"vanish", "escape", "(report)", "appear", "retreat", "dodge", "destroyed", "change", "exchange",
                "transform", "move", "move right", "move left", "return"}
# Immunity to these is common on big enemies and says nothing about what the creature is.
NOT_A_TYPE = {"gravity", "hidden"}
STATS = ("str", "def_", "mag", "mdef", "spd", "lck", "eva")


def hp_ref(level: int) -> float:
    return 30 + 45 * level + 0.45 * level * level


def mp_ref(level: int) -> float:
    return 10 + 7 * level + 0.03 * level * level


def stat_ref(level: int) -> float:
    return 12 + 0.9 * level


def factor(value: float, ref: float, lo=0.6, hi=1.6) -> float:
    if value <= 0:
        return lo
    return round(min(hi, max(lo, math.sqrt(value / ref))), 3)


# Attack target flags (scene.bin): 0x01 selectable, 0x02 defaults to the enemy side, 0x04 multiple by default,
# 0x08 single/multi toggle.
def battle_usable(a: dict) -> bool:
    """A move a sent-out monster can use safely: it aims at the (from the monster's view) enemy side and can be single.
    Our party is the monster's 'enemy side' in the engine and the dispatch hook retargets single targets onto the wild
    side; multi-target-only moves and ally moves (heals) would land on the wrong side."""
    f = a.get("target_flags", 0)
    # Kernel spells (ids < 256: Fire, Ice2, Bolt3 ...) cast by an enemy-type actor fighting on the party side deal
    # their damage and then never finish (battle soft-lock, measured even in the monster's own scene). The monster's
    # own skills (ids >= 256) play through its model and effects and are fine (Beam Gun, Drilldrive, Bodyblow).
    if a.get("id", 0) < 256:
        return False
    return bool(f & 0x02) and not (f & 0x04 and not f & 0x08)


# Moves that never finish when a sent-out monster uses them (battle_mode 8 forever), found by the lab sweep that
# forces every move of every species once (lab/sweep_moves.py, 2026-10-09): {species id: {move ids}}.
# Grosspanzer-Mobile's model hangs on all of its moves; Suicide Yang downs its user mid-action; Eye's Emerald Laser.
# A species left with no moves is not catchable.
HANGS = {15: {989}, 152: {576}, 293: {301, 424, 742, 793}}


def real_moves(moves: list[dict]) -> list[dict]:
    out, seen = [], set()
    for a in moves:
        name = a.get("name", "")
        if not name or not any(c.isalpha() for c in name):
            continue
        if name.lower() in SCRIPT_VERBS or a.get("target_flags", 0) == 0:
            continue
        if name in seen:
            continue
        seen.add(name)
        out.append(a)
    return out


def main(dex_path: str, out_path: str):
    dex = json.loads(Path(dex_path).read_text())
    species = []
    for m in dex:
        moves = [a for a in real_moves(m["moves"]) if battle_usable(a) and a["id"] not in HANGS.get(m["id"], ())]
        if not moves:
            continue
        lvl = max(1, m["level"])
        by_name = {a["name"]: a for a in moves}
        start = [by_name[n] for n in m["manipulate_moves"] if n in by_name][:3] or moves[:2]
        rest = [a for a in moves if a not in start]
        learn = [dict(level=lvl, move=a["id"]) for a in start]
        for i, a in enumerate(rest):
            learn.append(dict(level=min(99, lvl + round(30 * (i + 1) / len(rest))), move=a["id"]))
        aff = m["affinities"]
        types = []
        for e in aff.get("absorb", []) + aff.get("null", []):
            if e not in NOT_A_TYPE and e not in types:
                types.append(e)
        types = types[:2] or ["normal"]
        st = m["stats"]
        species.append(dict(
            id=m["id"], name=m["name"], native_level=lvl, boss=m["boss"],
            types=types, affinities=aff, status_immunity_mask=m["status_immunity_mask"],
            factors=dict(hp=factor(st["hp"], hp_ref(lvl), 0.6, 1.8), mp=factor(st["mp"], mp_ref(lvl), 0.5, 1.8),
                         **{k: factor(st[k], stat_ref(lvl)) for k in STATS}),
            exp_yield=m["exp"], ap_yield=m["ap"], gil=m["gil"],
            capture_rate=0.0 if m["boss"] else round(max(0.12, min(0.9, 1.0 - lvl / 110)), 3),
            enemy_record=m["record"],
            model=m["model"]["id"] if m.get("model") else None, rig=m["model"]["rig"] if m.get("model") else None,
            moves={str(a["id"]): dict(name=a["name"], mp=a["mp"], power=a["power"], elements=a["elements"],
                                      raw=a["raw"]) for a in moves},
            learnset=learn,
        ))
    lines = defaultdict(list)
    for s in species:
        if s["rig"]:
            lines[s["rig"]].append(s)
    for members in lines.values():
        members.sort(key=lambda s: (s["native_level"], s["id"]))
        names = {s["name"] for s in members}
        for i, s in enumerate(members):
            nxt = next((t for t in members[i + 1:] if t["name"] != s["name"] and len(names) > 1), None)
            s["evolves_to"] = dict(species=nxt["id"], level=nxt["native_level"]) if nxt and not nxt["boss"] else None
    Path(out_path).write_text(json.dumps(dict(version=1, species=species), indent=1))

    evo = sum(1 for s in species if s.get("evolves_to"))
    print(f"species: {len(species)} (bosses {sum(s['boss'] for s in species)}), with evolutions: {evo}")
    print(f"moves per species: {sorted(len(s['moves']) for s in species)[len(species) // 2]} median")
    tc = defaultdict(int)
    for s in species:
        for t in s["types"]:
            tc[t] += 1
    print("types:", dict(sorted(tc.items(), key=lambda kv: -kv[1])))
    for n in ("Mono Drive", "Guard Hound", "Bomb", "Cactuar"):
        s = next(x for x in species if x["name"] == n)
        print(f"  {n}: types {s['types']} capture {s['capture_rate']} factors {s['factors']} "
              f"learnset {[(l['level'], s['moves'][str(l['move'])]['name']) for l in s['learnset']]} "
              f"evolves {s.get('evolves_to')}")


if __name__ == "__main__":
    main(*sys.argv[1:])
