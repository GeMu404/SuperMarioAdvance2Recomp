#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "runtime.h"
#include "smw_extended_view.h"

#if defined(GBAGAME_RECOMP_UI)
#include "game_launcher_boot.h"
#endif

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 ||
            std::strcmp(argv[i], "-h") == 0) {
            std::printf(
                "SuperMarioWorldRecomp [--bios <path>] [--rom <path>] [game.toml]\n");
            return 0;
        }
    }

    gbarecomp::RunOptions opts;
    opts.builtin_game_name = "Super Mario Advance 2: Super Mario World";
    opts.builtin_rom_sha1 = "5101ddf223d1d918928fe1f306b63a42ada14a5e";
    opts.builtin_rom_crc32 = 0x5206880Au;
    opts.mod_game_id = "super-mario-world-us";
    opts.mod_owns_adaptive_view = true;

    opts.resize_driven_view = true;
    // View size comes from game.toml [video] view_height (+aspect); the
    // runtime derives width and margins (gbarecomp view_config.h).
    opts.launcher_expose_widescreen = true;
    opts.launcher_expose_adaptive_view = true;
    opts.extended_view_init = smw::install_extended_view;

    opts.launcher_region = "USA";
    opts.launcher_game_config = "game.toml";
    opts.launcher_save_path = "saves/super_mario_world_usa.sav";

#if defined(GBAGAME_RECOMP_UI)
    std::vector<std::string> args(argv, argv + argc);
    if (game_launcher_preboot(args, opts)) return 0;
    std::vector<char*> av;
    av.reserve(args.size());
    for (auto& arg : args) av.push_back(arg.data());
    return gbarecomp::run_game(static_cast<int>(av.size()), av.data(), opts);
#else
    return gbarecomp::run_game(argc, argv, opts);
#endif
}
