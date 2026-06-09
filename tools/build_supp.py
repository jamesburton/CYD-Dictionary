#!/usr/bin/env python3
"""Build a supplementary dictionary (format v4) from a JSON source.
Usage: python tools/build_supp.py tools/sources/mythology.json mythology --mode additive --floor safe"""
import json, os, struct, sys
from normalize import norm_key
TIER = {"safe":0,"mild":1,"teen":2,"full":3}; POS = {"noun":0,"verb":1,"adjective":2,"adverb":3,"name":4,"other":4}
def main():
    src, name = sys.argv[1], sys.argv[2]
    mode = "additive"; floor = "safe"
    if "--mode" in sys.argv: mode = sys.argv[sys.argv.index("--mode")+1]
    if "--floor" in sys.argv: floor = sys.argv[sys.argv.index("--floor")+1]
    entries = json.load(open(src, encoding="utf-8"))
    rows = []
    for e in entries:
        disp = e["term"].strip(); key = norm_key(disp)
        if not key: continue
        tier = TIER.get(e.get("tier","safe"),0); pc = POS.get(e.get("pos","name"),4)
        ms = [(tier, pc, d.strip(), (e.get("example","") if i==0 else "").strip())
              for i,d in enumerate(e["defs"]) if d.strip()]
        if ms: rows.append((key, disp, ms))
    rows.sort(key=lambda r: r[0])
    # Drop adjacent duplicate norm_keys (keep first, warn about dropped).
    deduped = []; dropped = []
    for row in rows:
        if deduped and deduped[-1][0] == row[0]:
            dropped.append(row[0])
        else:
            deduped.append(row)
    if dropped:
        print(f"WARNING: dropped {len(dropped)} duplicate key(s): {', '.join(dropped)}")
    rows = deduped
    out = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "data", "dicts")
    os.makedirs(out, exist_ok=True)
    offsets = {}
    with open(os.path.join(out, name+".dat"), "wb") as dat:
        for key, disp, ms in rows:
            offsets[key] = dat.tell()
            db = disp.encode("utf-8")[:255]; dat.write(struct.pack("<B", len(db)) + db)
            dat.write(struct.pack("<B", len(ms)))
            for tier, pc, d, ex in ms:
                tr = tier | (3 << 2)  # minTier=tier, maxTier=FULL
                de = d.encode("utf-8")[:600]; ee = ex.encode("utf-8")[:400]
                dat.write(struct.pack("<B", tr) + struct.pack("<B", pc))
                dat.write(struct.pack("<H", len(de)) + de); dat.write(struct.pack("<H", len(ee)) + ee)
    with open(os.path.join(out, name+".idx"), "wb") as idx:
        idx.write(b"DIDX"); idx.write(struct.pack("<II", 4, len(rows)))
        for key, disp, ms in rows:
            wmin = min(m[0] for m in ms)
            idx.write(struct.pack("<I", offsets[key])); idx.write(struct.pack("<B", wmin))
            idx.write(key.encode("ascii") + b"\x00")
    with open(os.path.join(out, name+".meta"), "w", encoding="utf-8") as m:
        m.write(f"name={name.title()}\nversion=supp-2026.06\nmode={mode}\nfloor={floor}\nformat=4\n")
    print(f"{name}: {len(rows)} entries -> data/dicts/{name}.idx/.dat/.meta")
if __name__ == "__main__": raise SystemExit(main())
