// title.cpp -- "OVERDRIVE EDITION", a green line under the STREET FIGHTER logo of the title screen.
//
// A copy of the title's static (c) CAPCOM text (e_text_copyright, a child of the Panel c_main of
// app.menu.UIFlowTitle's GUI), made inside a post-hook of app.menu.UIFlowTitle.update (R19: a duplicate made outside
// a UI update is unfinished), attached to c_main and found again by name every frame (no reference kept on it: R19 /
// R35). Shown only while c_main plays its "DEFAULT2" state: the logo is fully displayed and PRESS ANY BUTTON blinks
// (seen live: "DEFAULT" during the logo animation and around the demo video). Entering DEFAULT2 starts a short
// entrance (white flash, scale down, white to green); it stays through SELECT and FADE_OUT (a button was pressed: the
// logo stays during the loading); any other state hides it. The flow's own Select state hooks
// were tried first and never fired. Every return to the title makes a new flow, so the copy is made again when
// missing; it dies with the title GUI.
//
// Off: an empty reframework/data/sf6core/title_off.txt (read when the hooks are installed).
// Everything is drawn through the core's own guarded display calls (client "sf6core.title").
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include "sf6core_internal.h"

void title_frame();

// ======================= tuning (all values, in one place) =======================
namespace tcfg {
constexpr const char* kText          = "OVERDRIVE EDITION";
constexpr const char* kCopyName      = "e_text_overdrive";
constexpr const char* kTemplateName  = "e_text_copyright";
constexpr int32_t     kFontSlot      = 6;                  // the bold display font (character names, CFN tabs)
constexpr float       kPosX = 960.0f, kPosY = 812.0f, kPosZ = 0.0f;
constexpr float       kFontW = 76.0f, kFontH = 76.0f;
constexpr float       kRegionW = 1600.0f, kRegionH = 130.0f;
constexpr float       kRasterW = 152.0f, kRasterH = 152.0f; // glyph raster size: the template's is its own 24 px, so a
                                                         // 76 px line drawn from it was blurry; 2x the font size
                                                         // stays sharp under the 1.25 flash scale and at 4K
constexpr int32_t     kPriority      = 10;                 // the movie texture and the others have 0
constexpr uint32_t    kGreenRgba     = 0xFF00D40Eu;        // the Drive gauge's full green (c_gauge00: R 14 G 212 B 0)
constexpr uint32_t    kWhiteRgba     = 0xFFFFFFFFu;
constexpr const char* kShownState    = "DEFAULT2";         // c_main's play state once the logo is complete
constexpr const char* kKeepStates[]  = { "SELECT", "FADE_OUT" }; // after a button press (SELECT for a few frames, then
                                                         // FADE_OUT): PRESS ANY BUTTON goes, the logo stays
                                                         // during the loading: the line stays too (no new entrance)
// entrance (frames counted from c_main entering kShownState)
constexpr float       kScaleStart    = 1.25f;              // frame 0
constexpr int         kScaleFrames   = 8;                  // ease-out down to 1.0
constexpr int         kColorStart    = 8;                  // the white holds while the scale comes down
constexpr int         kColorFrames   = 16;                 // white -> green, ease-out
constexpr bool        kSecondFlash   = true;               // a faint second flash
constexpr int         kFlash2Gap     = 10;                 // frames after the colour ends
constexpr int         kFlash2Frames  = 6;                  // toward white and back
constexpr float       kFlash2Amount  = 0.5f;               // 50% toward white at the peak
}  // namespace tcfg

namespace {

constexpr int kMaxFaults = 5;
constexpr int kMaxCreateTries = 5;     // per entry into kShownState

std::atomic<bool>     g_installed{false};
std::atomic<bool>     g_failed{false};
std::atomic<uint64_t> g_ticks{0};              // update post-hook calls
std::atomic<uint64_t> g_enter_tick{0};
std::atomic<int>      g_faults{0};

SF6CoreClient* g_client = nullptr;
REFrameworkTypeDefinitionHandle g_td_flow = nullptr;
int g_install_step = 0;

thread_local void* tl_flow = nullptr;

// state of the animation (touched by the update hook only)
bool     g_prev_sel = false;                    // c_main was in kShownState last tick
bool     g_first_logged = false;
bool     g_shown = false;                       // we made it visible and not yet hidden
int64_t  g_last_rgba = -1;
int      g_last_scale_q = -1;                   // scale * 1000
int      g_create_tries = 0;
bool     g_warned = false;

bool is_type(void* obj, REFrameworkTypeDefinitionHandle td) {
    if (!obj || !td || !s_sdk) return false;
    return s_sdk->managed_object->get_type_definition((REFrameworkManagedObjectHandle)obj) == td;
}

void* call0(void* obj, const char* name) {
    if (!obj) return nullptr;
    auto mh = find_method_on(s_sdk, obj, name);
    if (!mh) return nullptr;
    RawInvokeRet r;
    if (!invoke_c(s_sdk, mh, obj, nullptr, 0, &r) || r.exception_thrown) return nullptr;
    return r.ptr;
}

uint32_t lerp_rgba(uint32_t a, uint32_t b, float t) {   // per channel
    uint32_t out = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        const float ca = (float)((a >> sh) & 0xFF), cb = (float)((b >> sh) & 0xFF);
        int v = (int)(ca + (cb - ca) * t + 0.5f);
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        out |= (uint32_t)v << sh;
    }
    return out;
}
float ease_out(float x) {
    if (x < 0) x = 0;
    if (x > 1) x = 1;
    return 1.0f - (1.0f - x) * (1.0f - x);
}

// colour and scale at animation frame f
void anim_at(int f, uint32_t* rgba, float* scale) {
    using namespace tcfg;
    *scale = f >= kScaleFrames ? 1.0f : 1.0f + (kScaleStart - 1.0f) * (1.0f - ease_out((float)f / (float)kScaleFrames));
    if (f < kColorStart) { *rgba = kWhiteRgba; return; }
    const int c = f - kColorStart;
    if (c < kColorFrames) { *rgba = lerp_rgba(kWhiteRgba, kGreenRgba, ease_out((float)c / (float)kColorFrames)); return; }
    *rgba = kGreenRgba;
    if (kSecondFlash) {
        const int g = c - kColorFrames - kFlash2Gap;
        if (g >= 0 && g < kFlash2Frames) {
            const float x = (float)g / (float)kFlash2Frames;          // triangle: up then down
            const float tri = x < 0.5f ? x * 2.0f : (1.0f - x) * 2.0f;
            *rgba = lerp_rgba(kGreenRgba, kWhiteRgba, kFlash2Amount * tri);
        }
    }
}

// c_main of the flow's GUI: flow.FlowParam.CtrlCMain.get_Component().get_View().get_Child(); *out_view = the View
void* find_c_main(void* flow, void** out_view) {
    void* param = read_field_ref(s_sdk, flow, "FlowParam");
    void* ctrl = param ? read_field_ref(s_sdk, param, "CtrlCMain") : nullptr;
    void* gui = call0(ctrl, "get_Component");
    void* view = call0(gui, "get_View");
    *out_view = view;
    if (!view) return nullptr;
    void* c = call0(view, "get_Child");
    if (c) {
        void* nm = call0(c, "get_Name");
        char buf[64];
        if (nm && managed_str_to_utf8(nm, buf, sizeof buf) && strcmp(buf, "c_main") == 0) return c;
    }
    return sf6core_api_ptr()->ui_find_child(view, "c_main", 2);
}

void invoke_i64(void* obj, const char* sig, int64_t v) {
    SF6CoreArg a{};
    a.type = SF6CORE_ARG_I64;
    a.v.i = v;
    sf6core_api_ptr()->obj_invoke(g_client, obj, sig, &a, 1, nullptr);
}

// via.Size {w, h} of a Text (RasterSize / FontSize), or {-1, -1}
void get_size(void* text, const char* getter, float* w, float* h) {
    *w = *h = -1.0f;
    auto mh = text ? find_method_on(s_sdk, text, getter) : nullptr;
    RawInvokeRet r;
    if (mh && invoke_c(s_sdk, mh, text, nullptr, 0, &r) && !r.exception_thrown) { *w = ((float*)r.bytes)[0]; *h = ((float*)r.bytes)[1]; }
}

// set_RasterSize(via.Size): the size the glyphs are drawn at before they are scaled to the font size
bool set_raster(void* text, float w, float h) {
    auto mh = text ? find_method_on(s_sdk, text, "set_RasterSize") : nullptr;
    if (!mh) return false;
    float sz[2] = { w, h };
    void* args[] = { (void*)sz };          // a value type is passed by address (as in display.cpp)
    RawInvokeRet r;
    return invoke_c(s_sdk, mh, text, args, 1, &r) && !r.exception_thrown;
}

void configure_new(void* copy, void* tmpl) {
    using namespace tcfg;
    const SF6CoreAPI* api = sf6core_api_ptr();
    float tw, th, cw, ch;
    get_size(tmpl, "get_RasterSize", &tw, &th);
    const bool raster_ok = set_raster(copy, kRasterW, kRasterH);
    get_size(copy, "get_RasterSize", &cw, &ch);
    core_log(SF6CORE_LOG_INFO, nullptr, "[title] raster size: template %.0fx%.0f, line %.0fx%.0f (%s)", tw, th, cw, ch, raster_ok ? "set" : "set failed");
    invoke_i64(copy, "set_FontSlot(via.gui.FontSlot)", kFontSlot);
    invoke_i64(copy, "set_Priority(System.UInt16)", kPriority);
    api->ui_set_text(g_client, copy, kText);
    api->ui_set_region_size(g_client, copy, kRegionW, kRegionH);
    api->ui_set_font_size(g_client, copy, kFontW, kFontH);
    api->ui_set_position(g_client, copy, kPosX, kPosY, kPosZ);
    g_last_rgba = -1;
    g_last_scale_q = -1;
}

void tick_inner(void* flow) {
    using namespace tcfg;
    const SF6CoreAPI* api = sf6core_api_ptr();
    void* view = nullptr;
    void* cmain = find_c_main(flow, &view);
    if (!cmain) { g_prev_sel = false; g_shown = false; return; }
    char st[32] = "";
    void* ps = call0(cmain, "get_PlayState");
    if (ps) managed_str_to_utf8(ps, st, sizeof st);
    const bool sel = strcmp(st, kShownState) == 0;
    if (!g_first_logged) { g_first_logged = true; core_log(SF6CORE_LOG_INFO, nullptr, "[title] title seen (c_main state \"%s\")", st); }
    if (sel && !g_prev_sel) { g_enter_tick.store(g_ticks.load()); g_last_rgba = -1; g_last_scale_q = -1; g_create_tries = 0; }
    g_prev_sel = sel;
    bool after_press = false;
    for (const char* k : kKeepStates) after_press = after_press || strcmp(st, k) == 0;
    const bool keep = sel || (g_shown && after_press);
    if (!keep && !g_shown) return;                                   // nothing to hide, nothing to show
    void* copy = api->ui_find_child(view, kCopyName, 0);

    if (!keep) {
        if (copy) api->ui_set_visible(g_client, copy, 0);
        g_shown = false;
        return;
    }
    if (!copy) {
        if (g_create_tries >= kMaxCreateTries) return;
        ++g_create_tries;
        void* tmpl = api->ui_find_child(cmain, kTemplateName, 0);
        if (!tmpl) return;
        // attached to the View, not to c_main: c_main's state clips (FADE_OUT after a button press) fade the copy
        // with the template it was made from (R31), while the logo stays on screen
        if (api->ui_clone_control(g_client, tmpl, view, kCopyName, &copy) != SF6CORE_OK || !copy) {
            if (!g_warned && g_create_tries >= kMaxCreateTries) {
                g_warned = true;
                core_log(SF6CORE_LOG_WARN, nullptr, "[title] the OVERDRIVE EDITION line could not be created");
            }
            return;
        }
        configure_new(copy, tmpl);
        core_log(SF6CORE_LOG_INFO, nullptr, "[title] OVERDRIVE EDITION line created (font slot %d, size %.0fx%.0f, at %.0f,%.0f)",
                 (int)kFontSlot, kFontW, kFontH, kPosX, kPosY);
    }
    if (!sel) g_last_rgba = -1;                                      // FADE_OUT: written again every frame
    const int f = (int)(g_ticks.load() - g_enter_tick.load());
    uint32_t rgba;
    float sc;
    anim_at(f < 0 ? 0 : f, &rgba, &sc);
    const int sq = (int)(sc * 1000.0f + 0.5f);
    if (sq != g_last_scale_q) { api->ui_set_scale(g_client, copy, sc, sc, 1.0f); g_last_scale_q = sq; }
    if ((int64_t)rgba != g_last_rgba) { api->ui_set_color(g_client, copy, rgba); g_last_rgba = (int64_t)rgba; }
    api->ui_set_visible(g_client, copy, 1);
    g_shown = true;
}

void tick_seh(void* flow) {
    __try { tick_inner(flow); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        const int n = ++g_faults;
        core_log(SF6CORE_LOG_WARN, nullptr, "[title] fault 0x%08lX in the title line (%d/%d)", GetExceptionCode(), n, kMaxFaults);
    }
}

int pre_update(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    tl_flow = (argv && is_type(argv[1], g_td_flow)) ? argv[1] : nullptr;
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void post_update(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    void* flow = tl_flow;
    tl_flow = nullptr;
    if (!flow || core_closing() || g_faults.load() >= kMaxFaults) return;
    g_ticks.fetch_add(1);
    tick_seh(flow);
}

bool hook_one(REFrameworkTypeDefinitionHandle td, const char* name, REFPreHookFn pre, REFPostHookFn post) {
    auto mh = td ? s_sdk->type_definition->find_method(td, name) : nullptr;
    if (!mh) { core_log(SF6CORE_LOG_WARN, nullptr, "[title] method %s not found: title line off", name); return false; }
    if (!s_sdk->functions->add_hook(mh, pre, post, false)) {
        core_log(SF6CORE_LOG_WARN, nullptr, "[title] hook %s failed: title line off", name);
        return false;
    }
    return true;
}

bool off_file_present() {
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return false;
    char* slash = strrchr(path, '\\');
    if (!slash) return false;
    *slash = 0;
    strncat_s(path, "\\reframework\\data\\sf6core\\title_off.txt", _TRUNCATE);
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

}  // namespace

// Each core frame (sf6core.cpp): after the first seconds of the game, installs one hook per frame (add_hook stalls
// the game's threads while it patches).
void title_frame() {
    if (g_installed.load() || g_failed.load() || !s_sdk || core_closing()) return;
    if (s_frame < 300) return;
    switch (g_install_step) {
    case 0: {
        if (off_file_present()) {
            g_failed = true;
            core_log(SF6CORE_LOG_INFO, nullptr, "[title] off (title_off.txt present)");
            return;
        }
        auto* tdb = s_sdk->functions->get_tdb();
        g_td_flow = s_sdk->tdb->find_type(tdb, "app.menu.UIFlowTitle");
        if (!g_td_flow) {
            core_log(SF6CORE_LOG_WARN, nullptr, "[title] types not found: title line off");
            g_failed = true;
            return;
        }
        SF6CoreClientDesc d{ sizeof(SF6CoreClientDesc), "sf6core.title", "1", 0 };
        if (sf6core_api_ptr()->register_client(&d, &g_client) != SF6CORE_OK || !g_client) {
            g_client = nullptr;
            core_log(SF6CORE_LOG_WARN, nullptr, "[title] client not registered: title line off");
            g_failed = true;
            return;
        }
        g_install_step = 1;
        return;
    }
    case 1:
        if (!hook_one(g_td_flow, "update", pre_update, post_update)) g_failed = true;
        else {
            g_installed = true;
            core_log(SF6CORE_LOG_INFO, nullptr, "[title] hook installed (UIFlowTitle.update)");
        }
        return;
    }
}
