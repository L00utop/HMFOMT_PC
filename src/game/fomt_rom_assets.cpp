#include "fomt_rom_assets.hpp"

#include "hal/gba_bus.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace hm::game {

namespace {

constexpr hal::u16 read_le16(const hal::u8* p) {
    return static_cast<hal::u16>(p[0] | (static_cast<hal::u16>(p[1]) << 8));
}

constexpr hal::u32 read_le32(const hal::u8* p) {
    return static_cast<hal::u32>(p[0])
         | (static_cast<hal::u32>(p[1]) << 8)
         | (static_cast<hal::u32>(p[2]) << 16)
         | (static_cast<hal::u32>(p[3]) << 24);
}

constexpr int OAM_W_TBL[3][4] = {
    { 8, 16, 32, 64},
    {16, 32, 32, 64},
    { 8,  8, 16, 32},
};
constexpr int OAM_H_TBL[3][4] = {
    { 8, 16, 32, 64},
    { 8,  8, 16, 32},
    {16, 32, 32, 64},
};

} // namespace

hal::u16 PaletteTint::apply(hal::u16 c) const {
    if (is_identity) {
        return c;
    }
    hal::u16 out = 0;
    for (int ch = 0; ch < 3; ++ch) {
        const float x = static_cast<float>((c >> (ch * 5)) & 31u);
        const float v = std::clamp(scale[static_cast<std::size_t>(ch)] * x
                                 + offset[static_cast<std::size_t>(ch)], 0.0f, 31.0f);
        const int iv = std::clamp(static_cast<int>(std::lround(v)), 0, 31);
        out |= static_cast<hal::u16>(static_cast<hal::u16>(iv) << (ch * 5));
    }
    return out;
}

hal::u16 recolor_coop_bgr555(hal::u16 c, hal::u8 player_idx) {
    if (player_idx == 0) {
        return c;
    }
    const hal::u16 r = c & 31u;
    const hal::u16 g = (c >> 5) & 31u;
    const hal::u16 b = (c >> 10) & 31u;
    // Remap Pete's blue overalls while preserving skin tones, dark outlines, and highlights
    if (b >= r + 4u && b >= g + 2u) {
        hal::u16 nr = r;
        hal::u16 ng = g;
        hal::u16 nb = b;
        if (player_idx == 1u) { // P2 Crimson
            nr = b;
            ng = r;
            nb = g;
        } else if (player_idx == 2u) { // P3 Emerald
            nr = r;
            ng = b;
            nb = g;
        } else { // P4 Gold / Violet accent
            nr = b;
            ng = static_cast<hal::u16>((b * 3u) / 4u);
            nb = r;
        }
        return static_cast<hal::u16>((nr & 31u) | ((ng & 31u) << 5) | ((nb & 31u) << 10));
    }
    return c;
}

PaletteTint fit_palette_tint(
    const hal::u8* raw_pal_32b,
    const hal::u8* live_pal_32b,
    float* out_mse)
{
    PaletteTint tint{};
    if (!raw_pal_32b || !live_pal_32b) {
        if (out_mse) *out_mse = 1e9f;
        return tint;
    }
    if (std::memcmp(raw_pal_32b + 2, live_pal_32b + 2, 30u) == 0) {
        if (out_mse) *out_mse = 0.0f;
        return tint;
    }

    float total_sq_err = 0.0f;
    bool near_identity = true;
    for (int ch = 0; ch < 3; ++ch) {
        const int shift = ch * 5;
        float sum_x = 0.0f;
        float sum_y = 0.0f;
        for (int i = 1; i < 16; ++i) {
            const hal::u16 rc = read_le16(raw_pal_32b + i * 2) & 0x7FFFu;
            const hal::u16 lc = read_le16(live_pal_32b + i * 2) & 0x7FFFu;
            sum_x += static_cast<float>((rc >> shift) & 31u);
            sum_y += static_cast<float>((lc >> shift) & 31u);
        }
        const float mx = sum_x / 15.0f;
        const float my = sum_y / 15.0f;
        float sxx = 0.0f;
        float sxy = 0.0f;
        for (int i = 1; i < 16; ++i) {
            const hal::u16 rc = read_le16(raw_pal_32b + i * 2) & 0x7FFFu;
            const hal::u16 lc = read_le16(live_pal_32b + i * 2) & 0x7FFFu;
            const float dx = static_cast<float>((rc >> shift) & 31u) - mx;
            const float dy = static_cast<float>((lc >> shift) & 31u) - my;
            sxx += dx * dx;
            sxy += dx * dy;
        }
        float a = 1.0f;
        if (sxx > 1e-3f) {
            a = std::clamp(sxy / sxx, 0.0f, 2.0f);
        }
        const float b = my - a * mx;
        tint.scale[static_cast<std::size_t>(ch)] = a;
        tint.offset[static_cast<std::size_t>(ch)] = b;
        if (std::fabs(a - 1.0f) >= 0.02f || std::fabs(b) >= 0.35f) {
            near_identity = false;
        }
        for (int i = 1; i < 16; ++i) {
            const hal::u16 rc = read_le16(raw_pal_32b + i * 2) & 0x7FFFu;
            const hal::u16 lc = read_le16(live_pal_32b + i * 2) & 0x7FFFu;
            const float x = static_cast<float>((rc >> shift) & 31u);
            const float y = static_cast<float>((lc >> shift) & 31u);
            const float pred = std::clamp(a * x + b, 0.0f, 31.0f);
            const float diff = pred - y;
            total_sq_err += diff * diff;
        }
    }
    tint.is_identity = near_identity;
    if (out_mse) {
        *out_mse = total_sq_err / 45.0f;
    }
    return tint;
}

namespace {

void copy_pal_to_gba(
    hal::u8* dst_pal,
    const hal::u8* src_pal,
    hal::u8 recolor_player = 0,
    const PaletteTint* tint = nullptr)
{
    if (!src_pal) {
        return;
    }
    const bool has_tint = (tint != nullptr && !tint->is_identity);
    if (recolor_player == 0 && !has_tint) {
        std::memcpy(dst_pal, src_pal, 32u);
        return;
    }
    for (int i = 0; i < 16; ++i) {
        if (i == 0) {
            dst_pal[0] = src_pal[0];
            dst_pal[1] = src_pal[1];
            continue;
        }
        hal::u16 c = recolor_coop_bgr555(read_le16(src_pal + i * 2), recolor_player);
        if (has_tint) {
            c = tint->apply(c);
        }
        dst_pal[i * 2 + 0] = static_cast<hal::u8>(c & 0xFFu);
        dst_pal[i * 2 + 1] = static_cast<hal::u8>(c >> 8);
    }
}

void write_oam_raw(hal::GbaBus& bus, hal::u32 slot, hal::u16 a0, hal::u16 a1, hal::u16 a2) {
    if (slot >= 128u) {
        return;
    }
    const hal::u32 base = hal::ADDR_OAM_BASE + slot * 8u;
    bus.write16(base + 0u, a0);
    bus.write16(base + 2u, a1);
    bus.write16(base + 4u, a2);
    bus.write16(base + 6u, 0u);
}

void stream_cel_to_oam(
    hal::GbaBus& bus,
    const SpriteArchiveView& arch,
    const SpriteArchiveView::Cel& cel,
    hal::u32& oam_slot,
    hal::u32& vram_tile_cursor,
    hal::i16 screen_x,
    hal::i16 screen_y,
    hal::u8 base_pal_bank,
    hal::u8 prio,
    bool two_bank_mode,
    hal::u8 recolor_player,
    const PaletteTint* tint = nullptr)
{
    if (!arch.valid() || cel.oam_n == 0) {
        return;
    }

    // Upload the primary palette (and optional +1 bank when prel != 0)
    if (const hal::u8* p0 = arch.pal_ptr(cel.pal_i)) {
        copy_pal_to_gba(
            bus.pal_ram() + 512u + static_cast<std::size_t>(base_pal_bank & 0xFu) * 32u,
            p0, recolor_player, tint);
    }
    bool uploaded_second_pal = false;

    for (hal::u16 k = 0; k < cel.oam_n && oam_slot < 128u; ++k) {
        const SpriteArchiveView::Oam o = arch.oam(static_cast<hal::u16>(cel.oam_i + k));
        const hal::i32 y_rel = static_cast<hal::i8>(o.attr0 & 0xFFu);
        const hal::i32 x_rel = static_cast<hal::i32>(static_cast<hal::u32>(o.attr1 & 0x1FFu) << 23) >> 23;
        const hal::i32 sx = static_cast<hal::i32>(screen_x) + x_rel;
        const hal::i32 sy = static_cast<hal::i32>(screen_y) + y_rel;

        const hal::u32 sh = (o.attr0 >> 14) & 3u;
        const hal::u32 sz = (o.attr1 >> 14) & 3u;
        if (sh > 2u) {
            continue;
        }
        const int sw = OAM_W_TBL[sh][sz];
        const int sh_px = OAM_H_TBL[sh][sz];
        if (sx + sw <= 0 || sx >= static_cast<hal::i32>(hal::SCREEN_WIDTH) ||
            sy + sh_px <= 0 || sy >= static_cast<hal::i32>(hal::SCREEN_HEIGHT)) {
            continue;
        }

        const bool bpp8 = (o.attr0 & 0x2000u) != 0;
        const hal::u32 tw = static_cast<hal::u32>(sw / 8);
        const hal::u32 th = static_cast<hal::u32>(sh_px / 8);
        const hal::u32 n_blocks = tw * th;
        const hal::u32 tiles_needed = bpp8 ? (n_blocks * 2u) : n_blocks;

        if (bpp8 && (vram_tile_cursor & 1u) != 0u) {
            ++vram_tile_cursor;
        }
        if (vram_tile_cursor + tiles_needed > 1024u) {
            break;
        }

        const hal::u32 trel = o.attr2 & 0x3FFu;
        const hal::u32 src_tile_idx = static_cast<hal::u32>(cel.tile_i) + trel;
        if (src_tile_idx + tiles_needed > arch.n_tile) {
            continue;
        }
        const hal::u8* src_tiles = arch.tile_ptr(src_tile_idx);
        if (!src_tiles) {
            continue;
        }
        std::memcpy(
            bus.vram() + 0x10000u + static_cast<std::size_t>(vram_tile_cursor) * 32u,
            src_tiles,
            static_cast<std::size_t>(tiles_needed) * 32u);

        const hal::u8 prel = static_cast<hal::u8>((o.attr2 >> 12) & 0xFu);
        hal::u8 dst_pal = base_pal_bank;
        if (prel != 0) {
            if (two_bank_mode) {
                dst_pal = static_cast<hal::u8>((base_pal_bank + 1u) & 0xFu);
                if (!uploaded_second_pal) {
                    if (const hal::u8* p1 = arch.pal_ptr(static_cast<hal::u16>(cel.pal_i + prel))) {
                        copy_pal_to_gba(
                            bus.pal_ram() + 512u + static_cast<std::size_t>(dst_pal) * 32u,
                            p1, recolor_player, tint);
                    }
                    uploaded_second_pal = true;
                }
            }
        }

        const hal::u16 a0 = static_cast<hal::u16>((o.attr0 & 0xFC00u) | (static_cast<hal::u16>(sy) & 0xFFu));
        const hal::u16 a1 = static_cast<hal::u16>((o.attr1 & 0xFE00u) | (static_cast<hal::u16>(sx) & 0x1FFu));
        const hal::u16 a2 = static_cast<hal::u16>((vram_tile_cursor & 0x3FFu)
                                                | ((static_cast<hal::u16>(prio) & 3u) << 10)
                                                | (static_cast<hal::u16>(dst_pal & 0xFu) << 12));
        write_oam_raw(bus, oam_slot++, a0, a1, a2);
        vram_tile_cursor += tiles_needed;
    }
}

} // namespace

bool unpack_fomt(const hal::u8* rom, std::size_t rom_size, std::size_t offset, std::vector<hal::u8>& out) {
    if (!rom || offset + 8u > rom_size || (offset & 3u) != 0u) {
        return false;
    }
    const hal::u32 hdr = read_le32(rom + offset);
    const hal::u32 out_size = hdr >> 8;
    if (out_size == 0 || out_size > 256u * 1024u) {
        return false;
    }

    std::size_t off = offset + 4u;
    hal::u32 word = 0;
    hal::i32 rem_bits = 0;
    bool stream_oob = false;

    auto read_bits = [&](int n) -> hal::u32 {
        if (n <= 0) {
            return 0;
        }
        const hal::u32 hi = (n >= 32) ? word : (word >> (32 - n));
        rem_bits -= n;
        if (rem_bits >= 0) {
            word = (n >= 32) ? 0u : (word << n);
            return hi;
        }
        if (off + 4u > rom_size) {
            stream_oob = true;
            return 0;
        }
        word = read_le32(rom + off);
        off += 4u;
        const hal::u32 need = static_cast<hal::u32>(-rem_bits);
        rem_bits += 32;
        const hal::u32 lo = word >> rem_bits;
        word = (need >= 32u) ? 0u : (word << need);
        return hi | lo;
    };

    auto next_bit = [&]() -> bool {
        --rem_bits;
        if (rem_bits < 0) {
            if (off + 4u > rom_size) {
                stream_oob = true;
                return false;
            }
            word = read_le32(rom + off);
            off += 4u;
            rem_bits += 32;
        }
        const bool b = (word >> 31) != 0;
        word <<= 1;
        return b;
    };

    const hal::u8 flags = static_cast<hal::u8>(read_bits(8));
    if (stream_oob) {
        return false;
    }

    std::vector<hal::i16> huff_tree;
    auto build_huff = [&](int width) -> bool {
        const std::size_t init_nodes = static_cast<std::size_t>(1u << width);
        huff_tree.assign(init_nodes * 2u + 2048u, -1);
        hal::u32 next_off = 0x20u;
        hal::u32 code = 0;
        const int max_len = width * 2;
        for (int len_idx = 0; len_idx < max_len; ++len_idx) {
            code <<= 1;
            const hal::u32 count = read_bits(width);
            if (stream_oob) {
                return false;
            }
            for (hal::u32 k = 0; k < count; ++k) {
                hal::u32 node_off = 0x20u;
                for (int b = len_idx; b >= 1; --b) {
                    const hal::u32 bit = (code >> b) & 1u;
                    const hal::u32 slot_off = node_off + bit * 2u;
                    const std::size_t idx = (slot_off - 0x20u) / 2u;
                    if (idx >= huff_tree.size()) {
                        return false;
                    }
                    hal::i16& entry = huff_tree[idx];
                    if (entry < 0) {
                        next_off += 4u;
                        entry = static_cast<hal::i16>(next_off);
                    }
                    node_off = static_cast<hal::u32>(entry);
                }
                const hal::u32 leaf_off = node_off + (code & 1u) * 2u;
                const std::size_t leaf_idx = (leaf_off - 0x20u) / 2u;
                if (leaf_idx >= huff_tree.size()) {
                    return false;
                }
                const hal::u32 sym = read_bits(width);
                if (stream_oob) {
                    return false;
                }
                huff_tree[leaf_idx] = static_cast<hal::i16>(~sym);
                ++code;
            }
        }
        return true;
    };

    auto decode_huff = [&]() -> hal::u8 {
        hal::u32 node_off = 0x20u;
        for (int guard = 0; guard < 64; ++guard) {
            const hal::u32 bit = next_bit() ? 1u : 0u;
            if (stream_oob) {
                return 0;
            }
            const std::size_t idx = (node_off - 0x20u) / 2u + bit;
            if (idx >= huff_tree.size()) {
                stream_oob = true;
                return 0;
            }
            const hal::i16 val = huff_tree[idx];
            if (val < 0) {
                return static_cast<hal::u8>(~val);
            }
            node_off = static_cast<hal::u32>(val);
        }
        stream_oob = true;
        return 0;
    };

    const int huff_mode = (flags >> 3) & 3;
    if (huff_mode == 1) {
        if (!build_huff(4)) return false;
    } else if (huff_mode == 2) {
        if (!build_huff(8)) return false;
    } else if (huff_mode != 0) {
        return false;
    }

    auto read_sym = [&]() -> hal::u8 {
        if (huff_mode == 1) {
            const hal::u8 hi = decode_huff();
            const hal::u8 lo = decode_huff();
            return static_cast<hal::u8>((hi << 4) | lo);
        }
        if (huff_mode == 2) {
            return decode_huff();
        }
        return static_cast<hal::u8>(read_bits(8));
    };

    struct DistEntry {
        hal::u16 base;
        hal::u8  bits;
    };
    auto read_dist_table = [&](int count) -> std::vector<DistEntry> {
        std::vector<DistEntry> tbl(static_cast<std::size_t>(count));
        hal::u16 base = 1;
        for (int i = 0; i < count; ++i) {
            const hal::u8 w = static_cast<hal::u8>(read_bits(4) + 1u);
            tbl[static_cast<std::size_t>(i)] = {base, w};
            base = static_cast<hal::u16>(base + (1u << w));
        }
        return tbl;
    };

    out.clear();
    out.reserve(out_size + 16u);

    const int lz_mode = flags & 7;
    if (lz_mode == 4) {
        while (out.size() < out_size && !stream_oob) {
            out.push_back(read_sym());
        }
    } else if (lz_mode == 0) {
        const auto tbl = read_dist_table(2);
        while (out.size() < out_size && !stream_oob) {
            const hal::u32 tag = read_bits(2);
            if (tag < 2u) {
                const hal::u32 dist = tbl[tag].base + read_bits(tbl[tag].bits);
                const hal::u32 len = read_bits(6) + 3u;
                for (hal::u32 i = 0; i < len && out.size() < out_size; ++i) {
                    out.push_back(dist <= out.size() ? out[out.size() - dist] : 0u);
                }
            } else if (tag == 2u) {
                const hal::u32 len = read_bits(6) + 1u;
                for (hal::u32 i = 0; i < len && out.size() < out_size && !stream_oob; ++i) {
                    out.push_back(read_sym());
                }
            } else {
                const hal::u32 len = read_bits(6) + 1u;
                const hal::u8 b = static_cast<hal::u8>(read_bits(8));
                for (hal::u32 i = 0; i < len && out.size() < out_size; ++i) {
                    out.push_back(b);
                }
            }
        }
    } else if (lz_mode == 1) {
        const auto tbl = read_dist_table(4);
        while (out.size() < out_size && !stream_oob) {
            if (next_bit()) {
                const hal::u32 tag = read_bits(2);
                const hal::u32 dist = tbl[tag].base + read_bits(tbl[tag].bits);
                const hal::u32 len = read_bits(4) + 3u;
                for (hal::u32 i = 0; i < len && out.size() < out_size; ++i) {
                    out.push_back(dist <= out.size() ? out[out.size() - dist] : 0u);
                }
            } else {
                out.push_back(read_sym());
            }
        }
    } else if (lz_mode == 2) {
        const auto tbl = read_dist_table(7);
        while (out.size() < out_size && !stream_oob) {
            if (!next_bit()) {
                out.push_back(read_sym());
                continue;
            }
            const hal::u32 tag = read_bits(3);
            if (tag < 7u) {
                const hal::u32 dist = tbl[tag].base + read_bits(tbl[tag].bits);
                const hal::u32 len = read_bits(4) + 3u;
                for (hal::u32 i = 0; i < len && out.size() < out_size; ++i) {
                    out.push_back(dist <= out.size() ? out[out.size() - dist] : 0u);
                }
            } else {
                hal::u32 len_hi = 0;
                for (int guard = 0; guard < 16 && !stream_oob; ++guard) {
                    const hal::u32 nib = read_bits(4);
                    len_hi = (len_hi << 4) + nib;
                    const bool carry = (len_hi & 1u) != 0;
                    len_hi >>= 1;
                    if (!carry) break;
                }
                if (next_bit()) {
                    const hal::u32 t2 = read_bits(3);
                    if (t2 >= 7u) return false;
                    const hal::u32 dist = tbl[t2].base + read_bits(tbl[t2].bits);
                    const hal::u32 len = (len_hi << 4) + read_bits(4) + 3u;
                    for (hal::u32 i = 0; i < len && out.size() < out_size; ++i) {
                        out.push_back(dist <= out.size() ? out[out.size() - dist] : 0u);
                    }
                } else {
                    const hal::u32 len = len_hi + 1u;
                    for (hal::u32 i = 0; i < len && out.size() < out_size && !stream_oob; ++i) {
                        out.push_back(read_sym());
                    }
                }
            }
        }
    } else if (lz_mode == 3) {
        const auto tbl = read_dist_table(3);
        while (out.size() + 1u < out_size && !stream_oob) {
            if (!next_bit()) {
                out.push_back(read_sym());
                out.push_back(read_sym());
                continue;
            }
            const hal::u32 tag = read_bits(2);
            if (tag < 3u) {
                const hal::u32 dist_w = tbl[tag].base + read_bits(tbl[tag].bits);
                const hal::u32 len_w = read_bits(3) + 2u;
                const hal::u32 dist_b = dist_w * 2u;
                for (hal::u32 i = 0; i < len_w && out.size() + 1u < out_size; ++i) {
                    const hal::u8 b0 = dist_b <= out.size() ? out[out.size() - dist_b] : 0u;
                    out.push_back(b0);
                    const hal::u8 b1 = dist_b <= out.size() ? out[out.size() - dist_b] : 0u;
                    out.push_back(b1);
                }
            } else {
                hal::u32 len_hi = 0;
                for (int guard = 0; guard < 16 && !stream_oob; ++guard) {
                    const hal::u32 v = read_bits(3);
                    len_hi = (len_hi << 3) + v;
                    const bool carry = (len_hi & 1u) != 0;
                    len_hi >>= 1;
                    if (!carry) break;
                }
                if (next_bit()) {
                    const hal::u32 t2 = read_bits(2);
                    if (t2 >= 3u) return false;
                    const hal::u32 dist_w = tbl[t2].base + read_bits(tbl[t2].bits);
                    const hal::u32 len_w = (len_hi << 3) + read_bits(3) + 2u;
                    const hal::u32 dist_b = dist_w * 2u;
                    for (hal::u32 i = 0; i < len_w && out.size() + 1u < out_size; ++i) {
                        const hal::u8 b0 = dist_b <= out.size() ? out[out.size() - dist_b] : 0u;
                        out.push_back(b0);
                        const hal::u8 b1 = dist_b <= out.size() ? out[out.size() - dist_b] : 0u;
                        out.push_back(b1);
                    }
                } else {
                    const hal::u32 len_w = len_hi + 1u;
                    for (hal::u32 i = 0; i < len_w && out.size() + 1u < out_size && !stream_oob; ++i) {
                        out.push_back(read_sym());
                        out.push_back(read_sym());
                    }
                }
            }
        }
    } else {
        return false;
    }

    if (stream_oob || out.size() < out_size) {
        return false;
    }
    if (out.size() > out_size) {
        out.resize(out_size);
    }

    const int filt = (flags >> 5) & 7;
    if (filt == 1) {
        hal::u16 acc = 0;
        for (std::size_t i = 0; i + 1u < out.size(); i += 2u) {
            const hal::u16 w = read_le16(&out[i]);
            const hal::u16 n1 = ((w >> 4) + acc) & 0xFu;
            const hal::u16 n0 = (w + n1) & 0xFu;
            const hal::u16 n3 = ((w >> 12) + n0) & 0xFu;
            const hal::u16 n2 = ((w >> 8) + n3) & 0xFu;
            acc = n2;
            const hal::u16 res = static_cast<hal::u16>(n0 | (n1 << 4) | (n2 << 8) | (n3 << 12));
            out[i] = static_cast<hal::u8>(res & 0xFFu);
            out[i + 1] = static_cast<hal::u8>(res >> 8);
        }
    } else if (filt == 2) {
        hal::u8 acc = 0;
        for (std::size_t i = 0; i < out.size(); ++i) {
            acc = static_cast<hal::u8>(acc + out[i]);
            out[i] = acc;
        }
    } else if (filt == 3) {
        hal::u16 acc = 0;
        for (std::size_t i = 0; i + 1u < out.size(); i += 2u) {
            const hal::u16 w = read_le16(&out[i]);
            acc = static_cast<hal::u16>(acc + w);
            out[i] = static_cast<hal::u8>(acc & 0xFFu);
            out[i + 1] = static_cast<hal::u8>(acc >> 8);
        }
    } else if (filt == 4) {
        hal::u8 acc0 = 0;
        hal::u8 acc1 = 0;
        for (std::size_t i = 0; i + 1u < out.size(); i += 2u) {
            acc0 = static_cast<hal::u8>(acc0 + out[i]);
            acc1 = static_cast<hal::u8>(acc1 + out[i + 1]);
            out[i] = acc0;
            out[i + 1] = acc1;
        }
    }
    return true;
}

bool SpriteArchiveView::parse(const hal::u8* data, std::size_t len) {
    *this = SpriteArchiveView{};
    if (!data || len < 28u) {
        return false;
    }

    std::size_t p = 0;
    auto take_section = [&](hal::u16& count, std::size_t& sec_off, std::size_t elem_size) -> bool {
        if (p + 4u > len) {
            return false;
        }
        count = read_le16(data + p);
        p += 4u;
        sec_off = p;
        const std::size_t bytes = static_cast<std::size_t>(count) * elem_size;
        if (p + bytes > len) {
            return false;
        }
        p += bytes;
        return true;
    };

    std::size_t dummy_unk_off = 0;
    if (!take_section(n_anim, off_anim, 4u)) return false;
    if (!take_section(n_cel,  off_cel,  16u)) return false;
    if (!take_section(n_oam,  off_oam,  8u)) return false;
    if (!take_section(n_tile, off_tile, 32u)) return false;
    if (!take_section(n_pal,  off_pal,  32u)) return false;
    if (!take_section(n_unk,  dummy_unk_off, 8u)) return false;
    if (!take_section(n_seq,  off_seq,  4u)) return false;

    base = data;
    total_size = len;
    return valid();
}

SpriteArchiveView::Anim SpriteArchiveView::anim(hal::u16 idx) const {
    if (!base || idx >= n_anim) return {};
    const hal::u8* p = base + off_anim + static_cast<std::size_t>(idx) * 4u;
    return {read_le16(p), read_le16(p + 2u)};
}

SpriteArchiveView::Cel SpriteArchiveView::cel(hal::u16 idx) const {
    if (!base || idx >= n_cel) return {};
    const hal::u8* p = base + off_cel + static_cast<std::size_t>(idx) * 16u;
    return {
        read_le16(p + 0u), read_le16(p + 2u), read_le16(p + 4u), read_le16(p + 6u),
        read_le16(p + 8u), read_le16(p + 10u), read_le16(p + 12u), read_le16(p + 14u)
    };
}

SpriteArchiveView::Oam SpriteArchiveView::oam(hal::u16 idx) const {
    if (!base || idx >= n_oam) return {};
    const hal::u8* p = base + off_oam + static_cast<std::size_t>(idx) * 8u;
    return {read_le16(p + 0u), read_le16(p + 2u), read_le16(p + 4u), read_le16(p + 6u)};
}

SpriteArchiveView::Seq SpriteArchiveView::seq(hal::u16 idx) const {
    if (!base || idx >= n_seq) return {};
    const hal::u8* p = base + off_seq + static_cast<std::size_t>(idx) * 4u;
    return {read_le16(p + 0u), read_le16(p + 2u)};
}

hal::u16 SpriteArchiveView::cel_for_anim(hal::u16 anim_id, hal::u32 tick) const {
    const Anim a = anim(anim_id);
    if (a.n_frames == 0 || a.seq0 >= n_seq) {
        return 0;
    }
    const hal::u16 max_frames = static_cast<hal::u16>(std::min<hal::u32>(a.n_frames, n_seq - a.seq0));
    hal::u32 total_dur = 0;
    for (hal::u16 i = 0; i < max_frames; ++i) {
        const Seq s = seq(static_cast<hal::u16>(a.seq0 + i));
        total_dur += std::max<hal::u32>(1u, s.duration);
    }
    if (total_dur == 0) {
        return seq(a.seq0).cel_id;
    }
    hal::u32 rem = tick % total_dur;
    for (hal::u16 i = 0; i < max_frames; ++i) {
        const Seq s = seq(static_cast<hal::u16>(a.seq0 + i));
        const hal::u32 d = std::max<hal::u32>(1u, s.duration);
        if (rem < d) {
            return s.cel_id;
        }
        rem -= d;
    }
    return seq(a.seq0).cel_id;
}

const hal::u8* SpriteArchiveView::tile_ptr(hal::u32 tile_idx) const {
    if (!base || tile_idx >= n_tile) return nullptr;
    return base + off_tile + static_cast<std::size_t>(tile_idx) * 32u;
}

const hal::u8* SpriteArchiveView::pal_ptr(hal::u16 pal_idx) const {
    if (!base || pal_idx >= n_pal) return nullptr;
    return base + off_pal + static_cast<std::size_t>(pal_idx) * 32u;
}

void FomtRomAssets::clear() {
    loaded_ = false;
    rom_ptr_ = nullptr;
    rom_size_ = 0;
    title_arch_buf_.clear();
    title_arch_ = SpriteArchiveView{};
    title_bg_tiles_.clear();
    title_bg_pal_.fill(0);
    for (auto& m : title_bg_maps_) m.clear();
    map_img_.clear();
    map_pal_.clear();
    map_t1_.clear();
    map_t2_.clear();
    map_t3_.clear();
    map_win_img_.clear();
    map_win_pal_.clear();
    map_win_t1_.clear();
    map_win_t2_.clear();
    map_win_t3_.clear();
    cursor_arch_ = SpriteArchiveView{};
    farmer_arch_ = SpriteArchiveView{};
    item_arch_ = SpriteArchiveView{};
}

bool FomtRomAssets::load_from_rom(const hal::u8* rom, std::size_t rom_size) {
    clear();
    if (!rom || rom_size < 0x800000u) {
        return false;
    }
    rom_ptr_ = rom;
    rom_size_ = rom_size;

    // 1. Title screen archive, BG tiles, BG palette, and 4 split BG maps
    if (!unpack_fomt(rom, rom_size, TITLE_SPRITE_ARCH_OFF, title_arch_buf_) ||
        !title_arch_.parse(title_arch_buf_.data(), title_arch_buf_.size())) {
        clear();
        return false;
    }
    if (!unpack_fomt(rom, rom_size, TITLE_BG_TILES_OFF, title_bg_tiles_)) {
        clear();
        return false;
    }
    if (TITLE_BG_PAL_OFF + title_bg_pal_.size() > rom_size) {
        clear();
        return false;
    }
    std::memcpy(title_bg_pal_.data(), rom + TITLE_BG_PAL_OFF, title_bg_pal_.size());

    for (std::size_t i = 0; i < 4u; ++i) {
        const hal::u32 ptr = read_le32(rom + TITLE_BG_MAP_PTRS_OFF + i * 4u);
        if (ptr < hal::ADDR_ROM_BASE) {
            clear();
            return false;
        }
        const std::size_t off = ptr - hal::ADDR_ROM_BASE;
        if (!unpack_fomt(rom, rom_size, off, title_bg_maps_[i])) {
            clear();
            return false;
        }
    }

    // 2. Farm Map[0] (Spring/Summer/Fall) & Map[1] (Winter snowy terrain)
    const hal::u32 img_p  = read_le32(rom + MAP_TABLE_OFF + 0x00u);
    const hal::u32 pal_p  = read_le32(rom + MAP_TABLE_OFF + 0x04u);
    const hal::u32 t1_p   = read_le32(rom + MAP_TABLE_OFF + 0x0Cu);
    const hal::u32 t2_p   = read_le32(rom + MAP_TABLE_OFF + 0x10u);
    const hal::u32 t3_p   = read_le32(rom + MAP_TABLE_OFF + 0x14u);
    if (img_p < hal::ADDR_ROM_BASE || pal_p < hal::ADDR_ROM_BASE || t1_p < hal::ADDR_ROM_BASE) {
        clear();
        return false;
    }

    std::vector<hal::u8> raw_t1;
    std::vector<hal::u8> raw_t2;
    std::vector<hal::u8> raw_t3;
    if (!unpack_fomt(rom, rom_size, img_p - hal::ADDR_ROM_BASE, map_img_) ||
        !unpack_fomt(rom, rom_size, pal_p - hal::ADDR_ROM_BASE, map_pal_) ||
        !unpack_fomt(rom, rom_size, t1_p  - hal::ADDR_ROM_BASE, raw_t1)) {
        clear();
        return false;
    }
    if (t2_p >= hal::ADDR_ROM_BASE) {
        unpack_fomt(rom, rom_size, t2_p - hal::ADDR_ROM_BASE, raw_t2);
    }
    if (t3_p >= hal::ADDR_ROM_BASE) {
        unpack_fomt(rom, rom_size, t3_p - hal::ADDR_ROM_BASE, raw_t3);
    }

    if (map_img_.size() < 0x8000u) map_img_.resize(0x8000u, 0);
    if (map_pal_.size() < 512u) map_pal_.resize(512u, 0);

    const std::size_t map_cells = FARM_MAP_W * FARM_MAP_H;
    auto decode_layer = [map_cells](const std::vector<hal::u8>& raw, std::vector<hal::u16>& out) {
        out.assign(map_cells, 0x03FFu);
        const std::size_t n = std::min(map_cells, raw.size() / 2u);
        for (std::size_t i = 0; i < n; ++i) {
            out[i] = read_le16(&raw[i * 2u]);
        }
    };
    decode_layer(raw_t1, map_t1_);
    decode_layer(raw_t2, map_t2_);
    decode_layer(raw_t3, map_t3_);

    // Map[1] (Winter Farm: +40 bytes in gUnk_08105EDC)
    const std::size_t win_off = MAP_TABLE_OFF + 40u;
    const hal::u32 wimg_p = read_le32(rom + win_off + 0x00u);
    const hal::u32 wpal_p = read_le32(rom + win_off + 0x04u);
    const hal::u32 wt1_p  = read_le32(rom + win_off + 0x0Cu);
    const hal::u32 wt2_p  = read_le32(rom + win_off + 0x10u);
    const hal::u32 wt3_p  = read_le32(rom + win_off + 0x14u);
    if (wimg_p >= hal::ADDR_ROM_BASE && wpal_p >= hal::ADDR_ROM_BASE && wt1_p >= hal::ADDR_ROM_BASE) {
        std::vector<hal::u8> wraw_t1, wraw_t2, wraw_t3;
        if (unpack_fomt(rom, rom_size, wimg_p - hal::ADDR_ROM_BASE, map_win_img_) &&
            unpack_fomt(rom, rom_size, wpal_p - hal::ADDR_ROM_BASE, map_win_pal_) &&
            unpack_fomt(rom, rom_size, wt1_p  - hal::ADDR_ROM_BASE, wraw_t1)) {
            if (wt2_p >= hal::ADDR_ROM_BASE) unpack_fomt(rom, rom_size, wt2_p - hal::ADDR_ROM_BASE, wraw_t2);
            if (wt3_p >= hal::ADDR_ROM_BASE) unpack_fomt(rom, rom_size, wt3_p - hal::ADDR_ROM_BASE, wraw_t3);
            if (map_win_img_.size() < 0x8000u) map_win_img_.resize(0x8000u, 0);
            if (map_win_pal_.size() < 512u) map_win_pal_.resize(512u, 0);
            decode_layer(wraw_t1, map_win_t1_);
            decode_layer(wraw_t2, map_win_t2_);
            decode_layer(wraw_t3, map_win_t3_);
        }
    }

    // 3. Title Cursor, Farmer & Item Icon uncompressed SpriteArchives
    if (!cursor_arch_.parse(rom + TITLE_CURSOR_ARCH_OFF, rom_size - TITLE_CURSOR_ARCH_OFF) ||
        !farmer_arch_.parse(rom + FARMER_ARCH_OFF, rom_size - FARMER_ARCH_OFF) ||
        !item_arch_.parse(rom + ITEM_ICON_ARCH_OFF, rom_size - ITEM_ICON_ARCH_OFF)) {
        clear();
        return false;
    }

    // 4. Soil & Stump metatiles
    for (int i = 0; i < 4; ++i) {
        meta_untilled_[static_cast<std::size_t>(i)] = read_le16(rom + 0x6D6518u + i * 2u);
        meta_tilled_[static_cast<std::size_t>(i)]   = read_le16(rom + 0x6D6520u + i * 2u);
        meta_watered_[static_cast<std::size_t>(i)]  = read_le16(rom + 0x6D6528u + i * 2u);
    }
    for (int i = 0; i < 6; ++i) {
        meta_stump_.tiles[static_cast<std::size_t>(i)] = read_le16(rom + 0x6D6530u + i * 2u);
    }

    // 5. gUnk_080E93F8[0..24] crop/obstacle visual table
    for (std::size_t k = 0; k < crop_entries_.size(); ++k) {
        const std::size_t eoff = CROP_TABLE_OFF + k * 12u;
        const hal::u32 ptr = read_le32(rom + eoff);
        crop_entries_[k].rom_frames_off = (ptr >= hal::ADDR_ROM_BASE) ? (ptr - hal::ADDR_ROM_BASE) : 0u;
        for (std::size_t s = 0; s < 5u; ++s) {
            crop_entries_[k].stages[s] = rom[eoff + 4u + s];
        }
    }

    loaded_ = true;
    return true;
}

void FomtRomAssets::upload_title_vram_and_pal(hal::GbaBus& bus) const {
    if (!loaded_) {
        return;
    }

    // Upload BG tiles (CharBlock 0) & BG palette
    const std::size_t bg_tile_bytes = std::min<std::size_t>(title_bg_tiles_.size(), 0x8000u);
    if (bg_tile_bytes > 0) {
        std::memcpy(bus.vram(), title_bg_tiles_.data(), bg_tile_bytes);
    }
    std::memcpy(bus.pal_ram(), title_bg_pal_.data(), title_bg_pal_.size());

    // Upload 4 64x32 BG screenmaps (split into 2 consecutive 32x32 screenblocks per layer at 24, 26, 28, 30)
    for (std::size_t layer = 0; layer < 4u; ++layer) {
        const std::size_t sb_base = (24u + layer * 2u) * 0x800u;
        std::memset(bus.vram() + sb_base, 0, 0x1000u);
        const auto& src = title_bg_maps_[layer];
        if (src.size() < 64u * 32u * 2u) {
            continue;
        }
        hal::u8* dst = bus.vram() + sb_base;
        for (hal::u32 y = 0; y < 32u; ++y) {
            for (hal::u32 x = 0; x < 64u; ++x) {
                const hal::u32 blk = (x >= 32u) ? 1u : 0u;
                const std::size_t s_idx = static_cast<std::size_t>(y * 64u + x) * 2u;
                const std::size_t d_idx = static_cast<std::size_t>(blk * 1024u + y * 32u + (x & 31u)) * 2u;
                dst[d_idx + 0u] = src[s_idx + 0u];
                dst[d_idx + 1u] = src[s_idx + 1u];
            }
        }
    }

    // Upload all Title sprite tiles into OBJ VRAM (0x06010000)
    const std::size_t obj_tile_bytes = std::min<std::size_t>(static_cast<std::size_t>(title_arch_.n_tile) * 32u, 0x8000u);
    if (obj_tile_bytes > 0 && title_arch_.base) {
        std::memcpy(bus.vram() + 0x10000u, title_arch_.base + title_arch_.off_tile, obj_tile_bytes);
    }

    // Title ta.pal[0..15] -> OBJ PAL 0..15 (includes 8bpp logo indices 0xA0..0xFF);
    // Title ta.pal[16..18] (4bpp Start/Continue/Copyright buttons) -> unused OBJ PAL 5..7
    const std::size_t pal_low_bytes = std::min<std::size_t>(static_cast<std::size_t>(title_arch_.n_pal) * 32u, 512u);
    if (pal_low_bytes > 0 && title_arch_.base) {
        std::memcpy(bus.pal_ram() + 512u, title_arch_.base + title_arch_.off_pal, pal_low_bytes);
    }
    if (title_arch_.n_pal > 16u && title_arch_.base) {
        const hal::u16 extra_pals = static_cast<hal::u16>(std::min<hal::u16>(3u, title_arch_.n_pal - 16u));
        std::memcpy(bus.pal_ram() + 512u + 5u * 32u, title_arch_.pal_ptr(16u), static_cast<std::size_t>(extra_pals) * 32u);
    }

    // Title white glove cursor (gUnk_0874E530) -> OBJ VRAM tile >= 576 & OBJ PAL 8
    if (cursor_arch_.valid() && cursor_arch_.n_tile > 0u) {
        const hal::u32 cursor_tile_base = std::min<hal::u32>(std::max<hal::u32>(title_arch_.n_tile, 576u), 960u);
        const std::size_t cur_bytes = std::min<std::size_t>(
            static_cast<std::size_t>(cursor_arch_.n_tile) * 32u,
            static_cast<std::size_t>(1024u - cursor_tile_base) * 32u);
        if (const hal::u8* tp = cursor_arch_.tile_ptr(0u)) {
            std::memcpy(bus.vram() + 0x10000u + cursor_tile_base * 32u, tp, cur_bytes);
        }
        if (const hal::u8* cp = cursor_arch_.pal_ptr(0u)) {
            std::memcpy(bus.pal_ram() + 512u + 8u * 32u, cp, 32u);
        }
    }
}

void FomtRomAssets::write_title_oam(
    hal::GbaBus& bus,
    hal::u32& oam_slot,
    hal::u16 anim_id,
    hal::i16 center_x,
    hal::i16 anchor_y,
    hal::u8 prio,
    hal::u32 tick,
    hal::i8 fixed_pal_bank) const
{
    if (!loaded_ || !title_arch_.valid()) {
        return;
    }
    const hal::u16 cel_id = title_arch_.cel_for_anim(anim_id, tick);
    const SpriteArchiveView::Cel cel = title_arch_.cel(cel_id);

    for (hal::u16 k = 0; k < cel.oam_n && oam_slot < 128u; ++k) {
        const SpriteArchiveView::Oam o = title_arch_.oam(static_cast<hal::u16>(cel.oam_i + k));
        const hal::i32 y_rel = static_cast<hal::i8>(o.attr0 & 0xFFu);
        const hal::i32 x_rel = static_cast<hal::i32>(static_cast<hal::u32>(o.attr1 & 0x1FFu) << 23) >> 23;
        const hal::i32 sx = static_cast<hal::i32>(center_x) + x_rel;
        const hal::i32 sy = static_cast<hal::i32>(anchor_y) + y_rel;

        if (sy <= -64 || sy >= static_cast<hal::i32>(hal::SCREEN_HEIGHT)) {
            continue;
        }

        const hal::u16 trel = o.attr2 & 0x3FFu;
        const hal::u8 prel = static_cast<hal::u8>((o.attr2 >> 12) & 0xFu);
        const hal::u16 abs_tile = static_cast<hal::u16>((cel.tile_i + trel) & 0x3FFu);
        const bool bpp8 = (o.attr0 & 0x2000u) != 0;

        hal::u8 dst_pal = 0;
        if (bpp8) {
            dst_pal = 0;
        } else if (fixed_pal_bank >= 0) {
            dst_pal = static_cast<hal::u8>(fixed_pal_bank & 0xFu);
        } else {
            const hal::u16 src_pal = static_cast<hal::u16>(cel.pal_i + prel);
            if (src_pal >= 16u && src_pal < 19u) {
                dst_pal = static_cast<hal::u8>((src_pal - 16u) + 5u);
            } else {
                dst_pal = static_cast<hal::u8>(src_pal & 0xFu);
            }
        }

        const hal::u16 a0 = static_cast<hal::u16>((o.attr0 & 0xFC00u) | (static_cast<hal::u16>(sy) & 0xFFu));
        const hal::u16 a1 = static_cast<hal::u16>((o.attr1 & 0xFE00u) | (static_cast<hal::u16>(sx) & 0x1FFu));
        const hal::u16 a2 = static_cast<hal::u16>(abs_tile
                                                | ((static_cast<hal::u16>(prio) & 3u) << 10)
                                                | (static_cast<hal::u16>(dst_pal) << 12));
        write_oam_raw(bus, oam_slot++, a0, a1, a2);
    }
}

void FomtRomAssets::write_title_cursor_oam(
    hal::GbaBus& bus,
    hal::u32& oam_slot,
    hal::i16 anchor_x,
    hal::i16 anchor_y,
    hal::u32 tick,
    hal::u8 prio) const
{
    if (!loaded_ || !cursor_arch_.valid()) {
        return;
    }
    const hal::u32 cursor_tile_base = std::min<hal::u32>(std::max<hal::u32>(title_arch_.n_tile, 576u), 960u);
    const hal::u16 cel_id = cursor_arch_.cel_for_anim(0u, tick);
    const SpriteArchiveView::Cel cel = cursor_arch_.cel(cel_id);
    for (hal::u16 k = 0; k < cel.oam_n && oam_slot < 128u; ++k) {
        const SpriteArchiveView::Oam o = cursor_arch_.oam(static_cast<hal::u16>(cel.oam_i + k));
        const hal::i32 y_rel = static_cast<hal::i8>(o.attr0 & 0xFFu);
        const hal::i32 x_rel = static_cast<hal::i32>(static_cast<hal::u32>(o.attr1 & 0x1FFu) << 23) >> 23;
        const hal::i32 sx = static_cast<hal::i32>(anchor_x) + x_rel;
        const hal::i32 sy = static_cast<hal::i32>(anchor_y) + y_rel;
        if (sy <= -32 || sy >= static_cast<hal::i32>(hal::SCREEN_HEIGHT)) {
            continue;
        }
        const hal::u16 abs_tile = static_cast<hal::u16>((cursor_tile_base + cel.tile_i + (o.attr2 & 0x3FFu)) & 0x3FFu);
        const hal::u16 a0 = static_cast<hal::u16>((o.attr0 & 0xFC00u) | (static_cast<hal::u16>(sy) & 0xFFu));
        const hal::u16 a1 = static_cast<hal::u16>((o.attr1 & 0xFE00u) | (static_cast<hal::u16>(sx) & 0x1FFu));
        const hal::u16 a2 = static_cast<hal::u16>(abs_tile
                                                | ((static_cast<hal::u16>(prio) & 3u) << 10)
                                                | (8u << 12));
        write_oam_raw(bus, oam_slot++, a0, a1, a2);
    }
}

void FomtRomAssets::upload_gameplay_map_base(hal::GbaBus& bus, Season season) const {
    if (!loaded_) {
        return;
    }

    // Preserve procedural ASCII font (0..95) + SOLID_FILL (96) + UI_FRAME (97..105) into CharBlock 2 (0x06008000)
    // and UI palette (BG pal 2) into BG pal 0 (unused by Map[0], preserving backdrop color 0).
    std::memcpy(bus.vram() + 0x8000u, bus.vram(), 106u * 32u);

    // Build opaque HUD font (128..223: dark espresso idx 6 bg + white idx 2 text),
    // solid HUD bar tile (224: all idx 6), and opaque cream dialog font (225..320: cream idx 1 bg + dark idx 3 text).
    for (hal::u16 g = 0; g < 96u; ++g) {
        const hal::u8* src_tile = bus.vram() + 0x8000u + static_cast<std::size_t>(g) * 32u;
        hal::u8* dst_hud   = bus.vram() + 0x8000u + static_cast<std::size_t>(HUD_OPAQUE_FONT_BASE + g) * 32u;
        hal::u8* dst_cream = bus.vram() + 0x8000u + static_cast<std::size_t>(HUD_CREAM_FONT_BASE + g) * 32u;
        for (std::size_t b = 0; b < 32u; ++b) {
            const hal::u8 px = src_tile[b];
            const hal::u8 lo = px & 0x0Fu;
            const hal::u8 hi = (px >> 4) & 0x0Fu;
            dst_hud[b]   = static_cast<hal::u8>((lo == 0 ? 6u : lo) | ((hi == 0 ? 6u : hi) << 4));
            dst_cream[b] = static_cast<hal::u8>((lo == 0 ? 1u : 3u) | ((hi == 0 ? 1u : 3u) << 4));
        }
    }
    std::memset(bus.vram() + 0x8000u + static_cast<std::size_t>(HUD_OPAQUE_FILL_TILE) * 32u, 0x66, 32u);

    std::array<hal::u8, 32> ui_pal{};
    std::memcpy(ui_pal.data(), bus.pal_ram() + 2u * 32u, 32u);
    auto write_ui_col = [&](std::size_t idx, hal::u16 bgr555) {
        ui_pal[idx * 2u]     = static_cast<hal::u8>(bgr555 & 0xFFu);
        ui_pal[idx * 2u + 1] = static_cast<hal::u8>(bgr555 >> 8);
    };
    write_ui_col(6u, hal::rgb8_to_bgr555(36u, 28u, 22u));   // Deep espresso HUD panel fill
    write_ui_col(7u, hal::rgb8_to_bgr555(116u, 76u, 42u));  // Warm wood divider accent

    // Apply base Farm Map tiles/palettes (Map[0] vs Map[1] in Winter) + seasonal soil & crop overlays
    upload_season_overlays(bus, season);

    // Put HUD palette in BG pal 0 (unused by Map[0]/Map[1]), keeping backdrop color 0 dark/neutral
    ui_pal[0] = 0;
    ui_pal[1] = 0;
    std::memcpy(bus.pal_ram(), ui_pal.data(), 32u);
}

void FomtRomAssets::upload_season_overlays(hal::GbaBus& bus, Season season) const {
    if (!loaded_ || !rom_ptr_) {
        return;
    }

    const bool use_winter_map = (season == Season::Winter && !map_win_img_.empty() && !map_win_pal_.empty());
    const std::vector<hal::u8>& base_img = use_winter_map ? map_win_img_ : map_img_;
    const std::vector<hal::u8>& base_pal = use_winter_map ? map_win_pal_ : map_pal_;
    if (!base_img.empty()) {
        std::memcpy(bus.vram(), base_img.data(), std::min<std::size_t>(base_img.size(), 0x8000u));
    }
    if (!base_pal.empty()) {
        std::memcpy(bus.pal_ram() + 0x20u, base_pal.data(), std::min<std::size_t>(base_pal.size(), 480u));
    }

    const std::size_t soil_tiles_off = (season == Season::Winter) ? SOIL_TILES_WIN_OFF : SOIL_TILES_NORM_OFF;
    const std::size_t soil_pal_off   = (season == Season::Winter) ? SOIL_PAL_WIN_OFF   : SOIL_PAL_NORM_OFF;
    if (soil_tiles_off + 0xE60u <= rom_size_) {
        std::memcpy(bus.vram() + 0x5FC0u, rom_ptr_ + soil_tiles_off, 0xE60u);
    }
    if (soil_pal_off + 0x60u <= rom_size_) {
        std::memcpy(bus.pal_ram() + 0x120u, rom_ptr_ + soil_pal_off, 0x60u);
    }

    std::size_t crop_tiles_off = CROP_TILES_SPR_OFF;
    std::size_t crop_pal_off   = CROP_PAL_SPR_OFF;
    if (season == Season::Summer) {
        crop_tiles_off = CROP_TILES_SUM_OFF;
        crop_pal_off   = CROP_PAL_SUM_OFF;
    } else if (season == Season::Fall) {
        crop_tiles_off = CROP_TILES_FAL_OFF;
        crop_pal_off   = CROP_PAL_FAL_OFF;
    }

    if (crop_tiles_off + 0x11E0u <= rom_size_) {
        std::memcpy(bus.vram() + 0x6E20u, rom_ptr_ + crop_tiles_off, 0x11E0u);
    }
    if (crop_pal_off + 0x80u <= rom_size_) {
        std::memcpy(bus.pal_ram() + 0x180u, rom_ptr_ + crop_pal_off, 0x80u);
    }
    // Spring crop palette leaves pal[15] (0x1E0) empty; fill from Summer/Fall Grass palette so Grass renders cleanly in Spring
    if (season == Season::Spring && CROP_PAL_SUM_OFF + 0x80u <= rom_size_) {
        std::memcpy(bus.pal_ram() + 0x1E0u, rom_ptr_ + CROP_PAL_SUM_OFF + 0x60u, 0x20u);
    }
}

Metatile2x3 FomtRomAssets::crop_obstacle_metatile(const FarmTile& tile) const {
    Metatile2x3 m{};
    if (!loaded_ || !rom_ptr_) {
        return m;
    }

    auto read_frame = [&](std::size_t table_idx, hal::u8 frame_idx) -> Metatile2x3 {
        Metatile2x3 res{};
        if (table_idx >= crop_entries_.size()) return res;
        const std::size_t base = crop_entries_[table_idx].rom_frames_off;
        if (base == 0) return res;
        const std::size_t off = base + static_cast<std::size_t>(frame_idx) * 12u;
        if (off + 12u > rom_size_) return res;
        for (std::size_t i = 0; i < 6u; ++i) {
            res.tiles[i] = read_le16(rom_ptr_ + off + i * 2u);
        }
        return res;
    };

    if (tile.obstacle != ObstacleKind::None) {
        switch (tile.obstacle) {
            case ObstacleKind::Boulder:
                m.tiles = {0xA303u, 0xA304u, 0xA307u, 0xA308u, 0xA30Fu, 0xA310u};
                return m;
            case ObstacleKind::Stone:   return read_frame(22u, 0u);
            case ObstacleKind::Weed:    return read_frame(21u, 0u);
            case ObstacleKind::Branch:  return read_frame(23u, 0u);
            case ObstacleKind::Stump:   return meta_stump_;
            case ObstacleKind::None:    break;
        }
        return m;
    }

    if (tile.crop != CropKind::None) {
        const hal::u8 gs = tile.growth_stage();
        if (gs == 0u) {
            // gUnk_086D6608: universal newly planted seeds on soil
            m.tiles = {0x93FFu, 0x93FFu, 0x9351u, 0x9352u, 0x9353u, 0x9354u};
            return m;
        }
        if (tile.crop == CropKind::Grass) {
            return read_frame(20u, static_cast<hal::u8>(std::min<hal::u32>(gs - 1u, 3u)));
        }
        std::size_t entry_idx = 0u;
        switch (tile.crop) {
            case CropKind::Turnip:   entry_idx = 0u;  break;
            case CropKind::Potato:   entry_idx = 1u;  break;
            case CropKind::Cucumber: entry_idx = 2u;  break;
            case CropKind::Tomato:   entry_idx = 5u;  break;
            case CropKind::Corn:     entry_idx = 6u;  break;
            case CropKind::Eggplant: entry_idx = 10u; break;
            case CropKind::Carrot:   entry_idx = 11u; break;
            case CropKind::Grass:    entry_idx = 20u; break;
            case CropKind::None:     return m;
        }
        const auto& stages = crop_entries_[entry_idx].stages;
        hal::u8 frame_idx = (stages[0] != 255u) ? stages[0] : 0u;
        if (gs >= 3u) {
            // stages[3] holds mature crop; stages[4] holds withered crop
            for (int i = 3; i >= 0; --i) {
                if (stages[static_cast<std::size_t>(i)] != 255u) {
                    frame_idx = stages[static_cast<std::size_t>(i)];
                    break;
                }
            }
        } else if (gs == 2u) {
            if (stages[2] != 255u) frame_idx = stages[2];
            else if (stages[1] != 255u) frame_idx = stages[1];
        } else if (stages[0] != 255u) {
            frame_idx = stages[0];
        }
        return read_frame(entry_idx, frame_idx);
    }

    return m;
}

void FomtRomAssets::compose_farm_layers(
    const FarmWorld& world,
    std::vector<hal::u16>& out_t1,
    std::vector<hal::u16>& out_t2,
    std::vector<hal::u16>& out_t3) const
{
    if (!loaded_) {
        return;
    }
    const bool is_winter = (world.clock.season == Season::Winter && !map_win_t1_.empty());
    out_t1 = is_winter ? map_win_t1_ : map_t1_;
    out_t2 = is_winter ? map_win_t2_ : map_t2_;
    out_t3 = is_winter ? map_win_t3_ : map_t3_;

    for (hal::u32 py = 0; py < FARM_FIELD_HEIGHT; ++py) {
        for (hal::u32 px = 0; px < FARM_FIELD_WIDTH; ++px) {
            const FarmTile& tile = world.tile_at(px, py);
            const hal::u32 tx = FIELD_ORIGIN_TX + px * 2u;
            const hal::u32 ty = FIELD_ORIGIN_TY + py * 2u;

            if (!is_winter) {
                const std::array<hal::u16, 4>* soil = &meta_untilled_;
                if (tile.soil == SoilState::Tilled) soil = &meta_tilled_;
                else if (tile.soil == SoilState::Watered) soil = &meta_watered_;

                for (hal::u32 sy = 0; sy < 2u; ++sy) {
                    for (hal::u32 sx = 0; sx < 2u; ++sx) {
                        const hal::u16 v = (*soil)[static_cast<std::size_t>(sy * 2u + sx)];
                        out_t1[static_cast<std::size_t>((ty + sy) * FARM_MAP_W + (tx + sx))] = v;
                    }
                }
            }

            const Metatile2x3 meta = crop_obstacle_metatile(tile);
            for (hal::u32 sx = 0; sx < 2u; ++sx) {
                const hal::u16 r0 = meta.tiles[sx];
                if ((r0 & 0x3FFu) != 0x3FFu && ty > 0u) {
                    out_t3[static_cast<std::size_t>((ty - 1u) * FARM_MAP_W + (tx + sx))] = r0;
                }
                const hal::u16 r1 = meta.tiles[2u + sx];
                if ((r1 & 0x3FFu) != 0x3FFu) {
                    out_t2[static_cast<std::size_t>(ty * FARM_MAP_W + (tx + sx))] = r1;
                }
                const hal::u16 r2 = meta.tiles[4u + sx];
                if ((r2 & 0x3FFu) != 0x3FFu) {
                    out_t2[static_cast<std::size_t>((ty + 1u) * FARM_MAP_W + (tx + sx))] = r2;
                }
            }
        }
    }
}

hal::u16 FomtRomAssets::select_pete_anim(Facing facing, bool moving, hal::u8 equipped_tool, hal::u8 tool_timer) {
    const hal::u16 f = static_cast<hal::u16>(facing) & 3u;
    if (tool_timer > 0) {
        switch (equipped_tool) {
            case TOOL_IRON_SICKLE:  return static_cast<hal::u16>(0x166u + f);
            case TOOL_IRON_HOE:     return static_cast<hal::u16>(0x01Au + f);
            case TOOL_IRON_AXE:     return static_cast<hal::u16>(0x1CEu + f);
            case TOOL_IRON_HAMMER:  return static_cast<hal::u16>(0x0C2u + f);
            case TOOL_WATERING_CAN: return static_cast<hal::u16>(0x09Eu + f);
            default:                return static_cast<hal::u16>(0x01Au + f);
        }
    }
    if (moving) {
        return static_cast<hal::u16>(0x14Eu + f);
    }
    return static_cast<hal::u16>(0x146u + f);
}

bool FomtRomAssets::write_pete_oam(
    hal::GbaBus& bus,
    hal::u32& oam_slot,
    hal::u32& vram_tile_cursor,
    hal::u8 player_idx,
    hal::i16 screen_x,
    hal::i16 screen_y,
    hal::u16 anim_id,
    hal::u32 tick,
    hal::u8 prio,
    const PaletteTint* tint,
    hal::i8 pal_bank_override) const
{
    if (!loaded_ || !farmer_arch_.valid() || oam_slot >= 128u) {
        return false;
    }
    const hal::u16 cel_id = farmer_arch_.cel_for_anim(anim_id, tick);
    const SpriteArchiveView::Cel cel = farmer_arch_.cel(cel_id);
    const hal::u8 pal_bank = (pal_bank_override >= 0)
        ? static_cast<hal::u8>(pal_bank_override & 0xFu)
        : static_cast<hal::u8>(8u + (player_idx & 3u) * 2u);
    stream_cel_to_oam(
        bus, farmer_arch_, cel, oam_slot, vram_tile_cursor,
        screen_x, screen_y, pal_bank, prio, true, player_idx, tint);
    return true;
}

namespace {

constexpr std::array<hal::u16, 171> FOOD_ICON_IDS = {
#define o(tag, is_drink, stam, fat, icon, name, desc) static_cast<hal::u16>(icon),
#include "data/item/food.def"
#undef o
};

constexpr std::array<hal::u16, 95> ARTICLE_ICON_IDS = {
#define o(tag, icon, name, desc) static_cast<hal::u16>(icon),
#include "data/item/article.def"
#undef o
};

} // namespace

bool FomtRomAssets::held_item_icon_id(hal::u32 held_raw, hal::u16& out_icon_id) const {
    if (!loaded_ || !item_arch_.valid()) {
        return false;
    }
    const hal::u8 kind = static_cast<hal::u8>(held_raw & 7u);
    const bool wrapped = (held_raw & 8u) != 0u;
    const hal::u16 inner_u16 = static_cast<hal::u16>((held_raw >> 16) & 0xFFFFu);
    const hal::u8 inner_id = static_cast<hal::u8>(inner_u16 & 0xFFu);

    hal::u16 icon_id = 0xFFFFu;
    switch (kind) {
        case 0u: // KIND_FOOD
            if (inner_id >= FOOD_ICON_IDS.size()) {
                return false;
            }
            icon_id = wrapped ? static_cast<hal::u16>(0x160u) : FOOD_ICON_IDS[inner_id];
            break;
        case 1u: // KIND_ARTICLE
            if (inner_id >= ARTICLE_ICON_IDS.size()) {
                return false;
            }
            icon_id = wrapped ? static_cast<hal::u16>(0x160u) : ARTICLE_ICON_IDS[inner_id];
            break;
        case 4u: // KIND_4 (Basket)
            icon_id = 0x35u;
            break;
        case 5u: // KIND_5 (Empty when 0xFFFF)
            if (inner_u16 == 0xFFFFu) {
                return false;
            }
            icon_id = inner_u16;
            break;
        default:
            return false;
    }
    if (icon_id >= item_arch_.n_anim) {
        return false;
    }
    out_icon_id = icon_id;
    return true;
}

bool FomtRomAssets::write_item_icon_oam(
    hal::GbaBus& bus,
    hal::u32& oam_slot,
    hal::u32& vram_tile_cursor,
    hal::u16 icon_id,
    hal::i16 screen_x,
    hal::i16 screen_y,
    hal::u8 pal_bank,
    hal::u8 prio,
    const PaletteTint* tint) const
{
    if (!loaded_ || !item_arch_.valid() || oam_slot >= 128u) {
        return false;
    }
    const hal::u16 cel_id = item_arch_.cel_for_anim(icon_id, 0u);
    const SpriteArchiveView::Cel cel = item_arch_.cel(cel_id);
    stream_cel_to_oam(
        bus, item_arch_, cel, oam_slot, vram_tile_cursor,
        screen_x, screen_y, pal_bank, prio, false, 0u, tint);
    return true;
}

} // namespace hm::game
