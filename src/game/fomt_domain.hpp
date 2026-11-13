#pragma once

#include "hal/gba_types.hpp"

#include <array>
#include <cstddef>
#include <string_view>
#include <vector>

namespace hm::game {

// Tool IDs generated directly from StanHash/fomt data/item/tool.def
enum ToolId : hal::u8 {
#define o(tag, icon, name, desc) TOOL_##tag,
#include "data/item/tool.def"
#undef o
    TOOL_NONE,
    NUM_TOOLS = TOOL_NONE
};

const char* tool_display_name(hal::u8 tool_id);
hal::u16 tool_icon_id(hal::u8 tool_id);

enum class Season : hal::u8 {
    Spring = 0,
    Summer = 1,
    Fall   = 2,
    Winter = 3,
};

enum class Weather : hal::u8 {
    Sunny   = 0,
    Rainy   = 1,
    Typhoon = 2,
    Snowy   = 3,
};

enum class SoilState : hal::u8 {
    Untilled = 0,
    Tilled   = 1,
    Watered  = 2,
};

enum class ObstacleKind : hal::u8 {
    None    = 0,
    Weed    = 1,
    Branch  = 2,
    Stone   = 3,
    Stump   = 4,
    Boulder = 5,
};

enum class CropKind : hal::u8 {
    None      = 0,
    Turnip    = 1, // 4 days (Spring)
    Potato    = 2, // 7 days (Spring)
    Cucumber  = 3, // 9 days, regrow 5 (Spring)
    Tomato    = 4, // 9 days, regrow 3 (Summer)
    Corn      = 5, // 14 days, regrow 3 (Summer)
    Eggplant  = 6, // 9 days, regrow 3 (Fall)
    Carrot    = 7, // 7 days (Fall)
    Grass     = 8, // 11 days, regrow
};

struct CropSpec {
    const char* name;
    Season primary_season;
    hal::u8 days_to_mature;
    hal::u8 regrow_days; // 0 = single harvest
    hal::u16 sell_price;
};

CropSpec get_crop_spec(CropKind kind);
CropKind seed_tool_to_crop(hal::u8 tool_id);

// Matches StanHash/fomt Farm::FIELD_WIDTH / FIELD_HEIGHT (include/farm.hh)
inline constexpr hal::u32 FARM_FIELD_WIDTH  = 43;
inline constexpr hal::u32 FARM_FIELD_HEIGHT = 25;
inline constexpr std::size_t FARM_TILE_COUNT = FARM_FIELD_WIDTH * FARM_FIELD_HEIGHT;
inline constexpr std::size_t MAX_COOP_PLAYERS = 4;

// Compact packed farm tile state (4 bytes per plot, matches FieldPlot semantics)
struct FarmTile {
    SoilState    soil     = SoilState::Untilled;
    ObstacleKind obstacle = ObstacleKind::None;
    CropKind     crop     = CropKind::None;
    hal::u8      growth_days = 0; // Accumulated watered days

    hal::u8 growth_stage() const; // 0=seed, 1=sprout, 2=growing, 3=mature
    bool is_mature() const;
    bool blocks_movement() const;
};
static_assert(sizeof(FarmTile) == 4, "FarmTile must stay 4 bytes for compact network sync");

enum class Facing : hal::u8 {
    Down  = 0,
    Up    = 1,
    Left  = 2,
    Right = 3,
};

struct FarmerState {
    hal::u8  id = 0;
    bool     connected = false;
    hal::u16 map_id = 0; // 0 = Farm, 1 = TownSquare, etc. (independent per player)
    hal::i16 x = 16 * 12;
    hal::i16 y = 16 * 10;
    Facing   facing = Facing::Down;
    hal::u8  anim_frame = 0;
    hal::u8  tool_anim_timer = 0;
    hal::u8  equipped_tool = TOOL_IRON_HOE;
    hal::u8  stamina = 100;
    hal::u32 gold = 500;
};

struct WorldClock {
    hal::u16 year = 1;
    Season   season = Season::Spring;
    hal::u8  day = 1; // 1..30
    hal::u8  hour = 6;
    hal::u8  minute = 0;
    Weather  weather = Weather::Sunny;
    hal::u32 tick_accum = 0;

    const char* season_name() const;
    const char* weather_name() const;
    void step_tick(hal::u32 ticks_per_game_minute = 30);
};

struct ToolActionResult {
    bool modified_tile = false;
    bool harvested = false;
    hal::u32 gold_earned = 0;
    hal::u8 stamina_used = 0;
    std::string_view message;
};

class FarmWorld {
public:
    FarmWorld();

    void init_new_farm(hal::u32 seed = 0x464F4D54u);

    FarmTile& tile_at(hal::u32 x, hal::u32 y);
    const FarmTile& tile_at(hal::u32 x, hal::u32 y) const;
    bool in_bounds(hal::i32 x, hal::i32 y) const;

    ToolActionResult apply_tool(FarmerState& farmer, hal::i32 tile_x, hal::i32 tile_y, hal::u8 tool_id);
    void advance_day();

    const std::array<FarmTile, FARM_TILE_COUNT>& tiles() const { return tiles_; }
    std::array<FarmTile, FARM_TILE_COUNT>& tiles() { return tiles_; }

    WorldClock clock{};
    std::array<FarmerState, MAX_COOP_PLAYERS> farmers{};

    // Binary serialization for .sav PC persistence
    std::vector<hal::u8> serialize_state() const;
    bool deserialize_state(const hal::u8* data, std::size_t size);

private:
    std::array<FarmTile, FARM_TILE_COUNT> tiles_{};
    hal::u32 rng_state_ = 0x12345678u;
    hal::u32 next_rand();
};

} // namespace hm::game
