#pragma once

#include "gba_types.hpp"

#include <array>
#include <cstddef>
#include <vector>

namespace hm::hal {

class HalAudio;

enum class DmaTiming : u8 {
    Immediate = 0,
    VBlank    = 1,
    HBlank    = 2,
    Special   = 3, // Sound FIFO on DMA1/2, Display Capture on DMA3
};

enum class SaveChip : u8 {
    Flash128K = 0, // Default for unit tests & Flash carts (MX29L010)
    Sram64K   = 1, // Auto-selected for SRAM_Vxxx carts (e.g., FoMT US SRAM_V113)
};

class GbaBus {
public:
    GbaBus();

    void reset();
    void attach_audio(HalAudio* audio);
    HalAudio* audio() const { return audio_; }
    void set_save_chip(SaveChip chip) { save_chip_ = chip; }
    SaveChip save_chip() const { return save_chip_; }

    bool halt_requested() const { return halt_requested_; }
    void request_halt() { halt_requested_ = true; }
    void clear_halt_request() { halt_requested_ = false; }

    // Load cartridge ROM image (e.g., baserom.gba)
    bool load_rom(const u8* data, std::size_t size);
    bool load_rom_file(const char* path);
    bool has_rom() const { return !rom_.empty(); }
    std::size_t rom_size() const { return rom_.size(); }
    bool coop_rom_patches_applied() const { return coop_rom_patches_applied_; }

    // 8/16/32-bit Bus Memory Operations (GBA physical addresses)
    u8  read8(u32 addr) const;
    u16 read16(u32 addr) const;
    u32 read32(u32 addr) const;

    void write8(u32 addr, u8 val);
    void write16(u32 addr, u16 val);
    void write32(u32 addr, u32 val);

    // Direct MMIO helpers (offset relative to 0x04000000)
    u16 io_read16(u32 io_offset) const;
    u32 io_read32(u32 io_offset) const;
    void io_write16(u32 io_offset, u16 val);
    void io_write32(u32 io_offset, u32 val);

    // Internal hardware updates (from Timing / Input HAL)
    void set_vcount(u16 vcount);
    void set_dispstat_flags(u16 mask, bool set);
    void set_keyinput(u16 active_low_mask);
    void raise_irq(u16 irq_bit);

    void trigger_dma_by_timing(DmaTiming timing);
    void trigger_fifo_dma(int fifo_index);
    void step_timers(u32 cpu_cycles);

    // BIOS SVC equivalents
    void svc_register_ram_reset(u32 flags);
    void svc_cpu_set(u32 src_addr, u32 dst_addr, u32 control);
    void svc_cpu_fast_set(u32 src_addr, u32 dst_addr, u32 control);
    void svc_lz77_uncomp(u32 src_addr, u32 dst_addr, bool vram_16bit = false);
    void svc_rl_uncomp(u32 src_addr, u32 dst_addr, bool vram_16bit = false);
    void svc_bg_affine_set(u32 src_addr, u32 dst_addr, i32 count);
    void svc_obj_affine_set(u32 src_addr, u32 dst_addr, i32 count, i32 offset);
    void svc_bit_unpack(u32 src_addr, u32 dst_addr, u32 info_addr);
    void svc_diff_unfilter(u32 src_addr, u32 dst_addr, bool bit16, bool vram_16bit = false);

    // Direct memory spans for PPU / Save HAL / Native modules
    u8* ewram() { return ewram_.data(); }
    const u8* ewram() const { return ewram_.data(); }
    u8* iwram() { return iwram_.data(); }
    const u8* iwram() const { return iwram_.data(); }
    u8* io() { return io_.data(); }
    const u8* io() const { return io_.data(); }
    u8* pal_ram() { return pal_.data(); }
    const u8* pal_ram() const { return pal_.data(); }
    u8* vram() { return vram_.data(); }
    const u8* vram() const { return vram_.data(); }
    u8* oam() { return oam_.data(); }
    const u8* oam() const { return oam_.data(); }
    u8* save_ram() { return save_.data(); }
    const u8* save_ram() const { return save_.data(); }
    const std::vector<u8>& rom() const { return rom_; }

    bool save_dirty() const { return save_dirty_; }
    void clear_save_dirty() { save_dirty_ = false; }

private:
    struct DmaState {
        u32 sad_latch = 0;
        u32 dad_latch = 0;
        u32 count_latch = 0;
    };

    struct TimerState {
        u16 reload = 0;
        u16 counter = 0;
        u32 cycle_accum = 0;
        bool running = false;
    };

    enum class FlashState : u8 {
        Ready,
        Cmd5555,
        Cmd2AAA,
        EraseCmd,
        Erase5555,
        Erase2AAA,
        WriteByte,
        BankSelect,
    };

    static u32 map_vram_offset(u32 addr);
    void install_bios_trampoline();
    void detect_save_chip_from_rom();
    void apply_fomt_coop_rom_patches();
    void execute_dma(int ch);
    void on_io_write8(u32 io_offset, u8 val);
    void on_io_write16(u32 io_offset, u16 val);
    void on_timer_overflow_internal(int ch);

    u8  read_flash_or_sram(u32 offset) const;
    void write_flash_or_sram(u32 offset, u8 val);

    std::array<u8, BIOS_SIZE>  bios_{};
    std::array<u8, EWRAM_SIZE> ewram_{};
    std::array<u8, IWRAM_SIZE> iwram_{};
    std::array<u8, IO_SIZE>    io_{};
    std::array<u8, PAL_SIZE>   pal_{};
    std::array<u8, VRAM_SIZE>  vram_{};
    std::array<u8, OAM_SIZE>   oam_{};
    std::array<u8, SAVE_SIZE>  save_{};
    std::vector<u8>            rom_{};

    std::array<DmaState, 4>   dmas_{};
    std::array<TimerState, 4> timers_{};
    HalAudio* audio_ = nullptr;
    SaveChip save_chip_ = SaveChip::Flash128K;
    bool halt_requested_ = false;

    // 128KB Macronix Flash state (0xC2, 0x09)
    FlashState flash_state_ = FlashState::Ready;
    bool flash_id_mode_ = false;
    u8 flash_bank_ = 0;
    bool save_dirty_ = false;
    bool coop_rom_patches_applied_ = false;
};

} // namespace hm::hal
