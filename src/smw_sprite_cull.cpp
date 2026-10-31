#include "smw_sprite_cull.h"
#include "smw_view.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include "gba_ppu.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"

extern "C" unsigned g_ws_extra_left;
extern "C" unsigned g_ws_extra_right;

// Objects stay alive this far past each edge of the picture.
constexpr unsigned kViewportSlack = 16u;
// Two 16px blocks above the picture. Sides stay at one block.
constexpr unsigned kYMargin = 32u;

// OAM / display-list submit AABB (Sub0808C4A4 and twins). Stock:
//   X ∈ [-32, 240], Y ∈ [-32, 160].
// Wide: X max is the full host view (240+el+er), not native+er only —
// when bias==extra_left the FB right edge is guest screen_x ≈ view_w.
extern "C" std::uint32_t smw_ws_sprite_cull_xmax(void) {
    return static_cast<unsigned>(smw_view::view_w()) + 32u + kViewportSlack;
}

extern "C" std::uint32_t smw_ws_sprite_cull_ymax(void) {
    // Same byte as a row above the glass. Anything past the last
    // picture row must not be submitted, or it is drawn at the top.
    const unsigned bottom = static_cast<unsigned>(smw_view::kNativeH) + gba::g_ws_extra_bottom;
    return bottom > 0u ? bottom - 1u : 0u;
}

extern "C" std::uint32_t smw_ws_sprite_cull_overhang(void) {
    // ROM uses one negated constant for both X and Y mins. Widen by the
    // larger margin — do not add left+top (over-opens one axis).
    return 32u + std::max(g_ws_extra_left, gba::g_ws_extra_top) + kViewportSlack;
}

void smw_ws_remember_margin_row(void);

std::uint32_t smw_ws_sprite_y_lo(std::uint32_t stock) {
    // rN still holds the signed row. The store that follows keeps only
    // the byte, which the picture would draw under the floor.
    smw_ws_remember_margin_row();
    return stock + gba::g_ws_extra_top + kYMargin;
}

std::uint32_t smw_ws_sprite_y_above(std::uint32_t stock) {
    // The compare is on the top of the tile. Stopping at the picture
    // drops a tile that only sticks out a few pixels. The margin is
    // how far past that edge the top may sit and still be drawn.
    const unsigned top = gba::g_ws_extra_top + kYMargin;
    return stock > top ? stock : top;
}

std::uint32_t smw_ws_sprite_y_floor(void) {
    // 160 + extra_bottom - 1. Inclusive: a tile on the last row stays.
    const unsigned bottom = static_cast<unsigned>(smw_view::kNativeH) + gba::g_ws_extra_bottom;
    return bottom > 0u ? bottom - 1u : 0u;
}

std::uint32_t smw_ws_sprite_y_span(void) {
    return 0xDFu + gba::g_ws_extra_top + kYMargin;
}

std::uint32_t smw_ws_sprite_y_head_pad(void) {
    return (24u + gba::g_ws_extra_top + kYMargin) << 16;
}

std::uint32_t smw_ws_fold_peek_row(std::uint32_t y) {
    if (gba::g_ws_player_screen_y >= smw_view::kNativeH) return y;
    if (y >= 192u && y <= 255u)
        return static_cast<std::uint32_t>(static_cast<int>(y) - 256);
    return y;
}

int smw_ws_player_y_in_view(std::uint32_t y16) {
    const int sy = static_cast<std::int16_t>(y16 & 0xFFFFu);
    const int top = -static_cast<int>(gba::g_ws_extra_top) -
                    static_cast<int>(kYMargin);
    const int bot = smw_view::kNativeH + static_cast<int>(gba::g_ws_extra_bottom) + 80;
    return (sy >= top && sy < bot) ? 1 : 0;
}

// The compare sees (screen Y + 31) in 16 bits. A row above the glass is
// negative there, and the stock test throws it out. Remember the signed
// row when it still belongs in the tall view, including one tile above
// the picture so a sprite that crosses the top line is submitted.
void smw_ws_arm_kept_row(int keep, int screen_y);

int smw_ws_row_above_in_view(std::uint32_t raw) {
    const int s = static_cast<std::int16_t>(raw & 0xFFFFu);
    // A later on-screen tile is tested before this one is stored.
    // Leaving the arm up is what lets the store keep the signed row.
    if (s >= 0) return 0;
    const int top = -static_cast<int>(gba::g_ws_extra_top) -
                    static_cast<int>(kYMargin);
    const int screen_y = s - 31;
    const int keep = s >= top + 31 ? 1 : 0;
    if (keep) smw_ws_arm_kept_row(1, screen_y);
    return keep;
}

int smw_ws_signed_x_in_view(std::uint32_t raw) {
    const int px = static_cast<std::int32_t>(raw) >> 16;
    if (px >= 0) return 0;
    const int left = -static_cast<int>(g_ws_extra_left) -
                     static_cast<int>(kViewportSlack) - 48;
    return px >= left ? 1 : 0;
}

std::uint32_t smw_ws_keep_screen_y(std::uint32_t oam, std::int32_t y) {
    int sy = y;
    if (sy >= 0 && sy <= 0xFFFF && (sy & 0x8000))
        sy = static_cast<std::int16_t>(sy);
    else if (sy >= smw_view::kNativeH && sy <= 255)
        sy -= 256;
    // Death fall: the real row is under the floor. Leave the hide.
    if (gba::g_ws_player_screen_y >= smw_view::kNativeH) return 0xC0u;
    const int top = -static_cast<int>(gba::g_ws_extra_top) -
                    static_cast<int>(kYMargin);
    if (sy < top || sy >= smw_view::kNativeH) return 0xC0u;
    smw_ws_note_obj_y(oam, sy);
    return static_cast<std::uint32_t>(sy) & 0xFFu;
}

// screen_x is sprite − Layer1. Layer1 is the left of the view, so the
// visible range is the view itself, plus the stock slack outside it.
extern "C" int smw_ws_sprite_offscreen_x(std::int32_t screen_x) {
    const int xmin = -16 - static_cast<int>(g_ws_extra_left) - 48 -
                     static_cast<int>(kViewportSlack);
    const int xmax =
        239 + static_cast<int>(g_ws_extra_left + g_ws_extra_right) + 48 +
        static_cast<int>(kViewportSlack);
    return (screen_x < xmin || screen_x > xmax) ? 1 : 0;
}

extern "C" int smw_ws_sprite_offscreen_x_u8(std::int32_t screen_x) {
    const int xmin = -static_cast<int>(g_ws_extra_left) - 48 -
                     static_cast<int>(kViewportSlack);
    const int xmax =
        255 + static_cast<int>(g_ws_extra_left + g_ws_extra_right) + 48 +
        static_cast<int>(kViewportSlack);
    return (screen_x < xmin || screen_x > xmax) ? 1 : 0;
}

// Right edge used by the per-sprite hide (`cmp` against 0x012F / 0x013F).
// Stock 0x012F sits 63 past the 240 glass; the same slack past the view.
extern "C" std::uint32_t smw_ws_sprite_x_limit(std::uint32_t stock) {
    return stock + g_ws_extra_left + g_ws_extra_right + kViewportSlack;
}

// Rex tile gate (Sub08044F98). Stock keeps a tile when its screen Y is in
// [-32, 191]. The wide view starts 64 rows above Y=0, and a tall room shifts
// sprite Y down by that same amount after this compare. 191 + extra_top and
// -(32 + extra_top) keep the stock 32px slack on both sides of that view.
extern "C" std::uint32_t smw_ws_rex_y_hi(void) {
    return 0xBFu + gba::g_ws_extra_top;
}

extern "C" std::uint32_t smw_ws_rex_y_lo(void) {
    return 0x20u + gba::g_ws_extra_top;
}

// Draw skip in 16.16. Stock edges are 0x011F0000 (pixel 287) and
// 0x01300000 (pixel 304): 47 and 64 px past a 240-wide view. The
// compared value already includes whatever bias the drawer added.
std::uint32_t smw_ws_wide_edge(std::uint32_t rom_limit) {
    const std::uint32_t px =
        (rom_limit >> 16) + g_ws_extra_left + g_ws_extra_right + kViewportSlack;
    return px << 16;
}

// Sub08036E94 backs Layer1 by this many px before the 16px column walk.
extern "C" std::uint32_t smw_ws_spawn_sweep_back(void) {
    return 0x60u + g_ws_extra_left;
}

// Last column index (stock 0x1F). Span must reach view_w+48 past the
// restored Layer1 so the right peek is populated at level load.
extern "C" std::uint32_t smw_ws_spawn_sweep_last(void) {
    const unsigned back = 0x60u + g_ws_extra_left;
    const unsigned right =
        static_cast<unsigned>(smw_view::view_w()) + 48u;
    const unsigned span = back + right;
    const unsigned steps = (span + 15u) / 16u;
    const unsigned last = steps > 0u ? (steps - 1u) : 0u;
    static unsigned hits = 0;
    if (hits < 4) {
        std::fprintf(stderr, "[smw:spawn] sweep_back=%u last=%u\n", back, last);
        ++hits;
    }
    return last;
}

// Facing stores the original points. Spawn reads them. The wide measure
// is in the follow's screen subtract, not here.
extern "C" std::uint32_t smw_ws_deadzone_face_right(void) {
    return 0x4Eu;
}
extern "C" std::uint32_t smw_ws_deadzone_face_left(void) {
    return 0x80u;
}

// Sub08006434 screen X. Original is player - Layer1, inside 240.
// The 240 window sits in the middle of the view, so the same point is
// player - (Layer1 + extra). Layer1 stays the left edge of the view:
// at the start of the room it remains 0 until that point is crossed.
extern "C" std::uint32_t smw_ws_screen_x(std::uint32_t player, std::uint32_t cam) {
    return player - cam - g_ws_extra_left;
}

// Original room max adds 16 to (screens-1)*256, which is room_w-240.
// The view is wider by extra on both sides, so the max is room_w-view_w.
extern "C" std::uint32_t smw_ws_cam_span_max(std::uint32_t base) {
    return base + 16u - (g_ws_extra_left + g_ws_extra_right);
}

// Sub08006340 tail. Stock clamps Layer1 Y to the level's camera max
// (0x03002BA8) for a 160-row screen. The wide view shows extra_bottom rows
// below Layer1+160, so the same max minus those rows keeps the view's bottom
// on the world's bottom (same idea as cam_span_max in X).
extern "C" std::uint32_t smw_ws_cam_y_max(std::uint32_t max_y) {
    const std::int32_t v = static_cast<std::int16_t>(max_y);
    const std::int32_t eb = static_cast<std::int32_t>(gba::g_ws_extra_bottom);
    if (eb <= 0 || v <= 0) return max_y;
    return static_cast<std::uint32_t>(std::max(0, v - eb));
}

// Sub0806B7C8. Stock right barrier is Layer1 + 0xE8 (or + 0xE0 for a
// few block types). 0xE8 is 8px in from the right of the 240 screen
// (240-8). Layer1 is the left of the wide view, so the same inset from
// the right of the view is inset + (view_w-240).
extern "C" std::uint32_t smw_ws_screen_barrier(std::uint32_t layer, std::uint32_t inset) {
    return layer + inset + (g_ws_extra_left + g_ws_extra_right);
}

// Sub0806B834. Stock loads the low byte of screen X (player − Layer1)
// and kills when that byte is 0xF0..0xFD: just past the right of the 240
// screen, and the same bytes for 16..3px left of the camera. The push at
// 0xE8 used to stop him before the right band. The wide right edge is
// 0xF0 plus the extra width. The left band stays.
// The caller compares the result with 0x0D and kills when it is not above.
extern "C" std::uint32_t smw_ws_offscreen_x_test(std::uint32_t addr) {
    const std::int32_t sx = static_cast<std::int16_t>(bus_read_u16(addr));
    const std::int32_t right =
        0xF0 + static_cast<std::int32_t>(g_ws_extra_left + g_ws_extra_right);
    const bool kill = (sx >= right) || (sx >= -16 && sx <= -3);
    return kill ? 0u : 0x0Eu;
}

// Sub080381F8. Using the reserve spawns it at Layer1 + (0x70, 0):
// the top of the 240-wide glass, under the item box. Layer1 is the
// left of the wide view and the box sits extra_top above that glass,
// centered by extra_left. The same offsets, on the spawn itself.
extern "C" std::uint32_t smw_ws_reserve_drop_x(std::uint32_t layer_x) {
    return layer_x + 0x70u + g_ws_extra_left;
}
extern "C" std::uint32_t smw_ws_reserve_drop_y(std::uint32_t layer_y) {
    return layer_y - gba::g_ws_extra_top +
        static_cast<std::uint32_t>(gba::kStatusTopMargin);
}

// Sub08009EA0. While a reserve is stored, this writes OAM entry 0:
// X is 0x70, Y is the bobbing value the game just negated into r0.
// Those are screen coordinates on the 240x160 glass. The brackets now
// sit extra_left to the right and extra_top above that glass, so the
// same OAM byte is raised by extra_top and the X gains extra_left.
// kStatusTopMargin drops it with the plate, so it stays inside the brackets.
extern "C" std::uint32_t smw_ws_reserve_icon_x(void) {
    return 0x70u + g_ws_extra_left;
}
extern "C" std::uint32_t smw_ws_reserve_icon_y(std::uint32_t y) {
    return (y - gba::g_ws_extra_top +
            static_cast<std::uint32_t>(gba::kStatusTopMargin)) & 0xFFu;
}

// Sub0806D420. Stock subtracts 0xC8 and the next compare kills when the
// result is >= 0, so screen Y >= 200: forty pixels under the 160 glass.
// This view is bottom-anchored (the extra rows sit above the camera), so
// the floor of the picture is still that glass. Killing only at 256 left
// him alive while the 8-bit OAM Y wrapped into the rows above — garbage
// in the sky for the whole fall. extra_bottom, if it ever opens under the
// floor, moves the same forty-pixel grace down with it. Negative screen Y
// is above the camera and stays alive. The caller does cmps #0 / blt.
extern "C" std::uint32_t smw_ws_offscreen_y_test(std::uint32_t screen_y) {
    const std::int32_t sy = static_cast<std::int32_t>(screen_y);
    const int limit = 200 + static_cast<int>(gba::g_ws_extra_bottom);
    if (sy >= limit) {
        static int hits = 0;
        if (hits < 8) {
            const int py = static_cast<std::int16_t>(bus_read_u16(0x03003FD4u));
            const int ly = static_cast<std::int16_t>(bus_read_u16(smw_view::kLayer1Y));
            std::fprintf(stderr, "[smw:pit] screen_y=%d player_y=%d L1y=%d\n",
                         sy, py, ly);
            ++hits;
        }
    }
    return static_cast<std::uint32_t>(sy - limit);
}

// Y-capture logging removed with the per-slot Y notes.
void smw_ws_log_y_real(std::uint32_t, int, std::uint32_t) {}
void smw_ws_log_y_decide(std::uint32_t) {}

// Soft-OAM hide pads screen_x by this before cmp 0x12F (stock 32<<16).
extern "C" std::uint32_t smw_ws_sprite_vis_x_pad_shl16(void) {
    return (32u + g_ws_extra_left + kViewportSlack) << 16;
}

// Sub0802F678 and the two copies. The stock test is (screenY+pad)&0xFF00,
// a 256px window. The view is 224 tall and sits extra_top above screen Y 0,
// so that window marks the top rows off and the sprite never starts.
extern "C" std::uint32_t smw_ws_sprite_offscreen_y_mask(std::uint32_t sprite) {
    const int sy = static_cast<std::int16_t>(bus_read_u16(sprite + 0x12u));
    const int ly = static_cast<std::int16_t>(bus_read_u16(smw_view::kLayer1Y));
    const int rel = sy - ly;
    const int top = -48 - static_cast<int>(gba::g_ws_extra_top) -
                    static_cast<int>(kYMargin);
    const int bot = smw_view::kNativeH + static_cast<int>(gba::g_ws_extra_bottom) + 48 +
                    static_cast<int>(kViewportSlack);
    const std::uint32_t off = (rel < top || rel > bot) ? 0x100u : 0u;
    smw_ws_log_y_real(sprite, rel, off);
    return off;
}

// Spawning is left to the game: its own spawn/despawn bounds are widened
// once from extra_left/right (widen_load_table). The old per-column forced
// spawn walk caused duplicated sprites; kept as a no-op for generated calls.
extern "C" void smw_ws_spawn_other_edge(void) {}

namespace {

void (*g_previous_fn_entry)(std::uint32_t) = nullptr;
RuntimeBusReadOverride g_previous_bus_read = nullptr;

constexpr std::uint32_t kLoadEd6 = 0x08100ED6u;
constexpr std::uint32_t kLoadEe2 = 0x08100EE2u;

int widen_load_table(std::uint32_t addr, std::uint32_t width,
                     std::uint32_t original, std::uint32_t* out) {
    if (!out) return 0;

    // Literal pools live in the cart. The same numbers also appear as live
    // coordinates in RAM: player X/Y == 0x012F (303) was being read back as
    // 303+158 and then stored, which is the teleport on both axes.
    const bool rom_literal = addr >= 0x08000000u && addr < 0x0E000000u;

    // Far-right flag threshold 0x013F << 16 in literal pools.
    // Stock 0x13F = 319 ≈ offscreen right. Widen by full view extras.
    if (rom_literal && width == 4u && original == 0x013F0000u) {
        static unsigned hits = 0;
        if (hits < 3) {
            std::fprintf(stderr, "[smw:widen] 013F hit #%u -> %u\n", hits,
                         0x13Fu + g_ws_extra_left + g_ws_extra_right);
            ++hits;
        }
        *out = (0x13Fu + g_ws_extra_left + g_ws_extra_right) << 16;
        return 1;
    }

    // Soft-OAM / draw hide: (sx+32) > 0x12F → park Y at 0xC0.
    // Stock 0x12F=303 ⇒ sx>271. Wide must reach view_w+slack.
    if (rom_literal && width == 4u && original == 0x012F0000u) {
        *out = (0x12Fu + g_ws_extra_left + g_ws_extra_right) << 16;
        return 1;
    }
    if (rom_literal && width == 2u && original == 0x012Fu) {
        *out = 0x12Fu + g_ws_extra_left + g_ws_extra_right;
        return 1;
    }

    if (width != 2u) return 0;

    // Sprite keep-distance (Sub0802EE08 / EED8 / EFA8). Even entries are the
    // right side: stock 304 deletes a koopa that is still inside the 398 view.
    // Moles and chucks use 416, which already clears 398. Odd entries are the
    // left side and stay as they are.
    {
        struct Span { std::uint32_t base; unsigned halfs; };
        const Span spans[] = {
            {0x080FF7D8u, 10u},
            {0x080FF7ECu, 10u},
            {0x080FF800u, 8u},
        };
        for (const Span& s : spans) {
            if (addr < s.base || addr >= s.base + s.halfs * 2u) continue;
            const unsigned idx = (addr - s.base) / 2u;
            if (idx & 1u) return 0;
            const int stock = static_cast<std::int16_t>(original & 0xFFFFu);
            if (stock <= 0) return 0;
            const int wide = stock + static_cast<int>(g_ws_extra_left + g_ws_extra_right);
            static int noted = 0;
            if (noted < 4) {
                std::fprintf(stderr, "[smw:spawn] keep-right %d -> %d\n", stock, wide);
                ++noted;
            }
            *out = static_cast<std::uint32_t>(static_cast<std::uint16_t>(
                static_cast<std::int16_t>(wide)));
            return 1;
        }
    }

    if (addr >= kLoadEd6 && addr < kLoadEd6 + 12u &&
        ((addr - kLoadEd6) & 1u) == 0u) {
        // Data08100ED6: [Xleft, Xright, 0, Ytop, Ybot, 0]
        //   X: −48 / +288   Y: −48 / +208 (=160+48)
        // Bulk init (Sub08036E94) forces level+0x1C=2 → ED6[2]=0 and walks
        // Layer1; incremental Sub08036A9C uses ED6[0]. Widening ED6[0] left
        // by extra_left makes (L1+offset)<0 → early return until L1 is huge,
        // so enemies never re-load. Keep left stock; widen right + Y top.
        const unsigned idx =
            static_cast<unsigned>((addr - kLoadEd6) / 2u);
        const int extra_b = 0;  // bottom-anchor; no g_ws_extra_bottom yet
        static unsigned ed6_hits = 0;
        auto note = [&](int v) {
            if (ed6_hits < 8) {
                std::fprintf(stderr, "[smw:widen] ED6[%u]=%d\n", idx, v);
                ++ed6_hits;
            }
        };
        switch (idx) {
        case 1: {
            // Stock 288 = 240+48. Wide: view_w+48.
            const int v =
                288 + static_cast<int>(g_ws_extra_left + g_ws_extra_right);
            note(v);
            *out = static_cast<std::uint32_t>(static_cast<std::uint16_t>(
                static_cast<std::int16_t>(v)));
            return 1;
        }
        case 3: {
            const int v = -48 - static_cast<int>(gba::g_ws_extra_top);
            note(v);
            *out = static_cast<std::uint32_t>(static_cast<std::uint16_t>(
                static_cast<std::int16_t>(v)));
            return 1;
        }
        case 4: {
            const int v = 208 + extra_b;
            note(v);
            *out = static_cast<std::uint32_t>(static_cast<std::uint16_t>(
                static_cast<std::int16_t>(v)));
            return 1;
        }
        default:
            (void)original;
            return 0;
        }
    }

    if (addr >= kLoadEe2 && addr < kLoadEe2 + 4u &&
        ((addr - kLoadEe2) & 1u) == 0u) {
        // Far X window (−96 / +336). Same early-out risk on the left —
        // only widen the right edge.
        const unsigned idx =
            static_cast<unsigned>((addr - kLoadEe2) / 2u);
        if (idx == 1u) {
            *out = static_cast<std::uint32_t>(static_cast<std::uint16_t>(
                static_cast<std::int16_t>(
                    336 + static_cast<int>(g_ws_extra_left + g_ws_extra_right))));
            return 1;
        }
    }
    return 0;
}

int playfield_bus_read(std::uint32_t pc, std::uint32_t addr, std::uint32_t width,
                       std::uint32_t original, std::uint32_t* out) {
    if (widen_load_table(addr, width, original, out)) return 1;
    if (g_previous_bus_read)
        return g_previous_bus_read(pc, addr, width, original, out);
    return 0;
}

// Signed 9-bit OAM X re-centred on the wide view: the unused band between
// the right margin and the left margin decides the fold point, so a guest X
// in [-(extra_left+slack), 240+extra_right+slack] is unambiguous.
int wide_obj_x(int raw_x, int* out_x) {
    if (!out_x) return 0;
    const int el = static_cast<int>(g_ws_extra_left);
    const int er = static_cast<int>(g_ws_extra_right);
    const int unused = 512 - smw_view::kNativeW - el - er;
    const int fold = smw_view::kNativeW + er + (unused > 0 ? unused / 2 : 0);
    *out_x = raw_x >= fold ? raw_x - 512 : raw_x;
    return 1;
}

}  // namespace

// Generated code still calls these per-slot note hooks. The tall view is
// symmetric (extra_top == extra_bottom == 32 at 224 lines), so the PPU's
// band fold of the 8-bit OAM Y is unambiguous and no per-slot state is kept.
void smw_ws_note_obj_x(std::uint32_t, std::int32_t) {}
void smw_ws_begin_screen_x(std::int32_t) {}
void smw_ws_begin_screen_y(std::int32_t) {}
void smw_ws_arm_screen_y(std::int32_t) {}
void smw_ws_keep_armed_y(std::uint32_t, std::uint32_t) {}
void smw_ws_note_latched_slot(std::uint32_t) {}
void smw_ws_shift_noted_x(std::uint32_t, std::int32_t) {}
extern "C" void smw_ws_expire_obj_x(void) {}
void smw_ws_note_player_tile(std::uint32_t) {}
void smw_ws_note_player_slot(std::uint32_t) {}
void smw_ws_hide_player_piece(std::uint32_t sp) {
    const std::uint32_t index = bus_read_u32(sp);
    if (index >= 128u) return;
    bus_write_u8(smw_view::kSoftOam + (index << 3), 0xC0u);
}
void smw_ws_note_obj_y(std::uint32_t, std::int32_t) {}
void smw_ws_arm_kept_row(int, int) {}
void smw_ws_remember_margin_row(void) {}
void smw_ws_note_obj_y_stored(std::uint32_t, std::uint32_t) {}
void smw_ws_note_obj_y_tile(std::uint32_t, std::int32_t, std::uint32_t) {}

namespace smw {

void install_sprite_cull_hooks() {
    gba::g_ws_obj_x_provider = wide_obj_x;
    g_previous_bus_read = g_runtime_bus_read_override;
    g_runtime_bus_read_override = playfield_bus_read;
}

}  // namespace smw
