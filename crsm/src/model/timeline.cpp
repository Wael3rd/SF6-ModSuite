#include "timeline.hpp"

#include "lutil.hpp"

namespace model::timeline {
namespace {

using lutil::is_digit;
using lutil::is_space;

// The digit run starting at i: its end, and its value (saturated).
size_t digit_run(std::string_view s, size_t i, int64_t* value) {
    int64_t v = 0;
    size_t e = i;
    while (e < s.size() && is_digit(s[e])) {
        if (v < kMaxFrames) v = v * 10 + (s[e] - '0');
        ++e;
    }
    if (v > kMaxFrames) v = kMaxFrames;
    *value = v;
    return e;
}

// string.match(s, "(%d+)f") from `from`: the first digit run followed by 'f'. `at` = where the run starts.
bool find_digits_f(std::string_view s, size_t from, int64_t* frames, size_t* at, size_t* after_f) {
    size_t i = from;
    while (i < s.size()) {
        if (!is_digit(s[i])) { ++i; continue; }
        int64_t v;
        const size_t e = digit_run(s, i, &v);
        if (e < s.size() && s[e] == 'f') { *frames = v; *at = i; *after_f = e + 1; return true; }
        i = e;                                   // a run that is not followed by 'f' fails at every start inside it too
    }
    return false;
}

bool match_run(std::string_view line, bool anchored, Run* out) {
    size_t from = 0;
    for (;;) {
        int64_t frames; size_t at, after;
        if (!find_digits_f(line, from, &frames, &at, &after)) return false;
        if (anchored && at != 0) return false;
        // then " : " and at least one character
        if (line.substr(after, 3) == " : " && line.size() > after + 3) {
            if (out) { out->frames = frames; out->inputs = std::string(line.substr(after + 3)); }
            return true;
        }
        if (anchored) return false;
        from = after;                            // the next candidate
    }
}

}  // namespace

std::string decode_to_numpad(uint32_t val) {
    const bool u = (val & UP) != 0, d = (val & DOWN) != 0, l = (val & LEFT) != 0, r = (val & RIGHT) != 0;
    int num = 5;
    if (u) { num = l ? 7 : (r ? 9 : 8); }
    else if (d) { num = l ? 1 : (r ? 3 : 2); }
    else { num = l ? 4 : (r ? 6 : 5); }
    std::string s = std::to_string(num);
    if (val & LP) s += " + LP";
    if (val & MP) s += " + MP";
    if (val & HP) s += " + HP";
    if (val & LK) s += " + LK";
    if (val & MK) s += " + MK";
    if (val & HK) s += " + HK";
    return s;
}

uint32_t encode_from_numpad(std::string_view str) {
    uint32_t total = 0;
    size_t i = 0;
    while (i < str.size()) {
        while (i < str.size() && (is_space(str[i]) || str[i] == '+')) ++i;
        const size_t b = i;
        while (i < str.size() && !(is_space(str[i]) || str[i] == '+')) ++i;
        if (i == b) break;
        const std::string_view key = str.substr(b, i - b);
        if (key.size() == 1 && key[0] >= '1' && key[0] <= '9') {
            switch (key[0]) {
                case '1': total |= DOWN | LEFT; break;
                case '2': total |= DOWN; break;
                case '3': total |= DOWN | RIGHT; break;
                case '4': total |= LEFT; break;
                case '5': break;
                case '6': total |= RIGHT; break;
                case '7': total |= UP | LEFT; break;
                case '8': total |= UP; break;
                default:  total |= UP | RIGHT; break;     // '9'
            }
        } else if (key == "UP") total |= UP;
        else if (key == "DOWN") total |= DOWN;
        else if (key == "RIGHT") total |= RIGHT;
        else if (key == "LEFT") total |= LEFT;
        else if (key == "LP") total |= LP;
        else if (key == "MP") total |= MP;
        else if (key == "HP") total |= HP;
        else if (key == "LK") total |= LK;
        else if (key == "MK") total |= MK;
        else if (key == "HK") total |= HK;
    }
    return total;
}

std::vector<std::string> compress(const uint16_t* values, size_t frames) {
    std::vector<std::string> seq;
    if (!values || frames == 0) return seq;
    uint16_t cur = values[0];
    int64_t run = 1;
    for (size_t f = 1; f < frames; ++f) {
        if (values[f] == cur) { ++run; continue; }
        seq.push_back(std::to_string(run) + "f : " + decode_to_numpad(cur));
        cur = values[f];
        run = 1;
    }
    seq.push_back(std::to_string(run) + "f : " + decode_to_numpad(cur));
    return seq;
}

int64_t needed_frames(const std::vector<std::string>& lines) {
    int64_t total = 0;
    for (const std::string& e : lines) {
        int64_t d; size_t at, after;
        if (find_digits_f(e, 0, &d, &at, &after)) total += d;
        if (total > kMaxFrames) total = kMaxFrames;
    }
    return total;
}

int64_t entry_frames(const std::vector<std::string>& lines) {
    int64_t total = 0;
    for (const std::string& x : lines) {
        if (x.empty() || !is_digit(x[0])) continue;
        int64_t v;
        const size_t e = digit_run(x, 0, &v);
        if (e < x.size() && x[e] == 'f') total += v;
        if (total > kMaxFrames) total = kMaxFrames;
    }
    return total;
}

bool parse_run(std::string_view line, Run* out) { return match_run(line, false, out); }

Expanded expand(const std::vector<std::string>& lines) {
    Expanded r;
    for (const std::string& e : lines) {
        Run run;
        if (!match_run(e, false, &run)) { r.ok = false; return r; }
        const uint16_t val = (uint16_t)encode_from_numpad(run.inputs);
        for (int64_t k = 0; k < run.frames; ++k) r.values.push_back(val);
    }
    return r;
}

std::vector<uint16_t> expand_anchored(const std::vector<std::string>& lines) {
    std::vector<uint16_t> values;
    for (const std::string& e : lines) {
        Run run;
        if (!match_run(e, true, &run)) continue;
        const uint16_t val = (uint16_t)encode_from_numpad(run.inputs);
        for (int64_t k = 0; k < run.frames; ++k) values.push_back(val);
    }
    return values;
}

}  // namespace model::timeline
