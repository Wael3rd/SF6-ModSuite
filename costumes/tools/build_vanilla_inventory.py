"""
build_vanilla_inventory.py — SF6 vanilla costume file inventory builder.

Traverses the game's pak files to build a complete map of every file
referenced by each fighter costume (scenes, meshes, mdf2s, textures,
user-data, chains, etc.). The output is a single JSON file that a mod
loader can use to detect which vanilla files a mod pak overwrites.

Usage:
    python build_vanilla_inventory.py [--out <dir>]

The output folder defaults to OUT_DIR under the game folder; override it with
--out <dir> or the VANILLA_INVENTORY_OUT environment variable (the game folder
is then only read, never written).
"""

import os
import sys, os, re, io, json, time, importlib.util
from collections import OrderedDict

# ---------------------------------------------------------------------------
# REasy-parser imports (standalone, no native extension)
# ---------------------------------------------------------------------------
REASY = os.environ.get("REASY_PARSER", r"..\REasy-parser")  # point REASY_PARSER at your REasy-parser checkout
sys.path.insert(0, REASY)

# PakFile: load via importlib to bypass the native extension dependency
_spec = importlib.util.spec_from_file_location(
    "pakfile_standalone", os.path.join(REASY, "file_handlers", "pak", "pakfile.py")
)
_pakfile_mod = importlib.util.module_from_spec(_spec)
sys.modules["pakfile_standalone"] = _pakfile_mod
_spec.loader.exec_module(_pakfile_mod)
PakFile = _pakfile_mod.PakFile

# RSZ & MDF parsers
from file_handlers.rsz.rsz_file import RszFile
from file_handlers.rsz.rsz_data_types import (
    ResourceData, UserDataData, ObjectData, StringData, ArrayData,
)
from utils.type_registry import TypeRegistry
from file_handlers.mdf.mdf_file import MdfFile

# ---------------------------------------------------------------------------
# Pure-python murmur3 (copied from extract.py, no native ext)
# ---------------------------------------------------------------------------
def _rotl32(x, r):
    return ((x << r) | (x >> (32 - r))) & 0xFFFFFFFF

def _fmix(h):
    h ^= h >> 16; h = (h * 0x85EBCA6B) & 0xFFFFFFFF
    h ^= h >> 13; h = (h * 0xC2B2AE35) & 0xFFFFFFFF
    h ^= h >> 16; return h

def murmur3_hash(data: bytes) -> int:
    c1, c2 = 0xCC9E2D51, 0x1B873593
    h1 = 0xFFFFFFFF
    stream_length = 0
    i, n = 0, len(data)
    while i < n:
        chunk = data[i:i + 4]
        i += len(chunk)
        stream_length += len(chunk)
        k1 = 0
        if len(chunk) >= 1: k1 = chunk[0]
        if len(chunk) >= 2: k1 |= chunk[1] << 8
        if len(chunk) >= 3: k1 |= chunk[2] << 16
        if len(chunk) == 4:
            k1 |= chunk[3] << 24
            k1 = (k1 * c1) & 0xFFFFFFFF
            k1 = _rotl32(k1, 15)
            k1 = (k1 * c2) & 0xFFFFFFFF
            h1 ^= k1
            h1 = _rotl32(h1, 13)
            h1 = (h1 * 5 + 0xE6546B64) & 0xFFFFFFFF
        else:
            k1 = (k1 * c1) & 0xFFFFFFFF
            k1 = _rotl32(k1, 15)
            k1 = (k1 * c2) & 0xFFFFFFFF
            h1 ^= k1
    h1 ^= stream_length
    return _fmix(h1)

def filepath_hash(filepath: str) -> int:
    p = filepath.strip().replace("\\", "/")
    while "//" in p:
        p = p.replace("//", "/")
    lower = murmur3_hash(p.lower().encode("utf-16le")) & 0xFFFFFFFF
    upper = murmur3_hash(p.upper().encode("utf-16le")) & 0xFFFFFFFF
    return ((upper << 32) | lower) & 0xFFFFFFFFFFFFFFFF

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------
GAME_DIR = r"C:\Program Files (x86)\Steam\steamapps\common\Street Fighter 6"
OUT_DIR  = os.path.join(GAME_DIR, "reframework", "agent", "tmp", "costume_loader")
if "--out" in sys.argv:
    OUT_DIR = sys.argv[sys.argv.index("--out") + 1]
elif os.environ.get("VANILLA_INVENTORY_OUT"):
    OUT_DIR = os.environ["VANILLA_INVENTORY_OUT"]
REGISTRY_PATH = os.path.join(REASY, "resources", "data", "dumps", "rszsf6.json")

PAK_PATHS = [
    os.path.join(GAME_DIR, "re_chunk_000.pak"),
    os.path.join(GAME_DIR, "dlc", "re_dlc_stm_1792750.pak"),
    os.path.join(GAME_DIR, "dlc", "re_dlc_stm_1792751.pak"),
]

# Known version suffixes per RE Engine file type (SF6 build).
# These were determined empirically from extracted pak files.
KNOWN_SUFFIXES = {
    "scn":       "20",
    "user":      "2",
    "mesh":      "230110883",
    "mdf2":      "31",
    "tex":       "241101895",
    "chain":     "52",
    "msg":       "21",
}

# Candidate suffixes to try when the extension isn't in the known table.
# Includes all known values plus a broad search range.
CANDIDATE_SUFFIXES = sorted(set(
    list(range(0, 500))
    + [230110883, 241101895, 2109148288]
    + [int(v) for v in KNOWN_SUFFIXES.values()]
))

ESF_ROOT_SCENE = "natives/stm/product/charparam/esf/esf.scn.20"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def load_type_registry():
    print("Loading RSZ type registry...")
    reg = TypeRegistry(REGISTRY_PATH)
    print("Registry loaded.")
    return reg

def load_paks():
    """Load all pak indices, return (list_of_PakFile, combined_hash_map)."""
    paks = []
    combined = {}  # hash -> (pak, entry)
    for pak_path in PAK_PATHS:
        if not os.path.exists(pak_path):
            print(f"[skip] pak not found: {pak_path}")
            continue
        size_gb = os.path.getsize(pak_path) / 1e9
        print(f"Loading pak index: {os.path.basename(pak_path)} ({size_gb:.1f} GB)...")
        pak = PakFile()
        pak.filepath = pak_path
        with open(pak_path, "rb") as f:
            pak.read_contents(f)
        print(f"  {len(pak.entries)} entries")
        paks.append(pak)
        for e in pak.entries:
            combined[e.combined_hash] = (pak, e)
    print(f"Combined index: {len(combined)} unique hashes")
    return paks, combined

def extract_from_pak(pak_index, file_path):
    """Extract a file from the pak by path. Returns bytes or None."""
    h = filepath_hash(file_path)
    item = pak_index.get(h)
    if item is None:
        return None
    pak, entry = item
    buf = io.BytesIO()
    try:
        pak.read_entry(entry, buf)
    except Exception as exc:
        print(f"  [WARN] extraction failed for {file_path}: {exc}")
        return None
    return buf.getvalue()

def path_in_pak(pak_index, file_path):
    """Check if a file path exists in the pak index."""
    return filepath_hash(file_path) in pak_index

def parse_rsz(data, filepath, registry):
    """Parse RSZ (scn/user) from raw bytes."""
    rsz = RszFile()
    rsz.filepath = filepath
    rsz.type_registry = registry
    rsz.game_version = "SF6"
    rsz.read(data, validate_type_registry=False)
    return rsz

def parse_mdf(data, filepath):
    """Parse MDF2 from raw bytes."""
    mdf = MdfFile()
    mdf.read(data, file_path=filepath)
    return mdf

def clean_path(raw):
    """Normalize a path from RSZ: strip @, NUL, whitespace."""
    if raw is None:
        return ""
    s = raw.strip().rstrip("\x00").strip()
    if s.startswith("@"):
        s = s[1:]
    return s

def to_pak_path(relative_path, ext_suffix):
    """Convert a relative resource path to a full pak path.

    relative_path: e.g. 'Product/Model/esf/esf001/001/01/foo.mesh'
    ext_suffix:    e.g. '230110883'
    Returns:       'natives/stm/product/model/esf/esf001/001/01/foo.mesh.230110883'
    """
    p = relative_path.replace("\\", "/").lower()
    return f"natives/stm/{p}.{ext_suffix}"

def get_extension(path):
    """Get the RE Engine file extension (last dotted part)."""
    # e.g. 'foo.mesh' -> 'mesh', 'bar.mdf2' -> 'mdf2', 'baz.scn' -> 'scn'
    base = path.rsplit("/", 1)[-1]
    parts = base.split(".")
    if len(parts) >= 2:
        return parts[-1]
    return ""

def resolve_pak_path(pak_index, relative_path, suffix_cache, unresolved):
    """Resolve a relative path to a full pak path with version suffix.

    Returns (pak_path, hash, in_pak) or (pak_path_guess, 0, False) if unresolved.
    """
    ext = get_extension(relative_path)
    if not ext:
        return (relative_path, 0, False)

    # Try known suffix first
    if ext in suffix_cache:
        suffix = suffix_cache[ext]
        pak_path = to_pak_path(relative_path, suffix)
        h = filepath_hash(pak_path)
        return (pak_path, h, h in pak_index)

    # Discover suffix for this extension
    for candidate in CANDIDATE_SUFFIXES:
        candidate_path = to_pak_path(relative_path, str(candidate))
        h = filepath_hash(candidate_path)
        if h in pak_index:
            suffix_cache[ext] = str(candidate)
            print(f"  [DISCOVERED] suffix for .{ext} = .{candidate}")
            return (candidate_path, h, True)

    # Not found — record as unresolved
    unresolved_key = f"suffix_{ext}"
    if unresolved_key not in unresolved:
        unresolved[unresolved_key] = f"Could not determine pak suffix for .{ext} files (tried {len(CANDIDATE_SUFFIXES)} candidates on: {relative_path})"
    # Return a guess
    pak_path = f"natives/stm/{relative_path.replace(chr(92), '/').lower()}"
    return (pak_path, 0, False)

# ---------------------------------------------------------------------------
# Data extraction from RSZ scenes
# ---------------------------------------------------------------------------

def extract_esf_entries(rsz):
    """Parse the root esf.scn.20 and return list of (name, scene_path, parent_id)."""
    entries = []
    for fi in rsz.folder_infos:
        inst_id = rsz.object_table[fi.id] if fi.id < len(rsz.object_table) else None
        if inst_id is None:
            continue
        fields = rsz.parsed_elements.get(inst_id, {})
        name_f = fields.get("Name")
        path_f = fields.get("ScenePath")
        if name_f and isinstance(name_f, (StringData, ResourceData)):
            name = clean_path(name_f.value)
            scene_path = ""
            if path_f and isinstance(path_f, (StringData, ResourceData)):
                scene_path = clean_path(path_f.value)
            entries.append((name, scene_path, fi.parent_id))
    return entries

def extract_costume_refs(rsz, registry):
    """Scan a costume scene for all Resource and UserData paths.

    Returns dict with keys:
      resources: list of (field_name, clean_path, type_name)
      userdata:  list of (field_name, clean_path, type_name)
    """
    resources = []
    userdata = []
    for idx, inst in enumerate(rsz.instance_infos):
        info = registry.get_type_info(inst.type_id)
        type_name = info.get("name", "") if info else ""
        fields = rsz.parsed_elements.get(idx, {})
        for fname, fval in fields.items():
            if isinstance(fval, ResourceData):
                p = clean_path(fval.value)
                if p:
                    resources.append((fname, p, type_name))
            elif isinstance(fval, UserDataData):
                p = clean_path(fval.string)
                if p:
                    userdata.append((fname, p, type_name))
            elif isinstance(fval, ArrayData):
                for elt in fval.values:
                    if isinstance(elt, ResourceData):
                        p = clean_path(elt.value)
                        if p:
                            resources.append((fname + "[]", p, type_name))

    return {"resources": resources, "userdata": userdata}

def extract_tex_paths_from_mdf(mdf):
    """Get all texture paths from an MdfFile."""
    paths = []
    for mat in mdf.materials:
        for tex in mat.textures:
            p = clean_path(tex.tex_path)
            if p:
                paths.append(p)
    return paths

def extract_ccvd_refs(rsz, registry):
    """Parse a CCVD .user file and return all referenced CMD user paths."""
    paths = []
    for idx, inst in enumerate(rsz.instance_infos):
        info = registry.get_type_info(inst.type_id)
        type_name = info.get("name", "") if info else ""
        fields = rsz.parsed_elements.get(idx, {})
        for fname, fval in fields.items():
            if isinstance(fval, UserDataData):
                p = clean_path(fval.string)
                if p:
                    paths.append(p)
    return paths

# ---------------------------------------------------------------------------
# Main logic
# ---------------------------------------------------------------------------

def classify_kind(ext, field_name=""):
    """Map a file extension to a kind label."""
    mapping = {
        "mesh": "mesh", "mdf2": "mdf2", "tex": "tex",
        "user": "user", "scn": "scene", "chain": "chain",
        "motbank": "motbank", "mcambank": "mcambank",
        "jmap": "jmap", "ikbodyrig": "ikbodyrig",
        "msg": "msg",
    }
    kind = mapping.get(ext, ext)
    # Refine user files based on field name
    if ext == "user":
        fl = field_name.lower()
        if "ccvd" in fl or "costumecolorvariation" in fl:
            kind = "ccvd"
        elif "shape" in fl or "musclecontrol" in fl:
            kind = "shape"
        elif "jcs" in fl or "jointconstraints" in fl:
            kind = "jcs"
        elif "chain" in fl:
            kind = "chain_user"
        elif "aogeo" in fl or "aogeometry" in fl:
            kind = "aogeo"
        elif "havok" in fl:
            kind = "havok"
        elif "sound" in fl:
            kind = "sound"
        elif "cmd" in fl.replace("_", ""):
            kind = "cmd"
    return kind

def build_inventory():
    t0 = time.time()

    registry = load_type_registry()
    paks, pak_index = load_paks()

    suffix_cache = dict(KNOWN_SUFFIXES)  # ext -> suffix string
    unresolved = OrderedDict()

    # ------ Step 1: Enumerate all costume scenes by probing the pak ------
    # The root esf.scn.20 only lists base fighters; DLC fighters have
    # individual scene files. We probe all plausible IDs and slots.
    print("\nProbing pak for all costume scenes...")
    costume_entries = []
    for fid in range(1, 50):
        for prefix in ("v", "a"):
            for slot in range(0, 10):
                name = f"esf{fid:03d}{prefix}{slot:02d}"
                scene_rel = f"Product/CharParam/esf/esf{fid:03d}/{name}.scn"
                scene_pak = f"natives/stm/product/charparam/esf/esf{fid:03d}/{name}.scn.{suffix_cache['scn']}"
                if path_in_pak(pak_index, scene_pak):
                    effective_no = slot if prefix == "v" else 100 + slot
                    costume_entries.append({
                        "fighter": fid,
                        "fighter_dir": f"esf{fid:03d}",
                        "costume_no": effective_no,
                        "slot_type": prefix,
                        "name": name,
                        "scene_rel": scene_rel,
                    })

    print(f"Found {len(costume_entries)} costume entries across "
          f"{len(set(c['fighter'] for c in costume_entries))} fighters")

    # ------ Step 2: Process each costume ------
    costumes_out = []
    total_files = 0
    total_not_found = 0
    mdf2_parse_count = 0
    ccvd_parse_count = 0

    for ci, ce in enumerate(costume_entries):
        fighter_id = ce["fighter"]
        costume_no = ce["costume_no"]
        scene_rel = ce["scene_rel"]  # e.g. "Product/CharParam/esf/esf001/esf001v00.scn"

        # Build the pak path for the costume scene
        scene_pak = f"natives/stm/{scene_rel.lower()}.{suffix_cache['scn']}"
        scene_hash = filepath_hash(scene_pak)
        scene_in_pak = scene_hash in pak_index

        print(f"\n[{ci+1}/{len(costume_entries)}] {ce['name']} -> {scene_pak} "
              f"({'OK' if scene_in_pak else 'MISSING'})")

        files = []
        # Add the scene file itself
        files.append({
            "path": scene_pak,
            "hash": scene_hash,
            "kind": "scene",
            "in_pak": scene_in_pak,
        })

        if not scene_in_pak:
            unresolved[f"scene_{ce['name']}"] = f"Costume scene not found in pak: {scene_pak}"
            costumes_out.append({
                "fighter": fighter_id,
                "fighter_dir": ce["fighter_dir"],
                "costume_no": costume_no,
                "scene": scene_pak,
                "model_dir": "",
                "files": files,
            })
            total_files += len(files)
            total_not_found += 1
            continue

        # Extract and parse the costume scene
        scene_data = extract_from_pak(pak_index, scene_pak)
        if scene_data is None:
            unresolved[f"parse_{ce['name']}"] = f"Failed to extract scene from pak: {scene_pak}"
            costumes_out.append({
                "fighter": fighter_id,
                "fighter_dir": ce["fighter_dir"],
                "costume_no": costume_no,
                "scene": scene_pak,
                "model_dir": "",
                "files": files,
            })
            total_files += len(files)
            continue

        try:
            scene_rsz = parse_rsz(scene_data, ce["name"] + ".scn.20", registry)
        except Exception as exc:
            unresolved[f"rsz_{ce['name']}"] = f"RSZ parse error: {exc}"
            costumes_out.append({
                "fighter": fighter_id,
                "fighter_dir": ce["fighter_dir"],
                "costume_no": costume_no,
                "scene": scene_pak,
                "model_dir": "",
                "files": files,
            })
            total_files += len(files)
            continue

        refs = extract_costume_refs(scene_rsz, registry)

        # Collect unique paths from scene
        seen_paths = {scene_pak}
        model_dir = ""
        mdf2_rel_paths = []   # relative paths of mdf2 to parse for textures
        ccvd_rel_paths = []   # relative paths of CCVD user files to parse for CMD refs

        # Process Resource references (mesh, mdf2, motbank, etc.)
        for field_name, rel_path, type_name in refs["resources"]:
            ext = get_extension(rel_path)
            pak_path, h, in_pak = resolve_pak_path(pak_index, rel_path, suffix_cache, unresolved)
            if pak_path not in seen_paths:
                seen_paths.add(pak_path)
                kind = classify_kind(ext, field_name)
                files.append({
                    "path": pak_path,
                    "hash": h,
                    "kind": kind,
                    "in_pak": in_pak,
                })
                if not in_pak:
                    total_not_found += 1
                if ext == "mdf2":
                    mdf2_rel_paths.append(rel_path)
                # Derive model_dir from first mesh path
                if ext == "mesh" and not model_dir:
                    # e.g. "product/model/esf/esf001/001/01/foo.mesh" -> "natives/stm/product/model/esf/esf001/001"
                    parts = pak_path.rsplit("/", 2)
                    if len(parts) >= 3:
                        model_dir = parts[0]  # up to the part-number directory

        # Process UserData references
        for field_name, rel_path, type_name in refs["userdata"]:
            ext = get_extension(rel_path)
            pak_path, h, in_pak = resolve_pak_path(pak_index, rel_path, suffix_cache, unresolved)
            if pak_path not in seen_paths:
                seen_paths.add(pak_path)
                kind = classify_kind(ext, field_name)
                files.append({
                    "path": pak_path,
                    "hash": h,
                    "kind": kind,
                    "in_pak": in_pak,
                })
                if not in_pak:
                    total_not_found += 1
                # Track CCVD files for further parsing
                if "ccvd" in rel_path.lower() or "costumecolorvariation" in field_name.lower():
                    ccvd_rel_paths.append(rel_path)

        # ------ Step 2b: Parse each mdf2 for texture paths ------
        for mdf_rel in mdf2_rel_paths:
            ext = get_extension(mdf_rel)
            mdf_pak_path = to_pak_path(mdf_rel, suffix_cache.get(ext, "31"))
            mdf_data = extract_from_pak(pak_index, mdf_pak_path)
            if mdf_data is None:
                continue
            try:
                mdf = parse_mdf(mdf_data, os.path.basename(mdf_rel) + "." + suffix_cache.get(ext, "31"))
                mdf2_parse_count += 1
            except Exception as exc:
                unresolved[f"mdf_{mdf_pak_path}"] = f"MDF parse error: {exc}"
                continue

            for tex_rel in extract_tex_paths_from_mdf(mdf):
                tex_ext = get_extension(tex_rel)
                tex_pak, tex_h, tex_in = resolve_pak_path(
                    pak_index, tex_rel, suffix_cache, unresolved
                )
                if tex_pak not in seen_paths:
                    seen_paths.add(tex_pak)
                    files.append({
                        "path": tex_pak,
                        "hash": tex_h,
                        "kind": "tex",
                        "in_pak": tex_in,
                    })
                    if not tex_in:
                        total_not_found += 1

        # ------ Step 2c: Parse CCVD for CMD references ------
        for ccvd_rel in ccvd_rel_paths:
            ccvd_ext = get_extension(ccvd_rel)
            ccvd_pak_path = to_pak_path(ccvd_rel, suffix_cache.get(ccvd_ext, "2"))
            ccvd_data = extract_from_pak(pak_index, ccvd_pak_path)
            if ccvd_data is None:
                continue
            try:
                ccvd_rsz = parse_rsz(ccvd_data, os.path.basename(ccvd_rel) + ".2", registry)
                ccvd_parse_count += 1
            except Exception as exc:
                unresolved[f"ccvd_{ccvd_pak_path}"] = f"CCVD RSZ parse error: {exc}"
                continue

            for cmd_rel in extract_ccvd_refs(ccvd_rsz, registry):
                cmd_ext = get_extension(cmd_rel)
                cmd_pak, cmd_h, cmd_in = resolve_pak_path(
                    pak_index, cmd_rel, suffix_cache, unresolved
                )
                if cmd_pak not in seen_paths:
                    seen_paths.add(cmd_pak)
                    files.append({
                        "path": cmd_pak,
                        "hash": cmd_h,
                        "kind": "cmd",
                        "in_pak": cmd_in,
                    })
                    if not cmd_in:
                        total_not_found += 1

        total_files += len(files)

        costumes_out.append({
            "fighter": fighter_id,
            "fighter_dir": ce["fighter_dir"],
            "costume_no": costume_no,
            "scene": scene_pak,
            "model_dir": model_dir,
            "files": files,
        })

        sys.stdout.write(f"  -> {len(files)} files collected\r")
        sys.stdout.flush()

    # ------ Step 3: Build version_suffixes map from cache ------
    version_suffixes = {}
    for ext, suffix in sorted(suffix_cache.items()):
        version_suffixes[ext] = f".{ext}.{suffix}"

    # ------ Step 4: Output JSON ------
    output = OrderedDict()
    output["version_suffixes"] = version_suffixes
    output["unresolved"] = dict(unresolved) if unresolved else {}
    output["costumes"] = costumes_out

    os.makedirs(OUT_DIR, exist_ok=True)
    out_path = os.path.join(OUT_DIR, "vanilla_costume_paths.json")
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(output, f, indent=2, ensure_ascii=False)

    elapsed = time.time() - t0
    n_fighters = len(set(c["fighter"] for c in costumes_out))
    n_costumes = len(costumes_out)

    print(f"\n{'='*60}")
    print(f"DONE in {elapsed:.1f}s")
    print(f"  Fighters:          {n_fighters}")
    print(f"  Costumes:          {n_costumes}")
    print(f"  Total files:       {total_files}")
    print(f"  Not found in pak:  {total_not_found}")
    print(f"  MDF2 parsed:       {mdf2_parse_count}")
    print(f"  CCVD parsed:       {ccvd_parse_count}")
    print(f"  Suffixes found:    {version_suffixes}")
    if unresolved:
        print(f"  Unresolved issues: {len(unresolved)}")
        for k, v in list(unresolved.items())[:5]:
            print(f"    {k}: {v}")
        if len(unresolved) > 5:
            print(f"    ... and {len(unresolved)-5} more")
    print(f"\n  Output: {out_path}")
    print(f"{'='*60}")

if __name__ == "__main__":
    build_inventory()
