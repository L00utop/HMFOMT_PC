#include "cpu/arm7tdmi.hpp"
#include "hal/gba_bus.hpp"
#include "hal/hal_audio.hpp"
#include "hal/hal_timing.hpp"
#include "ppu/ppu.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using namespace hm::cpu;
using namespace hm::hal;
using namespace hm::ppu;

namespace {

std::string find_baserom() {
    for (const char* candidate : {"baserom.gba", "../baserom.gba", "../../baserom.gba"}) {
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) {
            return candidate;
        }
    }
    return {};
}

bool write_bmp24(const std::filesystem::path& path, const u32* rgba_pixels) {
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) return false;

    const u32 w = SCREEN_WIDTH;
    const u32 h = SCREEN_HEIGHT;
    const u32 row_bytes = w * 3u;
    const u32 pixel_bytes = row_bytes * h;
    const u32 file_size = 54u + pixel_bytes;

    u8 hdr[54]{};
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = file_size & 0xFFu;
    hdr[3] = (file_size >> 8) & 0xFFu;
    hdr[4] = (file_size >> 16) & 0xFFu;
    hdr[5] = (file_size >> 24) & 0xFFu;
    hdr[10] = 54;
    hdr[14] = 40;
    hdr[18] = w & 0xFFu; hdr[19] = (w >> 8) & 0xFFu;
    hdr[22] = h & 0xFFu; hdr[23] = (h >> 8) & 0xFFu;
    hdr[26] = 1;
    hdr[28] = 24;
    ofs.write(reinterpret_cast<const char*>(hdr), 54);

    std::vector<u8> row(row_bytes);
    for (int y = static_cast<int>(h) - 1; y >= 0; --y) {
        for (u32 x = 0; x < w; ++x) {
            const u32 c = rgba_pixels[static_cast<u32>(y) * w + x];
            row[x * 3 + 0] = static_cast<u8>((c >> 16) & 0xFFu); // B
            row[x * 3 + 1] = static_cast<u8>((c >> 8) & 0xFFu);  // G
            row[x * 3 + 2] = static_cast<u8>((c >> 0) & 0xFFu);  // R
        }
        ofs.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size()));
    }
    return ofs.good();
}

std::size_t count_unique_colors(const u32* fb) {
    std::set<u32> colors;
    for (u32 i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; ++i) {
        colors.insert(fb[i]);
    }
    return colors.size();
}

u64 frame_delta(const std::vector<u32>& a, const std::vector<u32>& b) {
    u64 diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) ++diff;
    }
    return diff;
}

} // namespace

int main(int argc, char** argv) {
    const std::string rom_path = find_baserom();
    if (rom_path.empty()) {
        std::puts("test_rom_boot: baserom.gba not found, skipping.");
        return 0;
    }

    const bool dump_frames = (argc > 1);
    const std::filesystem::path dump_dir = (argc > 1) ? std::filesystem::path(argv[1]) : std::filesystem::path{};

    GbaBus bus;
    HalAudio audio;
    assert(audio.init(44100, true));
    bus.attach_audio(&audio);
    assert(bus.load_rom_file(rom_path.c_str()));
    assert(bus.save_chip() == SaveChip::Sram64K);

    Arm7Cpu cpu;
    cpu.reset(ADDR_ROM_BASE);
    Ppu ppu;
    ppu.reset();
    std::vector<u32> fb(SCREEN_WIDTH * SCREEN_HEIGHT, 0u);

    auto step_frame = [&](u16 key_pressed_mask) {
        const u16 active_low = static_cast<u16>(KEY_ALL & ~key_pressed_mask);
        bus.set_keyinput(active_low);
        FrameCallbacks cb{};
        cb.on_cycles = [&](u32 cycles) {
            u32 spent = 0;
            while (spent < cycles) {
                if (cpu.halted() && !cpu.service_irq_if_pending(bus)) {
                    const u32 rem = cycles - spent;
                    bus.step_timers(rem);
                    cpu.service_irq_if_pending(bus);
                    break;
                }
                const u32 cy = cpu.step_one(bus);
                bus.step_timers(cy);
                spent += cy;
            }
        };
        cb.on_scanline = [&](u16 y) {
            if (y == 0u) ppu.latch_affine_refs(bus);
            ppu.render_scanline(bus, y, fb.data() + static_cast<std::size_t>(y) * SCREEN_WIDTH);
        };
        HalTiming::run_gba_frame(bus, cb);
    };

    auto scan_ewram_player = [&]() -> u32 {
        for (u32 addr = 0x02000000u; addr < 0x0203FF80u; addr += 4u) {
            if (bus.read32(addr + 0x14u) == 0x080E6658u) {
                return addr;
            }
        }
        return 0u;
    };

    // Boot through crt0 -> AgbMain -> logos -> Title -> New Game / Name / Farm / Farmhouse -> Farm
    std::vector<u32> fb_logo_or_intro;
    std::vector<u32> fb_title;
    std::vector<u32> fb_after_start;
    std::vector<u32> fb_gameplay;

    const bool farm_probe = (argc > 2 && std::string(argv[2]) == "--farm");
    const int max_frames = farm_probe ? 10500 : 450;
    bool has_control = false;
    int post_ctrl_frames = 0;
    i32 ref_px = -9999, ref_py = -9999;

    for (int f = 0; f < max_frames; ++f) {
        const u32 scene_now = bus.read32(0x0300040Cu);
        const u32 pl_now = (farm_probe && scene_now >= 0x02000000u && scene_now < 0x02040000u)
                               ? scan_ewram_player()
                               : 0u;
        const i32 cur_px = (pl_now != 0u) ? (static_cast<i32>(bus.read32(pl_now + 8u)) >> 16) : -9999;
        const i32 cur_py = (pl_now != 0u) ? (static_cast<i32>(bus.read32(pl_now + 12u)) >> 16) : -9999;

        if (farm_probe && f > 5750 && pl_now != 0u && !has_control) {
            if (ref_px == -9999) {
                ref_px = cur_px;
                ref_py = cur_py;
            } else if (cur_px != ref_px || cur_py != ref_py) {
                if (f > 5900 && (cur_px < ref_px || cur_py > ref_py)) {
                    has_control = true;
                }
                ref_px = cur_px;
                ref_py = cur_py;
            }
        }

        u16 keys = 0;
        if (f == 150 || f == 151 || f == 220 || f == 221 || f == 280 || f == 281 || f == 340 || f == 341) {
            keys = KEY_START;
        } else if (farm_probe && (f == 450 || f == 451)) {
            keys = KEY_A;
        } else if (farm_probe && (f == 470 || f == 471)) {
            keys = KEY_START;
        } else if (farm_probe && (f == 490 || f == 491 || f == 580 || f == 581 || f == 660 || f == 661)) {
            keys = KEY_A;
        } else if (farm_probe && (f == 680 || f == 681)) {
            keys = KEY_START;
        } else if (farm_probe && (f == 700 || f == 701)) {
            keys = KEY_A;
        } else if (farm_probe && !has_control && f >= 740) {
            const int mod = (f - 740) % 8;
            if (mod < 2) {
                keys = KEY_A;
            } else if (f > 5900) {
                keys = KEY_DOWN;
            }
            if (f == 820 || f == 821) keys = KEY_START;
        } else if (farm_probe && has_control) {
            ++post_ctrl_frames;
            if (post_ctrl_frames <= 30) keys = KEY_DOWN;
            else if (post_ctrl_frames <= 65) keys = KEY_LEFT;
            else if (post_ctrl_frames == 75 || post_ctrl_frames == 76) keys = KEY_B;
        }

        step_frame(keys);

        if (f == 180) {
            fb_logo_or_intro = fb;
            if (dump_frames) write_bmp24(dump_dir / "boot_f180.bmp", fb_logo_or_intro.data());
        }
        if (f == 320) {
            fb_title = fb;
            if (dump_frames) write_bmp24(dump_dir / "boot_f320.bmp", fb_title.data());
        }
        if (f == 419) {
            fb_after_start = fb;
            if (dump_frames) write_bmp24(dump_dir / "boot_f420.bmp", fb_after_start.data());
        }
        if (farm_probe && has_control && post_ctrl_frames == 120) {
            const u32 gs = bus.read32(scene_now + 0x358u);
            assert(gs >= 0x02000000u && gs < 0x02040000u);
            assert(pl_now != 0u);
            if (dump_frames) {
                write_bmp24(dump_dir / "boot_farm_ctrl_120.bmp", fb.data());
            }
            break;
        }
    }

    const u32 dispcnt = bus.io_read16(reg::DISPCNT);
    const u32 ie = bus.io_read16(reg::IE);
    const u32 irq_vec = bus.read32(0x03007FFCu);
    const std::size_t colors_f180 = count_unique_colors(fb_logo_or_intro.data());
    const std::size_t colors_f320 = count_unique_colors(fb_title.data());
    const std::size_t colors_f420 = count_unique_colors(fb_after_start.data());
    const u64 delta_180_320 = frame_delta(fb_logo_or_intro, fb_title);
    const u64 delta_320_420 = frame_delta(fb_title, fb_after_start);

    // Sample audio generated by ROM's native m4a DirectSound/CGB engine
    std::vector<i16> pcm(1024 * 2, 0);
    audio.render_samples(pcm.data(), 1024);
    i64 audio_energy = 0;
    for (i16 s : pcm) {
        audio_energy += (s >= 0 ? s : -s);
    }

    std::printf(
        "test_rom_boot: insns=%llu pc=0x%08X thumb=%d cpsr=0x%08X sp=0x%08X lr=0x%08X irq_vec=0x%08X DISPCNT=0x%04X IE=0x%04X IF=0x%04X IME=0x%04X "
        "colors=(%zu,%zu,%zu) deltas=(%llu,%llu) soundcnt_x=0x%04X audio_energy=%lld\n",
        static_cast<unsigned long long>(cpu.instruction_count()),
        cpu.pc(),
        cpu.in_thumb() ? 1 : 0,
        cpu.cpsr(),
        cpu.sp(),
        cpu.lr(),
        irq_vec,
        dispcnt,
        ie,
        bus.io_read16(reg::IF),
        bus.io_read16(reg::IME),
        colors_f180,
        colors_f320,
        colors_f420,
        static_cast<unsigned long long>(delta_180_320),
        static_cast<unsigned long long>(delta_320_420),
        bus.io_read16(reg::SOUNDCNT_X),
        static_cast<long long>(audio_energy));
    std::fflush(stdout);

    assert(cpu.instruction_count() > 100000ULL);
    assert(irq_vec == 0x03000958u);
    assert((ie & IRQ_VBLANK) != 0);
    assert(colors_f320 > 8);
    assert(delta_180_320 > 500 || delta_320_420 > 500);

    // Verify in-memory coop ROM patches were applied cleanly without modifying baserom.gba on disk
    assert(bus.coop_rom_patches_applied());
    assert(bus.read16(0x08017E1Eu) == 0x46C0u);
    assert(bus.read16(0x08017E3Eu) == 0x284Bu);
    assert(bus.read16(0x08017E58u) == 0x2300u);
    {
        std::ifstream disk_ifs(rom_path, std::ios::binary);
        assert(disk_ifs.is_open());
        auto read_disk16 = [&](std::size_t off) -> u16 {
            u8 b[2]{};
            disk_ifs.seekg(static_cast<std::streamoff>(off), std::ios::beg);
            disk_ifs.read(reinterpret_cast<char*>(b), 2);
            return static_cast<u16>(b[0] | (static_cast<u16>(b[1]) << 8));
        };
        assert(read_disk16(0x017E1Eu) == 0xE0F0u);
        assert(read_disk16(0x017E3Eu) == 0x2819u);
        assert(read_disk16(0x017E58u) == 0x4653u);
    }

    // Execute the actual patched Thumb instructions at func_08017C30+clock block on Arm7Cpu:
    // verify indoor time progression, 75 frames/min day length, and indoor outdoor-visual guard!
    {
        constexpr u32 SCENE_ADDR = 0x02002000u;
        constexpr u32 GS_ADDR    = 0x02006000u;
        constexpr u32 OBJ6_ADDR  = 0x02004000u;
        constexpr u32 VTBL_ADDR  = 0x02004100u;
        constexpr u32 STUB_ADDR  = 0x02004300u; // movs r0, #0 ; bx lr
        constexpr u32 STACK_TOP  = 0x03007E00u;

        bus.write32(SCENE_ADDR + 0x1040u, GS_ADDR + 0x10u);
        bus.write32(OBJ6_ADDR + 0x14u, VTBL_ADDR);
        bus.write32(VTBL_ADDR + 0x98u, STUB_ADDR | 1u);
        bus.write16(STUB_ADDR + 0u, 0x2000u);
        bus.write16(STUB_ADDR + 2u, 0x4770u);

        auto run_clock_tick = [&](u32 sl_is_interior, u16 initial_time, u8 initial_sub, bool& hit_outdoor_pal) {
            hit_outdoor_pal = false;
            bus.write16(GS_ADDR + 0x12u, initial_time);
            bus.write8(SCENE_ADDR + 0x1044u, initial_sub);

            Arm7Cpu test_cpu;
            test_cpu.reset(0x08017E18u);
            test_cpu.set_cpsr(static_cast<u32>(Arm7Cpu::Mode::System) | Arm7Cpu::FLAG_T);
            test_cpu.set_reg(13, STACK_TOP);
            test_cpu.set_reg(6, OBJ6_ADDR);
            test_cpu.set_reg(8, SCENE_ADDR);
            test_cpu.set_reg(10, sl_is_interior); // sl = is_interior

            u32 captured_r0 = 0, captured_r1 = 0, captured_r2 = 0, captured_r3 = 0;
            test_cpu.set_pc_hook(0x0801A13Cu, [&](Arm7Cpu& c, GbaBus&) {
                hit_outdoor_pal = true;
                captured_r0 = c.reg(0);
                captured_r1 = c.reg(1);
                captured_r2 = c.reg(2);
                captured_r3 = c.reg(3);
                c.set_reg(15, c.lr() & ~1u);
                c.set_cpsr((c.lr() & 1u) ? (c.cpsr() | Arm7Cpu::FLAG_T) : (c.cpsr() & ~Arm7Cpu::FLAG_T));
                return true;
            });

            for (int step = 0; step < 300; ++step) {
                const u32 cur_pc = test_cpu.pc();
                if (cur_pc == 0x08018002u) break;
                // Stop safely before 0x08018002 if it hits 20:00 tile swap or reaches 0x08017FFEu
                if (cur_pc == 0x08017FFEu || cur_pc == 0x08012464u || cur_pc == 0x0801DB58u) break;
                test_cpu.step_one(bus);
            }
            if (hit_outdoor_pal) {
                assert(captured_r0 == SCENE_ADDR);
                assert(captured_r1 == 8u && captured_r2 == 0u && captured_r3 == 0u);
            }
        };

        bool hit_outdoor = false;
        // 1) Indoors (sl=1) at 07:59, sub_frame=24 -> increments to 25, does NOT roll to 08:00 (25 -> 75!)
        run_clock_tick(1u, static_cast<u16>(7u | (59u << 5)), 24u, hit_outdoor);
        assert(bus.read8(SCENE_ADDR + 0x1044u) == 25u);
        assert((bus.read16(GS_ADDR + 0x12u) & 0x07FFu) == static_cast<u16>(7u | (59u << 5)));
        assert(!hit_outdoor);

        // 2) Indoors (sl=1) at 07:59, sub_frame=74 -> rolls to 08:00, sub_frame=0, skips func_0801A13C!
        run_clock_tick(1u, static_cast<u16>(7u | (59u << 5)), 74u, hit_outdoor);
        assert(bus.read8(SCENE_ADDR + 0x1044u) == 0u);
        assert((bus.read16(GS_ADDR + 0x12u) & 0x07FFu) == static_cast<u16>(8u | (0u << 5)));
        assert(!hit_outdoor);

        // 3) Outdoors (sl=0) at 07:59, sub_frame=74 -> rolls to 08:00, sub_frame=0, invokes func_0801A13C(scene, 8, 0, 0)!
        run_clock_tick(0u, static_cast<u16>(7u | (59u << 5)), 74u, hit_outdoor);
        assert(bus.read8(SCENE_ADDR + 0x1044u) == 0u);
        assert((bus.read16(GS_ADDR + 0x12u) & 0x07FFu) == static_cast<u16>(8u | (0u << 5)));
        assert(hit_outdoor);
    }

    return 0;
}
