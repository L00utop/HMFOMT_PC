#pragma once

#include "fomt_domain.hpp"
#include "hal/gba_types.hpp"

#include <array>
#include <cstddef>
#include <vector>

namespace hm::hal {
class GbaBus;
}

namespace hm::game {

// Decompresses a Harvest Moon: FoMT ROM asset block (compression types 0..4, delta filter 0..4)
bool unpack_fomt(const hal::u8* rom, std::size_t rom_size, std::size_t offset, std::vector<hal::u8>& out);

// Non-owning parsed view into an uncompressed FoMT SpriteArchive (anims, cels, oams, 4bpp/8bpp tiles, pals, seqs)
struct SpriteArchiveView {
    struct Anim {
        hal::u16 n_frames = 0;
        hal::u16 seq0     = 0;
    };
    struct Cel {
        hal::u16 oam_n = 0;
        hal::u16 oam_i = 0;
        hal::u16 unk4  = 0;
        hal::u16 tile_i = 0;
        hal::u16 unk8  = 0;
        hal::u16 pal_i = 0;
        hal::u16 unkC  = 0;
        hal::u16 unkE  = 0;
    };
    struct Oam {
        hal::u16 attr0 = 0;
        hal::u16 attr1 = 0;
        hal::u16 attr2 = 0;
        hal::u16 pad   = 0;
    };
    struct Seq {
        hal::u16 cel_id   = 0;
        hal::u16 duration = 0;
    };

    const hal::u8* base = nullptr;
    std::size_t total_size = 0;

    hal::u16 n_anim = 0;
    hal::u16 n_cel  = 0;
    hal::u16 n_oam  = 0;
    hal::u16 n_tile = 0;
    hal::u16 n_pal  = 0;
    hal::u16 n_unk  = 0;
    hal::u16 n_seq  = 0;

    std::size_t off_anim = 0;
    std::size_t off_cel  = 0;
    std::size_t off_oam  = 0;
    std::size_t off_tile = 0;
    std::size_t off_pal  = 0;
    std::size_t off_seq  = 0;

    bool valid() const { return base != nullptr && n_anim > 0 && n_cel > 0 && n_oam > 0; }
    bool parse(const hal::u8* data, std::size_t len);

    Anim anim(hal::u16 idx) const;
    Cel  cel(hal::u16 idx) const;
    Oam  oam(hal::u16 idx) const;
    Seq  seq(hal::u16 idx) const;
    hal::u16 cel_for_anim(hal::u16 anim_id, hal::u32 tick) const;

    const hal::u8* tile_ptr(hal::u32 tile_idx) const;
    const hal::u8* pal_ptr(hal::u16 pal_idx) const;
};

struct Metatile2x3 {
    std::array<hal::u16, 6> tiles{0x93FFu, 0x93FFu, 0x93FFu, 0x93FFu, 0x93FFu, 0x93FFu};
};

// Per-channel affine transform (out_5bit = clamp(round(scale * in_5bit + offset), 0, 31))
// modeling FoMT's outdoor time-of-day / weather / fade palette blending (func_03000C2C).
struct PaletteTint {
    std::array<float, 3> scale{1.0f, 1.0f, 1.0f};
    std::array<float, 3> offset{0.0f, 0.0f, 0.0f};
    bool is_identity = true;

    hal::u16 apply(hal::u16 bgr555) const;
};

// Recolor Pete's overalls/hat for co-op player index (0=original blue, 1=crimson, 2=emerald, 3=gold).
hal::u16 recolor_coop_bgr555(hal::u16 bgr555, hal::u8 player_idx);

// Recover affine PaletteTint from comparing raw ROM 16-color palette bytes against live PALRAM bytes.
PaletteTint fit_palette_tint(
    const hal::u8* raw_pal_32b,
    const hal::u8* live_pal_32b,
    float* out_mse = nullptr);

// Holds decoded official assets from US Harvest Moon: FoMT ROM (Title Screen, Pete, Items, Farm Map[0])
class FomtRomAssets {
public:
    // Known US FoMT ROM offsets (gUnk_* symbols in StanHash/fomt)
    static constexpr std::size_t TITLE_SPRITE_ARCH_OFF = 0x747A74u;
    static constexpr std::size_t TITLE_BG_TILES_OFF    = 0x74A9C0u;
    static constexpr std::size_t TITLE_BG_PAL_OFF      = 0x74E330u;
    static constexpr std::size_t TITLE_BG_MAP_PTRS_OFF = 0x0E8684u;
    static constexpr std::size_t TITLE_CURSOR_ARCH_OFF = 0x74E530u;

    static constexpr hal::u16 HUD_OPAQUE_FONT_BASE = 128u;
    static constexpr hal::u16 HUD_OPAQUE_FILL_TILE = 224u;
    static constexpr hal::u16 HUD_CREAM_FONT_BASE  = 225u;

    static constexpr std::size_t FARMER_ARCH_OFF       = 0x58BA28u;
    static constexpr std::size_t ITEM_ICON_ARCH_OFF    = 0x6678A0u;

    static constexpr std::size_t MAP_TABLE_OFF         = 0x105EDCu;
    static constexpr std::size_t CROP_TABLE_OFF        = 0x0E93F8u;
    static constexpr std::size_t SOIL_TILES_NORM_OFF   = 0x6D5508u; // Spring/Summer/Fall -> 0x06005FC0
    static constexpr std::size_t SOIL_TILES_WIN_OFF    = 0x6D6698u; // Winter -> 0x06005FC0
    static constexpr std::size_t SOIL_PAL_NORM_OFF     = 0x6D6368u; // 0x60B -> pal[8..10]
    static constexpr std::size_t SOIL_PAL_WIN_OFF      = 0x6D6548u; // 0x60B -> pal[8..10]

    static constexpr std::size_t CROP_TILES_SPR_OFF    = 0x69A0A4u; // 0x11E0B -> 0x06006E20
    static constexpr std::size_t CROP_TILES_SUM_OFF    = 0x697920u;
    static constexpr std::size_t CROP_TILES_FAL_OFF    = 0x698E14u;
    static constexpr std::size_t CROP_PAL_SPR_OFF      = 0x699FF4u; // 0x80B -> pal[11..14]
    static constexpr std::size_t CROP_PAL_SUM_OFF      = 0x69B284u;
    static constexpr std::size_t CROP_PAL_FAL_OFF      = 0x698B00u;

    static constexpr hal::u32 FARM_MAP_W = 128u;
    static constexpr hal::u32 FARM_MAP_H = 88u;
    // Pixel/tile origin of the 43x25 FarmWorld field inside Map[0]
    static constexpr hal::u32 FIELD_ORIGIN_TX = 34u;
    static constexpr hal::u32 FIELD_ORIGIN_TY = 22u;
    static constexpr hal::i32 FIELD_ORIGIN_PX = static_cast<hal::i32>(FIELD_ORIGIN_TX * 8u);
    static constexpr hal::i32 FIELD_ORIGIN_PY = static_cast<hal::i32>(FIELD_ORIGIN_TY * 8u);

    bool load_from_rom(const hal::u8* rom, std::size_t rom_size);
    void clear();
    bool loaded() const { return loaded_; }

    // Title Screen hardware upload helpers
    void upload_title_vram_and_pal(hal::GbaBus& bus) const;
    void write_title_oam(
        hal::GbaBus& bus,
        hal::u32& oam_slot,
        hal::u16 anim_id,
        hal::i16 center_x,
        hal::i16 anchor_y,
        hal::u8 prio,
        hal::u32 tick,
        hal::i8 fixed_pal_bank = -1) const;
    void write_title_cursor_oam(
        hal::GbaBus& bus,
        hal::u32& oam_slot,
        hal::i16 anchor_x,
        hal::i16 anchor_y,
        hal::u32 tick,
        hal::u8 prio = 0) const;

    // Gameplay Map[0] hardware upload & seasonal swap helpers
    void upload_gameplay_map_base(hal::GbaBus& bus, Season season) const;
    void upload_season_overlays(hal::GbaBus& bus, Season season) const;

    // Builds composited 128x88 Farm layers with current world farm_grid tiles
    void compose_farm_layers(
        const FarmWorld& world,
        std::vector<hal::u16>& out_t1,
        std::vector<hal::u16>& out_t2,
        std::vector<hal::u16>& out_t3) const;

    // Dynamic OAM sprite streaming helpers for Gameplay (Pete + Tool Icons)
    static hal::u16 select_pete_anim(Facing facing, bool moving, hal::u8 equipped_tool, hal::u8 tool_timer);
    bool write_pete_oam(
        hal::GbaBus& bus,
        hal::u32& oam_slot,
        hal::u32& vram_tile_cursor,
        hal::u8 player_idx,
        hal::i16 screen_x,
        hal::i16 screen_y,
        hal::u16 anim_id,
        hal::u32 tick,
        hal::u8 prio = 1,
        const PaletteTint* tint = nullptr,
        hal::i8 pal_bank_override = -1) const;
    bool write_item_icon_oam(
        hal::GbaBus& bus,
        hal::u32& oam_slot,
        hal::u32& vram_tile_cursor,
        hal::u16 icon_id,
        hal::i16 screen_x,
        hal::i16 screen_y,
        hal::u8 pal_bank = 4,
        hal::u8 prio = 0,
        const PaletteTint* tint = nullptr) const;
    bool held_item_icon_id(hal::u32 held_raw, hal::u16& out_icon_id) const;

    const SpriteArchiveView& title_arch() const { return title_arch_; }
    const SpriteArchiveView& farmer_arch() const { return farmer_arch_; }
    const SpriteArchiveView& item_arch() const { return item_arch_; }

private:
    Metatile2x3 crop_obstacle_metatile(const FarmTile& tile) const;

    bool loaded_ = false;
    const hal::u8* rom_ptr_ = nullptr;
    std::size_t rom_size_ = 0;

    // Title Screen assets
    std::vector<hal::u8> title_arch_buf_;
    SpriteArchiveView title_arch_{};
    SpriteArchiveView cursor_arch_{};
    std::vector<hal::u8> title_bg_tiles_;
    std::array<hal::u8, 512> title_bg_pal_{};
    std::array<std::vector<hal::u8>, 4> title_bg_maps_;

    // Gameplay Map[0] (Spring/Summer/Fall) & Map[1] (Winter) static assets
    std::vector<hal::u8> map_img_;
    std::vector<hal::u8> map_pal_;
    std::vector<hal::u16> map_t1_;
    std::vector<hal::u16> map_t2_;
    std::vector<hal::u16> map_t3_;

    std::vector<hal::u8> map_win_img_;
    std::vector<hal::u8> map_win_pal_;
    std::vector<hal::u16> map_win_t1_;
    std::vector<hal::u16> map_win_t2_;
    std::vector<hal::u16> map_win_t3_;

    // Pre-parsed ROM Soil / Crop metatile & seasonal palettes/tiles slices
    std::array<hal::u16, 4> meta_untilled_{};
    std::array<hal::u16, 4> meta_tilled_{};
    std::array<hal::u16, 4> meta_watered_{};
    Metatile2x3 meta_stump_{};
    struct CropEntry {
        std::size_t rom_frames_off = 0;
        std::array<hal::u8, 5> stages{0, 255, 255, 255, 255};
    };
    std::array<CropEntry, 25> crop_entries_{};

    SpriteArchiveView farmer_arch_{};
    SpriteArchiveView item_arch_{};
};

} // namespace hm::game
