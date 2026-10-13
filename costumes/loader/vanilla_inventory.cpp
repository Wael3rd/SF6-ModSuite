#include "vanilla_inventory.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>

// ----------------------------------------------------------------------------
// What this reproduces (costumes/tools/build_vanilla_inventory.py, REasy-parser), per costume:
//   1. the scene esfNNN/esfNNNvCC|aCC.scn is the first file;
//   2. the scene's Resource fields (Prefab paths, meshes, materials, motion banks, chains...), in
//      instance order then field order, array elements included, then its UserData fields (arrays of
//      user data are not followed), each file once, the first mesh giving the model folder;
//   3. the textures of every material file (.mdf2) found in 2, material by material;
//   4. the files the costume's color variation list (ccvd) points to (the cmd files).
// A path becomes a row only when the game has it: the version suffix of its extension is the one
// known for that extension, else the first one (probing 0-499 and three large ones) that exists.
//
// The Python reads the scene through a type registry; here the few types a costume scene and a ccvd
// file hold are described by the table below (copied from the registry, SF6 build of 13/10), because
// the data of an instance cannot be walked without its layout. A type that is not in the table, or a
// layout that does not end exactly where the data ends (a game update changed a type), fails the
// costume instead of giving a list that misses files.
// ----------------------------------------------------------------------------

namespace {

// Field kinds: b = 1 byte, i = 4-byte value or instance reference, v = 16-byte vector (aligned to 16),
// s = string (u32 character count incl. NUL + UTF-16), R = Resource (a string), U = UserData
// (instance id of an entry in the userdata table, which names the file).
struct FieldDef { const char* name; char kind; bool array; };
struct TypeDef { uint32_t id; const char* name; const FieldDef* fields; int count; };

// via.GameObject
static const FieldDef GameObject_fields[] = {
    {"Name", 's', false},
    {"Tag", 's', false},
    {"UpdateSelf", 'b', false},
    {"DrawSelf", 'b', false},
    {"Timescale", 'i', false},
};
// via.Transform
static const FieldDef Transform_fields[] = {
    {"Position", 'v', false},
    {"Rotation", 'v', false},
    {"Scale", 'v', false},
    {"ParentJoint", 's', false},
    {"SameJointsContraint", 'b', false},
    {"AbsoluteScaling", 'b', false},
    {"JointSegmentScale", 'b', false},
    {"JointFastLockScene", 'b', false},
};
// via.Prefab
static const FieldDef Prefab_fields[] = {
    {"Standby", 'b', false},
    {"Path", 'R', false},
};
// app.battle.assets.FighterVisualHolder.PartsHolder
static const FieldDef PartsHolder_fields[] = {
    {"type", 'b', false},
    {"mesh", 'R', false},
    {"material", 'R', false},
    {"jointConstraintsData", 'U', false},
    {"chainData", 'U', false},
    {"aoGeometryData", 'U', false},
    {"havokClothData", 'U', false},
};
// app.battle.assets.FighterVisualHolder.JointConstreinsAsset
static const FieldDef JointConstreinsAsset_fields[] = {
    {"Body", 'R', true},
    {"Head", 'R', true},
    {"Hair", 'R', true},
};
// app.battle.assets.FighterVisualHolder.SettingData
static const FieldDef SettingData_fields[] = {
    {"fighterPrefab", 'i', false},
    {"projectilePrefab", 'i', false},
    {"soundPrefab_jpn", 'i', false},
    {"soundPrefab_eng", 'i', false},
    {"partsDatas", 'i', true},
    {"motionBank", 'R', false},
    {"motionBank_faces", 'R', true},
    {"motionBank_faces_eng", 'R', true},
    {"camMotionBank", 'R', false},
    {"jointMap", 'R', false},
    {"lookAt", 'U', false},
    {"smallDmgRes", 'U', false},
    {"ikBodyRig", 'R', false},
    {"vfxDatas", 'U', true},
    {"vfxExternVariables", 'R', false},
    {"afterImageMax", 'i', false},
    {"soundData", 'U', false},
    {"soundCostumeData", 'U', false},
    {"mUdAddVoiceData", 'U', false},
    {"soundFacial", 'U', false},
    {"costumeColorVariation", 'U', false},
    {"muscleControlData", 'U', false},
    {"wrinkleControlData", 'U', false},
    {"timelineContainer", 'U', false},
    {"winMessData", 'U', false},
    {"muscleControlTimeline", 'U', false},
    {"timelineMaterialParamData", 'U', false},
    {"modelParts", 'U', false},
    {"size_type", 'i', false},
    {"parts_disables", 'i', true},
    {"use_parts_disables_2p", 'b', false},
    {"parts_disables_2p", 'i', true},
    {"isDisableMirrorSymmetry2P", 'b', false},
    {"fighterSelectPrefabs", 'i', true},
    {"motionBank2P", 'R', false},
    {"isDisableMirrorSymmetryOp", 'b', false},
    {"vsDemoPrefabs", 'i', true},
    {"optionPrefab", 'i', false},
    {"FacialEmoteRttOverrideSetting", 'U', false},
    {"battleSettingPrefabs", 'i', true},
    {"arcadeContinuePrefab", 'i', true},
    {"windData", 'U', false},
    {"chainBody", 'R', false},
    {"chainHair", 'R', false},
    {"chainHead", 'R', false},
    {"chainWeapon", 'R', true},
    {"JointConstreinsAsset", 'i', false},
    {"uiFighterAsset", 'i', false},
    {"soundEquipFootType", 'i', false},
    {"soundOverwriteData", 'i', true},
    {"uniqueParamWind", 'i', true},
    {"facialBlendMax", 'i', false},
};
// app.battle.assets.FighterVisualHolder
static const FieldDef FighterVisualHolder_fields[] = {
    {"_Setting", 'i', false},
};
// app.CostumeColorData
static const FieldDef CostumeColorData_fields[] = {
    {"ColorId", 'i', false},
    {"Data", 'U', false},
};
// app.CostumeColorVariationData
static const FieldDef CostumeColorVariationData_fields[] = {
    {"IsFixed1P2P", 'b', false},
    {"Default", 'U', false},
    {"Colors", 'i', true},
};
// app.battle.assets.FighterActionHolder
static const FieldDef FighterActionHolder_fields[] = {
    {"STRUCT_m_setting_characterAsset", 'R', false},
    {"STRUCT_m_setting_aiData", 'U', false},
};
// via.AnimationCurve
static const FieldDef AnimationCurve_fields[] = {
    {"Keys", 'v', true},
    {"MinValue", 'i', false},
    {"MaxValue", 'i', false},
    {"LoopStartTime", 'i', false},
    {"LoopEndTime", 'i', false},
    {"LoopCount", 'i', false},
    {"LoopWrapNo", 'i', false},
};
// app.BattleWindMananger.UniqueParamWind
static const FieldDef UniqueParamWind_fields[] = {
    {"UniqueParamValue", 'i', false},
    {"ChainWindId", 'i', false},
    {"EnableChainGroupPresetID", 'b', false},
    {"ChainGroupPresetID", 'i', false},
    {"WindPower", 'i', false},
};
// app.battle.assets.FighterVisualHolder.SoundOverwriteData
static const FieldDef SoundOverwriteData_fields[] = {
    {"SourceFlowKey", 'i', false},
    {"OverwriteFlowKey", 'i', false},
};
static const TypeDef TYPES[] = {
    {0xC902A417, "via.GameObject", GameObject_fields, 5},
    {0xDFAC3046, "via.Transform", Transform_fields, 8},
    {0xF53C28F9, "via.Prefab", Prefab_fields, 2},
    {0x6EF1A0E7, "app.battle.assets.FighterVisualHolder.PartsHolder", PartsHolder_fields, 7},
    {0x5DCD2C84, "app.battle.assets.FighterVisualHolder.JointConstreinsAsset", JointConstreinsAsset_fields, 3},
    {0x3F832FE5, "app.battle.assets.FighterVisualHolder.SettingData", SettingData_fields, 52},
    {0x5D6CDB88, "app.battle.assets.FighterVisualHolder", FighterVisualHolder_fields, 1},
    {0x55E3F852, "app.CostumeColorData", CostumeColorData_fields, 2},
    {0xB73A5683, "app.CostumeColorVariationData", CostumeColorVariationData_fields, 3},
    {0xF858E0DC, "app.battle.assets.FighterActionHolder", FighterActionHolder_fields, 2},
    {0xEAB06D4B, "via.AnimationCurve", AnimationCurve_fields, 7},
    {0x1BFE226A, "app.BattleWindMananger.UniqueParamWind", UniqueParamWind_fields, 5},
    {0xBEBB1CE1, "app.battle.assets.FighterVisualHolder.SoundOverwriteData", SoundOverwriteData_fields, 2},
};

const TypeDef* find_type(uint32_t id) {
    for (const TypeDef& t : TYPES) if (t.id == id) return &t;
    return nullptr;
}

constexpr const char* ROOT = "natives/stm/";

// ----- small helpers -----------------------------------------------------------------------------

std::string lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; }

// Python str.strip()
std::string strip(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && is_space(s[a])) a++;
    while (b > a && is_space(s[b - 1])) b--;
    return s.substr(a, b - a);
}

// build_vanilla_inventory.clean_path: a path read from a scene (spaces, NULs and one leading '@' off)
std::string clean_path(const std::string& raw) {
    std::string s = strip(raw);
    while (!s.empty() && s.back() == '\0') s.pop_back();
    s = strip(s);
    if (!s.empty() && s[0] == '@') s.erase(0, 1);
    return s;
}

// "Product/Model/x/foo.mesh" -> "mesh"; empty when the file name has no dot
std::string get_extension(const std::string& path) {
    size_t sl = path.rfind('/');
    std::string base = (sl == std::string::npos) ? path : path.substr(sl + 1);
    size_t dot = base.rfind('.');
    return dot == std::string::npos ? std::string() : base.substr(dot + 1);
}

std::string slashed_lower(const std::string& p) {
    std::string s = p;
    for (auto& c : s) if (c == '\\') c = '/';
    return lower(s);
}

std::string to_pak_path(const std::string& rel, const std::string& suffix) {
    return ROOT + slashed_lower(rel) + "." + suffix;
}

bool in_pak(PakReader& pak, const std::string& path) {
    return pak.find(pak_path_hash(std::string_view(path))) != nullptr;
}

bool load_file(PakReader& pak, const std::string& path, std::vector<uint8_t>& out) {
    const PakEntry* e = pak.find(pak_path_hash(std::string_view(path)));
    if (!e) return false;
    out = pak.read(*e);
    return !out.empty();
}

// Version suffixes tried for an extension that is not in the cache: every value the Python tool tries
const std::vector<uint32_t>& candidate_suffixes() {
    static const std::vector<uint32_t> list = [] {
        std::set<uint32_t> s;
        for (uint32_t i = 0; i < 500; i++) s.insert(i);
        for (uint32_t v : {230110883u, 241101895u, 2109148288u}) s.insert(v);
        for (auto& kv : vanilla_known_suffixes()) s.insert((uint32_t)strtoul(kv.second.c_str(), nullptr, 10));
        return std::vector<uint32_t>(s.begin(), s.end());
    }();
    return list;
}

struct Resolved { std::string path; uint64_t hash; bool found; };

// build_vanilla_inventory.resolve_pak_path: a path as the scene writes it -> the pak path with its version
Resolved resolve(PakReader& pak, const std::string& rel, SuffixMap& suffixes) {
    const std::string ext = get_extension(rel);
    if (ext.empty()) return { rel, 0, false };
    auto it = suffixes.find(ext);
    if (it != suffixes.end()) {
        std::string p = to_pak_path(rel, it->second);
        uint64_t h = pak_path_hash(std::string_view(p));
        return { p, h, pak.find(h) != nullptr };
    }
    for (uint32_t cand : candidate_suffixes()) {
        std::string p = to_pak_path(rel, std::to_string(cand));
        uint64_t h = pak_path_hash(std::string_view(p));
        if (pak.find(h)) { suffixes[ext] = std::to_string(cand); return { p, h, true }; }
    }
    return { std::string(ROOT) + slashed_lower(rel), 0, false };   // not in the game: no row
}

// build_vanilla_inventory.classify_kind
std::string classify_kind(const std::string& ext, const std::string& field) {
    static const std::unordered_map<std::string, std::string> kinds = {
        {"mesh", "mesh"}, {"mdf2", "mdf2"}, {"tex", "tex"}, {"user", "user"}, {"scn", "scene"},
        {"chain", "chain"}, {"motbank", "motbank"}, {"mcambank", "mcambank"}, {"jmap", "jmap"},
        {"ikbodyrig", "ikbodyrig"}, {"msg", "msg"} };
    auto k = kinds.find(ext);
    std::string kind = k == kinds.end() ? ext : k->second;
    if (ext == "user") {
        const std::string fl = lower(field);
        auto has = [&](const char* s) { return fl.find(s) != std::string::npos; };
        std::string nounder = fl;
        nounder.erase(std::remove(nounder.begin(), nounder.end(), '_'), nounder.end());
        if (has("ccvd") || has("costumecolorvariation")) kind = "ccvd";
        else if (has("shape") || has("musclecontrol")) kind = "shape";
        else if (has("jcs") || has("jointconstraints")) kind = "jcs";
        else if (has("chain")) kind = "chain_user";
        else if (has("aogeo") || has("aogeometry")) kind = "aogeo";
        else if (has("havok")) kind = "havok";
        else if (has("sound")) kind = "sound";
        else if (nounder.find("cmd") != std::string::npos) kind = "cmd";
    }
    return kind;
}

// ----- RSZ walk ------------------------------------------------------------------------------------

struct Ref { std::string field, value; };
struct Refs { std::vector<Ref> resources, userdata; };

uint32_t rd32(const std::vector<uint8_t>& d, size_t o) { uint32_t v; memcpy(&v, &d[o], 4); return v; }
uint64_t rd64(const std::vector<uint8_t>& d, size_t o) { uint64_t v; memcpy(&v, &d[o], 8); return v; }

// Walks the instances of a scene (.scn) or user file (.user) like REasy-parser does and collects the
// Resource fields (array elements too) and the UserData fields (single ones: the Python tool does not
// follow arrays of them). Layout, checked on the game's files:
//   SCN header 64 bytes (RSZ block offset at 56) / USR header 48 bytes (RSZ block offset at 32)
//   RSZ header 48 bytes: "RSZ\0", version 16, object, instance and userdata counts, reserved,
//     instance table offset, data offset, userdata table offset (u64, from the RSZ start)
//   instance table (u32 type id, u32 crc), userdata table (u32 instance id, u32 hash, u64 offset of
//   the file name, a NUL-ended UTF-16 string), the instances' data one after the other: a field is
//   aligned to its own size, an array is a u32 count aligned to 4 then its elements; instances that
//   are userdata entries have no data here.
bool walk_rsz(const std::vector<uint8_t>& d, bool usr, Refs& out, std::string* err) {
    auto fail = [&](const char* why) { if (err) *err = why; return false; };
    const size_t hdr = usr ? 48 : 64;
    if (d.size() < hdr + 48 || memcmp(d.data(), usr ? "USR\0" : "SCN\0", 4) != 0) return fail("not a scene / user file");
    const uint64_t base = rd64(d, usr ? 32 : 56);
    if (base + 48 > d.size() || memcmp(&d[(size_t)base], "RSZ\0", 4) != 0) return fail("RSZ block not found");
    if (rd32(d, (size_t)base + 4) != 16) return fail("RSZ version is not 16");
    const uint32_t n_inst = rd32(d, (size_t)base + 12), n_ud = rd32(d, (size_t)base + 16);
    const uint64_t inst_off = rd64(d, (size_t)base + 24), data_off = rd64(d, (size_t)base + 32), ud_off = rd64(d, (size_t)base + 40);
    if (n_inst == 0 || base + inst_off + 8ull * n_inst > d.size()) return fail("instance table out of range");
    if (base + ud_off + 16ull * n_ud > d.size()) return fail("userdata table out of range");

    std::unordered_map<uint32_t, std::string> ud;   // instance id -> file the userdata entry names
    for (uint32_t i = 0; i < n_ud; i++) {
        size_t e = (size_t)(base + ud_off + 16ull * i);
        uint32_t id = rd32(d, e);
        uint64_t so = rd64(d, e + 8);
        if (!so) continue;
        std::string s;
        for (size_t k = (size_t)(base + so); k + 1 < d.size(); k += 2) {
            uint16_t c = (uint16_t)(d[k] | (d[k + 1] << 8));
            if (!c) break;
            if (c >= 0x80) return fail("userdata name is not ASCII");
            s += (char)c;
        }
        ud[id] = s;
    }

    size_t pos = (size_t)(base + data_off);
    auto align = [&](size_t a) { if (a > 1 && pos % a) pos += a - pos % a; };
    for (uint32_t idx = 1; idx < n_inst; idx++) {
        const uint32_t type_id = rd32(d, (size_t)(base + inst_off + 8ull * idx));
        if (type_id == 0 || ud.count(idx)) continue;
        const TypeDef* t = find_type(type_id);
        if (!t) {
            char b[96];
            snprintf(b, sizeof b, "instance of a type this code does not know (type id 0x%08X)", type_id);
            return fail(b);
        }
        for (int f = 0; f < t->count; f++) {
            const FieldDef& fd = t->fields[f];
            uint32_t count = 1;
            if (fd.array) {
                align(4);
                if (pos + 4 > d.size()) return fail("data ends inside an instance");
                count = rd32(d, pos);
                pos += 4;
                if (count > d.size()) return fail("array count out of range");
            }
            for (uint32_t k = 0; k < count; k++) {
                switch (fd.kind) {
                case 'b': pos += 1; break;
                case 'i': align(4); pos += 4; break;
                case 'v': align(16); pos += 16; break;
                case 'U': {
                    align(4);
                    if (pos + 4 > d.size()) return fail("data ends inside an instance");
                    uint32_t id = rd32(d, pos);
                    pos += 4;
                    if (!fd.array) {
                        auto u = ud.find(id);
                        std::string p = clean_path(u == ud.end() ? std::string() : u->second);
                        if (!p.empty()) out.userdata.push_back({ fd.name, p });
                    }
                    break;
                }
                case 's': case 'R': {
                    align(4);
                    if (pos + 4 > d.size()) return fail("data ends inside an instance");
                    uint32_t n = rd32(d, pos);
                    pos += 4;
                    if (n > (d.size() - pos) / 2) return fail("string runs past the data");
                    std::string s;
                    for (uint32_t c = 0; c < n; c++) {
                        uint16_t ch = (uint16_t)(d[pos + 2 * c] | (d[pos + 2 * c + 1] << 8));
                        if (ch >= 0x80) return fail("string is not ASCII");
                        s += (char)ch;
                    }
                    pos += 2ull * n;
                    if (fd.kind == 'R') {
                        std::string p = clean_path(s);
                        if (!p.empty()) out.resources.push_back({ fd.array ? std::string(fd.name) + "[]" : std::string(fd.name), p });
                    }
                    break;
                }
                }
                if (pos > d.size()) return fail("data ends inside an instance");
            }
        }
    }
    // The last instance ends the data (a few bytes of padding at most): otherwise a type changed
    if (d.size() - pos >= 16) return fail("instances do not end where the data ends (a type changed?)");
    return true;
}

// ----- fallback: strings that look like game paths ---------------------------------------------------

bool g_force_heuristic = false;

// "Product/Model/esf/esf001/001/body.mesh", "product/charparam/x.user"... : a path with folders and an
// extension, no spaces. Whether the game has the file decides the rest (a row exists only for files in the paks).
bool looks_like_path(const std::string& s) {
    if (s.size() < 6 || s.find('/') == std::string::npos) return false;
    for (unsigned char c : s) if (c <= 0x20 || c >= 0x7f) return false;
    const std::string ext = get_extension(s);
    if (ext.empty() || ext.size() > 12) return false;
    return std::all_of(ext.begin(), ext.end(), [](char c) { return isalnum((unsigned char)c) != 0; });
}

// The kind of a user file is told by the field of the scene that names it; without the layout, by its path
// (sound containers sit under sound/, joint and chain files under their own names...).
std::string field_from_name(const std::string& path) { return path; }

// UTF-16LE strings of the file that look like paths, in file order: what the typed walk would have found, and
// the user files of arrays it leaves out. User files are listed after the other files, as the typed walk does.
void heuristic_refs(const std::vector<uint8_t>& d, Refs& out) {
    out = Refs{};
    for (size_t i = 0; i + 1 < d.size(); i += 2) {
        size_t j = i;
        std::string s;
        while (j + 1 < d.size() && d[j + 1] == 0 && d[j] >= 0x20 && d[j] < 0x7f) { s += (char)d[j]; j += 2; }
        if (s.empty()) continue;
        std::string p = clean_path(s);
        if (looks_like_path(p)) {
            // The vfx files of a scene are an array of user data, which the typed walk does not follow.
            if (get_extension(p) == "user") {
                if (lower(p).find("product/vfx/") == std::string::npos) out.userdata.push_back({ field_from_name(p), p });
            }
            else out.resources.push_back({ "", p });
        }
        i = j;
    }
}

// ----- material files ------------------------------------------------------------------------------

// Texture paths of a material file (.mdf2.<version>), material by material, texture by texture.
// Header: "MDF\0", i16 version, i16 material count, i64 options; then the material headers (their size
// depends on the version), each pointing to its texture headers (32 bytes from version 13: offset of
// the texture type name, 2 hashes, offset of the texture path, 8 bytes).
bool mdf_textures(const std::vector<uint8_t>& b, int version, std::vector<std::string>& out) {
    if (b.size() < 16 || memcmp(b.data(), "MDF\0", 4) != 0 || version < 19) return false;
    int16_t count;
    memcpy(&count, &b[6], 2);
    if (count < 0) return false;
    struct Mat { int32_t tex_count; int64_t tex_off; };
    std::vector<Mat> mats;
    size_t p = 16;
    auto need = [&](size_t n) { return p + n <= b.size(); };
    for (int m = 0; m < count; m++) {
        Mat mat{};
        if (!need(8 + 4 + 12 + 8)) return false;
        p += 8 + 4 + 4 + 4;                                 // name offset, name hash, params size, param count
        memcpy(&mat.tex_count, &b[p], 4); p += 4;
        p += 8;                                             // gpbf name / data counts
        if (version >= 31) p += 4;                          // bake texture array size
        p += 4;                                             // shader type
        p += version >= 31 ? 8 : 4;                         // material flags
        if (version >= 51) p += 8;
        if (version >= 31) p += 4;                          // shader LOD count
        if (!need(8 + 8 + 8 + 8 + 8 + 8)) return false;
        p += 8;                                             // param header offset
        memcpy(&mat.tex_off, &b[p], 8); p += 8;
        p += 8 + 8 + 8;                                     // gpbf offset, params offset, mmtr path offset
        if (version >= 31) p += 8;                          // shader LOD redirects offset
        mats.push_back(mat);
    }
    for (const Mat& mat : mats) {
        if (!mat.tex_off) continue;
        const size_t hs = version >= 13 ? 32 : 24;
        // the Python tool takes a texture list that runs past the file for an empty one
        if (mat.tex_off < 0 || mat.tex_count < 0 || (uint64_t)mat.tex_off + hs * (uint64_t)mat.tex_count > b.size()) continue;
        for (int t = 0; t < mat.tex_count; t++) {
            const size_t h = (size_t)mat.tex_off + hs * (size_t)t;
            int64_t path_off;
            memcpy(&path_off, &b[h + 16], 8);
            if (!path_off) continue;
            if (path_off < 0 || (uint64_t)path_off >= b.size()) return false;
            std::string s;
            for (size_t k = (size_t)path_off; k + 1 < b.size(); k += 2) {
                uint16_t c = (uint16_t)(b[k] | (b[k + 1] << 8));
                if (!c) break;
                if (c >= 0x80) return false;
                s += (char)c;
            }
            s = clean_path(s);
            if (!s.empty()) out.push_back(s);
        }
    }
    return true;
}

bool is_digits(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

}  // namespace

// ----- public --------------------------------------------------------------------------------------

void vanilla_force_heuristic(bool on) { g_force_heuristic = on; }

SuffixMap vanilla_known_suffixes() {
    return { {"scn", "20"}, {"user", "2"}, {"mesh", "230110883"}, {"mdf2", "31"},
             {"tex", "241101895"}, {"chain", "52"}, {"msg", "21"} };
}

std::string vanilla_costume_label(int fighter, int costume_no) {
    char b[32];
    snprintf(b, sizeof b, "esf%03d %c%02d", fighter, costume_no < 100 ? 'v' : 'a', costume_no % 100);
    return b;
}

static std::string scene_pak_path(int fighter, int costume_no, const SuffixMap& suffixes) {
    auto sc = suffixes.find("scn");
    char b[160];
    snprintf(b, sizeof b, "natives/stm/product/charparam/esf/esf%03d/esf%03d%c%02d.scn.%s", fighter, fighter,
             costume_no < 100 ? 'v' : 'a', costume_no % 100, sc == suffixes.end() ? "20" : sc->second.c_str());
    return b;
}

std::vector<int> vanilla_costume_numbers(PakReader& pak, int fighter, const SuffixMap& suffixes) {
    std::vector<int> out;
    for (int base : {0, 100})
        for (int slot = 0; slot < 10; slot++)
            if (in_pak(pak, scene_pak_path(fighter, base + slot, suffixes))) out.push_back(base + slot);
    return out;
}

bool vanilla_costume_inventory(PakReader& pak, int fighter, int costume_no, SuffixMap& suffixes,
                               VanillaCostume& out, std::string* err) {
    auto fail = [&](const std::string& why) { if (err) *err = why; return false; };
    out = VanillaCostume{};
    out.fighter = fighter;
    char fd[16];
    snprintf(fd, sizeof fd, "esf%03d", fighter);
    out.fighter_dir = fd;
    out.costume_no = costume_no;
    out.scene = scene_pak_path(fighter, costume_no, suffixes);

    std::vector<uint8_t> scene;
    if (!load_file(pak, out.scene, scene)) return fail("scene not found in the paks");
    Refs refs;
    std::string why;
    if (g_force_heuristic || !walk_rsz(scene, false, refs, &why)) {
        out.fallback = g_force_heuristic ? "forced" : "scene: " + why;
        heuristic_refs(scene, refs);
    }

    std::set<std::string> seen{ out.scene };
    std::vector<std::string> mdf_rel, ccvd_rel;
    out.files.push_back({ pak_path_hash(std::string_view(out.scene)), "scene", out.scene });

    // Adds a file once, as a row when the game has it; false when it was already there.
    auto add = [&](const std::string& rel, const std::string& kind, Resolved* res) {
        Resolved r = resolve(pak, rel, suffixes);
        if (!seen.insert(r.path).second) return false;
        if (r.found) out.files.push_back({ r.hash, kind, r.path });
        if (res) *res = r;
        return true;
    };

    for (const Ref& r : refs.resources) {
        const std::string ext = get_extension(r.value);
        Resolved res;
        if (!add(r.value, classify_kind(ext, r.field), &res)) continue;
        if (ext == "mdf2") mdf_rel.push_back(r.value);
        // the model folder is the first mesh's, up to the part number directory
        if (ext == "mesh" && out.model_dir.empty()) {
            size_t s1 = res.path.rfind('/');
            size_t s2 = (s1 == std::string::npos || s1 == 0) ? std::string::npos : res.path.rfind('/', s1 - 1);
            if (s2 != std::string::npos) out.model_dir = res.path.substr(0, s2);
        }
    }
    for (const Ref& r : refs.userdata) {
        if (!add(r.value, classify_kind(get_extension(r.value), r.field), nullptr)) continue;
        if (lower(r.value).find("ccvd") != std::string::npos || lower(r.field).find("costumecolorvariation") != std::string::npos)
            ccvd_rel.push_back(r.value);
    }

    // textures of each material file
    for (const std::string& rel : mdf_rel) {
        auto sf = suffixes.find(get_extension(rel));
        const std::string suffix = sf == suffixes.end() ? "31" : sf->second;
        std::vector<uint8_t> mdf;
        if (!is_digits(suffix) || !load_file(pak, to_pak_path(rel, suffix), mdf)) continue;
        std::vector<std::string> tex;
        if (g_force_heuristic || !mdf_textures(mdf, atoi(suffix.c_str()), tex)) {
            // texture paths are UTF-16 strings of the file
            tex.clear();
            Refs tr;
            heuristic_refs(mdf, tr);
            for (auto& r : tr.resources) if (get_extension(r.value) == "tex") tex.push_back(r.value);
        }
        for (const std::string& t : tex) add(t, "tex", nullptr);
    }

    // files the color variation lists name
    for (const std::string& rel : ccvd_rel) {
        auto sf = suffixes.find(get_extension(rel));
        std::vector<uint8_t> cc;
        if (!load_file(pak, to_pak_path(rel, sf == suffixes.end() ? "2" : sf->second), cc)) continue;
        Refs cr;
        std::string cwhy;
        if (g_force_heuristic || !walk_rsz(cc, true, cr, &cwhy)) {
            if (out.fallback.empty()) out.fallback = g_force_heuristic ? "forced" : rel + ": " + cwhy;
            heuristic_refs(cc, cr);
            cr.userdata.insert(cr.userdata.end(), cr.resources.begin(), cr.resources.end());
        }
        for (const Ref& r : cr.userdata) add(r.value, "cmd", nullptr);
    }
    return true;
}

std::string vanilla_costume_row(const VanillaCostume& c) {
    return std::to_string(c.fighter) + "\t" + c.fighter_dir + "\t" + std::to_string(c.costume_no) + "\t"
         + c.model_dir + "\t" + c.scene;
}

std::vector<std::string> vanilla_file_rows(const VanillaCostume& c) {
    std::vector<std::string> rows;
    for (const auto& f : c.files) {
        char h[24];
        snprintf(h, sizeof h, "%016llX", (unsigned long long)f.hash);
        rows.push_back(std::string(h) + "\t" + std::to_string(c.fighter) + "\t" + std::to_string(c.costume_no)
                       + "\t" + f.kind + "\t" + f.path);
    }
    return rows;
}
