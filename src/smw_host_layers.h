#pragma once

#include <cstdint>

namespace smw {

// Host-side compose that is NOT limited by GBA OAM 8-bit Y or BG3 status
// chrome. Captures full int16 sprite screen positions at Sub0808C4A4 and
// finishes the tall framebuffer after PPU latch (HUD without grey plate,
// sprites placed with real Y).
void install_host_layers();

}  // namespace smw
