// vfx::feed — the frame loop (game side): the UpdateBehavior blocks of SF6_DIRecolor.lua (body_push every 6 frames,
// parts_pull every 60, the "di_dirty" signal) and SF6_DriveRecolor.lua (the Drive colours every 6 frames, the
// fighters' actions while in a fight). Same counters as the scripts: the first push on frame 6, the fight check
// every 30 frames from then on.

#include <mutex>

#include "body.hpp"
#include "feed.hpp"
#include "scene.hpp"
#include "tick.hpp"

#include "../common.hpp"
#include "../perf.hpp"

namespace vfx::feed {
namespace {

std::mutex g_mx;
int g_hint[2] = { 0, 0 };                          // SF6_ColorSpinExtra.to_apply's fighter per side
bool g_dirty = true;                               // SF6_DIRecolor's dirty starts true
uint64_t g_frame = 0;
bool g_in_fight = false;
int64_t g_last_act[2] = { 0, 0 };
bool g_has_act[2] = { false, false };
uint64_t g_pushes = 0, g_raised = 0, g_parts_saved = 0;

}  // namespace

void set_side_hint(int side, int fighter) {
    if (side != 1 && side != 2) return;
    std::lock_guard<std::mutex> lk(g_mx);
    g_hint[side - 1] = fighter > 0 ? fighter : 0;
}

void mark_di_dirty() { std::lock_guard<std::mutex> lk(g_mx); g_dirty = true; }

void on_frame() {
    ++g_frame;
    // light fight: the settings do not change during a fight (a Training popup change still lands within a second)
    const uint64_t every = (cmn::light_fight() && g_in_fight) ? 60 : 6;
    if (g_frame % every == 0) {
        int fid[2];
        {
            std::lock_guard<std::mutex> lk(g_mx);
            for (int s = 0; s < 2; ++s) { fid[s] = scene::side_char(s); if (!fid[s]) fid[s] = g_hint[s]; }
        }
        {
            const int64_t t0 = perf::begin();
            auto t = di_state_text(fid);
            perf::end(perf::FeedDiState, t0);
            if (t) {
                const int64_t t1 = perf::begin();
                tick::set_di_state(*t);
                perf::end(perf::FeedDiText, t1);
                ++g_pushes;
            } else {
                ++g_raised;
            }
        }
        if (g_frame % 60 == 0) {
            perf::Scope s3(perf::FeedPartsMerge);
            for (int s = 0; s < 2; ++s) if (fid[s] > 0 && parts_merge(fid[s], tick::parts(fid[s]))) ++g_parts_saved;
        }
        const int dfid[2] = { scene::side_char(0), scene::side_char(1) };
        {
            perf::Scope s2(perf::FeedDrColors);
            if (auto c = dr_colors(dfid)) tick::set_dr_colors(c->data());
        }
    }
    if (g_frame < 6) return;                                            // the scripts learnt they were owned on frame 6
    bool dirty = false;
    { std::lock_guard<std::mutex> lk(g_mx); dirty = g_dirty; g_dirty = false; }
    if (dirty) tick::ui("di_dirty");
    if (g_frame % 30 == 0) g_in_fight = scene::battle_char(0) != 0 || scene::battle_char(1) != 0;
    if (!g_in_fight) return;
    for (int i = 0; i < 2; ++i) {
        int64_t a = 0;
        if (scene::battle_act(i, a) && (!g_has_act[i] || a != g_last_act[i])) {
            g_has_act[i] = true;
            g_last_act[i] = a;
            body::set_act(i + 1, a);
        }
    }
}

std::string status() {
    std::lock_guard<std::mutex> lk(g_mx);
    return "feed frames=" + std::to_string(g_frame) + " pushes=" + std::to_string(g_pushes) + " raised=" + std::to_string(g_raised) +
           " parts_saved=" + std::to_string(g_parts_saved) + " hint=" + std::to_string(g_hint[0]) + "/" + std::to_string(g_hint[1]) +
           " fight=" + (g_in_fight ? "1" : "0");
}

}  // namespace vfx::feed
