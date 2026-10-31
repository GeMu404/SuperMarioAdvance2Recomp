#pragma once

// Shared SMA2 widescreen geometry and guest addresses. The view size itself
// is derived once by gbarecomp (view_config.h) from game.toml view_height;
// game code only reads the resulting extra_* margins through these helpers.

#include <cstdint>

#include "gba_ppu.h"
#include "view_config.h"

extern "C" unsigned g_ws_extra_left;
extern "C" unsigned g_ws_extra_right;

namespace smw_view {

constexpr int kNativeW = static_cast<int>(gbarecomp::kNativeViewWidth);
constexpr int kNativeH = static_cast<int>(gbarecomp::kNativeViewHeight);

// Guest RAM.
constexpr std::uint32_t kLevelPtr = 0x03003F98u;  // current level object
constexpr std::uint32_t kLayer1X = 0x03002C04u;
constexpr std::uint32_t kLayer1Y = 0x03002C08u;
constexpr std::uint32_t kSoftOam = 0x03002C28u;

inline int extra_lr() { return static_cast<int>(g_ws_extra_left + g_ws_extra_right); }
inline int view_w() { return kNativeW + extra_lr(); }
inline int view_h() {
    return kNativeH + static_cast<int>(gba::g_ws_extra_top + gba::g_ws_extra_bottom);
}

}  // namespace smw_view
