#include "transfer.hpp"

#include "luajson.hpp"

namespace model::transfer {
namespace {

void nulls_to_tables(json& v) {
    if (v.is_null()) { v = json::object(); return; }
    if (v.is_object()) { for (auto it = v.begin(); it != v.end(); ++it) nulls_to_tables(it.value()); }
    else if (v.is_array()) { for (auto& e : v) nulls_to_tables(e); }
}

}  // namespace

json from_lua(const std::string& text) {
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded()) return j;
    nulls_to_tables(j);
    return j;
}

std::string to_lua(const json& v) {
    return model::luajson::encode(v).dump(-1, ' ', false, json::error_handler_t::replace);
}

bool file_text(const std::string& lua_text, std::string* lf_out) {
    const json j = json::parse(lua_text, nullptr, false);
    if (j.is_discarded()) return false;
    try { *lf_out = j.dump(4); } catch (const json::exception&) { return false; }
    return true;
}

}  // namespace model::transfer
