#pragma once

#include "hal/gba_types.hpp"

#include <string>

namespace hm::hal {
class GbaBus;
}

namespace hm::game {

struct RomProbeInfo {
    bool found = false;
    bool valid_fomt_us = false;
    std::string title;
    std::string game_code;
    std::size_t size_bytes = 0;
};

// Probes baserom.gba (or custom path), loads into GbaBus ROM space if present
RomProbeInfo probe_and_load_rom(hal::GbaBus& bus, const std::string& path = "baserom.gba");

// Uploads 4bpp GBA palettes, 8x8 tiles (font + farm + title + UI), and 16x16 OAM sprites to VRAM/PAL
void upload_gba_assets(hal::GbaBus& bus);

// Helper to write a 4bpp 8x8 tile entry into a GBA 64x32 or 32x32 BG screenblock
void write_bg_tile(
    hal::GbaBus& bus,
    hal::u32 screen_block,
    hal::u32 map_w_tiles,
    hal::u32 tx,
    hal::u32 ty,
    hal::u16 tile_idx,
    hal::u8 pal_bank,
    bool h_flip = false,
    bool v_flip = false);

// Helper to draw ASCII text onto a BG screenblock using the uploaded font tiles (ASCII 32..127 -> tiles 0..95)
void draw_bg_text(
    hal::GbaBus& bus,
    hal::u32 screen_block,
    hal::u32 map_w_tiles,
    hal::u32 tx,
    hal::u32 ty,
    const char* text,
    hal::u8 pal_bank);

// Helper to write an OAM sprite attribute entry (0..127)
void write_oam_sprite(
    hal::GbaBus& bus,
    hal::u32 oam_idx,
    hal::i16 x,
    hal::i16 y,
    hal::u16 tile_idx,
    hal::u8 pal_bank,
    hal::u8 priority = 1,
    bool h_flip = false,
    bool semi_transparent = false,
    hal::u8 shape = 0, // 0=Square
    hal::u8 size = 1); // 1=16x16

// Tile index layout in CharBlock 0 (BG VRAM)
namespace bgtile {
inline constexpr hal::u16 FONT_BASE       = 0;   // 0..95 (ASCII 32..127)
inline constexpr hal::u16 SOLID_FILL      = 96;  // Color index 1 solid
inline constexpr hal::u16 UI_FRAME_BASE   = 97;  // 97..105 (9-slice box)
inline constexpr hal::u16 SKY_GRAD_BASE   = 106; // 106..109
inline constexpr hal::u16 HILL_TILE       = 110;
inline constexpr hal::u16 FENCE_TILE      = 111;

// 2x2 meta-tiles for 16x16 farm plots (starting at 120, 4 tiles per plot type)
inline constexpr hal::u16 META_GRASS      = 120;
inline constexpr hal::u16 META_TILLED     = 124;
inline constexpr hal::u16 META_WATERED    = 128;
inline constexpr hal::u16 META_WEED       = 132;
inline constexpr hal::u16 META_BRANCH     = 136;
inline constexpr hal::u16 META_STONE      = 140;
inline constexpr hal::u16 META_STUMP      = 144;
inline constexpr hal::u16 META_BOULDER    = 148;
inline constexpr hal::u16 META_SEED       = 152;
inline constexpr hal::u16 META_SPROUT     = 156;
inline constexpr hal::u16 META_GROWING    = 160;
inline constexpr hal::u16 META_MATURE_W   = 164; // White/purple turnip
inline constexpr hal::u16 META_MATURE_R   = 168; // Red tomato / carrot
inline constexpr hal::u16 META_MATURE_Y   = 172; // Yellow corn / potato
} // namespace bgtile

// Sprite tile indices in OBJ VRAM (0x06010000, 1D mapping, 4 tiles per 16x16 sprite)
namespace objtile {
inline constexpr hal::u16 FARMER_DOWN_0   = 0;
inline constexpr hal::u16 FARMER_DOWN_1   = 4;
inline constexpr hal::u16 FARMER_UP_0     = 8;
inline constexpr hal::u16 FARMER_LEFT_0   = 12;
inline constexpr hal::u16 FARMER_SWING    = 16;
inline constexpr hal::u16 CURSOR_16X16    = 20;
inline constexpr hal::u16 TURNIP_16X16    = 24;
inline constexpr hal::u16 CLOUD_16X16     = 28;
} // namespace objtile

} // namespace hm::game
