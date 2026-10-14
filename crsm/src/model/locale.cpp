#include "locale.hpp"

#include "luajson.hpp"
#include "lutil.hpp"

namespace model::locale {

const char* code_for_language(int v) {
    switch (v) {
        case 0: return "ja"; case 1: return "en"; case 2: return "fr"; case 3: return "it"; case 4: return "de";
        case 5: return "es"; case 6: return "ru"; case 7: return "pl"; case 8: return "nl"; case 9: return "pt";
        case 10: return "pt"; case 11: return "ko"; case 12: return "zh-Hant"; case 13: return "zh-Hans";
        case 21: return "ar"; case 32: return "es-419";
        default: return nullptr;
    }
}

const char* near_of(const std::string& code) {
    if (code == "es-419") return "es";
    if (code == "zh-Hant") return "zh-Hans";
    if (code == "pt") return "es";
    return nullptr;
}

void Locale::load(const json& file) {
    tables_ = model::luajson::is_table(file) && file.is_object() ? file : json::object();
    if (!tables_.contains("en") || !model::luajson::is_table(tables_["en"])) tables_["en"] = json::object();
}

std::optional<std::string> Locale::lookup(const std::string& key) const {
    auto find_in = [&](const std::string& code) -> std::optional<std::string> {
        auto t = tables_.find(code);
        if (t == tables_.end() || !t->is_object()) return std::nullopt;
        auto v = t->find(key);
        if (v == t->end() || !v->is_string()) return std::nullopt;
        return v->get<std::string>();
    };
    if (auto v = find_in(lang_)) return v;
    if (const char* n = near_of(lang_)) if (auto v = find_in(n)) return v;
    return find_in("en");
}

bool format(const std::string& tmpl, const std::vector<std::string>& args, std::string* out) {
    std::string r;
    size_t next = 0;
    for (size_t i = 0; i < tmpl.size(); ++i) {
        const char c = tmpl[i];
        if (c != '%') { r += c; continue; }
        if (++i >= tmpl.size()) return false;                    // "invalid conversion '%' to 'format'"
        const char s = tmpl[i];
        if (s == '%') { r += '%'; continue; }
        if (s == 's') {
            if (next >= args.size()) return false;               // "bad argument #n to 'format' (no value)"
            r += args[next++];
        } else if (s == 'd') {
            if (next >= args.size()) return false;
            const auto n = lutil::to_number(json(args[next++]));
            if (!n) return false;
            const auto k = lutil::to_int_key(*n);
            if (!k) return false;                                // "number has no integer representation"
            r += std::to_string(*k);
        } else {
            return false;
        }
    }
    *out = std::move(r);
    return true;
}

std::string Locale::L(const std::string& key, const std::vector<std::string>& args) const {
    const auto v = lookup(key);
    if (!v) return key;
    if (!args.empty()) {
        std::string s;
        if (format(*v, args, &s)) return s;
    }
    return *v;
}

std::string Locale::T(const std::string& key, const std::vector<std::string>& args) const {
    const std::string full = "rsm." + key;
    const std::string v = L(full, args);
    if (v != full) return v;
    return key;
}

}  // namespace model::locale
