# crsm/tests/tdb_methods.py -- the game's methods read OFFLINE from the type database (TDB v71) inside StreetFighter6.exe.
# Read-only (the exe is mapped, nothing is written); no game, no REFramework. For a hook "not found" after a game update:
# which signature the method has now, or where a name went.
#
#   python -I crsm\tests\tdb_methods.py <exe> type <full type name> [name filter]   the type's methods (and its parents')
#   python -I crsm\tests\tdb_methods.py <exe> find <method name>                     every method of that name, any type
#
# Layout: REFramework shared/sdk/RETypeDB.hpp (tdb71::TDB, REMethodDefinition, REMethodImpl, REParameterDef, ParamList,
# RETypeImpl) and RETypeDefinition.hpp (RETypeDefVersion71); SF6 = TDB 71, 19-bit type indexes. On disk the header's
# pointers are offsets from the TDB's start; method code addresses are filled at run time (encoded offsets are 0 here).
# Checked on the build of 13/10/2026 (1455453 methods): the indexes and signatures of the 55 hook targets of
# SF6_CRSMNative_data/codecheck.txt match.
import mmap
import struct
import sys


def mask_for(n):
    o = 1
    while o < n:
        o <<= 1
    return o - 1


class TDB:
    def __init__(self, path):
        f = open(path, 'rb')
        self.m = m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        i = 0
        while True:
            i = m.find(b'TDB\x00', i)
            if i < 0:
                raise SystemExit('no TDB v71 in ' + path)
            if struct.unpack_from('<I', m, i + 4)[0] == 71:
                break
            i += 1
        self.base = b = i
        h = struct.unpack_from('<22I', m, b)
        self.n_types, self.n_methods, self.n_params = h[3], h[4], h[12]
        self.n_strings, self.n_bytes = h[20], h[21]
        p = [b + x for x in struct.unpack_from('<18Q', m, b + 0x58)]
        self.types, self.types_impl, self.methods, self.methods_impl = p[1], p[2], p[3], p[4]
        self.params, self.strings, self.bytes = p[10], p[15], p[16]
        self.smask, self.bmask, self.pmask = mask_for(self.n_strings), mask_for(self.n_bytes), mask_for(self.n_params)
        self._names = {}

    def s(self, off):
        a = self.strings + (off & self.smask)
        return self.m[a:self.m.find(b'\x00', a)].decode('utf-8', 'replace')

    def type_raw(self, ti):                      # index, parent, declaring type, impl, first method
        a = self.types + ti * 0x48
        u0, u1 = struct.unpack_from('<QQ', self.m, a)
        u20, = struct.unpack_from('<Q', self.m, a + 0x20)
        return u0 & 0x7FFFF, (u0 >> 19) & 0x7FFFF, (u0 >> 38) & 0x7FFFF, (u1 >> 38) & 0x3FFFF, (u20 >> 22) & 0x3FFFFF

    def type_name(self, ti):
        if ti in self._names:
            return self._names[ti]
        _, _, decl, impl, _ = self.type_raw(ti)
        name_off, ns_off = struct.unpack_from('<ii', self.m, self.types_impl + impl * 0x30)
        name = self.s(name_off)
        if decl and decl != ti:
            full = self.type_name(decl) + '.' + name
        else:
            ns = self.s(ns_off)
            full = (ns + '.' if ns else '') + name
        self._names[ti] = full
        return full

    def type_methods(self, ti):
        _, _, _, impl, first = self.type_raw(ti)
        n, = struct.unpack_from('<H', self.m, self.types_impl + impl * 0x30 + 0x18)
        return range(first, first + n)

    def method_name(self, mi):
        w1, = struct.unpack_from('<I', self.m, self.methods + mi * 12 + 4)
        return self.s(struct.unpack_from('<I', self.m, self.methods_impl + (w1 & 0x7FFFF) * 12 + 8)[0])

    def method(self, mi):                        # (declaring type, name, [(param type, param name)], return type)
        w0, w1, _ = struct.unpack_from('<IIi', self.m, self.methods + mi * 12)
        pidx = ((w1 >> 19) << 13) | (w0 >> 19)
        pl = self.bytes + (pidx & self.bmask)
        n, _, ret = struct.unpack_from('<HHI', self.m, pl)
        ps = []
        for k in range(n):
            pa = self.params + (struct.unpack_from('<I', self.m, pl + 8 + 4 * k)[0] & self.pmask) * 12
            _, _, nm, tw = struct.unpack_from('<HHII', self.m, pa)
            ps.append((self.type_name(tw & 0x7FFFF), self.s(nm & 0x3FFFFFFF)))
        rt, = struct.unpack_from('<I', self.m, self.params + (ret & self.pmask) * 12 + 8)
        return w0 & 0x7FFFF, self.method_name(mi), ps, self.type_name(rt & 0x7FFFF)

    def find_type(self, want):
        for ti in range(self.n_types):
            try:
                if self.type_name(ti) == want:
                    return ti
            except Exception:
                pass
        return None


def line(t, mi):
    decl, name, ps, ret = t.method(mi)
    return '%8d  %s %s(%s)  [%s]' % (mi, ret, name, ', '.join(p for p, _ in ps), ', '.join(n for _, n in ps))


def main(argv):
    if len(argv) < 4 or argv[2] not in ('type', 'find'):
        raise SystemExit(__doc__ or 'usage: tdb_methods.py <exe> type <full type name> [filter] | find <method name>')
    t = TDB(argv[1])
    print('TDB v71 at file offset 0x%X: %d types, %d methods' % (t.base, t.n_types, t.n_methods))
    if argv[2] == 'find':
        for mi in range(t.n_methods):
            if t.method_name(mi) == argv[3]:
                print('%s: %s' % (t.type_name(t.method(mi)[0]), line(t, mi).strip()))
        return
    ti = t.find_type(argv[3])
    if ti is None:
        raise SystemExit('type not found: ' + argv[3])
    flt = argv[4].lower() if len(argv) > 4 else ''
    while ti:
        print('%s (%d methods)' % (t.type_name(ti), len(t.type_methods(ti))))
        for mi in t.type_methods(ti):
            if flt in t.method_name(mi).lower():
                print(line(t, mi))
        parent = t.type_raw(ti)[1]
        ti = parent if parent != ti else 0


if __name__ == '__main__':
    main(sys.argv)
