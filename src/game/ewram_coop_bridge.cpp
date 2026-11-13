#include "ewram_coop_bridge.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace hm::game {

using namespace hm::hal;
using namespace hm::net;

namespace {

constexpr u32 EWRAM_LO = ADDR_EWRAM_BASE;
constexpr u32 EWRAM_HI = ADDR_EWRAM_BASE + EWRAM_SIZE;

inline bool is_valid_ewram_ptr(u32 addr, u32 min_room = 64u) {
    return addr >= EWRAM_LO && addr + min_room <= EWRAM_HI && (addr & 3u) == 0u;
}

enum class EwramMsgKind : u8 {
    PlayerPose = 1,
    AllPoses   = 2,
    FieldChunk = 3,
    FieldDelta = 4,
    WorldChunk = 5,
    WorldDelta = 6,
};

#pragma pack(push, 1)
struct EwramPoseEntry {
    u8  active;
    u16 map_id;
    i16 x;
    i16 y;
    u8  facing;
    u16 anim_id;
    u32 held_raw;
};

struct EwramPlayerPoseMsg {
    u8  kind = static_cast<u8>(EwramMsgKind::PlayerPose);
    u8  valid_pose = 0;
    u16 map_id = 0xFFFFu;
    i16 x = 0;
    i16 y = 0;
    u8  facing = 0;
    u16 anim_id = 0xFFFFu;
    u32 held_raw = 0xFFFF0005u;
};

struct EwramAllPosesMsg {
    u8  kind = static_cast<u8>(EwramMsgKind::AllPoses);
    u8  valid_gs = 0;
    u16 host_date_u16 = 0;
    u16 host_time_u16 = 0;
    u8  valid_subframe = 0;
    u8  host_sub_frame = 0;
    u8  host_weather_today = 0;
    u8  host_weather_tomorrow = 0;
    EwramPoseEntry entries[MAX_COOP_PLAYERS]{};
};

struct EwramFieldChunkMsg {
    static constexpr std::size_t MAX_PLOTS = 128u;
    u8  kind = static_cast<u8>(EwramMsgKind::FieldChunk);
    u16 start_idx = 0;
    u16 count = 0;
    u32 plots[MAX_PLOTS]{};
};

struct EwramFieldDeltaMsg {
    u8  kind = static_cast<u8>(EwramMsgKind::FieldDelta);
    u16 index = 0;
    u32 raw_plot = 0;
};

struct EwramWorldChunkMsg {
    static constexpr std::size_t MAX_BYTES = 512u;
    u8  kind = static_cast<u8>(EwramMsgKind::WorldChunk);
    u16 shadow_offset = 0;
    u16 count = 0;
    u8  data[MAX_BYTES]{};
};

struct EwramWorldDeltaMsg {
    static constexpr std::size_t MAX_BYTES = 256u;
    u8  kind = static_cast<u8>(EwramMsgKind::WorldDelta);
    u16 shadow_offset = 0;
    u16 count = 0;
    u8  data[MAX_BYTES]{};
};
#pragma pack(pop)

bool find_world_region(std::size_t shadow_idx, u16& out_gs_off, std::size_t& out_local_off) {
    for (const auto& r : EwramCoopBridge::WORLD_REGIONS) {
        const std::size_t start = r.shadow_offset;
        const std::size_t end = start + r.size;
        if (shadow_idx >= start && shadow_idx < end) {
            out_gs_off = r.gs_offset;
            out_local_off = shadow_idx - start;
            return true;
        }
    }
    return false;
}

u8 merge_world_byte(std::size_t shadow_idx, u8 cur, u8 incoming) {
    u16 gs_off = 0;
    std::size_t loff = 0;
    if (!find_world_region(shadow_idx, gs_off, loff)) {
        return incoming;
    }
    if (gs_off == EwramCoopBridge::OFF_GS_FARM_HDR && loff == 0x11u) {
        return static_cast<u8>(incoming | (cur & 0x04u));
    }
    if (gs_off == EwramCoopBridge::OFF_GS_FARMHOUSE) {
        if (loff == 0u) {
            const u8 lvl = std::max<u8>(cur & 0x03u, incoming & 0x03u);
            return static_cast<u8>((incoming & 0xFCu) | lvl);
        }
        if (loff == 1u) {
            return static_cast<u8>(incoming | (cur & 0x7Fu));
        }
        if (loff == 3u) {
            return static_cast<u8>(incoming | (cur & 0x03u));
        }
        if (loff == 4u) {
            return static_cast<u8>(incoming | (cur & 0x02u));
        }
        if (loff == 5u) {
            return static_cast<u8>(incoming | cur);
        }
    }
    if ((gs_off == EwramCoopBridge::OFF_GS_COOP || gs_off == EwramCoopBridge::OFF_GS_BARN) && loff == 0u) {
        return static_cast<u8>(incoming | (cur & 0x01u));
    }
    return incoming;
}

u32 gs_addr_for_shadow_idx(u32 gs_ptr, std::size_t shadow_idx) {
    u16 gs_off = 0;
    std::size_t loff = 0;
    if (!find_world_region(shadow_idx, gs_off, loff)) {
        return 0u;
    }
    return gs_ptr + gs_off + static_cast<u32>(loff);
}

void read_world_from_ewram(
    const GbaBus& bus,
    u32 gs_ptr,
    std::array<u8, EwramCoopBridge::WORLD_SYNC_BYTES>& out)
{
    for (const auto& r : EwramCoopBridge::WORLD_REGIONS) {
        const u32 base = gs_ptr + r.gs_offset;
        if (!is_valid_ewram_ptr(base, r.size)) {
            continue;
        }
        for (u16 k = 0; k < r.size; ++k) {
            out[static_cast<std::size_t>(r.shadow_offset) + k] = bus.read8(base + k);
        }
    }
}

bool read_live_camera(
    const GbaBus& bus,
    u32 player_ptr,
    u32 scene_ptr,
    i32& out_cam_x,
    i32& out_cam_y)
{
    u32 go = 0u;
    if (is_valid_ewram_ptr(player_ptr, 0x14u)) {
        const u32 unk10 = bus.read32(player_ptr + 0x10u);
        if (is_valid_ewram_ptr(unk10, 0x0Cu)) {
            go = bus.read32(unk10 + 0x08u);
        }
    }
    if (!is_valid_ewram_ptr(go, 0x0Cu) &&
        is_valid_ewram_ptr(scene_ptr, EwramCoopBridge::OFF_SCENE_MAPCTRL + 4u)) {
        go = bus.read32(scene_ptr + EwramCoopBridge::OFF_SCENE_MAPCTRL);
    }
    if (!is_valid_ewram_ptr(go, 0x0Cu)) {
        return false;
    }
    const u32 cam = bus.read32(go + 0x04u);
    if (!is_valid_ewram_ptr(cam, 0x10u)) {
        return false;
    }
    out_cam_x = static_cast<i32>(bus.read32(cam + 0x08u)) >> 16;
    out_cam_y = static_cast<i32>(bus.read32(cam + 0x0Cu)) >> 16;
    return true;
}

bool oam_slot_is_free(const u8* oam, u32 slot) {
    const std::size_t off = static_cast<std::size_t>(slot) * 8u;
    const u16 a0 = static_cast<u16>(oam[off + 0u]) | (static_cast<u16>(oam[off + 1u]) << 8);
    const u16 a1 = static_cast<u16>(oam[off + 2u]) | (static_cast<u16>(oam[off + 3u]) << 8);
    const u16 a2 = static_cast<u16>(oam[off + 4u]) | (static_cast<u16>(oam[off + 5u]) << 8);
    if ((a0 & 0x0300u) == 0x0200u) return true; // Hidden
    return (a0 == 0u && a1 == 0u && a2 == 0u);
}

u32 find_max_active_oam_tile(const u8* oam) {
    static constexpr u8 TW[4][4] = {
        {1, 2, 4, 8},
        {2, 4, 4, 8},
        {1, 1, 2, 4},
        {0, 0, 0, 0},
    };
    static constexpr u8 TH[4][4] = {
        {1, 2, 4, 8},
        {1, 1, 2, 4},
        {2, 4, 4, 8},
        {0, 0, 0, 0},
    };

    u32 max_end = 0u;
    for (u32 s = 0; s < 128u; ++s) {
        if (oam_slot_is_free(oam, s)) continue;
        const std::size_t off = static_cast<std::size_t>(s) * 8u;
        const u16 a0 = static_cast<u16>(oam[off + 0u]) | (static_cast<u16>(oam[off + 1u]) << 8);
        const u16 a1 = static_cast<u16>(oam[off + 2u]) | (static_cast<u16>(oam[off + 3u]) << 8);
        const u16 a2 = static_cast<u16>(oam[off + 4u]) | (static_cast<u16>(oam[off + 5u]) << 8);
        const u32 sh = (a0 >> 14) & 3u;
        const u32 sz = (a1 >> 14) & 3u;
        if (sh > 2u) continue;
        const bool bpp8 = (a0 & 0x2000u) != 0;
        const u32 base_tile = a2 & 0x03FFu;
        const u32 n_tiles = static_cast<u32>(TW[sh][sz]) * static_cast<u32>(TH[sh][sz]) * (bpp8 ? 2u : 1u);
        max_end = std::max(max_end, base_tile + n_tiles);
    }
    return max_end;
}

struct LocalVisualProbe {
    PaletteTint tint{};
    u8 body_prio = 1u;
    bool has_shadow = false;
    u16 shadow_a0 = 0;
    u16 shadow_a1 = 0;
    u16 shadow_a2 = 0;
    i16 shadow_rel_x = 0;
    i16 shadow_rel_y = 0;
    u16 used_pal_mask = 0;
};

LocalVisualProbe probe_local_visuals(
    const GbaBus& bus,
    const FomtRomAssets& assets,
    const EwramCoopBridge::LocalSnapshot& local,
    i32 cam_x,
    i32 cam_y)
{
    static constexpr int OAM_W[3][4] = {
        { 8, 16, 32, 64},
        {16, 32, 32, 64},
        { 8,  8, 16, 32},
    };
    static constexpr int OAM_H[3][4] = {
        { 8, 16, 32, 64},
        { 8,  8, 16, 32},
        {16, 32, 32, 64},
    };

    LocalVisualProbe out{};
    const i32 local_sx = static_cast<i32>(local.x) - cam_x;
    const i32 local_sy = static_cast<i32>(local.y) - cam_y;

    const u8* raw_cands[2]{nullptr, nullptr};
    int num_raw_cands = 0;
    const SpriteArchiveView& arch = assets.farmer_arch();
    if (arch.valid()) {
        u16 anim = static_cast<u16>(local.anim_id + (local.facing & 3u));
        if (anim >= arch.n_anim) {
            anim = (local.anim_id < arch.n_anim)
                ? local.anim_id
                : FomtRomAssets::select_pete_anim(static_cast<Facing>(local.facing & 3u), false, TOOL_IRON_HOE, 0u);
        }
        const auto cel = arch.cel(arch.cel_for_anim(anim, 0u));
        if (const u8* p = arch.pal_ptr(cel.pal_i)) {
            raw_cands[num_raw_cands++] = p;
        }
        if (const u8* p0 = arch.pal_ptr(0u)) {
            if (num_raw_cands == 0 || p0 != raw_cands[0]) {
                raw_cands[num_raw_cands++] = p0;
            }
        }
    }

    const u8* oam = bus.oam();
    const u8* obj_pal = bus.pal_ram() + 512u;
    float best_body_score = 1e9f;
    u8 body_pal = 0xFFu;

    // Pass 1: Identify local Pete's palette bank, body priority, used banks, and live ambient tint.
    for (u32 s = 0; s < 128u; ++s) {
        if (oam_slot_is_free(oam, s)) continue;
        const std::size_t off = static_cast<std::size_t>(s) * 8u;
        const u16 a0 = static_cast<u16>(oam[off + 0u]) | (static_cast<u16>(oam[off + 1u]) << 8);
        const u16 a1 = static_cast<u16>(oam[off + 2u]) | (static_cast<u16>(oam[off + 3u]) << 8);
        const u16 a2 = static_cast<u16>(oam[off + 4u]) | (static_cast<u16>(oam[off + 5u]) << 8);
        const u8 bank = static_cast<u8>((a2 >> 12) & 0xFu);
        out.used_pal_mask |= static_cast<u16>(1u << bank);

        const u32 sh = (a0 >> 14) & 3u;
        const u32 sz = (a1 >> 14) & 3u;
        if (sh > 2u) continue;
        const int w = OAM_W[sh][sz];
        const int h = OAM_H[sh][sz];
        int oy = static_cast<int>(a0 & 0xFFu);
        if (oy >= 192) oy -= 256;
        int ox = static_cast<int>(a1 & 0x1FFu);
        if (ox >= 256) ox -= 512;
        const int cx = ox + w / 2;
        const int cy = oy + h / 2;
        const u32 gfx_mode = (a0 >> 10) & 3u;

        // Normal 4bpp Pete body sprite near (local_sx, local_sy - 12): fit live ambient palette tint.
        if (gfx_mode == 0u && (a0 & 0x2000u) == 0u && num_raw_cands > 0) {
            const int dx = cx - local_sx;
            if (std::abs(dx) <= 18 && cy >= local_sy - 40 && cy <= local_sy + 14) {
                const int body_dy = cy - (local_sy - 12);
                const float dist = std::sqrt(static_cast<float>(dx * dx + body_dy * body_dy));
                for (int c = 0; c < num_raw_cands; ++c) {
                    float mse = 1e9f;
                    const PaletteTint t = fit_palette_tint(
                        raw_cands[c],
                        obj_pal + static_cast<std::size_t>(bank) * 32u,
                        &mse);
                    const float score = mse + 0.02f * dist;
                    if (mse <= 8.0f && score < best_body_score) {
                        best_body_score = score;
                        body_pal = bank;
                        out.tint = t;
                        out.body_prio = static_cast<u8>((a2 >> 10) & 3u);
                    }
                }
            }
        }
    }

    // Pass 2: Find Pete's oval ground shadow directly under local Pete's feet.
    // Depending on the map, FoMT draws it either semi-transparent (mode=1) or normal (mode=0, tile<32, distinct bank).
    int best_shadow_score = 1000000;
    for (u32 s = 0; s < 128u; ++s) {
        if (oam_slot_is_free(oam, s)) continue;
        const std::size_t off = static_cast<std::size_t>(s) * 8u;
        const u16 a0 = static_cast<u16>(oam[off + 0u]) | (static_cast<u16>(oam[off + 1u]) << 8);
        const u16 a1 = static_cast<u16>(oam[off + 2u]) | (static_cast<u16>(oam[off + 3u]) << 8);
        const u16 a2 = static_cast<u16>(oam[off + 4u]) | (static_cast<u16>(oam[off + 5u]) << 8);
        if ((a0 & 0x0300u) != 0u) continue; // non-affine only
        const u8 bank = static_cast<u8>((a2 >> 12) & 0xFu);
        if (body_pal != 0xFFu && bank == body_pal) continue;

        const u32 sh = (a0 >> 14) & 3u;
        const u32 sz = (a1 >> 14) & 3u;
        if (sh > 2u) continue;
        const int w = OAM_W[sh][sz];
        const int h = OAM_H[sh][sz];
        if (w > 32 || h > 16) continue;

        const u32 gfx_mode = (a0 >> 10) & 3u;
        const u32 tile = a2 & 0x03FFu;
        if (gfx_mode != 1u && !(gfx_mode == 0u && tile < 32u)) continue;

        int oy = static_cast<int>(a0 & 0xFFu);
        if (oy >= 192) oy -= 256;
        int ox = static_cast<int>(a1 & 0x1FFu);
        if (ox >= 256) ox -= 512;
        const int cx = ox + w / 2;
        const int cy = oy + h / 2;
        const int dx = cx - local_sx;
        const int dy = cy - local_sy;
        if (std::abs(dx) <= 12 && std::abs(dy) <= 10) {
            const int score = dx * dx + (dy - 1) * (dy - 1) + (gfx_mode == 1u ? 0 : 1);
            if (score < best_shadow_score) {
                best_shadow_score = score;
                out.has_shadow = true;
                out.shadow_a0 = a0;
                out.shadow_a1 = a1;
                out.shadow_a2 = a2;
                out.shadow_rel_x = static_cast<i16>(ox - local_sx);
                out.shadow_rel_y = static_cast<i16>(oy - local_sy);
            }
        }
    }
    return out;
}

u8 alloc_pal_bank_pair(u16& used_mask, u8 player_idx) {
    for (int b = 14; b >= 2; b -= 2) {
        const u16 bits = static_cast<u16>(3u << b);
        if ((used_mask & bits) == 0u) {
            used_mask |= bits;
            return static_cast<u8>(b);
        }
    }
    return static_cast<u8>(8u + (player_idx & 3u) * 2u);
}

u8 alloc_single_pal_bank(u16& used_mask, u8 fallback_bank) {
    // Prefer odd free banks first so aligned even+odd pairs stay available for Pete bodies.
    for (int pass = 0; pass < 2; ++pass) {
        for (int b = 15; b >= 1; --b) {
            if (pass == 0 && (b & 1) == 0) continue;
            const u16 bit = static_cast<u16>(1u << b);
            if ((used_mask & bit) == 0u) {
                used_mask |= bit;
                return static_cast<u8>(b);
            }
        }
    }
    return static_cast<u8>(fallback_bank & 0xFu);
}

inline bool is_valid_rom_ptr(u32 addr) {
    return (addr & 0xFF000000u) == 0x08000000u;
}

void refresh_single_farm_plot_in_map_renderer(
    GbaBus& bus,
    u32 gs_ptr,
    u32 mr,
    u32 bg0,
    u32 bg1,
    u32 bg2,
    u32 col,
    u32 fx,
    u32 fy)
{
    constexpr u32 FARM_MAP_W = 128u;
    if (fx >= FARM_FIELD_WIDTH || fy >= FARM_FIELD_HEIGHT) {
        return;
    }

    const u32 field_base = gs_ptr + EwramCoopBridge::OFF_GS_FIELD;
    const u32 raw = bus.read32(field_base + (fy * FARM_FIELD_WIDTH + fx) * 4u);
    const u32 unk0 = raw & 0x3u;
    const u32 unk2 = (raw >> 2) & 0x3Fu;
    const u32 unk8 = (raw >> 8) & 0xFu;

    const u32 tx = 34u + fx * 2u;
    const u32 ty = 22u + fy * 2u;

    // 1. FieldPlot::method_0800AF20 -> BG0 ground/soil 2x2 tiles
    u32 bg0_rom = 0u;
    if (unk0 == 0u)      bg0_rom = 0x086D6518u;
    else if (unk0 == 1u) bg0_rom = 0x086D6520u;
    else if (unk0 == 2u) bg0_rom = 0x086D6528u;
    if (bg0_rom != 0u) {
        bus.write32(bg0 + (ty * FARM_MAP_W + tx) * 2u, bus.read32(bg0_rom + 0u));
        bus.write32(bg0 + ((ty + 1u) * FARM_MAP_W + tx) * 2u, bus.read32(bg0_rom + 4u));
        bus.write8(mr + 0x29u, 1u);
    }

    // 2. FieldPlot::method_0800AF5C -> BG2 row (ty - 1) & BG1 rows (ty, ty + 1)
    u32 overlay_rom = 0u;
    if (unk8 == 0u) {
        overlay_rom = 0x086D6458u;
    } else if (unk8 == 1u) {
        overlay_rom = 0x086D6608u;
    } else if (unk2 < 0x27u) {
        const u32 entry_addr = 0x080E93F8u + unk2 * 12u;
        const u32 base_ptr = bus.read32(entry_addr + 0u);
        u32 rel_idx = 0xFFu;
        if (unk2 != 0x14u) {
            switch (unk8) {
                case 2u: rel_idx = bus.read8(entry_addr + 4u); break;
                case 3u: rel_idx = bus.read8(entry_addr + 5u); break;
                case 4u: rel_idx = bus.read8(entry_addr + 6u); break;
                case 5u: rel_idx = bus.read8(entry_addr + 7u); break;
                case 6u: rel_idx = bus.read8(entry_addr + 8u); break;
                case 8u: rel_idx = (unk0 != 3u) ? 0u : 1u; break;
                default: break;
            }
        } else if (unk8 == 7u) {
            rel_idx = 0x0Cu;
        } else {
            switch (unk8) {
                case 3u: rel_idx = 0u; break;
                case 4u: rel_idx = 4u; break;
                case 5u: rel_idx = 8u; break;
                default: break;
            }
            if (rel_idx != 0xFFu) {
                const bool has_above = (fy > 0u);
                const bool has_below = (fy + 1u < FARM_FIELD_HEIGHT);
                const u32 above_raw = has_above ? bus.read32(field_base + ((fy - 1u) * FARM_FIELD_WIDTH + fx) * 4u) : 0u;
                const u32 below_raw = has_below ? bus.read32(field_base + ((fy + 1u) * FARM_FIELD_WIDTH + fx) * 4u) : 0u;
                auto breaks_edge = [&](bool valid, u32 neighbor) -> bool {
                    if (!valid) return true;
                    const u32 n2 = (neighbor >> 2) & 0x3Fu;
                    const u32 n8 = (neighbor >> 8) & 0xFu;
                    return n2 != 0x14u || n8 == 7u || n8 < unk8;
                };
                if (breaks_edge(has_above, above_raw)) {
                    if (!breaks_edge(has_below, below_raw)) {
                        rel_idx += 1u;
                    }
                } else {
                    rel_idx += breaks_edge(has_below, below_raw) ? 2u : 3u;
                }
            }
        }
        if (is_valid_rom_ptr(base_ptr) && rel_idx != 0xFFu) {
            overlay_rom = base_ptr + rel_idx * 12u;
        }
    }
    if (is_valid_rom_ptr(overlay_rom)) {
        bus.write32(bg2 + ((ty - 1u) * FARM_MAP_W + tx) * 2u, bus.read32(overlay_rom + 0u));
        bus.write32(bg1 + (ty * FARM_MAP_W + tx) * 2u, bus.read32(overlay_rom + 4u));
        bus.write32(bg1 + ((ty + 1u) * FARM_MAP_W + tx) * 2u, bus.read32(overlay_rom + 8u));
        bus.write8(mr + 0x29u, 1u);
    }

    // 3. FieldPlot::method_0800A07C (solid bit 0) & method_0800A0A4 (interaction id in bits 15:1 of +2)
    const bool solid = (unk8 >= 2u && unk8 <= 8u && unk2 != 0x14u);
    u16 action_id = 0u;
    if (unk8 == 8u) {
        if (unk2 >= 0x1Bu && unk2 <= 0x1Eu)      action_id = 0x3CCu;
        else if (unk2 >= 0x23u && unk2 <= 0x26u) action_id = 0x3CBu;
        else if (unk2 >= 0x1Fu && unk2 <= 0x22u) action_id = 0x3CAu;
    }

    const u32 cell_coords[4][2] = {
        {tx,      ty},
        {tx + 1u, ty},
        {tx,      ty + 1u},
        {tx + 1u, ty + 1u},
    };
    for (const auto& c : cell_coords) {
        const u32 addr = col + (c[1] * FARM_MAP_W + c[0]) * 4u;
        const u8 b0 = bus.read8(addr);
        bus.write8(addr, static_cast<u8>((b0 & ~1u) | (solid ? 1u : 0u)));
        const u16 h2 = bus.read16(addr + 2u);
        bus.write16(addr + 2u, static_cast<u16>((h2 & 1u) | ((action_id & 0x7FFFu) << 1)));
    }
}

void refresh_farm_field_plot_in_map_renderer(
    GbaBus& bus,
    u32 scene_ptr,
    u32 gs_ptr,
    std::size_t plot_idx,
    u32 prev_raw)
{
    constexpr u32 FARM_MAP_W = 128u;
    constexpr u32 FARM_MAP_H = 88u;
    constexpr u32 BG_BYTES   = FARM_MAP_W * FARM_MAP_H * 2u;
    constexpr u32 COL_BYTES  = FARM_MAP_W * FARM_MAP_H * 4u;

    if (plot_idx >= EwramCoopBridge::FIELD_PLOT_COUNT) return;
    if (!is_valid_ewram_ptr(scene_ptr, 0x10u) ||
        !is_valid_ewram_ptr(gs_ptr, EwramCoopBridge::OFF_GS_FIELD + EwramCoopBridge::FIELD_PLOT_COUNT * 4u)) {
        return;
    }
    const u32 mr = bus.read32(scene_ptr + EwramCoopBridge::OFF_SCENE_MAP_RENDERER);
    if (!is_valid_ewram_ptr(mr, 0x30u) || bus.read32(mr + 0x04u) != 0u) {
        return;
    }
    if (!is_valid_rom_ptr(bus.read32(0x080E93F8u))) {
        return;
    }
    const u32 bg0 = bus.read32(mr + 0x10u);
    const u32 bg1 = bus.read32(mr + 0x14u);
    const u32 bg2 = bus.read32(mr + 0x18u);
    const u32 col = bus.read32(mr + 0x2Cu);
    if (!is_valid_ewram_ptr(bg0, BG_BYTES) || !is_valid_ewram_ptr(bg1, BG_BYTES) ||
        !is_valid_ewram_ptr(bg2, BG_BYTES) || !is_valid_ewram_ptr(col, COL_BYTES)) {
        return;
    }

    const u32 fx = static_cast<u32>(plot_idx % FARM_FIELD_WIDTH);
    const u32 fy = static_cast<u32>(plot_idx / FARM_FIELD_WIDTH);
    const u32 cur_raw = bus.read32(gs_ptr + EwramCoopBridge::OFF_GS_FIELD + static_cast<u32>(plot_idx * 4u));
    refresh_single_farm_plot_in_map_renderer(bus, gs_ptr, mr, bg0, bg1, bg2, col, fx, fy);

    if (((prev_raw >> 2) & 0x3Fu) == 0x14u || ((cur_raw >> 2) & 0x3Fu) == 0x14u) {
        if (fy > 0u) {
            refresh_single_farm_plot_in_map_renderer(bus, gs_ptr, mr, bg0, bg1, bg2, col, fx, fy - 1u);
        }
        if (fy + 1u < FARM_FIELD_HEIGHT) {
            refresh_single_farm_plot_in_map_renderer(bus, gs_ptr, mr, bg0, bg1, bg2, col, fx, fy + 1u);
        }
    }
}

} // namespace

void EwramCoopBridge::reset() {
    cached_player_ptr_ = 0;
    last_gs_ptr_ = 0;
    tick_counter_ = 0;
    local_ = LocalSnapshot{};
    has_prev_cam_ = false;
    prev_cam_map_ = 0xFFFFu;
    prev_cam_x_ = 0;
    prev_cam_y_ = 0;
    remotes_.fill(RemoteFarmer{});
    pending_host_date_ = 0;
    pending_host_time_ = 0;
    pending_host_subframe_ = 0;
    pending_host_weather_today_ = 0;
    pending_host_weather_tomorrow_ = 0;
    has_host_subframe_ = false;
    has_host_clock_ = false;
    has_host_world_snapshot_ = false;
    pending_initial_world_apply_ = false;
    shadow_plots_.fill(0u);
    shadow_initialized_ = false;
    pending_client_plots_.fill(0u);
    pending_client_dirty_.fill(false);
    local_plot_prev_.fill(0u);
    local_plot_hold_.fill(0u);
    has_pending_client_plots_ = false;
    shadow_world_.fill(0u);
    world_shadow_initialized_ = false;
    pending_world_.fill(0u);
    pending_world_dirty_.fill(false);
    pending_world_monotonic_.fill(false);
    local_world_hold_.fill(0u);
    has_pending_world_ = false;
    for (bool& b : pending_peer_sync_) b = false;
    injected_this_frame_ = false;
    obj_vram_backup_.clear();
}

void EwramCoopBridge::bind(NetCoopSession& net) {
    net.set_domain_sync_enabled(false);
    net.set_app_handlers(
        [this, &net](u8 sender_id, const u8* payload, std::size_t len) {
            on_app_packet(net, sender_id, payload, len);
        },
        [this](u8 peer_player_id) {
            if (peer_player_id > 0 && peer_player_id < MAX_COOP_PLAYERS) {
                pending_peer_sync_[peer_player_id] = true;
            }
        });
}

EwramCoopBridge::LocalSnapshot EwramCoopBridge::resolve_live_pointers(const GbaBus& bus) {
    LocalSnapshot snap{};
    const u32 scene = bus.read32(PTR_GAME_SCENE);
    if (!is_valid_ewram_ptr(scene, 0x360u)) {
        cached_player_ptr_ = 0;
        return snap;
    }
    snap.scene_ptr = scene;

    u32 gs = bus.read32(scene + OFF_SCENE_GAMESTATE);
    if (!is_valid_ewram_ptr(gs, OFF_GS_FARMER + 0x60u)) {
        gs = bus.read32(scene + 4u);
    }
    if (!is_valid_ewram_ptr(gs, OFF_GS_FARMER + 0x60u)) {
        return snap;
    }

    snap.gs_ptr = gs;
    snap.valid_gs = true;
    snap.held_raw = bus.read32(gs + OFF_GS_FARMER + 0x54u);
    snap.date_u16 = bus.read16(gs + OFF_GS_DATE_U16);
    snap.time_u16 = bus.read16(gs + OFF_GS_TIME_U16);
    snap.weather_today = static_cast<u8>(bus.read32(gs + OFF_GS_WEATHER_TODAY) & 0xFFu);
    snap.weather_tomorrow = static_cast<u8>(bus.read32(gs + OFF_GS_WEATHER_TOMORROW) & 0xFFu);
    if (is_valid_ewram_ptr(scene, OFF_SCENE_SUBFRAME + 4u) &&
        bus.read32(scene + OFF_SCENE_CLOCK_PTR) == gs + OFF_GS_DATE_U16) {
        snap.sub_frame = bus.read8(scene + OFF_SCENE_SUBFRAME);
        snap.valid_subframe = true;
    }

    if (is_valid_ewram_ptr(scene, OFF_SCENE_MAPCTRL + 4u)) {
        const u32 scene_go = bus.read32(scene + OFF_SCENE_MAPCTRL);
        if (is_valid_ewram_ptr(scene_go, 0x0Cu)) {
            const u32 direct_pl = bus.read32(scene_go + 0x08u);
            if (is_valid_ewram_ptr(direct_pl, 0x30u) &&
                bus.read32(direct_pl + 0x14u) == PLAYER_VTABLE) {
                cached_player_ptr_ = direct_pl;
            }
        }
    }

    if (!is_valid_ewram_ptr(cached_player_ptr_, 0x30u) ||
        bus.read32(cached_player_ptr_ + 0x14u) != PLAYER_VTABLE) {
        cached_player_ptr_ = 0;
        for (u32 addr = EWRAM_LO; addr < EWRAM_HI - 0x40u; addr += 4u) {
            if (bus.read32(addr + 0x14u) == PLAYER_VTABLE) {
                cached_player_ptr_ = addr;
                break;
            }
        }
    }

    if (cached_player_ptr_ != 0u) {
        snap.player_ptr = cached_player_ptr_;
        snap.valid_player = true;
        snap.map_id = bus.read16(cached_player_ptr_ + 0x04u);
        snap.x = static_cast<i16>(static_cast<i32>(bus.read32(cached_player_ptr_ + 0x08u)) >> 16);
        snap.y = static_cast<i16>(static_cast<i32>(bus.read32(cached_player_ptr_ + 0x0Cu)) >> 16);
        snap.facing = bus.read8(cached_player_ptr_ + 0x20u);
        snap.anim_id = bus.read16(cached_player_ptr_ + 0x22u);
    }

    return snap;
}

void EwramCoopBridge::send_field_chunks_to_peer(NetCoopSession& net, u8 peer_id) const {
    if (!shadow_initialized_) return;
    for (std::size_t start = 0; start < FIELD_PLOT_COUNT; start += EwramFieldChunkMsg::MAX_PLOTS) {
        EwramFieldChunkMsg msg{};
        msg.start_idx = static_cast<u16>(start);
        msg.count = static_cast<u16>(std::min(EwramFieldChunkMsg::MAX_PLOTS, FIELD_PLOT_COUNT - start));
        for (u16 i = 0; i < msg.count; ++i) {
            msg.plots[i] = shadow_plots_[start + i];
        }
        net.send_app_to_peer(peer_id, &msg, sizeof(msg));
    }
}

void EwramCoopBridge::send_world_chunks_to_peer(NetCoopSession& net, u8 peer_id) const {
    if (!world_shadow_initialized_) return;
    for (std::size_t start = 0; start < WORLD_SYNC_BYTES; start += EwramWorldChunkMsg::MAX_BYTES) {
        EwramWorldChunkMsg msg{};
        msg.shadow_offset = static_cast<u16>(start);
        msg.count = static_cast<u16>(std::min(EwramWorldChunkMsg::MAX_BYTES, WORLD_SYNC_BYTES - start));
        std::memcpy(msg.data, shadow_world_.data() + start, msg.count);
        net.send_app_to_peer(peer_id, &msg, sizeof(msg));
    }
}

void EwramCoopBridge::broadcast_field_chunks(NetCoopSession& net) const {
    for (u8 p = 1; p < static_cast<u8>(MAX_COOP_PLAYERS); ++p) {
        send_field_chunks_to_peer(net, p);
    }
}

void EwramCoopBridge::on_app_packet(NetCoopSession& net, u8 sender_id, const u8* payload, std::size_t len) {
    if (!payload || len == 0) return;
    const auto kind = static_cast<EwramMsgKind>(payload[0]);

    if (net.role() == NetRole::Host) {
        if (sender_id == 0 || sender_id >= MAX_COOP_PLAYERS) return;
        auto relay_to_other_peers = [&](const void* data, std::size_t msg_len) {
            for (u8 p = 1u; p < static_cast<u8>(MAX_COOP_PLAYERS); ++p) {
                if (p != sender_id) {
                    net.send_app_to_peer(p, data, msg_len);
                }
            }
        };
        if (kind == EwramMsgKind::PlayerPose && len >= sizeof(EwramPlayerPoseMsg)) {
            EwramPlayerPoseMsg msg{};
            std::memcpy(&msg, payload, sizeof(msg));
            auto& rf = remotes_[sender_id];
            rf.active = (msg.valid_pose != 0u);
            rf.map_id = msg.map_id;
            rf.x = msg.x;
            rf.y = msg.y;
            rf.facing = msg.facing;
            rf.anim_id = msg.anim_id;
            rf.held_raw = msg.held_raw;
            rf.ticks_since_seen = 0;
        } else if (kind == EwramMsgKind::FieldDelta && len >= sizeof(EwramFieldDeltaMsg)) {
            EwramFieldDeltaMsg msg{};
            std::memcpy(&msg, payload, sizeof(msg));
            if (msg.index < FIELD_PLOT_COUNT) {
                pending_client_plots_[msg.index] = msg.raw_plot;
                pending_client_dirty_[msg.index] = true;
                has_pending_client_plots_ = true;
                relay_to_other_peers(&msg, sizeof(msg));
            }
        } else if (kind == EwramMsgKind::WorldDelta && len >= 5u) {
            EwramWorldDeltaMsg msg{};
            std::memcpy(&msg, payload, std::min(len, sizeof(msg)));
            const std::size_t max_payload = (len > 5u) ? (len - 5u) : 0u;
            const std::size_t cnt = std::min<std::size_t>({msg.count, EwramWorldDeltaMsg::MAX_BYTES, max_payload});
            for (std::size_t k = 0; k < cnt; ++k) {
                const std::size_t idx = static_cast<std::size_t>(msg.shadow_offset) + k;
                if (idx < WORLD_SYNC_BYTES) {
                    pending_world_[idx] = msg.data[k];
                    pending_world_dirty_[idx] = true;
                    pending_world_monotonic_[idx] = true;
                    has_pending_world_ = true;
                }
            }
            if (cnt > 0u) {
                relay_to_other_peers(payload, 5u + cnt);
            }
        }
        return;
    }

    if (net.role() == NetRole::Client) {
        if (kind == EwramMsgKind::AllPoses && len >= sizeof(EwramAllPosesMsg)) {
            EwramAllPosesMsg msg{};
            std::memcpy(&msg, payload, sizeof(msg));
            if (msg.valid_gs != 0u) {
                pending_host_date_ = msg.host_date_u16;
                pending_host_time_ = msg.host_time_u16;
                pending_host_subframe_ = msg.host_sub_frame;
                pending_host_weather_today_ = msg.host_weather_today;
                pending_host_weather_tomorrow_ = msg.host_weather_tomorrow;
                has_host_subframe_ = (msg.valid_subframe != 0u);
                has_host_clock_ = true;
            }
            const u8 my_id = net.local_player_id();
            for (std::size_t i = 0; i < MAX_COOP_PLAYERS; ++i) {
                if (i == my_id) continue;
                auto& rf = remotes_[i];
                rf.active = (msg.entries[i].active != 0u);
                rf.map_id = msg.entries[i].map_id;
                rf.x = msg.entries[i].x;
                rf.y = msg.entries[i].y;
                rf.facing = msg.entries[i].facing;
                rf.anim_id = msg.entries[i].anim_id;
                rf.held_raw = msg.entries[i].held_raw;
                rf.ticks_since_seen = 0;
            }
        } else if (kind == EwramMsgKind::FieldChunk && len >= sizeof(EwramFieldChunkMsg)) {
            EwramFieldChunkMsg msg{};
            std::memcpy(&msg, payload, sizeof(msg));
            for (u16 i = 0; i < msg.count && i < EwramFieldChunkMsg::MAX_PLOTS; ++i) {
                const std::size_t idx = static_cast<std::size_t>(msg.start_idx) + i;
                if (idx < FIELD_PLOT_COUNT) {
                    pending_client_plots_[idx] = msg.plots[i];
                    pending_client_dirty_[idx] = true;
                    has_pending_client_plots_ = true;
                }
            }
            if (!has_host_world_snapshot_) {
                pending_initial_world_apply_ = true;
            }
        } else if (kind == EwramMsgKind::FieldDelta && len >= sizeof(EwramFieldDeltaMsg)) {
            EwramFieldDeltaMsg msg{};
            std::memcpy(&msg, payload, sizeof(msg));
            if (msg.index < FIELD_PLOT_COUNT) {
                pending_client_plots_[msg.index] = msg.raw_plot;
                pending_client_dirty_[msg.index] = true;
                has_pending_client_plots_ = true;
            }
        } else if (kind == EwramMsgKind::WorldChunk && len >= sizeof(EwramWorldChunkMsg)) {
            EwramWorldChunkMsg msg{};
            std::memcpy(&msg, payload, sizeof(msg));
            for (u16 k = 0; k < msg.count && k < EwramWorldChunkMsg::MAX_BYTES; ++k) {
                const std::size_t idx = static_cast<std::size_t>(msg.shadow_offset) + k;
                if (idx < WORLD_SYNC_BYTES) {
                    pending_world_[idx] = msg.data[k];
                    pending_world_dirty_[idx] = true;
                    pending_world_monotonic_[idx] = false;
                    has_pending_world_ = true;
                }
            }
            has_host_world_snapshot_ = true;
        } else if (kind == EwramMsgKind::WorldDelta && len >= 5u) {
            EwramWorldDeltaMsg msg{};
            std::memcpy(&msg, payload, std::min(len, sizeof(msg)));
            const std::size_t max_payload = (len > 5u) ? (len - 5u) : 0u;
            const std::size_t cnt = std::min<std::size_t>({msg.count, EwramWorldDeltaMsg::MAX_BYTES, max_payload});
            for (std::size_t k = 0; k < cnt; ++k) {
                const std::size_t idx = static_cast<std::size_t>(msg.shadow_offset) + k;
                if (idx < WORLD_SYNC_BYTES) {
                    pending_world_[idx] = msg.data[k];
                    pending_world_dirty_[idx] = true;
                    pending_world_monotonic_[idx] = true;
                    has_pending_world_ = true;
                }
            }
        }
    }
}

void EwramCoopBridge::update_before_frame(GbaBus& bus, NetCoopSession& net) {
    ++tick_counter_;
    const u16 prev_date_u16 = local_.date_u16;
    local_ = resolve_live_pointers(bus);

    if (!local_.valid_gs) {
        last_gs_ptr_ = 0;
        shadow_initialized_ = false;
        world_shadow_initialized_ = false;
        local_plot_hold_.fill(0u);
        local_world_hold_.fill(0u);
    } else if (local_.gs_ptr != last_gs_ptr_ ||
               (net.role() == NetRole::Host && prev_date_u16 == 0u && local_.date_u16 != 0u)) {
        last_gs_ptr_ = local_.gs_ptr;
        shadow_initialized_ = false;
        world_shadow_initialized_ = false;
        local_plot_hold_.fill(0u);
        local_world_hold_.fill(0u);
        if (net.role() == NetRole::Host) {
            for (std::size_t p = 1; p < MAX_COOP_PLAYERS; ++p) {
                pending_peer_sync_[p] = true;
            }
        }
    }

    if (!net.is_connected()) {
        return;
    }

    for (std::size_t i = 0; i < MAX_COOP_PLAYERS; ++i) {
        if (remotes_[i].active && ++remotes_[i].ticks_since_seen > 600u) {
            remotes_[i].active = false;
        }
    }

    if (local_.valid_gs) {
        const u32 field_base = local_.gs_ptr + OFF_GS_FIELD;

        if (!shadow_initialized_) {
            for (std::size_t i = 0; i < FIELD_PLOT_COUNT; ++i) {
                shadow_plots_[i] = bus.read32(field_base + static_cast<u32>(i * 4u));
            }
            shadow_initialized_ = true;
        }
        if (!world_shadow_initialized_) {
            read_world_from_ewram(bus, local_.gs_ptr, shadow_world_);
            world_shadow_initialized_ = true;
        }

        // Step 1: Emit local GBA CPU edits BEFORE applying incoming snapshots so an in-flight chunk
        // from Host cannot erase a local plot/world change made during the frame just finished.
        if (net.role() == NetRole::Host || has_host_world_snapshot_) {
            for (std::size_t i = 0; i < FIELD_PLOT_COUNT; ++i) {
                const u32 cur = bus.read32(field_base + static_cast<u32>(i * 4u));
                if (cur != shadow_plots_[i]) {
                    local_plot_prev_[i] = shadow_plots_[i];
                    shadow_plots_[i] = cur;
                    local_plot_hold_[i] = 20u;
                    pending_client_dirty_[i] = false;
                    EwramFieldDeltaMsg msg{};
                    msg.index = static_cast<u16>(i);
                    msg.raw_plot = cur;
                    net.broadcast_app(&msg, sizeof(msg));
                }
            }

            std::array<u8, WORLD_SYNC_BYTES> live_world{};
            read_world_from_ewram(bus, local_.gs_ptr, live_world);
            for (const auto& r : WORLD_REGIONS) {
                std::size_t pos = r.shadow_offset;
                const std::size_t end = pos + r.size;
                while (pos < end) {
                    if (live_world[pos] == shadow_world_[pos]) {
                        ++pos;
                        continue;
                    }
                    const std::size_t span_start = pos;
                    std::size_t last_diff = pos;
                    ++pos;
                    while (pos < end && (pos - span_start) < EwramWorldDeltaMsg::MAX_BYTES) {
                        if (live_world[pos] != shadow_world_[pos]) {
                            last_diff = pos;
                            ++pos;
                        } else if ((pos - last_diff) <= 4u) {
                            ++pos;
                        } else {
                            break;
                        }
                    }
                    const std::size_t span_len = std::min<std::size_t>(
                        EwramWorldDeltaMsg::MAX_BYTES,
                        last_diff - span_start + 1u);
                    EwramWorldDeltaMsg msg{};
                    msg.shadow_offset = static_cast<u16>(span_start);
                    msg.count = static_cast<u16>(span_len);
                    for (std::size_t k = 0; k < span_len; ++k) {
                        const std::size_t widx = span_start + k;
                        msg.data[k] = live_world[widx];
                        if (live_world[widx] != shadow_world_[widx]) {
                            shadow_world_[widx] = live_world[widx];
                            local_world_hold_[widx] = 20u;
                            if (pending_world_dirty_[widx] && !pending_world_monotonic_[widx]) {
                                pending_world_dirty_[widx] = false;
                            }
                        }
                    }
                    net.broadcast_app(&msg, 5u + span_len);
                    pos = span_start + span_len;
                }
            }
        }

        // Step 2: Apply incoming remote field plot updates (protecting fresh local edits from stale echoes)
        if (has_pending_client_plots_) {
            for (std::size_t i = 0; i < FIELD_PLOT_COUNT; ++i) {
                if (!pending_client_dirty_[i]) continue;
                const u32 raw = pending_client_plots_[i];
                pending_client_dirty_[i] = false;
                if (local_plot_hold_[i] > 0u) {
                    if (raw == shadow_plots_[i]) {
                        local_plot_hold_[i] = 0u;
                        continue;
                    }
                    if (raw == local_plot_prev_[i]) {
                        continue;
                    }
                    local_plot_hold_[i] = 0u;
                }
                const u32 old_raw = bus.read32(field_base + static_cast<u32>(i * 4u));
                bus.write32(field_base + static_cast<u32>(i * 4u), raw);
                shadow_plots_[i] = raw;
                if (old_raw != raw) {
                    refresh_farm_field_plot_in_map_renderer(bus, local_.scene_ptr, local_.gs_ptr, i, old_raw);
                }
            }
            has_pending_client_plots_ = false;
        }
        for (u8& h : local_plot_hold_) {
            if (h > 0u) --h;
        }

        if (has_pending_world_) {
            for (std::size_t i = 0; i < WORLD_SYNC_BYTES; ++i) {
                if (!pending_world_dirty_[i]) continue;
                const bool mono = pending_world_monotonic_[i];
                const u8 inc = pending_world_[i];
                pending_world_dirty_[i] = false;
                pending_world_monotonic_[i] = false;
                if (!mono && local_world_hold_[i] > 0u) {
                    if (inc == shadow_world_[i]) {
                        local_world_hold_[i] = 0u;
                    }
                    continue;
                }
                const u32 addr = gs_addr_for_shadow_idx(local_.gs_ptr, i);
                if (addr != 0u) {
                    const u8 cur = bus.read8(addr);
                    const u8 val = mono ? merge_world_byte(i, cur, inc) : inc;
                    bus.write8(addr, val);
                    shadow_world_[i] = val;
                }
            }
            has_pending_world_ = false;
        }
        for (u8& h : local_world_hold_) {
            if (h > 0u) --h;
        }

        if (pending_initial_world_apply_) {
            has_host_world_snapshot_ = true;
            pending_initial_world_apply_ = false;
        }

        // Apply Host authoritative clock/date/weather/sub_frame cleanly when running as Client
        if (net.role() == NetRole::Client && has_host_clock_) {
            bus.write16(local_.gs_ptr + OFF_GS_DATE_U16, pending_host_date_);
            const u16 cur_time = bus.read16(local_.gs_ptr + OFF_GS_TIME_U16);
            const u16 merged = static_cast<u16>((cur_time & 0xF800u) | (pending_host_time_ & 0x07FFu));
            bus.write16(local_.gs_ptr + OFF_GS_TIME_U16, merged);
            bus.write32(local_.gs_ptr + OFF_GS_WEATHER_TODAY, static_cast<u32>(pending_host_weather_today_));
            bus.write32(local_.gs_ptr + OFF_GS_WEATHER_TOMORROW, static_cast<u32>(pending_host_weather_tomorrow_));
            if (has_host_subframe_ && local_.valid_subframe) {
                bus.write8(local_.scene_ptr + OFF_SCENE_SUBFRAME, static_cast<u8>(pending_host_subframe_ % 75u));
            }
        }
    }

    if (net.role() == NetRole::Host) {
        if (shadow_initialized_ && world_shadow_initialized_) {
            for (u8 p = 1; p < static_cast<u8>(MAX_COOP_PLAYERS); ++p) {
                if (pending_peer_sync_[p]) {
                    send_field_chunks_to_peer(net, p);
                    send_world_chunks_to_peer(net, p);
                    pending_peer_sync_[p] = false;
                }
            }
        }

        if ((tick_counter_ % 2u) == 0u) {
            EwramAllPosesMsg msg{};
            msg.valid_gs = local_.valid_gs ? 1u : 0u;
            msg.host_date_u16 = local_.date_u16;
            msg.host_time_u16 = local_.time_u16;
            msg.valid_subframe = local_.valid_subframe ? 1u : 0u;
            msg.host_sub_frame = local_.sub_frame;
            msg.host_weather_today = local_.weather_today;
            msg.host_weather_tomorrow = local_.weather_tomorrow;

            msg.entries[0].active = local_.valid_player ? 1u : 0u;
            msg.entries[0].map_id = local_.map_id;
            msg.entries[0].x = local_.x;
            msg.entries[0].y = local_.y;
            msg.entries[0].facing = local_.facing;
            msg.entries[0].anim_id = local_.anim_id;
            msg.entries[0].held_raw = local_.held_raw;

            for (std::size_t i = 1; i < MAX_COOP_PLAYERS; ++i) {
                msg.entries[i].active = remotes_[i].active ? 1u : 0u;
                msg.entries[i].map_id = remotes_[i].map_id;
                msg.entries[i].x = remotes_[i].x;
                msg.entries[i].y = remotes_[i].y;
                msg.entries[i].facing = remotes_[i].facing;
                msg.entries[i].anim_id = remotes_[i].anim_id;
                msg.entries[i].held_raw = remotes_[i].held_raw;
            }
            net.broadcast_app(&msg, sizeof(msg));
        }
    } else if (net.role() == NetRole::Client && (tick_counter_ % 2u) == 0u) {
        EwramPlayerPoseMsg msg{};
        msg.valid_pose = local_.valid_player ? 1u : 0u;
        msg.map_id = local_.map_id;
        msg.x = local_.x;
        msg.y = local_.y;
        msg.facing = local_.facing;
        msg.anim_id = local_.anim_id;
        msg.held_raw = local_.held_raw;
        net.send_app_to_host(&msg, sizeof(msg));
    }
}

void EwramCoopBridge::inject_remote_oam_before_render(
    GbaBus& bus,
    const FomtRomAssets& assets,
    u8 local_player_id)
{
    injected_this_frame_ = false;
    if (!assets.loaded() || !local_.valid_player) {
        return;
    }

    i32 raw_cam_x = std::max<i32>(0, static_cast<i32>(local_.x) - 120);
    i32 raw_cam_y = std::max<i32>(0, static_cast<i32>(local_.y) - 80);
    const bool has_ewram_cam = read_live_camera(
        bus, local_.player_ptr, local_.scene_ptr, raw_cam_x, raw_cam_y);

    i32 cam_x = raw_cam_x;
    i32 cam_y = raw_cam_y;
    if (has_ewram_cam) {
        // The ROM commits OAM/BG registers 1 frame after stepping EWRAM entities/camera:
        // when the EWRAM camera steps onto the next fine pixel before VBlank latches BG1HOFS/VOFS,
        // keep the matching previous-frame camera pixel so remote sprites stay rock-solid on the map.
        const i32 hw_fine_x = static_cast<i32>(bus.io_read16(reg::BG1HOFS) & 7u);
        const i32 hw_fine_y = static_cast<i32>(bus.io_read16(reg::BG1VOFS) & 7u);
        if (has_prev_cam_ && prev_cam_map_ == local_.map_id) {
            if ((cam_x & 7) != hw_fine_x && (prev_cam_x_ & 7) == hw_fine_x &&
                std::abs(cam_x - prev_cam_x_) <= 3) {
                cam_x = prev_cam_x_;
            }
            if ((cam_y & 7) != hw_fine_y && (prev_cam_y_ & 7) == hw_fine_y &&
                std::abs(cam_y - prev_cam_y_) <= 3) {
                cam_y = prev_cam_y_;
            }
        }
        has_prev_cam_ = true;
        prev_cam_map_ = local_.map_id;
        prev_cam_x_ = raw_cam_x;
        prev_cam_y_ = raw_cam_y;
    } else {
        has_prev_cam_ = false;
    }

    bool any_visible = false;
    for (std::size_t i = 0; i < MAX_COOP_PLAYERS; ++i) {
        if (i == local_player_id) continue;
        const auto& rf = remotes_[i];
        if (!rf.active || rf.map_id != local_.map_id) continue;
        const i32 sx = static_cast<i32>(rf.x) - cam_x;
        const i32 sy = static_cast<i32>(rf.y) - cam_y;
        if (sx >= -32 && sx < static_cast<i32>(SCREEN_WIDTH) + 32 &&
            sy >= -48 && sy < static_cast<i32>(SCREEN_HEIGHT) + 32) {
            any_visible = true;
            break;
        }
    }
    if (!any_visible) {
        return;
    }

    // Find the best contiguous run of free OAM slots (prefer trailing slots so draw order stays clean)
    u32 best_start = 128u;
    u32 best_len = 0u;
    u32 cur_start = 0u;
    u32 cur_len = 0u;
    for (u32 s = 0; s <= 128u; ++s) {
        const bool free = (s < 128u) && oam_slot_is_free(bus.oam(), s);
        if (free) {
            if (cur_len == 0u) cur_start = s;
            ++cur_len;
        } else {
            if (cur_len >= best_len) {
                best_len = cur_len;
                best_start = cur_start;
            }
            cur_len = 0u;
        }
    }
    if (best_len < 4u || best_start >= 128u) {
        return;
    }

    const u32 max_active_tile = find_max_active_oam_tile(bus.oam());
    u32 vram_tile_cursor = std::max<u32>(832u, (max_active_tile + 15u) & ~15u);
    if (vram_tile_cursor + 16u > 1024u) {
        // Fallback into tail of OBJ VRAM while keeping >=16 tiles headroom
        vram_tile_cursor = 960u;
    }
    const u32 backup_byte_start = 0x10000u + vram_tile_cursor * 32u;
    if (backup_byte_start >= VRAM_SIZE) {
        return;
    }

    // Probe local Pete's live OBJ palette tint & semi-transparent drop shadow before modifying OAM/PALRAM
    LocalVisualProbe probe = probe_local_visuals(bus, assets, local_, cam_x, cam_y);

    // Save original OAM, OBJ Palette RAM, and the upper OBJ VRAM slice
    std::memcpy(oam_backup_.data(), bus.oam(), OAM_SIZE);
    std::memcpy(obj_pal_backup_.data(), bus.pal_ram() + 512u, 512u);
    backup_vram_start_ = backup_byte_start;
    obj_vram_backup_.resize(VRAM_SIZE - backup_vram_start_);
    std::memcpy(obj_vram_backup_.data(), bus.vram() + backup_vram_start_, obj_vram_backup_.size());
    injected_this_frame_ = true;

    struct VisibleRemote {
        i32 sx = 0;
        i32 sy = 0;
    };
    std::vector<VisibleRemote> visible_remotes;

    // Pass 1: Emit all remote farmers' body OAM sprites first so lowest OAM indices draw over shadows
    u32 oam_slot = best_start;
    for (std::size_t i = 0; i < MAX_COOP_PLAYERS && oam_slot < 128u; ++i) {
        if (i == local_player_id) continue;
        const auto& rf = remotes_[i];
        if (!rf.active || rf.map_id != local_.map_id) continue;

        const i32 sx = static_cast<i32>(rf.x) - cam_x;
        const i32 sy = static_cast<i32>(rf.y) - cam_y;
        if (sx < -32 || sx >= static_cast<i32>(SCREEN_WIDTH) + 32 ||
            sy < -48 || sy >= static_cast<i32>(SCREEN_HEIGHT) + 32) {
            continue;
        }

        u16 held_icon_id = 0u;
        if (assets.held_item_icon_id(rf.held_raw, held_icon_id) && oam_slot < 128u) {
            const u8 item_pal_bank = alloc_single_pal_bank(
                probe.used_pal_mask, static_cast<u8>(6u + i));
            assets.write_item_icon_oam(
                bus,
                oam_slot,
                vram_tile_cursor,
                held_icon_id,
                static_cast<i16>(sx),
                static_cast<i16>(sy - 24),
                item_pal_bank,
                probe.body_prio,
                &probe.tint);
        }

        u16 anim = static_cast<u16>(rf.anim_id + (rf.facing & 3u));
        if (assets.farmer_arch().valid() && anim >= assets.farmer_arch().n_anim) {
            anim = (rf.anim_id < assets.farmer_arch().n_anim)
                ? rf.anim_id
                : FomtRomAssets::select_pete_anim(static_cast<Facing>(rf.facing & 3u), false, TOOL_IRON_HOE, 0u);
        }
        const u8 pal_bank = alloc_pal_bank_pair(probe.used_pal_mask, static_cast<u8>(i));
        if (assets.write_pete_oam(
                bus,
                oam_slot,
                vram_tile_cursor,
                static_cast<u8>(i),
                static_cast<i16>(sx),
                static_cast<i16>(sy),
                anim,
                tick_counter_,
                probe.body_prio,
                &probe.tint,
                static_cast<hal::i8>(pal_bank))) {
            visible_remotes.push_back(VisibleRemote{sx, sy});
        }
    }

    // Pass 2: Clone local Pete's semi-transparent ground shadow under each visible remote farmer
    if (probe.has_shadow) {
        const u8 sh_prio = std::max<u8>(
            probe.body_prio,
            static_cast<u8>((probe.shadow_a2 >> 10) & 3u));
        for (const auto& vr : visible_remotes) {
            if (oam_slot >= 128u) break;
            const i32 sh_x = vr.sx + probe.shadow_rel_x;
            const i32 sh_y = vr.sy + probe.shadow_rel_y;
            if (sh_x <= -32 || sh_x >= static_cast<i32>(SCREEN_WIDTH) ||
                sh_y <= -16 || sh_y >= static_cast<i32>(SCREEN_HEIGHT)) {
                continue;
            }
            const u16 a0 = static_cast<u16>((probe.shadow_a0 & 0xFF00u) | (static_cast<u16>(sh_y) & 0x00FFu));
            const u16 a1 = static_cast<u16>((probe.shadow_a1 & 0xFE00u) | (static_cast<u16>(sh_x) & 0x01FFu));
            const u16 a2 = static_cast<u16>((probe.shadow_a2 & 0xF3FFu) | (static_cast<u16>(sh_prio & 3u) << 10));
            const u32 base = ADDR_OAM_BASE + oam_slot * 8u;
            bus.write16(base + 0u, a0);
            bus.write16(base + 2u, a1);
            bus.write16(base + 4u, a2);
            bus.write16(base + 6u, 0u);
            ++oam_slot;
        }
    }
}

void EwramCoopBridge::restore_oam_after_render(GbaBus& bus) {
    if (!injected_this_frame_) {
        return;
    }
    std::memcpy(bus.oam(), oam_backup_.data(), OAM_SIZE);
    std::memcpy(bus.pal_ram() + 512u, obj_pal_backup_.data(), 512u);
    if (!obj_vram_backup_.empty() && backup_vram_start_ + obj_vram_backup_.size() <= VRAM_SIZE) {
        std::memcpy(bus.vram() + backup_vram_start_, obj_vram_backup_.data(), obj_vram_backup_.size());
    }
    injected_this_frame_ = false;
}

} // namespace hm::game
