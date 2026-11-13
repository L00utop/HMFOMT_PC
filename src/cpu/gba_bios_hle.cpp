#include "gba_bios_hle.hpp"
#include "arm7tdmi.hpp"
#include "hal/gba_bus.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace hm::cpu {

namespace {

u32 isqrt_u32(u32 x) {
    if (x == 0) return 0;
    u32 res = 0;
    u32 bit = 1u << 30;
    while (bit > x) bit >>= 2;
    while (bit != 0) {
        if (x >= res + bit) {
            x -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return res;
}

u16 gba_arctan(i16 tan_val) {
    // 1.14 fixed-point arctan matching GBA BIOS LUT/polynomial accuracy
    const double t = static_cast<double>(tan_val) / 16384.0;
    const double rad = std::atan(t);
    const double scaled = (rad / 3.14159265358979323846) * 32768.0;
    return static_cast<u16>(static_cast<i32>(std::lround(scaled)) & 0xFFFF);
}

u16 gba_arctan2(i16 x, i16 y) {
    if (x == 0 && y == 0) return 0u;
    const double rad = std::atan2(static_cast<double>(y), static_cast<double>(x));
    double angle = (rad / (2.0 * 3.14159265358979323846)) * 65536.0;
    if (angle < 0.0) angle += 65536.0;
    return static_cast<u16>(static_cast<i32>(std::lround(angle)) & 0xFFFF);
}

} // namespace

void GbaBiosHle::dispatch(u8 swi_num, Arm7Cpu& cpu, hal::GbaBus& bus) {
    switch (swi_num) {
        case 0x00u: { // SoftReset
            // Wipe top 0x200 of IWRAM (0x03007E00..0x03007FFF), reset stacks & jump
            for (u32 off = 0x7E00u; off < 0x8000u; off += 4u) {
                bus.write32(hal::ADDR_IWRAM_BASE + off, 0u);
            }
            const u8 flag = bus.read8(0x03007FFAu);
            cpu.set_cpsr(static_cast<u32>(Arm7Cpu::Mode::Irq) | Arm7Cpu::FLAG_I | Arm7Cpu::FLAG_F);
            cpu.set_reg(13, 0x03007FA0u);
            cpu.set_cpsr(static_cast<u32>(Arm7Cpu::Mode::Svc) | Arm7Cpu::FLAG_I | Arm7Cpu::FLAG_F);
            cpu.set_reg(13, 0x03007FE0u);
            cpu.set_cpsr(static_cast<u32>(Arm7Cpu::Mode::System));
            cpu.set_reg(13, 0x03007F00u);
            cpu.set_reg(14, 0u);
            cpu.set_reg(15, flag ? 0x02000000u : 0x08000000u);
            break;
        }

        case 0x01u: // RegisterRamReset
            bus.svc_register_ram_reset(cpu.reg(0));
            break;

        case 0x02u: // Halt
            cpu.set_halted(true);
            break;

        case 0x03u: // Stop
            cpu.set_halted(true);
            break;

        case 0x04u: { // IntrWait(discard_old, irq_mask)
            const u32 discard_old = cpu.reg(0);
            const u16 mask = static_cast<u16>(cpu.reg(1) & 0x3FFFu);
            bus.io_write16(hal::reg::IME, 1u);
            cpu.set_cpsr(cpu.cpsr() & ~Arm7Cpu::FLAG_I);

            if (mask == 0u) break;

            const u16 intr_check = bus.read16(0x03007FF8u);
            if (!discard_old && (intr_check & mask) != 0u) {
                bus.write16(0x03007FF8u, static_cast<u16>(intr_check & ~mask));
                break;
            }
            bus.write16(0x03007FF8u, static_cast<u16>(intr_check & ~mask));
            cpu.begin_intr_wait(mask, cpu.pc());
            cpu.set_halted(true);
            break;
        }

        case 0x05u: { // VBlankIntrWait()
            bus.io_write16(hal::reg::IME, 1u);
            cpu.set_cpsr(cpu.cpsr() & ~Arm7Cpu::FLAG_I);
            const u16 intr_check = bus.read16(0x03007FF8u);
            bus.write16(0x03007FF8u, static_cast<u16>(intr_check & ~hal::IRQ_VBLANK));
            cpu.begin_intr_wait(hal::IRQ_VBLANK, cpu.pc());
            cpu.set_halted(true);
            break;
        }

        case 0x06u: // Div(num, denom)
        case 0x07u: { // DivArm(denom, num)
            const i32 num = static_cast<i32>(swi_num == 0x07u ? cpu.reg(1) : cpu.reg(0));
            const i32 den = static_cast<i32>(swi_num == 0x07u ? cpu.reg(0) : cpu.reg(1));
            if (den == 0) {
                cpu.set_reg(0, (num < 0) ? 0xFFFFFFFFu : 1u);
                cpu.set_reg(1, static_cast<u32>(num));
                cpu.set_reg(3, (num < 0) ? 1u : 1u);
            } else if (num == INT32_MIN && den == -1) {
                cpu.set_reg(0, static_cast<u32>(INT32_MIN));
                cpu.set_reg(1, 0u);
                cpu.set_reg(3, 0x80000000u);
            } else {
                const i32 q = num / den;
                const i32 r = num % den;
                const u32 abs_q = static_cast<u32>(q < 0 ? -static_cast<i64>(q) : static_cast<i64>(q));
                cpu.set_reg(0, static_cast<u32>(q));
                cpu.set_reg(1, static_cast<u32>(r));
                cpu.set_reg(3, abs_q);
            }
            break;
        }

        case 0x08u: // Sqrt
            cpu.set_reg(0, isqrt_u32(cpu.reg(0)));
            break;

        case 0x09u: // ArcTan
            cpu.set_reg(0, gba_arctan(static_cast<i16>(cpu.reg(0) & 0xFFFFu)));
            break;

        case 0x0Au: // ArcTan2
            cpu.set_reg(0, gba_arctan2(static_cast<i16>(cpu.reg(0) & 0xFFFFu),
                                       static_cast<i16>(cpu.reg(1) & 0xFFFFu)));
            break;

        case 0x0Bu: // CpuSet
            bus.svc_cpu_set(cpu.reg(0), cpu.reg(1), cpu.reg(2));
            break;

        case 0x0Cu: // CpuFastSet
            bus.svc_cpu_fast_set(cpu.reg(0), cpu.reg(1), cpu.reg(2));
            break;

        case 0x0Du: // GetBiosChecksum
            cpu.set_reg(0, 0xBAAE187Fu);
            break;

        case 0x0Eu: // BgAffineSet
            bus.svc_bg_affine_set(cpu.reg(0), cpu.reg(1), static_cast<i32>(cpu.reg(2)));
            break;

        case 0x0Fu: // ObjAffineSet
            bus.svc_obj_affine_set(cpu.reg(0), cpu.reg(1),
                                   static_cast<i32>(cpu.reg(2)), static_cast<i32>(cpu.reg(3)));
            break;

        case 0x10u: // BitUnPack
            bus.svc_bit_unpack(cpu.reg(0), cpu.reg(1), cpu.reg(2));
            break;

        case 0x11u: // LZ77UnCompWram
            bus.svc_lz77_uncomp(cpu.reg(0), cpu.reg(1), false);
            break;

        case 0x12u: // LZ77UnCompVram
            bus.svc_lz77_uncomp(cpu.reg(0), cpu.reg(1), true);
            break;

        case 0x14u: // RLUnCompWram
            bus.svc_rl_uncomp(cpu.reg(0), cpu.reg(1), false);
            break;

        case 0x15u: // RLUnCompVram
            bus.svc_rl_uncomp(cpu.reg(0), cpu.reg(1), true);
            break;

        case 0x16u: // Diff8bitUnFilterWram
            bus.svc_diff_unfilter(cpu.reg(0), cpu.reg(1), false, false);
            break;

        case 0x17u: // Diff8bitUnFilterVram
            bus.svc_diff_unfilter(cpu.reg(0), cpu.reg(1), false, true);
            break;

        case 0x18u: // Diff16bitUnFilter
            bus.svc_diff_unfilter(cpu.reg(0), cpu.reg(1), true, false);
            break;

        case 0x19u: { // SoundBias
            const u16 level = (cpu.reg(0) != 0) ? 0x0200u : 0x0000u;
            const u16 cur = bus.io_read16(hal::reg::SOUNDBIAS);
            bus.io_write16(hal::reg::SOUNDBIAS, static_cast<u16>((cur & 0xFC00u) | level));
            break;
        }

        case 0x1Fu: { // MidiKey2Freq(WaveData*, mk, fp)
            const u32 wave_addr = cpu.reg(0);
            const u8 mk = static_cast<u8>(cpu.reg(1) & 0xFFu);
            const u8 fp = static_cast<u8>(cpu.reg(2) & 0xFFu);
            const u32 base_freq = bus.read32(wave_addr + 4u);
            const double semitones = (static_cast<double>(180 - mk) - static_cast<double>(fp) / 256.0) / 12.0;
            const double scale = std::pow(2.0, -semitones);
            cpu.set_reg(0, static_cast<u32>(static_cast<double>(base_freq) * scale));
            break;
        }

        case 0x2Au: { // SoundGetJumpList (if called directly via SWI 0x2A)
            const u32 dst = cpu.reg(0);
            if (dst >= hal::ADDR_EWRAM_BASE && dst < hal::ADDR_ROM_BASE && bus.rom().size() >= 0x139B18u) {
                for (u32 i = 0; i < 36u; ++i) {
                    bus.write32(dst + i * 4u, bus.read32(0x08139A88u + i * 4u));
                }
            }
            break;
        }

        default:
            break;
    }
}

} // namespace hm::cpu
