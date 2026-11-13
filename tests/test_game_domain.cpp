#include "game/fomt_domain.hpp"
#include "game/game_engine.hpp"
#include "game/script_vm.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>

using namespace hm::game;
using namespace hm::hal;

int main() {
    // 1. Verify ToolId matches StanHash/fomt data/item/tool.def
    assert(TOOL_IRON_SICKLE == 0x00);
    assert(TOOL_IRON_HOE == 0x08);
    assert(TOOL_IRON_AXE == 0x10);
    assert(TOOL_IRON_HAMMER == 0x18);
    assert(TOOL_WATERING_CAN == 0x20);
    assert(TOOL_TURNIP_SEEDS == 0x32);
    assert(NUM_TOOLS == 0x51);

    // 2. FarmWorld crop lifecycle: Clear weed -> Hoe -> Plant Turnip -> Water -> Grow -> Harvest
    FarmWorld world;
    auto& f = world.farmers[0];
    const u32 initial_gold = f.gold;

    // Put a weed on (15, 10)
    world.tile_at(15, 10) = FarmTile{SoilState::Untilled, ObstacleKind::Weed, CropKind::None, 0};
    assert(world.apply_tool(f, 15, 10, TOOL_IRON_SICKLE).modified_tile);
    assert(world.tile_at(15, 10).obstacle == ObstacleKind::None);

    assert(world.apply_tool(f, 15, 10, TOOL_IRON_HOE).modified_tile);
    assert(world.tile_at(15, 10).soil == SoilState::Tilled);

    assert(world.apply_tool(f, 15, 10, TOOL_TURNIP_SEEDS).modified_tile);
    assert(world.tile_at(15, 10).crop == CropKind::Turnip);

    // Water & advance 4 days to reach Turnip maturity (days_to_mature = 4)
    for (int d = 0; d < 4; ++d) {
        if (world.tile_at(15, 10).soil == SoilState::Tilled) {
            assert(world.apply_tool(f, 15, 10, TOOL_WATERING_CAN).modified_tile);
        }
        world.advance_day();
    }
    assert(world.tile_at(15, 10).is_mature());
    const auto harvest = world.apply_tool(f, 15, 10, TOOL_IRON_HOE);
    assert(harvest.harvested);
    assert(f.gold == initial_gold + 60u);

    // 3. Serialize & Deserialize roundtrip
    const auto saved = world.serialize_state();
    FarmWorld restored;
    assert(restored.deserialize_state(saved.data(), saved.size()));
    assert(restored.farmers[0].gold == f.gold);
    assert(restored.clock.day == world.clock.day);

    // 4. ScriptVm (AScriptEngine RIFF/SCR bytecode)
    // Compute vmmem[0] = (10 + 5) * 2 = 30; PUSH str 0; CALL 99; END
    const std::vector<u8> code = {
        0x23, 10,                      // PUSH8 10
        0x23, 5,                       // PUSH8 5
        0x07,                          // ADD -> 15
        0x23, 2,                       // PUSH8 2
        0x09,                          // MUL -> 30
        0x14, 0, 0, 0, 0,              // POP_STORE vmmem[0]
        0x23, 0,                       // PUSH8 str_id 0
        0x21, 99, 0, 0, 0,             // CALL 99
        0x20                           // END
    };
    const auto bin = ScriptVm::build_riff_script(code, {"Hello Mineral Town"});
    ScriptVm vm;
    std::string captured;
    vm.set_on_call([&](ScriptVm& s, i32 call_id) {
        assert(call_id == 99);
        captured = s.get_string(static_cast<u32>(s.pop()));
        return 0;
    });
    assert(vm.load(bin.data(), bin.size()));
    assert(vm.run_until_yield() == 0);
    assert(vm.get_var(0) == 30);
    assert(captured == "Hello Mineral Town");

    // 5. GameEngine Headless Title -> Start -> Farm -> PPU renders non-blank frames
    EngineConfig cfg{};
    cfg.headless = true;
    cfg.save_path = "build_test_engine/fomt.sav";
    {
        GameEngine engine;
        assert(engine.init(cfg));
        assert(engine.screen_mode() == ScreenMode::Title);
        assert(engine.tick_frame(false));

        // Press Start to enter Gameplay
        engine.input().set_pressed(KEY_START, true);
        assert(engine.tick_frame(false));
        engine.input().set_pressed(KEY_START, false);
        assert(engine.screen_mode() == ScreenMode::Gameplay);
        assert(engine.tick_frame(false));

        // Verify PPU rendered visible pixels (not all identical)
        const u32* fb = engine.video().framebuffer();
        const u32 first = fb[0];
        bool differs = false;
        for (std::size_t i = 1; i < SCREEN_WIDTH * SCREEN_HEIGHT; ++i) {
            if (fb[i] != first) { differs = true; break; }
        }
        assert(differs);
    }
    std::filesystem::remove_all("build_test_engine");

    std::puts("test_game_domain: ALL PASSED");
    return 0;
}
