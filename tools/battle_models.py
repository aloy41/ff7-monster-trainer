"""Index battle.lgp: each dex enemy -> its battle model, bone/texture/animation counts, and rig family.

    python tools/battle_models.py <battle.lgp> out/dex.json

Model files are named <2-letter model id><2-letter part>: the model id is base-26 letters of the enemy id
(enemy 0 -> "aa", 27 -> "bb"); part "aa" = skeleton header, "da" = animation pack.
Skeleton header ("XXaa", Qhimm FF7/Battle/Battle_Model): 13 u32s: [3]=bones, [6]=textures, [7]=body animations,
[9]=weapons, [10]=weapon animations; then 12 bytes per bone.
Rig family = enemies whose skeleton bone table AND animation pack are byte-identical (palette/stat swaps).
"""
import hashlib
import json
import struct
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from lgp import Lgp  # noqa: E402


def model_id(enemy_id: int) -> str:
    return chr(ord("a") + enemy_id // 26) + chr(ord("a") + enemy_id % 26)


def main(lgp_path: str, dex_path: str):
    lgp = Lgp(lgp_path)
    dex = json.loads(Path(dex_path).read_text())
    families = defaultdict(list)
    missing = []
    for m in dex:
        mid = model_id(m["id"])
        try:
            skel = lgp.read(mid + "aa")
        except KeyError:
            missing.append(m["name"])
            m["model"] = None
            continue
        h = struct.unpack_from("<13I", skel, 0)
        bones, textures, anims = h[3], h[6], h[7]
        bone_table = skel[52:52 + bones * 12]
        try:
            anim_pack = lgp.read(mid + "da")
        except KeyError:
            anim_pack = b""
        rig = hashlib.sha1(bone_table + b"|" + anim_pack).hexdigest()[:12]
        m["model"] = dict(id=mid, bones=bones, textures=textures, animations=anims,
                          anim_pack_bytes=len(anim_pack), rig=rig)
        families[rig].append(m["name"])
    for m in dex:
        if m.get("model"):
            fam = families[m["model"]["rig"]]
            m["model"]["family"] = sorted(set(fam)) if len(set(fam)) > 1 else []
    Path(dex_path).write_text(json.dumps(dex, indent=1))

    have = [m for m in dex if m.get("model")]
    multi = {k: sorted(set(v)) for k, v in families.items() if len(set(v)) > 1}
    anim_counts = sorted(m["model"]["animations"] for m in have)
    print(f"enemies with a battle model: {len(have)} / {len(dex)}  (missing: {missing[:8]}{'...' if len(missing) > 8 else ''})")
    print(f"animations per model: min {anim_counts[0]}, median {anim_counts[len(anim_counts) // 2]}, max {anim_counts[-1]}")
    print(f"rig families with 2+ distinct species: {len(multi)}")
    for fam in sorted(multi.values(), key=len, reverse=True)[:12]:
        print("  ", " / ".join(fam))


if __name__ == "__main__":
    main(*sys.argv[1:])
