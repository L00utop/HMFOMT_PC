#include "hal/gba_bus.hpp"
#include "hal/hal_audio.hpp"
#include "hal/hal_input.hpp"
#include "hal/hal_save.hpp"
#include "hal/hal_timing.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <vector>

using namespace hm::hal;

int main() {
    GbaBus bus;
    HalAudio audio;
    assert(audio.init(44100, true));
    bus.attach_audio(&audio);

    // 1. EWRAM & IWRAM mirroring
    bus.write32(ADDR_EWRAM_BASE + 0x100u, 0xDEADBEEFu);
    assert(bus.read32(ADDR_EWRAM_BASE + EWRAM_SIZE + 0x100u) == 0xDEADBEEFu);

    bus.write16(ADDR_IWRAM_BASE + 0x40u, 0xCAFEu);
    assert(bus.read16(ADDR_IWRAM_BASE + IWRAM_SIZE + 0x40u) == 0xCAFEu);

    // 2. GBA 96KB VRAM mirroring inside 128KB window
    bus.write16(ADDR_VRAM_BASE + 0x10020u, 0x1234u);
    assert(bus.read16(ADDR_VRAM_BASE + 0x18020u) == 0x1234u);

    // 3. Immediate DMA3 32-bit copy from EWRAM to VRAM + IRQ flag ack
    for (u32 i = 0; i < 4; ++i) {
        bus.write32(ADDR_EWRAM_BASE + i * 4u, 0xA0000000u + i);
    }
    bus.io_write32(reg::DMA3SAD, ADDR_EWRAM_BASE);
    bus.io_write32(reg::DMA3DAD, ADDR_VRAM_BASE + 0x200u);
    bus.io_write16(reg::DMA3CNT_L, 4u);
    bus.io_write16(reg::DMA3CNT_H, 0xC400u); // Enable + IRQ + 32-bit + Immediate
    assert(bus.read32(ADDR_VRAM_BASE + 0x200u) == 0xA0000000u);
    assert(bus.read32(ADDR_VRAM_BASE + 0x20Cu) == 0xA0000003u);
    assert((bus.io_read16(reg::IF) & IRQ_DMA3) != 0);
    bus.io_write16(reg::IF, IRQ_DMA3); // Acknowledge by writing 1
    assert((bus.io_read16(reg::IF) & IRQ_DMA3) == 0);

    // 4. BIOS LZ77 Uncomp (header 0x10, 4 bytes uncompressed: 'F','O','M','T')
    const u8 lz_stream[] = {
        0x10, 0x04, 0x00, 0x00, // Type 0x10, size 4
        0x00,                   // Flags: 8 literal bytes
        'F', 'O', 'M', 'T'
    };
    for (u32 i = 0; i < sizeof(lz_stream); ++i) {
        bus.write8(ADDR_EWRAM_BASE + 0x2000u + i, lz_stream[i]);
    }
    bus.svc_lz77_uncomp(ADDR_EWRAM_BASE + 0x2000u, ADDR_EWRAM_BASE + 0x3000u);
    assert(bus.read8(ADDR_EWRAM_BASE + 0x3000u) == 'F');
    assert(bus.read8(ADDR_EWRAM_BASE + 0x3003u) == 'T');

    // 5. Input active-low KEYINPUT & KEYCNT IRQ
    HalInput input;
    input.init(true);
    bus.io_write16(reg::KEYCNT, 0x4000u | KEY_A); // Enable IRQ on A
    input.set_pressed(KEY_A | KEY_START, true);
    input.poll_events(&bus);
    const u16 keyinput = bus.io_read16(reg::KEYINPUT);
    assert((keyinput & KEY_A) == 0);
    assert((keyinput & KEY_START) == 0);
    assert((keyinput & KEY_B) != 0);
    assert((bus.io_read16(reg::IF) & IRQ_KEYPAD) != 0);

    // 6. Macronix 128KB Flash state machine + HalSave atomic persistence
    auto flash_cmd = [&](u16 addr, u8 val) { bus.write8(ADDR_SRAM_BASE + addr, val); };
    flash_cmd(0x5555, 0xAA); flash_cmd(0x2AAA, 0x55); flash_cmd(0x5555, 0x90); // Enter ID mode
    assert(bus.read8(ADDR_SRAM_BASE + 0x0000) == 0xC2u);
    assert(bus.read8(ADDR_SRAM_BASE + 0x0001) == 0x09u);
    flash_cmd(0x5555, 0xAA); flash_cmd(0x2AAA, 0x55); flash_cmd(0x5555, 0xF0); // Exit ID mode

    flash_cmd(0x5555, 0xAA); flash_cmd(0x2AAA, 0x55); flash_cmd(0x5555, 0xB0); // Bank 1
    bus.write8(ADDR_SRAM_BASE + 0x0000, 0x01);
    flash_cmd(0x5555, 0xAA); flash_cmd(0x2AAA, 0x55); flash_cmd(0x5555, 0xA0); // Program byte
    bus.write8(ADDR_SRAM_BASE + 0x0042, 0x5Au);
    assert(bus.read8(ADDR_SRAM_BASE + 0x0042) == 0x5Au);

    const std::filesystem::path tmp_sav = "build_test_save/test.sav";
    HalSave saver(tmp_sav);
    assert(saver.flush_from_bus(bus));
    GbaBus bus2;
    assert(saver.load_into_bus(bus2));
    assert(bus2.save_ram()[0x10042u] == 0x5Au);
    std::filesystem::remove_all("build_test_save");

    // 7. SPU DMG Ch1 Square wave produces non-zero audio energy
    bus.io_write16(reg::SOUNDCNT_X, 0x0080u);
    bus.io_write16(reg::SOUNDCNT_L, 0xFF77u);
    bus.io_write16(reg::SOUNDCNT_H, 0x0002u);
    bus.io_write16(reg::SOUND1CNT_H, 0xF080u); // Vol 15, duty 50%
    bus.io_write16(reg::SOUND1CNT_X, 0x8600u); // Trigger freq
    std::vector<i16> pcm(1024 * 2, 0);
    audio.render_samples(pcm.data(), 1024);
    i64 energy = 0;
    for (i16 s : pcm) energy += std::abs(s);
    assert(energy > 0);

    // 8. HalTiming scanline VBlank / HBlank flags
    bool vblank_fired = false;
    HalTiming::run_gba_frame(bus, FrameCallbacks{
        nullptr,
        [&]() { vblank_fired = true; }
    });
    assert(vblank_fired);
    assert(bus.io_read16(reg::VCOUNT) == TOTAL_SCANLINES - 1u);

    // 9. Cycle-synced DirectSound FIFO + Timer0 + Linear Interpolation + DMA1 refill
    audio.reset();
    bus.io_write16(reg::SOUNDCNT_X, 0x0080u);
    bus.io_write16(reg::SOUNDCNT_L, 0x0000u);
    bus.io_write16(reg::SOUNDCNT_H, 0x0B0Eu); // Reset FIFO A + DS A 100% L+R on Timer 0
    for (u32 i = 0; i < 512u; ++i) {
        const u32 p = i & 63u;
        const i8 sample = static_cast<i8>((p < 32u) ? (static_cast<i32>(p) * 3 - 48) : (48 - static_cast<i32>(p - 32u) * 3));
        bus.write8(ADDR_EWRAM_BASE + 0x1000u + i, static_cast<u8>(sample));
    }
    bus.io_write32(reg::DMA1SAD, ADDR_EWRAM_BASE + 0x1000u);
    bus.io_write32(reg::DMA1DAD, ADDR_IO_BASE + reg::FIFO_A);
    bus.io_write16(reg::DMA1CNT_L, 4u);
    bus.io_write16(reg::DMA1CNT_H, 0xB600u); // Enable + Special + 32-bit + Repeat
    bus.io_write16(reg::TM0CNT_L, 0xFB1Au);  // 1254 cycles (~13.38 kHz)
    bus.io_write16(reg::TM0CNT_H, 0x0080u);

    HalTiming::run_gba_frame(bus, FrameCallbacks{});
    HalTiming::run_gba_frame(bus, FrameCallbacks{});
    assert(audio.buffered_frames() >= 1400u);
    std::vector<i16> ds_pcm(1400u * 2u, 0);
    audio.render_samples(ds_pcm.data(), 1400u);
    int ds_diffs = 0;
    i32 ds_peak = 0;
    i32 ds_max_jump = 0;
    for (std::size_t i = 0; i < 1400u; ++i) {
        const i32 s = ds_pcm[i * 2u];
        ds_peak = std::max<i32>(ds_peak, std::abs(s));
        if (i > 0) {
            const i32 prev = ds_pcm[(i - 1u) * 2u];
            if (s != prev) ++ds_diffs;
            ds_max_jump = std::max<i32>(ds_max_jump, std::abs(s - prev));
        }
    }
    assert(ds_peak > 1500);
    assert(ds_diffs > 700);
    assert(ds_max_jump < 2000);

    std::puts("test_hal_bus: ALL PASSED");
    return 0;
}
