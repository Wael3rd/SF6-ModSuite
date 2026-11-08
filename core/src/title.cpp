// title.cpp -- "OVERDRIVE EDITION", a line in the logo's white under the STREET FIGHTER logo of the title screen.
//
// A copy of the title's static (c) CAPCOM text (e_text_copyright, a child of the Panel c_main of
// app.menu.UIFlowTitle's GUI), made inside a post-hook of app.menu.UIFlowTitle.update (R19: a duplicate made outside
// a UI update is unfinished), attached to c_main and found again by name every frame (no reference kept on it: R19 /
// R35). Shown only while c_main plays its "DEFAULT2" state: the logo is fully displayed and PRESS ANY BUTTON blinks
// (seen live: "DEFAULT" during the logo animation and around the demo video). Entering DEFAULT2 starts a short
// entrance (scale down, from pure white to the logo's white); it stays through SELECT and FADE_OUT (a button was pressed: the
// logo stays during the loading); any other state hides it. The flow's own Select state hooks
// were tried first and never fired. Every return to the title makes a new flow, so the copy is made again when
// missing; it dies with the title GUI.
//
// Off: an empty reframework/data/sf6core/title_off.txt (read when the hooks are installed).
// Position and size: reframework/data/sf6core/title.ini, read once when the hooks are installed, so a change needs a new
// launch. Written with the defaults when it is missing, never overwritten. A missing or unusable value keeps its default.
// Per background (Wael 08/11), when the Slots Loader's title packs are there (reframework/data/SF6_TitleSlots_Data/
// registry.json, written by its loader at each launch): the background shown this session is the registry's "applied" pack
// ("" = the game's own title, key "game"), and its line is the one of overdrive.json, next to it, written by the Slots
// Loader's title preview (Options > Interface > Title Screen, Confirm): "show", "x" and "y" (the line's middle, in % of the
// screen), "size" (its font height, in % of the screen's height). A background without an entry has no line. Both files are
// read each time the logo completes (a return to the title reads them again); the line is placed by its middle
// (ControlPoint and PageAlignment CenterCenter, as in the preview). Without the registry: title.ini, as before.
// Everything is drawn through the core's own guarded display calls (client "sf6core.title").
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
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
constexpr int         kFontMin = 8, kFontMax = 150;        // at 150 the line still fits kRegionW: nothing is squeezed
constexpr float       kSpaceW = 1920.0f, kSpaceH = 1080.0f; // the UI's own space: title.ini's positions are shares of it
constexpr float       kRegionW = 1600.0f, kRegionH = 130.0f;
constexpr float       kRasterW = 152.0f, kRasterH = 152.0f; // glyph raster size: the template's is its own 24 px, so a
                                                         // 76 px line drawn from it was blurry; 2x the font size
                                                         // stays sharp under the 1.25 flash scale and at 4K
constexpr int32_t     kPriority      = 10;                 // the movie texture and the others have 0
constexpr int64_t     kCenterCenter  = 5;                  // via.gui.ControlPoint / PageAlignment CenterCenter
constexpr uint32_t    kLineRgba      = 0xFFEBEBECu;        // the STREET FIGHTER logo's white, read on a screenshot: R 236 G 235 B 235
constexpr uint32_t    kWhiteRgba     = 0xFFFFFFFFu;
constexpr const char* kShownState    = "DEFAULT2";         // c_main's play state once the logo is complete
constexpr const char* kKeepStates[]  = { "SELECT", "FADE_OUT" }; // after a button press (SELECT for a few frames, then
                                                         // FADE_OUT): PRESS ANY BUTTON goes, the logo stays
                                                         // during the loading: the line stays too (no new entrance)
// entrance (frames counted from c_main entering kShownState)
constexpr float       kScaleStart    = 1.25f;              // frame 0
constexpr int         kScaleFrames   = 8;                  // ease-out down to 1.0
constexpr int         kColorStart    = 8;                  // the white holds while the scale comes down
constexpr int         kColorFrames   = 16;                 // white -> the line's colour, ease-out
constexpr bool        kSecondFlash   = false;              // a faint second flash (was for the green; on white it would not show)
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
    if (c < kColorFrames) { *rgba = lerp_rgba(kWhiteRgba, kLineRgba, ease_out((float)c / (float)kColorFrames)); return; }
    *rgba = kLineRgba;
    if (kSecondFlash) {
        const int g = c - kColorFrames - kFlash2Gap;
        if (g >= 0 && g < kFlash2Frames) {
            const float x = (float)g / (float)kFlash2Frames;          // triangle: up then down
            const float tri = x < 0.5f ? x * 2.0f : (1.0f - x) * 2.0f;
            *rgba = lerp_rgba(kLineRgba, kWhiteRgba, kFlash2Amount * tri);
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

// title.ini's values, the defaults until it is read
struct TitleCfg { float x, y, font; };
TitleCfg g_cfg{ tcfg::kPosX, tcfg::kPosY, tcfg::kFontW };
TitleCfg g_ini{ tcfg::kPosX, tcfg::kPosY, tcfg::kFontW };   // title.ini's own (g_cfg is what the line uses now)
bool g_line_on = true;                                      // this visit of the title shows the line
bool g_per_bg = false;                                      // the line follows the background's entry (placed by its middle)
bool g_reapply = false;                                     // the line's place and size to write again (a new visit)

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

// The line's size and place, from g_cfg (a new line, or a new visit with another background's entry)
void apply_geometry(void* copy) {
    using namespace tcfg;
    const SF6CoreAPI* api = sf6core_api_ptr();
    const float raster = g_cfg.font * (kRasterW / kFontW);    // the same 2x as the default size, whatever the size
    set_raster(copy, raster, raster);
    if (g_per_bg) {                                            // placed by its middle, as the Slots Loader's preview draws it
        invoke_i64(copy, "set_ControlPoint(via.gui.ControlPoint)", kCenterCenter);
        invoke_i64(copy, "set_PageAlignment(via.gui.PageAlignment)", kCenterCenter);
    }
    api->ui_set_region_size(g_client, copy, kRegionW, g_cfg.font * (kRegionH / kFontH));
    api->ui_set_font_size(g_client, copy, g_cfg.font, g_cfg.font);
    api->ui_set_position(g_client, copy, g_cfg.x, g_cfg.y, kPosZ);
}

void configure_new(void* copy, void* tmpl) {
    using namespace tcfg;
    const SF6CoreAPI* api = sf6core_api_ptr();
    float tw, th, cw, ch;
    get_size(tmpl, "get_RasterSize", &tw, &th);
    invoke_i64(copy, "set_FontSlot(via.gui.FontSlot)", kFontSlot);
    invoke_i64(copy, "set_Priority(System.UInt16)", kPriority);
    api->ui_set_text(g_client, copy, kText);
    apply_geometry(copy);
    get_size(copy, "get_RasterSize", &cw, &ch);
    core_log(SF6CORE_LOG_INFO, nullptr, "[title] raster size: template %.0fx%.0f, line %.0fx%.0f", tw, th, cw, ch);
    g_last_rgba = -1;
    g_last_scale_q = -1;
}

// <game folder>\reframework\data\<rel>
bool game_data_path(const char* rel, char (&path)[MAX_PATH]) {
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return false;
    char* slash = strrchr(path, '\\');
    if (!slash) return false;
    *slash = 0;
    strncat_s(path, "\\reframework\\data\\", _TRUNCATE);
    strncat_s(path, rel, _TRUNCATE);
    return true;
}
// A small text file whole ("" when missing or over 1 MB)
std::string read_small(const char* rel) {
    char path[MAX_PATH];
    if (!game_data_path(rel, path)) return {};
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    std::string out;
    LARGE_INTEGER sz{};
    if (GetFileSizeEx(f, &sz) && sz.QuadPart > 0 && sz.QuadPart < (1 << 20)) {
        out.resize((size_t)sz.QuadPart);
        DWORD got = 0;
        if (!ReadFile(f, out.data(), (DWORD)out.size(), &got, nullptr)) got = 0;
        out.resize(got);
    }
    CloseHandle(f);
    return out;
}
// The string after "name": in text, from `from` ("" when absent). Both files are written by our own tools, flat.
std::string json_string_after(const std::string& t, const char* name, size_t from = 0) {
    const std::string key = std::string("\"") + name + "\"";
    size_t i = t.find(key, from);
    if (i == std::string::npos) return {};
    i = t.find(':', i + key.size());
    if (i == std::string::npos) return {};
    i = t.find('"', i + 1);
    if (i == std::string::npos) return {};
    const size_t e = t.find('"', i + 1);
    return e == std::string::npos ? std::string() : t.substr(i + 1, e - i - 1);
}
// The number or boolean after "name": inside [from, to); false when absent
bool json_number_in(const std::string& t, const char* name, size_t from, size_t to, float* out) {
    const std::string key = std::string("\"") + name + "\"";
    size_t i = t.find(key, from);
    if (i == std::string::npos || i >= to) return false;
    i = t.find(':', i + key.size());
    if (i == std::string::npos || i >= to) return false;
    ++i;
    while (i < to && (t[i] == ' ' || t[i] == '\t')) ++i;
    if (t.compare(i, 4, "true") == 0) { *out = 1.0f; return true; }
    if (t.compare(i, 5, "false") == 0) { *out = 0.0f; return true; }
    char* end = nullptr;
    const float v = strtof(t.c_str() + i, &end);
    if (end == t.c_str() + i) return false;
    *out = v;
    return true;
}

std::string cyc_playlist_key();   // the playlist's background in this visit ("": none), below

// At each new visit of the title (the logo complete): the line of this session's background, or title.ini's
// The game's own title's line when overdrive.json has no entry for it: shown (the plugin's editor, title_preview.cpp, the same)
constexpr float kGameDefX = 50.00f, kGameDefY = 62.49f, kGameDefSize = 11.44f;
void read_background_cfg() {
    using namespace tcfg;
    g_per_bg = false;
    g_line_on = true;
    g_cfg = g_ini;
    const std::string reg = read_small("SF6_TitleSlots_Data\\registry.json");
    if (reg.empty()) return;                                           // no title packs: title.ini, as before
    g_per_bg = true;
    g_line_on = false;
    // the background shown now: the loader's (a change in the Options shows at a return to the title, Random draws at each
    // visit), else the registry's, applied at launch
    std::string key;
    using ShownFn = int (*)(char*, int);
    static ShownFn shown = nullptr;
    static bool looked = false;
    if (!looked) {
        looked = true;
        if (HMODULE h = GetModuleHandleW(L"amd_ags_x64.dll")) shown = reinterpret_cast<ShownFn>(GetProcAddress(h, "SF6TitleSlots_Shown"));
    }
    char buf[64] = {};
    const std::string pk = cyc_playlist_key();                     // the playlist switched this visit
    if (!pk.empty()) key = pk;
    else if (shown && shown(buf, sizeof buf) > 0) key = buf;
    if (key.empty()) {
        const size_t applied = reg.find("\"applied\"");
        key = applied == std::string::npos ? std::string() : json_string_after(reg, "pack", applied);
    }
    if (key.empty()) key = "game";
    const std::string od = read_small("SF6_TitleSlots_Data\\overdrive.json");
    const std::string k = "\"" + key + "\"";
    const size_t at = od.find(k);
    const size_t open = at == std::string::npos ? std::string::npos : od.find('{', at + k.size());
    const size_t close = open == std::string::npos ? std::string::npos : od.find('}', open);
    float show = 0, x = 0, y = 0, size = 0;
    bool found = close != std::string::npos && json_number_in(od, "show", open, close, &show) &&
                 json_number_in(od, "x", open, close, &x) && json_number_in(od, "y", open, close, &y) && json_number_in(od, "size", open, close, &size);
    if (!found && key == "game") {           // the game's own title never set (a first install): the line shown, Wael's place (08/11)
        found = true;
        show = 1; x = kGameDefX; y = kGameDefY; size = kGameDefSize;
    }
    if (found && show > 0.5f) {
        g_line_on = true;
        g_cfg.x = roundf(fminf(fmaxf(x, 0.0f), 100.0f) * kSpaceW) / 100.0f;
        g_cfg.y = roundf(fminf(fmaxf(y, 0.0f), 100.0f) * kSpaceH) / 100.0f;
        g_cfg.font = fminf(fmaxf(size * kSpaceH / 100.0f, (float)kFontMin), (float)kFontMax);
    }
    core_log(SF6CORE_LOG_INFO, nullptr, "[title] background \"%s\": %s", key.c_str(),
             g_line_on ? ("line at " + std::to_string((int)g_cfg.x) + "," + std::to_string((int)g_cfg.y) + ", size " + std::to_string((int)g_cfg.font)).c_str()
                       : "no line (no entry, or hidden, in overdrive.json)");
}


// ======================= Random: a playlist while the title stays on screen =======================
// The flow's three movies (UIFlowTitle.mArrMovie: via.movie.Movie IN / LOOP / OUT, read 08/11). Left alone, each cycle plays IN,
// then LOOP for 20 s; then the game stops both and prepares them again, waits about 25 s with nothing playing, and plays IN again.
// The same paths prepared again keep what the first opening learnt of their files: another file served under them (the loader's
// redirect) made Media Foundation fail to open it, a crash twice (08/11). So the files are not changed under the game's paths: in
// the wait, with every movie ready and nothing playing, the three movies are given new resources, the next background's own copies
// (the loader's private paths: _titleslots/<key>/product/gui/gm/ui10000/part/title_*.mov, streamed, their resource files beside),
// ONE AT A TIME, the next once the last one is ready (LOOP, IN, OUT): reset, set_ResourceHandle, prepare. Tried in game (Lua,
// 08/11): any one alone works; two or three prepared in the same frame froze the game; without reset a movie already given a
// resource keeps playing the one before until the game prepares it again. Start, loop and the transition after a key then always
// come from the same background (seen by Wael, Alex -> Chun-Li, a key: Chun-Li's transition). The loader draws the key
// (SF6TitleSlots_Draw, "random" only) without changing what it serves: a new visit (new flow, new movies) keeps its own way.
// A key pressed (SELECT, FADE_OUT) ends it for the visit. Off: reframework/data/sf6core/title_cycle_off.txt.
namespace cyc {
enum State { Preparing = 0, Ready = 1, Playing = 2, Paused = 3, Finished = 4 };   // via.movie.Movie.CosmeticState
const char* const kNames[3] = { "title_start", "title_loop", "title_end" };       // EPlayType IN, LOOP, OUT
bool g_off = false;
void* g_flow = nullptr;          // the visit (a new flow object: a new visit; compared, never used)
bool g_loop_seen = false;        // LOOP played in this cycle: the next wait is a switch point
uint64_t g_ready_at = 0;         // the tick every movie was first seen ready with nothing playing
int g_phase = 0;                 // 0 watching, 1 giving the new resources, one movie at a time
uint64_t g_set_at = 0;
const int kOrder[3] = { 1, 0, 2 };   // LOOP, IN, OUT
int g_step = 0;                  // the next movie of kOrder to give its resource (phase 1)
int g_pending = -1;              // the movie given its resource, waited for (-1: none)
uint64_t g_pending_at = 0;
void* g_holders[3] = {};         // the new resources (one reference kept each)
std::string g_next;              // the key being given
std::string g_key;               // the background the visit shows since a switch ("": the one it started with)
bool g_logged_missing = false;

void* movie_at(void* arr, int i) {
    auto mh = find_method_on(s_sdk, arr, "GetValue");
    if (!mh) mh = find_method_on(s_sdk, arr, "Get");
    if (!mh) return nullptr;
    void* args[] = { (void*)(uintptr_t)(uint32_t)i };
    RawInvokeRet r;
    return invoke_c(s_sdk, mh, arr, args, 1, &r) && !r.exception_thrown ? r.ptr : nullptr;
}
int state_of(void* m) {
    auto mh = m ? find_method_on(s_sdk, m, "get_State") : nullptr;
    RawInvokeRet r;
    return mh && invoke_c(s_sdk, mh, m, nullptr, 0, &r) && !r.exception_thrown ? (int)(int32_t)r.dword : -1;
}
bool call_bool0(void* o, const char* name, bool* out) {
    auto mh = o ? find_method_on(s_sdk, o, name) : nullptr;
    RawInvokeRet r;
    if (!mh || !invoke_c(s_sdk, mh, o, nullptr, 0, &r) || r.exception_thrown) return false;
    *out = r.bytes[0] != 0;
    return true;
}
bool call_void(void* o, const char* name, void* arg = nullptr) {
    auto mh = o ? find_method_on(s_sdk, o, name) : nullptr;
    void* args[] = { arg };
    RawInvokeRet r;
    return mh && invoke_c(s_sdk, mh, o, arg ? args : nullptr, arg ? 1 : 0, &r) && !r.exception_thrown;
}
// A movie resource holder for a path (one reference kept: the movie holds it as long as it plays it)
void* movie_holder(const std::string& path) {
    void* holder = nullptr;
    __try {
        auto* rm = s_sdk->functions->get_resource_manager();
        auto* res = rm ? s_sdk->resource_manager->create_resource(rm, "via.movie.MovieResource", path.c_str()) : nullptr;
        if (!res) return nullptr;
        s_sdk->resource->add_ref((REFrameworkResourceHandle)res);
        auto h = s_sdk->resource->create_holder((REFrameworkResourceHandle)res, "via.movie.MovieResourceHolder");
        s_sdk->resource->release((REFrameworkResourceHandle)res);
        if (!h) return nullptr;
        s_sdk->managed_object->add_ref((REFrameworkManagedObjectHandle)h);
        holder = (void*)h;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    return holder;
}
int loader_draw(const std::string& not_key, char* buf, int cap) {
    using DrawFn = int (*)(const char*, char*, int);
    static DrawFn draw = nullptr;
    static bool looked = false;
    if (!looked) {
        looked = true;
        if (HMODULE h = GetModuleHandleW(L"amd_ags_x64.dll")) draw = reinterpret_cast<DrawFn>(GetProcAddress(h, "SF6TitleSlots_Draw"));
    }
    return draw ? draw(not_key.c_str(), buf, cap) : 0;
}
std::string shown_now() {
    if (!g_key.empty()) return g_key;
    using ShownFn = int (*)(char*, int);
    static ShownFn shown = nullptr;
    static bool looked = false;
    if (!looked) {
        looked = true;
        if (HMODULE h = GetModuleHandleW(L"amd_ags_x64.dll")) shown = reinterpret_cast<ShownFn>(GetProcAddress(h, "SF6TitleSlots_Shown"));
    }
    char b[64] = {};
    return shown && shown(b, sizeof b) > 0 ? std::string(b) : std::string();
}

void tick(void* flow, const char* st) {
    if (g_off) return;
    if (flow != g_flow) { g_flow = flow; g_loop_seen = false; g_ready_at = 0; g_phase = 0; g_key.clear(); }   // a new visit
    if (strcmp(st, "SELECT") == 0 || strcmp(st, "FADE_OUT") == 0) {
        if (g_phase == 1) core_log(SF6CORE_LOG_WARN, nullptr, "[title] playlist: a key pressed while \"%s\" was given: stopped there", g_next.c_str());
        g_phase = 0; g_loop_seen = false; g_ready_at = 0;
        return;
    }
    void* arr = read_field_ref(s_sdk, flow, "mArrMovie");
    void* mv[3] = { arr ? movie_at(arr, 0) : nullptr, arr ? movie_at(arr, 1) : nullptr, arr ? movie_at(arr, 2) : nullptr };
    const int si = state_of(mv[0]), sl = state_of(mv[1]), so = state_of(mv[2]);
    if (si < 0 || sl < 0 || so < 0) {
        if (!g_logged_missing) { g_logged_missing = true; core_log(SF6CORE_LOG_WARN, nullptr, "[title] playlist: the three movies not read: off"); }
        return;
    }
    const int states[3] = { si, sl, so };
    const uint64_t now = g_ticks.load();
    if (g_phase == 1) {                                   // one movie at a time
        if (g_pending >= 0) {
            if (states[g_pending] == Ready && now - g_pending_at >= 10) g_pending = -1;
            else if (now - g_pending_at > 600) {
                g_phase = 0;
                core_log(SF6CORE_LOG_WARN, nullptr, "[title] playlist: %s of \"%s\" not ready after 10 s (state %d): stopped there",
                         kNames[g_pending], g_next.c_str(), states[g_pending]);
            }
            return;
        }
        if (g_step >= 3) {
            g_phase = 0;
            g_key = g_next;
            core_log(SF6CORE_LOG_INFO, nullptr, "[title] playlist: \"%s\" ready for the next cycle", g_key.c_str());
            return;
        }
        const int i = kOrder[g_step++];
        const bool ok = call_void(mv[i], "reset") && call_void(mv[i], "set_ResourceHandle", g_holders[i]) && call_void(mv[i], "prepare");
        g_pending = i;
        g_pending_at = now;
        if (!ok) core_log(SF6CORE_LOG_WARN, nullptr, "[title] playlist: %s of \"%s\": a call failed", kNames[i], g_next.c_str());
        return;
    }
    if (sl == Playing) { g_loop_seen = true; g_ready_at = 0; return; }
    bool playing = true;
    const bool idle = si == Ready && sl == Ready && so == Ready && call_bool0(flow, "IsPlay", &playing) && !playing;
    if (!idle || !g_loop_seen) { g_ready_at = 0; return; }
    if (!g_ready_at) { g_ready_at = now; return; }
    if (now - g_ready_at < 60) return;                   // a second into the wait: the game's own preparation is over
    g_loop_seen = false;
    char key[64] = {};
    const std::string cur = shown_now();
    if (loader_draw(cur, key, sizeof key) <= 0) return;  // not on "random"
    void* holders[3] = {};
    for (int i = 0; i < 3; ++i) {
        holders[i] = movie_holder(std::string("_titleslots/") + key + "/product/gui/gm/ui10000/part/" + kNames[i] + ".mov");
        if (!holders[i]) { core_log(SF6CORE_LOG_WARN, nullptr, "[title] playlist: no resource for \"%s\" %s: nothing changed", key, kNames[i]); return; }
    }
    for (int i = 0; i < 3; ++i) g_holders[i] = holders[i];
    g_next = key;
    g_phase = 1;
    g_step = 0;
    g_pending = -1;
    g_set_at = now;
    core_log(SF6CORE_LOG_INFO, nullptr, "[title] playlist: \"%s\" given to the three movies, one at a time", key);
}
}  // namespace cyc
std::string cyc_playlist_key() { return cyc::g_key; }

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
    cyc::tick(flow, st);
    if (sel && !g_prev_sel) {
        g_enter_tick.store(g_ticks.load()); g_last_rgba = -1; g_last_scale_q = -1; g_create_tries = 0;
        read_background_cfg();
        g_reapply = true;
    }
    g_prev_sel = sel;
    bool after_press = false;
    for (const char* k : kKeepStates) after_press = after_press || strcmp(st, k) == 0;
    const bool keep = g_line_on && (sel || (g_shown && after_press));
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
        g_reapply = false;
        core_log(SF6CORE_LOG_INFO, nullptr, "[title] OVERDRIVE EDITION line created (font slot %d, size %.0fx%.0f, at %.0f,%.0f)",
                 (int)kFontSlot, g_cfg.font, g_cfg.font, g_cfg.x, g_cfg.y);
    }
    if (g_reapply) { apply_geometry(copy); g_reapply = false; }
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

// <game folder>\reframework\data\sf6core\<name>
bool data_path(const char* name, char (&path)[MAX_PATH]) {
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return false;
    char* slash = strrchr(path, '\\');
    if (!slash) return false;
    *slash = 0;
    strncat_s(path, "\\reframework\\data\\sf6core\\", _TRUNCATE);
    strncat_s(path, name, _TRUNCATE);
    return true;
}

bool off_file_present() {
    char path[MAX_PATH];
    return data_path("title_off.txt", path) && GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}
bool cycle_off_file_present() {
    char path[MAX_PATH];
    return data_path("title_cycle_off.txt", path) && GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}


// One number of title.ini within [lo, hi]; anything else keeps the default and says so. A decimal comma is accepted (players
// type 0,5). GetPrivateProfileInt is not used: it reads "abc" as 0, a valid position, and takes no decimals.
float ini_value(const char* path, const char* key, float lo, float hi, float def) {
    char text[32] = {};
    GetPrivateProfileStringA("title", key, "", text, sizeof(text), path);
    if (!text[0]) return def;
    if (char* comma = strchr(text, ',')) *comma = '.';
    char* end = nullptr;
    const float v = strtof(text, &end);
    while (end && *end == ' ') ++end;
    if (end == text || (end && *end) || !(v >= lo && v <= hi)) {
        core_log(SF6CORE_LOG_WARN, nullptr, "[title] title.ini: %s is not a number from %g to %g: default kept", key, lo, hi);
        return def;
    }
    return v;
}

void load_ini() {
    using namespace tcfg;
    char path[MAX_PATH];
    if (!data_path("title.ini", path)) return;
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {           // made once, so that players find it; never overwritten
        char text[512];
        const int len = snprintf(text, sizeof(text),
            "; The OVERDRIVE EDITION line under the logo of the title screen. Read when the game starts.\r\n"
            "; pos_x and pos_y: a share of the screen, from 0 to 1 (0.5 is the middle). pos_x is the centre of the line.\r\n"
            "; font_size: %d to %d, at the game's 1920 x 1080 reference. Delete this file to get the defaults back.\r\n"
            "[title]\r\npos_x=%.4f\r\npos_y=%.4f\r\nfont_size=%d\r\n",
            kFontMin, kFontMax, kPosX / kSpaceW, kPosY / kSpaceH, (int)kFontW);
        HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            if (len > 0 && len < (int)sizeof(text)) WriteFile(f, text, (DWORD)len, &written, nullptr);
            CloseHandle(f);
        }
        return;
    }
    g_cfg.x = roundf(ini_value(path, "pos_x", 0.0f, 1.0f, kPosX / kSpaceW) * kSpaceW);
    g_cfg.y = roundf(ini_value(path, "pos_y", 0.0f, 1.0f, kPosY / kSpaceH) * kSpaceH);
    g_cfg.font = ini_value(path, "font_size", (float)kFontMin, (float)kFontMax, kFontW);
    g_ini = g_cfg;
    core_log(SF6CORE_LOG_INFO, nullptr, "[title] title.ini: at %.0f,%.0f, size %.0f", g_cfg.x, g_cfg.y, g_cfg.font);
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
        load_ini();
        cyc::g_off = cycle_off_file_present();
        if (cyc::g_off) core_log(SF6CORE_LOG_INFO, nullptr, "[title] playlist: off (title_cycle_off.txt present)");
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
