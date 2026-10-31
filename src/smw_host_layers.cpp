#include "smw_host_layers.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "gba_bus.h"
#include "gba_ppu.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"

namespace smw {
namespace {

// Display-list entry at Sub0808C4A4 — full signed screen X/Y before OAM
// truncates Y to 8 bits. On PC we composite from this list; OAM is irrelevant
// for vertical placement of world sprites.
struct HostSprite {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint16_t tile = 0;
    std::uint8_t shape = 0;
    std::uint8_t size = 0;
    std::uint8_t palette = 0;
    std::uint8_t hflip = 0;
    std::uint8_t vflip = 0;
    std::uint8_t mode = 0;
};

constexpr std::uint32_t kOamSubmit = 0x0808C4A4u;
constexpr int kHudBand = 40;

std::mutex g_mu;
std::vector<HostSprite> g_building;
std::vector<HostSprite> g_latched;
void (*g_prev_fn_entry)(std::uint32_t) = nullptr;

constexpr int kSpriteWH[3][4][2] = {
    {{8, 8}, {16, 16}, {32, 32}, {64, 64}},
    {{16, 8}, {32, 8}, {32, 16}, {64, 32}},
    {{8, 16}, {8, 32}, {16, 32}, {32, 64}},
};

bool plateish(const std::uint8_t* p) {
    const int dr = (p[0] > p[1]) ? (p[0] - p[1]) : (p[1] - p[0]);
    const int dg = (p[1] > p[2]) ? (p[1] - p[2]) : (p[2] - p[1]);
    const int db = (p[0] > p[2]) ? (p[0] - p[2]) : (p[2] - p[0]);
    const int lum = (p[0] + p[1] + p[2]) / 3;
    return dr <= 14 && dg <= 14 && db <= 14 && lum >= 70 && lum <= 210;
}

void capture_sprite(std::uint32_t base) {
    if (base < 0x02000000u || base >= 0x04000000u) return;

    HostSprite s;
    s.x = static_cast<std::int32_t>(bus_read_u32(base + 0x00u));
    s.y = static_cast<std::int32_t>(bus_read_u32(base + 0x04u));
    s.hflip = bus_read_u8(base + 0x08u) & 1u;
    s.vflip = bus_read_u8(base + 0x09u) & 1u;
    s.palette = bus_read_u8(base + 0x0Au) & 0xFu;
    s.tile = static_cast<std::uint16_t>(bus_read_u16(base + 0x0Cu) & 0x3FFu);
    s.mode = bus_read_u8(base + 0x0Eu) & 0x3u;
    // +0x10 → attr0 shape; +0x11 → attr1 size (see Sub0808C4A4).
    s.shape = bus_read_u8(base + 0x10u) & 0x3u;
    s.size = bus_read_u8(base + 0x11u) & 0x3u;

    std::lock_guard<std::mutex> lock(g_mu);
    if (g_building.size() < 512u) g_building.push_back(s);
}

void on_fn_entry(std::uint32_t pc) {
    if (g_prev_fn_entry) g_prev_fn_entry(pc);
    if (pc == kOamSubmit) capture_sprite(g_cpu.R[0]);
    const char* trace = std::getenv("GBARECOMP_SMW_OBJECT_TRACE");
    if (trace && *trace && *trace != '0' &&
        (pc == 0x0808C4A4u || pc == 0x0808C70Au ||
         pc == 0x0808C716u || pc == 0x0808C900u)) {
        std::fprintf(stderr,
                     "[smw:object] frame=%llu pc=%08x r0=%08x r1=%08x "
                     "r2=%08x r5=%08x r12=%08x\n",
                     g_runtime_vblank_starts, pc, g_cpu.R[0], g_cpu.R[1],
                     g_cpu.R[2], g_cpu.R[5], g_cpu.R[12]);
    }
}

void begin_frame_capture() {
    std::lock_guard<std::mutex> lock(g_mu);
    g_building.clear();
}

void publish_capture() {
    std::lock_guard<std::mutex> lock(g_mu);
    g_latched.swap(g_building);
    g_building.clear();
    static bool dumped = false;
    const char* e = std::getenv("GBARECOMP_SMW_HOST_DUMP");
    if (!dumped && e && *e && *e != '0' && g_runtime_vblank_starts >= 400) {
        dumped = true;
        int n = 0;
        std::fprintf(stderr, "[smw:host] count=%zu frame=%llu\n",
                     g_latched.size(), g_runtime_vblank_starts);
        for (const HostSprite& s : g_latched) {
            if (s.shape >= 3) continue;
            const int sw = kSpriteWH[s.shape][s.size][0];
            const int sh = kSpriteWH[s.shape][s.size][1];
            if (s.y < -16 || s.y > 48) continue;
            std::fprintf(stderr,
                "  host xy=%d,%d %dx%d tile=%u pal=%u mode=%u\n",
                s.x, s.y, sw, sh, s.tile, s.palette, s.mode);
            if (++n >= 80) break;
        }
        std::fprintf(stderr, "[smw:host] in-band=%d\n", n);
    }
}

void punch_hud_plate(std::uint8_t* fb, std::uint32_t w, std::uint32_t h,
                     std::uint32_t /*extra_left*/, std::uint32_t extra_top) {
    // Remove residual BG3 grey plate in the peek / top band without touching
    // saturated world pixels.
    if (h < 8 || w < 8) return;
    const int band = std::min(kHudBand, static_cast<int>(h));
    for (int y = 0; y < band; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            std::uint8_t* p = fb + (static_cast<std::size_t>(y) * w + x) * 3u;
            if (!plateish(p)) continue;
            int src_y = y + 1;
            while (src_y < static_cast<int>(h)) {
                const std::uint8_t* q =
                    fb + (static_cast<std::size_t>(src_y) * w + x) * 3u;
                if (!plateish(q)) {
                    p[0] = q[0];
                    p[1] = q[1];
                    p[2] = q[2];
                    break;
                }
                ++src_y;
            }
            if (src_y >= static_cast<int>(h) && extra_top > 0 &&
                extra_top < h) {
                const std::uint8_t* q =
                    fb + (static_cast<std::size_t>(extra_top) * w + x) * 3u;
                p[0] = q[0];
                p[1] = q[1];
                p[2] = q[2];
            }
        }
    }
}

void blit_one(std::uint8_t* fb, std::uint32_t w, std::uint32_t h,
              const HostSprite& s, int dest_x, int dest_y,
              const std::uint8_t* vram, const std::uint8_t* obj_pal,
              bool obj_1d) {
    if (s.shape >= 3) return;
    const int sw = kSpriteWH[s.shape][s.size][0];
    const int sh = kSpriteWH[s.shape][s.size][1];
    if (dest_y + sh <= 0 || dest_y >= static_cast<int>(h)) return;
    if (dest_x + sw <= 0 || dest_x >= static_cast<int>(w)) return;

    constexpr std::uint32_t kObjTileBase = 0x10000u;
    const int tiles_w = sw / 8;
    const int tiles_h = sh / 8;
    for (int ty = 0; ty < tiles_h; ++ty) {
        for (int tx = 0; tx < tiles_w; ++tx) {
            const int src_tx = s.hflip ? (tiles_w - 1 - tx) : tx;
            const int src_ty = s.vflip ? (tiles_h - 1 - ty) : ty;
            const std::uint32_t this_tile =
                obj_1d
                    ? (s.tile +
                       static_cast<std::uint32_t>(src_ty * tiles_w + src_tx))
                    : (s.tile +
                       static_cast<std::uint32_t>(src_ty * 32 + src_tx));
            const std::uint32_t tile_off = kObjTileBase + this_tile * 32u;
            for (int py = 0; py < 8; ++py) {
                const int src_py = s.vflip ? (7 - py) : py;
                const int out_y = dest_y + ty * 8 + py;
                if (out_y < 0 || out_y >= static_cast<int>(h)) continue;
                for (int px = 0; px < 8; ++px) {
                    const int src_px = s.hflip ? (7 - px) : px;
                    const int out_x = dest_x + tx * 8 + px;
                    if (out_x < 0 || out_x >= static_cast<int>(w)) continue;
                    const std::uint32_t off =
                        tile_off + static_cast<std::uint32_t>(src_py) * 4u +
                        static_cast<std::uint32_t>(src_px / 2);
                    if (off + 1u > 96u * 1024u) continue;
                    std::uint8_t packed = vram[off];
                    std::uint8_t idx =
                        (src_px & 1) ? (packed >> 4) : (packed & 0x0F);
                    if (idx == 0) continue;
                    idx = static_cast<std::uint8_t>(
                        idx | static_cast<std::uint8_t>(s.palette << 4));
                    const std::uint16_t color = static_cast<std::uint16_t>(
                        obj_pal[idx * 2u] |
                        (static_cast<std::uint16_t>(obj_pal[idx * 2u + 1u])
                         << 8));
                    std::uint8_t* dst =
                        fb + (static_cast<std::size_t>(out_y) * w +
                              static_cast<std::uint32_t>(out_x)) *
                                 3u;
                    dst[0] = static_cast<std::uint8_t>(
                        ((color & 0x1Fu) * 255 + 15) / 31);
                    dst[1] = static_cast<std::uint8_t>(
                        (((color >> 5) & 0x1Fu) * 255 + 15) / 31);
                    dst[2] = static_cast<std::uint8_t>(
                        (((color >> 10) & 0x1Fu) * 255 + 15) / 31);
                }
            }
        }
    }
}

bool is_hud_digit(const HostSprite& s) {
    if (s.shape >= 3) return false;
    const int sw = kSpriteWH[s.shape][s.size][0];
    const int sh = kSpriteWH[s.shape][s.size][1];
    // Status digits live in the native top band; leave them for HUD pin.
    return s.y >= 0 && s.y + sh <= kHudBand && sw <= 8 && sh <= 16;
}

void blit_host_sprites(std::uint8_t* fb, std::uint32_t w, std::uint32_t h,
                       std::uint32_t extra_left, std::uint32_t extra_top) {
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus) return;

    const std::uint8_t* vram = bus->vram_ptr();
    const std::uint8_t* pal = bus->pal_ptr();
    if (!vram || !pal) return;

    const std::uint8_t* obj_pal = pal + 0x200;
    const std::uint16_t dispcnt = bus->io().read16(0);
    const bool obj_1d = (dispcnt & 0x0040u) != 0;

    std::vector<HostSprite> sprites;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        sprites = g_latched;
    }

    const int bias_x = gba::g_ws_scroll_bias_x;
    const int bias_y = gba::g_ws_scroll_bias_y;
    for (const HostSprite& s : sprites) {
        if (is_hud_digit(s)) continue;
        const int dest_x = s.x + static_cast<int>(extra_left) - bias_x;
        // Match PPU: status-band sprites stay; world follows Y bias.
        int dest_y = s.y + static_cast<int>(extra_top);
        if (!(s.y >= 0 && s.y < 80))
            dest_y -= bias_y;
        blit_one(fb, w, h, s, dest_x, dest_y, vram, obj_pal, obj_1d);
    }
}

void pin_hud_digits(std::uint8_t* fb, std::uint32_t w, std::uint32_t h,
                    std::uint32_t extra_left, std::uint32_t /*extra_top*/) {
    // OBJ-only status glyphs pinned to the tall-FB top (oy∈[0,kHudBand)).
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus) return;
    const std::uint8_t* vram = bus->vram_ptr();
    const std::uint8_t* pal = bus->pal_ptr();
    if (!vram || !pal) return;
    const std::uint8_t* obj_pal = pal + 0x200;
    const std::uint16_t dispcnt = bus->io().read16(0);
    const bool obj_1d = (dispcnt & 0x0040u) != 0;

    std::vector<HostSprite> sprites;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        sprites = g_latched;
    }

    const int bias_x = gba::g_ws_scroll_bias_x;
    const int player_sx = gba::g_ws_player_screen_x;
    for (const HostSprite& s : sprites) {
        if (!is_hud_digit(s)) continue;
        // Avoid stomping Mario when he straddles the status band.
        if (player_sx > -32 && player_sx < 272) {
            const int sw = kSpriteWH[s.shape][s.size][0];
            const int dx = s.x - player_sx;
            if (dx > -24 && dx < sw + 24 && s.y < 32) continue;
        }
        const int dest_x = s.x + static_cast<int>(extra_left) - bias_x;
        const int dest_y = s.y;  // pin to tall-FB top, not +extra_top
        blit_one(fb, w, h, s, dest_x, dest_y, vram, obj_pal, obj_1d);
    }
}

bool is_item_box(const HostSprite& s) {
    if (s.shape >= 3) return false;
    const int sw = kSpriteWH[s.shape][s.size][0];
    const int sh = kSpriteWH[s.shape][s.size][1];
    // Reserve item box: 16×16 pair near X≈27, Y≈44/60 (hangs under status).
    return sw == 16 && sh == 16 && s.y >= 40 && s.y < 80 && s.x >= 8 &&
           s.x < 56;
}

void pin_item_box(std::uint8_t* fb, std::uint32_t w, std::uint32_t h,
                  std::uint32_t extra_left, std::uint32_t extra_top) {
    if (extra_top == 0) return;
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus) return;
    const std::uint8_t* vram = bus->vram_ptr();
    const std::uint8_t* pal = bus->pal_ptr();
    if (!vram || !pal) return;
    const std::uint8_t* obj_pal = pal + 0x200;
    const std::uint16_t dispcnt = bus->io().read16(0);
    const bool obj_1d = (dispcnt & 0x0040u) != 0;
    // Prefer soft-OAM (latched each frame) over host display-list.
    const std::uint8_t* oam = bus->oam_ptr();
    if (!oam) return;
    const int bias_x = gba::g_ws_scroll_bias_x;
    for (int idx = 0; idx < 128; ++idx) {
        const std::uint8_t* e = oam + idx * 8;
        const std::uint16_t a0 =
            static_cast<std::uint16_t>(e[0] | (std::uint16_t(e[1]) << 8));
        const std::uint16_t a1 =
            static_cast<std::uint16_t>(e[2] | (std::uint16_t(e[3]) << 8));
        const std::uint16_t a2 =
            static_cast<std::uint16_t>(e[4] | (std::uint16_t(e[5]) << 8));
        if ((a0 & 0x0300u) == 0x0200u) continue;  // disabled
        const int shape = (a0 >> 14) & 3;
        const int size = (a1 >> 14) & 3;
        if (shape >= 3) continue;
        HostSprite s{};
        s.x = static_cast<int>(a1 & 0x1FFu);
        if (s.x & 0x100) s.x -= 0x200;
        s.y = static_cast<int>(a0 & 0xFFu);
        if (s.y >= 160) s.y -= 256;
        s.shape = shape;
        s.size = size;
        s.tile = a2 & 0x3FFu;
        s.palette = (a2 >> 12) & 0xFu;
        s.hflip = (a1 & 0x1000u) != 0;
        s.vflip = (a1 & 0x2000u) != 0;
        s.mode = (a0 >> 10) & 3u;
        if (!is_item_box(s)) continue;
        const int dest_x = s.x + static_cast<int>(extra_left) - bias_x;
        const int dest_y = s.y;  // FB top, not +extra_top
        // Peek already sits at oy=s.y; keep it to punch the mid ghost.
        std::uint8_t under[16 * 16 * 3];
        for (int ly = 0; ly < 16; ++ly) {
            for (int lx = 0; lx < 16; ++lx) {
                const int sx = dest_x + lx;
                const int sy = dest_y + ly;
                std::uint8_t* u = under + (ly * 16 + lx) * 3;
                if (sx < 0 || sy < 0 || sx >= static_cast<int>(w) ||
                    sy >= static_cast<int>(h)) {
                    u[0] = u[1] = u[2] = 0;
                    continue;
                }
                const std::uint8_t* p =
                    fb + (static_cast<std::size_t>(sy) * w +
                          static_cast<std::uint32_t>(sx)) *
                             3u;
                u[0] = p[0];
                u[1] = p[1];
                u[2] = p[2];
            }
        }
        blit_one(fb, w, h, s, dest_x, dest_y, vram, obj_pal, obj_1d);
        const int mid_y0 = s.y + static_cast<int>(extra_top);
        for (int ly = 0; ly < 16; ++ly) {
            const int my = mid_y0 + ly;
            if (my < 0 || my >= static_cast<int>(h)) continue;
            for (int lx = 0; lx < 16; ++lx) {
                const int mx = dest_x + lx;
                if (mx < 0 || mx >= static_cast<int>(w)) continue;
                const std::uint8_t* u = under + (ly * 16 + lx) * 3;
                std::uint8_t* dst =
                    fb + (static_cast<std::size_t>(my) * w +
                          static_cast<std::uint32_t>(mx)) *
                             3u;
                dst[0] = u[0];
                dst[1] = u[1];
                dst[2] = u[2];
            }
        }
    }
}

// --- Status detach (tall view) ------------------------------------------------
// Continuous tall FB keeps HUD in the middle (guest hy≈0 + extra_top). Instead
// of re-rendering those scanlines (shears BG), we:
//   1) punch the mid status band with the already-rendered peek rows above it
//   2) paint ONLY the stamp tile buffers (level+0x34 / +0x6C) at FB top
//   3) pin the reserve item-box OBJ the same way
// Peek→mid is world filling a hole, not copying the HUD strip upward.

constexpr int kStatusDetachBand = 40;
constexpr std::uint32_t kLevelObj = 0x03003F98u;
constexpr std::uint32_t kStampRow0 = 0x34u;
constexpr std::uint32_t kStampRow1 = 0x6Cu;
constexpr int kStampTiles = 0x1C;  // Sub0800A7EA copies 28 entries

void punch_mid_with_peek(std::uint8_t* fb, std::uint32_t w, std::uint32_t h,
                         std::uint32_t extra_top, int band) {
    if (extra_top == 0 || band <= 0) return;
    const int b = std::min(band, static_cast<int>(extra_top));
    if (extra_top + static_cast<std::uint32_t>(b) > h) return;
    const std::size_t row_bytes =
        static_cast<std::size_t>(w) * 3u;
    for (int i = 0; i < b; ++i) {
        const std::uint8_t* src =
            fb + static_cast<std::size_t>(i) * row_bytes;
        std::uint8_t* dst =
            fb + static_cast<std::size_t>(extra_top + static_cast<std::uint32_t>(i)) *
                     row_bytes;
        std::memcpy(dst, src, row_bytes);
    }
}

void blit_bg_entry(std::uint8_t* fb, std::uint32_t w, std::uint32_t h,
                   int dest_x, int dest_y, std::uint16_t entry,
                   const std::uint8_t* vram, const std::uint8_t* pal,
                   std::uint32_t char_base, bool color256) {
    const std::uint32_t tile_num = entry & 0x03FFu;
    if (tile_num == 0) return;
    // Blank fillers from the status stamp path (tile and full entry forms).
    if (tile_num == 0x0FCu || tile_num == 0x08Eu || entry == 0x108Eu) return;
    const bool hflip = (entry & 0x0400u) != 0;
    const bool vflip = (entry & 0x0800u) != 0;
    const std::uint32_t pal_bank = (entry >> 12) & 0xFu;
    for (int py = 0; py < 8; ++py) {
        const int src_py = vflip ? (7 - py) : py;
        const int out_y = dest_y + py;
        if (out_y < 0 || out_y >= static_cast<int>(h)) continue;
        for (int px = 0; px < 8; ++px) {
            const int src_px = hflip ? (7 - px) : px;
            const int out_x = dest_x + px;
            if (out_x < 0 || out_x >= static_cast<int>(w)) continue;
            std::uint8_t pal_idx = 0;
            if (color256) {
                const std::uint32_t addr =
                    char_base + tile_num * 64u +
                    static_cast<std::uint32_t>(src_py) * 8u +
                    static_cast<std::uint32_t>(src_px);
                if (addr >= 96u * 1024u) continue;
                pal_idx = vram[addr];
                if (pal_idx == 0) continue;
            } else {
                const std::uint32_t addr =
                    char_base + tile_num * 32u +
                    static_cast<std::uint32_t>(src_py) * 4u +
                    static_cast<std::uint32_t>(src_px >> 1);
                if (addr >= 96u * 1024u) continue;
                const std::uint8_t packed = vram[addr];
                pal_idx = (src_px & 1) ? (packed >> 4) : (packed & 0x0F);
                if (pal_idx == 0) continue;
                pal_idx = static_cast<std::uint8_t>(
                    pal_idx | static_cast<std::uint8_t>(pal_bank << 4));
            }
            const std::uint16_t color = static_cast<std::uint16_t>(
                pal[pal_idx * 2u] |
                (static_cast<std::uint16_t>(pal[pal_idx * 2u + 1u]) << 8));
            std::uint8_t* dst =
                fb + (static_cast<std::size_t>(out_y) * w +
                      static_cast<std::uint32_t>(out_x)) *
                         3u;
            dst[0] = static_cast<std::uint8_t>(((color & 0x1Fu) * 255 + 15) / 31);
            dst[1] = static_cast<std::uint8_t>(
                (((color >> 5) & 0x1Fu) * 255 + 15) / 31);
            dst[2] = static_cast<std::uint8_t>(
                (((color >> 10) & 0x1Fu) * 255 + 15) / 31);
        }
    }
}

void overlay_stamp_status(std::uint8_t* fb, std::uint32_t w, std::uint32_t h,
                          std::uint32_t extra_left) {
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus) return;
    const std::uint8_t* vram = bus->vram_ptr();
    const std::uint8_t* pal = bus->pal_ptr();
    const std::uint8_t* io = bus->io().raw();
    if (!vram || !pal || !io) return;

    const std::uint32_t level = bus_read_u32(kLevelObj);
    if (level < 0x02000000u || level >= 0x04000000u) return;

    const std::uint16_t bg0cnt = static_cast<std::uint16_t>(
        io[0x08] | (static_cast<std::uint16_t>(io[0x09]) << 8));
    const std::uint32_t char_base = ((bg0cnt >> 2) & 0x3u) * 0x4000u;
    const bool color256 = (bg0cnt & 0x80u) != 0;
    const int bias_x = gba::g_ws_scroll_bias_x;
    // Stamp DMA skips the first map entry (+2): start at guest x = 8.
    const int base_x =
        static_cast<int>(extra_left) - bias_x + 8;

    for (int row = 0; row < 2; ++row) {
        const std::uint32_t off = (row == 0) ? kStampRow0 : kStampRow1;
        const int dest_y = row * 8;
        for (int i = 0; i < kStampTiles; ++i) {
            const std::uint16_t entry = bus_read_u16(level + off +
                                                    static_cast<std::uint32_t>(i) * 2u);
            blit_bg_entry(fb, w, h, base_x + i * 8, dest_y, entry, vram, pal,
                          char_base, color256);
        }
    }
}

bool in_level_status(const std::uint8_t* io) {
    if (!io) return false;
    const std::uint16_t dispcnt = static_cast<std::uint16_t>(
        io[0] | (static_cast<std::uint16_t>(io[1]) << 8));
    // Title / file select often 0x1600; in-level typically has BG0+BG1+OBJ.
    if (dispcnt == 0x1600u) return false;
    return (dispcnt & 0x1300u) == 0x1300u;
}

void on_post_latch(std::uint8_t* fb, std::uint32_t w, std::uint32_t h,
                   std::uint32_t extra_left, std::uint32_t extra_top) {
    publish_capture();
    if (!fb || w == 0 || h == 0) return;
    (void)extra_left;
    (void)extra_top;
    if (gba::g_ws_host_obj_compose)
        blit_host_sprites(fb, w, h, extra_left, extra_top);
    begin_frame_capture();
}

}  // namespace

void install_host_layers() {
    g_prev_fn_entry = g_runtime_fn_entry_hook;
    g_runtime_fn_entry_hook = on_fn_entry;
    gba::g_ws_post_latch_hook = on_post_latch;
    gba::g_ws_host_obj_compose = (std::getenv("GBARECOMP_SMW_HOST_LAYERS") &&
        std::getenv("GBARECOMP_SMW_HOST_LAYERS")[0] != '0') ? 1 : 0;
    begin_frame_capture();
}

}  // namespace smw
