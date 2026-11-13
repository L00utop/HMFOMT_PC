#include "game/fomt_rom_assets.hpp"
#include "game/game_engine.hpp"

#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::fprintf(stderr, "CHECK FAILED at line %d: %s\n", __LINE__, #cond); \
            return 1;                                                           \
        }                                                                       \
    } while (0)
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

using namespace hm::game;
using namespace hm::hal;

namespace {

void push_le16(std::vector<u8>& v, u16 val) {
    v.push_back(static_cast<u8>(val & 0xFFu));
    v.push_back(static_cast<u8>(val >> 8));
}

void push_le32(std::vector<u8>& v, u32 val) {
    v.push_back(static_cast<u8>(val & 0xFFu));
    v.push_back(static_cast<u8>((val >> 8) & 0xFFu));
    v.push_back(static_cast<u8>((val >> 16) & 0xFFu));
    v.push_back(static_cast<u8>((val >> 24) & 0xFFu));
}

std::string find_baserom() {
    for (const char* candidate : {"baserom.gba", "../baserom.gba", "../../baserom.gba"}) {
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
    return {};
}

std::size_t count_distinct_colors(const u32* fb, std::size_t count) {
    std::set<u32> colors;
    for (std::size_t i = 0; i < count; ++i) {
        colors.insert(fb[i]);
    }
    return colors.size();
}

} // namespace

int main() {
    // 1. unpack_fomt: raw mode (lz=4, huff=0, filt=0) and byte-delta filter (filt=2)
    {
        // Header: out_size = 4, word0 big-endian bit stream: flags(0x04) + "ABCD"
        // Actually flags + 4 raw bytes need 40 bits => 2 32-bit LE words
        std::vector<u8> blob;
        const u32 out_sz = 4u;
        push_le32(blob, out_sz << 8);
        // Word 1 bits [31..0]: [flags=0x04 (bits31..24)] ['A'=0x41 (23..16)] ['B'=0x42 (15..8)] ['C'=0x43 (7..0)]
        push_le32(blob, 0x04414243u);
        // Word 2 bits [31..24]: ['D'=0x44]
        push_le32(blob, 0x44000000u);

        std::vector<u8> out;
        CHECK(unpack_fomt(blob.data(), blob.size(), 0u, out));
        CHECK(out.size() == 4u);
        CHECK(out[0] == 'A' && out[1] == 'B' && out[2] == 'C' && out[3] == 'D');

        // Same payload with filt=2 (byte delta sum): flags = 0x44, deltas [1, 2, 3, 4] -> [1, 3, 6, 10]
        blob.clear();
        push_le32(blob, out_sz << 8);
        push_le32(blob, 0x44010203u);
        push_le32(blob, 0x04000000u);
        CHECK(unpack_fomt(blob.data(), blob.size(), 0u, out));
        CHECK(out.size() == 4u);
        CHECK(out[0] == 1u && out[1] == 3u && out[2] == 6u && out[3] == 10u);

        // Rejection tests: nullptr, misaligned, OOB, out_size=0, truncated stream
        CHECK(!unpack_fomt(nullptr, 16u, 0u, out));
        CHECK(!unpack_fomt(blob.data(), blob.size(), 1u, out));
        CHECK(!unpack_fomt(blob.data(), blob.size(), blob.size(), out));
        std::vector<u8> bad_zero(12u, 0u);
        CHECK(!unpack_fomt(bad_zero.data(), bad_zero.size(), 0u, out));
        std::vector<u8> trunc(8u, 0u);
        trunc[1] = 32u; // out_size = 8192 but only 4 stream bytes follow
        trunc[4] = 0x04u;
        CHECK(!unpack_fomt(trunc.data(), trunc.size(), 0u, out));
    }

    // 2. SpriteArchiveView parsing & bounds safety
    {
        std::vector<u8> arch;
        push_le32(arch, 1u); // n_anim = 1
        push_le16(arch, 1u); // frames = 1
        push_le16(arch, 0u); // seq0 = 0

        push_le32(arch, 1u); // n_cel = 1
        push_le16(arch, 1u); push_le16(arch, 0u); // oam_n=1, oam_i=0
        push_le16(arch, 0u); push_le16(arch, 0u); // unk4, tile_i=0
        push_le16(arch, 0u); push_le16(arch, 0u); // unk8, pal_i=0
        push_le16(arch, 0u); push_le16(arch, 0u); // unkC, unkE

        push_le32(arch, 1u); // n_oam = 1
        push_le16(arch, 0x40F8u); push_le16(arch, 0x00F8u); // attr0, attr1
        push_le16(arch, 0x0000u); push_le16(arch, 0x0000u); // attr2, pad

        push_le32(arch, 1u); // n_tile = 1
        for (int i = 0; i < 32; ++i) arch.push_back(static_cast<u8>(i));

        push_le32(arch, 1u); // n_pal = 1
        for (int i = 0; i < 32; ++i) arch.push_back(static_cast<u8>(i));

        push_le32(arch, 0u); // n_unk = 0

        push_le32(arch, 1u); // n_seq = 1
        push_le16(arch, 0u); // cel_id = 0
        push_le16(arch, 8u); // duration = 8

        SpriteArchiveView view{};
        CHECK(view.parse(arch.data(), arch.size()));
        CHECK(view.valid());
        CHECK(view.cel_for_anim(0u, 3u) == 0u);
        CHECK(view.tile_ptr(0u) != nullptr);
        CHECK(view.tile_ptr(1u) == nullptr);
        CHECK(view.pal_ptr(0u) != nullptr);
        CHECK(view.pal_ptr(1u) == nullptr);

        // Truncated archive rejected cleanly
        CHECK(!view.parse(arch.data(), arch.size() - 2u));
        CHECK(!view.parse(arch.data(), 10u));
    }

    // 3. Explicit procedural fallback when ROM is missing
    {
        std::filesystem::remove_all("build_test_rom_fallback");
        EngineConfig cfg{};
        cfg.headless = true;
        cfg.use_cpu_scaffold = false;
        cfg.rom_path = "nonexistent_missing_rom_for_test.gba";
        cfg.save_path = "build_test_rom_fallback/fomt.sav";

        GameEngine engine;
        CHECK(engine.init(cfg));
        CHECK(!engine.rom_info().valid_fomt_us);
        CHECK(!engine.has_rom_assets());
        CHECK(engine.tick_frame(false));
        CHECK(count_distinct_colors(engine.video().framebuffer(), SCREEN_WIDTH * SCREEN_HEIGHT) > 2u);

        engine.input().set_pressed(KEY_START, true);
        CHECK(engine.tick_frame(false));
        CHECK(engine.screen_mode() == ScreenMode::Gameplay);
        engine.input().set_pressed(KEY_START, false);
        CHECK(engine.tick_frame(false));
        CHECK(count_distinct_colors(engine.video().framebuffer(), SCREEN_WIDTH * SCREEN_HEIGHT) > 4u);

        engine.shutdown();
        std::filesystem::remove_all("build_test_rom_fallback");
    }

    // 4. Official US baserom.gba end-to-end integration test (when baserom.gba is present)
    const std::string rom_path = find_baserom();
    if (!rom_path.empty()) {
        std::filesystem::remove_all("build_test_rom_official");
        EngineConfig cfg{};
        cfg.headless = true;
        cfg.use_cpu_scaffold = false;
        cfg.rom_path = rom_path;
        cfg.save_path = "build_test_rom_official/fomt.sav";

        GameEngine engine;
        CHECK(engine.init(cfg));
        CHECK(engine.rom_info().valid_fomt_us);
        CHECK(engine.has_rom_assets());

        // Render Title screen and verify rich official color palette + Start vs Continue cursor movement
        CHECK(engine.screen_mode() == ScreenMode::Title);
        CHECK(engine.tick_frame(false));
        const std::size_t title_colors = count_distinct_colors(engine.video().framebuffer(), SCREEN_WIDTH * SCREEN_HEIGHT);
        CHECK(title_colors > 20u);
        std::vector<u32> title_start_fb(
            engine.video().framebuffer(), engine.video().framebuffer() + SCREEN_WIDTH * SCREEN_HEIGHT);
        engine.input().set_pressed(KEY_RIGHT, true);
        CHECK(engine.tick_frame(false));
        engine.input().set_pressed(KEY_RIGHT, false);
        CHECK(engine.tick_frame(false));
        std::size_t title_diff_px = 0;
        for (std::size_t i = 0; i < title_start_fb.size(); ++i) {
            if (title_start_fb[i] != engine.video().framebuffer()[i]) ++title_diff_px;
        }
        CHECK(title_diff_px > 50u);
        engine.input().set_pressed(KEY_LEFT, true);
        CHECK(engine.tick_frame(false));
        engine.input().set_pressed(KEY_LEFT, false);
        CHECK(engine.tick_frame(false));

        // Enter Gameplay via Start button
        engine.input().set_pressed(KEY_START, true);
        CHECK(engine.tick_frame(false));
        engine.input().set_pressed(KEY_START, false);
        CHECK(engine.screen_mode() == ScreenMode::Gameplay);
        CHECK(engine.tick_frame(false));

        const std::size_t farm_colors = count_distinct_colors(engine.video().framebuffer(), SCREEN_WIDTH * SCREEN_HEIGHT);
        CHECK(farm_colors > 20u);

        // Walk right, swing equipped tool, cycle tool, and check visual change in framebuffer
        std::vector<u32> before_fb(engine.video().framebuffer(), engine.video().framebuffer() + SCREEN_WIDTH * SCREEN_HEIGHT);
        engine.input().set_pressed(KEY_RIGHT, true);
        for (int i = 0; i < 6; ++i) CHECK(engine.tick_frame(false));
        engine.input().set_pressed(KEY_RIGHT, false);
        engine.input().set_pressed(KEY_A, true);
        CHECK(engine.tick_frame(false));
        engine.input().set_pressed(KEY_A, false);
        CHECK(engine.tick_frame(false));

        std::size_t diff_px = 0;
        const u32* after_fb = engine.video().framebuffer();
        for (std::size_t i = 0; i < before_fb.size(); ++i) {
            if (before_fb[i] != after_fb[i]) ++diff_px;
        }
        CHECK(diff_px > 100u);

        // Test seasonal palette/tile swap cleanly (including m_wpal snow transform in Winter)
        std::vector<u32> summer_fb;
        std::vector<u32> winter_fb;
        for (Season s : {Season::Summer, Season::Fall, Season::Winter, Season::Spring}) {
            engine.world().clock.season = s;
            CHECK(engine.tick_frame(false));
            CHECK(count_distinct_colors(engine.video().framebuffer(), SCREEN_WIDTH * SCREEN_HEIGHT) > 15u);
            if (s == Season::Summer) {
                summer_fb.assign(engine.video().framebuffer(), engine.video().framebuffer() + SCREEN_WIDTH * SCREEN_HEIGHT);
            } else if (s == Season::Winter) {
                winter_fb.assign(engine.video().framebuffer(), engine.video().framebuffer() + SCREEN_WIDTH * SCREEN_HEIGHT);
            }
        }
        std::size_t winter_diff_px = 0;
        for (std::size_t i = 0; i < summer_fb.size(); ++i) {
            if (summer_fb[i] != winter_fb[i]) ++winter_diff_px;
        }
        CHECK(winter_diff_px > 1000u);

        engine.shutdown();
        std::filesystem::remove_all("build_test_rom_official");
        std::printf("test_rom_assets: Official ROM checks PASSED (title=%zu colors, title_diff=%zu px, farm=%zu colors, move_diff=%zu px, winter_diff=%zu px)\n",
            title_colors, title_diff_px, farm_colors, diff_px, winter_diff_px);
    } else {
        std::puts("test_rom_assets: baserom.gba not found, skipped live ROM check.");
    }

    std::puts("test_rom_assets: ALL PASSED");
    return 0;
}
