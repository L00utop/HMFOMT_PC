#pragma once

#include <cstddef>
#include <cstdint>

namespace hm::hal {

using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8  = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

inline constexpr u32 SCREEN_WIDTH          = 240;
inline constexpr u32 SCREEN_HEIGHT         = 160;
inline constexpr u32 VBLANK_SCANLINES      = 68;
inline constexpr u32 TOTAL_SCANLINES       = SCREEN_HEIGHT + VBLANK_SCANLINES; // 228
inline constexpr double GBA_REFRESH_HZ     = 59.7275;
inline constexpr u32 CPU_CYCLES_PER_LINE   = 1232;

// GBA Memory Map Constants
inline constexpr u32 ADDR_BIOS_BASE    = 0x00000000u;
inline constexpr u32 ADDR_EWRAM_BASE   = 0x02000000u;
inline constexpr u32 ADDR_IWRAM_BASE   = 0x03000000u;
inline constexpr u32 ADDR_IO_BASE      = 0x04000000u;
inline constexpr u32 ADDR_PAL_BASE     = 0x05000000u;
inline constexpr u32 ADDR_VRAM_BASE    = 0x06000000u;
inline constexpr u32 ADDR_OAM_BASE     = 0x07000000u;
inline constexpr u32 ADDR_ROM_BASE     = 0x08000000u;
inline constexpr u32 ADDR_ROM_END      = 0x0E000000u;
inline constexpr u32 ADDR_SRAM_BASE    = 0x0E000000u;

inline constexpr std::size_t BIOS_SIZE   = 16u * 1024u;
inline constexpr std::size_t EWRAM_SIZE  = 256u * 1024u;
inline constexpr std::size_t IWRAM_SIZE  = 32u * 1024u;
inline constexpr std::size_t IO_SIZE     = 1024u;
inline constexpr std::size_t PAL_SIZE    = 1024u;        // 512B BG + 512B OBJ
inline constexpr std::size_t VRAM_SIZE   = 96u * 1024u;
inline constexpr std::size_t OAM_SIZE    = 1024u;        // 128 sprites * 8 bytes
inline constexpr std::size_t SAVE_SIZE   = 128u * 1024u; // 128KB Flash / SRAM
inline constexpr std::size_t MAX_ROM_SIZE = 32u * 1024u * 1024u;

// MMIO Register Offsets (relative to 0x04000000)
namespace reg {
inline constexpr u32 DISPCNT    = 0x000u;
inline constexpr u32 GREENSWAP  = 0x002u;
inline constexpr u32 DISPSTAT   = 0x004u;
inline constexpr u32 VCOUNT     = 0x006u;
inline constexpr u32 BG0CNT     = 0x008u;
inline constexpr u32 BG1CNT     = 0x00Au;
inline constexpr u32 BG2CNT     = 0x00Cu;
inline constexpr u32 BG3CNT     = 0x00Eu;
inline constexpr u32 BG0HOFS    = 0x010u;
inline constexpr u32 BG0VOFS    = 0x012u;
inline constexpr u32 BG1HOFS    = 0x014u;
inline constexpr u32 BG1VOFS    = 0x016u;
inline constexpr u32 BG2HOFS    = 0x018u;
inline constexpr u32 BG2VOFS    = 0x01Au;
inline constexpr u32 BG3HOFS    = 0x01Cu;
inline constexpr u32 BG3VOFS    = 0x01Eu;
inline constexpr u32 BG2PA      = 0x020u;
inline constexpr u32 BG2PB      = 0x022u;
inline constexpr u32 BG2PC      = 0x024u;
inline constexpr u32 BG2PD      = 0x026u;
inline constexpr u32 BG2X       = 0x028u;
inline constexpr u32 BG2Y       = 0x02Cu;
inline constexpr u32 BG3PA      = 0x030u;
inline constexpr u32 BG3PB      = 0x032u;
inline constexpr u32 BG3PC      = 0x034u;
inline constexpr u32 BG3PD      = 0x036u;
inline constexpr u32 BG3X       = 0x038u;
inline constexpr u32 BG3Y       = 0x03Cu;
inline constexpr u32 WIN0H      = 0x040u;
inline constexpr u32 WIN1H      = 0x042u;
inline constexpr u32 WIN0V      = 0x044u;
inline constexpr u32 WIN1V      = 0x046u;
inline constexpr u32 WININ      = 0x048u;
inline constexpr u32 WINOUT     = 0x04Au;
inline constexpr u32 MOSAIC     = 0x04Cu;
inline constexpr u32 BLDCNT     = 0x050u;
inline constexpr u32 BLDALPHA   = 0x052u;
inline constexpr u32 BLDY       = 0x054u;

inline constexpr u32 SOUND1CNT_L = 0x060u;
inline constexpr u32 SOUND1CNT_H = 0x062u;
inline constexpr u32 SOUND1CNT_X = 0x064u;
inline constexpr u32 SOUND2CNT_L = 0x068u;
inline constexpr u32 SOUND2CNT_H = 0x06Cu;
inline constexpr u32 SOUND3CNT_L = 0x070u;
inline constexpr u32 SOUND3CNT_H = 0x072u;
inline constexpr u32 SOUND3CNT_X = 0x074u;
inline constexpr u32 SOUND4CNT_L = 0x078u;
inline constexpr u32 SOUND4CNT_H = 0x07Cu;
inline constexpr u32 SOUNDCNT_L  = 0x080u;
inline constexpr u32 SOUNDCNT_H  = 0x082u;
inline constexpr u32 SOUNDCNT_X  = 0x084u;
inline constexpr u32 SOUNDBIAS   = 0x088u;
inline constexpr u32 WAVE_RAM0   = 0x090u;
inline constexpr u32 WAVE_RAM1   = 0x094u;
inline constexpr u32 WAVE_RAM2   = 0x098u;
inline constexpr u32 WAVE_RAM3   = 0x09Cu;
inline constexpr u32 FIFO_A      = 0x0A0u;
inline constexpr u32 FIFO_B      = 0x0A4u;

inline constexpr u32 DMA0SAD    = 0x0B0u;
inline constexpr u32 DMA0DAD    = 0x0B4u;
inline constexpr u32 DMA0CNT_L  = 0x0B8u;
inline constexpr u32 DMA0CNT_H  = 0x0BAu;
inline constexpr u32 DMA1SAD    = 0x0BCu;
inline constexpr u32 DMA1DAD    = 0x0C0u;
inline constexpr u32 DMA1CNT_L  = 0x0C4u;
inline constexpr u32 DMA1CNT_H  = 0x0C6u;
inline constexpr u32 DMA2SAD    = 0x0C8u;
inline constexpr u32 DMA2DAD    = 0x0CCu;
inline constexpr u32 DMA2CNT_L  = 0x0D0u;
inline constexpr u32 DMA2CNT_H  = 0x0D2u;
inline constexpr u32 DMA3SAD    = 0x0D4u;
inline constexpr u32 DMA3DAD    = 0x0D8u;
inline constexpr u32 DMA3CNT_L  = 0x0DCu;
inline constexpr u32 DMA3CNT_H  = 0x0DEu;

inline constexpr u32 TM0CNT_L   = 0x100u;
inline constexpr u32 TM0CNT_H   = 0x102u;
inline constexpr u32 TM1CNT_L   = 0x104u;
inline constexpr u32 TM1CNT_H   = 0x106u;
inline constexpr u32 TM2CNT_L   = 0x108u;
inline constexpr u32 TM2CNT_H   = 0x10Au;
inline constexpr u32 TM3CNT_L   = 0x10Cu;
inline constexpr u32 TM3CNT_H   = 0x10Eu;

inline constexpr u32 KEYINPUT   = 0x130u;
inline constexpr u32 KEYCNT     = 0x132u;
inline constexpr u32 IE         = 0x200u;
inline constexpr u32 IF         = 0x202u;
inline constexpr u32 WAITCNT    = 0x204u;
inline constexpr u32 IME        = 0x208u;
inline constexpr u32 POSTFLG    = 0x300u;
inline constexpr u32 HALTCNT    = 0x301u;
} // namespace reg

// Keypad bit masks (active-low in KEYINPUT, bit=0 means pressed)
enum KeyMask : u16 {
    KEY_A      = 1u << 0,
    KEY_B      = 1u << 1,
    KEY_SELECT = 1u << 2,
    KEY_START  = 1u << 3,
    KEY_RIGHT  = 1u << 4,
    KEY_LEFT   = 1u << 5,
    KEY_UP     = 1u << 6,
    KEY_DOWN   = 1u << 7,
    KEY_R      = 1u << 8,
    KEY_L      = 1u << 9,
    KEY_ALL    = 0x03FFu,
};

// Hardware Interrupt Bits (IE / IF)
enum IrqBit : u16 {
    IRQ_VBLANK  = 1u << 0,
    IRQ_HBLANK  = 1u << 1,
    IRQ_VCOUNT  = 1u << 2,
    IRQ_TIMER0  = 1u << 3,
    IRQ_TIMER1  = 1u << 4,
    IRQ_TIMER2  = 1u << 5,
    IRQ_TIMER3  = 1u << 6,
    IRQ_SERIAL  = 1u << 7,
    IRQ_DMA0    = 1u << 8,
    IRQ_DMA1    = 1u << 9,
    IRQ_DMA2    = 1u << 10,
    IRQ_DMA3    = 1u << 11,
    IRQ_KEYPAD  = 1u << 12,
    IRQ_GAMEPAK = 1u << 13,
};

// BIOS RegisterRamReset flags
enum ResetFlags : u32 {
    RESET_EWRAM    = 1u << 0,
    RESET_IWRAM    = 1u << 1,
    RESET_PALETTE  = 1u << 2,
    RESET_VRAM     = 1u << 3,
    RESET_OAM      = 1u << 4,
    RESET_SIO_IO   = 1u << 5,
    RESET_SOUND_IO = 1u << 6,
    RESET_IO       = 1u << 7,
};

/// Convert GBA 15-bit BGR555 color to 32-bit RGBA8888 (A=255, B, G, R in little-endian ABGR/RGBA).
/// Output format matches SDL_PIXELFORMAT_ABGR8888 (byte order R, G, B, A on little-endian).
inline constexpr u32 bgr555_to_rgba8888(u16 c) {
    const u32 r5 = (c >> 0) & 0x1Fu;
    const u32 g5 = (c >> 5) & 0x1Fu;
    const u32 b5 = (c >> 10) & 0x1Fu;
    const u32 r8 = (r5 << 3) | (r5 >> 2);
    const u32 g8 = (g5 << 3) | (g5 >> 2);
    const u32 b8 = (b5 << 3) | (b5 >> 2);
    return 0xFF000000u | (b8 << 16) | (g8 << 8) | r8;
}

inline constexpr u16 rgb8_to_bgr555(u8 r, u8 g, u8 b) {
    return static_cast<u16>(((r >> 3) & 0x1Fu) | (((g >> 3) & 0x1Fu) << 5) | (((b >> 3) & 0x1Fu) << 10));
}

} // namespace hm::hal
