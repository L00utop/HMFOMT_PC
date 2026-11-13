#pragma once

#include "hal/gba_types.hpp"

#include <array>

namespace hm::hal {
class GbaBus;
}

namespace hm::ppu {

class Ppu {
public:
    Ppu() = default;

    void reset();

    // Latch internal BG2/BG3 affine reference points (call at start of frame / VBlank end)
    void latch_affine_refs(const hal::GbaBus& bus);

    // Render one scanline (0..159) into RGBA8888 destination row (240 pixels)
    void render_scanline(const hal::GbaBus& bus, hal::u16 y, hal::u32* dst_rgba);

    // Convenience: render full 240x160 frame into RGBA8888 buffer (latching affine refs at y=0)
    void render_frame(const hal::GbaBus& bus, hal::u32* dst_rgba);

private:
    struct PixelCandidate {
        hal::u16 bgr555 = 0;
        hal::u8  priority = 4;
        hal::u8  layer_bit = 5; // 0..3 = BG0..3, 4 = OBJ, 5 = Backdrop
        bool     valid = false;
        bool     semi_transparent = false;
    };

    struct ObjPixel {
        hal::u16 bgr555 = 0;
        hal::u8  priority = 4;
        bool     valid = false;
        bool     semi_transparent = false;
        bool     obj_window = false;
    };

    static hal::i32 sign_extend_28(hal::u32 val);
    static bool in_window_range(hal::u32 pos, hal::u32 start, hal::u32 end);

    void render_regular_bg(
        const hal::GbaBus& bus,
        int bg_idx,
        hal::u16 y,
        std::array<PixelCandidate, hal::SCREEN_WIDTH>& out_line) const;

    void render_affine_bg(
        const hal::GbaBus& bus,
        int bg_idx,
        hal::u16 y,
        hal::i32 ref_x,
        hal::i32 ref_y,
        std::array<PixelCandidate, hal::SCREEN_WIDTH>& out_line) const;

    void render_bitmap_bg(
        const hal::GbaBus& bus,
        hal::u8 mode,
        hal::u16 y,
        std::array<PixelCandidate, hal::SCREEN_WIDTH>& out_line) const;

    void render_objects(
        const hal::GbaBus& bus,
        hal::u16 y,
        hal::u16 dispcnt,
        std::array<ObjPixel, hal::SCREEN_WIDTH>& out_obj) const;

    hal::u8 compute_window_mask(const hal::GbaBus& bus, hal::u16 x, hal::u16 y, bool obj_win_pixel) const;
    static hal::u16 blend_alpha(hal::u16 top, hal::u16 bot, hal::u8 eva, hal::u8 evb);
    static hal::u16 blend_bright_inc(hal::u16 c, hal::u8 evy);
    static hal::u16 blend_bright_dec(hal::u16 c, hal::u8 evy);

    hal::i32 bg2x_int_ = 0;
    hal::i32 bg2y_int_ = 0;
    hal::i32 bg3x_int_ = 0;
    hal::i32 bg3y_int_ = 0;
};

} // namespace hm::ppu
