#pragma once

#include "game/fomt_rom_assets.hpp"
#include "hal/gba_bus.hpp"
#include "net/net_coop.hpp"

#include <array>
#include <vector>

namespace hm::game {

// Bridges the live emulated GBA ROM (EWRAM / IWRAM / OAM) to NetCoopSession
// when running Option 1B (ARM7TDMI + GBA BIOS HLE):
//  - Extracts local GameScene / GameState / PlayerEntity / Clock+Weather / World regions
//  - Synchronizes Host Clock/Weather/Subframe across Host & Clients
//  - Collaboratively syncs FarmHouse upgrades, Gotz builds, Lumber/Horse, Coop, Barn, Field<43,25>, Dog
//    while keeping per-player Money (Gold/Ledger), Farmer (inventory/stamina/tools), ShippingBin, and ToolChest separate
//  - Injects remote P2..P4 farmers into free OAM/OBJ VRAM right before scanline 0 with live ambient tint + drop shadow
//    and restores the exact OAM/VRAM/PALRAM slices on VBlank so the ROM sees zero side effects.
class EwramCoopBridge {
public:
    static constexpr hal::u32 PTR_GAME_SCENE          = 0x0300040Cu;
    static constexpr hal::u32 OFF_SCENE_MAP_RENDERER  = 0x0004u;
    static constexpr hal::u32 OFF_SCENE_GAMESTATE     = 0x358u;
    static constexpr hal::u32 OFF_SCENE_CLOCK_PTR     = 0x1040u;
    static constexpr hal::u32 OFF_SCENE_SUBFRAME      = 0x1044u;
    static constexpr hal::u32 OFF_SCENE_MAPCTRL       = 0x3B10u;

    static constexpr hal::u32 OFF_GS_WEATHER_TODAY    = 0x08u;
    static constexpr hal::u32 OFF_GS_WEATHER_TOMORROW = 0x0Cu;
    static constexpr hal::u32 OFF_GS_DATE_U16         = 0x10u;
    static constexpr hal::u32 OFF_GS_TIME_U16         = 0x12u;
    static constexpr hal::u32 OFF_GS_FARM_HDR         = 0x0014u; // lumber, has_horse, Horse (excludes ShippingBin @ 0x0054)
    static constexpr hal::u32 OFF_GS_FARMHOUSE        = 0x01F4u; // upgrades, styles, utensils, fridge, shelf (excl ToolChest @ 0x0380)
    static constexpr hal::u32 OFF_GS_COOP             = 0x0410u;
    static constexpr hal::u32 OFF_GS_BARN             = 0x05F0u;
    static constexpr hal::u32 OFF_GS_FIELD            = 0x09DCu;
    static constexpr hal::u32 OFF_GS_GOLD             = 0x1AA8u; // per-player Gold + Ledger (never synced)
    static constexpr hal::u32 OFF_GS_FARMER           = 0x1BD8u; // per-player Farmer stats/inventory/tools (never synced)
    static constexpr hal::u32 OFF_GS_DOG              = 0x1C70u;
    static constexpr hal::u32 OFF_GS_GOTZ_BUILD       = 0x1F78u; // Gotz + 0x14..+0x15 construction state
    static constexpr hal::u32 OFF_GS_WILD_FORAGE      = 0x34C8u; // daily Mountain/Forest/Beach wild forageable bitflags
    static constexpr hal::u32 PLAYER_VTABLE           = 0x080E6658u;
    static constexpr std::size_t FIELD_PLOT_COUNT     = FARM_FIELD_WIDTH * FARM_FIELD_HEIGHT;

    struct WorldRegionSpec {
        hal::u16 gs_offset;
        hal::u16 size;
        hal::u16 shadow_offset;
    };
    static constexpr std::array<WorldRegionSpec, 7> WORLD_REGIONS{{
        {OFF_GS_FARM_HDR,  0x0040u,    0u},
        {OFF_GS_FARMHOUSE, 0x018Cu,   64u},
        {OFF_GS_COOP,      0x01E0u,  460u},
        {OFF_GS_BARN,      0x03ECu,  940u},
        {OFF_GS_DOG,       0x0030u, 1944u},
        {OFF_GS_GOTZ_BUILD,0x0002u, 1992u},
        {OFF_GS_WILD_FORAGE,0x0010u,1994u},
    }};
    static constexpr std::size_t WORLD_SYNC_BYTES = 2010u;

    struct RemoteFarmer {
        bool     active = false;
        hal::u16 map_id = 0xFFFFu;
        hal::i16 x = 0;
        hal::i16 y = 0;
        hal::u8  facing = 0;
        hal::u16 anim_id = 0xFFFFu;
        hal::u32 held_raw = 0xFFFF0005u;
        hal::u32 ticks_since_seen = 0;
    };

    struct LocalSnapshot {
        hal::u32 scene_ptr = 0;
        hal::u32 gs_ptr = 0;
        hal::u32 player_ptr = 0;
        bool     valid_gs = false;
        bool     valid_player = false;
        hal::u16 map_id = 0xFFFFu;
        hal::i16 x = 0;
        hal::i16 y = 0;
        hal::u8  facing = 0;
        hal::u16 anim_id = 0xFFFFu;
        hal::u32 held_raw = 0xFFFF0005u;
        hal::u16 date_u16 = 0;
        hal::u16 time_u16 = 0;
        hal::u8  sub_frame = 0;
        hal::u8  weather_today = 0;
        hal::u8  weather_tomorrow = 0;
        bool     valid_subframe = false;
    };

    void reset();
    void bind(net::NetCoopSession& net);

    // Call once per frame right before net.tick() + CPU execution
    void update_before_frame(hal::GbaBus& bus, net::NetCoopSession& net);

    // Call at scanline 0 (after ROM VBlank setup) and at VBlank (y == 160)
    void inject_remote_oam_before_render(hal::GbaBus& bus, const FomtRomAssets& assets, hal::u8 local_player_id);
    void restore_oam_after_render(hal::GbaBus& bus);

    const LocalSnapshot& local_snapshot() const { return local_; }
    const std::array<RemoteFarmer, MAX_COOP_PLAYERS>& remote_farmers() const { return remotes_; }

private:
    LocalSnapshot resolve_live_pointers(const hal::GbaBus& bus);
    void send_field_chunks_to_peer(net::NetCoopSession& net, hal::u8 peer_id) const;
    void send_world_chunks_to_peer(net::NetCoopSession& net, hal::u8 peer_id) const;
    void broadcast_field_chunks(net::NetCoopSession& net) const;
    void on_app_packet(net::NetCoopSession& net, hal::u8 sender_id, const hal::u8* payload, std::size_t len);

    hal::u32 cached_player_ptr_ = 0;
    hal::u32 last_gs_ptr_ = 0;
    hal::u32 tick_counter_ = 0;
    LocalSnapshot local_{};

    bool     has_prev_cam_ = false;
    hal::u16 prev_cam_map_ = 0xFFFFu;
    hal::i32 prev_cam_x_ = 0;
    hal::i32 prev_cam_y_ = 0;

    std::array<RemoteFarmer, MAX_COOP_PLAYERS> remotes_{};
    hal::u16 pending_host_date_ = 0;
    hal::u16 pending_host_time_ = 0;
    hal::u8  pending_host_subframe_ = 0;
    hal::u8  pending_host_weather_today_ = 0;
    hal::u8  pending_host_weather_tomorrow_ = 0;
    bool     has_host_subframe_ = false;
    bool     has_host_clock_ = false;
    bool     has_host_world_snapshot_ = false;
    bool     pending_initial_world_apply_ = false;

    std::array<hal::u32, FIELD_PLOT_COUNT> shadow_plots_{};
    bool shadow_initialized_ = false;

    std::array<hal::u32, FIELD_PLOT_COUNT> pending_client_plots_{};
    std::array<bool, FIELD_PLOT_COUNT>     pending_client_dirty_{};
    std::array<hal::u32, FIELD_PLOT_COUNT> local_plot_prev_{};
    std::array<hal::u8, FIELD_PLOT_COUNT>  local_plot_hold_{};
    bool has_pending_client_plots_ = false;

    std::array<hal::u8, WORLD_SYNC_BYTES> shadow_world_{};
    bool world_shadow_initialized_ = false;
    std::array<hal::u8, WORLD_SYNC_BYTES> pending_world_{};
    std::array<bool, WORLD_SYNC_BYTES>    pending_world_dirty_{};
    std::array<bool, WORLD_SYNC_BYTES>    pending_world_monotonic_{};
    std::array<hal::u8, WORLD_SYNC_BYTES> local_world_hold_{};
    bool has_pending_world_ = false;

    bool pending_peer_sync_[MAX_COOP_PLAYERS]{};

    // OAM/VRAM/PALRAM scratch backups restored on VBlank after scanline 159
    bool injected_this_frame_ = false;
    hal::u32 backup_vram_start_ = 0x16800u;
    std::array<hal::u8, hal::OAM_SIZE> oam_backup_{};
    std::array<hal::u8, 512> obj_pal_backup_{};
    std::vector<hal::u8> obj_vram_backup_{};
};

} // namespace hm::game
