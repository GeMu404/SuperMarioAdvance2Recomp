// Diagnostic: who writes each soft-OAM slot. OFF unless SMA2_OAM_LOG=1.
// F10 (or SMA2_OAM_LOG_FRAME=N, a vblank count) dumps the frame just built
// to logs/oam-<vblank>.txt: slot, writer PCs of attr0/attr1, attrs, decoded
// X/Y/tile/shape, the plain-fold draw X, and the last full X latched by
// smw_ws_begin_screen_x before the attr1 write (if the path is hooked).
#include "smw_view.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#include "runtime_arm.h"
#include "runtime_bus_bridge.h"

namespace smw { int oam_log_latched_x(); int oam_log_fold_x(int raw); }

namespace {

struct SlotLog {
    std::uint32_t pc0, pc1, pc2;
    std::int32_t latched;
    unsigned long long frame1;
};
SlotLog g_cur[128];
unsigned long long g_frame = ~0ull;
unsigned long long g_target = 0;
bool g_in_write = false;

void dump(unsigned long long frame) {
    std::error_code ec;
    std::filesystem::create_directories("logs", ec);
    char name[64];
    std::snprintf(name, sizeof name, "logs/oam-%llu.txt", frame);
    std::FILE* f = std::fopen(name, "w");
    if (!f) return;
    std::fprintf(f, "# vblank %llu  view_w %d  slot pc_attr0 pc_attr1 pc_attr2 "
                    "attr0 attr1 attr2 y x9 tile shape size fold_x latched_x\n",
                 frame, smw_view::view_w());
    static const char* kShape[] = {"sq", "wide", "tall", "bad"};
    for (int i = 0; i < 128; ++i) {
        const std::uint32_t e = smw_view::kSoftOam + i * 8u;
        const unsigned a0 = bus_read_u16(e), a1 = bus_read_u16(e + 2),
                       a2 = bus_read_u16(e + 4);
        const SlotLog& s = g_cur[i];
        const int x9 = a1 & 0x1FF;
        std::fprintf(f, "%3d %08X %08X %08X %04X %04X %04X %3u %3d %4u %-4s %u %5d ",
                     i, s.pc0, s.pc1, s.pc2, a0, a1, a2, a0 & 0xFFu, x9, a2 & 0x3FFu,
                     kShape[(a0 >> 14) & 3], (a1 >> 14) & 3,
                     smw::oam_log_fold_x(x9));
        if (s.frame1 == frame && (s.latched & 0x1FF) == x9)
            std::fprintf(f, "%d\n", s.latched);
        else
            std::fprintf(f, "-\n");
    }
    std::fclose(f);
    std::fprintf(stderr, "[smw:oam] wrote %s\n", name);
}

void on_write(std::uint32_t pc, std::uint32_t addr, std::uint32_t,
              std::uint32_t width) {
    if (g_in_write) return;
    const unsigned long long fr = g_runtime_vblank_starts;
    if (fr != g_frame) {
        // A new frame started: the previous one's soft OAM is complete.
        if (g_frame != ~0ull &&
            (g_runtime_dump_request || (g_target && g_frame == g_target))) {
            g_runtime_dump_request = 0;
            g_in_write = true;
            dump(g_frame);
            g_in_write = false;
        }
        g_frame = fr;
    }
    const std::uint32_t off = addr - smw_view::kSoftOam;
    if (off >= 128u * 8u) return;
    SlotLog& s = g_cur[off >> 3];
    const std::uint32_t lo = off & 7u, hi = lo + (width ? width : 1) - 1;
    if (lo <= 1 && hi >= 0) s.pc0 = pc;
    if (lo <= 3 && hi >= 2) {
        s.pc1 = pc;
        s.latched = smw::oam_log_latched_x();
        s.frame1 = g_frame;
    }
    if (lo <= 5 && hi >= 4) s.pc2 = pc;
}

}  // namespace

namespace smw {
void install_oam_log() {
    const char* e = std::getenv("SMA2_OAM_LOG");
    if (!e || !*e || *e == '0') return;
    if (const char* t = std::getenv("SMA2_OAM_LOG_FRAME")) g_target = std::strtoull(t, nullptr, 10);
    std::memset(g_cur, 0, sizeof g_cur);
    g_runtime_write_observer = on_write;
    std::fprintf(stderr, "[smw:oam] log on: F10 dumps a frame to logs/\n");
}
}  // namespace smw
