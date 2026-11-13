#include "arm7tdmi.hpp"
#include "gba_bios_hle.hpp"
#include "hal/gba_bus.hpp"

#include <algorithm>

namespace hm::cpu {

namespace {

inline u32 rotr32(u32 v, u32 shift) {
    shift &= 31u;
    if (shift == 0u) return v;
    return (v >> shift) | (v << (32u - shift));
}

inline u32 sign_extend(u32 val, int bits) {
    const u32 mask = 1u << (bits - 1);
    return (val ^ mask) - mask;
}

inline u32 arm_read32_rot(const hal::GbaBus& bus, u32 addr) {
    const u32 word = bus.read32(addr & ~3u);
    const u32 rot = (addr & 3u) * 8u;
    return rotr32(word, rot);
}

inline u32 arm_read16_rot(const hal::GbaBus& bus, u32 addr) {
    const u16 hw = bus.read16(addr & ~1u);
    if (addr & 1u) {
        return rotr32(hw, 8u);
    }
    return hw;
}

inline u32 arm_read_s16_rot(const hal::GbaBus& bus, u32 addr) {
    if (addr & 1u) {
        return sign_extend(bus.read8(addr), 8);
    }
    return sign_extend(bus.read16(addr), 16);
}

struct ShiftResult {
    u32 val;
    bool carry;
};

ShiftResult barrel_shift_imm(u32 val, u32 type, u32 amount, bool carry_in) {
    switch (type & 3u) {
        case 0u: // LSL
            if (amount == 0u) return {val, carry_in};
            return {val << amount, ((val >> (32u - amount)) & 1u) != 0};
        case 1u: // LSR
            if (amount == 0u) return {0u, (val >> 31) != 0};
            return {val >> amount, ((val >> (amount - 1u)) & 1u) != 0};
        case 2u: { // ASR
            const u32 sign = val >> 31;
            if (amount == 0u) return {sign ? 0xFFFFFFFFu : 0u, sign != 0};
            return {static_cast<u32>(static_cast<i32>(val) >> amount), ((val >> (amount - 1u)) & 1u) != 0};
        }
        case 3u: // ROR / RRX
        default:
            if (amount == 0u) {
                return {(static_cast<u32>(carry_in) << 31) | (val >> 1), (val & 1u) != 0};
            }
            {
                const u32 res = rotr32(val, amount);
                return {res, (res >> 31) != 0};
            }
    }
}

ShiftResult barrel_shift_reg(u32 val, u32 type, u32 amount, bool carry_in) {
    amount &= 0xFFu;
    if (amount == 0u) return {val, carry_in};

    switch (type & 3u) {
        case 0u: // LSL
            if (amount < 32u) return {val << amount, ((val >> (32u - amount)) & 1u) != 0};
            if (amount == 32u) return {0u, (val & 1u) != 0};
            return {0u, false};
        case 1u: // LSR
            if (amount < 32u) return {val >> amount, ((val >> (amount - 1u)) & 1u) != 0};
            if (amount == 32u) return {0u, (val >> 31) != 0};
            return {0u, false};
        case 2u: { // ASR
            const u32 sign = val >> 31;
            if (amount < 32u) {
                return {static_cast<u32>(static_cast<i32>(val) >> amount),
                        ((val >> (amount - 1u)) & 1u) != 0};
            }
            return {sign ? 0xFFFFFFFFu : 0u, sign != 0};
        }
        case 3u: { // ROR
            const u32 s = amount & 31u;
            if (s == 0u) return {val, (val >> 31) != 0};
            const u32 res = rotr32(val, s);
            return {res, (res >> 31) != 0};
        }
        default:
            return {val, carry_in};
    }
}

} // namespace

void Arm7Cpu::reset(u32 entry_pc) {
    r_.fill(0u);
    usr_r8_12_.fill(0u);
    fiq_r8_12_.fill(0u);
    bank_sp_.fill(0u);
    bank_lr_.fill(0u);
    bank_spsr_.fill(0u);

    // Default GBA post-BIOS stack pointers per bank
    bank_sp_[0] = 0x03007F00u; // User / System
    bank_sp_[1] = 0x03007F00u; // FIQ
    bank_sp_[2] = 0x03007FA0u; // IRQ
    bank_sp_[3] = 0x03007FE0u; // SVC
    bank_sp_[4] = 0x03007FE0u; // ABT
    bank_sp_[5] = 0x03007FE0u; // UND

    cpsr_ = static_cast<u32>(Mode::System);
    r_[13] = bank_sp_[0];
    r_[14] = 0u;
    r_[15] = entry_pc & ~1u;
    if (entry_pc & 1u) {
        cpsr_ |= FLAG_T;
    }

    halted_ = false;
    intr_wait_mask_ = 0u;
    intr_wait_resume_pc_ = 0u;
    insn_count_ = 0u;
}

int Arm7Cpu::mode_to_bank(Mode m) {
    switch (m) {
        case Mode::Fiq:   return 1;
        case Mode::Irq:   return 2;
        case Mode::Svc:   return 3;
        case Mode::Abort: return 4;
        case Mode::Undef: return 5;
        case Mode::User:
        case Mode::System:
        default:          return 0;
    }
}

void Arm7Cpu::switch_bank(Mode old_mode, Mode new_mode) {
    const int old_b = mode_to_bank(old_mode);
    const int new_b = mode_to_bank(new_mode);
    if (old_b == new_b) return;

    bank_sp_[old_b] = r_[13];
    bank_lr_[old_b] = r_[14];
    if (old_b == 1) {
        for (int i = 0; i < 5; ++i) fiq_r8_12_[i] = r_[8 + i];
        for (int i = 0; i < 5; ++i) r_[8 + i] = usr_r8_12_[i];
    } else if (new_b == 1) {
        for (int i = 0; i < 5; ++i) usr_r8_12_[i] = r_[8 + i];
        for (int i = 0; i < 5; ++i) r_[8 + i] = fiq_r8_12_[i];
    }

    r_[13] = bank_sp_[new_b];
    r_[14] = bank_lr_[new_b];
}

void Arm7Cpu::set_cpsr(u32 value) {
    const Mode old_m = mode();
    const u32 masked = value & 0xF00000FFu;
    Mode new_m = static_cast<Mode>(masked & 0x1Fu);
    switch (new_m) {
        case Mode::User:
        case Mode::Fiq:
        case Mode::Irq:
        case Mode::Svc:
        case Mode::Abort:
        case Mode::Undef:
        case Mode::System:
            break;
        default:
            new_m = Mode::System;
            break;
    }
    switch_bank(old_m, new_m);
    cpsr_ = (masked & ~0x1Fu) | static_cast<u32>(new_m);
}

u32 Arm7Cpu::spsr() const {
    const int b = mode_to_bank(mode());
    if (b == 0) return cpsr_;
    return bank_spsr_[b];
}

void Arm7Cpu::set_spsr(u32 value) {
    const int b = mode_to_bank(mode());
    if (b != 0) {
        bank_spsr_[b] = value & 0xF00000FFu;
    }
}

u32 Arm7Cpu::read_user_reg(u32 idx) const {
    idx &= 0xFu;
    const int b = mode_to_bank(mode());
    if (b == 0) return r_[idx];
    if (b == 1 && idx >= 8 && idx <= 12) return usr_r8_12_[idx - 8];
    if (idx == 13) return bank_sp_[0];
    if (idx == 14) return bank_lr_[0];
    return r_[idx];
}

void Arm7Cpu::write_user_reg(u32 idx, u32 val) {
    idx &= 0xFu;
    const int b = mode_to_bank(mode());
    if (b == 0) {
        r_[idx] = val;
        return;
    }
    if (b == 1 && idx >= 8 && idx <= 12) {
        usr_r8_12_[idx - 8] = val;
        return;
    }
    if (idx == 13) {
        bank_sp_[0] = val;
        return;
    }
    if (idx == 14) {
        bank_lr_[0] = val;
        return;
    }
    r_[idx] = val;
}

void Arm7Cpu::set_nz(u32 result) {
    cpsr_ &= ~(FLAG_N | FLAG_Z);
    if (result & 0x80000000u) cpsr_ |= FLAG_N;
    if (result == 0u) cpsr_ |= FLAG_Z;
}

void Arm7Cpu::set_nzcv(u32 result, bool carry, bool overflow) {
    cpsr_ &= ~(FLAG_N | FLAG_Z | FLAG_C | FLAG_V);
    if (result & 0x80000000u) cpsr_ |= FLAG_N;
    if (result == 0u) cpsr_ |= FLAG_Z;
    if (carry) cpsr_ |= FLAG_C;
    if (overflow) cpsr_ |= FLAG_V;
}

bool Arm7Cpu::eval_cond(u32 cond) const {
    const bool N = flag_n();
    const bool Z = flag_z();
    const bool C = flag_c();
    const bool V = flag_v();
    switch (cond & 0xFu) {
        case 0x0u: return Z;
        case 0x1u: return !Z;
        case 0x2u: return C;
        case 0x3u: return !C;
        case 0x4u: return N;
        case 0x5u: return !N;
        case 0x6u: return V;
        case 0x7u: return !V;
        case 0x8u: return C && !Z;
        case 0x9u: return !C || Z;
        case 0xAu: return N == V;
        case 0xBu: return N != V;
        case 0xCu: return !Z && (N == V);
        case 0xDu: return Z || (N != V);
        case 0xEu: return true;
        case 0xFu: default: return true;
    }
}

void Arm7Cpu::jump_to(u32 addr, bool thumb) {
    if (thumb) {
        cpsr_ |= FLAG_T;
        r_[15] = addr & ~1u;
    } else {
        cpsr_ &= ~FLAG_T;
        r_[15] = addr & ~3u;
    }
}

void Arm7Cpu::begin_intr_wait(u16 irq_mask, u32 resume_pc) {
    intr_wait_mask_ = irq_mask;
    intr_wait_resume_pc_ = resume_pc;
}

bool Arm7Cpu::service_irq_if_pending(hal::GbaBus& bus) {
    const u16 ie = bus.io_read16(hal::reg::IE);
    const u16 if_reg = bus.io_read16(hal::reg::IF);
    const bool irq_pending = (ie & if_reg) != 0;

    if (bus.halt_requested()) {
        halted_ = true;
        bus.clear_halt_request();
    }

    if (irq_pending) {
        halted_ = false;
    } else {
        return false;
    }

    if ((cpsr_ & FLAG_I) != 0) {
        return false;
    }
    if ((bus.io_read16(hal::reg::IME) & 1u) == 0) {
        return false;
    }

    // Standard ARM7TDMI IRQ vectoring: enter IRQ mode, mask I, enter ARM state
    const u32 old_cpsr = cpsr_;
    const u32 ret_lr = r_[15] + 4u; // Works for both ARM & Thumb return via subs pc, lr, #4

    set_cpsr((old_cpsr & ~0x1Fu) | static_cast<u32>(Mode::Irq));
    cpsr_ |= FLAG_I;
    cpsr_ &= ~FLAG_T;
    set_spsr(old_cpsr);
    r_[14] = ret_lr;
    r_[15] = 0x00000018u; // BIOS IRQ trampoline -> [0x03007FFC] -> 0x00000138 epilogue
    return true;
}

u32 Arm7Cpu::step_one(hal::GbaBus& bus) {
    if (service_irq_if_pending(bus)) {
        // Vector transition consumed setup cycles
        return 7u;
    }

    // Complete pending IntrWait / VBlankIntrWait once we return to resume PC in non-IRQ mode
    if (intr_wait_mask_ != 0u && !halted_ && mode() != Mode::Irq &&
        (r_[15] & ~1u) == (intr_wait_resume_pc_ & ~1u)) {
        const u16 chk = bus.read16(0x03007FF8u);
        if ((chk & intr_wait_mask_) != 0u) {
            bus.write16(0x03007FF8u, static_cast<u16>(chk & ~intr_wait_mask_));
            intr_wait_mask_ = 0u;
        } else {
            // Spurious wake (e.g., HBlank/Timer IRQ arrived while waiting for VBlank): re-halt
            bus.io_write16(hal::reg::IME, 1u);
            cpsr_ &= ~FLAG_I;
            halted_ = true;
            return 4u;
        }
    }

    if (halted_) {
        return 4u;
    }

    if (!pc_hooks_.empty()) {
        const auto it = pc_hooks_.find(r_[15] & ~1u);
        if (it != pc_hooks_.end() && it->second(*this, bus)) {
            ++insn_count_;
            return 4u;
        }
    }

    ++insn_count_;
    if (in_thumb()) {
        const u32 instr_pc = r_[15] & ~1u;
        r_[15] = instr_pc + 2u;
        const u16 op = bus.read16(instr_pc);
        return exec_thumb(bus, op, instr_pc);
    } else {
        const u32 instr_pc = r_[15] & ~3u;
        r_[15] = instr_pc + 4u;
        const u32 op = bus.read32(instr_pc);
        return exec_arm(bus, op, instr_pc);
    }
}

u32 Arm7Cpu::step(hal::GbaBus& bus, u32 cycle_budget) {
    u32 spent = 0u;
    while (spent < cycle_budget) {
        if (halted_) {
            // Check once if halted without pending IRQ; fast-forward remainder of slice!
            if (!service_irq_if_pending(bus)) {
                const u32 rem = cycle_budget - spent;
                bus.step_timers(rem);
                spent += rem;
                // Re-check IRQ at slice end in case timer overflow just raised TIMER IRQ
                service_irq_if_pending(bus);
                break;
            }
        }
        const u32 cy = step_one(bus);
        bus.step_timers(cy);
        spent += cy;
    }
    return spent;
}

u32 Arm7Cpu::exec_arm(hal::GbaBus& bus, u32 op, u32 instr_pc) {
    const u32 cond = op >> 28;
    if (cond != 0xEu && !eval_cond(cond)) {
        return 1u;
    }

    auto read_arm_reg = [&](u32 r, bool pc_plus_12 = false) -> u32 {
        if (r == 15u) return instr_pc + (pc_plus_12 ? 12u : 8u);
        return r_[r];
    };

    // 1. SWI (bits 27:24 == 1111)
    if ((op & 0x0F000000u) == 0x0F000000u) {
        const u8 imm = static_cast<u8>((op >> 16) & 0xFFu);
        GbaBiosHle::dispatch(imm, *this, bus);
        return 3u;
    }

    // 2. Branch and Exchange: BX Rm (0x012FFF1x)
    if ((op & 0x0FFFFFF0u) == 0x012FFF10u) {
        const u32 target = read_arm_reg(op & 0xFu);
        jump_to(target, (target & 1u) != 0);
        return 3u;
    }

    // 3. Branch / Branch with Link: B / BL (bits 27:25 == 101)
    if ((op & 0x0E000000u) == 0x0A000000u) {
        const bool link = (op & 0x01000000u) != 0;
        const i32 offset = static_cast<i32>(sign_extend(op & 0x00FFFFFFu, 24) << 2);
        if (link) {
            r_[14] = instr_pc + 4u;
        }
        r_[15] = static_cast<u32>(static_cast<i32>(instr_pc + 8u) + offset) & ~3u;
        return 3u;
    }

    // 4. Block Data Transfer: LDM / STM (bits 27:25 == 100)
    if ((op & 0x0E000000u) == 0x08000000u) {
        const bool pre   = (op & (1u << 24)) != 0;
        const bool up    = (op & (1u << 23)) != 0;
        const bool s_bit = (op & (1u << 22)) != 0;
        const bool wb    = (op & (1u << 21)) != 0;
        const bool load  = (op & (1u << 20)) != 0;
        const u32 rn     = (op >> 16) & 0xFu;
        const u32 rlist  = op & 0xFFFFu;

        u32 base = read_arm_reg(rn);
        const u32 orig_base = base;
        u32 count = 0u;
        for (u32 i = 0; i < 16u; ++i) {
            if (rlist & (1u << i)) ++count;
        }

        const u32 bytes = (count == 0u) ? 64u : count * 4u;
        u32 addr = up ? (pre ? base + 4u : base) : (pre ? base - bytes : base - bytes + 4u);
        const u32 final_base = up ? (base + bytes) : (base - bytes);

        if (count == 0u) {
            // ARM7TDMI empty rlist edge case: transfers R15, adjusts base by +/-0x40
            if (load) {
                r_[15] = bus.read32(addr) & ~3u;
            } else {
                bus.write32(addr, instr_pc + 12u);
            }
            if (wb) r_[rn] = final_base;
            return 4u;
        }

        const bool user_bank = s_bit && !(load && (rlist & (1u << 15)));
        bool first_stored = true;
        for (u32 i = 0; i < 16u; ++i) {
            if (!(rlist & (1u << i))) continue;
            if (load) {
                const u32 val = bus.read32(addr);
                if (i == 15u) {
                    if (s_bit) {
                        const u32 restored_cpsr = spsr();
                        set_cpsr(restored_cpsr);
                        r_[15] = val & (in_thumb() ? ~1u : ~3u);
                    } else {
                        r_[15] = val & ~3u;
                    }
                } else if (user_bank) {
                    write_user_reg(i, val);
                } else {
                    r_[i] = val;
                }
            } else {
                u32 val = 0u;
                if (i == 15u) {
                    val = instr_pc + 12u;
                } else if (user_bank) {
                    val = read_user_reg(i);
                } else if (i == rn && wb) {
                    val = first_stored ? orig_base : final_base;
                } else {
                    val = r_[i];
                }
                bus.write32(addr, val);
                first_stored = false;
            }
            addr += 4u;
        }

        if (wb && !(load && (rlist & (1u << rn)))) {
            r_[rn] = final_base;
        }
        return count + 2u;
    }

    // 5. SWP / SWPB (00010 B 00 Rn Rd 0000 1001 Rm)
    if ((op & 0x0FB00FF0u) == 0x01000090u) {
        const bool is_byte = (op & (1u << 22)) != 0;
        const u32 rn = (op >> 16) & 0xFu;
        const u32 rd = (op >> 12) & 0xFu;
        const u32 rm = op & 0xFu;
        const u32 addr = read_arm_reg(rn);
        const u32 src = read_arm_reg(rm);
        if (is_byte) {
            const u8 old_b = bus.read8(addr);
            bus.write8(addr, static_cast<u8>(src & 0xFFu));
            r_[rd] = old_b;
        } else {
            const u32 old_w = arm_read32_rot(bus, addr);
            bus.write32(addr, src);
            r_[rd] = old_w;
        }
        return 4u;
    }

    // 6. Multiply & Multiply-Long (bits 27:24 == 0000 and bits 7:4 == 1001)
    if ((op & 0x0F0000F0u) == 0x00000090u) {
        const bool set_flags = (op & (1u << 20)) != 0;
        const u32 rd = (op >> 16) & 0xFu;
        const u32 rn = (op >> 12) & 0xFu;
        const u32 rs = (op >> 8) & 0xFu;
        const u32 rm = op & 0xFu;

        if ((op & 0x0FC000F0u) == 0x00000090u) {
            // MUL / MLA
            const bool acc = (op & (1u << 21)) != 0;
            u32 res = read_arm_reg(rm) * read_arm_reg(rs);
            if (acc) res += read_arm_reg(rn);
            r_[rd] = res;
            if (set_flags) set_nz(res);
            return 2u;
        }

        if ((op & 0x0F8000F0u) == 0x00800090u) {
            // UMULL / UMLAL / SMULL / SMLAL (rd = RdHi, rn = RdLo)
            const bool is_signed = (op & (1u << 22)) != 0;
            const bool acc       = (op & (1u << 21)) != 0;
            u64 res = 0u;
            if (is_signed) {
                const i64 prod = static_cast<i64>(static_cast<i32>(read_arm_reg(rm))) *
                                 static_cast<i64>(static_cast<i32>(read_arm_reg(rs)));
                if (acc) {
                    const u64 cur = (static_cast<u64>(read_arm_reg(rd)) << 32) | read_arm_reg(rn);
                    res = static_cast<u64>(static_cast<i64>(cur) + prod);
                } else {
                    res = static_cast<u64>(prod);
                }
            } else {
                res = static_cast<u64>(read_arm_reg(rm)) * static_cast<u64>(read_arm_reg(rs));
                if (acc) {
                    const u64 cur = (static_cast<u64>(read_arm_reg(rd)) << 32) | read_arm_reg(rn);
                    res += cur;
                }
            }
            r_[rn] = static_cast<u32>(res & 0xFFFFFFFFu);
            r_[rd] = static_cast<u32>(res >> 32);
            if (set_flags) {
                cpsr_ &= ~(FLAG_N | FLAG_Z);
                if (res >> 63) cpsr_ |= FLAG_N;
                if (res == 0u) cpsr_ |= FLAG_Z;
            }
            return 3u;
        }
    }

    // 7. Halfword & Signed Data Transfer: LDRH/STRH/LDRSB/LDRSH (bits 27:25 == 000, bit 7=1, bit 4=1)
    if ((op & 0x0E000090u) == 0x00000090u && ((op >> 5) & 3u) != 0u) {
        const bool pre   = (op & (1u << 24)) != 0;
        const bool up    = (op & (1u << 23)) != 0;
        const bool imm_f = (op & (1u << 22)) != 0;
        const bool wb    = (op & (1u << 21)) != 0;
        const bool load  = (op & (1u << 20)) != 0;
        const u32 rn     = (op >> 16) & 0xFu;
        const u32 rd     = (op >> 12) & 0xFu;
        const u32 sh     = (op >> 5) & 3u;

        const u32 offset = imm_f
            ? (((op >> 4) & 0xF0u) | (op & 0x0Fu))
            : read_arm_reg(op & 0xFu);

        const u32 base = read_arm_reg(rn);
        const u32 ea = up ? (base + offset) : (base - offset);
        const u32 addr = pre ? ea : base;

        if (!pre || wb) {
            r_[rn] = ea;
        }

        if (load) {
            u32 val = 0u;
            switch (sh) {
                case 1u: val = arm_read16_rot(bus, addr); break;       // LDRH
                case 2u: val = sign_extend(bus.read8(addr), 8); break; // LDRSB
                case 3u: val = arm_read_s16_rot(bus, addr); break;     // LDRSH
                default: break;
            }
            if (rd == 15u) r_[15] = val & ~3u;
            else r_[rd] = val;
        } else if (sh == 1u) {
            const u16 val = static_cast<u16>((rd == 15u ? (instr_pc + 12u) : r_[rd]) & 0xFFFFu);
            bus.write16(addr & ~1u, val);
        }
        return 3u;
    }

    // 8. Single Data Transfer: LDR/STR/LDRB/STRB (bits 27:26 == 01)
    if ((op & 0x0C000000u) == 0x04000000u) {
        const bool reg_off = (op & (1u << 25)) != 0;
        const bool pre     = (op & (1u << 24)) != 0;
        const bool up      = (op & (1u << 23)) != 0;
        const bool is_byte = (op & (1u << 22)) != 0;
        const bool wb      = (op & (1u << 21)) != 0;
        const bool load    = (op & (1u << 20)) != 0;
        const u32 rn       = (op >> 16) & 0xFu;
        const u32 rd       = (op >> 12) & 0xFu;

        u32 offset = 0u;
        if (!reg_off) {
            offset = op & 0xFFFu;
        } else {
            const u32 rm = op & 0xFu;
            const u32 stype = (op >> 5) & 3u;
            const u32 samount = (op >> 7) & 0x1Fu;
            offset = barrel_shift_imm(read_arm_reg(rm), stype, samount, flag_c()).val;
        }

        const u32 base = read_arm_reg(rn);
        const u32 ea = up ? (base + offset) : (base - offset);
        const u32 addr = pre ? ea : base;
        const u32 store_val = (rd == 15u) ? (instr_pc + 12u) : r_[rd];

        if (!pre || wb) {
            r_[rn] = ea;
        }

        if (load) {
            const u32 val = is_byte ? static_cast<u32>(bus.read8(addr)) : arm_read32_rot(bus, addr);
            if (rd == 15u) r_[15] = val & ~3u;
            else r_[rd] = val;
        } else {
            if (is_byte) bus.write8(addr, static_cast<u8>(store_val & 0xFFu));
            else bus.write32(addr & ~3u, store_val);
        }
        return 3u;
    }

    // 9. PSR Transfer: MRS / MSR (bits 27:26 == 00, S=0, opcode in 10xx)
    if ((op & 0x0FB00000u) == 0x01000000u || (op & 0x0FB00000u) == 0x01200000u ||
        (op & 0x0FB00000u) == 0x03200000u) {
        const bool use_spsr = (op & (1u << 22)) != 0;
        const bool to_psr   = (op & (1u << 21)) != 0;
        if (!to_psr && (op & 0x00000FFFu) == 0u && ((op >> 16) & 0xFu) == 0xFu) {
            // MRS Rd, CPSR/SPSR
            const u32 rd = (op >> 12) & 0xFu;
            r_[rd] = use_spsr ? spsr() : cpsr_;
            return 1u;
        }
        if (to_psr && ((op >> 12) & 0xFu) == 0xFu) {
            // MSR CPSR/SPSR_<fields>, Rm or #imm
            const u32 src = (op & (1u << 25))
                ? rotr32(op & 0xFFu, ((op >> 8) & 0xFu) * 2u)
                : read_arm_reg(op & 0xFu);
            u32 mask = 0u;
            if (op & (1u << 16)) mask |= 0x000000FFu; // c
            if (op & (1u << 19)) mask |= 0xF0000000u; // f
            if (use_spsr) {
                set_spsr((spsr() & ~mask) | (src & mask));
            } else {
                // Privileged modes can modify control bits too
                if (mode() == Mode::User) mask &= 0xF0000000u;
                set_cpsr((cpsr_ & ~mask) | (src & mask));
            }
            return 1u;
        }
    }

    // 10. Data Processing (bits 27:26 == 00)
    if ((op & 0x0C000000u) == 0x00000000u) {
        const bool imm_op    = (op & (1u << 25)) != 0;
        const u32 opcode     = (op >> 21) & 0xFu;
        const bool set_flags = (op & (1u << 20)) != 0;
        const u32 rn         = (op >> 16) & 0xFu;
        const u32 rd         = (op >> 12) & 0xFu;

        bool reg_shift = false;
        ShiftResult op2{0u, flag_c()};
        if (imm_op) {
            const u32 imm8 = op & 0xFFu;
            const u32 rot = ((op >> 8) & 0xFu) * 2u;
            const u32 val = rotr32(imm8, rot);
            op2 = {val, (rot != 0u) ? ((val >> 31) != 0) : flag_c()};
        } else {
            const u32 rm = op & 0xFu;
            const u32 stype = (op >> 5) & 3u;
            if (op & (1u << 4)) {
                reg_shift = true;
                const u32 rs = (op >> 8) & 0xFu;
                op2 = barrel_shift_reg(read_arm_reg(rm, true), stype, read_arm_reg(rs, true), flag_c());
            } else {
                const u32 samount = (op >> 7) & 0x1Fu;
                op2 = barrel_shift_imm(read_arm_reg(rm, false), stype, samount, flag_c());
            }
        }

        const u32 a = read_arm_reg(rn, reg_shift);
        const u32 b = op2.val;
        u32 res = 0u;
        bool write_rd = true;
        bool is_arith = false;
        bool carry = op2.carry;
        bool overflow = flag_v();

        switch (opcode) {
            case 0x0u: res = a & b; break;                                   // AND
            case 0x1u: res = a ^ b; break;                                   // EOR
            case 0x2u: { // SUB
                res = a - b;
                is_arith = true;
                carry = (a >= b);
                overflow = (((a ^ b) & (a ^ res)) >> 31) != 0;
                break;
            }
            case 0x3u: { // RSB
                res = b - a;
                is_arith = true;
                carry = (b >= a);
                overflow = (((b ^ a) & (b ^ res)) >> 31) != 0;
                break;
            }
            case 0x4u: { // ADD
                const u64 s = static_cast<u64>(a) + static_cast<u64>(b);
                res = static_cast<u32>(s);
                is_arith = true;
                carry = (s >> 32) != 0;
                overflow = ((~(a ^ b) & (a ^ res)) >> 31) != 0;
                break;
            }
            case 0x5u: { // ADC
                const u64 s = static_cast<u64>(a) + static_cast<u64>(b) + (flag_c() ? 1u : 0u);
                res = static_cast<u32>(s);
                is_arith = true;
                carry = (s >> 32) != 0;
                overflow = ((~(a ^ b) & (a ^ res)) >> 31) != 0;
                break;
            }
            case 0x6u: { // SBC
                const u64 s = static_cast<u64>(a) + static_cast<u64>(~b) + (flag_c() ? 1u : 0u);
                res = static_cast<u32>(s);
                is_arith = true;
                carry = (s >> 32) != 0;
                overflow = (((a ^ b) & (a ^ res)) >> 31) != 0;
                break;
            }
            case 0x7u: { // RSC
                const u64 s = static_cast<u64>(b) + static_cast<u64>(~a) + (flag_c() ? 1u : 0u);
                res = static_cast<u32>(s);
                is_arith = true;
                carry = (s >> 32) != 0;
                overflow = (((b ^ a) & (b ^ res)) >> 31) != 0;
                break;
            }
            case 0x8u: res = a & b; write_rd = false; break;                 // TST
            case 0x9u: res = a ^ b; write_rd = false; break;                 // TEQ
            case 0xAu: { // CMP
                res = a - b;
                write_rd = false;
                is_arith = true;
                carry = (a >= b);
                overflow = (((a ^ b) & (a ^ res)) >> 31) != 0;
                break;
            }
            case 0xBu: { // CMN
                const u64 s = static_cast<u64>(a) + static_cast<u64>(b);
                res = static_cast<u32>(s);
                write_rd = false;
                is_arith = true;
                carry = (s >> 32) != 0;
                overflow = ((~(a ^ b) & (a ^ res)) >> 31) != 0;
                break;
            }
            case 0xCu: res = a | b; break;                                   // ORR
            case 0xDu: res = b; break;                                       // MOV
            case 0xEu: res = a & ~b; break;                                  // BIC
            case 0xFu: res = ~b; break;                                      // MVN
            default: break;
        }

        if (set_flags) {
            if (rd == 15u && write_rd) {
                set_cpsr(spsr());
            } else if (is_arith) {
                set_nzcv(res, carry, overflow);
            } else {
                set_nzcv(res, carry, flag_v());
            }
        }

        if (write_rd) {
            if (rd == 15u) {
                r_[15] = res & (in_thumb() ? ~1u : ~3u);
                return 3u;
            }
            r_[rd] = res;
        }
        return reg_shift ? 2u : 1u;
    }

    return 1u;
}

u32 Arm7Cpu::exec_thumb(hal::GbaBus& bus, u16 op, u32 instr_pc) {
    // 1. Move shifted register (000xx, xx != 11)
    if ((op & 0xE000u) == 0x0000u && (op & 0x1800u) != 0x1800u) {
        const u32 stype  = (op >> 11) & 3u;
        const u32 offset = (op >> 6) & 0x1Fu;
        const u32 rs     = (op >> 3) & 7u;
        const u32 rd     = op & 7u;
        const ShiftResult sr = barrel_shift_imm(r_[rs], stype, offset, flag_c());
        r_[rd] = sr.val;
        set_nzcv(sr.val, sr.carry, flag_v());
        return 1u;
    }

    // 2. Add/Subtract 3-bit reg/imm (00011)
    if ((op & 0xF800u) == 0x1800u) {
        const bool is_imm = (op & (1u << 10)) != 0;
        const bool is_sub = (op & (1u << 9)) != 0;
        const u32 rn_imm  = (op >> 6) & 7u;
        const u32 rs      = (op >> 3) & 7u;
        const u32 rd      = op & 7u;
        const u32 a = r_[rs];
        const u32 b = is_imm ? rn_imm : r_[rn_imm];
        if (is_sub) {
            const u32 res = a - b;
            r_[rd] = res;
            set_nzcv(res, a >= b, (((a ^ b) & (a ^ res)) >> 31) != 0);
        } else {
            const u64 s = static_cast<u64>(a) + static_cast<u64>(b);
            const u32 res = static_cast<u32>(s);
            r_[rd] = res;
            set_nzcv(res, (s >> 32) != 0, ((~(a ^ b) & (a ^ res)) >> 31) != 0);
        }
        return 1u;
    }

    // 3. Mov/Cmp/Add/Sub 8-bit immediate (001xx)
    if ((op & 0xE000u) == 0x2000u) {
        const u32 opcode = (op >> 11) & 3u;
        const u32 rd     = (op >> 8) & 7u;
        const u32 imm    = op & 0xFFu;
        const u32 a      = r_[rd];
        switch (opcode) {
            case 0u: // MOV
                r_[rd] = imm;
                set_nz(imm);
                break;
            case 1u: { // CMP
                const u32 res = a - imm;
                set_nzcv(res, a >= imm, (((a ^ imm) & (a ^ res)) >> 31) != 0);
                break;
            }
            case 2u: { // ADD
                const u64 s = static_cast<u64>(a) + static_cast<u64>(imm);
                const u32 res = static_cast<u32>(s);
                r_[rd] = res;
                set_nzcv(res, (s >> 32) != 0, ((~(a ^ imm) & (a ^ res)) >> 31) != 0);
                break;
            }
            case 3u: { // SUB
                const u32 res = a - imm;
                r_[rd] = res;
                set_nzcv(res, a >= imm, (((a ^ imm) & (a ^ res)) >> 31) != 0);
                break;
            }
            default: break;
        }
        return 1u;
    }

    // 4. ALU Operations (010000)
    if ((op & 0xFC00u) == 0x4000u) {
        const u32 opcode = (op >> 6) & 0xFu;
        const u32 rs     = (op >> 3) & 7u;
        const u32 rd     = op & 7u;
        const u32 a = r_[rd];
        const u32 b = r_[rs];
        switch (opcode) {
            case 0x0u: r_[rd] = a & b; set_nz(r_[rd]); break; // AND
            case 0x1u: r_[rd] = a ^ b; set_nz(r_[rd]); break; // EOR
            case 0x2u: { // LSL
                const ShiftResult sr = barrel_shift_reg(a, 0u, b, flag_c());
                r_[rd] = sr.val;
                set_nzcv(sr.val, sr.carry, flag_v());
                break;
            }
            case 0x3u: { // LSR
                const ShiftResult sr = barrel_shift_reg(a, 1u, b, flag_c());
                r_[rd] = sr.val;
                set_nzcv(sr.val, sr.carry, flag_v());
                break;
            }
            case 0x4u: { // ASR
                const ShiftResult sr = barrel_shift_reg(a, 2u, b, flag_c());
                r_[rd] = sr.val;
                set_nzcv(sr.val, sr.carry, flag_v());
                break;
            }
            case 0x5u: { // ADC
                const u64 s = static_cast<u64>(a) + static_cast<u64>(b) + (flag_c() ? 1u : 0u);
                const u32 res = static_cast<u32>(s);
                r_[rd] = res;
                set_nzcv(res, (s >> 32) != 0, ((~(a ^ b) & (a ^ res)) >> 31) != 0);
                break;
            }
            case 0x6u: { // SBC
                const u64 s = static_cast<u64>(a) + static_cast<u64>(~b) + (flag_c() ? 1u : 0u);
                const u32 res = static_cast<u32>(s);
                r_[rd] = res;
                set_nzcv(res, (s >> 32) != 0, (((a ^ b) & (a ^ res)) >> 31) != 0);
                break;
            }
            case 0x7u: { // ROR
                const ShiftResult sr = barrel_shift_reg(a, 3u, b, flag_c());
                r_[rd] = sr.val;
                set_nzcv(sr.val, sr.carry, flag_v());
                break;
            }
            case 0x8u: set_nz(a & b); break; // TST
            case 0x9u: { // NEG (0 - b)
                const u32 res = 0u - b;
                r_[rd] = res;
                set_nzcv(res, 0u >= b, ((b & res) >> 31) != 0);
                break;
            }
            case 0xAu: { // CMP
                const u32 res = a - b;
                set_nzcv(res, a >= b, (((a ^ b) & (a ^ res)) >> 31) != 0);
                break;
            }
            case 0xBu: { // CMN
                const u64 s = static_cast<u64>(a) + static_cast<u64>(b);
                const u32 res = static_cast<u32>(s);
                set_nzcv(res, (s >> 32) != 0, ((~(a ^ b) & (a ^ res)) >> 31) != 0);
                break;
            }
            case 0xCu: r_[rd] = a | b; set_nz(r_[rd]); break;  // ORR
            case 0xDu: r_[rd] = a * b; set_nz(r_[rd]); break;  // MUL
            case 0xEu: r_[rd] = a & ~b; set_nz(r_[rd]); break; // BIC
            case 0xFu: r_[rd] = ~b; set_nz(r_[rd]); break;     // MVN
            default: break;
        }
        return 1u;
    }

    // 5. Hi register operations / BX (010001)
    if ((op & 0xFC00u) == 0x4400u) {
        const u32 opcode = (op >> 8) & 3u;
        const u32 h1     = (op >> 7) & 1u;
        const u32 h2     = (op >> 6) & 1u;
        const u32 rs     = ((op >> 3) & 7u) | (h2 << 3);
        const u32 rd     = (op & 7u) | (h1 << 3);
        const u32 vs     = (rs == 15u) ? (instr_pc + 4u) : r_[rs];
        const u32 vd     = (rd == 15u) ? (instr_pc + 4u) : r_[rd];

        switch (opcode) {
            case 0u: { // ADD
                const u32 res = vd + vs;
                if (rd == 15u) r_[15] = res & ~1u;
                else r_[rd] = res;
                break;
            }
            case 1u: { // CMP
                const u32 res = vd - vs;
                set_nzcv(res, vd >= vs, (((vd ^ vs) & (vd ^ res)) >> 31) != 0);
                break;
            }
            case 2u: // MOV
                if (rd == 15u) r_[15] = vs & ~1u;
                else r_[rd] = vs;
                break;
            case 3u: // BX
                jump_to(vs, (vs & 1u) != 0);
                return 3u;
            default: break;
        }
        return 1u;
    }

    // 6. PC-relative load: LDR Rd, [PC, #imm] (01001)
    if ((op & 0xF800u) == 0x4800u) {
        const u32 rd = (op >> 8) & 7u;
        const u32 imm = (op & 0xFFu) << 2;
        const u32 addr = ((instr_pc + 4u) & ~3u) + imm;
        r_[rd] = bus.read32(addr);
        return 3u;
    }

    // 7 & 8. Load/store with register offset (0101xx)
    if ((op & 0xF000u) == 0x5000u) {
        const u32 ro = (op >> 6) & 7u;
        const u32 rb = (op >> 3) & 7u;
        const u32 rd = op & 7u;
        const u32 addr = r_[rb] + r_[ro];
        if ((op & (1u << 9)) == 0) {
            const u32 opcode = (op >> 10) & 3u;
            switch (opcode) {
                case 0u: bus.write32(addr & ~3u, r_[rd]); break;                     // STR
                case 1u: bus.write8(addr, static_cast<u8>(r_[rd] & 0xFFu)); break;   // STRB
                case 2u: r_[rd] = arm_read32_rot(bus, addr); break;                  // LDR
                case 3u: r_[rd] = bus.read8(addr); break;                            // LDRB
                default: break;
            }
        } else {
            const u32 opcode = (op >> 10) & 3u;
            switch (opcode) {
                case 0u: bus.write16(addr & ~1u, static_cast<u16>(r_[rd])); break;   // STRH
                case 1u: r_[rd] = sign_extend(bus.read8(addr), 8); break;            // LDSB
                case 2u: r_[rd] = arm_read16_rot(bus, addr); break;                  // LDRH
                case 3u: r_[rd] = arm_read_s16_rot(bus, addr); break;                // LDSH
                default: break;
            }
        }
        return 3u;
    }

    // 9. Load/store immediate offset (011xx)
    if ((op & 0xE000u) == 0x6000u) {
        const bool is_byte = (op & (1u << 12)) != 0;
        const bool load    = (op & (1u << 11)) != 0;
        const u32 imm5     = (op >> 6) & 0x1Fu;
        const u32 rb       = (op >> 3) & 7u;
        const u32 rd       = op & 7u;
        const u32 addr     = r_[rb] + (is_byte ? imm5 : (imm5 << 2));
        if (load) {
            r_[rd] = is_byte ? static_cast<u32>(bus.read8(addr)) : arm_read32_rot(bus, addr);
        } else {
            if (is_byte) bus.write8(addr, static_cast<u8>(r_[rd] & 0xFFu));
            else bus.write32(addr & ~3u, r_[rd]);
        }
        return 3u;
    }

    // 10. Load/store halfword (1000x)
    if ((op & 0xF000u) == 0x8000u) {
        const bool load = (op & (1u << 11)) != 0;
        const u32 imm   = ((op >> 6) & 0x1Fu) << 1;
        const u32 rb    = (op >> 3) & 7u;
        const u32 rd    = op & 7u;
        const u32 addr  = r_[rb] + imm;
        if (load) r_[rd] = arm_read16_rot(bus, addr);
        else bus.write16(addr & ~1u, static_cast<u16>(r_[rd]));
        return 3u;
    }

    // 11. SP-relative load/store (1001x)
    if ((op & 0xF000u) == 0x9000u) {
        const bool load = (op & (1u << 11)) != 0;
        const u32 rd    = (op >> 8) & 7u;
        const u32 addr  = r_[13] + ((op & 0xFFu) << 2);
        if (load) r_[rd] = arm_read32_rot(bus, addr);
        else bus.write32(addr & ~3u, r_[rd]);
        return 3u;
    }

    // 12. Load address (1010x)
    if ((op & 0xF000u) == 0xA000u) {
        const bool use_sp = (op & (1u << 11)) != 0;
        const u32 rd      = (op >> 8) & 7u;
        const u32 imm     = (op & 0xFFu) << 2;
        const u32 base    = use_sp ? r_[13] : ((instr_pc + 4u) & ~3u);
        r_[rd] = base + imm;
        return 1u;
    }

    // 13. Add offset to SP (10110000)
    if ((op & 0xFF00u) == 0xB000u) {
        const u32 imm = (op & 0x7Fu) << 2;
        if (op & 0x0080u) r_[13] -= imm;
        else r_[13] += imm;
        return 1u;
    }

    // 14. PUSH / POP (1011x10x)
    if ((op & 0xF600u) == 0xB400u) {
        const bool is_pop   = (op & (1u << 11)) != 0;
        const bool pc_or_lr = (op & (1u << 8)) != 0;
        const u32 rlist     = op & 0xFFu;

        if (is_pop) {
            for (u32 i = 0; i < 8u; ++i) {
                if (rlist & (1u << i)) {
                    r_[i] = bus.read32(r_[13]);
                    r_[13] += 4u;
                }
            }
            if (pc_or_lr) {
                r_[15] = bus.read32(r_[13]) & ~1u;
                r_[13] += 4u;
                return 5u;
            }
            return 4u;
        } else {
            u32 count = pc_or_lr ? 1u : 0u;
            for (u32 i = 0; i < 8u; ++i) {
                if (rlist & (1u << i)) ++count;
            }
            r_[13] -= count * 4u;
            u32 addr = r_[13];
            for (u32 i = 0; i < 8u; ++i) {
                if (rlist & (1u << i)) {
                    bus.write32(addr, r_[i]);
                    addr += 4u;
                }
            }
            if (pc_or_lr) {
                bus.write32(addr, r_[14]);
            }
            return 4u;
        }
    }

    // 15. Multiple load/store: STMIA / LDMIA (1100x)
    if ((op & 0xF000u) == 0xC000u) {
        const bool load  = (op & (1u << 11)) != 0;
        const u32 rb     = (op >> 8) & 7u;
        const u32 rlist  = op & 0xFFu;
        u32 addr = r_[rb];

        if (rlist == 0u) {
            if (load) r_[15] = bus.read32(addr) & ~1u;
            else bus.write32(addr, instr_pc + 6u);
            r_[rb] += 0x40u;
            return 4u;
        }

        const u32 orig_base = addr;
        u32 count = 0u;
        for (u32 i = 0; i < 8u; ++i) {
            if (rlist & (1u << i)) ++count;
        }
        const u32 final_base = orig_base + count * 4u;
        bool first = true;

        for (u32 i = 0; i < 8u; ++i) {
            if (!(rlist & (1u << i))) continue;
            if (load) {
                r_[i] = bus.read32(addr);
            } else {
                const u32 val = (i == rb) ? (first ? orig_base : final_base) : r_[i];
                bus.write32(addr, val);
                first = false;
            }
            addr += 4u;
        }

        if (!(load && (rlist & (1u << rb)))) {
            r_[rb] = final_base;
        }
        return count + 2u;
    }

    // 17. SWI (11011111)
    if ((op & 0xFF00u) == 0xDF00u) {
        GbaBiosHle::dispatch(static_cast<u8>(op & 0xFFu), *this, bus);
        return 3u;
    }

    // 16. Conditional Branch (1101xxxx, xxxx < 14)
    if ((op & 0xF000u) == 0xD000u) {
        const u32 cond = (op >> 8) & 0xFu;
        if (cond < 14u && eval_cond(cond)) {
            const i32 off = static_cast<i32>(sign_extend(op & 0xFFu, 8) << 1);
            r_[15] = static_cast<u32>(static_cast<i32>(instr_pc + 4u) + off) & ~1u;
            return 3u;
        }
        return 1u;
    }

    // 18. Unconditional Branch (11100)
    if ((op & 0xF800u) == 0xE000u) {
        const i32 off = static_cast<i32>(sign_extend(op & 0x7FFu, 11) << 1);
        r_[15] = static_cast<u32>(static_cast<i32>(instr_pc + 4u) + off) & ~1u;
        return 3u;
    }

    // 19. Long Branch with Link: BL prefix (11110) / BL suffix (11111)
    if ((op & 0xF000u) == 0xF000u) {
        const bool is_second = (op & (1u << 11)) != 0;
        const u32 imm11 = op & 0x7FFu;
        if (!is_second) {
            const i32 hi = static_cast<i32>(sign_extend(imm11, 11) << 12);
            r_[14] = static_cast<u32>(static_cast<i32>(instr_pc + 4u) + hi);
            return 1u;
        } else {
            const u32 target = (r_[14] + (imm11 << 1)) & ~1u;
            r_[14] = (instr_pc + 2u) | 1u;
            r_[15] = target;
            return 3u;
        }
    }

    return 1u;
}

} // namespace hm::cpu
