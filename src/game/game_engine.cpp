#include "game_engine.hpp"

#include <SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace hm::game {

using namespace hm::hal;

namespace {

constexpr u8 EQUIP_CYCLE[] = {
    TOOL_IRON_HOE,
    TOOL_WATERING_CAN,
    TOOL_TURNIP_SEEDS,
    TOOL_POTATO_SEEDS,
    TOOL_TOMATO_SEEDS,
    TOOL_IRON_SICKLE,
    TOOL_IRON_AXE,
    TOOL_IRON_HAMMER,
};

u16 select_farm_metatile(const FarmTile& t) {
    switch (t.obstacle) {
        case ObstacleKind::Weed:    return bgtile::META_WEED;
        case ObstacleKind::Branch:  return bgtile::META_BRANCH;
        case ObstacleKind::Stone:   return bgtile::META_STONE;
        case ObstacleKind::Stump:   return bgtile::META_STUMP;
        case ObstacleKind::Boulder: return bgtile::META_BOULDER;
        default: break;
    }

    if (t.crop != CropKind::None) {
        const u8 st = t.growth_stage();
        if (st == 0) return bgtile::META_SEED;
        if (st == 1) return bgtile::META_SPROUT;
        if (st == 2) return bgtile::META_GROWING;
        if (t.crop == CropKind::Tomato) return bgtile::META_MATURE_R;
        if (t.crop == CropKind::Corn || t.crop == CropKind::Potato) return bgtile::META_MATURE_Y;
        return bgtile::META_MATURE_W;
    }

    if (t.soil == SoilState::Watered) return bgtile::META_WATERED;
    if (t.soil == SoilState::Tilled)  return bgtile::META_TILLED;
    return bgtile::META_GRASS;
}

void clear_screenblock(GbaBus& bus, u32 screen_block, u16 fill_entry = 0) {
    const u32 base = ADDR_VRAM_BASE + screen_block * 0x0800u;
    for (u32 i = 0; i < 1024u; ++i) {
        bus.write16(base + i * 2u, fill_entry);
    }
}

void hide_all_oam(GbaBus& bus) {
    for (u32 i = 0; i < 128u; ++i) {
        bus.write16(ADDR_OAM_BASE + i * 8u, 0x0200u); // Object mode 2 = Hidden
    }
}

void draw_ui_panel(GbaBus& bus, u32 screen_block, u32 x0, u32 y0, u32 x1, u32 y1, u8 pal_bank) {
    for (u32 y = y0; y <= y1; ++y) {
        for (u32 x = x0; x <= x1; ++x) {
            u16 idx = 4u;
            if (y == y0 && x == x0) idx = 0u;
            else if (y == y0 && x == x1) idx = 2u;
            else if (y == y1 && x == x0) idx = 6u;
            else if (y == y1 && x == x1) idx = 8u;
            else if (y == y0) idx = 1u;
            else if (y == y1) idx = 7u;
            else if (x == x0) idx = 3u;
            else if (x == x1) idx = 5u;
            write_bg_tile(bus, screen_block, 32u, x, y, static_cast<u16>(bgtile::UI_FRAME_BASE + idx), pal_bank);
        }
    }
}

void draw_gameplay_hud(
    GbaBus& bus,
    u32 screen_block,
    u8 pal_bank,
    const FarmWorld& world,
    const FarmerState& me,
    const std::string& net_lbl,
    bool dialog_open,
    const std::string& dialog_line1,
    const std::string& dialog_line2,
    const std::string& toast_msg,
    u32 toast_timer,
    bool opaque_rom_hud)
{
    clear_screenblock(bus, screen_block, 0u);
    const u16 fill_tile = opaque_rom_hud ? FomtRomAssets::HUD_OPAQUE_FILL_TILE : bgtile::SOLID_FILL;
    for (u32 x = 0; x < 30u; ++x) {
        write_bg_tile(bus, screen_block, 32u, x, 0u, fill_tile, pal_bank);
        write_bg_tile(bus, screen_block, 32u, x, 1u, fill_tile, pal_bank);
    }

    auto draw_custom_text = [&](u32 tx, u32 ty, const char* str, u16 font_base) {
        if (!str) return;
        for (const char* p = str; *p != '\0' && tx < 30u; ++p, ++tx) {
            unsigned char ch = static_cast<unsigned char>(*p);
            if (ch < 32u || ch >= 128u) ch = ' ';
            write_bg_tile(bus, screen_block, 32u, tx, ty, static_cast<u16>(font_base + (ch - 32u)), pal_bank);
        }
    };

    const u16 hud_font   = opaque_rom_hud ? FomtRomAssets::HUD_OPAQUE_FONT_BASE : bgtile::FONT_BASE;
    const u16 cream_font = opaque_rom_hud ? FomtRomAssets::HUD_CREAM_FONT_BASE  : bgtile::FONT_BASE;

    char top_line[32]{};
    std::snprintf(
        top_line,
        sizeof(top_line),
        "%s%02u %02u:%02u %-4s %5uG",
        world.clock.season_name(),
        static_cast<unsigned>(world.clock.day),
        static_cast<unsigned>(world.clock.hour),
        static_cast<unsigned>(world.clock.minute),
        world.clock.weather_name(),
        static_cast<unsigned>(me.gold));
    if (opaque_rom_hud) {
        draw_custom_text(0u, 0u, top_line, hud_font);
    } else {
        draw_bg_text(bus, screen_block, 32u, 0u, 0u, top_line, pal_bank);
    }

    char sub_line[32]{};
    std::snprintf(
        sub_line,
        sizeof(sub_line),
        "%-14.14s ST:%3u %s",
        tool_display_name(me.equipped_tool),
        static_cast<unsigned>(me.stamina),
        net_lbl.c_str());
    if (opaque_rom_hud) {
        draw_custom_text(0u, 1u, sub_line, hud_font);
    } else {
        draw_bg_text(bus, screen_block, 32u, 0u, 1u, sub_line, pal_bank);
    }

    if (dialog_open || toast_timer > 0u) {
        draw_ui_panel(bus, screen_block, 0u, 15u, 29u, 19u, pal_bank);
        if (dialog_open) {
            if (opaque_rom_hud) {
                draw_custom_text(1u, 16u, dialog_line1.c_str(), cream_font);
                draw_custom_text(1u, 17u, dialog_line2.c_str(), cream_font);
                draw_custom_text(22u, 18u, "[B]NEXT", cream_font);
            } else {
                draw_bg_text(bus, screen_block, 32u, 1u, 16u, dialog_line1.c_str(), pal_bank);
                draw_bg_text(bus, screen_block, 32u, 1u, 17u, dialog_line2.c_str(), pal_bank);
                draw_bg_text(bus, screen_block, 32u, 22u, 18u, "[B]NEXT", pal_bank);
            }
        } else {
            if (opaque_rom_hud) {
                draw_custom_text(1u, 17u, toast_msg.c_str(), cream_font);
            } else {
                draw_bg_text(bus, screen_block, 32u, 1u, 17u, toast_msg.c_str(), pal_bank);
            }
        }
    }
}

} // namespace

GameEngine::~GameEngine() {
    shutdown();
}

bool GameEngine::init(const EngineConfig& cfg) {
    shutdown();
    cfg_ = cfg;

    if (!cfg_.headless) {
        SDL_SetMainReady();
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
            return false;
        }
    }

    bus_.reset();
    audio_.init(HalAudio::DEFAULT_SAMPLE_RATE, cfg_.headless);
    bus_.attach_audio(&audio_);
    // Register FIFO refill callback for DirectSound DMA 1/2
    audio_.set_fifo_refill_callback([this](int fifo_idx) {
        bus_.trigger_fifo_dma(fifo_idx);
    });

    input_.init(cfg_.headless);
    save_.set_path(cfg_.save_path);

    if (!video_.init("Harvest Moon: FoMT PC (GBA HAL + Coop)", cfg_.scale, cfg_.headless)) {
        shutdown();
        return false;
    }

    ppu_.reset();
    rom_info_ = probe_and_load_rom(bus_, cfg_.rom_path);
    if (rom_info_.valid_fomt_us) {
        rom_assets_.load_from_rom(bus_.rom().data(), bus_.rom().size());
    } else {
        rom_assets_.clear();
    }

    coop_bridge_.reset();
    if (is_native_rom_mode()) {
        cpu_.reset(hal::ADDR_ROM_BASE);
        load_game();
        coop_bridge_.bind(net_);
        if (cfg_.start_host) {
            net_.start_host(cfg_.host_port);
            coop_bridge_.bind(net_);
        } else if (!cfg_.connect_addr.empty()) {
            net_.connect_client(cfg_.connect_addr);
            coop_bridge_.bind(net_);
        }
        initialized_ = true;
        timing_.reset();
        return true;
    }

    upload_gba_assets(bus_);
    world_.init_new_farm();

    // Load .sav if present
    load_game();
    uploaded_mode_initialized_ = false;
    uploaded_season_ = world_.clock.season;
    title_continue_selected_ = false;

    // Initialize ScriptVm with FoMT-style OnCall host procedures
    script_vm_.set_on_call([this](ScriptVm& vm, i32 call_id) -> int {
        if (call_id == 1) {
            // Procedure 1: Show Dialog line1 & line2 from string IDs popped from stack
            const u32 s2 = static_cast<u32>(vm.pop());
            const u32 s1 = static_cast<u32>(vm.pop());
            dialog_line1_ = vm.get_string(s1);
            dialog_line2_ = vm.get_string(s2);
            dialog_open_ = true;
            return 1; // Yield until player presses B/A
        }
        if (call_id == 2) {
            // Procedure 2: Grant stamina bonus pushed on stack
            const i32 bonus = vm.pop();
            auto& f = world_.farmers[net_.local_player_id()];
            f.stamina = static_cast<u8>(std::min<int>(100, static_cast<int>(f.stamina) + bonus));
            return 0;
        }
        return 0;
    });

    // Enable GBA Master Sound + DMG Ch1 on L/R
    bus_.io_write16(reg::SOUNDCNT_X, 0x0080u);
    bus_.io_write16(reg::SOUNDCNT_L, 0xFF77u);
    bus_.io_write16(reg::SOUNDCNT_H, 0x0002u);

    if (cfg_.start_host) {
        net_.start_host(cfg_.host_port);
        mode_ = ScreenMode::Gameplay;
    } else if (!cfg_.connect_addr.empty()) {
        net_.connect_client(cfg_.connect_addr);
        mode_ = ScreenMode::Gameplay;
    } else {
        mode_ = ScreenMode::Title;
    }

    initialized_ = true;
    timing_.reset();
    return true;
}

void GameEngine::shutdown() {
    net_.disconnect();
    if (initialized_) {
        save_game();
        initialized_ = false;
    }
    coop_bridge_.reset();
    rom_assets_.clear();
    uploaded_mode_initialized_ = false;
    video_.shutdown();
    input_.shutdown();
    audio_.shutdown();
    if (!cfg_.headless && SDL_WasInit(0) != 0) {
        SDL_Quit();
    }
}

void GameEngine::play_sfx(u16 freq_reg, u8 duty, u8 len) {
    bus_.io_write16(reg::SOUND1CNT_L, 0x0000u);
    bus_.io_write16(reg::SOUND1CNT_H, static_cast<u16>(0xA200u | ((duty & 3u) << 6) | (len & 0x3Fu)));
    bus_.io_write16(reg::SOUND1CNT_X, static_cast<u16>(0xC000u | (freq_reg & 0x07FFu)));
}

void GameEngine::trigger_welcome_script() {
    // Build a real RIFF/SCR bytecode script using AScriptEngine opcodes:
    // PUSH8 0; PUSH8 1; CALL 1 (show dialog); PUSH8 5; CALL 2 (+5 stamina); END
    std::vector<u8> code = {
        0x23, 0x00,                   // PUSH8 str 0
        0x23, 0x01,                   // PUSH8 str 1
        0x21, 0x01, 0x00, 0x00, 0x00, // CALL 1 (Dialog + Yield)
        0x23, 0x02,                   // PUSH8 str 2
        0x23, 0x03,                   // PUSH8 str 3
        0x21, 0x01, 0x00, 0x00, 0x00, // CALL 1 (Dialog + Yield)
        0x23, 0x05,                   // PUSH8 5
        0x21, 0x02, 0x00, 0x00, 0x00, // CALL 2 (+5 stamina)
        0x20                          // END
    };
    const std::string status_str = rom_info_.valid_fomt_us
        ? "ROM US OK! Use A=Tool Start=Day"
        : "Use L/R=Tool A=Use Start=NextDay";

    auto bin = ScriptVm::build_riff_script(code, {
        "Mineral Town Coop (mary VM)",
        status_str,
        "F9=Host UDP  F10=Join 127.0.0.1",
        "Restored +5 Stamina! Have fun!"
    });
    script_vm_.load(bin.data(), bin.size());
    script_vm_.run_until_yield();
}

bool GameEngine::save_game() {
    if (is_native_rom_mode()) {
        return save_.flush_from_bus(bus_, true);
    }
    // Write FarmWorld state into GBA Flash/SRAM region (0x0E000000) via bus, then flush to .sav
    const auto bytes = world_.serialize_state();
    const std::size_t max_copy = std::min(bytes.size(), SAVE_SIZE);
    std::memcpy(bus_.save_ram(), bytes.data(), max_copy);
    // Mark bus save dirty by writing byte 0 through Flash/SRAM path or force flush
    return save_.flush_from_bus(bus_, true);
}

bool GameEngine::load_game() {
    if (!save_.load_into_bus(bus_)) return false;
    if (is_native_rom_mode()) return true;
    return world_.deserialize_state(bus_.save_ram(), SAVE_SIZE);
}

void GameEngine::target_tile_in_front(const FarmerState& f, i32& out_tx, i32& out_ty) const {
    const i32 cx = (f.x + 8) / 16;
    const i32 cy = (f.y + 8) / 16;
    out_tx = cx;
    out_ty = cy;
    switch (f.facing) {
        case Facing::Down:  out_ty += 1; break;
        case Facing::Up:    out_ty -= 1; break;
        case Facing::Left:  out_tx -= 1; break;
        case Facing::Right: out_tx += 1; break;
    }
}

void GameEngine::step_title_screen() {
    if (input_.is_just_pressed(KEY_UP) || input_.is_just_pressed(KEY_DOWN) ||
        input_.is_just_pressed(KEY_LEFT) || input_.is_just_pressed(KEY_RIGHT)) {
        title_continue_selected_ = !title_continue_selected_;
        play_sfx(1560u, 1u, 18u);
    }
    if (input_.is_just_pressed(KEY_START) || input_.is_just_pressed(KEY_A)) {
        if (title_continue_selected_) {
            load_game();
        }
        play_sfx(1750u, 2u, 32u);
        mode_ = ScreenMode::Gameplay;
        return;
    }
    if (input_.is_just_pressed(KEY_SELECT)) {
        net_.start_host(cfg_.host_port);
        mode_ = ScreenMode::Gameplay;
    }
}

void GameEngine::step_gameplay() {
    net_.tick(world_);

    // Continuous world clock (host or offline drives time; never freezes on local dialog)
    if (net_.role() != net::NetRole::Client) {
        world_.clock.step_tick(45u);
    }

    auto& me = world_.farmers[net_.local_player_id()];
    me.connected = true;
    me.equipped_tool = EQUIP_CYCLE[tool_cursor_];

    if (toast_timer_ > 0) --toast_timer_;

    if (dialog_open_) {
        if (input_.is_just_pressed(KEY_B) || input_.is_just_pressed(KEY_A)) {
            play_sfx(1620u, 1u, 48u);
            if (script_vm_.run_until_yield() == 0) {
                dialog_open_ = false;
            }
        }
        return;
    }

    // Tool selection L/R
    constexpr std::size_t tool_count = sizeof(EQUIP_CYCLE) / sizeof(EQUIP_CYCLE[0]);
    if (input_.is_just_pressed(KEY_R)) {
        tool_cursor_ = (tool_cursor_ + 1u) % tool_count;
        me.equipped_tool = EQUIP_CYCLE[tool_cursor_];
        play_sfx(1820u, 1u, 52u);
    }
    if (input_.is_just_pressed(KEY_L)) {
        tool_cursor_ = (tool_cursor_ + tool_count - 1u) % tool_count;
        me.equipped_tool = EQUIP_CYCLE[tool_cursor_];
        play_sfx(1780u, 1u, 52u);
    }

    // Movement
    i32 dx = 0, dy = 0;
    if (input_.is_held(KEY_LEFT))  { dx -= 2; me.facing = Facing::Left; }
    if (input_.is_held(KEY_RIGHT)) { dx += 2; me.facing = Facing::Right; }
    if (input_.is_held(KEY_UP))    { dy -= 2; me.facing = Facing::Up; }
    if (input_.is_held(KEY_DOWN))  { dy += 2; me.facing = Facing::Down; }

    if (dx != 0 || dy != 0) {
        me.anim_frame = static_cast<u8>(1u + ((frame_count_ / 8u) & 1u));

        auto can_walk = [&](i16 nx, i16 ny) -> bool {
            if (nx < 0 || ny < 0 ||
                nx > static_cast<i16>(FARM_FIELD_WIDTH * 16 - 16) ||
                ny > static_cast<i16>(FARM_FIELD_HEIGHT * 16 - 16)) {
                return false;
            }
            const i32 tx = (nx + 8) / 16;
            const i32 ty = (ny + 10) / 16;
            if (!world_.in_bounds(tx, ty)) return false;
            return !world_.tile_at(static_cast<u32>(tx), static_cast<u32>(ty)).blocks_movement();
        };

        if (dx != 0 && can_walk(static_cast<i16>(me.x + dx), me.y)) me.x = static_cast<i16>(me.x + dx);
        if (dy != 0 && can_walk(me.x, static_cast<i16>(me.y + dy))) me.y = static_cast<i16>(me.y + dy);
    } else {
        me.anim_frame = 0;
    }

    if (me.tool_anim_timer > 0) --me.tool_anim_timer;

    // Action A: Use tool on tile in front
    if (input_.is_just_pressed(KEY_A)) {
        i32 tx = 0, ty = 0;
        target_tile_in_front(me, tx, ty);
        me.tool_anim_timer = 10;
        net_.send_use_tool(world_, tx, ty, me.equipped_tool);
        play_sfx(1480u, 0u, 45u);
    }

    // Action B: Trigger AScriptEngine (mary VM) event dialog
    if (input_.is_just_pressed(KEY_B)) {
        trigger_welcome_script();
        play_sfx(1680u, 2u, 40u);
    }

    // Start: Advance Day (sleep)
    if (input_.is_just_pressed(KEY_START)) {
        net_.send_advance_day(world_);
        toast_msg_ = "New Day! Watered crops grew.";
        toast_timer_ = 120;
        play_sfx(1860u, 2u, 24u);
    }

    // Select: Quick save to Flash/.sav
    if (input_.is_just_pressed(KEY_SELECT)) {
        save_game();
        toast_msg_ = "Saved to saves/fomt.sav!";
        toast_timer_ = 120;
        play_sfx(1900u, 2u, 36u);
    }
}

void GameEngine::sync_title_ppu() {
    if (rom_assets_.loaded()) {
        if (!uploaded_mode_initialized_ || uploaded_mode_ != ScreenMode::Title) {
            rom_assets_.upload_title_vram_and_pal(bus_);
            uploaded_mode_initialized_ = true;
            uploaded_mode_ = ScreenMode::Title;
        }

        // Mode 0: BG0..BG3 64x32 ScreenBlocks 24,26,28,30, VOFS=0, 1D OBJ enabled
        bus_.io_write16(reg::DISPCNT, 0x1F40u);
        bus_.io_write16(reg::BG0CNT,  0x5801u); // layer 0 (frontmost BG) prio 1
        bus_.io_write16(reg::BG1CNT,  0x5A01u); // layer 1 prio 1
        bus_.io_write16(reg::BG2CNT,  0x5C02u); // layer 2 prio 2
        bus_.io_write16(reg::BG3CNT,  0x5E03u); // layer 3 (sky back) prio 3
        for (u32 bg = 0; bg < 4u; ++bg) {
            bus_.io_write16(reg::BG0HOFS + bg * 4u, 0u);
            bus_.io_write16(reg::BG0VOFS + bg * 4u, 0u);
        }
        bus_.io_write16(reg::BLDCNT, 0u);
        bus_.io_write16(reg::BLDALPHA, 0u);

        hide_all_oam(bus_);
        u32 oam_slot = 0u;
        const i16 bounce = ((frame_count_ / 24u) % 2u) ? -1 : 0;
        rom_assets_.write_title_cursor_oam(
            bus_, oam_slot, static_cast<i16>(title_continue_selected_ ? 132 : 40), 112, frame_count_, 0u);
        rom_assets_.write_title_oam(bus_, oam_slot, 0u, 120, static_cast<i16>(10 + bounce), 0u, frame_count_);
        rom_assets_.write_title_oam(bus_, oam_slot, title_continue_selected_ ? 1u : 2u, 44, 108, 0u, frame_count_);
        rom_assets_.write_title_oam(bus_, oam_slot, title_continue_selected_ ? 5u : 4u, 136, 108, 0u, frame_count_);
        rom_assets_.write_title_oam(bus_, oam_slot, 7u, 120, 142, 0u, frame_count_);
        return;
    }

    if (!uploaded_mode_initialized_ || uploaded_mode_ != ScreenMode::Title) {
        upload_gba_assets(bus_);
        uploaded_mode_initialized_ = true;
        uploaded_mode_ = ScreenMode::Title;
    }

    // Mode 0: BG0 = Sky/Hills (ScreenBlock 28), BG1 = Text Banner (ScreenBlock 30), OBJ enabled
    bus_.io_write16(reg::DISPCNT, 0x1340u); // Mode 0, 1D OBJ, BG0, BG1, OBJ
    bus_.io_write16(reg::BG0CNT,  static_cast<u16>((28u << 8) | 3u)); // Priority 3
    bus_.io_write16(reg::BG1CNT,  static_cast<u16>((30u << 8) | 1u)); // Priority 1
    bus_.io_write16(reg::BG0HOFS, static_cast<u16>((frame_count_ / 3u) & 0xFFu));
    bus_.io_write16(reg::BG0VOFS, 0u);
    bus_.io_write16(reg::BG1HOFS, 0u);
    bus_.io_write16(reg::BG1VOFS, 0u);

    // Alpha blend subtle shimmer on BG1 text
    const u8 pulse = static_cast<u8>(11 + ((frame_count_ / 16u) % 6u));
    bus_.io_write16(reg::BLDCNT, 0x0142u); // BG1 1st target, alpha blend with BG0
    bus_.io_write16(reg::BLDALPHA, static_cast<u16>(pulse | ((16u - pulse) << 8)));

    clear_screenblock(bus_, 28u, 0u);
    clear_screenblock(bus_, 30u, 0u);
    hide_all_oam(bus_);

    for (u32 y = 0; y < 20u; ++y) {
        const u16 tile = (y < 4u) ? (bgtile::SKY_GRAD_BASE + 0u)
                       : (y < 8u) ? (bgtile::SKY_GRAD_BASE + 1u)
                       : (y < 12u) ? (bgtile::SKY_GRAD_BASE + 2u)
                       : (y < 15u) ? bgtile::HILL_TILE
                                   : bgtile::META_GRASS;
        for (u32 x = 0; x < 32u; ++x) {
            const u16 final_tile = (y >= 15u) ? static_cast<u16>(bgtile::META_GRASS + ((y & 1u) * 2u + (x & 1u))) : tile;
            const u8 pal = (y >= 15u) ? 0u : 3u;
            write_bg_tile(bus_, 28u, 32u, x, y, final_tile, pal);
        }
    }

    // Banner Panel on BG1
    draw_ui_panel(bus_, 30u, 3u, 3u, 26u, 8u, 2u);

    draw_bg_text(bus_, 30u, 32u, 5, 4, "HARVEST MOON: FoMT PC", 2u);
    draw_bg_text(bus_, 30u, 32u, 5, 6, "GBA PPU + COOP ENGINE", 2u);
    draw_bg_text(bus_, 30u, 32u, 7, 12, "PRESS ENTER / Z", 2u);
    draw_bg_text(bus_, 30u, 32u, 4, 17, "F9:HOST UDP  F10:COOP", 2u);

    // Animated Clouds + Turnip Mascot OAM Sprites
    const i16 c1x = static_cast<i16>((frame_count_ / 2u) % 260u) - 16;
    const i16 c2x = static_cast<i16>((frame_count_ / 3u + 120u) % 260u) - 16;
    write_oam_sprite(bus_, 0, c1x, 14, objtile::CLOUD_16X16, 0, 2);
    write_oam_sprite(bus_, 1, c2x, 22, objtile::CLOUD_16X16, 0, 2);

    const i16 bounce = ((frame_count_ / 18u) % 2u) ? -2 : 0;
    write_oam_sprite(bus_, 2, 112, static_cast<i16>(114 + bounce), objtile::TURNIP_16X16, 2, 1);
}

void GameEngine::sync_gameplay_ppu() {
    const auto& me = world_.farmers[net_.local_player_id()];

    if (rom_assets_.loaded()) {
        if (!uploaded_mode_initialized_ || uploaded_mode_ != ScreenMode::Gameplay) {
            upload_gba_assets(bus_);
            rom_assets_.upload_gameplay_map_base(bus_, world_.clock.season);
            uploaded_mode_initialized_ = true;
            uploaded_mode_ = ScreenMode::Gameplay;
            uploaded_season_ = world_.clock.season;
        } else if (world_.clock.season != uploaded_season_) {
            rom_assets_.upload_season_overlays(bus_, world_.clock.season);
            uploaded_season_ = world_.clock.season;
        }

        bus_.io_write16(reg::DISPCNT, 0x1F40u); // Mode 0, 1D OBJ, BG0 + BG1 + BG2 + BG3 + OBJ
        bus_.io_write16(reg::BG0CNT,  static_cast<u16>((24u << 8) | (2u << 2) | 0u)); // HUD: CharBase 2, SB 24, Prio 0
        bus_.io_write16(reg::BG1CNT,  static_cast<u16>((26u << 8) | 3u)); // Map t1: CharBase 0, SB 26, Prio 3
        bus_.io_write16(reg::BG2CNT,  static_cast<u16>((28u << 8) | 2u)); // Map t2: CharBase 0, SB 28, Prio 2
        bus_.io_write16(reg::BG3CNT,  static_cast<u16>((30u << 8) | 0u)); // Map t3: CharBase 0, SB 30, Prio 0
        bus_.io_write16(reg::BLDCNT,  0u);
        bus_.io_write16(reg::BLDALPHA, 0u);

        rom_assets_.compose_farm_layers(world_, composed_t1_, composed_t2_, composed_t3_);

        const i32 me_map_x = FomtRomAssets::FIELD_ORIGIN_PX + me.x;
        const i32 me_map_y = FomtRomAssets::FIELD_ORIGIN_PY + me.y;
        const i32 max_cam_x = static_cast<i32>(FomtRomAssets::FARM_MAP_W * 8u - SCREEN_WIDTH);
        const i32 max_cam_y = static_cast<i32>(FomtRomAssets::FARM_MAP_H * 8u - SCREEN_HEIGHT);
        const i32 cam_x = std::clamp<i32>(me_map_x - static_cast<i32>(SCREEN_WIDTH / 2 - 8), 0, max_cam_x);
        const i32 cam_y = std::clamp<i32>(me_map_y - static_cast<i32>(SCREEN_HEIGHT / 2 - 8), 0, max_cam_y);

        const u32 cam_tile8_x = static_cast<u32>(cam_x >> 3);
        const u32 cam_tile8_y = static_cast<u32>(cam_y >> 3);
        const u16 hofs = static_cast<u16>(cam_x & 7);
        const u16 vofs = static_cast<u16>(cam_y & 7);

        bus_.io_write16(reg::BG0HOFS, 0u);
        bus_.io_write16(reg::BG0VOFS, 0u);
        for (u32 bg = 1u; bg < 4u; ++bg) {
            bus_.io_write16(reg::BG0HOFS + bg * 4u, hofs);
            bus_.io_write16(reg::BG0VOFS + bg * 4u, vofs);
        }

        const u32 sb1 = ADDR_VRAM_BASE + 26u * 0x0800u;
        const u32 sb2 = ADDR_VRAM_BASE + 28u * 0x0800u;
        const u32 sb3 = ADDR_VRAM_BASE + 30u * 0x0800u;
        for (u32 sy = 0; sy < 21u; ++sy) {
            const u32 my = std::min<u32>(cam_tile8_y + sy, FomtRomAssets::FARM_MAP_H - 1u);
            for (u32 sx = 0; sx < 31u; ++sx) {
                const u32 mx = std::min<u32>(cam_tile8_x + sx, FomtRomAssets::FARM_MAP_W - 1u);
                const std::size_t src_idx = static_cast<std::size_t>(my * FomtRomAssets::FARM_MAP_W + mx);
                const u32 dst_off = (sy * 32u + sx) * 2u;
                bus_.write16(sb1 + dst_off, composed_t1_[src_idx]);
                bus_.write16(sb2 + dst_off, composed_t2_[src_idx]);
                bus_.write16(sb3 + dst_off, composed_t3_[src_idx]);
            }
        }

        draw_gameplay_hud(
            bus_, 24u, 0u, world_, me, net_.status_label(),
            dialog_open_, dialog_line1_, dialog_line2_, toast_msg_, toast_timer_, true);

        hide_all_oam(bus_);
        u32 oam_slot = 0u;
        u32 vram_tile_cursor = 24u;

        // Target tile cursor on farm plot (OBJ pal 0, prio 2)
        i32 ttx = 0, tty = 0;
        target_tile_in_front(me, ttx, tty);
        if (world_.in_bounds(ttx, tty) && (frame_count_ % 30u) < 22u) {
            const i16 cx = static_cast<i16>(FomtRomAssets::FIELD_ORIGIN_PX + ttx * 16 - cam_x);
            const i16 cy = static_cast<i16>(FomtRomAssets::FIELD_ORIGIN_PY + tty * 16 - cam_y);
            if (cx >= -16 && cx < static_cast<i16>(SCREEN_WIDTH) && cy >= -16 && cy < static_cast<i16>(SCREEN_HEIGHT)) {
                write_oam_sprite(bus_, oam_slot++, cx, cy, objtile::CURSOR_16X16, 0u, 2u);
            }
        }

        // Local + Coop Farmers with official ROM sprite animations
        for (std::size_t i = 0; i < MAX_COOP_PLAYERS; ++i) {
            const auto& f = world_.farmers[i];
            if (!f.connected || f.map_id != me.map_id) continue;

            const i16 sx = static_cast<i16>(FomtRomAssets::FIELD_ORIGIN_PX + f.x + 8 - cam_x);
            const i16 sy = static_cast<i16>(FomtRomAssets::FIELD_ORIGIN_PY + f.y + 6 - cam_y);
            if (sx < -24 || sx >= static_cast<i16>(SCREEN_WIDTH + 24) ||
                sy < -24 || sy >= static_cast<i16>(SCREEN_HEIGHT + 40)) {
                continue;
            }
            const u16 anim_id = FomtRomAssets::select_pete_anim(
                f.facing, f.anim_frame != 0u, f.equipped_tool, f.tool_anim_timer);
            rom_assets_.write_pete_oam(
                bus_, oam_slot, vram_tile_cursor, static_cast<u8>(i), sx, sy, anim_id, frame_count_, 1u);
        }

        // Equipped tool icon in top-right HUD corner
        rom_assets_.write_item_icon_oam(
            bus_, oam_slot, vram_tile_cursor, tool_icon_id(me.equipped_tool), 229, 8, 4u, 0u);
        return;
    }

    if (!uploaded_mode_initialized_ || uploaded_mode_ != ScreenMode::Gameplay) {
        upload_gba_assets(bus_);
        uploaded_mode_initialized_ = true;
        uploaded_mode_ = ScreenMode::Gameplay;
    }

    bus_.io_write16(reg::DISPCNT, 0x1340u); // Mode 0, 1D OBJ, BG0 + BG1 + OBJ
    bus_.io_write16(reg::BG0CNT,  static_cast<u16>((28u << 8) | 2u)); // Farm priority 2
    bus_.io_write16(reg::BG1CNT,  static_cast<u16>((30u << 8) | 0u)); // HUD priority 0
    bus_.io_write16(reg::BLDCNT,  0u);

    const i32 max_cam_x = static_cast<i32>(FARM_FIELD_WIDTH * 16 - SCREEN_WIDTH);
    const i32 max_cam_y = static_cast<i32>(FARM_FIELD_HEIGHT * 16 - SCREEN_HEIGHT);
    const i32 cam_x = std::clamp<i32>(me.x - static_cast<i32>(SCREEN_WIDTH / 2 - 8), 0, max_cam_x);
    const i32 cam_y = std::clamp<i32>(me.y - static_cast<i32>(SCREEN_HEIGHT / 2 - 8), 0, max_cam_y);

    const u32 cam_tile8_x = static_cast<u32>(cam_x >> 3);
    const u32 cam_tile8_y = static_cast<u32>(cam_y >> 3);

    bus_.io_write16(reg::BG0HOFS, static_cast<u16>(cam_x & 7));
    bus_.io_write16(reg::BG0VOFS, static_cast<u16>(cam_y & 7));
    bus_.io_write16(reg::BG1HOFS, 0u);
    bus_.io_write16(reg::BG1VOFS, 0u);

    // Stream visible 31x21 8x8 tile window from FarmWorld into BG0 ScreenBlock 28
    for (u32 sy = 0; sy < 21u; ++sy) {
        const u32 map_ty = cam_tile8_y + sy;
        const u32 fy = map_ty >> 1;
        const u32 sub_y = map_ty & 1u;
        for (u32 sx = 0; sx < 31u; ++sx) {
            const u32 map_tx = cam_tile8_x + sx;
            const u32 fx = map_tx >> 1;
            const u32 sub_x = map_tx & 1u;

            u16 tile = bgtile::META_GRASS;
            if (fx < FARM_FIELD_WIDTH && fy < FARM_FIELD_HEIGHT) {
                if (fx == 0 || fy == 0 || fx == FARM_FIELD_WIDTH - 1 || fy == FARM_FIELD_HEIGHT - 1) {
                    tile = bgtile::FENCE_TILE;
                } else {
                    const u16 meta = select_farm_metatile(world_.tile_at(fx, fy));
                    tile = static_cast<u16>(meta + sub_y * 2u + sub_x);
                }
            }
            write_bg_tile(bus_, 28u, 32u, sx, sy, tile, 0u);
        }
    }

    // HUD & Dialog Box on BG1 (ScreenBlock 30)
    draw_gameplay_hud(
        bus_, 30u, 2u, world_, me, net_.status_label(),
        dialog_open_, dialog_line1_, dialog_line2_, toast_msg_, toast_timer_, false);

    // OAM Sprites: Farmers on same map + Target Cursor
    hide_all_oam(bus_);
    u32 oam_slot = 0;

    // Target cursor in front of local farmer
    i32 ttx = 0, tty = 0;
    target_tile_in_front(me, ttx, tty);
    if (world_.in_bounds(ttx, tty) && (frame_count_ % 30u) < 22u) {
        const i16 cx = static_cast<i16>(ttx * 16 - cam_x);
        const i16 cy = static_cast<i16>(tty * 16 - cam_y);
        if (cx >= -16 && cx < static_cast<i16>(SCREEN_WIDTH) && cy >= -16 && cy < static_cast<i16>(SCREEN_HEIGHT)) {
            write_oam_sprite(bus_, oam_slot++, cx, cy, objtile::CURSOR_16X16, 0, 2);
        }
    }

    // Render Local + Remote Farmers in same map_id
    for (std::size_t i = 0; i < MAX_COOP_PLAYERS; ++i) {
        const auto& f = world_.farmers[i];
        if (!f.connected || f.map_id != me.map_id) continue;

        const i16 sx = static_cast<i16>(f.x - cam_x);
        const i16 sy = static_cast<i16>(f.y - cam_y);
        if (sx < -16 || sx >= static_cast<i16>(SCREEN_WIDTH) || sy < -16 || sy >= static_cast<i16>(SCREEN_HEIGHT)) {
            continue;
        }

        u16 spr_tile = objtile::FARMER_DOWN_0;
        bool h_flip = false;
        if (f.tool_anim_timer > 0) {
            spr_tile = objtile::FARMER_SWING;
            h_flip = (f.facing == Facing::Right);
        } else {
            switch (f.facing) {
                case Facing::Down:  spr_tile = f.anim_frame ? objtile::FARMER_DOWN_1 : objtile::FARMER_DOWN_0; break;
                case Facing::Up:    spr_tile = objtile::FARMER_UP_0; break;
                case Facing::Left:  spr_tile = objtile::FARMER_LEFT_0; h_flip = false; break;
                case Facing::Right: spr_tile = objtile::FARMER_LEFT_0; h_flip = true; break;
            }
        }
        write_oam_sprite(bus_, oam_slot++, sx, sy, spr_tile, static_cast<u8>(i & 3u), 1, h_flip);
    }
}

bool GameEngine::tick_native_rom_frame(const HostActions& act) {
    if (act.quit) return false;
    if (act.set_scale > 0) video_.set_integer_scale(act.set_scale);
    if (act.toggle_fullscreen) video_.toggle_fullscreen();
    if (act.toggle_mute) audio_.set_muted(!audio_.is_muted());
    if (act.quick_save) save_game();
    if (act.quick_load) load_game();
    if (act.start_host && net_.start_host(cfg_.host_port)) {
        coop_bridge_.bind(net_);
    }
    if (act.join_client && net_.connect_client("127.0.0.1:7777")) {
        coop_bridge_.bind(net_);
    }

    coop_bridge_.update_before_frame(bus_, net_);
    net_.tick(world_);

    FrameCallbacks cb{};
    cb.on_cycles = [this](u32 cycles) {
        u32 spent = 0;
        while (spent < cycles) {
            if (cpu_.halted() && !cpu_.service_irq_if_pending(bus_)) {
                const u32 rem = cycles - spent;
                bus_.step_timers(rem);
                cpu_.service_irq_if_pending(bus_);
                break;
            }
            const u32 cy = cpu_.step_one(bus_);
            bus_.step_timers(cy);
            spent += cy;
        }
    };
    cb.on_scanline = [this](u16 y) {
        if (y == 0u) {
            ppu_.latch_affine_refs(bus_);
            if (net_.is_connected()) {
                coop_bridge_.inject_remote_oam_before_render(bus_, rom_assets_, net_.local_player_id());
            }
        }
        ppu_.render_scanline(bus_, y, video_.framebuffer() + static_cast<std::size_t>(y) * SCREEN_WIDTH);
    };
    cb.on_vblank = [this]() {
        coop_bridge_.restore_oam_after_render(bus_);
    };

    HalTiming::run_gba_frame(bus_, cb);

    if (bus_.save_dirty() && (frame_count_ % 300u) == 0u) {
        save_.flush_from_bus(bus_, false);
    }

    video_.present();
    ++frame_count_;
    return true;
}

bool GameEngine::tick_frame(bool poll_host_events) {
    HostActions act{};
    if (poll_host_events) {
        act = input_.poll_events(&bus_);
    } else {
        input_.poll_events(&bus_);
    }

    if (is_native_rom_mode()) {
        return tick_native_rom_frame(act);
    }

    if (poll_host_events) {
        if (act.quit) return false;
        if (act.set_scale > 0) video_.set_integer_scale(act.set_scale);
        if (act.toggle_fullscreen) video_.toggle_fullscreen();
        if (act.toggle_mute) audio_.set_muted(!audio_.is_muted());
        if (act.quick_save) {
            save_game();
            toast_msg_ = "Quick saved to .sav!";
            toast_timer_ = 90;
        }
        if (act.quick_load) {
            if (load_game()) {
                toast_msg_ = "Loaded saves/fomt.sav!";
                toast_timer_ = 90;
            }
        }
        if (act.start_host) {
            if (net_.start_host(cfg_.host_port)) {
                mode_ = ScreenMode::Gameplay;
                toast_msg_ = "UDP Host started on :7777";
                toast_timer_ = 120;
            }
        }
        if (act.join_client) {
            if (net_.connect_client("127.0.0.1:7777")) {
                mode_ = ScreenMode::Gameplay;
                toast_msg_ = "Connecting 127.0.0.1:7777";
                toast_timer_ = 120;
            }
        }
    }

    if (mode_ == ScreenMode::Title) {
        step_title_screen();
    } else {
        step_gameplay();
    }

    if (mode_ == ScreenMode::Title) {
        sync_title_ppu();
    } else {
        sync_gameplay_ppu();
    }

    ppu_.latch_affine_refs(bus_);
    HalTiming::run_gba_frame(bus_, FrameCallbacks{
        [this](u16 y) {
            ppu_.render_scanline(bus_, y, video_.framebuffer() + static_cast<std::size_t>(y) * SCREEN_WIDTH);
        },
        nullptr
    });

    video_.present();
    ++frame_count_;
    return true;
}

int GameEngine::run() {
    if (audio_.has_active_device()) {
        // Pre-fill ~33 ms of silent cushion so SDL's first callback never underruns
        for (int i = 0; i < 2; ++i) {
            audio_.step_cycles(hal::TOTAL_SCANLINES * hal::CPU_CYCLES_PER_LINE);
        }
        audio_.flush_frame();
    }

    bool running = true;
    while (running) {
        if (audio_.has_active_device()) {
            const std::size_t buf = audio_.buffered_frames();
            const std::size_t max_buf = (static_cast<std::size_t>(audio_.sample_rate()) * 85u) / 1000u;
            if (buf > max_buf) {
                timing_.sleep_until_next_tick();
                continue;
            }
        }

        const u32 ticks = timing_.consume_ticks();
        if (ticks == 0) {
            if (audio_.has_active_device() &&
                audio_.buffered_frames() < (static_cast<std::size_t>(audio_.sample_rate()) * 18u) / 1000u) {
                timing_.reset();
                running = tick_frame(true);
                continue;
            }
            timing_.sleep_until_next_tick();
            continue;
        }
        for (u32 t = 0; t < ticks && running; ++t) {
            running = tick_frame(true);
        }
    }
    return 0;
}

} // namespace hm::game
