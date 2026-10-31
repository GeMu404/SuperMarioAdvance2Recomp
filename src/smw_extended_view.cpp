#include "smw_extended_view.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "gba_bus.h"
#include "gba_ppu.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"
#include "smw_sprite_cull.h"

extern "C" unsigned g_ws_extra_left;
extern "C" unsigned g_ws_extra_right;

// Sub08010074. The tile ring is 32 columns of 16px. The stock window is
// [col-8, col+23]: eight columns behind the camera and the right edge
// 384px ahead. That centers a 240px screen. This view starts at the
// camera and is wider, and the fast parallax band samples ahead of the
// scroll the window is built from, so the loaded tiles run out at the
// old 240 edge and the hill is sliced there. One column stays behind
// the camera. The other 31 of the ring go in front of it.
extern "C" std::uint32_t smw_ws_stream_lead(void) {
    const unsigned view = 240u + g_ws_extra_left + g_ws_extra_right;
    const unsigned cols = (view + 15u) / 16u;
    if (cols >= 32u) return 0u;
    return 1u;
}

// Layer 2 stores the right edge as camera_column + this. Stock is 0x17,
// which is 31 - 8. The span stays one ring: right = col + (31 - lead).
extern "C" void smw_ws_mode0_bg_refresh(void);

extern "C" std::uint32_t smw_ws_stream_ahead(void) {
    smw_ws_mode0_bg_refresh();
    return 31u - smw_ws_stream_lead();
}

// Sub08010104. Vertical levels store the same 32-row ring at level+0xC.
// Stock is [row-8, row+23]: eight rows above the camera, centering the
// 160px glass. This view is taller above that glass, so the lead is the
// rows above the camera. The +0x1F that follows still closes the ring.
extern "C" std::uint32_t smw_ws_stream_lead_y(void) {
    const unsigned above =
        (static_cast<unsigned>(gba::g_ws_extra_top) + 15u) / 16u;
    unsigned lead = above + 1u;
    // The stock 8 rows already sit above the glass. A shorter lead would
    // unload rows the level already streams there.
    if (lead < 8u) lead = 8u;
    if (lead > 31u) lead = 31u;
    return lead;
}

// Sub0800EB44. Bytes written into the mode-0 background at 0x02001800.
static unsigned g_mode0_bg_bytes;

extern "C" void smw_ws_note_mode0_bg_bytes(std::uint32_t n) {
    g_mode0_bg_bytes = n;
}

// 0801342C. The level's Map16 pointers at +0xE8 are the tileset the DMA
// already used for the first screen. 080E0B58 is only the base those
// pointers start at; reading it raw paints the grid (tile 0xF8) on every
// background that is not the cave.
static int mode0_map16_hw(unsigned id, unsigned sub, std::uint16_t* out) {
    if (out == nullptr || sub > 3u) return 0;
    const unsigned slot = id & 0x1FFu;
    const std::uint32_t level = bus_read_u32(0x03003F98u);
    if (level >= 0x02000000u && level < 0x04000000u && (id & 0x3FFFu) <= 0x1FFu) {
        const std::uint32_t ptr = bus_read_u32(level + 0xE8u + (slot << 2));
        if (ptr >= 0x08000000u && ptr < 0x0A000000u) {
            *out = bus_read_u16(ptr + sub * 2u);
            return 1;
        }
    }
    *out = bus_read_u16(0x080E0B58u + (id & 0x3FFFu) * 8u + sub * 2u);
    return 1;
}

// One cell of the mode-0 map is a 16px metatile: two tiles wide, two tall.
// The 512px image is wider than the half the scan is showing as sky.
extern "C" int smw_ws_mode0_bg_sample(unsigned tile_x, unsigned tile_y,
                                     std::uint16_t* out) {
    if (g_mode0_bg_bytes < 0x1B0u || out == nullptr) return 0;
    const std::uint32_t level = bus_read_u32(0x03003F98u);
    if (level < 0x02000000u || level >= 0x04000000u) return 0;
    const unsigned mode = bus_read_u16(level + 4u);
    if (mode != 0u && mode != 0x0Au && mode != 0x0Cu && mode != 0x0Du &&
        mode != 0x0Eu)
        return 0;
    if (bus_read_u16(level + 0x1Eu) & 2u) return 0;
    const unsigned row = tile_y >> 1;
    if (row > 26u) return 0;
    const unsigned col = tile_x >> 1;
    const unsigned idx = (col >> 4) * 0x1B0u + row * 0x10u + (col & 0xFu);
    if (idx >= g_mode0_bg_bytes || idx >= 0x400u) return 0;
    const unsigned id =
        (static_cast<unsigned>(bus_read_u8(0x02001C00u + idx)) << 8) |
        bus_read_u8(0x02001800u + idx);
    if ((id & 0xFFu) == 0u) return 0;
    const unsigned sub = (tile_y & 1u) + (tile_x & 1u) * 2u;
    return mode0_map16_hw(id & 0x1FFu, sub, out);
}

// Sub0800FF44 writes one 16px column from that buffer. The per-frame loader
// for this mode returns without doing it, so only the 32 columns from level
// load are in the 512px map. The wide view reads past them.
static void mode0_paint(std::uint32_t dest, unsigned packed, std::uint16_t hw,
                        int right_tile) {
    const std::uint32_t base = packed < 32u
        ? (right_tile ? 0x06000002u : 0x06000000u)
        : (right_tile ? 0x06001002u : 0x06001000u);
    const unsigned i = packed < 32u ? packed : packed - 32u;
    bus_write_u16(base + dest + i * 0x40u, hw);
}

static void mode0_upload_column(unsigned col) {
    const unsigned screen = col >> 4;
    const unsigned base = screen * 0x1B0u;
    const unsigned x = col & 0xFu;
    if (base + 0x1AFu >= g_mode0_bg_bytes || base + 0x1AFu >= 0x400u) return;
    const std::uint32_t dest = 0xC000u + (x << 2) + ((col & 0x10u) ? 0x800u : 0u);
    unsigned packed = 0;
    for (unsigned idx = x; idx <= 0x1AFu; idx += 0x10u, packed += 2u) {
        const unsigned id =
            (static_cast<unsigned>(bus_read_u8(0x02001C00u + base + idx)) << 8) |
            bus_read_u8(0x02001800u + base + idx);
        std::uint16_t hw0 = 0, hw1 = 0, hw2 = 0, hw3 = 0;
        if ((id & 0xFFu) != 0u) {
            mode0_map16_hw(id & 0x1FFu, 0u, &hw0);
            mode0_map16_hw(id & 0x1FFu, 1u, &hw1);
            mode0_map16_hw(id & 0x1FFu, 2u, &hw2);
            mode0_map16_hw(id & 0x1FFu, 3u, &hw3);
        }
        mode0_paint(dest, packed,     hw0, 0);
        mode0_paint(dest, packed + 1u, hw1, 0);
        mode0_paint(dest, packed,     hw2, 1);
        mode0_paint(dest, packed + 1u, hw3, 1);
    }
}

// Called when the per-frame layer-2 loader is the no-op (Return0800FD98).
extern "C" void smw_ws_mode0_bg_refresh(void) {
    if (g_mode0_bg_bytes == 0u) return;
    const std::uint32_t level = bus_read_u32(0x03003F98u);
    if (level < 0x02000000u || level >= 0x04000000u) return;
    const unsigned mode = bus_read_u16(level + 4u);
    if (mode != 0u && mode != 0x0Au && mode != 0x0Cu && mode != 0x0Du && mode != 0x0Eu)
        return;
    if (bus_read_u16(level + 0x1Eu) & 2u) return;
    const unsigned layer2 = bus_read_u16(0x03002C0Cu);
    static unsigned last = ~0u;
    if (last == layer2) return;
    last = layer2;
    const unsigned view = 240u + g_ws_extra_left + g_ws_extra_right;
    const unsigned left = layer2 >> 4;
    const unsigned right = (layer2 + view - 1u) >> 4;
    // Columns 0..15 stay as the level load wrote them. From column 16 on,
    // the wide view reaches the second screen of this background, which is
    // the right side of the hill.
    for (unsigned col = left < 16u ? 16u : left; col <= right; ++col)
        mode0_upload_column(col);
}

namespace smw {
namespace {

int (*g_previous_tilemap_provider)(int, int, int, std::uint16_t*) = nullptr;
int (*g_previous_bg_x_provider)(int, int, int, int*) = nullptr;
void (*g_previous_fn_entry)(std::uint32_t) = nullptr;

constexpr std::uint32_t kLayer1X = 0x03002C04u;
constexpr std::uint32_t kLayer1Y = 0x03002C08u;
constexpr std::uint32_t kCamX = 0x03002BF0u;
constexpr std::uint32_t kCamY = 0x03002BF4u;
constexpr std::uint32_t kPlayerX = 0x03003FD0u;
constexpr std::uint32_t kPlayerY = 0x03003FD4u;
constexpr std::uint32_t kLevelObj = 0x03003F98u;
constexpr std::uint32_t kBase = 0x03002340u;
constexpr std::uint32_t kDeadzone = kBase + 0x1D08u;
constexpr std::uint32_t kCamFollow = 0x08006434u;

std::int32_t read_s32(std::uint32_t addr) {
    return static_cast<std::int32_t>(bus_read_u32(addr));
}

std::uint32_t read_u32(std::uint32_t addr) {
    return bus_read_u32(addr);
}

std::int32_t read_s16(std::uint32_t addr) {
    return static_cast<std::int32_t>(static_cast<std::int16_t>(bus_read_u16(addr)));
}

std::uint8_t read_u8(std::uint32_t addr) {
    return bus_read_u8(addr);
}

void write_s16(std::uint32_t addr, std::int32_t v) {
    bus_write_u16(addr, static_cast<std::uint16_t>(static_cast<std::int16_t>(v)));
}


// Room size from level header (1175/1176). Returns false if not in a sublevel.
bool read_room_size(int* room_w, int* room_h, std::uint16_t* cam_min,
                    std::uint32_t* level_out) {
    const std::uint32_t level = read_u32(kLevelObj);
    if (level_out) *level_out = level;
    if (room_w) *room_w = 0;
    if (room_h) *room_h = 0;
    if (cam_min) *cam_min = 0;
    if (level < 0x02000000u || level >= 0x04000000u) return false;
    const int sx = static_cast<int>(read_u8(level + 0x1175u));
    const int sy = static_cast<int>(read_u8(level + 0x1176u));
    if (cam_min) *cam_min = bus_read_u16(level + 0x28u);
    if (sx <= 0 || sx >= 64 || sy <= 0 || sy >= 64) return false;
    if (room_w) *room_w = sx * 256;
    if (room_h) *room_h = sy * 256;
    return true;
}

// Guest still scrolls a 240-wide Layer1; host peeks with bias=extra_left.
// Sprite FB: sx - bias + extra_left (== sx when bias==extra_left). Target
// walk-right settle at view_w/2. Layer1 max = room_w-240-extra_right so
// host bias stays at extra_left on the right edge.
void apply_wide_guest_cam(bool before_follow) {
    const int extra_l = static_cast<int>(g_ws_extra_left);
    const int extra_r = static_cast<int>(g_ws_extra_right);
    const int extra_t = static_cast<int>(gba::g_ws_extra_top);
    const int extra_b = static_cast<int>(gba::g_ws_extra_bottom);
    if (extra_l == 0 && extra_r == 0 && extra_t == 0 && extra_b == 0) return;

    int room_w = 0;
    int room_h = 0;
    std::uint16_t cam_min = 0;
    std::uint32_t level = 0;
    if (!read_room_size(&room_w, &room_h, &cam_min, &level)) return;

    const int view_w = 240 + extra_l + extra_r;
    const int view_h = 160 + extra_t + extra_b;
    // Deadzone stays the facing store (0x4E / 0x80). The wide measure is
    // the follow's screen subtract. Do not write it.

    // After follow. Do not push Layer1 in by extra_left: at the start that
    // put Mario (x≈16) left of a camera already at 79, and the picture
    // showed the void behind him. The right edge still stops so the 398-wide
    // view ends on the last column of the room. Y is the guest follow; the
    // 64px of sky are rows above that camera, not a second Y algorithm.
    if (before_follow) return;
    (void)view_h;
    (void)extra_t;
    (void)extra_b;

    // Original follow, including its scroll animation. Only keep the wide
    // view from running past the last column of the room.
    (void)level;
    if (room_w > view_w && (extra_l != 0 || extra_r != 0)) {
        const int max_l1 = room_w - view_w;
        std::int32_t l1 = read_s16(kLayer1X);
        if (l1 > max_l1) l1 = max_l1;
        if (l1 < static_cast<int>(cam_min)) l1 = static_cast<int>(cam_min);
        bus_write_u16(kLayer1X, static_cast<std::uint16_t>(l1));
    }
}

bool title_layout(const std::uint8_t* io);



void publish_camera() {
    {
        const char* e = std::getenv("GBARECOMP_SMW_DISPCNT_LOG");
        if (e && *e && *e != '0' && (g_runtime_vblank_starts % 50ull) == 0ull) {
            if (gba::GbaBus* b = gbarecomp::active_bus()) {
                const auto* io = b->io().raw();
                unsigned d = io[0] | (unsigned(io[1]) << 8);
                std::fprintf(stderr, "[smw:dispcnt] frame=%llu DISPCNT=%04X\n",
                             g_runtime_vblank_starts, d);
            }
        }
    }
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus) return;

    // Clamp Layer1 to wide max after stock follow (same frame as bias pub).
    apply_wide_guest_cam(/*before_follow=*/false);

    const int extra_l = static_cast<int>(g_ws_extra_left);
    const int extra_t = static_cast<int>(gba::g_ws_extra_top);
    const int extra_b = static_cast<int>(gba::g_ws_extra_bottom);
    const int view_w = 240 + static_cast<int>(g_ws_extra_left + g_ws_extra_right);
    const int view_h = 160 + extra_t + extra_b;
    // Detach status as a host UI overlay (stamp tiles + peek punch). Never use
    // scanline pin — that redraws whole guest rows and shears the BG.
    gba::g_ws_status_ui_pin = 0;

    int room_w = 0;
    int room_h = 0;
    int screens_x = 0;
    int screens_y = 0;
    std::uint16_t cam_min_bias = 0;
    std::uint32_t level = 0;
    if (read_room_size(&room_w, &room_h, &cam_min_bias, &level)) {
        screens_x = room_w / 256;
        screens_y = room_h / 256;
    }

    // Re-read after optional short-room Y slide.
    const std::int32_t layer1_x = read_s16(kLayer1X);
    const std::int32_t layer1_y = read_s16(kLayer1Y);
    const std::int32_t cam_x = read_s32(kCamX);
    const std::int32_t cam_y = read_s32(kCamY);
    (void)cam_x;
    (void)cam_y;

    // Origin keeps the whole view inside [0, room_w - view_w].
    (void)cam_min_bias;

    int origin_x = layer1_x - extra_l;
    // Tall viewport on the map (same idea as X) — but only when stock Layer1
    // already fits in the room. Yoshi-1 style headers report room_h=256 while
    // L1_Y≈248 (L1+160 > room_h); clamping origin to room-view then sets
    // bias_y≈200 and the world disappears. Those levels keep bias_y=0.
    int origin_y = layer1_y - extra_t;
    const bool stock_y_in_room =
        room_h > 0 && layer1_y >= 0 &&
        (layer1_y + 160) <= room_h;
    if (room_w > view_w) {
        origin_x = std::clamp(origin_x, 0, room_w - view_w);
    } else if (room_w > 0) {
        origin_x = 0;
    }
    if (stock_y_in_room && room_h > view_h) {
        origin_y = std::clamp(origin_y, 0, room_h - view_h);
    } else if (room_h > 0 && room_h <= view_h) {
        origin_y = 0;
    }

    // Bias only when room bounds are known; inventing bias=extra_left on the
    // overworld (room_w=0) shifts the ring into unstreamed slots → black
    // margins. Snapshot behavior: bias 0 + KeepWrapped.
    if (room_w > 0) {
        gba::g_ws_scroll_bias_x = layer1_x - origin_x;
        gba::g_ws_view_origin_x = origin_x;
        gba::g_ws_room_w = room_w;
    } else {
        gba::g_ws_scroll_bias_x = 0;
        gba::g_ws_view_origin_x = layer1_x;
        gba::g_ws_room_w = 0;
    }
    // The extra rows are the map above and below the guest camera.
    // A Y bias shifts every layer and leaves sprites behind.
    gba::g_ws_scroll_bias_y = 0;
    (void)extra_b;
    gba::g_ws_block_top_peek = (origin_y <= 0 && stock_y_in_room) ? 1 : 0;

    const std::int32_t player_x = read_s16(kPlayerX);
    const std::int32_t player_y = read_s16(kPlayerY);
    gba::g_ws_player_screen_x = player_x - layer1_x;
    gba::g_ws_player_screen_y = player_y - layer1_y;

    // Log on level-size change, or every N frames when GBARECOMP_SMW_CAM_TRACE=1.
    {
        const char* trace = std::getenv("GBARECOMP_SMW_CAM_TRACE");
        const bool force = trace && *trace && *trace != '0';
        static unsigned long long last = ~0ull;
        static int last_sx = -1;
        static int last_room = -1;
        const bool screen_change = (screens_x != last_sx) || (room_w != last_room);
        const bool sample =
            force && (g_runtime_vblank_starts % 30ull) == 0ull;
        if ((force && sample) || screen_change) {
            if (last != g_runtime_vblank_starts) {
                last = g_runtime_vblank_starts;
                last_sx = screens_x;
                last_room = room_w;
                const std::uint16_t deadzone = bus_read_u16(kDeadzone);
                const unsigned hdr0 =
                    (level >= 0x02000000u && level < 0x04000000u)
                        ? read_u8(level + 0x1174u)
                        : 0u;
                const unsigned hdr1 =
                    (level >= 0x02000000u && level < 0x04000000u)
                        ? read_u8(level + 0x1175u)
                        : 0u;
                const unsigned hdr2 =
                    (level >= 0x02000000u && level < 0x04000000u)
                        ? read_u8(level + 0x1176u)
                        : 0u;
                const int fb_mario_x =
                    gba::g_ws_player_screen_x - gba::g_ws_scroll_bias_x +
                    static_cast<int>(g_ws_extra_left);
                std::fprintf(stderr,
                    "[smw:cam] frame=%llu L1=%d,%d player=%d,%d "
                    "screen=%d,%d fb_mario_x=%d origin=%d,%d bias=%d/%d "
                    "room=%dx%d hdr=%u/%u/%u deadzone=%u view=%dx%d "
                    "level=%08x\n",
                    g_runtime_vblank_starts, layer1_x, layer1_y, player_x,
                    player_y, gba::g_ws_player_screen_x,
                    gba::g_ws_player_screen_y, fb_mario_x,
                    gba::g_ws_view_origin_x, origin_y,
                    gba::g_ws_scroll_bias_x, gba::g_ws_scroll_bias_y, room_w,
                    room_h, hdr0, hdr1, hdr2, deadzone, view_w, view_h, level);
            }
        }
    }
}

// KillMario (080709F8) and the wrapper that calls it (08070AB8).
// The hook runs before the prologue, so R14 is still the caller's return.
void note_mario_death(std::uint32_t pc) {
    static int hits = 0;
    if (hits >= 6) return;
    ++hits;
    const std::int32_t player_y = read_s16(kPlayerY);
    const std::int32_t layer_y = read_s16(kLayer1Y);
    const std::int32_t screen_x = read_s16(kBase + 0x1C70u);
    const std::int32_t screen_y = read_s16(kBase + 0x1C74u);
    const unsigned blocked = read_u8(kBase + 0x1C66u);
    const int yspeed = static_cast<int>(static_cast<std::int8_t>(read_u8(kBase + 0x1C6Du)));
    std::fprintf(stderr,
                 "[smw:death] pc=%08x lr=%08x frame=%llu "
                 "player_y=%d L1y=%d screen=%d,%d blocked=%02x yspeed=%d\n",
                 pc, g_cpu.R[14], g_runtime_vblank_starts, player_y, layer_y,
                 screen_x, screen_y, blocked, yspeed);
    const std::uint32_t depth = runtime_call_stack_depth();
    const std::uint32_t* stack = runtime_call_stack_data();
    const std::uint32_t n = depth < 8u ? depth : 8u;
    for (std::uint32_t i = 0; i < n; ++i) {
        std::fprintf(stderr, "[smw:death] stack[%u]=%08x\n", i, stack[i]);
    }
}

void on_fn_entry(std::uint32_t pc) {
    if (g_previous_fn_entry) g_previous_fn_entry(pc);
    // Sub08006434 uses the facing deadzone. Nothing is written here.
    if (pc == kCamFollow) apply_wide_guest_cam(/*before_follow=*/true);
    if (pc == 0x0808C4A4u) publish_camera();
    if (pc == 0x080709F8u || pc == 0x08070AB8u || pc == 0x08070A14u)
        note_mario_death(pc);
}

void trace_scene_once() {
    static unsigned long long last_frame = ~0ull;
    if (last_frame == g_runtime_vblank_starts) return;
    last_frame = g_runtime_vblank_starts;
    publish_camera();
    const char* trace = std::getenv("GBARECOMP_SMW_WS_TRACE");
    if (!trace || !*trace || *trace == '0') return;
    static const unsigned long long target = std::strtoull(trace, nullptr, 0);
    if (target > 1 && target != g_runtime_vblank_starts) return;
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus) return;
    const std::uint8_t* io = bus->io().raw();
    auto read16 = [&](unsigned offset) {
        return static_cast<unsigned>(io[offset] |
            (static_cast<unsigned>(io[offset + 1]) << 8));
    };
    std::fprintf(stderr,
        "[smw:extended-view] frame=%llu DISPCNT=%04X "
        "BG=%04X/%04X/%04X/%04X WIN=%04X/%04X/%04X/%04X "
        "authored=%d pillarbox=%d/%d/%d extra_top=%u\n",
        g_runtime_vblank_starts, read16(0x00), read16(0x08),
        read16(0x0A), read16(0x0C), read16(0x0E), read16(0x40),
        read16(0x42), read16(0x48), read16(0x4A),
        gba::g_ws_authored_margin_layers, gba::g_ws_pillarbox,
        gba::g_ws_pillarbox_left, gba::g_ws_pillarbox_right,
        gba::g_ws_extra_top);
}

bool title_layout(const std::uint8_t* io) {
    const std::uint16_t dispcnt = static_cast<std::uint16_t>(
        io[0] | (static_cast<std::uint16_t>(io[1]) << 8));
    return dispcnt == 0x1600u;
}

int streamed_tilemap_provider(int bg, int hw_x, int screen_y,
                              std::uint16_t* out_entry) {
    trace_scene_once();
    if (gba::GbaBus* bus = gbarecomp::active_bus()) {
        // Title uses wrapped sprite staging outside native X — pillarbox it.
        gba::g_ws_pillarbox = title_layout(bus->io().raw()) ? 1 : 0;
    }
    // Never blank peek/margins with Unavailable — KeepWrapped fills world.
    // SMA2 streams columns via UpdateVramTileMaps; accepting the wrapped
    // entry also authorizes BG outside native WIN0/WIN1.
    if (bg >= 0 && bg < 4 && (hw_x < 0 || hw_x >= 240 || screen_y < 0 ||
                              screen_y >= 160)) {
        return gba::kWsTilemapKeepWrapped;
    }
    return g_previous_tilemap_provider
        ? g_previous_tilemap_provider(bg, hw_x, screen_y, out_entry)
        : gba::kWsTilemapUnavailable;
}

int bounded_bg_x(int bg, int output_x, int screen_y, int* out_hw_x) {
    auto fallback = [&]() {
        return g_previous_bg_x_provider
            ? g_previous_bg_x_provider(bg, output_x, screen_y, out_hw_x) : 0;
    };
    if (!out_hw_x || bg < 0 || bg >= 4) return fallback();

    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus) return fallback();
    const std::uint8_t* io = bus->io().raw();
    const unsigned cnt_offset = 0x08u + static_cast<unsigned>(bg) * 2u;
    const std::uint16_t bgcnt = static_cast<std::uint16_t>(
        io[cnt_offset] |
        (static_cast<std::uint16_t>(io[cnt_offset + 1]) << 8));

    // 256px-wide BG: clamp margins to edge (avoids stale wrap). 512px maps
    // keep off-screen continuation for true widescreen FOV.
    if ((bgcnt & 0x4000u) == 0) {
        const int left = static_cast<int>(g_ws_extra_left);
        if (output_x < left) {
            *out_hw_x = 0;
            return 1;
        }
        if (output_x >= left + 240) {
            *out_hw_x = 239;
            return 1;
        }
    }
    return fallback();
}

}  // namespace

void install_extended_view(std::uint32_t, std::uint32_t) {
    g_previous_tilemap_provider = gba::g_ws_tilemap_provider;
    g_previous_bg_x_provider = gba::g_ws_bg_x_provider;
    gba::g_ws_tilemap_provider = streamed_tilemap_provider;
    gba::g_ws_bg_x_provider = bounded_bg_x;
    gba::g_ws_authored_margin_layers = 1;
    gba::g_ws_pillarbox = 0;
    gba::g_ws_pillarbox_left = 0;
    gba::g_ws_pillarbox_right = 0;

    install_sprite_cull_hooks();

    g_previous_fn_entry = g_runtime_fn_entry_hook;
    g_runtime_fn_entry_hook = on_fn_entry;
}

}  // namespace smw
