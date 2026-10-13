"""
make_oracle.py - test data for the structural tests (scene_test, msg_test, table_test).

Usage: python make_oracle.py <out dir> [<game dir>]
  <out>/in/        the game's own files, read from re_chunk_000.pak
  <out>/expected/  what make_static_structural.py builds from them (REasy-parser, see that tool)
The C++ loader must build the same bytes as <out>/expected from <out>/in.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
sys.path.insert(0, TOOLS)
from costume_loader_reference import (PakIndex, filepath_hash, GAME_DIR_DEFAULT,
                                      ESF_ROOT_SCENE, COSTUME_TABLE, COSTUME_MSG)

VER_SCENE_FMT = "natives/stm/product/flow/version_update/ver_02_{:04d}/esf.scn.20"


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    out = os.path.abspath(sys.argv[1])
    game = sys.argv[2] if len(sys.argv) > 2 else GAME_DIR_DEFAULT
    os.makedirs(os.path.join(out, "in"), exist_ok=True)
    paks = PakIndex()
    paks.add_pak(os.path.join(game, "re_chunk_000.pak"))
    files = {"esf.scn.20": ESF_ROOT_SCENE, "fightercostumeuserdata.user.2": COSTUME_TABLE,
             "fightercostumemessage.msg.21": COSTUME_MSG}
    for num in range(300, 2000, 100):
        if paks.has(filepath_hash(VER_SCENE_FMT.format(num))):
            files[f"ver_02_{num:04d}_esf.scn.20"] = VER_SCENE_FMT.format(num)
    for local, path in files.items():
        with open(os.path.join(out, "in", local), "wb") as f:
            f.write(paks.read_data(filepath_hash(path)))
    paks.close()
    subprocess.check_call([sys.executable, os.path.join(TOOLS, "make_static_structural.py"),
                           os.path.join(out, "expected")])
    print(f"oracle written to {out}")


if __name__ == "__main__":
    main()
