#include "fomt_domain.hpp"

#include <algorithm>
#include <cstring>

namespace hm::game {

using namespace hm::hal;

namespace {

struct ToolMeta {
    const char* name;
    u16 icon;
};

constexpr ToolMeta TOOL_TABLE[] = {
#define o(tag, icon, name, desc) { name, icon },
#include "data/item/tool.def"
#undef o
};

} // namespace

const char* tool_display_name(u8 tool_id) {
    if (tool_id < NUM_TOOLS) {
        return TOOL_TABLE[tool_id].name;
    }
    return "None";
}

u16 tool_icon_id(u8 tool_id) {
    if (tool_id < NUM_TOOLS) {
        return TOOL_TABLE[tool_id].icon;
    }
    return 0;
}

CropSpec get_crop_spec(CropKind kind) {
    switch (kind) {
        case CropKind::Turnip:   return {"Turnip",   Season::Spring, 4, 0,  60};
        case CropKind::Potato:   return {"Potato",   Season::Spring, 7, 0,  80};
        case CropKind::Cucumber: return {"Cucumber", Season::Spring, 9, 5,  60};
        case CropKind::Tomato:   return {"Tomato",   Season::Summer, 9, 3,  60};
        case CropKind::Corn:     return {"Corn",     Season::Summer, 14, 3, 100};
        case CropKind::Eggplant: return {"Eggplant", Season::Fall,   9, 3,  80};
        case CropKind::Carrot:   return {"Carrot",   Season::Fall,   7, 0, 120};
        case CropKind::Grass:    return {"Grass",    Season::Spring, 11, 1, 0};
        default:                 return {"None",     Season::Spring, 0, 0,  0};
    }
}

CropKind seed_tool_to_crop(u8 tool_id) {
    switch (tool_id) {
        case TOOL_TURNIP_SEEDS:   return CropKind::Turnip;
        case TOOL_POTATO_SEEDS:   return CropKind::Potato;
        case TOOL_CUCUMBER_SEEDS: return CropKind::Cucumber;
        case TOOL_TOMATO_SEEDS:   return CropKind::Tomato;
        case TOOL_CORN_SEEDS:     return CropKind::Corn;
        case TOOL_EGGPLANT_SEEDS: return CropKind::Eggplant;
        case TOOL_CARROT_SEEDS:   return CropKind::Carrot;
        case TOOL_GRASS_SEEDS:    return CropKind::Grass;
        default:                  return CropKind::None;
    }
}

u8 FarmTile::growth_stage() const {
    if (crop == CropKind::None) return 0;
    const auto spec = get_crop_spec(crop);
    if (spec.days_to_mature == 0) return 0;
    if (growth_days >= spec.days_to_mature) return 3;
    if (growth_days >= (spec.days_to_mature * 2u) / 3u) return 2;
    if (growth_days >= 1u) return 1;
    return 0;
}

bool FarmTile::is_mature() const {
    if (crop == CropKind::None) return false;
    const auto spec = get_crop_spec(crop);
    return spec.days_to_mature > 0 && growth_days >= spec.days_to_mature;
}

bool FarmTile::blocks_movement() const {
    return obstacle == ObstacleKind::Stone ||
           obstacle == ObstacleKind::Branch ||
           obstacle == ObstacleKind::Stump ||
           obstacle == ObstacleKind::Boulder;
}

const char* WorldClock::season_name() const {
    switch (season) {
        case Season::Spring: return "SPR";
        case Season::Summer: return "SUM";
        case Season::Fall:   return "FAL";
        case Season::Winter: return "WIN";
    }
    return "SPR";
}

const char* WorldClock::weather_name() const {
    switch (weather) {
        case Weather::Sunny:   return "SUN";
        case Weather::Rainy:   return "RAIN";
        case Weather::Typhoon: return "TYPH";
        case Weather::Snowy:   return "SNOW";
    }
    return "SUN";
}

void WorldClock::step_tick(u32 ticks_per_game_minute) {
    if (++tick_accum >= ticks_per_game_minute) {
        tick_accum = 0;
        minute += 10;
        if (minute >= 60) {
            minute = 0;
            hour++;
            if (hour >= 24) {
                hour = 6;
            }
        }
    }
}

FarmWorld::FarmWorld() {
    init_new_farm();
}

u32 FarmWorld::next_rand() {
    rng_state_ ^= rng_state_ << 13;
    rng_state_ ^= rng_state_ >> 17;
    rng_state_ ^= rng_state_ << 5;
    return rng_state_;
}

void FarmWorld::init_new_farm(u32 seed) {
    rng_state_ = seed ? seed : 0x464F4D54u;
    clock = WorldClock{};
    tiles_.fill(FarmTile{});

    for (std::size_t i = 0; i < MAX_COOP_PLAYERS; ++i) {
        farmers[i] = FarmerState{};
        farmers[i].id = static_cast<u8>(i);
        farmers[i].connected = (i == 0);
        farmers[i].x = static_cast<i16>((10 + static_cast<int>(i) * 2) * 16);
        farmers[i].y = static_cast<i16>(8 * 16);
    }

    // Leave clear starter patch near farmhouse (cols 6..18, rows 4..14)
    for (u32 y = 0; y < FARM_FIELD_HEIGHT; ++y) {
        for (u32 x = 0; x < FARM_FIELD_WIDTH; ++x) {
            const bool starter_zone = (x >= 6 && x <= 18 && y >= 4 && y <= 14);
            if (starter_zone) continue;

            const u32 r = next_rand() % 100u;
            auto& t = tile_at(x, y);
            if (r < 12) t.obstacle = ObstacleKind::Weed;
            else if (r < 17) t.obstacle = ObstacleKind::Branch;
            else if (r < 22) t.obstacle = ObstacleKind::Stone;
            else if (r == 22) t.obstacle = ObstacleKind::Stump;
            else if (r == 23) t.obstacle = ObstacleKind::Boulder;
        }
    }

    // Starter pre-tilled & planted turnip patch so players see crops immediately
    for (u32 x = 10; x <= 12; ++x) {
        auto& t = tile_at(x, 10);
        t.soil = SoilState::Watered;
        t.crop = CropKind::Turnip;
        t.growth_days = (x == 12) ? 4 : 2;
    }
}

bool FarmWorld::in_bounds(i32 x, i32 y) const {
    return x >= 0 && y >= 0 &&
           x < static_cast<i32>(FARM_FIELD_WIDTH) &&
           y < static_cast<i32>(FARM_FIELD_HEIGHT);
}

FarmTile& FarmWorld::tile_at(u32 x, u32 y) {
    return tiles_[y * FARM_FIELD_WIDTH + x];
}

const FarmTile& FarmWorld::tile_at(u32 x, u32 y) const {
    return tiles_[y * FARM_FIELD_WIDTH + x];
}

ToolActionResult FarmWorld::apply_tool(FarmerState& farmer, i32 tile_x, i32 tile_y, u8 tool_id) {
    ToolActionResult res{};
    if (!in_bounds(tile_x, tile_y)) {
        res.message = "Out of bounds";
        return res;
    }

    FarmTile& t = tile_at(static_cast<u32>(tile_x), static_cast<u32>(tile_y));

    // Mature crop harvesting has priority regardless of held tool
    if (t.is_mature()) {
        const auto spec = get_crop_spec(t.crop);
        res.harvested = true;
        res.modified_tile = true;
        res.gold_earned = spec.sell_price;
        farmer.gold += spec.sell_price;
        if (spec.regrow_days > 0) {
            t.growth_days = static_cast<u8>(spec.days_to_mature - spec.regrow_days);
        } else {
            t.crop = CropKind::None;
            t.growth_days = 0;
        }
        res.message = "Harvested crop!";
        return res;
    }

    // Sickle: cuts weeds & clears planted crops
    if (tool_id <= TOOL_MYTHIC_SICKLE) {
        if (t.obstacle == ObstacleKind::Weed) {
            t.obstacle = ObstacleKind::None;
            res.modified_tile = true;
            res.stamina_used = 1;
            res.message = "Cleared weed";
        } else if (t.crop != CropKind::None) {
            t.crop = CropKind::None;
            t.growth_days = 0;
            res.modified_tile = true;
            res.stamina_used = 1;
            res.message = "Cut crop";
        }
    }
    // Hoe: tills clear untilled soil
    else if (tool_id >= TOOL_IRON_HOE && tool_id <= TOOL_MYTHIC_HOE) {
        if (t.obstacle == ObstacleKind::None && t.crop == CropKind::None && t.soil == SoilState::Untilled) {
            t.soil = (clock.weather == Weather::Rainy) ? SoilState::Watered : SoilState::Tilled;
            res.modified_tile = true;
            res.stamina_used = 2;
            res.message = "Tilled soil";
        }
    }
    // Axe: chops branches (and stumps on upgraded axe)
    else if (tool_id >= TOOL_IRON_AXE && tool_id <= TOOL_MYTHIC_AXE) {
        if (t.obstacle == ObstacleKind::Branch ||
            (t.obstacle == ObstacleKind::Stump && tool_id >= TOOL_COPPER_AXE)) {
            t.obstacle = ObstacleKind::None;
            res.modified_tile = true;
            res.stamina_used = 2;
            res.message = "Chopped wood";
        }
    }
    // Hammer: smashes stones (and boulders on upgraded hammer), or flattens tilled soil
    else if (tool_id >= TOOL_IRON_HAMMER && tool_id <= TOOL_MYTHIC_HAMMER) {
        if (t.obstacle == ObstacleKind::Stone ||
            (t.obstacle == ObstacleKind::Boulder && tool_id >= TOOL_SILVER_HAMMER)) {
            t.obstacle = ObstacleKind::None;
            res.modified_tile = true;
            res.stamina_used = 2;
            res.message = "Smashed stone";
        } else if (t.obstacle == ObstacleKind::None && t.crop == CropKind::None && t.soil != SoilState::Untilled) {
            t.soil = SoilState::Untilled;
            res.modified_tile = true;
            res.stamina_used = 1;
            res.message = "Flattened soil";
        }
    }
    // Watering Can: waters tilled soil
    else if (tool_id >= TOOL_WATERING_CAN && tool_id <= TOOL_MYTHIC_WATERING_CAN) {
        if (t.soil == SoilState::Tilled) {
            t.soil = SoilState::Watered;
            res.modified_tile = true;
            res.stamina_used = 2;
            res.message = "Watered plot";
        }
    }
    // Seeds: plant in tilled/watered empty soil matching season
    else {
        const CropKind crop = seed_tool_to_crop(tool_id);
        if (crop != CropKind::None && t.obstacle == ObstacleKind::None &&
            t.crop == CropKind::None && t.soil != SoilState::Untilled) {
            const auto spec = get_crop_spec(crop);
            if (clock.season == Season::Winter || (crop != CropKind::Grass && spec.primary_season != clock.season)) {
                res.message = "Wrong season!";
                return res;
            }
            t.crop = crop;
            t.growth_days = 0;
            res.modified_tile = true;
            res.stamina_used = 1;
            res.message = "Planted seeds";
        }
    }

    if (res.stamina_used > 0) {
        farmer.stamina = static_cast<u8>(farmer.stamina > res.stamina_used ? farmer.stamina - res.stamina_used : 0);
    }
    return res;
}

void FarmWorld::advance_day() {
    clock.day++;
    if (clock.day > 30) {
        clock.day = 1;
        const u8 s = (static_cast<u8>(clock.season) + 1u) % 4u;
        clock.season = static_cast<Season>(s);
        if (clock.season == Season::Spring) {
            clock.year++;
        }
    }
    clock.hour = 6;
    clock.minute = 0;

    // Simple weather forecast
    const u32 w = next_rand() % 10u;
    if (clock.season == Season::Winter) {
        clock.weather = (w < 3) ? Weather::Snowy : Weather::Sunny;
    } else {
        clock.weather = (w < 2) ? Weather::Rainy : Weather::Sunny;
    }

    for (auto& f : farmers) {
        if (f.connected) f.stamina = 100;
    }

    const bool rain_auto_water = (clock.weather == Weather::Rainy || clock.weather == Weather::Typhoon);

    for (auto& t : tiles_) {
        if (t.crop != CropKind::None) {
            const auto spec = get_crop_spec(t.crop);
            if (clock.season == Season::Winter || (t.crop != CropKind::Grass && spec.primary_season != clock.season)) {
                // Out-of-season crops wilt
                t.crop = CropKind::None;
                t.growth_days = 0;
            } else if (t.soil == SoilState::Watered || t.crop == CropKind::Grass) {
                if (t.growth_days < spec.days_to_mature) {
                    t.growth_days++;
                }
            }
        }
        if (t.soil == SoilState::Watered) {
            t.soil = rain_auto_water ? SoilState::Watered : SoilState::Tilled;
        } else if (t.soil == SoilState::Tilled && rain_auto_water) {
            t.soil = SoilState::Watered;
        }
    }
}

std::vector<u8> FarmWorld::serialize_state() const {
    // Magic "FMSV" + version 1 + clock + farmers[4] + tiles[43*25]
    constexpr u32 MAGIC = 0x56534D46u;
    constexpr u32 VERSION = 1u;
    std::vector<u8> out;
    out.reserve(16 + sizeof(clock) + sizeof(farmers) + sizeof(tiles_));

    auto push_bytes = [&](const void* ptr, std::size_t sz) {
        const auto* p = static_cast<const u8*>(ptr);
        out.insert(out.end(), p, p + sz);
    };

    push_bytes(&MAGIC, sizeof(MAGIC));
    push_bytes(&VERSION, sizeof(VERSION));
    push_bytes(&clock, sizeof(clock));
    push_bytes(farmers.data(), sizeof(farmers));
    push_bytes(tiles_.data(), sizeof(tiles_));
    return out;
}

bool FarmWorld::deserialize_state(const u8* data, std::size_t size) {
    constexpr u32 MAGIC = 0x56534D46u;
    constexpr u32 VERSION = 1u;
    const std::size_t expected = 8 + sizeof(clock) + sizeof(farmers) + sizeof(tiles_);
    if (!data || size < expected) return false;

    u32 magic = 0, version = 0;
    std::memcpy(&magic, data, 4);
    std::memcpy(&version, data + 4, 4);
    if (magic != MAGIC || version != VERSION) return false;

    std::size_t off = 8;
    std::memcpy(&clock, data + off, sizeof(clock)); off += sizeof(clock);
    std::memcpy(farmers.data(), data + off, sizeof(farmers)); off += sizeof(farmers);
    std::memcpy(tiles_.data(), data + off, sizeof(tiles_));
    return true;
}

} // namespace hm::game
