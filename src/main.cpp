#include "game/game_engine.hpp"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace {

void print_usage() {
    std::cout << "Harvest Moon: Friends of Mineral Town - Native PC Port (GBA HAL + Coop)\n"
              << "Options:\n"
              << "  --scale <1..6>          Integer window scale (default: 3 -> 720x480)\n"
              << "  --rom <path>            Path to any Friends of Mineral Town (US) .gba ROM\n"
              << "  --save <path>           Save file path (default: saves/fomt.sav)\n"
              << "  --host [port]           Start UDP Coop Host (default port 7777)\n"
              << "  --connect <ip:port>     Connect to UDP Coop Host (e.g. 127.0.0.1:7777)\n"
              << "  --fallback              Use standalone HAL scene mode instead of ARM7 ROM CPU\n"
              << "  --headless-frames <N>   Run N frames headless and exit (diagnostic)\n"
              << "  --help                  Show this help\n\n"
              << "Controls:\n"
              << "  Move: Arrows / WASD / Gamepad D-Pad & Stick\n"
              << "  Use Tool / Confirm: Z / K / Pad A\n"
              << "  Script Dialog (mary VM): X / J / Pad B\n"
              << "  Cycle Tool: Q / E / U / L / Pad LB & RB\n"
              << "  Next Day (Sleep): Enter / Space / Pad Start\n"
              << "  Save (.sav): Backspace / RShift / F5\n"
              << "  Host UDP (:7777): F9 | Join 127.0.0.1: F10 | Scale: 1..5 | Fullscreen: F11\n";
}

} // namespace

int main(int argc, char* argv[]) {
    hm::game::EngineConfig cfg{};
    int headless_frames = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        } else if (arg == "--scale" && i + 1 < argc) {
            cfg.scale = std::atoi(argv[++i]);
        } else if (arg == "--rom" && i + 1 < argc) {
            cfg.rom_path = argv[++i];
        } else if (arg == "--save" && i + 1 < argc) {
            cfg.save_path = argv[++i];
        } else if (arg == "--host") {
            cfg.start_host = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                cfg.host_port = static_cast<hm::hal::u16>(std::atoi(argv[++i]));
            }
        } else if (arg == "--connect" && i + 1 < argc) {
            cfg.connect_addr = argv[++i];
        } else if (arg == "--fallback") {
            cfg.use_cpu_scaffold = false;
        } else if (arg == "--headless-frames" && i + 1 < argc) {
            headless_frames = std::atoi(argv[++i]);
            cfg.headless = true;
        } else if (!arg.empty() && arg[0] != '-') {
            // Positional argument: e.g. dragging and dropping a .gba file onto hmfomt_pc.exe
            cfg.rom_path = arg;
        }
    }

    hm::game::GameEngine engine;
    if (!engine.init(cfg)) {
        std::cerr << "Failed to initialize Harvest Moon: FoMT PC engine.\n";
        return 1;
    }

    if (!cfg.headless) {
        if (engine.is_native_rom_mode()) {
            std::cout << "[ROM] Loaded official US ROM: " << engine.rom_info().title
                      << " [" << engine.rom_info().game_code << "]\n";
        } else {
            std::cout << "[ROM] No Friends of Mineral Town (US) .gba ROM detected. Running in clean-room fallback mode.\n"
                      << "      To play with official game code & assets, place any .gba file of the game\n"
                      << "      in the game folder, drag & drop it onto hmfomt_pc.exe, or run with --rom <path>.\n";
        }
    }

    if (headless_frames > 0) {
        for (int f = 0; f < headless_frames; ++f) {
            if (!engine.tick_frame(false)) break;
        }
        return 0;
    }

    return engine.run();
}
