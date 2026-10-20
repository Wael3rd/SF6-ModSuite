"""Manifest of an RE Engine v4 pak: the SHA-256 of the whole file and, per entry (table order), its hash,
offset, sizes, attributes and the SHA-1 of its stored blob. Two paks are byte-identical when their
manifests' "file" lines match; their entries are identical when the entry lines match once the
offsets are left out (--entries).

  python pak_manifest.py <pak> [-o out.txt]
  python pak_manifest.py --compare <a.txt|a.pak> <b.txt|b.pak>
"""
import argparse, hashlib, struct, sys

HDR = struct.Struct("<IBBhII")
ENT = struct.Struct("<IIqqqqq")
CHUNK = 64 << 20


def manifest(path):
    with open(path, "rb") as f:
        head = f.read(HDR.size)
        magic, major, minor, feat, count, fp = HDR.unpack(head)
        if magic != 0x414B504B or major != 4:
            raise SystemExit(f"{path}: not a v4 pak")
        table = f.read(count * ENT.size)
        whole = hashlib.sha256(head + table)
        ents = [ENT.unpack_from(table, i * ENT.size) for i in range(count)]
        blobs = sorted({(e[2], e[3]) for e in ents})
        pos = HDR.size + len(table)
        sums = {}
        for off, size in blobs:
            if off < pos:   # overlapping blobs: hashed separately
                f.seek(off)
                sums[(off, size)] = hashlib.sha1(f.read(size)).hexdigest()
                f.seek(pos)
                continue
            if off > pos:
                gap = f.read(off - pos)
                whole.update(gap)
                pos = off
            h = hashlib.sha1()
            left = size
            while left:
                b = f.read(min(CHUNK, left))
                if not b:
                    raise SystemExit(f"{path}: truncated at {pos}")
                h.update(b); whole.update(b)
                left -= len(b); pos += len(b)
            sums[(off, size)] = h.hexdigest()
        while True:
            b = f.read(CHUNK)
            if not b:
                break
            whole.update(b)
    lines = [f"file {whole.hexdigest()} entries {count}"]
    for lo, hi, off, cs, ds, attr, ck in ents:
        lines.append(f"{hi:08x}{lo:08x} off={off} cs={cs} ds={ds} attr={attr:x} ck={ck:x} sha1={sums[(off, cs)]}")
    return lines


def load(p):
    if p.lower().endswith(".pak"):
        return manifest(p)
    with open(p, encoding="utf-8") as f:
        return f.read().splitlines()


def entries_only(lines):
    out = []
    for l in lines[1:]:
        parts = [x for x in l.split() if not x.startswith("off=")]
        out.append(" ".join(parts))
    return sorted(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("paths", nargs="+")
    ap.add_argument("-o")
    ap.add_argument("--compare", action="store_true")
    a = ap.parse_args()
    if a.compare:
        x, y = load(a.paths[0]), load(a.paths[1])
        same_file = x[0] == y[0]
        ex, ey = entries_only(x), entries_only(y)
        print(f"whole file: {'IDENTICAL' if same_file else 'different'}  ({x[0]} / {y[0]})")
        if ex == ey:
            print(f"entries: IDENTICAL ({len(ex)})")
        else:
            sx, sy = set(ex), set(ey)
            print(f"entries: DIFFERENT, only in A: {len(sx - sy)}, only in B: {len(sy - sx)}")
            for l in sorted(sx - sy)[:20]: print("  A", l)
            for l in sorted(sy - sx)[:20]: print("  B", l)
        sys.exit(0 if same_file and ex == ey else (2 if ex == ey else 1))
    lines = manifest(a.paths[0])
    text = "\n".join(lines) + "\n"
    if a.o:
        open(a.o, "w", encoding="utf-8").write(text)
        print(lines[0])
    else:
        sys.stdout.write(text)


if __name__ == "__main__":
    main()
