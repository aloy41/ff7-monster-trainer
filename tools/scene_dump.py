"""Decode FF7 PC scene.bin into a monster list (the "dex").

Reads the user's own install; writes derived data to out/ (gitignored, never commit it).

    python tools/scene_dump.py "<FF7 install>/ff7/workingdir/data/lang-en/battle/scene.bin"

Layout (Qhimm wiki, FF7/Battle/Battle_Scenes):
  scene.bin = 0x2000-byte blocks; each block starts with 16 u32 pointers (x4 bytes)
  to gzip streams; 0xFFFFFFFF = unused. Each stream inflates to one 7808-byte scene:
    0x0000 enemy ids        3 x u16
    0x0008 battle setup     4 x 20
    0x0058 camera setup     4 x 48
    0x0118 formations       4 x 6 x 16
    0x0298 enemy records    3 x 184
    0x04C0 attack records  32 x 28
    0x0840 attack ids      32 x u16
    0x0880 attack names    32 x 32
    0x0C80 formation AI
    0x0E80 enemy AI        3 x u16 offsets, then scripts
"""
import gzip
import json
import struct
import sys
import zlib
from collections import Counter
from pathlib import Path

BLOCK = 0x2000
SCENE_SIZE = 7808
ELEMENTS = ["fire", "ice", "bolt", "earth", "poison", "gravity", "water", "wind",
            "holy", "restorative", "cut", "hit", "punch", "shoot", "shout", "hidden"]
ELEMENT_RATE = {0x00: "death", 0x02: "weak", 0x04: "half", 0x05: "null", 0x06: "absorb", 0x07: "full-cure"}
ATTACK_SCRIPT_SLOTS = ["init", "main", "counter_general", "counter_death", "counter_physical",
                       "counter_magical", "battle_end", "pre_action", *[f"custom_{i}" for i in range(1, 9)]]


def ff_text(raw: bytes) -> str:
    out = []
    for b in raw:
        if b == 0xFF:
            break
        # 0xC1 is the game's middle dot (Grosspanzer·Small); "-" keeps names plain ASCII for the HUD and messages.
        out.append(chr(b + 0x20) if b < 0x60 else "-" if b == 0xC1 else "?")
    return "".join(out).strip()


def scenes(data: bytes):
    """Yield (scene index, raw scene). The index is the game's: a running count over all blocks (blocks hold up to 16
    scenes but many hold fewer), so battle/formation id = index * 4 + formation. Verified in game: battle 640 loaded
    the 161st scene (Doorbull + Ancient Dragon), not block*16+i."""
    index = 0
    for blk in range(len(data) // BLOCK):
        base = blk * BLOCK
        ptrs = struct.unpack_from("<16I", data, base)
        for i, p in enumerate(ptrs):
            if p == 0xFFFFFFFF:
                continue
            start = base + p * 4
            nxt = [q for q in ptrs[i + 1:] if q != 0xFFFFFFFF]
            end = base + nxt[0] * 4 if nxt else base + BLOCK
            stream = data[start:end]
            try:
                raw = gzip.decompress(stream.rstrip(b"\xff"))
            except (OSError, EOFError, zlib.error):
                raw = zlib.decompressobj(31).decompress(stream)
            if len(raw) == SCENE_SIZE:
                yield index, raw
                index += 1


def attack(raw: bytes, idx: int) -> dict:
    o = 0x04C0 + idx * 28
    (pct, _impact, _hurt, _u, mp, _snd, _cs, _cm, target, effect, calc, power,
     cond, st_chance, add, add_mod, st_mask, el_mask, special) = struct.unpack_from("<4BH3H8BIHH", raw, o)
    return dict(
        id=struct.unpack_from("<H", raw, 0x0840 + idx * 2)[0],
        name=ff_text(raw[0x0880 + idx * 32:0x0880 + idx * 32 + 32]),
        accuracy=pct, mp=mp, power=power, damage_calc=calc,
        elements=[e for b, e in enumerate(ELEMENTS) if el_mask & (1 << b)],
        status_mask=st_mask, status_chance=st_chance, target_flags=target,
        effect=effect, raw=raw[o:o + 28].hex(),
    )


def ai_attack_ids(raw: bytes, slot: int) -> tuple[dict, set]:
    """Script entry points and the attack ids the AI pushes before 0x92 (perform command).

    Heuristic: `60 20` (push command 0x20 = enemy attack) then `61 lo hi` or `60 id`, then 0x92.
    Good enough to tell which listed attacks the AI actually uses; not a full disassembler.
    """
    offs = struct.unpack_from("<3H", raw, 0x0E80)
    if offs[slot] == 0xFFFF:
        return {}, set()
    sec = 0x0E80 + offs[slot]
    later = [0x0E80 + o for o in offs[slot + 1:] if o != 0xFFFF]
    sec_end = later[0] if later else SCENE_SIZE
    ptrs = struct.unpack_from("<16H", raw, sec)
    entry = {ATTACK_SCRIPT_SLOTS[i]: p for i, p in enumerate(ptrs) if p != 0xFFFF}
    code = raw[sec:sec_end]
    used = set()
    for i in range(len(code) - 6):
        if code[i] == 0x60 and code[i + 1] == 0x20:
            if code[i + 2] == 0x61 and code[i + 5] == 0x92:
                used.add(code[i + 3] | code[i + 4] << 8)
            elif code[i + 2] == 0x60 and code[i + 4] == 0x92:
                used.add(code[i + 3])
    return entry, used


def enemy(raw: bytes, slot: int, scene_id: int) -> dict:
    o = 0x0298 + slot * 184
    r = raw[o:o + 184]
    lvl, spd, lck, eva, stre, dfn, mag, mdf = r[0x20:0x28]
    el_types, el_rates = r[0x28:0x30], r[0x30:0x38]
    affinities = {}
    for t, rate in zip(el_types, el_rates):
        if t < len(ELEMENTS) and rate in ELEMENT_RATE:
            affinities.setdefault(ELEMENT_RATE[rate], []).append(ELEMENTS[t])
    attack_ids = [a for a in struct.unpack_from("<16H", r, 0x48) if a != 0xFFFF]
    manip = [a for a in struct.unpack_from("<3H", r, 0x94) if a != 0xFFFF]
    mp, ap, morph = struct.unpack_from("<3H", r, 0x9C)
    hp, exp, gil, immune = struct.unpack_from("<4I", r, 0xA4)
    table = {}
    for i in range(32):
        a = attack(raw, i)
        if a["id"] != 0xFFFF:
            table[a["id"]] = a
    entry, ai_used = ai_attack_ids(raw, slot)
    return dict(
        name=ff_text(r[:32]), scene=scene_id, level=lvl, record=r.hex(),
        stats=dict(hp=hp, mp=mp, str=stre, def_=dfn, mag=mag, mdef=mdf, spd=spd, lck=lck, eva=eva),
        exp=exp, ap=ap, gil=gil, affinities=affinities, status_immunity_mask=immune,
        morph_item=None if morph == 0xFFFF else morph,
        moves=[table.get(a, dict(id=a, name="?")) for a in attack_ids],
        manipulate_moves=[table.get(a, dict(id=a, name="?"))["name"] for a in manip],
        ai_entry_points=list(entry), ai_uses=sorted(ai_used),
    )


def formations(raw: bytes):
    """Yield (escapable, enemy ids placed) for each of the scene's 4 formations.

    Battle setup (20 bytes per formation at 0x08): u16 flags at +16; bit 0x0004 clear = can't escape
    (every boss formation checked: Guard Scorpion, Air Buster, Aps, H0512, Ruby Weapon). Placements: 6 x 16 bytes
    per formation at 0x118, u16 enemy id first, 0xFFFF = empty.
    """
    for f in range(4):
        flags = struct.unpack_from("<H", raw, 0x08 + f * 20 + 16)[0]
        ids = {struct.unpack_from("<H", raw, 0x118 + (f * 6 + k) * 16)[0] for k in range(6)} - {0xFFFF}
        if ids:
            yield bool(flags & 0x0004), ids


def main(path: str, out_dir: str = "out"):
    data = Path(path).read_bytes()
    dex, n_scenes = {}, 0
    seen = {}
    for sid, raw in scenes(data):
        n_scenes += 1
        for escapable, ids in formations(raw):
            for eid in ids:
                esc, no_esc = seen.get(eid, (0, 0))
                seen[eid] = (esc + escapable, no_esc + (not escapable))
        for slot, eid in enumerate(struct.unpack_from("<3H", raw, 0)):
            if eid == 0xFFFF or eid in dex:
                continue
            e = enemy(raw, slot, sid)
            if e["name"]:
                dex[eid] = dict(id=eid, **e)
    for eid, m in dex.items():
        esc, no_esc = seen.get(eid, (0, 0))
        m["formations"] = dict(escapable=esc, no_escape=no_esc)
        # Boss formations aren't reliably flagged (unused formation slots copy the flags), so combine signals:
        # bosses can't be Manipulated and appear in a no-escape fight; superbosses (WEAPONs) are caught by HP.
        m["boss"] = (not m["manipulate_moves"] and no_esc > 0) or m["stats"]["hp"] >= 30000
    out = Path(out_dir)
    out.mkdir(exist_ok=True)
    (out / "dex.json").write_text(json.dumps(sorted(dex.values(), key=lambda d: d["id"]), indent=1))

    mons = list(dex.values())
    with_moves = [m for m in mons if m["moves"]]
    with_ai = [m for m in mons if m["ai_entry_points"]]
    with_manip = [m for m in mons if m["manipulate_moves"]]
    counters = [m for m in mons if {"counter_physical", "counter_magical", "counter_general"} & set(m["ai_entry_points"])]
    moves = Counter(len(m["moves"]) for m in mons)
    typed = [m for m in mons if m["affinities"]]
    print(f"scenes decoded:            {n_scenes}")
    print(f"unique enemies:            {len(mons)}")
    print(f"  with >=1 move:           {len(with_moves)}")
    print(f"  with an AI script:       {len(with_ai)}")
    print(f"  with counter scripts:    {len(counters)}")
    print(f"  with Manipulate moves:   {len(with_manip)}")
    print(f"  with element affinities: {len(typed)}")
    print(f"moves per enemy:           {dict(sorted(moves.items()))}")
    uniq_moves = {(a['name'], a.get('power')) for m in mons for a in m['moves']}
    print(f"distinct move names:       {len({n for n, _ in uniq_moves})}")
    print(f"bosses: {sum(m['boss'] for m in mons)}")
    print(f"level range:               {min(m['level'] for m in mons)}-{max(m['level'] for m in mons)}")
    print(f"wrote {out / 'dex.json'}")


if __name__ == "__main__":
    main(*sys.argv[1:])
