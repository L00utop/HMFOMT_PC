#pragma once

#include "cpu/arm7tdmi.hpp"
#include "ewram_coop_bridge.hpp"
#include "fomt_domain.hpp"
#include "fomt_rom_assets.hpp"
#include "gba_scene_assets.hpp"
#include "hal/gba_bus.hpp"
#include "hal/hal_audio.hpp"
#include "hal/hal_input.hpp"
#include "hal/hal_save.hpp"
#include "hal/hal_timing.hpp"
#include "hal/hal_video.hpp"
#include "net/net_coop.hpp"
#include "ppu/ppu.hpp"
#include "script_vm.hpp"

#include <string>
#include <vector>

namespace hm::game {

enum class ScreenMode : hal::u8 {
    Title    = 0,
    Gameplay = 1,
};

struct EngineConfig {
    int scale = 3;
    bool headless = false;
    bool use_cpu_scaffold = true;
    std::string rom_path = "baserom.gba";
    std::string save_path = "saves/fomt.sav";
    bool start_host = false;
    hal::u16 host_port = net::NetCoopSession::DEFAULT_PORT;
    std::string connect_addr;
};

class GameEngine {
public:
    GameEngine() = default;
    ~GameEngine();

    bool init(const EngineConfig& cfg);
    void shutdown();

    // Runs one fixed-timestep tick (input -> net -> game -> GBA HW sync -> PPU render)
    bool tick_frame(bool poll_host_events = true);

    // Interactive main loop until user closes window or presses Escape
    int run();

    // Direct accessors for automated tests and diagnostics
    hal::GbaBus& bus() { return bus_; }
    ppu::Ppu& ppu() { return ppu_; }
    hal::HalVideo& video() { return video_; }
    hal::HalInput& input() { return input_; }
    hal::HalAudio& audio() { return audio_; }
    FarmWorld& world() { return world_; }
    ScriptVm& script_vm() { return script_vm_; }
    net::NetCoopSession& net() { return net_; }
    ScreenMode screen_mode() const { return mode_; }
    const RomProbeInfo& rom_info() const { return rom_info_; }
    bool has_rom_assets() const { return rom_assets_.loaded(); }
    bool is_native_rom_mode() const { return bus_.has_rom() && cfg_.use_cpu_scaffold; }
    cpu::Arm7Cpu& cpu() { return cpu_; }
    const cpu::Arm7Cpu& cpu() const { return cpu_; }
    EwramCoopBridge& coop_bridge() { return coop_bridge_; }

    bool save_game();
    bool load_game();

private:
    bool tick_native_rom_frame(const hal::HostActions& act);
    void step_title_screen();
    void step_gameplay();
    void sync_title_ppu();
    void sync_gameplay_ppu();
    void play_sfx(hal::u16 freq_reg, hal::u8 duty = 2, hal::u8 len = 48);
    void trigger_welcome_script();
    void target_tile_in_front(const FarmerState& f, hal::i32& out_tx, hal::i32& out_ty) const;

    EngineConfig cfg_{};
    bool initialized_ = false;
    hal::GbaBus bus_{};
    cpu::Arm7Cpu cpu_{};
    hal::HalAudio audio_{};
    hal::HalInput input_{};
    hal::HalSave save_{};
    hal::HalVideo video_{};
    hal::HalTiming timing_{};
    ppu::Ppu ppu_{};

    FarmWorld world_{};
    ScriptVm script_vm_{};
    net::NetCoopSession net_{};
    EwramCoopBridge coop_bridge_{};
    RomProbeInfo rom_info_{};
    FomtRomAssets rom_assets_{};
    std::vector<hal::u16> composed_t1_;
    std::vector<hal::u16> composed_t2_;
    std::vector<hal::u16> composed_t3_;

    ScreenMode mode_ = ScreenMode::Title;
    bool uploaded_mode_initialized_ = false;
    ScreenMode uploaded_mode_ = ScreenMode::Title;
    Season uploaded_season_ = Season::Spring;
    bool title_continue_selected_ = false;
    hal::u32 frame_count_ = 0;
    std::size_t tool_cursor_ = 0;
    std::string dialog_line1_;
    std::string dialog_line2_;
    bool dialog_open_ = false;
    std::string toast_msg_;
    hal::u32 toast_timer_ = 0;
};

} // namespace hm::game
