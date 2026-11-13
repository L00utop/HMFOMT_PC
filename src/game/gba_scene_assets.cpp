#include "gba_scene_assets.hpp"
#include "hal/gba_bus.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <vector>

namespace hm::game {

using namespace hm::hal;

namespace {

void set_pal_color(GbaBus& bus, bool is_obj, u8 bank, u8 idx, u8 r, u8 g, u8 b) {
    const u32 base = (is_obj ? ADDR_PAL_BASE + 512u : ADDR_PAL_BASE) + (static_cast<u32>(bank) * 16u + idx) * 2u;
    bus.write16(base, rgb8_to_bgr555(r, g, b));
}

void write_tile_4bpp(GbaBus& bus, u32 vram_byte_base, u16 tile_idx, const std::array<u8, 64>& pixels) {
    const u32 addr = ADDR_VRAM_BASE + vram_byte_base + static_cast<u32>(tile_idx) * 32u;
    for (u32 y = 0; y < 8u; ++y) {
        for (u32 x = 0; x < 8u; x += 2u) {
            const u8 lo = pixels[y * 8u + x] & 0x0Fu;
            const u8 hi = pixels[y * 8u + x + 1u] & 0x0Fu;
            bus.write8(addr + y * 4u + (x >> 1), static_cast<u8>(lo | (hi << 4)));
        }
    }
}

void write_meta_16x16(
    GbaBus& bus,
    u16 base_tile,
    const std::array<u8, 256>& px16) {

    for (u32 qy = 0; qy < 2u; ++qy) {
        for (u32 qx = 0; qx < 2u; ++qx) {
            std::array<u8, 64> t{};
            for (u32 y = 0; y < 8u; ++y) {
                for (u32 x = 0; x < 8u; ++x) {
                    t[y * 8u + x] = px16[(qy * 8u + y) * 16u + (qx * 8u + x)];
                }
            }
            write_tile_4bpp(bus, 0u, static_cast<u16>(base_tile + qy * 2u + qx), t);
        }
    }
}

void write_obj_16x16(
    GbaBus& bus,
    u16 base_tile,
    const std::array<u8, 256>& px16) {

    for (u32 qy = 0; qy < 2u; ++qy) {
        for (u32 qx = 0; qx < 2u; ++qx) {
            std::array<u8, 64> t{};
            for (u32 y = 0; y < 8u; ++y) {
                for (u32 x = 0; x < 8u; ++x) {
                    t[y * 8u + x] = px16[(qy * 8u + y) * 16u + (qx * 8u + x)];
                }
            }
            write_tile_4bpp(bus, 0x10000u, static_cast<u16>(base_tile + qy * 2u + qx), t);
        }
    }
}

// Compact 5x7 font bitmaps (each byte = 5 bits row0..row6) for ASCII 32..95 (upper + symbols)
// Lowercase 'a'..'z' map cleanly to compact lowercase or uppercase fallback for readability at 240x160
struct Glyph5x7 {
    char ch;
    u8 rows[7];
};

constexpr Glyph5x7 FONT_GLYPHS[] = {
    {' ', {0x00,0x00,0x00,0x00,0x00,0x00,0x00}},
    {'!', {0x04,0x04,0x04,0x04,0x04,0x00,0x04}},
    {'"', {0x0A,0x0A,0x00,0x00,0x00,0x00,0x00}},
    {'#', {0x0A,0x1F,0x0A,0x0A,0x1F,0x0A,0x00}},
    {'$', {0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04}},
    {'%', {0x18,0x19,0x02,0x04,0x08,0x13,0x03}},
    {'&', {0x0C,0x12,0x14,0x08,0x15,0x12,0x0D}},
    {'\'',{0x04,0x04,0x08,0x00,0x00,0x00,0x00}},
    {'(', {0x02,0x04,0x08,0x08,0x08,0x04,0x02}},
    {')', {0x08,0x04,0x02,0x02,0x02,0x04,0x08}},
    {'*', {0x00,0x0A,0x04,0x1F,0x04,0x0A,0x00}},
    {'+', {0x00,0x04,0x04,0x1F,0x04,0x04,0x00}},
    {',', {0x00,0x00,0x00,0x00,0x04,0x04,0x08}},
    {'-', {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}},
    {'.', {0x00,0x00,0x00,0x00,0x00,0x06,0x06}},
    {'/', {0x01,0x02,0x02,0x04,0x08,0x08,0x10}},
    {'0', {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}},
    {'1', {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},
    {'2', {0x0E,0x11,0x01,0x06,0x08,0x10,0x1F}},
    {'3', {0x1F,0x02,0x04,0x06,0x01,0x11,0x0E}},
    {'4', {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}},
    {'5', {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}},
    {'6', {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}},
    {'7', {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
    {'8', {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}},
    {'9', {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}},
    {':', {0x00,0x06,0x06,0x00,0x06,0x06,0x00}},
    {';', {0x00,0x06,0x06,0x00,0x06,0x04,0x08}},
    {'<', {0x02,0x04,0x08,0x10,0x08,0x04,0x02}},
    {'=', {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00}},
    {'>', {0x08,0x04,0x02,0x01,0x02,0x04,0x08}},
    {'?', {0x0E,0x11,0x01,0x02,0x04,0x00,0x04}},
    {'@', {0x0E,0x11,0x17,0x15,0x17,0x10,0x0E}},
    {'A', {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}},
    {'B', {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}},
    {'C', {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}},
    {'D', {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C}},
    {'E', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}},
    {'F', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},
    {'G', {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}},
    {'H', {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}},
    {'I', {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}},
    {'J', {0x07,0x02,0x02,0x02,0x02,0x12,0x0C}},
    {'K', {0x11,0x12,0x14,0x18,0x14,0x12,0x11}},
    {'L', {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},
    {'M', {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}},
    {'N', {0x11,0x11,0x19,0x15,0x13,0x11,0x11}},
    {'O', {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}},
    {'P', {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
    {'Q', {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}},
    {'R', {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},
    {'S', {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}},
    {'T', {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
    {'U', {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}},
    {'V', {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}},
    {'W', {0x11,0x11,0x11,0x15,0x15,0x1B,0x11}},
    {'X', {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}},
    {'Y', {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}},
    {'Z', {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}},
    {'[', {0x0E,0x08,0x08,0x08,0x08,0x08,0x0E}},
    {']', {0x0E,0x02,0x02,0x02,0x02,0x02,0x0E}},
    {'_', {0x00,0x00,0x00,0x00,0x00,0x00,0x1F}},
    {'a', {0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F}},
    {'b', {0x10,0x10,0x1E,0x11,0x11,0x11,0x1E}},
    {'c', {0x00,0x00,0x0E,0x10,0x10,0x10,0x0E}},
    {'d', {0x01,0x01,0x0F,0x11,0x11,0x11,0x0F}},
    {'e', {0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E}},
    {'f', {0x06,0x09,0x08,0x1C,0x08,0x08,0x08}},
    {'g', {0x00,0x0F,0x11,0x11,0x0F,0x01,0x0E}},
    {'h', {0x10,0x10,0x1E,0x11,0x11,0x11,0x11}},
    {'i', {0x04,0x00,0x0C,0x04,0x04,0x04,0x0E}},
    {'j', {0x02,0x00,0x06,0x02,0x02,0x12,0x0C}},
    {'k', {0x10,0x10,0x12,0x14,0x18,0x14,0x12}},
    {'l', {0x0C,0x04,0x04,0x04,0x04,0x04,0x0E}},
    {'m', {0x00,0x00,0x1A,0x15,0x15,0x11,0x11}},
    {'n', {0x00,0x00,0x1E,0x11,0x11,0x11,0x11}},
    {'o', {0x00,0x00,0x0E,0x11,0x11,0x11,0x0E}},
    {'p', {0x00,0x1E,0x11,0x11,0x1E,0x10,0x10}},
    {'q', {0x00,0x0F,0x11,0x11,0x0F,0x01,0x01}},
    {'r', {0x00,0x00,0x16,0x19,0x10,0x10,0x10}},
    {'s', {0x00,0x00,0x0F,0x10,0x0E,0x01,0x1E}},
    {'t', {0x08,0x08,0x1C,0x08,0x08,0x09,0x06}},
    {'u', {0x00,0x00,0x11,0x11,0x11,0x13,0x0D}},
    {'v', {0x00,0x00,0x11,0x11,0x11,0x0A,0x04}},
    {'w', {0x00,0x00,0x11,0x11,0x15,0x15,0x0A}},
    {'x', {0x00,0x00,0x11,0x0A,0x04,0x0A,0x11}},
    {'y', {0x00,0x11,0x11,0x0F,0x01,0x11,0x0E}},
    {'z', {0x00,0x00,0x1F,0x02,0x04,0x08,0x1F}},
};

std::array<u8, 256> make_solid_16(u8 base_col, u8 detail_col, int pattern) {
    std::array<u8, 256> p{};
    p.fill(base_col);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            if (pattern == 1) { // Grass speckles
                if ((x == 3 && y == 4) || (x == 11 && y == 10) || (x == 7 && y == 13)) p[y * 16 + x] = detail_col;
            } else if (pattern == 2) { // Tilled furrows
                if (x == 0 || x == 15 || y == 0 || y == 15) p[y * 16 + x] = 2; // dark border
                else if ((y % 4) == 2) p[y * 16 + x] = detail_col;
            } else if (pattern == 3) { // Weed tuft
                if ((x >= 5 && x <= 10 && y >= 7 && y <= 13) && ((x + y) % 2 == 0)) p[y * 16 + x] = detail_col;
                if ((x == 4 && y == 6) || (x == 11 && y == 6) || (x == 7 && y == 5)) p[y * 16 + x] = detail_col;
            } else if (pattern == 4) { // Branch
                if (y >= 6 && y <= 9 && x >= 2 && x <= 13) p[y * 16 + x] = detail_col;
                if (y == 5 && x >= 9 && x <= 11) p[y * 16 + x] = detail_col;
            } else if (pattern == 5) { // Stone
                if (x >= 4 && x <= 11 && y >= 5 && y <= 11) p[y * 16 + x] = detail_col;
                if (x >= 5 && x <= 9 && y >= 6 && y <= 8) p[y * 16 + x] = 10;
            } else if (pattern == 6) { // Stump
                if (x >= 3 && x <= 12 && y >= 4 && y <= 13) p[y * 16 + x] = detail_col;
                if (x >= 5 && x <= 10 && y >= 5 && y <= 7) p[y * 16 + x] = 11;
            } else if (pattern == 7) { // Boulder
                if (x >= 2 && x <= 13 && y >= 3 && y <= 13) p[y * 16 + x] = detail_col;
                if (x >= 4 && x <= 10 && y >= 4 && y <= 7) p[y * 16 + x] = 10;
            }
        }
    }
    return p;
}

std::array<u8, 256> make_crop_16(u8 soil_col, u8 soil_furrow, int stage, u8 bulb_col) {
    auto p = make_solid_16(soil_col, soil_furrow, 2);
    if (stage == 0) { // Seeds
        p[6 * 16 + 5] = 11; p[6 * 16 + 10] = 11; p[10 * 16 + 7] = 11; p[9 * 16 + 11] = 11;
    } else if (stage == 1) { // Small green sprout
        for (int y = 6; y <= 10; ++y) p[y * 16 + 7] = 6;
        p[6 * 16 + 6] = 7; p[6 * 16 + 8] = 7;
    } else if (stage == 2) { // Medium leafy crop
        for (int y = 4; y <= 11; ++y) {
            for (int x = 5; x <= 10; ++x) {
                if ((x + y) % 2 == 0) p[y * 16 + x] = 7;
                else if (y > 6) p[y * 16 + x] = 6;
            }
        }
    } else { // Mature crop with bright colored bulb/fruit
        for (int y = 3; y <= 8; ++y) {
            for (int x = 4; x <= 11; ++x) {
                if ((x + y) % 2 == 0) p[y * 16 + x] = 7;
            }
        }
        for (int y = 8; y <= 13; ++y) {
            for (int x = 5; x <= 10; ++x) {
                p[y * 16 + x] = bulb_col;
            }
        }
    }
    return p;
}

std::array<u8, 256> make_farmer_sprite(int pose) {
    // 16x16 4bpp sprite using OBJ palette indices:
    // 0=transparent, 1=outline, 2=cap/hat, 3=skin, 4=overalls, 5=shirt/white, 6=hair/boots, 7=highlight
    std::array<u8, 256> s{};
    auto px = [&](int x, int y, u8 c) {
        if (x >= 0 && x < 16 && y >= 0 && y < 16) s[y * 16 + x] = c;
    };
    auto rect = [&](int x0, int y0, int w, int h, u8 c) {
        for (int y = y0; y < y0 + h; ++y)
            for (int x = x0; x < x0 + w; ++x) px(x, y, c);
    };

    // Hat / head
    rect(4, 1, 8, 4, 2);
    rect(3, 3, 10, 2, 2); // cap brim
    rect(4, 5, 8, 4, (pose == 2) ? 6 : 3); // pose 2 = facing Up (back of head)
    if (pose == 0 || pose == 1) { // Down face eyes
        px(5, 6, 1); px(10, 6, 1);
    } else if (pose == 3) { // Left profile
        px(5, 6, 1);
        rect(3, 3, 5, 2, 2);
    }
    // Torso / overalls
    rect(4, 9, 8, 4, 4);
    px(5, 9, 5); px(10, 9, 5); // shirt straps
    // Arms
    if (pose == 4) { // Tool swing raised arm
        rect(2, 6, 2, 4, 5);
        rect(12, 9, 2, 3, 5);
        rect(1, 3, 2, 4, 7); // tool gleam
    } else {
        rect(2, 9, 2, 3, 5);
        rect(12, 9, 2, 3, 5);
    }
    // Boots (walk bounce in pose 1)
    if (pose == 1) {
        rect(4, 12, 3, 3, 6);
        rect(9, 13, 3, 3, 6);
    } else {
        rect(4, 13, 3, 3, 6);
        rect(9, 13, 3, 3, 6);
    }
    return s;
}

} // namespace

RomProbeInfo probe_and_load_rom(GbaBus& bus, const std::string& path) {
    RomProbeInfo info{};
    auto try_load = [&](const std::string& p) -> bool {
        if (!bus.load_rom_file(p.c_str())) {
            return false;
        }
        info.found = true;
        info.size_bytes = bus.rom_size();
        if (info.size_bytes >= 0xC0u) {
            char title_buf[13]{};
            char code_buf[5]{};
            for (int i = 0; i < 12; ++i) title_buf[i] = static_cast<char>(bus.read8(ADDR_ROM_BASE + 0xA0u + i));
            for (int i = 0; i < 4; ++i) code_buf[i] = static_cast<char>(bus.read8(ADDR_ROM_BASE + 0xACu + i));
            info.title = title_buf;
            info.game_code = code_buf;
            info.valid_fomt_us = (info.title == "HARVESTMOGBA" && info.game_code == "A4NE");
        }
        return info.valid_fomt_us;
    };

    if (!path.empty() && try_load(path)) {
        return info;
    }

    // Auto-search current folder, ./roms, ./rom for any Harvest Moon FoMT (U) GBA ROM
    std::error_code ec;
    const std::vector<std::string> search_dirs = {".", "roms", "rom"};
    for (const auto& dir : search_dirs) {
        if (!std::filesystem::exists(dir, ec)) continue;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (entry.is_regular_file(ec)) {
                const auto ext = entry.path().extension().string();
                if (ext == ".gba" || ext == ".GBA" || ext == ".bin") {
                    const std::string candidate = entry.path().string();
                    if (candidate != path && try_load(candidate)) {
                        return info;
                    }
                }
            }
        }
    }
    return info;
}

void upload_gba_assets(GbaBus& bus) {
    // --- BG Palette 0: Farm Environment ---
    set_pal_color(bus, false, 0, 0,  88, 172,  76); // Backdrop meadow green
    set_pal_color(bus, false, 0, 1,  88, 172,  76); // Grass
    set_pal_color(bus, false, 0, 2,  66, 138,  56); // Grass / furrow dark
    set_pal_color(bus, false, 0, 3, 174, 126,  76); // Tilled soil
    set_pal_color(bus, false, 0, 4, 136,  94,  52); // Tilled furrow
    set_pal_color(bus, false, 0, 5,  94,  62,  36); // Watered soil
    set_pal_color(bus, false, 0, 6,  52, 128,  44); // Leaf dark
    set_pal_color(bus, false, 0, 7, 108, 206,  82); // Leaf bright
    set_pal_color(bus, false, 0, 8, 138,  88,  46); // Wood brown
    set_pal_color(bus, false, 0, 9, 136, 144, 152); // Stone gray
    set_pal_color(bus, false, 0, 10, 198, 204, 212); // Stone highlight
    set_pal_color(bus, false, 0, 11, 232, 198, 112); // Seeds / wood ring
    set_pal_color(bus, false, 0, 12, 248, 248, 248); // White (turnip)
    set_pal_color(bus, false, 0, 13, 224,  68,  62); // Red (tomato)
    set_pal_color(bus, false, 0, 14, 244, 204,  54); // Gold (corn)
    set_pal_color(bus, false, 0, 15,  42,  36,  32); // Dark outline

    // --- BG Palette 2: Crisp HUD & Dialog Box ---
    set_pal_color(bus, false, 2, 0,   0,   0,   0); // Transparent
    set_pal_color(bus, false, 2, 1, 248, 242, 220); // Cream panel / bright font
    set_pal_color(bus, false, 2, 2, 248, 248, 248); // Pure white font
    set_pal_color(bus, false, 2, 3,  48,  34,  24); // Dark ink text
    set_pal_color(bus, false, 2, 4, 146,  92,  44); // Wood dialog border
    set_pal_color(bus, false, 2, 5, 250, 212,  76); // Gold highlight

    // --- BG Palette 3: Title Screen Sky & Hills ---
    set_pal_color(bus, false, 3, 0,  86, 168, 242);
    set_pal_color(bus, false, 3, 1,  72, 148, 228); // Sky top
    set_pal_color(bus, false, 3, 2, 114, 190, 248); // Sky mid
    set_pal_color(bus, false, 3, 3, 164, 220, 252); // Sky horizon
    set_pal_color(bus, false, 3, 4,  84, 168,  84); // Hills
    set_pal_color(bus, false, 3, 5, 250, 232,  92); // Banner gold
    set_pal_color(bus, false, 3, 6,  38,  48,  68); // Shadow

    // --- OBJ Palettes 0..3: 4 Distinct Coop Farmer Outfits ---
    const u8 overalls[4][3] = {
        { 46, 110, 224}, // P1 Blue
        {214,  56,  64}, // P2 Crimson
        { 48, 176,  86}, // P3 Emerald
        {236, 176,  42}, // P4 Gold
    };
    const u8 caps[4][3] = {
        {222,  56,  52}, // P1 Red cap
        { 42, 180, 186}, // P2 Teal cap
        {232, 132,  40}, // P3 Amber cap
        { 58,  72, 180}, // P4 Navy cap
    };
    for (u8 bank = 0; bank < 4; ++bank) {
        set_pal_color(bus, true, bank, 0,   0,   0,   0);
        set_pal_color(bus, true, bank, 1,  32,  24,  24); // Outline
        set_pal_color(bus, true, bank, 2, caps[bank][0], caps[bank][1], caps[bank][2]);
        set_pal_color(bus, true, bank, 3, 248, 206, 166); // Skin
        set_pal_color(bus, true, bank, 4, overalls[bank][0], overalls[bank][1], overalls[bank][2]);
        set_pal_color(bus, true, bank, 5, 246, 244, 238); // White shirt
        set_pal_color(bus, true, bank, 6,  92,  58,  34); // Boots / hair
        set_pal_color(bus, true, bank, 7, 250, 226,  84); // Cursor / gleam yellow
    }

    // --- Upload 8x8 Pixel Font (tiles 0..95 -> ASCII 32..127) ---
    for (u16 code = 32; code < 128; ++code) {
        std::array<u8, 64> glyph_px{};
        glyph_px.fill(0);
        for (const auto& g : FONT_GLYPHS) {
            if (static_cast<u16>(g.ch) == code) {
                for (int r = 0; r < 7; ++r) {
                    for (int c = 0; c < 5; ++c) {
                        if (g.rows[r] & (1u << (4 - c))) {
                            glyph_px[(r + 1) * 8 + (c + 1)] = 2; // White glyph index 2
                        }
                    }
                }
                break;
            }
        }
        write_tile_4bpp(bus, 0u, static_cast<u16>(bgtile::FONT_BASE + (code - 32)), glyph_px);
    }

    // Solid fill & UI 9-slice border tiles
    std::array<u8, 64> solid{};
    solid.fill(1);
    write_tile_4bpp(bus, 0u, bgtile::SOLID_FILL, solid);

    for (u16 idx = 0; idx < 9; ++idx) {
        std::array<u8, 64> box{};
        box.fill(1); // Cream interior
        const bool top = idx < 3, bot = idx >= 6, left = (idx % 3) == 0, right = (idx % 3) == 2;
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                if ((top && y < 2) || (bot && y >= 6) || (left && x < 2) || (right && x >= 6)) {
                    box[y * 8 + x] = 4; // Wood border
                }
            }
        }
        write_tile_4bpp(bus, 0u, static_cast<u16>(bgtile::UI_FRAME_BASE + idx), box);
    }

    // Sky gradient tiles
    for (u16 s = 0; s < 4; ++s) {
        std::array<u8, 64> sky{};
        sky.fill(static_cast<u8>(std::clamp<int>(1 + s, 1, 3)));
        write_tile_4bpp(bus, 0u, static_cast<u16>(bgtile::SKY_GRAD_BASE + s), sky);
    }
    std::array<u8, 64> hill{};
    hill.fill(4);
    write_tile_4bpp(bus, 0u, bgtile::HILL_TILE, hill);

    std::array<u8, 64> fence{};
    fence.fill(1);
    for (int y = 2; y <= 5; ++y) for (int x = 0; x < 8; ++x) fence[y * 8 + x] = 8;
    for (int y = 0; y < 8; ++y) { fence[y * 8 + 3] = 11; fence[y * 8 + 4] = 8; }
    write_tile_4bpp(bus, 0u, bgtile::FENCE_TILE, fence);

    // 16x16 Farm Meta-Tiles
    write_meta_16x16(bus, bgtile::META_GRASS,   make_solid_16(1, 2, 1));
    write_meta_16x16(bus, bgtile::META_TILLED,  make_solid_16(3, 4, 2));
    write_meta_16x16(bus, bgtile::META_WATERED, make_solid_16(5, 4, 2));
    write_meta_16x16(bus, bgtile::META_WEED,    make_solid_16(1, 7, 3));
    write_meta_16x16(bus, bgtile::META_BRANCH,  make_solid_16(1, 8, 4));
    write_meta_16x16(bus, bgtile::META_STONE,   make_solid_16(1, 9, 5));
    write_meta_16x16(bus, bgtile::META_STUMP,   make_solid_16(1, 8, 6));
    write_meta_16x16(bus, bgtile::META_BOULDER, make_solid_16(1, 9, 7));

    write_meta_16x16(bus, bgtile::META_SEED,     make_crop_16(3, 4, 0, 12));
    write_meta_16x16(bus, bgtile::META_SPROUT,   make_crop_16(5, 4, 1, 12));
    write_meta_16x16(bus, bgtile::META_GROWING,  make_crop_16(5, 4, 2, 12));
    write_meta_16x16(bus, bgtile::META_MATURE_W, make_crop_16(5, 4, 3, 12));
    write_meta_16x16(bus, bgtile::META_MATURE_R, make_crop_16(5, 4, 3, 13));
    write_meta_16x16(bus, bgtile::META_MATURE_Y, make_crop_16(5, 4, 3, 14));

    // 16x16 OAM Sprites
    write_obj_16x16(bus, objtile::FARMER_DOWN_0, make_farmer_sprite(0));
    write_obj_16x16(bus, objtile::FARMER_DOWN_1, make_farmer_sprite(1));
    write_obj_16x16(bus, objtile::FARMER_UP_0,   make_farmer_sprite(2));
    write_obj_16x16(bus, objtile::FARMER_LEFT_0, make_farmer_sprite(3));
    write_obj_16x16(bus, objtile::FARMER_SWING,  make_farmer_sprite(4));

    // Target Cursor 16x16 bracket
    std::array<u8, 256> cursor{};
    for (int i = 0; i < 5; ++i) {
        cursor[0 * 16 + i] = 7; cursor[i * 16 + 0] = 7;
        cursor[0 * 16 + (15 - i)] = 7; cursor[i * 16 + 15] = 7;
        cursor[15 * 16 + i] = 7; cursor[(15 - i) * 16 + 0] = 7;
        cursor[15 * 16 + (15 - i)] = 7; cursor[(15 - i) * 16 + 15] = 7;
    }
    write_obj_16x16(bus, objtile::CURSOR_16X16, cursor);

    // Floating Turnip mascot sprite for Title Screen
    std::array<u8, 256> turnip{};
    for (int y = 2; y <= 6; ++y)
        for (int x = 6; x <= 9; ++x) turnip[y * 16 + x] = 4; // Leafy top
    for (int y = 6; y <= 13; ++y)
        for (int x = 4; x <= 11; ++x)
            if ((x >= 5 && x <= 10) || (y >= 7 && y <= 12)) turnip[y * 16 + x] = 5;
    write_obj_16x16(bus, objtile::TURNIP_16X16, turnip);

    // Cloud sprite
    std::array<u8, 256> cloud{};
    for (int y = 5; y <= 11; ++y)
        for (int x = 2; x <= 13; ++x) cloud[y * 16 + x] = 5;
    for (int y = 3; y <= 6; ++y)
        for (int x = 5; x <= 10; ++x) cloud[y * 16 + x] = 5;
    write_obj_16x16(bus, objtile::CLOUD_16X16, cloud);
}

void write_bg_tile(
    GbaBus& bus,
    u32 screen_block,
    u32 map_w_tiles,
    u32 tx,
    u32 ty,
    u16 tile_idx,
    u8 pal_bank,
    bool h_flip,
    bool v_flip) {

    const u32 block_x = tx / 32u;
    const u32 block_y = ty / 32u;
    const u32 blocks_per_row = map_w_tiles / 32u;
    const u32 sb = screen_block + block_y * blocks_per_row + block_x;
    const u32 addr = ADDR_VRAM_BASE + sb * 0x0800u + (((ty & 31u) * 32u + (tx & 31u)) * 2u);
    const u16 entry = static_cast<u16>(
        (tile_idx & 0x03FFu) |
        (h_flip ? 0x0400u : 0u) |
        (v_flip ? 0x0800u : 0u) |
        ((static_cast<u16>(pal_bank) & 0x0Fu) << 12));
    bus.write16(addr, entry);
}

void draw_bg_text(
    GbaBus& bus,
    u32 screen_block,
    u32 map_w_tiles,
    u32 tx,
    u32 ty,
    const char* text,
    u8 pal_bank) {

    if (!text) return;
    u32 cur_x = tx;
    for (const char* p = text; *p && cur_x < map_w_tiles; ++p, ++cur_x) {
        const unsigned char c = static_cast<unsigned char>(*p);
        const u16 tile = (c >= 32 && c < 128) ? static_cast<u16>(bgtile::FONT_BASE + (c - 32)) : bgtile::FONT_BASE;
        write_bg_tile(bus, screen_block, map_w_tiles, cur_x, ty, tile, pal_bank);
    }
}

void write_oam_sprite(
    GbaBus& bus,
    u32 oam_idx,
    i16 x,
    i16 y,
    u16 tile_idx,
    u8 pal_bank,
    u8 priority,
    bool h_flip,
    bool semi_transparent,
    u8 shape,
    u8 size) {

    if (oam_idx >= 128u) return;
    const u32 base = ADDR_OAM_BASE + oam_idx * 8u;
    const u16 attr0 = static_cast<u16>(
        (static_cast<u16>(y) & 0x00FFu) |
        (semi_transparent ? 0x0400u : 0u) |
        ((static_cast<u16>(shape) & 0x03u) << 14));
    const u16 attr1 = static_cast<u16>(
        (static_cast<u16>(x) & 0x01FFu) |
        (h_flip ? 0x1000u : 0u) |
        ((static_cast<u16>(size) & 0x03u) << 14));
    const u16 attr2 = static_cast<u16>(
        (tile_idx & 0x03FFu) |
        ((static_cast<u16>(priority) & 0x03u) << 10) |
        ((static_cast<u16>(pal_bank) & 0x0Fu) << 12));

    bus.write16(base + 0u, attr0);
    bus.write16(base + 2u, attr1);
    bus.write16(base + 4u, attr2);
}

} // namespace hm::game
