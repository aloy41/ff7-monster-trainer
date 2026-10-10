"""Split FF7 PC kernel.bin into its 27 sections and decode the ones the trainer needs.

    python tools/kernel_dump.py <kernel.bin> [out_dir]

kernel.bin = 27 gzip streams, each prefixed by u16 compressed size, u16 raw size, u16 section index
(Qhimm FF7/Kernel/Kernel.bin). Sections used here:
  0  command data     32 x 8    (menu commands: Attack, Magic, Steal, Manipulate, ...)
  1  attack data     128 x 28   (player-side attacks; same record as scene.bin attacks)
  3  initial savemap  (new-game party, characters, items)
  8  materia data     91 x 20
  9  command descriptions, 17 command names, 18 magic names (text sections, 0xFF-terminated with u16 offset table)
"""
import gzip
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from scene_dump import ELEMENTS, ff_text  # noqa: E402

SECTION_NAMES = {
    0: "command_data", 1: "attack_data", 2: "battle_growth", 3: "initial_savemap", 4: "item_data",
    5: "weapon_data", 6: "armor_data", 7: "accessory_data", 8: "materia_data",
    9: "command_descriptions", 10: "magic_descriptions", 11: "item_descriptions", 12: "weapon_descriptions",
    13: "armor_descriptions", 14: "accessory_descriptions", 15: "materia_descriptions", 16: "key_item_descriptions",
    17: "command_names", 18: "magic_names", 19: "item_names", 20: "weapon_names", 21: "armor_names",
    22: "accessory_names", 23: "materia_names", 24: "key_item_names", 25: "battle_text", 26: "summon_names",
}


def sections(data: bytes) -> list[bytes]:
    out, o = [], 0
    while o + 6 <= len(data):
        csize, rsize, idx = struct.unpack_from("<3H", data, o)
        raw = gzip.decompress(data[o + 6:o + 6 + csize])
        if len(raw) != rsize:
            raise ValueError(f"section {idx}: {len(raw)} bytes, header says {rsize}")
        out.append(raw)
        o += 6 + csize
    return out


def text_table(raw: bytes) -> list[str]:
    first = struct.unpack_from("<H", raw, 0)[0]
    offs = struct.unpack_from(f"<{first // 2}H", raw, 0)
    return [ff_text(raw[o:]) for o in offs]


def main(path: str, out_dir: str = "out"):
    secs = sections(Path(path).read_bytes())
    out = Path(out_dir) / "kernel"
    out.mkdir(parents=True, exist_ok=True)
    for i, raw in enumerate(secs):
        (out / f"{i:02d}_{SECTION_NAMES.get(i, 'unknown')}.bin").write_bytes(raw)

    cmd_names, magic_names = text_table(secs[17]), text_table(secs[18])
    materia_names = text_table(secs[23])
    commands = []
    for i in range(len(secs[0]) // 8):
        cursor, target, _u, cam1, cam2 = struct.unpack_from("<BBHHH", secs[0], i * 8)
        commands.append(dict(id=i, name=cmd_names[i] if i < len(cmd_names) else "", cursor_action=cursor,
                             target_flags=target))
    attacks = []
    for i in range(len(secs[1]) // 28):
        r = secs[1][i * 28:(i + 1) * 28]
        (pct, _imp, _hurt, _u, mp, _snd, _cs, _cm, target, effect, calc, power, *_rest) = struct.unpack_from(
            "<4BH3H8B", r)
        el = struct.unpack_from("<H", r, 0x18)[0]
        attacks.append(dict(id=i, name=magic_names[i] if i < len(magic_names) else "", mp=mp, power=power,
                            damage_calc=calc, elements=[e for b, e in enumerate(ELEMENTS) if el & (1 << b)]))
    summary = dict(
        section_sizes={SECTION_NAMES.get(i, str(i)): len(s) for i, s in enumerate(secs)},
        commands=commands, attacks=attacks,
        materia=[dict(id=i, name=n) for i, n in enumerate(materia_names) if n],
    )
    (out / "kernel.json").write_text(json.dumps(summary, indent=1))
    print(f"sections: {len(secs)}")
    print("commands:", ", ".join(f"{c['id']}:{c['name']}" for c in commands if c["name"]))
    used = [a for a in attacks if a["name"]]
    print(f"player attacks: {len(attacks)} slots, {len(used)} named; enemy-skill block:",
          ", ".join(a["name"] for a in attacks[0x48:0x60]))
    print("materia:", len(summary["materia"]), "named, e.g.",
          [m["name"] for m in summary["materia"] if m["name"] in ("Manipulate", "Morph", "Steal", "Enemy Skill")])
    print(f"wrote {out}")


if __name__ == "__main__":
    main(*sys.argv[1:])
