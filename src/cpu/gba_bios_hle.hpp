#pragma once

#include "hal/gba_types.hpp"

namespace hm::hal {
class GbaBus;
}

namespace hm::cpu {

class Arm7Cpu;

/// High-level emulation of GBA BIOS software interrupts (SWI 0x00..0x2A).
class GbaBiosHle {
public:
    static void dispatch(hal::u8 swi_num, Arm7Cpu& cpu, hal::GbaBus& bus);
};

} // namespace hm::cpu
