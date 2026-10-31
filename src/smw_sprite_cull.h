#pragma once

#include <cstdint>

// Wide-view sprite submit bounds for SMA2. The stock ROM culls OAM at a
// native AABB (−32…240 × −32…160); these expand that gate to the logical
// framebuffer so margin actors stay alive long enough to be drawn.
extern "C" std::uint32_t smw_ws_sprite_cull_xmax(void);
extern "C" std::uint32_t smw_ws_sprite_cull_ymax(void);
extern "C" std::uint32_t smw_ws_sprite_cull_overhang(void);
// Playfield offscreen (Sub0802F678): stock (−16…239) via (sx+16)&0xFF00.
extern "C" int smw_ws_sprite_offscreen_x(std::int32_t screen_x);
// Sub0802F744 / F810: stock [0…255] via sx&0xFF00.
extern "C" int smw_ws_sprite_offscreen_x_u8(std::int32_t screen_x);

// Init spawn sweep (Sub08036E94): stock backs up Layer1 by 0x60 and runs
// indices 0…0x1F (32×16px). Wide needs a longer right reach.
extern "C" std::uint32_t smw_ws_spawn_sweep_back(void);
extern "C" std::uint32_t smw_ws_spawn_sweep_last(void);
extern "C" std::uint32_t smw_ws_deadzone_face_right(void);
extern "C" std::uint32_t smw_ws_deadzone_face_left(void);
extern "C" std::uint32_t smw_ws_screen_x(std::uint32_t player, std::uint32_t cam);
extern "C" std::uint32_t smw_ws_cam_span_max(std::uint32_t base);
extern "C" std::uint32_t smw_ws_cam_y_max(std::uint32_t max_y);
extern "C" std::uint32_t smw_ws_screen_barrier(std::uint32_t layer, std::uint32_t inset);
extern "C" std::uint32_t smw_ws_offscreen_x_test(std::uint32_t addr);
extern "C" std::uint32_t smw_ws_offscreen_y_test(std::uint32_t screen_y);
extern "C" std::uint32_t smw_ws_sprite_vis_x_pad_shl16(void);
// Stock sprite submit rejects screen Y < -N. N grows by the rows above
// the camera, so a tile there is still submitted.
std::uint32_t smw_ws_sprite_y_lo(std::uint32_t stock);
// Sprite-entry gate. Stock N is 32 or 64. The upper band is extra_top
// rows; a larger N shares its byte with the band under the glass.
std::uint32_t smw_ws_sprite_y_above(std::uint32_t stock);
// Last row of the picture. 160 is the glass floor; extra_bottom is the
// band under it. The next byte is also a negative Y, so the gate stops
// on this row.
std::uint32_t smw_ws_sprite_y_floor(void);
// Unsigned window (screen Y + pad) <= 0xDF. The pad and the limit both
// grow by the rows above the glass, so the top moves and the floor stays.
std::uint32_t smw_ws_sprite_y_span(void);
// (screen Y + 24) compared with 223. The 24 is what keeps a tile above
// the glass; it grows with the rows above the camera.
std::uint32_t smw_ws_sprite_y_head_pad(void);
// The hide writes 192. If the rejected value is a row above the glass,
// keep that row instead.
std::uint32_t smw_ws_keep_screen_y(std::uint32_t oam, std::int32_t y);
// Luigi's tiles sign-extend 16 bits, so a row above the glass arrives as
// 192..255 and the old test throws it out. That byte is the signed row
// the koopa store already keeps. The fall stays unsigned.
std::uint32_t smw_ws_fold_peek_row(std::uint32_t y);
// 0806B290. Stock keeps a player tile when (Y+16) fits in a byte, so
// only the first 16 rows above the glass pass. The paratroopa store has
// no such reject: it keeps the signed subtract. This is that same range.
int smw_ws_player_y_in_view(std::uint32_t y16);
// (screen Y + 31) kept as 16 bits. A row above the glass is negative and
// fails the unsigned compare. It is still inside the tall view.
int smw_ws_row_above_in_view(std::uint32_t raw);
// (screen X + 24) << 16. A negative X is still inside the wide view.
// The unsigned test treats it as off to the right and then hides the tiles.
int smw_ws_signed_x_in_view(std::uint32_t raw);
extern "C" void smw_ws_spawn_other_edge(void);
// Screen position of one object. X and Y are the signed result of
// sprite − camera. The halfword keeps the bits the guest packed; the
// renderer draws these values.
//
// The arm is that signed row for the sprite being submitted. A store
// keeps it only when the byte is that row plus a tile step. A hide
// (192) or a free slot (160) retires the note unless the row itself is
// already that byte, and the arm does not follow the next sprite.
void smw_ws_begin_screen_y(std::int32_t full_y);
void smw_ws_arm_screen_y(std::int32_t full_y);
void smw_ws_keep_armed_y(std::uint32_t oam_ptr, std::uint32_t bits);
void smw_ws_note_obj_x(std::uint32_t oam_ptr, std::int32_t full_x);
void smw_ws_note_obj_y(std::uint32_t oam_ptr, std::int32_t full_y);
// Tile Y is a signed byte added to the sprite's screen Y, then stored
// as 8 bits. The note is the signed sum, so a tile above the glass is
// drawn there instead of folded under the floor.
void smw_ws_note_obj_y_tile(std::uint32_t oam_ptr, std::int32_t screen_y,
                            std::uint32_t packed_sum);
// A store of a world-row byte. The screen row was named at the entry
// of that drawer. Any other store leaves the slot alone.
void smw_ws_note_obj_y_stored(std::uint32_t oam_ptr, std::uint32_t bits);

namespace smw {
void install_sprite_cull_hooks();
}
