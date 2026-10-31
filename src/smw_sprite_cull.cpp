#include "smw_sprite_cull.h"

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
    return 240u + g_ws_extra_left + g_ws_extra_right + 32u + kViewportSlack;
}

extern "C" std::uint32_t smw_ws_sprite_cull_ymax(void) {
    // Same byte as a row above the glass. Anything past the last
    // picture row must not be submitted, or it is drawn at the top.
    const unsigned bottom = 160u + gba::g_ws_extra_bottom;
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
    const unsigned bottom = 160u + gba::g_ws_extra_bottom;
    return bottom > 0u ? bottom - 1u : 0u;
}

std::uint32_t smw_ws_sprite_y_span(void) {
    return 0xDFu + gba::g_ws_extra_top + kYMargin;
}

std::uint32_t smw_ws_sprite_y_head_pad(void) {
    return (24u + gba::g_ws_extra_top + kYMargin) << 16;
}

std::uint32_t smw_ws_fold_peek_row(std::uint32_t y) {
    if (gba::g_ws_player_screen_y >= 160) return y;
    if (y >= 192u && y <= 255u)
        return static_cast<std::uint32_t>(static_cast<int>(y) - 256);
    return y;
}

int smw_ws_player_y_in_view(std::uint32_t y16) {
    const int sy = static_cast<std::int16_t>(y16 & 0xFFFFu);
    const int top = -static_cast<int>(gba::g_ws_extra_top) -
                    static_cast<int>(kYMargin);
    const int bot = 160 + static_cast<int>(gba::g_ws_extra_bottom) + 80;
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
    else if (sy >= 160 && sy <= 255)
        sy -= 256;
    // Death fall: the real row is under the floor. Leave the hide.
    if (gba::g_ws_player_screen_y >= 160) return 0xC0u;
    const int top = -static_cast<int>(gba::g_ws_extra_top) -
                    static_cast<int>(kYMargin);
    if (sy < top || sy >= 160) return 0xC0u;
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
        240u + g_ws_extra_left + g_ws_extra_right + 48u;
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
            const int ly = static_cast<std::int16_t>(bus_read_u16(0x03002C08u));
            std::fprintf(stderr, "[smw:pit] screen_y=%d player_y=%d L1y=%d\n",
                         sy, py, ly);
            ++hits;
        }
    }
    return static_cast<std::uint32_t>(sy - limit);
}

static bool y_capture_on() {
    std::FILE* f = std::fopen("/tmp/smw-y-capture", "r");
    if (!f) return false;
    std::fclose(f);
    return true;
}

// Real row is sprite Y minus the camera. off!=0 means the game
// marked it off the screen and will not draw it.
void smw_ws_log_y_real(std::uint32_t sprite, int rel, std::uint32_t off) {
    static int hits = 0;
    if (hits >= 40 || rel >= 0 || rel < -128 || !y_capture_on()) return;
    ++hits;
    std::fprintf(stderr, "[smw:yreal] altura=%d decision=%s id=%02x\n",
                 rel, off ? "no" : "si", bus_read_u8(sprite + 0x1Au));
    std::fflush(stderr);
}

// The number the on-screen test compares with 191.
void smw_ws_log_y_decide(std::uint32_t raw) {
    static int hits = 0;
    if (hits >= 40 || !y_capture_on()) return;
    const int v = static_cast<int>(raw);
    int shown = v;
    if ((raw >> 16) == 0u)
        shown = static_cast<std::int16_t>(static_cast<std::uint16_t>(raw));
    const bool dice_no = v > 191;
    if (shown >= 0 && shown <= 191 && !dice_no) return;
    ++hits;
    std::fprintf(stderr, "[smw:ydec] pc=%08x numero=%d leido=%d respuesta=%s\n",
                 g_cpu.R[15], v, shown, dice_no ? "no" : "si");
    std::fflush(stderr);
}

// Soft-OAM hide pads screen_x by this before cmp 0x12F (stock 32<<16).
extern "C" std::uint32_t smw_ws_sprite_vis_x_pad_shl16(void) {
    return (32u + g_ws_extra_left + kViewportSlack) << 16;
}

// Sub0802F678 and the two copies. The stock test is (screenY+pad)&0xFF00,
// a 256px window. The view is 224 tall and sits extra_top above screen Y 0,
// so that window marks the top rows off and the sprite never starts.
extern "C" std::uint32_t smw_ws_sprite_offscreen_y_mask(std::uint32_t sprite) {
    const int sy = static_cast<std::int16_t>(bus_read_u16(sprite + 0x12u));
    const int ly = static_cast<std::int16_t>(bus_read_u16(0x03002C08u));
    const int rel = sy - ly;
    const int top = -48 - static_cast<int>(gba::g_ws_extra_top) -
                    static_cast<int>(kYMargin);
    const int bot = 160 + static_cast<int>(gba::g_ws_extra_bottom) + 48 +
                    static_cast<int>(kViewportSlack);
    const std::uint32_t off = (rel < top || rel > bot) ? 0x100u : 0u;
    smw_ws_log_y_real(sprite, rel, off);
    return off;
}

// While this is set, the ED6[1] literal read returns this value instead of
// the widened stock right edge. Sub0803635C adds it to Layer1 and scans
// that one 16px column.
static int g_ed6_force = 0;
static int g_ed6_force_value = 0;

// Sub08036A9C asks for one column: the edge on the side you are moving.
// Coming from the left, that edge crosses each column and the sprite
// appears. Already standing in the area, the edge is past the columns
// that are on screen, so they are never asked for again.
// A load flag can stay set after its slot is gone. That column is then
// skipped forever. Ask for every 16px column of the view until a living
// slot is actually there. Once one has been seen, leave the column alone
// until it leaves the view, so a koopa you kill does not come back.
extern "C" void smw_ws_spawn_other_edge(void) {
    if (g_ws_extra_left == 0 && g_ws_extra_right == 0) return;
    constexpr std::uint32_t kLevelPtr = 0x03003F98u;
    constexpr std::uint32_t kLayer1X = 0x03002C04u;
    constexpr std::uint32_t kSpriteRam = 0x03007A48u;
    constexpr std::uint32_t kColOff = 0x02014D70u;
    constexpr std::uint32_t kColIdx = 0x02014F70u;
    const std::uint32_t level = bus_read_u32(kLevelPtr);
    if (level < 0x02000000u || level >= 0x04000000u) return;
    // Vertical levels scan Y through the same loader. This walk is X.
    if (bus_read_u16(level + 0x1Eu) & 1u) return;
    const std::uint16_t saved = bus_read_u16(level + 0x1Cu);
    // Sub080389B0 has not built the column table yet. 0xAA is the
    // scroll-command list: the loader walks from the start each call,
    // so a per-column ask still works.
    const std::uint8_t table0 = bus_read_u8(kColOff);
    if (table0 == 0u) return;
    const bool linear = table0 == 0xAAu;

    const std::uint32_t spr = bus_read_u32(kSpriteRam);
    if (spr < 0x02000000u || spr >= 0x04000000u) return;

    const int l1 = static_cast<std::int16_t>(bus_read_u16(kLayer1X));
    const int view = static_cast<int>(240u + g_ws_extra_left + g_ws_extra_right);
    const int left = (l1 - 48) & ~15;
    const int right = (l1 + view + 48) & ~15;

    const std::uint32_t list = bus_read_u32(level + 0x1194u);
    static std::uint32_t last_list = 0;
    static std::uint8_t done[512];
    // One column holds every sprite on that 16px line, including the ones
    // in the rows above the glass. "Any slot on the column" used to close
    // it after the ground enemy, so the one above never got asked.
    // seen = this index has been alive. A kill stays seen and does not
    // come back. An index that has never been alive is still owed a load.
    static std::uint8_t seen[1024];
    static int wait_y[512];
    if (list != last_list) {
        last_list = list;
        std::fill(std::begin(done), std::end(done), 0);
        std::fill(std::begin(seen), std::end(seen), 0);
        std::fill(std::begin(wait_y), std::end(wait_y),
                  static_cast<int>(0x80000000u));
        std::fprintf(stderr,
                     "[smw:spawn] walk l1=%d list=%08x table=%02x span=%d..%d\n",
                     l1, list, table0, left, right);
        std::fprintf(stderr, "[smw:spawn] cols");
        for (int ci = 0; ci <= 48; ++ci) {
            const unsigned off = bus_read_u8(kColOff + static_cast<std::uint32_t>(ci));
            const unsigned nxt =
                bus_read_u8(kColOff + static_cast<std::uint32_t>(ci + 1));
            if (off == nxt) continue;
            std::fprintf(stderr, " %d:%02x/%u", ci, off,
                         bus_read_u8(kColIdx + static_cast<std::uint32_t>(ci)));
        }
        std::fprintf(stderr, "\n");
        for (unsigned s = 0; s < 24u; ++s) {
            const std::uint32_t slot = spr + 0x06CCu + s * 0x64u;
            const unsigned st = bus_read_u8(slot + 0x1Cu);
            if (st == 0u) continue;
            const int x = static_cast<std::int16_t>(bus_read_u16(slot + 0x10u));
            std::fprintf(stderr, "[smw:spawn]  slot %u st=%u id=%02x x=%d\n",
                         s, st, bus_read_u8(slot + 0x1Au), x);
        }
    }
    const int l1y = static_cast<std::int16_t>(bus_read_u16(0x03002C08u));
    auto in_view = [&](int col) { return col >= left && col <= right; };
    auto mark_seen = [&]() {
        for (unsigned s = 0; s < 24u; ++s) {
            const std::uint32_t slot = spr + 0x06CCu + s * 0x64u;
            if (bus_read_u8(slot + 0x1Cu) == 0u) continue;
            const unsigned idx = bus_read_u8(slot + 0x39u);
            if (idx < 1024u) seen[idx] = 1;
        }
    };
    mark_seen();
    for (int ci = 0; ci < 512; ++ci) {
        if (!done[ci] && wait_y[ci] == 0x80000000) continue;
        const int col = ci << 4;
        if (!in_view(col)) {
            done[ci] = 0;
            wait_y[ci] = 0x80000000;
        }
    }

    auto slot_on_column = [&](int col, unsigned flag_i, unsigned count) {
        for (unsigned s = 0; s < 24u; ++s) {
            const std::uint32_t slot = spr + 0x06CCu + s * 0x64u;
            if (bus_read_u8(slot + 0x1Cu) == 0u) continue;
            const int x = static_cast<std::int16_t>(bus_read_u16(slot + 0x10u));
            if (x >= col && x < col + 16) return true;
            if (count == 0u) continue;
            const unsigned idx = bus_read_u8(slot + 0x39u);
            if (idx >= flag_i && idx < flag_i + count) return true;
        }
        return false;
    };

    // Same ask as scrolling right onto the column: direction 1, and the
    // right-edge offset landed on this column. The camera stays put.
    bus_write_u16(level + 0x1Cu, 0x0101u);
    g_ed6_force = 1;
    // Not 0x08036AB2: that is the return of the call we are inside.
    constexpr std::uint32_t kRet = 0xF0000000u;
    std::uint32_t saved_regs[15];
    for (int i = 0; i < 15; ++i) saved_regs[i] = g_cpu.R[i];
    for (int col = left; col <= right; col += 16) {
        if (col < 0) continue;
        const int ci = col >> 4;
        if (ci >= 512) break;
        if (done[ci]) continue;
        unsigned flag_i = 0;
        unsigned count = 0;
        unsigned off_here = 0;
        unsigned off_next = 0;
        if (!linear) {
            off_here = bus_read_u8(kColOff + static_cast<std::uint32_t>(ci));
            off_next = (ci + 1 < 512)
                           ? bus_read_u8(kColOff + static_cast<std::uint32_t>(ci + 1))
                           : off_here;
            // No sprite on this column. Do not lock it: the table can still
            // be the one from the previous room.
            if (off_here == off_next && off_here != 0u) continue;
            flag_i = bus_read_u8(kColIdx + static_cast<std::uint32_t>(ci));
            const unsigned flag_n =
                (ci + 1 < 512)
                    ? bus_read_u8(kColIdx + static_cast<std::uint32_t>(ci + 1))
                    : flag_i + 1u;
            count = flag_n > flag_i ? (flag_n - flag_i) : 1u;
            if (count > 8u) count = 8u;
        }
        // Indices never seen are the ones still outside the old window,
        // including the rows above the glass. Seen ones stay flagged so a
        // killed enemy does not return. Ask again only when the camera
        // moves vertically: the same Y already refused them.
        unsigned owed = 0;
        if (!linear && count > 0) {
            if (wait_y[ci] == l1y) continue;
            for (unsigned k = 0; k < count; ++k) {
                const unsigned idx = flag_i + k;
                if (idx < 1024u && seen[idx]) continue;
                ++owed;
                if (idx < 1024u)
                    bus_write_u8(spr + 0xE34u + idx, 0u);
            }
            if (owed == 0) {
                done[ci] = 1;
                continue;
            }
        } else if (slot_on_column(col, flag_i, count)) {
            done[ci] = 1;
            continue;
        } else {
            for (unsigned k = 0; k < count; ++k)
                bus_write_u8(spr + 0xE34u + flag_i + k, 0u);
        }
        g_ed6_force_value = col - l1;
        runtime_call_push_return(kRet);
        g_cpu.R[14] = kRet | 1u;
        g_cpu.R[15] = 0x0803635Cu;
        runtime_dispatch(0x0803635Cu);
        if ((g_cpu.R[15] & ~1u) != kRet) {
            runtime_call_cancel_return(kRet);
            g_cpu.R[13] = saved_regs[13];
            static int logged = 0;
            if (logged < 3) {
                std::fprintf(stderr,
                             "[smw:spawn] column %d stopped at pc=%08x\n",
                             col, g_cpu.R[15]);
                ++logged;
            }
            break;
        }
        const unsigned seen_before = owed;
        mark_seen();
        unsigned still = 0;
        if (!linear && count > 0) {
            for (unsigned k = 0; k < count; ++k) {
                const unsigned idx = flag_i + k;
                if (idx >= 1024u || !seen[idx]) ++still;
            }
        }
        const bool got = still < seen_before ||
                         slot_on_column(col, flag_i, count);
        static int asks = 0;
        if (asks < 12) {
            std::fprintf(stderr,
                         "[smw:spawn] ask col=%d off=%02x/%02x flag=%u got=%d owed=%u\n",
                         col, off_here, off_next, flag_i, got ? 1 : 0, owed);
            ++asks;
        }
        // Everything on this column has been alive once: leave it. A kill
        // stays seen. If this camera Y loaded nothing new, wait until Y
        // moves before asking again.
        if (!linear && count > 0 && still == 0) done[ci] = 1;
        else if (!linear && count > 0) wait_y[ci] = l1y;
        else if (got) done[ci] = 1;
    }
    g_ed6_force = 0;
    bus_write_u16(level + 0x1Cu, saved);
    for (int i = 0; i < 15; ++i) g_cpu.R[i] = saved_regs[i];
}

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
            // Stock 288 = 240+48. Wide: view_w+48. A span scan can
            // substitute one column at a time.
            if (g_ed6_force) {
                *out = static_cast<std::uint32_t>(static_cast<std::uint16_t>(
                    static_cast<std::int16_t>(g_ed6_force_value)));
                return 1;
            }
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

// 0805FD82 writes three OAM tiles from a world row. Defined below.
void arm_world_row();
// Signed row kept by the top-line test. Declared here so the entry
// hook can see it; the store writes only the byte.
std::int32_t g_kept_row = 0;
bool g_kept_row_on = false;
// Set once a matching OAM Y was noted, so the next different row
// retires the arm and a later sprite at the same byte does not inherit it.
bool g_kept_row_hit = false;

void trace_cull_entry(std::uint32_t pc) {
    if (g_previous_fn_entry) g_previous_fn_entry(pc);
    // The side tiles load world Y and store its low byte. The screen
    // row is that Y minus Layer1. The loop steps the slot; it does not
    // enter again.
    if (pc == 0x0805FD82u) arm_world_row();
    // The top-line test kept this tile. r1 is its screen row and r5 is
    // the OAM byte. 192 would otherwise be read as the hide.
    if (pc == 0x08049F60u && g_kept_row_on &&
        static_cast<int>(g_cpu.R[1] & 0xFFu) == (g_kept_row & 0xFF))
        smw_ws_note_obj_y(g_cpu.R[5], g_kept_row);
    const char* trace = std::getenv("GBARECOMP_SMW_CULL_TRACE");
    if (!trace || !*trace || *trace == '0') return;
    if (pc != 0x0808C4A4u && pc != 0x0808C70Au && pc != 0x0803635Cu &&
        pc != 0x0802F678u)
        return;
    std::fprintf(stderr,
                 "[smw:playfield] frame=%llu pc=%08x L/R=%u/%u xmax=%u\n",
                 g_runtime_vblank_starts, pc, g_ws_extra_left, g_ws_extra_right,
                 smw_ws_sprite_cull_xmax());
}

// Soft OAM built by Sub0808C4E0. Hardware OAM is a copy of the same slots.
constexpr std::uint32_t kSoftOam = 0x03002C28u;
constexpr std::uint32_t kHardOam = 0x07000000u;

struct ObjXNote {
    std::int32_t full = 0;
    std::uint16_t low9 = 0xFFFFu;
};
ObjXNote g_obj_x[128];

struct ObjYNote {
    std::int32_t full = 0;
    std::uint16_t low8 = 0xFFFFu;
};
ObjYNote g_obj_y[128];

int oam_slot(std::uint32_t ptr) {
    if (ptr >= kSoftOam && ptr < kSoftOam + 128u * 8u)
        return static_cast<int>((ptr - kSoftOam) >> 3);
    if (ptr >= kHardOam && ptr < kHardOam + 128u * 8u)
        return static_cast<int>((ptr - kHardOam) >> 3);
    return -1;
}

int wide_obj_attr_x(int oam_index, std::uint16_t, std::uint16_t attr1,
                    std::uint16_t, int* out_x) {
    if (!out_x || oam_index < 0 || oam_index >= 128) return 0;
    const ObjXNote& n = g_obj_x[oam_index];
    if (n.low9 == 0xFFFFu) return 0;
    // The signed screen X belongs to this slot only when the bits still
    // are that coordinate (a later nudge of a few pixels counts).
    const int bits = static_cast<int>(attr1 & 0x1FFu);
    int d = bits - static_cast<int>(n.low9);
    if (d > 256) d -= 512;
    if (d < -256) d += 512;
    if (d > 48 || d < -48) return 0;
    *out_x = n.full + d;
    return 1;
}

void note_obj_x(std::uint32_t oam_ptr, std::int32_t full_x) {
    const int slot = oam_slot(oam_ptr);
    if (slot < 0) return;
    g_obj_x[slot].full = full_x;
    g_obj_x[slot].low9 = static_cast<std::uint16_t>(full_x & 0x1FF);
}

// Screen X of the sprite currently being drawn, before the 9-bit fold.
// Every OAM attr1 store while this is armed is placed at
// screen_x + (tile's 9-bit X − screen_x's low 9 bits).
std::int32_t g_screen_x = 0;
bool g_screen_x_on = false;
// Screen Y before the lsl #16 / lsr #16 that drops the sign.
std::int32_t g_screen_y = 0;
bool g_screen_y_on = false;

void begin_screen_x(std::int32_t full_x) {
    g_screen_x = full_x;
    g_screen_x_on = true;
}

void begin_screen_y(std::int32_t full_y) {
    g_screen_y = full_y;
    g_screen_y_on = true;
}

void note_latched_slot(std::uint32_t oam_base) {
    if (!g_screen_x_on) return;
    const int slot = oam_slot(oam_base);
    if (slot < 0) return;
    const int stored =
        static_cast<int>(bus_read_u16(oam_base + 2u) & 0x1FFu);
    const int low = g_screen_x & 0x1FF;
    int d = stored - low;
    if (d > 256) d -= 512;
    if (d < -256) d += 512;
    // A store that is not this coordinate belongs to another sprite.
    // The arm does not follow it.
    if (d > 48 || d < -48) {
        g_screen_x_on = false;
        return;
    }
    g_obj_x[slot].full = g_screen_x + d;
    g_obj_x[slot].low9 =
        static_cast<std::uint16_t>((g_obj_x[slot].full) & 0x1FF);
}

// A later pass reads the 9-bit X back out, adds a small delta, and
// stores it. Keep the signed X and apply that same delta.
void shift_noted_x(std::uint32_t oam_base, std::int32_t delta) {
    const int slot = oam_slot(oam_base);
    if (slot < 0) return;
    if (g_obj_x[slot].low9 == 0xFFFFu) return;
    g_obj_x[slot].full += delta;
    g_obj_x[slot].low9 =
        static_cast<std::uint16_t>(g_obj_x[slot].full & 0x1FF);
}

int wide_obj_x(int raw_x, int* out_x) {
    if (!out_x) return 0;
    // No 9-bit sign. 0..511 stays where it was written. A coordinate
    // off the left is the signed screen X recorded for that slot.
    *out_x = raw_x;
    return 1;
}

// Distance between two Y bytes, in -128..127.
int y_byte_delta(int stored, int low) {
    int d = stored - low;
    if (d > 128) d -= 256;
    if (d < -128) d += 256;
    return d;
}

void retire_y(int slot) {
    if (slot < 0 || slot >= 128) return;
    g_obj_y[slot].low8 = 0xFFFFu;
}

// The byte is still this note when it is the noted row, or a nudge of a
// few pixels from the camera or a later pass.
bool y_note_holds(const ObjYNote& n, int stored) {
    if (n.low8 == 0xFFFFu) return false;
    const int d = y_byte_delta(stored, static_cast<int>(n.low8));
    return d <= 8 && d >= -8;
}

// A drawer stored the low byte of a world coordinate. full is the
// screen row (world − camera). low8 is the byte sitting in OAM, so the
// renderer can still tell this note from the next owner.
struct WorldRow {
    bool on = false;
    std::int32_t screen = 0;
    std::uint16_t byte = 0;
    std::uint32_t oam = 0;
    int left = 0;
};
WorldRow g_world_row;

void arm_world_row() {
    constexpr std::uint32_t kLayer1Y = 0x03002C08u;
    const int world = static_cast<std::int16_t>(g_cpu.R[7] & 0xFFFFu);
    const int cam = static_cast<std::int16_t>(bus_read_u16(kLayer1Y));
    g_world_row.on = true;
    g_world_row.screen = world - cam;
    g_world_row.byte = static_cast<std::uint16_t>(world & 0xFF);
    g_world_row.oam = g_cpu.R[5];
    g_world_row.left = 3;
}

void note_world_row_store(std::uint32_t oam, std::uint32_t bits) {
    if (!g_world_row.on) return;
    if ((bits & 0xFFu) != g_world_row.byte) return;
    if (oam != g_world_row.oam) return;
    const int slot = oam_slot(oam);
    if (slot >= 0) {
        g_obj_y[slot].full = g_world_row.screen;
        g_obj_y[slot].low8 = g_world_row.byte;
    }
    g_world_row.oam += 8u;
    if (--g_world_row.left <= 0) g_world_row.on = false;
}

}  // namespace

void smw_ws_note_obj_x(std::uint32_t oam_ptr, std::int32_t full_x) {
    note_obj_x(oam_ptr, full_x);
}

void smw_ws_begin_screen_x(std::int32_t full_x) {
    begin_screen_x(full_x);
}

void smw_ws_begin_screen_y(std::int32_t) {
    // These shifts are not this sprite's screen row.
}

// r1 at the submit gate is sprite Y − camera, still signed.
void smw_ws_arm_screen_y(std::int32_t full_y) {
    g_screen_y = full_y;
    g_screen_y_on = true;
}

// The following stores write that Y plus a small tile step, as a byte.
// 192 retires a slot. 160 parks a free slot. Either byte is a real row
// only when the armed row is already that byte. Any other one ends the
// arm, so the next sprite does not inherit this row.
void smw_ws_keep_armed_y(std::uint32_t oam_ptr, std::uint32_t bits) {
    const int slot = oam_slot(oam_ptr);
    const int stored = static_cast<int>(bits & 0xFFu);
    // The deck sits about one tile past the old top line. Its byte is
    // also a row under the floor, so the signed row from the compare
    // has to win over the pivot's smaller step.
    if (g_kept_row_on && stored == (g_kept_row & 0xFF)) {
        smw_ws_note_obj_y(oam_ptr, g_kept_row);
        g_kept_row_hit = true;
        return;
    }
    const bool park = stored == 0xC0 || stored == 0xA0;
    const bool own_park = g_screen_y_on && (g_screen_y & 0xFF) == stored;

    if (park && !own_park) {
        if (slot >= 0 && !y_note_holds(g_obj_y[slot], stored)) retire_y(slot);
        g_screen_y_on = false;
        return;
    }
    if (!g_screen_y_on) {
        if (slot >= 0 && !y_note_holds(g_obj_y[slot], stored)) retire_y(slot);
        return;
    }
    const int d = y_byte_delta(stored, g_screen_y & 0xFF);
    if (d > 32 || d < -32) {
        g_screen_y_on = false;
        if (slot >= 0 && !y_note_holds(g_obj_y[slot], stored)) retire_y(slot);
        return;
    }
    smw_ws_note_obj_y(oam_ptr, g_screen_y + d);
}

void smw_ws_note_latched_slot(std::uint32_t oam_base) {
    note_latched_slot(oam_base);
}

void smw_ws_shift_noted_x(std::uint32_t oam_base, std::int32_t delta) {
    shift_noted_x(oam_base, delta);
}

extern "C" void smw_ws_expire_obj_x(void) {
    for (ObjXNote& n : g_obj_x) n.low9 = 0xFFFFu;
    for (ObjYNote& n : g_obj_y) n.low8 = 0xFFFFu;
    g_screen_x_on = false;
    g_screen_y_on = false;
    g_kept_row_on = false;
    g_kept_row_hit = false;
    g_world_row.on = false;
}

int wide_obj_y(int oam_index, std::uint16_t attr0, int* out_y) {
    if (!out_y || oam_index < 0 || oam_index >= 128) return 0;
    const ObjYNote& n = g_obj_y[oam_index];
    if (n.low8 == 0xFFFFu) return 0;
    const int bits = static_cast<int>(attr0 & 0xFFu);
    // 192 is the hide. It is also the top row, Y = -64, and that note's
    // own byte is 192. Any other note died with the previous owner.
    if (bits == 0xC0 && static_cast<int>(n.low8) != 0xC0) {
        retire_y(oam_index);
        return 0;
    }
    // 160 parks a free slot. A note from the sprite that used the slot
    // last still matches when that sprite sits near the floor, and the
    // parked tile is the face. The byte is this row only when the note
    // itself is 160. X = 0 and tile 0 is the empty slot either way.
    if (bits == 0xA0) {
        const std::uint32_t base =
            0x07000000u + static_cast<std::uint32_t>(oam_index) * 8u;
        const unsigned raw_x = bus_read_u16(base + 2u) & 0x1FFu;
        const unsigned tile = bus_read_u16(base + 4u) & 0x3FFu;
        if (raw_x == 0u && tile == 0u) {
            retire_y(oam_index);
            return 0;
        }
        if (static_cast<int>(n.low8) != 0xA0) {
            retire_y(oam_index);
            return 0;
        }
    }
    if (!y_note_holds(n, bits)) {
        retire_y(oam_index);
        return 0;
    }
    const int y = n.full + y_byte_delta(bits, static_cast<int>(n.low8));
    // Mario's fall: the wrapped byte is his row above the glass. This
    // note is that copy. Leave it for the scanline to skip.
    if (gba::g_ws_player_screen_y >= 160) {
        int wrapped = gba::g_ws_player_screen_y & 0xFF;
        if (wrapped >= 128) wrapped -= 256;
        if (y >= wrapped - 40 && y <= wrapped + 16) return 0;
    }
    // The signed row, including one under the floor. Returning nothing
    // here makes the renderer fold the byte into the rows above.
    *out_y = y;
    return 1;
}

void smw_ws_note_player_tile(std::uint32_t) {}

void smw_ws_note_player_slot(std::uint32_t) {}

// 0806B294 / 0806B2A6 / a piece whose offset is not this pose.
// The store is skipped, so the old face byte would stay on the
// bottom edge. Park the slot off the glass.
void smw_ws_hide_player_piece(std::uint32_t sp) {
    const std::uint32_t index = bus_read_u32(sp);
    if (index >= 128u) return;
    bus_write_u8(0x03002C28u + (index << 3), 0xC0u);
    retire_y(static_cast<int>(index));
}

void smw_ws_note_obj_y(std::uint32_t oam_ptr, std::int32_t full_y) {
    const int slot = oam_slot(oam_ptr);
    if (slot < 0) return;
    g_obj_y[slot].full = full_y;
    g_obj_y[slot].low8 = static_cast<std::uint16_t>(full_y & 0xFF);
}

void smw_ws_arm_kept_row(int keep, int screen_y) {
    if (!keep) return;
    if (g_kept_row_on && g_kept_row == screen_y) return;
    g_kept_row = screen_y;
    g_kept_row_on = true;
    g_kept_row_hit = false;
}

// Every OAM Y store, including drawers that never call
// smw_ws_note_obj_y_stored. Byte 160..175 is both the under-floor band
// and one tile above the picture; the signed row is the only way to
// tell the deck from a sprite on the floor.
void note_kept_oam_y(std::uint32_t addr, std::uint32_t value, std::uint32_t width) {
    if (!g_kept_row_on) return;
    if (width != 1u && width != 2u && width != 4u) return;
    if ((addr & 7u) != 0u) return;
    if (oam_slot(addr) < 0) return;
    const int stored = static_cast<int>(value & 0xFFu);
    if (stored == (g_kept_row & 0xFF)) {
        smw_ws_note_obj_y(addr, g_kept_row);
        g_kept_row_hit = true;
        return;
    }
    if (stored == 0xC0 || stored == 0xA0) return;
    if (g_kept_row_hit) g_kept_row_on = false;
}

void smw_ws_note_obj_y_stored(std::uint32_t oam_ptr, std::uint32_t bits) {
    note_world_row_store(oam_ptr, bits);
    note_kept_oam_y(oam_ptr, bits, 1u);
    // The rotating platform stores the signed halfword, then only its
    // low byte. Above the picture that byte is also a lower-screen row.
    int y = static_cast<int>(0x80000000);
    if ((bits >> 16) == 0u)
        y = static_cast<std::int16_t>(bits & 0xFFFFu);
    else if ((bits >> 16) == 0xFFFFu)
        y = static_cast<std::int32_t>(bits);
    const int top = -static_cast<int>(gba::g_ws_extra_top) -
                    static_cast<int>(kYMargin);
    const int glass = -static_cast<int>(gba::g_ws_extra_top);
    if (y >= top && y < glass && (y & 0xFF) == static_cast<int>(bits & 0xFFu))
        smw_ws_note_obj_y(oam_ptr, y);
}

void smw_ws_note_obj_y_tile(std::uint32_t oam_ptr, std::int32_t screen_y,
                            std::uint32_t packed_sum) {
    const int rel = static_cast<int>(static_cast<std::int8_t>(
        static_cast<std::uint8_t>(packed_sum - static_cast<std::uint32_t>(screen_y))));
    smw_ws_note_obj_y(oam_ptr, screen_y + rel);
}

extern "C" void (*g_runtime_mem_write_hook)(std::uint32_t, std::uint32_t,
                                            std::uint32_t, std::uint32_t);
void (*g_previous_mem_write)(std::uint32_t, std::uint32_t, std::uint32_t,
                             std::uint32_t) = nullptr;

// Screen Y in the 16px above the picture. The guest keeps the tile
// (the cull margin) and stores only the low byte, which is also a row
// under the floor. The register still holds the signed row.
int cull_margin_y(std::uint32_t bits);

// cmp rd, #0xDE/#0xDF within the next few thumb instructions.
bool df_test_follows(std::uint32_t pc, int rd) {
    for (int step = 1; step <= 4; ++step) {
        const std::uint16_t hw =
            bus_read_u16(pc + static_cast<std::uint32_t>(step * 2));
        if ((hw >> 11) != 0b00101) continue;
        if (static_cast<int>((hw >> 8) & 7u) != rd) continue;
        const unsigned imm = hw & 0xFFu;
        if (imm == 0xDFu || imm == 0xDEu) return true;
    }
    return false;
}

bool df_test_behind(std::uint32_t pc) {
    for (int back = 1; back <= 8; ++back) {
        const std::uint16_t hw =
            bus_read_u16(pc - static_cast<std::uint32_t>(back * 2));
        if ((hw >> 11) != 0b00101) continue;
        const unsigned imm = hw & 0xFFu;
        if (imm == 0xDFu || imm == 0xDEu) return true;
    }
    return false;
}

void smw_ws_remember_margin_row(void) {
    std::uint32_t pc = g_cpu.R[15] & ~1u;
    const std::uint16_t here = bus_read_u16(pc);
    const std::uint16_t next = bus_read_u16(pc + 2u);
    // movs r0,#0x20 / cmns rN,r0. The row is still in rN.
    if ((next >> 6) == 0x10Bu) {
        const int rd = static_cast<int>(next & 7u);
        const int y = cull_margin_y(g_cpu.R[rd]);
        if (y == static_cast<int>(0x80000000)) return;
        smw_ws_arm_kept_row(1, y);
        return;
    }
    // adds rd,#0x20 / cmp rd,#0xDF. The row is still in rd; the
    // widened add has not been written back yet. A few pixels above
    // the picture pass this test and then store only the low byte.
    if ((here >> 11) != 0b00110 || (here & 0xFFu) != 0x20u) return;
    const int rd = static_cast<int>((here >> 8) & 7u);
    if (!df_test_follows(pc, rd)) return;
    const int y = cull_margin_y(g_cpu.R[rd]);
    if (y == static_cast<int>(0x80000000)) return;
    smw_ws_arm_kept_row(1, y);
}

int cull_margin_y(std::uint32_t bits) {
    const int top = -static_cast<int>(gba::g_ws_extra_top) -
                    static_cast<int>(kYMargin);
    const int glass = -static_cast<int>(gba::g_ws_extra_top);
    const auto in_margin = [&](int y) { return y >= top && y < glass; };
    const int full = static_cast<std::int32_t>(bits);
    if (in_margin(full)) return full;
    if ((bits >> 16) == 0u) {
        const int half = static_cast<std::int16_t>(bits & 0xFFFFu);
        if (in_margin(half)) return half;
    }
    return 0x80000000;
}

int store_source_reg(std::uint32_t pc) {
    const std::uint16_t hw = bus_read_u16(pc);
    // Thumb STRB/STR immediate, STRB register, STRH. Rd is the low 3 bits.
    const unsigned op11 = hw >> 11;
    const unsigned op9 = hw >> 9;
    if (op11 == 0x0Eu || op11 == 0x0Cu || op9 == 0x2Au || op9 == 0x42u)
        return static_cast<int>(hw & 7u);
    return -1;
}

// The hide is `cmns rN, r0` / `movs r0,#0xc0` / `strb`. rN is still the
// signed row. Thumb CMN register is 0100001011.
int cmns_row_reg(std::uint32_t pc) {
    for (int back = 1; back <= 8; ++back) {
        const std::uint16_t hw = bus_read_u16(pc - static_cast<std::uint32_t>(back * 2));
        if ((hw >> 6) == 0x10Bu) return static_cast<int>(hw & 7u);
    }
    return -1;
}

void keep_margin_row(std::uint32_t addr, int rd, int y) {
    if (rd >= 0 && rd < 16) {
        g_cpu.R[rd] = (g_cpu.R[rd] & ~0xFFu) |
                      static_cast<std::uint32_t>(y & 0xFF);
    }
    smw_ws_note_obj_y(addr, y);
}

void note_cull_margin_y(std::uint32_t pc, std::uint32_t addr,
                        std::uint32_t value, std::uint32_t width) {
    if (width != 1u && width != 2u) return;
    if ((addr & 7u) != 0u) return;
    if (oam_slot(addr) < 0) return;
    const int stored = static_cast<int>(value & 0xFFu);
    const int rd = store_source_reg(pc);

    // The top-line test failed and the guest is about to hide the tile.
    // The compare register still holds the row in the 16px margin.
    if (stored == 0xC0) {
        const int cr = cmns_row_reg(pc);
        if (cr >= 0) {
            const int y = cull_margin_y(g_cpu.R[cr]);
            if (y != static_cast<int>(0x80000000)) {
                keep_margin_row(addr, rd, y);
                g_kept_row_hit = true;
                return;
            }
        }
        // The (Y+pad) vs 0xDF test failed and the guest is hiding the
        // tile. The row was armed at the add, before this store.
        if (g_kept_row_on && !g_kept_row_hit && df_test_behind(pc)) {
            keep_margin_row(addr, rd, g_kept_row);
            g_kept_row_hit = true;
        }
        return;
    }

    // 176..255 already lands on the picture. The two blocks above it
    // are bytes 144..175, which are also rows in the lower half.
    // The signed row tells them apart.
    const int margin_byte =
        (256 - static_cast<int>(gba::g_ws_extra_top) -
         static_cast<int>(kYMargin)) &
        0xFF;
    if (stored < margin_byte || stored > 175) return;
    if (rd >= 0) {
        const int y = cull_margin_y(g_cpu.R[rd]);
        if (y != static_cast<int>(0x80000000) && (y & 0xFF) == stored) {
            smw_ws_note_obj_y(addr, y);
            return;
        }
    }
    int found = 0;
    int pick = 0;
    for (int r = 0; r < 8; ++r) {
        const int y = cull_margin_y(g_cpu.R[r]);
        if (y == static_cast<int>(0x80000000) || (y & 0xFF) != stored) continue;
        pick = y;
        ++found;
    }
    if (found == 1) smw_ws_note_obj_y(addr, pick);
}

extern "C" void smw_ws_on_mem_write(std::uint32_t pc, std::uint32_t addr,
                                    std::uint32_t value, std::uint32_t width) {
    if (g_previous_mem_write) g_previous_mem_write(pc, addr, value, width);
    note_kept_oam_y(addr, value, width);
    note_cull_margin_y(pc, addr, value, width);
}

namespace smw {

void install_sprite_cull_hooks() {
    gba::g_ws_obj_attr_x_provider = wide_obj_attr_x;
    gba::g_ws_obj_x_provider = wide_obj_x;
    gba::g_ws_obj_y_provider = wide_obj_y;
    g_previous_fn_entry = g_runtime_fn_entry_hook;
    g_runtime_fn_entry_hook = trace_cull_entry;
    g_previous_bus_read = g_runtime_bus_read_override;
    g_runtime_bus_read_override = playfield_bus_read;
    if (g_runtime_mem_write_hook != smw_ws_on_mem_write)
        g_previous_mem_write = g_runtime_mem_write_hook;
    g_runtime_mem_write_hook = smw_ws_on_mem_write;
}

}  // namespace smw
