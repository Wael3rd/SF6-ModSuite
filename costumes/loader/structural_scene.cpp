#include "structural_scene.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>

// ----------------------------------------------------------------------------
// Layout (what REasy-parser reads and writes for these scenes, checked on the game's files):
//   0   SCN header, 64 bytes: "SCN\0", info/resource/folder/prefab/userdata counts (u32 x5),
//       folder_tbl, resource_tbl, prefab_tbl, userdata_tbl, data_offset (u64 x5)
//   64  folder infos (i32 object id, i32 parent folder) x folder_count
//   ... zero padding to 16: the three other tables and data_offset all point here (they are empty)
//   RSZ header, 48 bytes: "RSZ\0", version 16, object_count, instance_count, userdata_count,
//       reserved (u32 x6), instance_offset, data_offset, userdata_offset (u64 x3, from RSZ start)
//   object table (i32 instance id), instance infos (u32 type, u32 crc), zero padding to 16 (no
//   userdata, so userdata_offset == data_offset), then the instance data: every instance is a
//   via.Folder, written back to back in instance order (instance 0 is null and has no data).
// via.Folder data: Name, Tag (u32 char count incl. NUL + UTF-16), Update/Draw/Startup (u8),
//   ScenePath (string), UniversalOffset (24 bytes, align 8). Alignment is relative to the data start.
// ----------------------------------------------------------------------------

namespace {

// via.Folder's type id today; its CRC is taken from the scene itself (an update can change it alone)
constexpr uint32_t FOLDER_TYPE = 0xAEA2D9CC;
constexpr size_t SCN_HDR = 64, RSZ_HDR = 48;

uint32_t rd32(const std::vector<uint8_t>& d, size_t o) { uint32_t v; std::memcpy(&v, &d[o], 4); return v; }
uint64_t rd64(const std::vector<uint8_t>& d, size_t o) { uint64_t v; std::memcpy(&v, &d[o], 8); return v; }
void wr32(std::vector<uint8_t>& d, size_t o, uint32_t v) { std::memcpy(&d[o], &v, 4); }
void wr64(std::vector<uint8_t>& d, size_t o, uint64_t v) { std::memcpy(&d[o], &v, 8); }
void put32(std::vector<uint8_t>& d, uint32_t v) { d.resize(d.size() + 4); wr32(d, d.size() - 4, v); }
void pad(std::vector<uint8_t>& d, size_t align) { while (d.size() % align) d.push_back(0); }
size_t align_up(size_t v, size_t a) { return (v + a - 1) / a * a; }

struct Folder {
    std::u16string name, tag, path;
    uint8_t upd = 0, draw = 0, startup = 0;
    uint8_t offset[24] = {};
};

struct Scene {
    std::vector<int32_t> objs;                           // object table: instance id
    std::vector<std::pair<int32_t, int32_t>> folders;    // folder infos: object id, parent
    size_t inst_count = 0;
    uint32_t folder_crc = 0;                             // of every instance
    size_t rsz = 0, ioff = 0, data = 0;                  // RSZ header (file offset), instance table (from RSZ), data (file offset)
    std::vector<Folder> inst;                            // by instance id (0 unused)
};

bool fail(std::string* err, const char* why) { if (err) *err = why; return false; }

// Reads a string at 4-byte alignment relative to base; false if it runs past the end.
bool rd_string(const std::vector<uint8_t>& d, size_t base, size_t& p, std::u16string& s) {
    p = base + align_up(p - base, 4);
    if (p + 4 > d.size()) return false;
    uint32_t n = rd32(d, p); p += 4;
    if (n > (d.size() - p) / 2) return false;
    s.resize(n);
    if (n) std::memcpy(&s[0], &d[p], n * 2);
    p += n * 2;
    return true;
}

void wr_string(std::vector<uint8_t>& d, const std::u16string& s) {
    pad(d, 4);
    put32(d, (uint32_t)s.size());
    const uint8_t* b = reinterpret_cast<const uint8_t*>(s.data());
    d.insert(d.end(), b, b + s.size() * 2);
}

void wr_folder(std::vector<uint8_t>& d, const Folder& f) {
    wr_string(d, f.name);
    wr_string(d, f.tag);
    d.push_back(f.upd); d.push_back(f.draw); d.push_back(f.startup);
    wr_string(d, f.path);
    pad(d, 8);
    d.insert(d.end(), f.offset, f.offset + 24);
}

bool parse_scene(const std::vector<uint8_t>& d, Scene& s, std::string* err) {
    if (d.size() < SCN_HDR + RSZ_HDR || std::memcmp(d.data(), "SCN\0", 4) != 0) return fail(err, "not an SCN file");
    uint32_t info = rd32(d, 4), res = rd32(d, 8), nfold = rd32(d, 12), pref = rd32(d, 16), usr = rd32(d, 20);
    if (info || res || pref || usr) return fail(err, "scene has game objects/resources/prefabs/userdata");
    uint64_t ftbl = rd64(d, 24), rtbl = rd64(d, 32), ptbl = rd64(d, 40), utbl = rd64(d, 48), doff = rd64(d, 56);
    if (nfold > d.size() / 8 || ftbl != SCN_HDR) return fail(err, "unexpected folder table");
    uint64_t rsz = align_up(SCN_HDR + (size_t)nfold * 8, 16);
    if (rtbl != rsz || ptbl != rsz || utbl != rsz || doff != rsz) return fail(err, "unexpected table offsets");
    if (rsz + RSZ_HDR > d.size() || std::memcmp(&d[rsz], "RSZ\0", 4) != 0 || rd32(d, rsz + 4) != 16)
        return fail(err, "unexpected RSZ header");
    uint32_t nobj = rd32(d, rsz + 8), ninst = rd32(d, rsz + 12), nusr = rd32(d, rsz + 16);
    uint64_t ioff = rd64(d, rsz + 24), dataoff = rd64(d, rsz + 32), uoff = rd64(d, rsz + 40);
    if (nusr || nobj > d.size() / 4 || ninst > d.size() / 8 || ninst < 1) return fail(err, "unexpected RSZ counts");
    if (ioff != RSZ_HDR + (uint64_t)nobj * 4) return fail(err, "unexpected instance table offset");
    uint64_t want = align_up((size_t)ioff + (size_t)ninst * 8, 16);
    if (uoff != want || dataoff != want || rsz + want > d.size()) return fail(err, "unexpected data offset");
    s.rsz = (size_t)rsz; s.ioff = (size_t)ioff; s.data = (size_t)(rsz + dataoff); s.inst_count = ninst;
    for (uint32_t i = 0; i < nfold; i++) {
        int32_t id, par; std::memcpy(&id, &d[ftbl + i * 8], 4); std::memcpy(&par, &d[ftbl + i * 8 + 4], 4);
        if (id < 0 || (uint32_t)id >= nobj || par < -1 || par >= (int32_t)nfold) return fail(err, "bad folder info");
        s.folders.push_back({id, par});
    }
    for (uint32_t i = 0; i < nobj; i++) {
        int32_t v; std::memcpy(&v, &d[rsz + RSZ_HDR + i * 4], 4);
        if (v < 1 || (uint32_t)v >= ninst) return fail(err, "bad object table");
        s.objs.push_back(v);
    }
    for (uint32_t i = 0; i < ninst; i++) {
        size_t o = (size_t)rsz + (size_t)ioff + i * 8;
        uint32_t t = rd32(d, o), c = rd32(d, o + 4);
        if (i == 1) s.folder_crc = c;
        if (i == 0 ? (t || c) : (t != FOLDER_TYPE || c != s.folder_crc)) return fail(err, "instance is not a via.Folder");
    }
    s.inst.resize(ninst);
    size_t p = s.data;
    for (uint32_t i = 1; i < ninst; i++) {
        Folder& f = s.inst[i];
        if (!rd_string(d, s.data, p, f.name) || !rd_string(d, s.data, p, f.tag)) return fail(err, "truncated folder data");
        if (p + 3 > d.size()) return fail(err, "truncated folder data");
        f.upd = d[p]; f.draw = d[p + 1]; f.startup = d[p + 2]; p += 3;
        if (!rd_string(d, s.data, p, f.path)) return fail(err, "truncated folder data");
        p = s.data + align_up(p - s.data, 8);
        if (p + 24 > d.size()) return fail(err, "truncated folder data");
        std::memcpy(f.offset, &d[p], 24); p += 24;
    }
    if (p != d.size()) return fail(err, "unexpected bytes after the instance data");
    return true;
}

// "esf034v05" (a trailing NUL is tolerated) -> fighter dir + costume number
bool split_name(const std::u16string& u, std::string& fd, int& cno) {
    std::string n;
    for (char16_t c : u) { if (c == 0) break; if (c > 0x7F) return false; n += (char)c; }
    if (n.size() < 9 || n.size() > 10 || n.compare(0, 3, "esf") != 0 || n[6] != 'v') return false;
    for (size_t i = 3; i < n.size(); i++) if (i != 6 && (n[i] < '0' || n[i] > '9')) return false;
    cno = 0;
    for (size_t i = 7; i < n.size(); i++) cno = cno * 10 + (n[i] - '0');
    fd = n.substr(0, 6);
    return true;
}

std::u16string widen(const std::string& s) { return std::u16string(s.begin(), s.end()) + u'\0'; }

} // namespace

std::map<std::string, std::set<int>> scene_costume_folders(const std::vector<uint8_t>& scn) {
    std::map<std::string, std::set<int>> out;
    Scene s;
    if (!parse_scene(scn, s, nullptr)) return out;
    for (auto& fi : s.folders) {
        std::string fd; int c;
        if (split_name(s.inst[s.objs[fi.first]].name, fd, c)) out[fd].insert(c);
    }
    return out;
}

bool add_scene_folders(std::vector<uint8_t>& scn, const std::vector<std::string>& fighter_dirs,
                       bool prefer_v04, int first, int last, std::string* err) {
    Scene s;
    if (!parse_scene(scn, s, err)) return false;
    if (first < 0 || last < first || last > 1000) return fail(err, "bad costume range");

    // Template per fighter: the lowest rank wins (v04 first when prefer_v04, else v00 only); on a tie
    // the first folder in the table does.
    struct Tmpl { int rank; size_t folder; };
    std::map<std::string, Tmpl> tmpl;
    for (size_t i = 0; i < s.folders.size(); i++) {
        std::string fd; int c;
        if (!split_name(s.inst[s.objs[s.folders[i].first]].name, fd, c)) continue;
        int rank = prefer_v04 ? (c == 4 ? 0 : 1 + c) : (c == 0 ? 0 : -1);
        if (rank < 0) continue;
        auto it = tmpl.find(fd);
        if (it == tmpl.end() || rank < it->second.rank) tmpl[fd] = {rank, i};
    }

    std::vector<std::string> dirs(fighter_dirs);
    std::sort(dirs.begin(), dirs.end());
    dirs.erase(std::unique(dirs.begin(), dirs.end()), dirs.end());

    // New instances go at the end of the instance table, new objects at the end of the object table.
    std::vector<uint8_t> data(scn.begin() + s.data, scn.end());
    std::vector<int32_t> objs = s.objs;
    auto folders = s.folders;
    size_t ninst = s.inst_count;
    for (auto& fd : dirs) {
        auto it = tmpl.find(fd);
        if (it == tmpl.end()) continue;
        const auto tf = s.folders[it->second.folder];
        const Folder& src = s.inst[s.objs[tf.first]];
        for (int c = first; c < last; c++) {
            char nm[32];
            std::snprintf(nm, sizeof nm, "%sv%02d", fd.c_str(), c);
            Folder f = src;
            f.name = widen(nm);
            f.path = widen("Product/CharParam/esf/" + fd + "/" + nm + ".scn");
            wr_folder(data, f);
            objs.push_back((int32_t)ninst++);
            folders.push_back({(int32_t)objs.size() - 1, tf.second});
        }
    }

    // Rebuild: SCN header, folder infos, RSZ header, object table, instance infos, data. Each table
    // is padded to 16 like REasy does, and every offset is recomputed.
    std::vector<uint8_t> out(scn.begin(), scn.begin() + SCN_HDR);
    for (auto& fi : folders) { put32(out, (uint32_t)fi.first); put32(out, (uint32_t)fi.second); }
    pad(out, 16);
    size_t rsz = out.size();
    out.insert(out.end(), scn.begin() + s.rsz, scn.begin() + s.rsz + RSZ_HDR);
    for (int32_t o : objs) put32(out, (uint32_t)o);
    size_t ioff = out.size() - rsz;
    out.insert(out.end(), scn.begin() + s.rsz + s.ioff, scn.begin() + s.rsz + s.ioff + s.inst_count * 8);
    for (size_t i = s.inst_count; i < ninst; i++) { put32(out, FOLDER_TYPE); put32(out, s.folder_crc); }
    pad(out, 16);
    size_t doff = out.size() - rsz;
    out.insert(out.end(), data.begin(), data.end());

    wr32(out, 12, (uint32_t)folders.size());
    for (size_t o : {32, 40, 48, 56}) wr64(out, o, rsz);
    wr32(out, rsz + 8, (uint32_t)objs.size());
    wr32(out, rsz + 12, (uint32_t)ninst);
    wr64(out, rsz + 24, ioff);
    wr64(out, rsz + 32, doff);
    wr64(out, rsz + 40, doff);
    scn.swap(out);
    return true;
}
