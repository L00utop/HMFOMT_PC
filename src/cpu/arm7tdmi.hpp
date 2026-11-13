#pragma once

#include "hal/gba_types.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <unordered_map>

namespace hm::hal {
class GbaBus;
}

namespace hm::cpu {

using hal::i16;
using hal::i32;
using hal::i64;
using hal::i8;
using hal::u16;
using hal::u32;
using hal::u64;
using hal::u8;

/// Clean C++17 ARM7TDMI (ARMv4T: ARM 32-bit + Thumb 16-bit) CPU scaffold
/// operating directly over `hm::hal::GbaBus`.
class Arm7Cpu {
public:
    enum class Mode : u32 {
        User   = 0x10u,
        Fiq    = 0x11u,
        Irq    = 0x12u,
        Svc    = 0x13u,
        Abort  = 0x17u,
        Undef  = 0x1Bu,
        System = 0x1Fu,
    };

    using PcHookFn = std::function<bool(Arm7Cpu&, hal::GbaBus&)>;

    Arm7Cpu() { reset(); }

    void reset(u32 entry_pc = hal::ADDR_ROM_BASE);

    /// Execute at most `cycle_budget` CPU cycles, advancing bus timers alongside.
    /// Returns actual cycles consumed (reaches budget cleanly when halted).
    u32 step(hal::GbaBus& bus, u32 cycle_budget);

    /// Execute a single instruction (or advance halt state); returns cycles spent.
    u32 step_one(hal::GbaBus& bus);

    /// Trigger a hardware IRQ if CPSR.I == 0, IME != 0, and (IE & IF) != 0.
    bool service_irq_if_pending(hal::GbaBus& bus);

    // Register & PSR accessors
    u32 reg(u32 idx) const { return r_[idx & 0xFu]; }
    void set_reg(u32 idx, u32 val) { r_[idx & 0xFu] = val; }
    u32 pc() const { return r_[15]; }
    u32 sp() const { return r_[13]; }
    u32 lr() const { return r_[14]; }

    u32 cpsr() const { return cpsr_; }
    void set_cpsr(u32 value);
    u32 spsr() const;
    void set_spsr(u32 value);

    Mode mode() const { return static_cast<Mode>(cpsr_ & 0x1Fu); }
    bool in_thumb() const { return (cpsr_ & FLAG_T) != 0; }
    bool irq_disabled() const { return (cpsr_ & FLAG_I) != 0; }
    bool halted() const { return halted_; }
    void set_halted(bool h) { halted_ = h; }

    void begin_intr_wait(u16 irq_mask, u32 resume_pc);
    bool intr_wait_active() const { return intr_wait_mask_ != 0; }

    // Banked register helpers (including User/Sys access for S-bit LDM/STM)
    u32 read_user_reg(u32 idx) const;
    void write_user_reg(u32 idx, u32 val);

    // Optional native C++ function overrides at specific GBA ROM/RAM addresses
    void clear_pc_hooks() { pc_hooks_.clear(); }
    void set_pc_hook(u32 addr, PcHookFn fn) { pc_hooks_[addr & ~1u] = std::move(fn); }

    // Flag bit positions in CPSR/SPSR
    static constexpr u32 FLAG_N = 1u << 31;
    static constexpr u32 FLAG_Z = 1u << 30;
    static constexpr u32 FLAG_C = 1u << 29;
    static constexpr u32 FLAG_V = 1u << 28;
    static constexpr u32 FLAG_I = 1u << 7;
    static constexpr u32 FLAG_F = 1u << 6;
    static constexpr u32 FLAG_T = 1u << 5;

    bool flag_n() const { return (cpsr_ & FLAG_N) != 0; }
    bool flag_z() const { return (cpsr_ & FLAG_Z) != 0; }
    bool flag_c() const { return (cpsr_ & FLAG_C) != 0; }
    bool flag_v() const { return (cpsr_ & FLAG_V) != 0; }

    void set_nz(u32 result);
    void set_nzcv(u32 result, bool carry, bool overflow);
    bool eval_cond(u32 cond) const;

    u64 instruction_count() const { return insn_count_; }

private:
    static int mode_to_bank(Mode m);
    void switch_bank(Mode old_mode, Mode new_mode);
    void jump_to(u32 addr, bool thumb);

    u32 exec_arm(hal::GbaBus& bus, u32 opcode, u32 instr_pc);
    u32 exec_thumb(hal::GbaBus& bus, u16 opcode, u32 instr_pc);

    std::array<u32, 16> r_{};
    u32 cpsr_ = static_cast<u32>(Mode::System);

    // Banked copies per privilege bank: [0]=USR/SYS, [1]=FIQ, [2]=IRQ, [3]=SVC, [4]=ABT, [5]=UND
    std::array<u32, 5> usr_r8_12_{};
    std::array<u32, 5> fiq_r8_12_{};
    std::array<u32, 6> bank_sp_{};
    std::array<u32, 6> bank_lr_{};
    std::array<u32, 6> bank_spsr_{};

    bool halted_ = false;
    u16 intr_wait_mask_ = 0;
    u32 intr_wait_resume_pc_ = 0;
    u64 insn_count_ = 0;
    std::unordered_map<u32, PcHookFn> pc_hooks_{};
};

} // namespace hm::cpu
