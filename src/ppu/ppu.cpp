#include "ppu.hpp"
#include "hal/gba_bus.hpp"

#include <algorithm>

namespace hm::ppu {

using namespace hm::hal;

namespace {

inline u16 load16(const u8* p) {
    return static_cast<u16>(p[0]) | (static_cast<u16>(p[1]) << 8);
}

inline i16 load_s16(const u8* p) {
    return static_cast<i16>(load16(p));
}

// Sprite dimensions indexed by [shape][size] -> {width, height}
constexpr u8 OBJ_WIDTH[4][4] = {
    {8, 16, 32, 64}, // Shape 0: Square
    {16, 32, 32, 64}, // Shape 1: Horizontal
    {8, 8, 16, 32},   // Shape 2: Vertical
    {0, 0, 0, 0}
};

constexpr u8 OBJ_HEIGHT[4][4] = {
    {8, 16, 32, 64}, // Shape 0: Square
    {8, 8, 16, 32},  // Shape 1: Horizontal
    {16, 32, 32, 64}, // Shape 2: Vertical
    {0, 0, 0, 0}
};

} // namespace

void Ppu::reset() {
    bg2x_int_ = 0;
    bg2y_int_ = 0;
    bg3x_int_ = 0;
    bg3y_int_ = 0;
}

i32 Ppu::sign_extend_28(u32 val) {
    val &= 0x0FFFFFFFu;
    if (val & 0x08000000u) {
        val |= 0xF0000000u;
    }
    return static_cast<i32>(val);
}

void Ppu::latch_affine_refs(const GbaBus& bus) {
    bg2x_int_ = sign_extend_28(bus.io_read32(reg::BG2X));
    bg2y_int_ = sign_extend_28(bus.io_read32(reg::BG2Y));
    bg3x_int_ = sign_extend_28(bus.io_read32(reg::BG3X));
    bg3y_int_ = sign_extend_28(bus.io_read32(reg::BG3Y));
}

bool Ppu::in_window_range(u32 pos, u32 start, u32 end) {
    if (start <= end) {
        return pos >= start && pos < end;
    }
    // Wraparound window
    return pos >= start || pos < end;
}

u8 Ppu::compute_window_mask(const GbaBus& bus, u16 x, u16 y, bool obj_win_pixel) const {
    const u16 dispcnt = bus.io_read16(reg::DISPCNT);
    const bool win0_en = (dispcnt & 0x2000u) != 0;
    const bool win1_en = (dispcnt & 0x4000u) != 0;
    const bool objw_en = (dispcnt & 0x8000u) != 0;

    if (!win0_en && !win1_en && !objw_en) {
        return 0x3Fu; // All layers + special effect enabled
    }

    const u16 winin  = bus.io_read16(reg::WININ);
    const u16 winout = bus.io_read16(reg::WINOUT);

    if (win0_en) {
        const u16 h = bus.io_read16(reg::WIN0H);
        const u16 v = bus.io_read16(reg::WIN0V);
        if (in_window_range(x, (h >> 8) & 0xFFu, h & 0xFFu) &&
            in_window_range(y, (v >> 8) & 0xFFu, v & 0xFFu)) {
            return static_cast<u8>(winin & 0x3Fu);
        }
    }

    if (win1_en) {
        const u16 h = bus.io_read16(reg::WIN1H);
        const u16 v = bus.io_read16(reg::WIN1V);
        if (in_window_range(x, (h >> 8) & 0xFFu, h & 0xFFu) &&
            in_window_range(y, (v >> 8) & 0xFFu, v & 0xFFu)) {
            return static_cast<u8>((winin >> 8) & 0x3Fu);
        }
    }

    if (objw_en && obj_win_pixel) {
        return static_cast<u8>((winout >> 8) & 0x3Fu);
    }

    return static_cast<u8>(winout & 0x3Fu);
}

u16 Ppu::blend_alpha(u16 top, u16 bot, u8 eva, u8 evb) {
    const u32 r1 = (top >> 0) & 0x1Fu, g1 = (top >> 5) & 0x1Fu, b1 = (top >> 10) & 0x1Fu;
    const u32 r2 = (bot >> 0) & 0x1Fu, g2 = (bot >> 5) & 0x1Fu, b2 = (bot >> 10) & 0x1Fu;
    const u32 r = std::min<u32>(31u, (r1 * eva + r2 * evb) >> 4);
    const u32 g = std::min<u32>(31u, (g1 * eva + g2 * evb) >> 4);
    const u32 b = std::min<u32>(31u, (b1 * eva + b2 * evb) >> 4);
    return static_cast<u16>(r | (g << 5) | (b << 10));
}

u16 Ppu::blend_bright_inc(u16 c, u8 evy) {
    const u32 r = (c >> 0) & 0x1Fu, g = (c >> 5) & 0x1Fu, b = (c >> 10) & 0x1Fu;
    const u32 nr = r + (((31u - r) * evy) >> 4);
    const u32 ng = g + (((31u - g) * evy) >> 4);
    const u32 nb = b + (((31u - b) * evy) >> 4);
    return static_cast<u16>(std::min<u32>(31u, nr) | (std::min<u32>(31u, ng) << 5) | (std::min<u32>(31u, nb) << 10));
}

u16 Ppu::blend_bright_dec(u16 c, u8 evy) {
    const u32 r = (c >> 0) & 0x1Fu, g = (c >> 5) & 0x1Fu, b = (c >> 10) & 0x1Fu;
    const u32 nr = r - ((r * evy) >> 4);
    const u32 ng = g - ((g * evy) >> 4);
    const u32 nb = b - ((b * evy) >> 4);
    return static_cast<u16>(nr | (ng << 5) | (nb << 10));
}

void Ppu::render_regular_bg(
    const GbaBus& bus,
    int bg_idx,
    u16 y,
    std::array<PixelCandidate, SCREEN_WIDTH>& out_line) const {

    const u16 bgcnt = bus.io_read16(reg::BG0CNT + static_cast<u32>(bg_idx) * 2u);
    const u8  prio  = static_cast<u8>(bgcnt & 0x03u);
    const u32 char_base = ((bgcnt >> 2) & 0x03u) * 0x4000u;
    const bool mosaic   = (bgcnt & 0x0040u) != 0;
    const bool is_8bpp  = (bgcnt & 0x0080u) != 0;
    const u32 scr_base  = ((bgcnt >> 8) & 0x1Fu) * 0x0800u;
    const u32 size_mode = (bgcnt >> 14) & 0x03u;

    const u32 map_w_tiles = (size_mode & 1u) ? 64u : 32u;
    const u32 map_h_tiles = (size_mode & 2u) ? 64u : 32u;

    const u16 hofs = bus.io_read16(reg::BG0HOFS + static_cast<u32>(bg_idx) * 4u) & 0x01FFu;
    const u16 vofs = bus.io_read16(reg::BG0VOFS + static_cast<u32>(bg_idx) * 4u) & 0x01FFu;

    u32 eff_y = y;
    u32 mos_w = 1;
    if (mosaic) {
        const u16 mos = bus.io_read16(reg::MOSAIC);
        mos_w = (mos & 0x0Fu) + 1u;
        const u32 mos_h = ((mos >> 4) & 0x0Fu) + 1u;
        eff_y = (y / mos_h) * mos_h;
    }

    const u32 map_y = (eff_y + vofs) & (map_h_tiles * 8u - 1u);
    const u32 tile_y = map_y >> 3;
    const u32 py_in_tile = map_y & 7u;

    const u8* vram = bus.vram();
    const u8* pal  = bus.pal_ram();

    for (u32 x = 0; x < SCREEN_WIDTH; ++x) {
        const u32 eff_x = mosaic ? ((x / mos_w) * mos_w) : x;
        const u32 map_x = (eff_x + hofs) & (map_w_tiles * 8u - 1u);
        const u32 tile_x = map_x >> 3;
        const u32 px_in_tile = map_x & 7u;

        // 32x32 screenblock layout
        u32 block_idx = 0;
        if (size_mode == 1u && tile_x >= 32u) block_idx = 1u;
        else if (size_mode == 2u && tile_y >= 32u) block_idx = 1u;
        else if (size_mode == 3u) block_idx = (tile_x >= 32u ? 1u : 0u) + (tile_y >= 32u ? 2u : 0u);

        const u32 entry_addr = scr_base + block_idx * 0x0800u + (((tile_y & 31u) * 32u + (tile_x & 31u)) * 2u);
        if (entry_addr + 1u >= VRAM_SIZE) continue;
        const u16 entry = load16(&vram[entry_addr]);

        const u32 tile_num = entry & 0x03FFu;
        const bool h_flip = (entry & 0x0400u) != 0;
        const bool v_flip = (entry & 0x0800u) != 0;
        const u32 pal_bank = (entry >> 12) & 0x0Fu;

        const u32 tx = h_flip ? (7u - px_in_tile) : px_in_tile;
        const u32 ty = v_flip ? (7u - py_in_tile) : py_in_tile;

        u8 color_idx = 0;
        if (is_8bpp) {
            const u32 addr = char_base + tile_num * 64u + ty * 8u + tx;
            if (addr >= 0x10000u) continue; // BG VRAM cannot access OBJ VRAM
            color_idx = vram[addr];
            if (color_idx == 0) continue;
            out_line[x] = PixelCandidate{load16(&pal[color_idx * 2u]), prio, static_cast<u8>(bg_idx), true, false};
        } else {
            const u32 addr = char_base + tile_num * 32u + ty * 4u + (tx >> 1);
            if (addr >= 0x10000u) continue;
            const u8 b = vram[addr];
            color_idx = (tx & 1u) ? ((b >> 4) & 0x0Fu) : (b & 0x0Fu);
            if (color_idx == 0) continue;
            const u32 pal_idx = pal_bank * 16u + color_idx;
            out_line[x] = PixelCandidate{load16(&pal[pal_idx * 2u]), prio, static_cast<u8>(bg_idx), true, false};
        }
    }
}

void Ppu::render_affine_bg(
    const GbaBus& bus,
    int bg_idx,
    u16 y,
    i32 ref_x,
    i32 ref_y,
    std::array<PixelCandidate, SCREEN_WIDTH>& out_line) const {

    const u16 bgcnt = bus.io_read16(reg::BG0CNT + static_cast<u32>(bg_idx) * 2u);
    const u8  prio  = static_cast<u8>(bgcnt & 0x03u);
    const u32 char_base = ((bgcnt >> 2) & 0x03u) * 0x4000u;
    const bool mosaic   = (bgcnt & 0x0040u) != 0;
    const u32 scr_base  = ((bgcnt >> 8) & 0x1Fu) * 0x0800u;
    const bool wrap     = (bgcnt & 0x2000u) != 0;
    const u32 size_mode = (bgcnt >> 14) & 0x03u;

    const i32 size_px = 128 << size_mode; // 128, 256, 512, 1024
    const u32 tiles_per_row = static_cast<u32>(size_px >> 3);

    const u32 affine_base = (bg_idx == 2) ? reg::BG2PA : reg::BG3PA;
    const i16 pa = static_cast<i16>(bus.io_read16(affine_base + 0u));
    const i16 pc = static_cast<i16>(bus.io_read16(affine_base + 4u));

    u32 mos_w = 1;
    if (mosaic) {
        mos_w = (bus.io_read16(reg::MOSAIC) & 0x0Fu) + 1u;
    }
    (void)y;

    const u8* vram = bus.vram();
    const u8* pal  = bus.pal_ram();

    i32 cur_x = ref_x;
    i32 cur_y = ref_y;

    for (u32 x = 0; x < SCREEN_WIDTH; ++x) {
        if (mosaic && (x % mos_w) != 0 && x > 0) {
            out_line[x] = out_line[x - 1];
            cur_x += pa;
            cur_y += pc;
            continue;
        }

        i32 tx = cur_x >> 8;
        i32 ty = cur_y >> 8;
        cur_x += pa;
        cur_y += pc;

        if (wrap) {
            tx &= (size_px - 1);
            ty &= (size_px - 1);
        } else if (tx < 0 || tx >= size_px || ty < 0 || ty >= size_px) {
            continue;
        }

        const u32 tile_x = static_cast<u32>(tx) >> 3;
        const u32 tile_y = static_cast<u32>(ty) >> 3;
        const u32 scr_addr = scr_base + tile_y * tiles_per_row + tile_x;
        if (scr_addr >= 0x10000u) continue;

        const u32 tile_num = vram[scr_addr];
        const u32 char_addr = char_base + tile_num * 64u + (static_cast<u32>(ty) & 7u) * 8u + (static_cast<u32>(tx) & 7u);
        if (char_addr >= 0x10000u) continue;

        const u8 color_idx = vram[char_addr];
        if (color_idx == 0) continue;
        out_line[x] = PixelCandidate{load16(&pal[color_idx * 2u]), prio, static_cast<u8>(bg_idx), true, false};
    }
}

void Ppu::render_bitmap_bg(
    const GbaBus& bus,
    u8 mode,
    u16 y,
    std::array<PixelCandidate, SCREEN_WIDTH>& out_line) const {

    const u16 dispcnt = bus.io_read16(reg::DISPCNT);
    const u16 bg2cnt  = bus.io_read16(reg::BG2CNT);
    const u8  prio    = static_cast<u8>(bg2cnt & 0x03u);
    const u32 frame_base = (dispcnt & 0x0010u) ? 0xA000u : 0x0000u;

    const u8* vram = bus.vram();
    const u8* pal  = bus.pal_ram();

    if (mode == 3) {
        const u32 row_off = static_cast<u32>(y) * SCREEN_WIDTH * 2u;
        for (u32 x = 0; x < SCREEN_WIDTH; ++x) {
            const u16 c = load16(&vram[row_off + x * 2u]);
            out_line[x] = PixelCandidate{static_cast<u16>(c & 0x7FFFu), prio, 2, true, false};
        }
    } else if (mode == 4) {
        const u32 row_off = frame_base + static_cast<u32>(y) * SCREEN_WIDTH;
        for (u32 x = 0; x < SCREEN_WIDTH; ++x) {
            const u8 idx = vram[row_off + x];
            if (idx == 0) continue;
            out_line[x] = PixelCandidate{static_cast<u16>(load16(&pal[idx * 2u]) & 0x7FFFu), prio, 2, true, false};
        }
    } else if (mode == 5) {
        if (y >= 128u) return;
        const u32 row_off = frame_base + static_cast<u32>(y) * 160u * 2u;
        for (u32 x = 0; x < 160u; ++x) {
            const u16 c = load16(&vram[row_off + x * 2u]);
            out_line[x] = PixelCandidate{static_cast<u16>(c & 0x7FFFu), prio, 2, true, false};
        }
    }
}

void Ppu::render_objects(
    const GbaBus& bus,
    u16 y,
    u16 dispcnt,
    std::array<ObjPixel, SCREEN_WIDTH>& out_obj) const {

    if (!(dispcnt & 0x1000u)) return;

    const u8 mode = static_cast<u8>(dispcnt & 0x07u);
    const bool mapping_1d = (dispcnt & 0x0040u) != 0;
    const bool bitmap_mode = (mode >= 3u);

    const u8* oam  = bus.oam();
    const u8* vram = bus.vram();
    const u8* pal  = bus.pal_ram() + 512u; // OBJ palette is second 512 bytes

    const u16 mos = bus.io_read16(reg::MOSAIC);
    const u32 obj_mos_w = ((mos >> 8) & 0x0Fu) + 1u;
    const u32 obj_mos_h = ((mos >> 12) & 0x0Fu) + 1u;

    for (int i = 0; i < 128; ++i) {
        const u16 attr0 = load16(&oam[i * 8 + 0]);
        const u16 attr1 = load16(&oam[i * 8 + 2]);
        const u16 attr2 = load16(&oam[i * 8 + 4]);

        const u8 obj_mode = static_cast<u8>((attr0 >> 8) & 0x03u);
        if (obj_mode == 2u) continue; // Disabled / Hidden

        const bool is_affine   = (obj_mode & 1u) != 0;
        const bool double_size = (obj_mode == 3u);
        const u8 gfx_mode      = static_cast<u8>((attr0 >> 10) & 0x03u);
        const bool mosaic      = (attr0 & 0x1000u) != 0;
        const bool is_8bpp     = (attr0 & 0x2000u) != 0;
        const u8 shape         = static_cast<u8>((attr0 >> 14) & 0x03u);
        const u8 size          = static_cast<u8>((attr1 >> 14) & 0x03u);

        const u8 spr_w = OBJ_WIDTH[shape][size];
        const u8 spr_h = OBJ_HEIGHT[shape][size];
        if (spr_w == 0 || spr_h == 0) continue;

        const i32 box_w = double_size ? spr_w * 2 : spr_w;
        const i32 box_h = double_size ? spr_h * 2 : spr_h;

        i32 spr_y = static_cast<i32>(attr0 & 0xFFu);
        if (spr_y >= 160) spr_y -= 256;
        if (static_cast<i32>(y) < spr_y || static_cast<i32>(y) >= spr_y + box_h) continue;

        i32 spr_x = static_cast<i32>(attr1 & 0x01FFu);
        if (spr_x >= 240) spr_x -= 512;

        const u32 base_tile = attr2 & 0x03FFu;
        const u8  prio      = static_cast<u8>((attr2 >> 10) & 0x03u);
        const u32 pal_bank  = (attr2 >> 12) & 0x0Fu;

        i16 pa = 0x0100, pb = 0, pc = 0, pd = 0x0100;
        if (is_affine) {
            const u32 aff_idx = (attr1 >> 9) & 0x1Fu;
            const u32 aff_base = aff_idx * 32u;
            pa = load_s16(&oam[aff_base + 0x06u]);
            pb = load_s16(&oam[aff_base + 0x0Eu]);
            pc = load_s16(&oam[aff_base + 0x16u]);
            pd = load_s16(&oam[aff_base + 0x1Eu]);
        }

        const bool h_flip = !is_affine && ((attr1 & 0x1000u) != 0);
        const bool v_flip = !is_affine && ((attr1 & 0x2000u) != 0);

        i32 local_y = static_cast<i32>(y) - spr_y;
        if (mosaic) {
            local_y = (static_cast<i32>(y / obj_mos_h) * static_cast<i32>(obj_mos_h)) - spr_y;
            local_y = std::clamp(local_y, 0, box_h - 1);
        }

        const u32 tiles_w = spr_w >> 3;

        for (i32 lx = 0; lx < box_w; ++lx) {
            const i32 sx = spr_x + lx;
            if (sx < 0 || sx >= static_cast<i32>(SCREEN_WIDTH)) continue;

            const i32 eff_lx = mosaic ? (static_cast<i32>((sx / obj_mos_w) * obj_mos_w) - spr_x) : lx;
            i32 tex_x = 0, tex_y = 0;

            if (is_affine) {
                const i32 dx = eff_lx - (box_w >> 1);
                const i32 dy = local_y - (box_h >> 1);
                tex_x = ((pa * dx + pb * dy) >> 8) + (spr_w >> 1);
                tex_y = ((pc * dx + pd * dy) >> 8) + (spr_h >> 1);
                if (tex_x < 0 || tex_x >= spr_w || tex_y < 0 || tex_y >= spr_h) continue;
            } else {
                tex_x = h_flip ? (spr_w - 1 - std::clamp(eff_lx, 0, spr_w - 1)) : std::clamp(eff_lx, 0, spr_w - 1);
                tex_y = v_flip ? (spr_h - 1 - local_y) : local_y;
            }

            const u32 tile_x = static_cast<u32>(tex_x) >> 3;
            const u32 tile_y = static_cast<u32>(tex_y) >> 3;
            const u32 px = static_cast<u32>(tex_x) & 7u;
            const u32 py = static_cast<u32>(tex_y) & 7u;

            u32 tile_num = 0;
            if (mapping_1d) {
                tile_num = base_tile + (tile_y * tiles_w + tile_x) * (is_8bpp ? 2u : 1u);
            } else {
                tile_num = base_tile + tile_y * 32u + tile_x * (is_8bpp ? 2u : 1u);
            }
            tile_num &= 0x03FFu;

            // Bitmap modes disallow tiles < 512 in OBJ VRAM
            if (bitmap_mode && tile_num < 512u) continue;

            u8 color_idx = 0;
            if (is_8bpp) {
                const u32 addr = 0x10000u + tile_num * 32u + py * 8u + px;
                if (addr >= VRAM_SIZE) continue;
                color_idx = vram[addr];
            } else {
                const u32 addr = 0x10000u + tile_num * 32u + py * 4u + (px >> 1);
                if (addr >= VRAM_SIZE) continue;
                const u8 b = vram[addr];
                color_idx = (px & 1u) ? ((b >> 4) & 0x0Fu) : (b & 0x0Fu);
            }

            if (color_idx == 0) continue;

            if (gfx_mode == 2u) {
                // OBJ Window mode sets window mask without drawing sprite pixel
                out_obj[sx].obj_window = true;
                continue;
            }

            // Lowest OAM index wins for equal priority; lower priority value wins overall
            if (!out_obj[sx].valid || prio < out_obj[sx].priority) {
                const u32 pal_off = is_8bpp ? (color_idx * 2u) : ((pal_bank * 16u + color_idx) * 2u);
                out_obj[sx].bgr555 = load16(&pal[pal_off]) & 0x7FFFu;
                out_obj[sx].priority = prio;
                out_obj[sx].valid = true;
                out_obj[sx].semi_transparent = (gfx_mode == 1u);
            }
        }
    }
}

void Ppu::render_scanline(const GbaBus& bus, u16 y, u32* dst_rgba) {
    if (y >= SCREEN_HEIGHT || !dst_rgba) return;

    const u16 dispcnt = bus.io_read16(reg::DISPCNT);

    // Forced blank (bit 7): pure white
    if (dispcnt & 0x0080u) {
        const u32 white = bgr555_to_rgba8888(0x7FFFu);
        std::fill_n(dst_rgba, SCREEN_WIDTH, white);
        return;
    }

    const u8 mode = static_cast<u8>(dispcnt & 0x07u);

    std::array<std::array<PixelCandidate, SCREEN_WIDTH>, 4> bg_lines{};
    std::array<ObjPixel, SCREEN_WIDTH> obj_line{};

    if (mode == 0u) {
        for (int bg = 0; bg < 4; ++bg) {
            if (dispcnt & (1u << (8 + bg))) {
                render_regular_bg(bus, bg, y, bg_lines[bg]);
            }
        }
    } else if (mode == 1u) {
        if (dispcnt & (1u << 8)) render_regular_bg(bus, 0, y, bg_lines[0]);
        if (dispcnt & (1u << 9)) render_regular_bg(bus, 1, y, bg_lines[1]);
        if (dispcnt & (1u << 10)) render_affine_bg(bus, 2, y, bg2x_int_, bg2y_int_, bg_lines[2]);
    } else if (mode == 2u) {
        if (dispcnt & (1u << 10)) render_affine_bg(bus, 2, y, bg2x_int_, bg2y_int_, bg_lines[2]);
        if (dispcnt & (1u << 11)) render_affine_bg(bus, 3, y, bg3x_int_, bg3y_int_, bg_lines[3]);
    } else if (mode == 3u || mode == 4u || mode == 5u) {
        if (dispcnt & (1u << 10)) render_bitmap_bg(bus, mode, y, bg_lines[2]);
    }

    render_objects(bus, y, dispcnt, obj_line);

    const u16 backdrop = load16(bus.pal_ram()) & 0x7FFFu;
    const u16 bldcnt   = bus.io_read16(reg::BLDCNT);
    const u16 bldalpha = bus.io_read16(reg::BLDALPHA);
    const u8  eva      = static_cast<u8>(std::min<u16>(16u, bldalpha & 0x1Fu));
    const u8  evb      = static_cast<u8>(std::min<u16>(16u, (bldalpha >> 8) & 0x1Fu));
    const u8  evy      = static_cast<u8>(std::min<u16>(16u, bus.io_read16(reg::BLDY) & 0x1Fu));
    const u8  bld_mode = static_cast<u8>((bldcnt >> 6) & 0x03u);

    for (u16 x = 0; x < SCREEN_WIDTH; ++x) {
        const u8 win_mask = compute_window_mask(bus, x, y, obj_line[x].obj_window);

        // Collect candidates in exact priority order (OBJ beats BG of equal priority)
        PixelCandidate top{backdrop, 4, 5, true, false};
        PixelCandidate bot{backdrop, 4, 5, true, false};
        bool found_top = false;
        bool found_bot = false;

        for (u8 prio = 0; prio <= 3; ++prio) {
            if ((win_mask & 0x10u) && obj_line[x].valid && obj_line[x].priority == prio) {
                PixelCandidate c{obj_line[x].bgr555, prio, 4, true, obj_line[x].semi_transparent};
                if (!found_top) { top = c; found_top = true; }
                else if (!found_bot) { bot = c; found_bot = true; break; }
            }
            for (int bg = 0; bg < 4; ++bg) {
                if ((win_mask & (1u << bg)) && bg_lines[bg][x].valid && bg_lines[bg][x].priority == prio) {
                    if (!found_top) { top = bg_lines[bg][x]; found_top = true; }
                    else if (!found_bot) { bot = bg_lines[bg][x]; found_bot = true; break; }
                }
            }
            if (found_bot) break;
        }

        u16 out_color = top.bgr555;
        const bool fx_allowed = (win_mask & 0x20u) != 0;
        const bool top_is_target1 = (bldcnt & (1u << top.layer_bit)) != 0;
        const bool bot_is_target2 = (bldcnt & (1u << (8u + bot.layer_bit))) != 0;

        if (top.semi_transparent && bot_is_target2) {
            out_color = blend_alpha(top.bgr555, bot.bgr555, eva, evb);
        } else if (fx_allowed && top_is_target1) {
            if (bld_mode == 1u && bot_is_target2) {
                out_color = blend_alpha(top.bgr555, bot.bgr555, eva, evb);
            } else if (bld_mode == 2u) {
                out_color = blend_bright_inc(top.bgr555, evy);
            } else if (bld_mode == 3u) {
                out_color = blend_bright_dec(top.bgr555, evy);
            }
        }

        dst_rgba[x] = bgr555_to_rgba8888(out_color);
    }

    // Advance internal affine reference points by dmx/dmy (PB/PD) after visible scanline
    if (mode == 1u || mode == 2u) {
        bg2x_int_ += static_cast<i16>(bus.io_read16(reg::BG2PB));
        bg2y_int_ += static_cast<i16>(bus.io_read16(reg::BG2PD));
        if (mode == 2u) {
            bg3x_int_ += static_cast<i16>(bus.io_read16(reg::BG3PB));
            bg3y_int_ += static_cast<i16>(bus.io_read16(reg::BG3PD));
        }
    }
}

void Ppu::render_frame(const GbaBus& bus, u32* dst_rgba) {
    if (!dst_rgba) return;
    latch_affine_refs(bus);
    for (u16 y = 0; y < SCREEN_HEIGHT; ++y) {
        render_scanline(bus, y, dst_rgba + static_cast<std::size_t>(y) * SCREEN_WIDTH);
    }
}

} // namespace hm::ppu
