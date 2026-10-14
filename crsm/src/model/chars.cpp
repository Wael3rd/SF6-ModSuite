#include "chars.hpp"

namespace model::chars {
namespace {
const std::vector<Entry> kAll = {
    { 1, "Ryu" },      { 2, "Luke" },     { 3, "Kimberly" }, { 4, "Chun-Li" },
    { 5, "Manon" },    { 6, "Zangief" },  { 7, "JP" },       { 8, "Dhalsim" },
    { 9, "Cammy" },    { 10, "Ken" },     { 11, "Dee Jay" }, { 12, "Lily" },
    { 13, "A.K.I" },   { 14, "Rashid" },  { 15, "Blanka" },  { 16, "Juri" },
    { 17, "Marisa" },  { 18, "Guile" },   { 19, "Ed" },      { 20, "E. Honda" },
    { 21, "Jamie" },   { 22, "Akuma" },
    { 25, "Sagat" },   { 26, "M. Bison" }, { 27, "Terry" },  { 28, "Mai" },
    { 29, "Elena" },   { 30, "Viper" },   { 31, "Alex" },    { 32, "Ingrid" },
    { 33, "Yasmine" },
};
}  // namespace

const std::vector<Entry>& all() { return kAll; }

const char* name_of(int id) {
    for (const Entry& e : kAll) if (e.id == id) return e.name;
    return nullptr;
}

std::string get_char_name(int id) {
    if (const char* n = name_of(id)) return n;
    return "Unknown(" + std::to_string(id) + ")";
}

}  // namespace model::chars
