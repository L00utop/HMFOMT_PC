#include "gba_bus.hpp"
#include "hal_audio.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

namespace hm::hal {

namespace {

inline u16 load16_le(const u8* p) {
    return static_cast<u16>(p[0]) | (static_cast<u16>(p[1]) << 8);
}

inline u32 load32_le(const u8* p) {
    return static_cast<u32>(p[0]) |
           (static_cast<u32>(p[1]) << 8) |
           (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}

inline void store16_le(u8* p, u16 v) {
    p[0] = static_cast<u8>(v & 0xFFu);
    p[1] = static_cast<u8>((v >> 8) & 0xFFu);
}

inline void store32_le(u8* p, u32 v) {
    p[0] = static_cast<u8>(v & 0xFFu);
    p[1] = static_cast<u8>((v >> 8) & 0xFFu);
    p[2] = static_cast<u8>((v >> 16) & 0xFFu);
    p[3] = static_cast<u8>((v >> 24) & 0xFFu);
}

constexpr u32 TIMER_PRESCALERS[4] = {1u, 64u, 256u, 1024u};
constexpr u32 GBA_CLOCK_HZ = 16777216u;

} // namespace

GbaBus::GbaBus() {
    reset();
}

void GbaBus::install_bios_trampoline() {
    bios_.fill(0);
    // Minimal authentic ARM exception vectors + GBA BIOS IRQ trampoline:
    //   IRQ entry @ 0x00000018 -> b 0x00000128
    //   0x0128: stmdb sp!, {r0-r3, r12, lr}
    //   0x012C: mov   r0, #0x04000000
    //   0x0130: add   lr, pc, #0        (lr = 0x00000138)
    //   0x0134: ldr   pc, [r0, #-4]     (call user ISR at [0x03007FFC])
    //   0x0138: ldmia sp!, {r0-r3, r12, lr}
    //   0x013C: subs  pc, lr, #4        (restore CPSR from SPSR_irq & return)
    store32_le(&bios_[0x000u], 0xEAFFFFFEu);
    store32_le(&bios_[0x004u], 0xEAFFFFFEu);
    store32_le(&bios_[0x008u], 0xEAFFFFFEu);
    store32_le(&bios_[0x00Cu], 0xEAFFFFFEu);
    store32_le(&bios_[0x010u], 0xEAFFFFFEu);
    store32_le(&bios_[0x014u], 0xEAFFFFFEu);
    store32_le(&bios_[0x018u], 0xEA000042u);
    store32_le(&bios_[0x01Cu], 0xEAFFFFFEu);

    store32_le(&bios_[0x128u], 0xE92D500Fu);
    store32_le(&bios_[0x12Cu], 0xE3A00301u);
    store32_le(&bios_[0x130u], 0xE28FE000u);
    store32_le(&bios_[0x134u], 0xE510F004u);
    store32_le(&bios_[0x138u], 0xE8BD500Fu);
    store32_le(&bios_[0x13Cu], 0xE25EF004u);
}

void GbaBus::detect_save_chip_from_rom() {
    save_chip_ = SaveChip::Flash128K;
    if (rom_.size() < 8u) return;
    static constexpr char kSramTag[] = "SRAM_";
    for (std::size_t i = 0; i + 5u <= rom_.size(); i += 4u) {
        if (std::memcmp(&rom_[i], kSramTag, 5u) == 0) {
            save_chip_ = SaveChip::Sram64K;
            return;
        }
    }
    for (std::size_t i = 0; i + 5u <= rom_.size(); ++i) {
        if (std::memcmp(&rom_[i], kSramTag, 5u) == 0) {
            save_chip_ = SaveChip::Sram64K;
            return;
        }
    }
}

void GbaBus::apply_fomt_coop_rom_patches() {
    coop_rom_patches_applied_ = false;
    // Patch strictly in memory only when an authentic US FoMT ("HARVESTMOGBAA4NE") ROM matches every anchor.
    if (rom_.size() < 0x75C278u) return;
    if (std::memcmp(&rom_[0xA0u], "HARVESTMOGBAA4NE", 16u) != 0) return;

    auto read16 = [&](std::size_t off) -> u16 {
        return load16_le(&rom_[off]);
    };
    auto write16 = [&](std::size_t off, u16 v) {
        store16_le(&rom_[off], v);
    };

    // 1.func_08017C30 anchors:
    //   0x017E1E: b 0x08018002 (0xE0F0) -> nop (0x46C0) so time passes inside buildings (preserving sl)
    //   0x017E3E: cmp r0, #25  (0x2819) -> cmp r0, #75 (0x284B) for 3.0x longer days with identical NPC schedules
    //   0x017E58: mov r3, sl   (0x4653) -> movs r3, #0 (0x2300) to reset sub-frame timer indoors too
    if (read16(0x017E1Eu) != 0xE0F0u) return;
    if (read16(0x017E3Eu) != 0x2819u) return;
    if (read16(0x017E58u) != 0x4653u) return;

    // 2. Indoor outdoor-visual guard at 0x017F88..0x017F93 (skip outdoor ambient blend & 20:00 tile swap when sl != 0)
    static constexpr u16 kExpectedGuardSite[6] = {
        0x4640u, 0x1C29u, 0x1C22u, 0x2300u, 0xF002u, 0xF8D4u
    };
    for (std::size_t i = 0; i < 6u; ++i) {
        if (read16(0x017F88u + i * 2u) != kExpectedGuardSite[i]) return;
    }

    // 3. Free trailing 0xFF region 0x75C250..0x75C277
    for (std::size_t off = 0x75C250u; off < 0x75C278u; off += 2u) {
        if (read16(off) != 0xFFFFu) return;
    }

    write16(0x017E1Eu, 0x46C0u);
    write16(0x017E3Eu, 0x284Bu);
    write16(0x017E58u, 0x2300u);

    // 0x08017F88: ldr r0, [pc, #4]; bx r0; nop; nop; .4byte 0x0875C251
    static constexpr u16 kHookSite[6] = {
        0x4801u, 0x4700u, 0x46C0u, 0x46C0u, 0xC251u, 0x0875u
    };
    for (std::size_t i = 0; i < 6u; ++i) {
        write16(0x017F88u + i * 2u, kHookSite[i]);
    }

    // Thumb trampoline @ 0x0875C250 (20 halfwords including 3 literal words):
    //   mov  r0, sl         ; cmp r0, #0 ; bne +16 (to 0x0875C268)
    //   ldr  r0, =0x08017F95; mov lr, r0
    //   ldr  r0, =0x0801A13D; mov ip, r0
    //   mov  r0, r8         ; adds r1, r5, #0 ; adds r2, r4, #0 ; movs r3, #0
    //   bx   ip
    // 0x0875C268 (indoors):
    //   ldr  r0, =0x08017FB9; bx r0
    static constexpr u16 kTrampoline[20] = {
        0x4650u, 0x2800u, 0xD108u,
        0x4805u, 0x4686u,
        0x4805u, 0x4684u,
        0x4640u, 0x1C29u, 0x1C22u, 0x2300u,
        0x4760u,
        0x4802u, 0x4700u,
        0x7F95u, 0x0801u,
        0xA13Du, 0x0801u,
        0x7FB9u, 0x0801u,
    };
    for (std::size_t i = 0; i < 20u; ++i) {
        write16(0x75C250u + i * 2u, kTrampoline[i]);
    }

    coop_rom_patches_applied_ = true;
}

void GbaBus::reset() {
    install_bios_trampoline();
    ewram_.fill(0);
    iwram_.fill(0);
    io_.fill(0);
    pal_.fill(0);
    vram_.fill(0);
    oam_.fill(0);
    // Do not wipe save_ here so loaded .sav survives hardware soft reset
    dmas_.fill(DmaState{});
    timers_.fill(TimerState{});
    halt_requested_ = false;
    flash_state_ = FlashState::Ready;
    flash_id_mode_ = false;
    flash_bank_ = 0;
    if (!rom_.empty()) {
        detect_save_chip_from_rom();
    } else {
        save_chip_ = SaveChip::Flash128K;
    }

    // Default active-low KEYINPUT = all buttons released (0x03FF)
    store16_le(&io_[reg::KEYINPUT], KEY_ALL);
    store16_le(&io_[reg::SOUNDBIAS], 0x0200u);
    io_[reg::POSTFLG] = 0x01u;
}

void GbaBus::attach_audio(HalAudio* audio) {
    audio_ = audio;
    if (audio_) {
        audio_->set_fifo_refill_callback([this](int fifo_idx) {
            trigger_fifo_dma(fifo_idx);
        });
    }
}

bool GbaBus::load_rom(const u8* data, std::size_t size) {
    if (!data || size == 0 || size > MAX_ROM_SIZE) return false;
    rom_.assign(data, data + size);
    detect_save_chip_from_rom();
    apply_fomt_coop_rom_patches();
    return true;
}

bool GbaBus::load_rom_file(const char* path) {
    if (!path) return false;
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) return false;
    const std::streamsize size = ifs.tellg();
    if (size <= 0 || static_cast<std::size_t>(size) > MAX_ROM_SIZE) return false;
    ifs.seekg(0, std::ios::beg);
    std::vector<u8> buf(static_cast<std::size_t>(size));
    if (!ifs.read(reinterpret_cast<char*>(buf.data()), size)) return false;
    rom_ = std::move(buf);
    detect_save_chip_from_rom();
    apply_fomt_coop_rom_patches();
    return true;
}

u32 GbaBus::map_vram_offset(u32 addr) {
    u32 off = addr & 0x1FFFFu;
    if (off >= 0x18000u) {
        off -= 0x8000u;
    }
    return off;
}

u8 GbaBus::read_flash_or_sram(u32 offset) const {
    const u32 local = offset & 0xFFFFu;
    if (save_chip_ == SaveChip::Sram64K) {
        return save_[local];
    }
    if (flash_id_mode_) {
        if (local == 0x0000u) return 0xC2u; // Macronix manufacturer ID
        if (local == 0x0001u) return 0x09u; // 128KB Flash device ID (MX29L010)
    }
    const u32 full = (static_cast<u32>(flash_bank_ & 1u) << 16) | local;
    return save_[full % SAVE_SIZE];
}

void GbaBus::write_flash_or_sram(u32 offset, u8 val) {
    const u32 local = offset & 0xFFFFu;
    if (save_chip_ == SaveChip::Sram64K) {
        save_[local] = val;
        save_dirty_ = true;
        return;
    }

    switch (flash_state_) {
        case FlashState::Ready:
            if (local == 0x5555u && val == 0xAAu) {
                flash_state_ = FlashState::Cmd5555;
            } else {
                // Direct SRAM fallback if game writes outside Flash command protocol
                const u32 full = (static_cast<u32>(flash_bank_ & 1u) << 16) | local;
                save_[full % SAVE_SIZE] = val;
                save_dirty_ = true;
            }
            return;

        case FlashState::Cmd5555:
            if (local == 0x2AAAu && val == 0x55u) {
                flash_state_ = FlashState::Cmd2AAA;
            } else {
                flash_state_ = FlashState::Ready;
            }
            return;

        case FlashState::Cmd2AAA:
            if (local == 0x5555u) {
                switch (val) {
                    case 0x90u: flash_id_mode_ = true;  flash_state_ = FlashState::Ready; break;
                    case 0xF0u: flash_id_mode_ = false; flash_state_ = FlashState::Ready; break;
                    case 0x80u: flash_state_ = FlashState::EraseCmd; break;
                    case 0xA0u: flash_state_ = FlashState::WriteByte; break;
                    case 0xB0u: flash_state_ = FlashState::BankSelect; break;
                    default:    flash_state_ = FlashState::Ready; break;
                }
            } else {
                flash_state_ = FlashState::Ready;
            }
            return;

        case FlashState::EraseCmd:
            if (local == 0x5555u && val == 0xAAu) flash_state_ = FlashState::Erase5555;
            else flash_state_ = FlashState::Ready;
            return;

        case FlashState::Erase5555:
            if (local == 0x2AAAu && val == 0x55u) flash_state_ = FlashState::Erase2AAA;
            else flash_state_ = FlashState::Ready;
            return;

        case FlashState::Erase2AAA:
            if (local == 0x5555u && val == 0x10u) {
                save_.fill(0xFFu); // Chip erase
                save_dirty_ = true;
            } else if (val == 0x30u) {
                // 4KB sector erase
                const u32 sector_base = ((static_cast<u32>(flash_bank_ & 1u) << 16) | (local & 0xF000u));
                for (u32 i = 0; i < 0x1000u && (sector_base + i) < SAVE_SIZE; ++i) {
                    save_[sector_base + i] = 0xFFu;
                }
                save_dirty_ = true;
            }
            flash_state_ = FlashState::Ready;
            return;

        case FlashState::WriteByte: {
            const u32 full = (static_cast<u32>(flash_bank_ & 1u) << 16) | local;
            save_[full % SAVE_SIZE] = val;
            save_dirty_ = true;
            flash_state_ = FlashState::Ready;
            return;
        }

        case FlashState::BankSelect:
            if (local == 0x0000u) {
                flash_bank_ = val & 1u;
            }
            flash_state_ = FlashState::Ready;
            return;
    }
}

u8 GbaBus::read8(u32 addr) const {
    const u32 region = addr >> 24;
    switch (region) {
        case 0x00: return (addr < BIOS_SIZE) ? bios_[addr] : 0u;
        case 0x02: return ewram_[addr & (EWRAM_SIZE - 1u)];
        case 0x03: return iwram_[addr & (IWRAM_SIZE - 1u)];
        case 0x04: {
            const u32 off = addr & 0x3FFu;
            return (addr < 0x04000400u) ? io_[off] : 0u;
        }
        case 0x05: return pal_[addr & (PAL_SIZE - 1u)];
        case 0x06: return vram_[map_vram_offset(addr)];
        case 0x07: return oam_[addr & (OAM_SIZE - 1u)];
        case 0x08:
        case 0x09:
        case 0x0A:
        case 0x0B:
        case 0x0C:
        case 0x0D: {
            const u32 rom_off = addr - ADDR_ROM_BASE;
            return (rom_off < rom_.size()) ? rom_[rom_off] : 0xFFu;
        }
        case 0x0E:
        case 0x0F:
            return read_flash_or_sram(addr & 0xFFFFu);
        default:
            return 0u;
    }
}

u16 GbaBus::read16(u32 addr) const {
    const u32 aligned = addr & ~1u;
    const u32 region = aligned >> 24;
    switch (region) {
        case 0x00: return (aligned + 1u < BIOS_SIZE) ? load16_le(&bios_[aligned]) : 0u;
        case 0x02: return load16_le(&ewram_[aligned & (EWRAM_SIZE - 1u)]);
        case 0x03: return load16_le(&iwram_[aligned & (IWRAM_SIZE - 1u)]);
        case 0x04: return (aligned < 0x04000400u) ? load16_le(&io_[aligned & 0x3FFu]) : 0u;
        case 0x05: return load16_le(&pal_[aligned & (PAL_SIZE - 1u)]);
        case 0x06: return load16_le(&vram_[map_vram_offset(aligned)]);
        case 0x07: return load16_le(&oam_[aligned & (OAM_SIZE - 1u)]);
        case 0x08:
        case 0x09:
        case 0x0A:
        case 0x0B:
        case 0x0C:
        case 0x0D: {
            const u32 off = aligned - ADDR_ROM_BASE;
            if (off + 1u < rom_.size()) return load16_le(&rom_[off]);
            return 0xFFFFu;
        }
        case 0x0E:
        case 0x0F: {
            const u8 lo = read_flash_or_sram(aligned & 0xFFFFu);
            return static_cast<u16>(lo) | (static_cast<u16>(lo) << 8);
        }
        default:
            return 0u;
    }
}

u32 GbaBus::read32(u32 addr) const {
    const u32 aligned = addr & ~3u;
    const u32 region = aligned >> 24;
    switch (region) {
        case 0x00: return (aligned + 3u < BIOS_SIZE) ? load32_le(&bios_[aligned]) : 0u;
        case 0x02: return load32_le(&ewram_[aligned & (EWRAM_SIZE - 1u)]);
        case 0x03: return load32_le(&iwram_[aligned & (IWRAM_SIZE - 1u)]);
        case 0x04: return (aligned < 0x04000400u) ? load32_le(&io_[aligned & 0x3FFu]) : 0u;
        case 0x05: return load32_le(&pal_[aligned & (PAL_SIZE - 1u)]);
        case 0x06: return load32_le(&vram_[map_vram_offset(aligned)]);
        case 0x07: return load32_le(&oam_[aligned & (OAM_SIZE - 1u)]);
        case 0x08:
        case 0x09:
        case 0x0A:
        case 0x0B:
        case 0x0C:
        case 0x0D: {
            const u32 off = aligned - ADDR_ROM_BASE;
            if (off + 3u < rom_.size()) return load32_le(&rom_[off]);
            return 0xFFFFFFFFu;
        }
        case 0x0E:
        case 0x0F: {
            const u32 b = read_flash_or_sram(aligned & 0xFFFFu);
            return b * 0x01010101u;
        }
        default:
            return 0u;
    }
}

void GbaBus::write8(u32 addr, u8 val) {
    const u32 region = addr >> 24;
    switch (region) {
        case 0x02: ewram_[addr & (EWRAM_SIZE - 1u)] = val; break;
        case 0x03: iwram_[addr & (IWRAM_SIZE - 1u)] = val; break;
        case 0x04:
            if (addr < 0x04000400u) on_io_write8(addr & 0x3FFu, val);
            break;
        case 0x05: pal_[addr & (PAL_SIZE - 1u)] = val; break;
        case 0x06: vram_[map_vram_offset(addr)] = val; break;
        case 0x07: oam_[addr & (OAM_SIZE - 1u)] = val; break;
        case 0x0E:
        case 0x0F:
            write_flash_or_sram(addr & 0xFFFFu, val);
            break;
        default:
            break;
    }
}

void GbaBus::write16(u32 addr, u16 val) {
    const u32 aligned = addr & ~1u;
    const u32 region = aligned >> 24;
    switch (region) {
        case 0x02: store16_le(&ewram_[aligned & (EWRAM_SIZE - 1u)], val); break;
        case 0x03: store16_le(&iwram_[aligned & (IWRAM_SIZE - 1u)], val); break;
        case 0x04:
            if (aligned < 0x04000400u) on_io_write16(aligned & 0x3FFu, val);
            break;
        case 0x05: store16_le(&pal_[aligned & (PAL_SIZE - 1u)], val); break;
        case 0x06: store16_le(&vram_[map_vram_offset(aligned)], val); break;
        case 0x07: store16_le(&oam_[aligned & (OAM_SIZE - 1u)], val); break;
        case 0x0E:
        case 0x0F:
            write_flash_or_sram(aligned & 0xFFFFu, static_cast<u8>(val & 0xFFu));
            break;
        default:
            break;
    }
}

void GbaBus::write32(u32 addr, u32 val) {
    const u32 aligned = addr & ~3u;
    const u32 region = aligned >> 24;
    switch (region) {
        case 0x02: store32_le(&ewram_[aligned & (EWRAM_SIZE - 1u)], val); break;
        case 0x03: store32_le(&iwram_[aligned & (IWRAM_SIZE - 1u)], val); break;
        case 0x04: {
            if (aligned >= 0x04000400u) break;
            const u32 off = aligned & 0x3FFu;
            if (off == reg::FIFO_A) {
                store32_le(&io_[off], val);
                if (audio_) audio_->push_fifo_word(0, val);
                break;
            }
            if (off == reg::FIFO_B) {
                store32_le(&io_[off], val);
                if (audio_) audio_->push_fifo_word(1, val);
                break;
            }
            if (off >= reg::WAVE_RAM0 && off <= reg::WAVE_RAM3) {
                store32_le(&io_[off], val);
                if (audio_) audio_->write_wave_ram(off - reg::WAVE_RAM0, val);
                break;
            }
            on_io_write16(off, static_cast<u16>(val & 0xFFFFu));
            on_io_write16(off + 2u, static_cast<u16>(val >> 16));
            break;
        }
        case 0x05: store32_le(&pal_[aligned & (PAL_SIZE - 1u)], val); break;
        case 0x06: store32_le(&vram_[map_vram_offset(aligned)], val); break;
        case 0x07: store32_le(&oam_[aligned & (OAM_SIZE - 1u)], val); break;
        case 0x0E:
        case 0x0F:
            write_flash_or_sram(aligned & 0xFFFFu, static_cast<u8>(val & 0xFFu));
            break;
        default:
            break;
    }
}

u16 GbaBus::io_read16(u32 io_offset) const {
    if (io_offset + 1u >= IO_SIZE) return 0u;
    return load16_le(&io_[io_offset & ~1u]);
}

u32 GbaBus::io_read32(u32 io_offset) const {
    if (io_offset + 3u >= IO_SIZE) return 0u;
    return load32_le(&io_[io_offset & ~3u]);
}

void GbaBus::io_write16(u32 io_offset, u16 val) {
    if (io_offset + 1u < IO_SIZE) {
        on_io_write16(io_offset & ~1u, val);
    }
}

void GbaBus::io_write32(u32 io_offset, u32 val) {
    write32(ADDR_IO_BASE + io_offset, val);
}

void GbaBus::on_io_write8(u32 io_offset, u8 val) {
    if (io_offset == reg::HALTCNT) {
        if (!(val & 0x80u)) {
            halt_requested_ = true;
        }
        return;
    }
    if (io_offset == reg::IF || io_offset == reg::IF + 1u) {
        const u16 ack_mask = (io_offset & 1u)
            ? static_cast<u16>(val) << 8
            : static_cast<u16>(val);
        const u16 cur_if = load16_le(&io_[reg::IF]);
        store16_le(&io_[reg::IF], cur_if & static_cast<u16>(~ack_mask));
        return;
    }

    const u32 aligned = io_offset & ~1u;
    u16 cur = load16_le(&io_[aligned]);
    if (io_offset & 1u) {
        cur = static_cast<u16>((cur & 0x00FFu) | (static_cast<u16>(val) << 8));
    } else {
        cur = static_cast<u16>((cur & 0xFF00u) | val);
    }
    on_io_write16(aligned, cur);
}

void GbaBus::on_io_write16(u32 io_offset, u16 val) {
    if (io_offset == reg::VCOUNT || io_offset == reg::KEYINPUT) {
        return; // Read-only hardware registers
    }

    if (io_offset == reg::DISPSTAT) {
        // Bits 0..2 are hardware status bits (VBlank, HBlank, VCount match)
        const u16 hw_bits = load16_le(&io_[reg::DISPSTAT]) & 0x0007u;
        store16_le(&io_[reg::DISPSTAT], (val & ~0x0007u) | hw_bits);
        return;
    }

    if (io_offset == reg::IF) {
        // Writing 1 acknowledges (clears) interrupt flags
        const u16 cur_if = load16_le(&io_[reg::IF]);
        store16_le(&io_[reg::IF], cur_if & static_cast<u16>(~val));
        return;
    }

    // Track previous values for edge detection (DMA enable, Timer enable)
    const u16 prev = load16_le(&io_[io_offset]);
    store16_le(&io_[io_offset], val);

    // Sound registers notification (clear write-only trigger/reset bits in io_ shadow)
    if (io_offset >= reg::SOUND1CNT_L && io_offset <= reg::SOUNDBIAS) {
        if (audio_) audio_->write_register(io_offset, val);
        if (io_offset == reg::SOUNDCNT_H) {
            store16_le(&io_[io_offset], static_cast<u16>(val & ~0x8800u));
        } else if (io_offset == reg::SOUND1CNT_X || io_offset == reg::SOUND2CNT_H ||
                   io_offset == reg::SOUND3CNT_X || io_offset == reg::SOUND4CNT_H) {
            store16_le(&io_[io_offset], static_cast<u16>(val & 0x7FFFu));
        }
        return;
    }
    if (io_offset >= reg::WAVE_RAM0 && io_offset <= reg::WAVE_RAM3 + 2u) {
        const u32 aligned4 = io_offset & ~3u;
        if (audio_) audio_->write_wave_ram(aligned4 - reg::WAVE_RAM0, load32_le(&io_[aligned4]));
        return;
    }

    // Timers 0..3
    for (int t = 0; t < 4; ++t) {
        const u32 base = reg::TM0CNT_L + static_cast<u32>(t) * 4u;
        if (io_offset == base) {
            timers_[t].reload = val;
            return;
        }
        if (io_offset == base + 2u) {
            const bool was_running = (prev & 0x0080u) != 0;
            const bool now_running = (val & 0x0080u) != 0;
            timers_[t].running = now_running;
            if (!was_running && now_running) {
                timers_[t].counter = timers_[t].reload;
                timers_[t].cycle_accum = 0;
            }
            return;
        }
    }

    // DMA 0..3 control
    for (int ch = 0; ch < 4; ++ch) {
        const u32 cnt_h = reg::DMA0CNT_H + static_cast<u32>(ch) * 12u;
        if (io_offset == cnt_h) {
            const bool was_enabled = (prev & 0x8000u) != 0;
            const bool now_enabled = (val & 0x8000u) != 0;
            if (!was_enabled && now_enabled) {
                const u32 base = reg::DMA0SAD + static_cast<u32>(ch) * 12u;
                dmas_[ch].sad_latch = load32_le(&io_[base + 0u]);
                dmas_[ch].dad_latch = load32_le(&io_[base + 4u]);
                const u16 len = load16_le(&io_[base + 8u]);
                dmas_[ch].count_latch = (len == 0) ? ((ch == 3) ? 0x10000u : 0x4000u) : len;

                const auto timing = static_cast<DmaTiming>((val >> 12) & 0x03u);
                if (timing == DmaTiming::Immediate) {
                    execute_dma(ch);
                }
            }
            return;
        }
    }
}

void GbaBus::execute_dma(int ch) {
    const u32 base = reg::DMA0SAD + static_cast<u32>(ch) * 12u;
    const u16 cnt_h = load16_le(&io_[base + 10u]);
    if (!(cnt_h & 0x8000u)) return;

    const bool is_32bit = (cnt_h & 0x0400u) != 0;
    const u32 dst_ctrl = (cnt_h >> 5) & 0x03u;
    const u32 src_ctrl = (cnt_h >> 7) & 0x03u;
    const bool repeat  = (cnt_h & 0x0200u) != 0;
    const bool irq_en  = (cnt_h & 0x4000u) != 0;
    const auto timing  = static_cast<DmaTiming>((cnt_h >> 12) & 0x03u);

    const bool fifo_mode = (timing == DmaTiming::Special) && (ch == 1 || ch == 2);
    const u32 unit = (is_32bit || fifo_mode) ? 4u : 2u;
    const u32 count = fifo_mode ? 4u : dmas_[ch].count_latch;

    u32 src = dmas_[ch].sad_latch & ~(unit - 1u);
    u32 dst = dmas_[ch].dad_latch & ~(unit - 1u);

    for (u32 i = 0; i < count; ++i) {
        if (unit == 4u) {
            write32(dst, read32(src));
        } else {
            write16(dst, read16(src));
        }

        if (src_ctrl == 0u) src += unit;
        else if (src_ctrl == 1u) src -= unit;

        if (!fifo_mode) {
            if (dst_ctrl == 0u || dst_ctrl == 3u) dst += unit;
            else if (dst_ctrl == 1u) dst -= unit;
        }
    }

    dmas_[ch].sad_latch = src;
    if (!fifo_mode && dst_ctrl != 3u) {
        dmas_[ch].dad_latch = dst;
    }

    if (repeat && timing != DmaTiming::Immediate) {
        if (!fifo_mode) {
            const u16 len = load16_le(&io_[base + 8u]);
            dmas_[ch].count_latch = (len == 0) ? ((ch == 3) ? 0x10000u : 0x4000u) : len;
            if (dst_ctrl == 3u) {
                dmas_[ch].dad_latch = load32_le(&io_[base + 4u]);
            }
        }
    } else {
        // Disable DMA enable bit on completion
        store16_le(&io_[base + 10u], cnt_h & static_cast<u16>(~0x8000u));
    }

    if (irq_en) {
        raise_irq(static_cast<u16>(IRQ_DMA0 << ch));
    }
}

void GbaBus::trigger_dma_by_timing(DmaTiming timing) {
    for (int ch = 0; ch < 4; ++ch) {
        const u16 cnt_h = load16_le(&io_[reg::DMA0CNT_H + static_cast<u32>(ch) * 12u]);
        if ((cnt_h & 0x8000u) && static_cast<DmaTiming>((cnt_h >> 12) & 0x03u) == timing) {
            execute_dma(ch);
        }
    }
}

void GbaBus::trigger_fifo_dma(int fifo_index) {
    const int ch = (fifo_index == 0) ? 1 : 2;
    const u16 cnt_h = load16_le(&io_[reg::DMA0CNT_H + static_cast<u32>(ch) * 12u]);
    if ((cnt_h & 0x8000u) && static_cast<DmaTiming>((cnt_h >> 12) & 0x03u) == DmaTiming::Special) {
        execute_dma(ch);
    }
}

void GbaBus::on_timer_overflow_internal(int ch) {
    const u16 cnt_h = load16_le(&io_[reg::TM0CNT_H + static_cast<u32>(ch) * 4u]);
    if (cnt_h & 0x0040u) {
        raise_irq(static_cast<u16>(IRQ_TIMER0 << ch));
    }
    if ((ch == 0 || ch == 1) && audio_) {
        const u32 prescale = TIMER_PRESCALERS[cnt_h & 0x03u];
        const u32 period_ticks = std::max<u32>(1u, 0x10000u - timers_[ch].reload);
        const u32 sample_hz = GBA_CLOCK_HZ / (prescale * period_ticks);
        audio_->on_timer_overflow(ch, sample_hz);
    }
    // Cascade next timer if count-up bit (bit 2) is set
    if (ch < 3) {
        const u16 next_cnt = load16_le(&io_[reg::TM0CNT_H + static_cast<u32>(ch + 1) * 4u]);
        if (timers_[ch + 1].running && (next_cnt & 0x0004u)) {
            if (++timers_[ch + 1].counter == 0) {
                timers_[ch + 1].counter = timers_[ch + 1].reload;
                on_timer_overflow_internal(ch + 1);
            }
        }
    }
}

void GbaBus::step_timers(u32 cpu_cycles) {
    u32 remaining = cpu_cycles;
    while (remaining > 0u) {
        u32 chunk = remaining;
        for (int ch = 0; ch < 4; ++ch) {
            if (!timers_[ch].running) continue;
            const u16 cnt_h = load16_le(&io_[reg::TM0CNT_H + static_cast<u32>(ch) * 4u]);
            if (ch > 0 && (cnt_h & 0x0004u)) continue; // Driven by cascade overflow
            const u32 prescale = TIMER_PRESCALERS[cnt_h & 0x03u];
            const u32 rem_ticks = 0x10000u - static_cast<u32>(timers_[ch].counter);
            const u32 raw_cy = rem_ticks * prescale;
            const u32 cy_to_ov = (raw_cy > timers_[ch].cycle_accum) ? (raw_cy - timers_[ch].cycle_accum) : 1u;
            if (cy_to_ov < chunk) {
                chunk = cy_to_ov;
            }
        }
        if (chunk == 0u) chunk = 1u;

        // Render SPU up to exact cycle instant right before any timer overflow updates FIFO
        if (audio_) {
            audio_->step_cycles(chunk);
        }

        for (int ch = 0; ch < 4; ++ch) {
            if (!timers_[ch].running) continue;
            const u16 cnt_h = load16_le(&io_[reg::TM0CNT_H + static_cast<u32>(ch) * 4u]);
            if (ch > 0 && (cnt_h & 0x0004u)) continue;
            const u32 prescale = TIMER_PRESCALERS[cnt_h & 0x03u];
            const u32 total_cy = timers_[ch].cycle_accum + chunk;
            const u32 ticks = total_cy / prescale;
            timers_[ch].cycle_accum = total_cy % prescale;

            const u32 next_val = static_cast<u32>(timers_[ch].counter) + ticks;
            if (next_val >= 0x10000u) {
                timers_[ch].counter = static_cast<u16>(timers_[ch].reload + (next_val - 0x10000u));
                on_timer_overflow_internal(ch);
            } else {
                timers_[ch].counter = static_cast<u16>(next_val);
            }
        }

        remaining -= chunk;
    }
}

void GbaBus::set_vcount(u16 vcount) {
    store16_le(&io_[reg::VCOUNT], vcount);
    u16 dispstat = load16_le(&io_[reg::DISPSTAT]);
    const u16 lyc = (dispstat >> 8) & 0xFFu;
    if (vcount == lyc) {
        dispstat |= 0x0004u;
        if (dispstat & 0x0020u) raise_irq(IRQ_VCOUNT);
    } else {
        dispstat &= static_cast<u16>(~0x0004u);
    }
    store16_le(&io_[reg::DISPSTAT], dispstat);
}

void GbaBus::set_dispstat_flags(u16 mask, bool set) {
    u16 dispstat = load16_le(&io_[reg::DISPSTAT]);
    if (set) dispstat |= (mask & 0x0007u);
    else dispstat &= static_cast<u16>(~(mask & 0x0007u));
    store16_le(&io_[reg::DISPSTAT], dispstat);
}

void GbaBus::set_keyinput(u16 active_low_mask) {
    store16_le(&io_[reg::KEYINPUT], active_low_mask & KEY_ALL);
}

void GbaBus::raise_irq(u16 irq_bit) {
    const u16 cur_if = load16_le(&io_[reg::IF]);
    store16_le(&io_[reg::IF], cur_if | irq_bit);
}

void GbaBus::svc_register_ram_reset(u32 flags) {
    if (flags & RESET_EWRAM) ewram_.fill(0);
    if (flags & RESET_IWRAM) iwram_.fill(0);
    if (flags & RESET_PALETTE) pal_.fill(0);
    if (flags & RESET_VRAM) vram_.fill(0);
    if (flags & RESET_OAM) oam_.fill(0);
    if (flags & RESET_SOUND_IO) {
        for (u32 off = 0x060u; off < 0x090u; off += 2u) io_write16(off, 0);
        if (audio_) audio_->reset();
    }
    if (flags & RESET_IO) {
        for (u32 off = 0x000u; off < 0x060u; off += 2u) io_write16(off, 0);
        for (u32 off = 0x0B0u; off < 0x130u; off += 2u) io_write16(off, 0);
    }
}

void GbaBus::svc_cpu_set(u32 src_addr, u32 dst_addr, u32 control) {
    const u32 count = control & 0x1FFFFFu;
    const bool fixed_src = (control & 0x01000000u) != 0;
    const bool is_32 = (control & 0x04000000u) != 0;
    const u32 step = is_32 ? 4u : 2u;

    for (u32 i = 0; i < count; ++i) {
        if (is_32) write32(dst_addr, read32(src_addr));
        else write16(dst_addr, read16(src_addr));
        dst_addr += step;
        if (!fixed_src) src_addr += step;
    }
}

void GbaBus::svc_cpu_fast_set(u32 src_addr, u32 dst_addr, u32 control) {
    const u32 words = (control & 0x1FFFFFu);
    const u32 aligned_words = (words + 7u) & ~7u;
    const bool fixed_src = (control & 0x01000000u) != 0;
    for (u32 i = 0; i < aligned_words; ++i) {
        write32(dst_addr, read32(src_addr));
        dst_addr += 4u;
        if (!fixed_src) src_addr += 4u;
    }
}

void GbaBus::svc_lz77_uncomp(u32 src_addr, u32 dst_addr, bool vram_16bit) {
    const u32 header = read32(src_addr);
    if ((header & 0xF0u) != 0x10u) return;
    const u32 decomp_size = header >> 8;
    src_addr += 4u;

    u32 written = 0;
    std::vector<u8> out(decomp_size, 0);

    while (written < decomp_size) {
        const u8 flags = read8(src_addr++);
        for (int bit = 7; bit >= 0 && written < decomp_size; --bit) {
            if (flags & (1u << bit)) {
                const u8 b1 = read8(src_addr++);
                const u8 b2 = read8(src_addr++);
                const u32 len = ((b1 >> 4) & 0x0Fu) + 3u;
                const u32 disp = (((static_cast<u32>(b1) & 0x0Fu) << 8) | b2) + 1u;
                for (u32 k = 0; k < len && written < decomp_size; ++k) {
                    const u8 val = (written >= disp) ? out[written - disp] : 0u;
                    out[written++] = val;
                }
            } else {
                out[written++] = read8(src_addr++);
            }
        }
    }

    if (vram_16bit) {
        for (u32 i = 0; i + 1u < decomp_size; i += 2u) {
            const u16 hw = static_cast<u16>(out[i]) | (static_cast<u16>(out[i + 1u]) << 8);
            write16(dst_addr + i, hw);
        }
        if (decomp_size & 1u) {
            write16(dst_addr + (decomp_size - 1u), out[decomp_size - 1u]);
        }
    } else {
        for (u32 i = 0; i < decomp_size; ++i) {
            write8(dst_addr + i, out[i]);
        }
    }
}

void GbaBus::svc_rl_uncomp(u32 src_addr, u32 dst_addr, bool vram_16bit) {
    const u32 header = read32(src_addr);
    if ((header & 0xF0u) != 0x30u) return;
    const u32 decomp_size = header >> 8;
    src_addr += 4u;

    u32 written = 0;
    std::vector<u8> out;
    out.reserve(decomp_size);

    while (written < decomp_size) {
        const u8 flag = read8(src_addr++);
        if (flag & 0x80u) {
            const u32 len = (flag & 0x7Fu) + 3u;
            const u8 data = read8(src_addr++);
            for (u32 i = 0; i < len && written < decomp_size; ++i) {
                out.push_back(data);
                ++written;
            }
        } else {
            const u32 len = (flag & 0x7Fu) + 1u;
            for (u32 i = 0; i < len && written < decomp_size; ++i) {
                out.push_back(read8(src_addr++));
                ++written;
            }
        }
    }

    if (vram_16bit) {
        for (u32 i = 0; i + 1u < out.size(); i += 2u) {
            const u16 hw = static_cast<u16>(out[i]) | (static_cast<u16>(out[i + 1u]) << 8);
            write16(dst_addr + i, hw);
        }
        if (out.size() & 1u) {
            write16(dst_addr + static_cast<u32>(out.size() - 1u), out.back());
        }
    } else {
        for (u32 i = 0; i < out.size(); ++i) {
            write8(dst_addr + i, out[i]);
        }
    }
}

void GbaBus::svc_bg_affine_set(u32 src_addr, u32 dst_addr, i32 count) {
    for (i32 i = 0; i < count; ++i) {
        const i32 tex_x = static_cast<i32>(read32(src_addr + 0));
        const i32 tex_y = static_cast<i32>(read32(src_addr + 4));
        const i16 scr_x = static_cast<i16>(read16(src_addr + 8));
        const i16 scr_y = static_cast<i16>(read16(src_addr + 10));
        const i16 sx    = static_cast<i16>(read16(src_addr + 12));
        const i16 sy    = static_cast<i16>(read16(src_addr + 14));
        const u16 alpha = read16(src_addr + 16);
        src_addr += 20;

        const double rad = (static_cast<double>(alpha >> 8) * 2.0 * 3.14159265358979323846) / 256.0;
        const double c = std::cos(rad);
        const double s = std::sin(rad);

        const i16 pa = static_cast<i16>(std::lround(sx * c));
        const i16 pb = static_cast<i16>(std::lround(-sx * s));
        const i16 pc = static_cast<i16>(std::lround(sy * s));
        const i16 pd = static_cast<i16>(std::lround(sy * c));

        const i32 ref_x = tex_x - (pa * scr_x + pb * scr_y);
        const i32 ref_y = tex_y - (pc * scr_x + pd * scr_y);

        write16(dst_addr + 0, static_cast<u16>(pa));
        write16(dst_addr + 2, static_cast<u16>(pb));
        write16(dst_addr + 4, static_cast<u16>(pc));
        write16(dst_addr + 6, static_cast<u16>(pd));
        write32(dst_addr + 8, static_cast<u32>(ref_x));
        write32(dst_addr + 12, static_cast<u32>(ref_y));
        dst_addr += 16;
    }
}

void GbaBus::svc_obj_affine_set(u32 src_addr, u32 dst_addr, i32 count, i32 offset) {
    for (i32 i = 0; i < count; ++i) {
        const i16 sx    = static_cast<i16>(read16(src_addr + 0));
        const i16 sy    = static_cast<i16>(read16(src_addr + 2));
        const u16 alpha = read16(src_addr + 4);
        src_addr += 8;

        const double rad = (static_cast<double>(alpha >> 8) * 2.0 * 3.14159265358979323846) / 256.0;
        const double c = std::cos(rad);
        const double s = std::sin(rad);

        const i16 pa = static_cast<i16>(std::lround(sx * c));
        const i16 pb = static_cast<i16>(std::lround(-sx * s));
        const i16 pc = static_cast<i16>(std::lround(sy * s));
        const i16 pd = static_cast<i16>(std::lround(sy * c));

        write16(dst_addr, static_cast<u16>(pa)); dst_addr += static_cast<u32>(offset);
        write16(dst_addr, static_cast<u16>(pb)); dst_addr += static_cast<u32>(offset);
        write16(dst_addr, static_cast<u16>(pc)); dst_addr += static_cast<u32>(offset);
        write16(dst_addr, static_cast<u16>(pd)); dst_addr += static_cast<u32>(offset);
    }
}

void GbaBus::svc_bit_unpack(u32 src_addr, u32 dst_addr, u32 info_addr) {
    const u16 src_len = read16(info_addr + 0u);
    const u8 src_width = read8(info_addr + 2u);
    const u8 dst_width = read8(info_addr + 3u);
    const u32 data_offset_raw = read32(info_addr + 4u);
    const u32 data_offset = data_offset_raw & 0x7FFFFFFFu;
    const bool zero_data_flag = (data_offset_raw & 0x80000000u) != 0;

    if (src_width == 0 || dst_width == 0 || (src_width & (src_width - 1)) != 0 ||
        (dst_width & (dst_width - 1)) != 0 || dst_width > 32) {
        return;
    }

    const u32 src_mask = (1u << src_width) - 1u;
    u32 accum = 0;
    u32 accum_bits = 0;

    for (u16 i = 0; i < src_len; ++i) {
        const u8 b = read8(src_addr + i);
        for (u32 bit = 0; bit < 8u; bit += src_width) {
            u32 unit = (b >> bit) & src_mask;
            if (unit != 0 || zero_data_flag) {
                unit += data_offset;
            }
            accum |= (unit << accum_bits);
            accum_bits += dst_width;
            if (accum_bits >= 32u) {
                write32(dst_addr, accum);
                dst_addr += 4u;
                accum = 0;
                accum_bits = 0;
            }
        }
    }
    if (accum_bits > 0) {
        write32(dst_addr, accum);
    }
}

void GbaBus::svc_diff_unfilter(u32 src_addr, u32 dst_addr, bool bit16, bool vram_16bit) {
    const u32 header = read32(src_addr);
    const u32 decomp_size = header >> 8;
    src_addr += 4u;

    if (bit16) {
        u16 prev = 0;
        for (u32 i = 0; i + 1u < decomp_size; i += 2u) {
            const u16 diff = read16(src_addr + i);
            prev = static_cast<u16>(prev + diff);
            write16(dst_addr + i, prev);
        }
    } else {
        u8 prev = 0;
        u16 pending = 0;
        for (u32 i = 0; i < decomp_size; ++i) {
            prev = static_cast<u8>(prev + read8(src_addr + i));
            if (vram_16bit) {
                if ((i & 1u) == 0u) {
                    pending = prev;
                } else {
                    write16(dst_addr + (i & ~1u), pending | (static_cast<u16>(prev) << 8));
                }
            } else {
                write8(dst_addr + i, prev);
            }
        }
        if (vram_16bit && (decomp_size & 1u)) {
            write16(dst_addr + (decomp_size - 1u), pending);
        }
    }
}

} // namespace hm::hal
