#include "hal/gba_bus.hpp"
#include "ppu/ppu.hpp"

#include <array>
#include <cassert>
#include <cstdio>

using namespace hm::hal;
using namespace hm::ppu;

namespace {

void write_4bpp_solid_tile(GbaBus& bus, u32 vram_off, u16 tile_idx, u8 pal_idx) {
    const u8 packed = static_cast<u8>((pal_idx & 0x0Fu) | ((pal_idx & 0x0Fu) << 4));
    for (u32 i = 0; i < 32u; ++i) {
        bus.write8(ADDR_VRAM_BASE + vram_off + tile_idx * 32u + i, packed);
    }
}

void write_8bpp_solid_tile(GbaBus& bus, u32 vram_off, u16 tile_idx, u8 pal_idx) {
    for (u32 i = 0; i < 64u; ++i) {
        bus.write8(ADDR_VRAM_BASE + vram_off + tile_idx * 64u + i, pal_idx);
    }
}

} // namespace

int main() {
    GbaBus bus;
    Ppu ppu;
    std::array<u32, SCREEN_WIDTH * SCREEN_HEIGHT> fb{};

    // 1. Mode 3: 15-bit bitmap
    bus.io_write16(reg::DISPCNT, 0x0403u); // Mode 3 + BG2
    const u16 red15 = rgb8_to_bgr555(248, 0, 0);
    bus.write16(ADDR_VRAM_BASE + (10 * SCREEN_WIDTH + 20) * 2u, red15);
    ppu.render_frame(bus, fb.data());
    assert(fb[10 * SCREEN_WIDTH + 20] == bgr555_to_rgba8888(red15));

    // 2. Mode 4: 8-bit indexed bitmap + Frame 1 page flip (0xA000)
    bus.reset();
    bus.io_write16(reg::DISPCNT, 0x0414u); // Mode 4, Frame 1 (bit 4), BG2
    const u16 green15 = rgb8_to_bgr555(0, 248, 0);
    bus.write16(ADDR_PAL_BASE + 5 * 2u, green15);
    bus.write8(ADDR_VRAM_BASE + 0xA000u + 5 * SCREEN_WIDTH + 7, 5);
    ppu.render_frame(bus, fb.data());
    assert(fb[5 * SCREEN_WIDTH + 7] == bgr555_to_rgba8888(green15));

    // 3. Mode 0: Regular BG0 4bpp + Scroll + OAM Sprite + WIN0 + Alpha Blend
    bus.reset();
    for (u32 i = 0; i < 128u; ++i) {
        bus.write16(ADDR_OAM_BASE + i * 8u, 0x0200u); // Hide unused OAM slots
    }
    bus.io_write16(reg::DISPCNT, 0x3140u); // Mode 0, 1D OBJ, BG0, OBJ, WIN0
    bus.io_write16(reg::BG0CNT, (28u << 8) | 1u); // ScreenBlock 28, priority 1
    bus.io_write16(reg::BG0HOFS, 4u);

    const u16 bg_blue = rgb8_to_bgr555(0, 64, 240);
    const u16 obj_red = rgb8_to_bgr555(240, 64, 0);
    const u16 backdrop = rgb8_to_bgr555(16, 16, 16);
    bus.write16(ADDR_PAL_BASE + 0, backdrop);
    bus.write16(ADDR_PAL_BASE + 1 * 2u, bg_blue);
    bus.write16(ADDR_PAL_BASE + 512u + 2 * 2u, obj_red);

    // Tile 1 = BG color 1
    write_4bpp_solid_tile(bus, 0u, 1, 1);
    // Map entry at (1, 0) (visible at x=4..11 because BG0HOFS=4)
    bus.write16(ADDR_VRAM_BASE + 28u * 0x0800u + 1 * 2u, 1u);

    // Sprite at OAM 0: x=6, y=0, 8x8, tile 0, semi-transparent, priority 1 (beats BG prio 1!)
    write_4bpp_solid_tile(bus, 0x10000u, 0, 2);
    bus.write16(ADDR_OAM_BASE + 0, 0x0400u); // y=0, semi-transparent
    bus.write16(ADDR_OAM_BASE + 2, 0x0006u); // x=6, 8x8
    bus.write16(ADDR_OAM_BASE + 4, 0x0400u); // tile 0, prio 1

    // Window 0 covers [0..32) x [0..16), allows BG0 + OBJ + Effects
    bus.io_write16(reg::WIN0H, (0u << 8) | 32u);
    bus.io_write16(reg::WIN0V, (0u << 8) | 16u);
    bus.io_write16(reg::WININ, 0x0031u);  // BG0 (0x01) + OBJ (0x10) + FX (0x20)
    bus.io_write16(reg::WINOUT, 0x0000u); // Outside WIN0: backdrop only

    // Semi-transparent OBJ blends 50/50 with BG0 (2nd target bit 8)
    bus.io_write16(reg::BLDCNT, 0x0100u);
    bus.io_write16(reg::BLDALPHA, 0x0808u); // EVA=8, EVB=8

    ppu.render_frame(bus, fb.data());

    // Pixel (4, 0): inside WIN0, BG0 visible without OBJ -> pure bg_blue
    assert(fb[0 * SCREEN_WIDTH + 4] == bgr555_to_rgba8888(bg_blue));
    // Pixel (6, 0): inside WIN0, semi-transparent OBJ over BG0 -> 50/50 blend (~120, 64, 120)
    const u16 expected_blend = rgb8_to_bgr555(120, 64, 120);
    assert(fb[0 * SCREEN_WIDTH + 6] == bgr555_to_rgba8888(expected_blend));
    // Pixel (40, 0): outside WIN0 -> backdrop
    assert(fb[0 * SCREEN_WIDTH + 40] == bgr555_to_rgba8888(backdrop));

    // 4. Mode 2: Affine BG2 (wraparound + 2x zoom)
    bus.reset();
    bus.io_write16(reg::DISPCNT, 0x0402u); // Mode 2, BG2
    bus.io_write16(reg::BG2CNT, (28u << 8) | 0x2000u); // 128x128, wraparound, screenblock 28
    const u16 gold = rgb8_to_bgr555(248, 200, 32);
    bus.write16(ADDR_PAL_BASE + 3 * 2u, gold);
    write_8bpp_solid_tile(bus, 0u, 1, 3);
    bus.write8(ADDR_VRAM_BASE + 28u * 0x0800u + 0, 1); // Tile (0,0) = tile 1
    bus.io_write16(reg::BG2PA, 0x0080u); // 0.5 step -> 2x zoom!
    bus.io_write16(reg::BG2PD, 0x0080u);
    ppu.render_frame(bus, fb.data());
    // Tile (0,0) is 8x8 -> at 2x zoom covers screen pixels 0..15!
    assert(fb[12 * SCREEN_WIDTH + 12] == bgr555_to_rgba8888(gold));

    std::puts("test_ppu: ALL PASSED");
    return 0;
}
