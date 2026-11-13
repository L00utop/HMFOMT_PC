#include "cpu/arm7tdmi.hpp"
#include "hal/gba_bus.hpp"
#include "hal/hal_timing.hpp"

#include <cassert>
#include <vector>

using namespace hm::cpu;
using namespace hm::hal;

namespace {

void write_arm_program(GbaBus& bus, u32 base, const std::vector<u32>& words) {
    for (std::size_t i = 0; i < words.size(); ++i) {
        bus.write32(base + static_cast<u32>(i) * 4u, words[i]);
    }
}

void write_thumb_program(GbaBus& bus, u32 base, const std::vector<u16>& halfs) {
    for (std::size_t i = 0; i < halfs.size(); ++i) {
        bus.write16(base + static_cast<u32>(i) * 2u, halfs[i]);
    }
}

} // namespace

int main() {
    GbaBus bus;
    Arm7Cpu cpu;

    // 1. ARM ALU flags, barrel shifter, 64-bit UMULL, unaligned LDR, SWP, MSR/MRS, LDM/STM
    {
        bus.reset();
        cpu.reset(ADDR_EWRAM_BASE);

        // Place little-endian test word 0x11223344 at EWRAM + 0x400
        bus.write32(ADDR_EWRAM_BASE + 0x400u, 0x11223344u);

        const std::vector<u32> arm_prog = {
            0xE3A00005u, // mov   r0, #5
            0xE3B01003u, // movs  r1, #3          (C unaffected=0)
            0xE250200Au, // subs  r2, r0, #10     (r2 = -5 = 0xFFFFFFFB, N=1, Z=0, C=0, V=0)
            0xE2A03000u, // adc   r3, r0, #0      (r3 = 5 + 0 + 0 = 5)
            0xE2D04002u, // sbcs  r4, r0, #2      (r4 = 5 - 2 - !0 = 2, C=1)
            0xE3A05480u, // mov   r5, #0x80000000
            0xE3A06003u, // mov   r6, #3
            0xE0878596u, // umull r8, r7, r6, r5  (3 * 0x80000000 = 0x00000001_80000000 -> r7=1, r8=0x80000000)
            0xE3A09402u, // mov   r9, #0x02000000 (2 ROR 8)
            0xE2899C04u, // add   r9, r9, #0x400
            0xE289A002u, // add   r10, r9, #2     (r10 = 0x02000402)
            0xE59AA000u, // ldr   r10, [r10]      (unaligned ROTATE -> 0x33441122)
            0xE3A0B0AAu, // mov   r11, #0xAA
            0xE109B09Bu, // swp   r11, r11, [r9]  (r11 = 0x11223344, [r9] = 0xAA)
            0xE92D000Fu, // stmdb sp!, {r0-r3}
            0xE8BD000Fu, // ldmia sp!, {r0-r3}
            0xEAFFFFFEu, // b     .
        };
        write_arm_program(bus, ADDR_EWRAM_BASE, arm_prog);

        for (std::size_t i = 0; i + 1 < arm_prog.size(); ++i) {
            cpu.step_one(bus);
        }

        assert(cpu.reg(0) == 5u);
        assert(cpu.reg(2) == 0xFFFFFFFBu);
        assert(cpu.reg(3) == 5u);
        assert(cpu.reg(4) == 2u);
        assert(cpu.flag_c());
        assert(cpu.reg(7) == 1u);
        assert(cpu.reg(8) == 0x80000000u);
        assert(cpu.reg(10) == 0x33441122u);
        assert(cpu.reg(11) == 0x11223344u);
        assert(bus.read32(ADDR_EWRAM_BASE + 0x400u) == 0xAAu);
        assert(cpu.sp() == 0x03007F00u);
    }

    // 2. ARM -> Thumb interwork via BX, Thumb PUSH/POP, BL, PC-rel LDR, SWI Div & Sqrt
    {
        bus.reset();
        cpu.reset(ADDR_EWRAM_BASE);

        const std::vector<u32> arm_stub = {
            0xE3A0E0AAu, // mov lr, #0xAA
            0xE28F0005u, // add r0, pc, #5 -> 0x02000011 (Thumb at 0x02000010)
            0xE12FFF10u, // bx  r0
        };
        write_arm_program(bus, ADDR_EWRAM_BASE, arm_stub);

        // Literal word at 0x02000034 (4-byte aligned)
        bus.write32(ADDR_EWRAM_BASE + 0x34u, 144u);

        // Thumb code starting at 0x02000010
        const std::vector<u16> thumb_code = {
            0x4808u, // 0x10: ldr  r0, [pc, #0x20] -> ((0x10+4)&~3)+0x20 = 0x02000034 -> r0 = 144
            0xDF08u, // 0x12: swi  #8 (Sqrt)        -> r0 = 12
            0x2105u, // 0x14: movs r1, #5
            0xDF06u, // 0x16: swi  #6 (Div 12/5)    -> r0 = 2, r1 = 2
            0xB503u, // 0x18: push {r0, r1, lr}     (pushes 2, 2, 0xAA)
            0x2000u, // 0x1A: movs r0, #0
            0x2100u, // 0x1C: movs r1, #0
            0xF000u, // 0x1E: bl   +8 -> 0x2A
            0xF804u, // 0x20
            0xBC0Cu, // 0x22: pop  {r2, r3}         -> r2=2, r3=2
            0xBC02u, // 0x24: pop  {r1}             -> r1=0xAA
            0xE7FEu, // 0x26: b    .
            0x0000u, // 0x28: nop
            0x202Au, // 0x2A: movs r0, #42
            0x4770u, // 0x2C: bx   lr
        };
        write_thumb_program(bus, ADDR_EWRAM_BASE + 0x10u, thumb_code);

        for (int s = 0; s < 17; ++s) {
            cpu.step_one(bus);
        }

        assert(cpu.in_thumb());
        assert(cpu.reg(0) == 42u);
        assert(cpu.reg(1) == 0xAAu);
        assert(cpu.reg(2) == 2u);
        assert(cpu.reg(3) == 2u);
    }

    // 3. Hardware IRQ Trampoline (0x00000018 -> 0x00000128 -> [0x03007FFC]) + IntrWait (SWI 0x04)
    {
        bus.reset();
        cpu.reset(ADDR_EWRAM_BASE);

        // User ARM ISR at IWRAM 0x03000100:
        //   r0 = 0x04000000 passed by BIOS wrapper
        //   Acknowledge VBlank IF = 1 at [r0, #0x202]
        //   Set INTR_CHECK bit 0 at [0x03007FF8]
        //   Increment counter at [0x03000200]
        //   Return via bx lr to BIOS 0x00000138 (ldmia + subs pc, lr, #4)
        const std::vector<u32> user_isr = {
            0xE3A01001u, // mov  r1, #1
            0xE2802C02u, // add  r2, r0, #0x200
            0xE1C210B2u, // strh r1, [r2, #2]       -> IF ack (0x04000202)
            0xE3A02403u, // mov  r2, #0x03000000
            0xE2822A07u, // add  r2, r2, #0x7000
            0xE2822F3Eu, // add  r2, r2, #0xF8      -> 0x030070F8... oops use exact 0xFF8:
            0xE12FFF1Eu, // bx   lr
        };
        // Encode exact 0x03007FF8 load cleanly with literal pool at end of ISR:
        const std::vector<u32> isr_clean = {
            0xE3A01001u, // 00: mov  r1, #1
            0xE2802C02u, // 04: add  r2, r0, #0x200
            0xE1C210B2u, // 08: strh r1, [r2, #2]    (ack IF)
            0xE59F2014u, // 0C: ldr  r2, [pc, #0x14] (=0x03007FF8)
            0xE1D230B0u, // 10: ldrh r3, [r2]
            0xE1833001u, // 14: orr  r3, r3, r1
            0xE1C230B0u, // 18: strh r3, [r2]
            0xE59F2008u, // 1C: ldr  r2, [pc, #0x08] (=0x03000200)
            0xE5821000u, // 20: str  r1, [r2]        (mark ran!)
            0xE12FFF1Eu, // 24: bx   lr
            0x03007FF8u, // 28: literal INTR_CHECK
            0x03000200u, // 2C: literal marker
        };
        (void)user_isr;
        write_arm_program(bus, 0x03000100u, isr_clean);
        bus.write32(0x03007FFCu, 0x03000100u);

        // Main program at EWRAM:
        //   Enable DISPSTAT VBlank IRQ (bit 3) and IE = IRQ_VBLANK (1)
        //   Call SWI 0x05 (VBlankIntrWait) -> halts until scanline 160 VBlank IRQ!
        //   After waking, write 0xCAFEBABE to r7 and loop
        const std::vector<u32> main_prog = {
            0xE3A00301u, // mov  r0, #0x04000000
            0xE3A01008u, // mov  r1, #0x0008        (DISPSTAT VBlank IRQ enable)
            0xE1C010B4u, // strh r1, [r0, #4]
            0xE2802C02u, // add  r2, r0, #0x200
            0xE3A01001u, // mov  r1, #1             (IE = VBlank)
            0xE1C210B0u, // strh r1, [r2]
            0xEF050000u, // swi  0x05 << 16         (VBlankIntrWait)
            0xE3A07077u, // mov  r7, #0x77          (executed AFTER VBlank wake!)
            0xEAFFFFFEu, // b    .
        };
        write_arm_program(bus, ADDR_EWRAM_BASE, main_prog);

        FrameCallbacks cb{};
        cb.on_cycles = [&](u32 cy) { cpu.step(bus, cy); };
        HalTiming::run_gba_frame(bus, cb);

        assert(!cpu.halted());
        assert(!cpu.intr_wait_active());
        assert(bus.read32(0x03000200u) == 1u);
        assert(cpu.reg(7) == 0x77u);
        assert((bus.read16(0x03007FF8u) & IRQ_VBLANK) == 0u); // Consumed by VBlankIntrWait
    }

    return 0;
}
