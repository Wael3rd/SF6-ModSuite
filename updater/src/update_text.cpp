// SF6 Mod Suite updater: the texts of its message boxes (update_text.hpp).
#include "update_text.hpp"

namespace sf6up::text {

const char* tr(const std::string& lang, Word w) {
    int li = 0;
    for (int i = 0; i < kLangCount; ++i)
        if (lang == kLangs[i]) li = i;
    const char* t = kText[li][(int)w];
    return t && *t ? t : kText[0][(int)w];
}

std::string format(const char* tmpl, std::initializer_list<std::string> args) {
    std::string out;
    auto a = args.begin();
    for (const char* p = tmpl; *p; ++p) {
        if (*p == '%' && p[1] == 's') {
            if (a != args.end()) out += *a++;
            ++p;
        } else if (*p == '%' && p[1] == '%') {
            out += '%';
            ++p;
        } else {
            out += *p;
        }
    }
    return out;
}

std::string title(const std::string& lang) { return tr(lang, Word::Title); }

std::string offer(const std::string& lang, const Plan& plan) {
    std::string lines;
    for (auto& it : plan.items) {
        if (!lines.empty()) lines += "\n";
        lines += format(tr(lang, Word::Line), { it.pkg.name, it.from, it.pkg.version });
    }
    return format(tr(lang, Word::Offer), { plan.suite, lines, plan.suite });
}

std::string ignore(const std::string& lang) { return tr(lang, Word::Ignore); }

std::string done(const std::string& lang) { return tr(lang, Word::Done); }

std::string show_zip(const std::string& lang) { return tr(lang, Word::ShowZip); }

std::string downloading(const std::string& lang, const std::string& suite, int percent) {
    return format(tr(lang, Word::Downloading), { suite, std::to_string(percent < 0 ? 0 : percent > 100 ? 100 : percent) });
}

std::string installing(const std::string& lang, const std::string& suite) { return format(tr(lang, Word::Installing), { suite }); }

std::string failed(const std::string& lang, Fail why, const std::string& what) {
    Word w = Word::WhyPackage;
    switch (why) {
    case Fail::Download: w = Word::WhyDownload; break;
    case Fail::Check: w = Word::WhyCheck; break;
    case Fail::Blocked: w = Word::WhyBlocked; break;
    case Fail::Files: w = Word::WhyFiles; break;
    default: break;
    }
    return format(tr(lang, Word::Failed), { format(tr(lang, w), { what }) });
}

}  // namespace sf6up::text
