#!/usr/bin/env python3
"""The game's own messages, read from its paks (read-only): every entry of every .msg file, in every language.

Used by sync_game_words.py and export_tsv.py. `load(game_dir)` returns {entry name: {"guid": ..., "text": {code: text}}}.
"""
import os
import struct
import uuid
import zlib

try:
    import zstandard
except ImportError:
    zstandard = None

DEFAULT_GAME = r"C:\Program Files (x86)\Steam\steamapps\common\Street Fighter 6"
# via.Language -> our language code (checked in game; 9 is European Portuguese, the game ships none)
CODES = {0: "ja", 1: "en", 2: "fr", 3: "it", 4: "de", 5: "es", 6: "ru", 7: "pl", 10: "pt", 11: "ko", 12: "zh-Hant", 13: "zh-Hans", 21: "ar", 32: "es-419"}
LANGS = ["en", "fr", "ja", "zh-Hans", "it", "de", "es", "es-419", "ru", "pl", "pt", "ko", "zh-Hant", "ar"]
_KEY = bytes([0xCF, 0xCE, 0xFB, 0xF8, 0xEC, 0x0A, 0x33, 0x66, 0x93, 0xA9, 0x1D, 0x93, 0x50, 0x39, 0x5F, 0x09])


def _decrypt(b):
    b = bytearray(b)
    prev = 0
    for i in range(len(b)):
        c = b[i]
        b[i] = c ^ prev ^ _KEY[i & 15]
        prev = c
    return bytes(b)


def _parse(data, out):
    count = struct.unpack_from("<I", data, 16)[0]
    nlang = struct.unpack_from("<I", data, 24)[0]
    data_off = struct.unpack_from("<Q", data, 32)[0]
    lang_off = struct.unpack_from("<Q", data, 48)[0]
    langs = struct.unpack_from("<%dI" % nlang, data, lang_off)
    pool = _decrypt(data[data_off:])

    def read(ptr):
        if ptr == 0:
            return ""
        p = ptr - data_off
        e = p
        while e + 1 < len(pool) and not (pool[e] == 0 and pool[e + 1] == 0):
            e += 2
        return pool[p:e].decode("utf-16-le", "replace")

    for i in range(count):
        eo = struct.unpack_from("<Q", data, 72 + i * 8)[0]
        guid = str(uuid.UUID(bytes_le=data[eo:eo + 16]))
        name = read(struct.unpack_from("<Q", data, eo + 24)[0])
        text = {}
        for l in range(nlang):
            code = CODES.get(langs[l])
            if code:
                text[code] = read(struct.unpack_from("<Q", data, eo + 40 + l * 8)[0])
        out[name] = {"guid": guid, "text": text}


def load(game_dir=DEFAULT_GAME):
    if zstandard is None:
        raise RuntimeError("the zstandard module is missing (pip install zstandard)")
    pak = os.path.join(game_dir, "re_chunk_000.pak")
    out = {}
    with open(pak, "rb") as f:
        magic, major, _minor, _feat, count, _fp = struct.unpack("<IBBhII", f.read(16))
        if magic != 0x414B504B or major != 4:
            raise RuntimeError("not a pak v4: " + pak)
        table = f.read(count * 48)
        entries = []
        for i in range(count):
            _lo, _hi, off, cs, ds, att, _chk = struct.unpack_from("<IIqqqqq", table, i * 48)
            if att & 0xFF00 == 0x1000:             # the attribute of the .msg files
                entries.append((off, cs, ds, att))
        entries.sort()
        d = zstandard.ZstdDecompressor()
        for off, cs, ds, att in entries:
            f.seek(off)
            raw = f.read(cs)
            comp = att & 0xF
            data = d.decompress(raw, max_output_size=ds) if comp == 2 else zlib.decompress(raw, -15) if comp == 1 else raw
            if data[4:8] == b"GMSG":
                _parse(data, out)
    return out
