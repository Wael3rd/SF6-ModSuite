#include "gx.hpp"

#include <windows.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "common.hpp"
#include "game/gm.hpp"
#include "perf.hpp"

namespace ui::gx {
namespace {

namespace gm = game::gm;

// RMT:732-742 TEXT_STYLE: the properties the Lua copied from the row's centre text onto a borrowed text
const char* const kTextStyle[] = {
    "FontSlot", "FontSize", "Color", "ColorTop", "ColorBottom", "ColorType", "ColorPreset", "ColorPresetTop",
    "ColorPresetBottom", "ColorPresetBlend", "ColorPresetBlendTop", "ColorPresetBlendBottom", "ColorPresetBlendRate",
    "ColorPresetBlendRateTop", "ColorPresetBlendRateBottom", "Bold", "Italic", "Kerning", "LetterSpace", "LineSpace",
    "GlowEnable", "GlowColor", "GlowColorPreset", "GlowColorPresetBlend", "GlowColorPresetBlendRate", "GlowStrength",
    "GlowBlurSize", "GlowInner", "GlowKnockout", "ShadowEnable", "ShadowColor", "ShadowColorPreset", "ShadowColorPresetBlend",
    "ShadowColorPresetBlendRate", "ShadowDistance", "ShadowDistanceZ", "ShadowRotation", "ShadowStrength", "ShadowBlurSize",
    "DistanceFieldEnable", "DistanceFieldType", "DistanceFieldColor", "DistanceFieldBaseColor", "DistanceFieldBaseColorThreshold",
    "DistanceFieldBorder", "DistanceFieldOutBorder", "DistanceFieldOutColor", "DistanceFieldOutSoftness", "DistanceFieldOutStrength",
    "DistanceFieldOutThickness", "DistanceFieldSoftness", "DistanceFieldStrength", "DistanceFieldThickness", "DistanceFieldDrawGlyph",
    "BlendType", "EffectAlphaType", "LinearAlphaBlend", "MaskType", "ControlPoint", "LetterAlignment", "PageAlignment",
    "RegionSize", "RasterSize", "AutoRegionFit", "AutoWrap", "ForceAutoWrap", "IconFontSize", "IconColorType", "Priority",
};

bool call0(MO* o, const char* method, reframework::InvokeRet* out) {
    if (!o) return false;
    return gm::call(o, method, {}, out);
}

}  // namespace

const SF6CoreAPI* api() { return cmn::core(); }
SF6CoreClient* cl() { return cmn::client(); }
MO* singleton(const char* name) { return gm::singleton(name); }

std::string str(void* s) {
    if (!s) return "";
    const int32_t len = *reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(s) + 0x10);   // System.String layout
    if (len <= 0 || len > 1 << 20) return "";
    const wchar_t* data = reinterpret_cast<const wchar_t*>(static_cast<const uint8_t*>(s) + 0x14);
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, data, len, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return "";
    std::string out((size_t)bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, data, len, out.data(), bytes, nullptr, nullptr);
    return out;
}

std::string msg(MO* text) {
    reframework::InvokeRet r{};
    if (!call0(text, "get_Message", &r) || !r.ptr) return "nil";
    return str(r.ptr);
}

std::string name(MO* o) {
    reframework::InvokeRet r{};
    if (!call0(o, "get_Name", &r) || !r.ptr) return "";
    return str(r.ptr);
}

bool is_type(MO* o, const char* full_name) {
    if (!o) return false;
    TD* want = gm::type(full_name);
    return want && o->get_type_definition() == want;
}

MO* first_child(MO* e) { return e ? gm::call_obj(e, "get_Child") : nullptr; }
MO* next(MO* e) { return e ? gm::call_obj(e, "get_Next") : nullptr; }
MO* parent(MO* e) { return e ? gm::call_obj(e, "get_Parent") : nullptr; }

MO* child(MO* e, const char* nm) {
    for (MO* c = first_child(e); c; c = next(c))
        if (name(c) == nm) return c;
    return nullptr;
}

MO* find_text(MO* e, const char* nm, int depth) {
    for (MO* c = first_child(e); c; c = next(c)) {
        if (name(c) == nm && is_type(c, "via.gui.Text")) return c;
        if (depth < 4) if (MO* f = find_text(c, nm, depth + 1)) return f;
    }
    return nullptr;
}

bool visible(MO* c) { bool v = false; return call_bool(c, "get_Visible", &v) && v; }
bool force_invisible(MO* c) { bool v = false; return call_bool(c, "get_ForceInvisible", &v) && v; }

bool size_of(MO* o, const char* getter, float* w, float* h) {
    reframework::InvokeRet r{};
    if (!call0(o, getter, &r)) return false;
    std::memcpy(w, r.bytes.data(), 4);
    if (h) std::memcpy(h, r.bytes.data() + 4, 4);
    return true;
}

bool vec3_of(MO* o, const char* getter, float out[3]) {
    reframework::InvokeRet r{};
    if (!call0(o, getter, &r)) return false;
    std::memcpy(out, r.bytes.data(), 12);
    return true;
}

bool color_of(MO* o, const char* getter, uint32_t* rgba) {
    reframework::InvokeRet r{};
    if (!call0(o, getter, &r)) return false;
    std::memcpy(rgba, r.bytes.data(), 4);
    return true;
}

bool call_i32(MO* o, const char* method, int32_t* out) { return o && gm::call_i32(o, method, out); }
bool call_bool(MO* o, const char* method, bool* out) { return o && gm::call_bool(o, method, out); }
MO* call_obj(MO* o, const char* method) { return o ? gm::call_obj(o, method) : nullptr; }
bool get_i64(MO* o, const char* field, int64_t* v) { return o && gm::get_int(o, field, v); }
MO* ref(MO* o, const char* field) { return o ? gm::ref(o, field) : nullptr; }

// ---- writes --------------------------------------------------------------------------------------------------------------
void set_text(MO* t, const std::string& s) { if (t && api()) perf::counted(api())->ui_set_text(cl(), t, s.c_str()); }
void set_visible(MO* c, bool on) { if (c && api()) perf::counted(api())->ui_set_visible(cl(), c, on ? 1 : 0); }
void set_force_invisible(MO* c, bool on) { if (c && api()) perf::counted(api())->ui_set_force_invisible(cl(), c, on ? 1 : 0); }
void set_pos(MO* c, float x, float y, float z) { if (c && api()) perf::counted(api())->ui_set_position(cl(), c, x, y, z); }
void set_font(MO* t, float w, float h) { if (t && api()) perf::counted(api())->ui_set_font_size(cl(), t, w, h); }
void set_region(MO* t, float w, float h) { if (t && api()) perf::counted(api())->ui_set_region_size(cl(), t, w, h); }
void set_size(MO* c, float w, float h) { if (c && api()) perf::counted(api())->ui_set_size(cl(), c, w, h); }
void set_block_input(MO* agent, bool on) { if (agent && api()) perf::counted(api())->ui_set_block_input(cl(), agent, on ? 1 : 0); }

SF6CoreArg I(int64_t v) { SF6CoreArg a{}; a.type = SF6CORE_ARG_I64; a.v.i = v; return a; }
SF6CoreArg B(bool v) { SF6CoreArg a{}; a.type = SF6CORE_ARG_BOOL; a.v.i = v ? 1 : 0; return a; }
SF6CoreArg P(const void* p) { SF6CoreArg a{}; a.type = SF6CORE_ARG_PTR; a.v.p = const_cast<void*>(p); return a; }
SF6CoreArg S(const char* s) { SF6CoreArg a{}; a.type = SF6CORE_ARG_STR; a.v.s = s; return a; }
SF6CoreArg F(double v) { SF6CoreArg a{}; a.type = SF6CORE_ARG_F64; a.v.f = v; return a; }

bool invoke(MO* o, const char* sig, std::initializer_list<SF6CoreArg> args, SF6CoreArg* out) {
    if (!o || !api()) return false;
    SF6CoreArg a[8]{};
    uint32_t n = 0;
    for (const SF6CoreArg& x : args) if (n < 8) a[n++] = x;
    return perf::counted(api())->obj_invoke(cl(), o, sig, n ? a : nullptr, n, out) == SF6CORE_OK;
}

void set_field_i64(MO* o, const char* field, int64_t v) {
    if (!o || !api()) return;
    int64_t cur = 0;
    if (gm::get_int(o, field, &cur) && cur == v) return;
    perf::counted(api())->obj_set_i64(cl(), o, field, v);
}

void set_field_str(MO* o, const char* field, const char* s) { if (o && api()) perf::counted(api())->obj_set_string(cl(), o, field, s ? s : ""); }

void set_color(MO* t, const char* setter, uint32_t rgba) {
    if (!t) return;
    const uint32_t v = rgba;                                    // via.Color { rgba }: a value type, passed by address
    invoke(t, setter, { P(&v) });
}

// The setter is called by its FULL signature ("set_FontSize(via.Size)"): the core's exact lookup. (Until 11/10 the
// signature was "set_X(v)", left to the core's name-and-arity search, which reads at most 64 methods per type and so
// never reached via.gui.Text's setters: nothing was copied, the borrowed name kept the donor's size, font and glow.)
//
// The plan: what copy_style looked up for every property at every copy (its getter, its setter by name and arity, the
// setter's parameter and full signature, the getter's return type), built from temporary strings each time (2 x 69
// lookups per borrowed text, 16 borrowed texts at each menu opening: the per-thread lookup cache keyed by the name's
// address never hit them), is resolved once per (source type, target type) and kept for the process. The copy itself is
// unchanged: the same getters, the same setter calls through the core (obj_invoke), in the same order, same arguments.
namespace {

struct StyleProp {
    enum Kind : uint8_t { kFloat, kBool, kInt, kStruct, kRef };
    API::Method* getter = nullptr;
    std::string set_sig;                        // "set_X(<parameter type>)"
    const char* fail_pre = nullptr;             // found before the get: "no getter / setter", "setter parameter"
    const char* fail_post = nullptr;            // found after the get: "return type", "struct size"
    Kind kind = kRef;
    uint32_t size = 0;
};
constexpr size_t kStyleCount = sizeof(kTextStyle) / sizeof(kTextStyle[0]);
struct StylePlan {
    TD* src = nullptr;
    TD* dst = nullptr;
    std::array<StyleProp, kStyleCount> props{};  // resolved in place: an entry never moves
    size_t resolved = 0;
};
std::mutex g_style_mx;                          // the plans (the menu threads' hooks, the frame callback's warm-up)
std::vector<StylePlan*> g_plans;                // a handful at most (via.gui.Text -> via.gui.Text in practice), kept

StyleProp resolve_prop(TD* td, TD* dtd, const char* pn) {
    StyleProp p;
    const std::string g = std::string("get_") + pn, s = std::string("set_") + pn;
    API::Method* getter = gm::method(td, g.c_str());
    API::Method* setter = gm::method_n(dtd, s.c_str(), 1);
    if (!getter || !setter || getter->get_num_params() != 0) { p.fail_pre = "no getter / setter"; return p; }
    const auto params = setter->get_params();
    API::TypeDefinition* pt = params.size() == 1 ? reinterpret_cast<API::TypeDefinition*>(params[0].t) : nullptr;
    if (!pt) { p.fail_pre = "setter parameter"; return p; }
    p.getter = getter;
    p.set_sig = s + "(" + pt->get_full_name() + ")";
    TD* rt = getter->get_return_type();
    if (!rt) { p.fail_post = "return type"; return p; }
    if (!rt->is_valuetype()) { p.kind = StyleProp::kRef; return p; }
    const std::string full = rt->get_full_name();
    uint32_t sz = rt->get_valuetype_size();
    if (full == "System.Single" || full == "System.Double") p.kind = StyleProp::kFloat;   // the invoke wrapper hands a float back as a double
    else if (full == "System.Boolean") p.kind = StyleProp::kBool;
    else if (rt->is_enum() || rt->is_primitive()) { p.kind = StyleProp::kInt; p.size = sz; }
    else {
        if (sz == 0) {                                                            // the engine's small value types
            if (full == "via.Color") sz = 4;
            else if (full == "via.Size" || full == "via.Float2" || full == "via.vec2") sz = 8;
            else if (full == "via.Float3" || full == "via.vec3") sz = 12;
            else if (full == "via.Float4" || full == "via.vec4") sz = 16;
        }
        if (sz == 0 || sz > 128) { p.fail_post = "struct size"; return p; }
        p.kind = StyleProp::kStruct;
        p.size = sz;
    }
    return p;
}

StylePlan* plan_of(TD* td, TD* dtd) {           // under g_style_mx
    for (StylePlan* pl : g_plans) if (pl->src == td && pl->dst == dtd) return pl;
    StylePlan* pl = new StylePlan();
    pl->src = td;
    pl->dst = dtd;
    g_plans.push_back(pl);
    return pl;
}

void resolve_upto(StylePlan& pl, size_t upto) { // under g_style_mx
    for (; pl.resolved < upto && pl.resolved < kStyleCount; ++pl.resolved)
        pl.props[pl.resolved] = resolve_prop(pl.src, pl.dst, kTextStyle[pl.resolved]);
}

}  // namespace

struct StyleRead {                              // the source's values, read once (StyleShare)
    const StylePlan* plan = nullptr;
    std::array<bool, kStyleCount> got{};
    std::array<reframework::InvokeRet, kStyleCount> ret{};
};
StyleShare::~StyleShare() { delete read; }

int copy_style(MO* dst, MO* src, std::string* failed, StyleShare* shared) {
    if (!dst || !src) return 0;
    TD* td = src->get_type_definition();
    TD* dtd = dst->get_type_definition();
    if (!td || !dtd) return 0;
    const StylePlan* pl = nullptr;
    {
        std::lock_guard lk(g_style_mx);
        StylePlan* p = plan_of(td, dtd);
        resolve_upto(*p, kStyleCount);
        pl = p;
    }
    int written = 0;
    auto fail = [&](const char* pn, const char* why) {
        if (!failed) return;
        if (!failed->empty()) *failed += ", ";
        *failed += std::string(pn) + "(" + why + ")";
    };
    // the source's values: its getters (the target's writes below never change the source), or what the first target of
    // this pass read from the same source
    StyleRead* rd = nullptr;
    std::unique_ptr<StyleRead> local;
    if (shared && shared->src == src && shared->read && shared->read->plan == pl) {
        rd = shared->read;
    } else {
        rd = new StyleRead();
        if (shared) { delete shared->read; shared->read = rd; shared->src = src; }
        else local.reset(rd);
        rd->plan = pl;
        for (size_t i = 0; i < kStyleCount; ++i) {
            const StyleProp& p = pl->props[i];
            if (!p.fail_pre) rd->got[i] = gm::call(src, p.getter, {}, &rd->ret[i]);
        }
    }
    for (size_t i = 0; i < kStyleCount; ++i) {
        const char* pn = kTextStyle[i];
        const StyleProp& p = pl->props[i];
        if (p.fail_pre) { fail(pn, p.fail_pre); continue; }
        if (!rd->got[i]) { fail(pn, "get"); continue; }
        if (p.fail_post) { fail(pn, p.fail_post); continue; }
        const reframework::InvokeRet& r = rd->ret[i];
        SF6CoreArg arg{};
        std::array<uint8_t, 128> buf{};
        switch (p.kind) {
            case StyleProp::kFloat: arg = F(r.d); break;
            case StyleProp::kBool: arg = B(r.byte != 0); break;
            case StyleProp::kInt: {
                uint64_t v = r.qword;
                if (p.size == 1) v &= 0xFF; else if (p.size == 2) v &= 0xFFFF; else if (p.size == 4) v &= 0xFFFFFFFFull;
                arg = I((int64_t)v);
                break;
            }
            case StyleProp::kStruct:                                              // a struct: by address
                std::memcpy(buf.data(), r.bytes.data(), p.size);
                arg = P(buf.data());
                break;
            case StyleProp::kRef: arg = P(r.ptr); break;
        }
        if (invoke(dst, p.set_sig.c_str(), { arg })) ++written;
        else fail(pn, "set");
    }
    return written;
}

// ---- the warm-up of the first menu opening (frame callback, menu closed) ---------------------------------------------------
// The first opening of a game session paid every first lookup at once (perf log 13/10: 45 ms in one frame, 18.8 ms in the
// first Record row): REFramework scans a type's methods, building their signatures, the first time a full signature is
// asked of it, and the core asks REFramework for each setter's full signature at its first obj_invoke of it. The same
// lookups are asked here beforehand, a few per frame: the plan of copy_style (its 69 getters and setters), REFramework's
// lookup of each full signature the core will be asked for (its cache then serves the core's), the plain getters the
// rows read. Lookups only: nothing is called, nothing written.
namespace {
const std::pair<const char*, const char*> kWarmSigs[] = {
    { "via.gui.Text", "set_DisableMessageTag(System.Boolean)" },
    { "via.gui.Text", "set_AutoRegionFit(System.Boolean)" },
    { "via.gui.Control", "addChild(via.gui.PlayObject)" },
    { "via.gui.Panel", "addChild(via.gui.PlayObject)" },
    { "via.gui.Control", "set_PlayState(System.String)" },
    { "via.gui.Panel", "set_PlayState(System.String)" },
    { "app.training.UIFlowTrainingMenu.Param", "UpdateRecordSlotItem(app.UIPartsGroup, System.Int32)" },
    { "app.training.UIFlowTrainingMenu.Param", "UpdatePlaySlotItem(app.UIPartsGroup, System.Int32)" },
    { "app.training.UIFlowTrainingMenu.Param", "UpdateReversalItem(app.UIPartsGroup, System.Int32)" },
};
const std::pair<const char*, const char*> kWarmGetters[] = {
    { "via.gui.Text", "get_Message" }, { "via.gui.Text", "get_Name" }, { "via.gui.Text", "get_Next" },
    { "via.gui.Text", "get_Child" }, { "via.gui.Text", "get_Parent" }, { "via.gui.Text", "get_Visible" },
    { "via.gui.Text", "get_ForceInvisible" }, { "via.gui.Text", "get_DisplaySize" }, { "via.gui.Text", "get_Position" },
    { "via.gui.Control", "get_Name" }, { "via.gui.Control", "get_Child" }, { "via.gui.Control", "get_Next" },
    { "via.gui.Control", "get_Parent" }, { "via.gui.Control", "get_Visible" }, { "via.gui.Control", "get_PlayState" },
    { "via.gui.Control", "getPlayStateNamesCount" }, { "via.gui.Control", "getPlayStateNames" },
    { "via.gui.Panel", "get_Name" }, { "via.gui.Panel", "get_Child" }, { "via.gui.Panel", "get_Next" },
    { "via.gui.Panel", "get_Parent" }, { "via.gui.Panel", "get_Visible" }, { "via.gui.Panel", "get_Position" },
    { "app.UIPartsGroup", "get_Control" },
    { "app.training.UIFlowTrainingMenu.Param", "get_SelectChildIndex" },
    { "app.training.UIFlowTrainingMenu.Param", "get_CurrentData" },
};
constexpr size_t kWarmSigCount = sizeof(kWarmSigs) / sizeof(kWarmSigs[0]);
constexpr size_t kWarmGetterCount = sizeof(kWarmGetters) / sizeof(kWarmGetters[0]);
size_t g_warm_step = 0;                         // the plan's properties, then the signatures, then the getters
bool g_warm_done = false;
uint64_t g_warm_frames = 0;
double g_warm_ms = 0, g_warm_worst = 0;

double since_ms(const LARGE_INTEGER& t0) {
    static const double kMsPerTick = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return 1000.0 / (double)f.QuadPart; }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - t0.QuadPart) * kMsPerTick;
}
}  // namespace

bool warm_step(double budget_ms) {
    if (g_warm_done) return true;
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    ++g_warm_frames;
    TD* text = gm::type("via.gui.Text");
    if (!text) { g_warm_done = true; return true; }
    do {
        const size_t i = g_warm_step;
        if (i < kStyleCount) {                   // one property of the plan, and REFramework's lookup of its setter's signature
            std::string sig;
            {
                std::lock_guard lk(g_style_mx);
                StylePlan* pl = plan_of(text, text);
                resolve_upto(*pl, i + 1);
                sig = pl->props[i].set_sig;
            }
            if (!sig.empty()) text->find_method(sig);
        } else if (i < kStyleCount + kWarmSigCount) {
            const auto& [tn, sig] = kWarmSigs[i - kStyleCount];
            if (TD* t = gm::type(tn)) t->find_method(sig);
        } else if (i < kStyleCount + kWarmSigCount + kWarmGetterCount) {
            const auto& [tn, nm] = kWarmGetters[i - kStyleCount - kWarmSigCount];
            if (TD* t = gm::type(tn)) gm::method(t, nm);
        } else {
            g_warm_done = true;
            break;
        }
        ++g_warm_step;
    } while (since_ms(t0) < budget_ms);
    const double ms = since_ms(t0);
    g_warm_ms += ms;
    if (ms > g_warm_worst) g_warm_worst = ms;
    if (g_warm_done)
        cmn::journal("ui: menu warm-up done: %zu lookups in %llu frames, %.2f ms (worst frame %.2f ms)", g_warm_step,
                     (unsigned long long)g_warm_frames, g_warm_ms, g_warm_worst);
    return g_warm_done;
}

std::string warm_status() {
    char b[112];
    std::snprintf(b, sizeof(b), "warm-up %s (%zu lookups, %llu frames, %.2f ms, worst %.2f)", g_warm_done ? "done" : "pending",
                  g_warm_step, (unsigned long long)g_warm_frames, g_warm_ms, g_warm_worst);
    return b;
}

// ---- the probe dump's formatting (read-only) ---------------------------------------------------------------------------
namespace {
bool read_raw(const void* p, void* out, size_t n) {
    __try { std::memcpy(out, p, n); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}
}  // namespace

std::string format_value(TD* t, const void* p) {
    if (!t || !p) return "?";
    char b[128];
    const std::string full = t->get_full_name();
    if (!t->is_valuetype()) {
        void* v = nullptr;
        if (!read_raw(p, &v, sizeof(v))) return "?";
        if (!v) return "null";
        if (!gm::is_managed(v)) { std::snprintf(b, sizeof(b), "%p(?)", v); return b; }
        MO* o = static_cast<MO*>(v);
        TD* ot = o->get_type_definition();
        const std::string on = ot ? ot->get_full_name() : std::string("?");
        if (on == "System.String") return "\"" + str(v).substr(0, 48) + "\"";
        if (!on.empty() && on.back() == ']') { std::snprintf(b, sizeof(b), "%s[%d]@%p", on.c_str(), gm::array_len(o), v); return b; }
        std::snprintf(b, sizeof(b), "%s@%p", on.c_str(), v);
        return b;
    }
    uint8_t raw[16]{};
    uint32_t sz = t->get_valuetype_size();
    if (full == "via.Color") sz = 4;
    else if (full == "via.Size" || full == "via.Float2" || full == "via.vec2") sz = 8;
    else if (full == "via.Float3" || full == "via.vec3") sz = 12;
    else if (full == "via.Float4" || full == "via.vec4" || full == "via.Rect") sz = 16;
    if (sz == 0 || sz > sizeof(raw) || !read_raw(p, raw, sz)) { std::snprintf(b, sizeof(b), "<%s %u>", full.c_str(), sz); return b; }
    float f[4]{};
    std::memcpy(f, raw, sizeof(f));
    if (full == "System.Boolean") return raw[0] ? "true" : "false";
    if (full == "System.Single") { std::snprintf(b, sizeof(b), "%g", f[0]); return b; }
    if (full == "System.Double") { double d = 0; std::memcpy(&d, raw, 8); std::snprintf(b, sizeof(b), "%g", d); return b; }
    if (full == "via.Color") { uint32_t c = 0; std::memcpy(&c, raw, 4); std::snprintf(b, sizeof(b), "#%08X", c); return b; }
    if (sz == 8 && full.rfind("via.", 0) == 0) { std::snprintf(b, sizeof(b), "(%g,%g)", f[0], f[1]); return b; }
    if (sz == 12 && full.rfind("via.", 0) == 0) { std::snprintf(b, sizeof(b), "(%g,%g,%g)", f[0], f[1], f[2]); return b; }
    if (sz == 16 && full.rfind("via.", 0) == 0) { std::snprintf(b, sizeof(b), "(%g,%g,%g,%g)", f[0], f[1], f[2], f[3]); return b; }
    if (t->is_enum() || t->is_primitive() || sz <= 8) {
        int64_t v = 0;
        if (sz == 1) v = (int8_t)raw[0];
        else if (sz == 2) { int16_t x; std::memcpy(&x, raw, 2); v = x; }
        else if (sz == 4) { int32_t x; std::memcpy(&x, raw, 4); v = x; }
        else std::memcpy(&v, raw, 8);
        std::snprintf(b, sizeof(b), "%lld", (long long)v);
        return b;
    }
    std::string hex = "<" + full + ":";
    for (uint32_t i = 0; i < sz; ++i) { std::snprintf(b, sizeof(b), "%02X", raw[i]); hex += b; }
    return hex + ">";
}

std::string describe_text(MO* t) {
    if (!t || !gm::is_managed(t)) return "(none)";
    TD* td = t->get_type_definition();
    std::string out = (td ? td->get_full_name() : std::string("?")) + " " + name(t) + ":";
    static const char* const kExtra[] = { "Message", "Visible", "ForceInvisible", "Position", "Scale", "GlobalPosition", "GlobalScale",
                                          "DisplaySize", "Size", "MessageId" };
    auto one = [&](const char* pn) {
        const std::string g = std::string("get_") + pn;
        API::Method* m = gm::method(td, g.c_str());
        if (!m || m->get_num_params() != 0) return;
        reframework::InvokeRet r{};
        if (!gm::call(t, m, {}, &r)) { out += std::string(" ") + pn + "=!"; return; }
        TD* rt = m->get_return_type();
        std::string v;
        if (rt && rt->get_full_name() == "System.Single") { char b[32]; std::snprintf(b, sizeof(b), "%g", r.d); v = b; }   // (a double here)
        else if (rt && !rt->is_valuetype()) { void* q = r.ptr; v = format_value(rt, &q); }
        else v = format_value(rt, r.bytes.data());
        out += std::string(" ") + pn + "=" + v;
    };
    for (const char* pn : kTextStyle) one(pn);
    for (const char* pn : kExtra) one(pn);
    out += " | path:";
    MO* p = t;
    for (int i = 0; i < 6 && p; ++i) { p = parent(p); if (p) out += " < " + name(p); }
    return out;
}

// ---- finders ----------------------------------------------------------------------------------------------------------
MO* find_param() {
    static TD* want = nullptr;
    if (!want) want = gm::type("app.training.UIFlowTrainingMenu.Param");
    MO* hs = gm::ref(gm::singleton("app.UIFlowManager"), "_Handles");
    if (!hs || !want) return nullptr;
    const int32_t n = gm::list_count(hs);
    for (int32_t i = 0; i < n; ++i) {
        MO* q = gm::ref(gm::list_item(hs, i), "<Param>k__BackingField");
        if (q && q->get_type_definition() == want) return q;
    }
    return nullptr;
}

MO* pause_agent() {
    if (!api() || !SF6CORE_HAS(api(), ui_agents)) return nullptr;
    void* out[1] = {};
    return perf::counted(api())->ui_agents("ui11200", out, 1) ? static_cast<MO*>(out[0]) : nullptr;
}

}  // namespace ui::gx
