#pragma once

#include "gba_types.hpp"

#include <chrono>
#include <functional>

namespace hm::hal {

class GbaBus;

struct FrameCallbacks {
    std::function<void(u16 scanline)> on_scanline = {};
    std::function<void()> on_vblank = {};
    // Optional CPU step callback invoked for each scanline's active-draw (960 cy)
    // and HBlank (272 cy) slice when running ROM instructions natively.
    std::function<void(u32 cycles)> on_cycles = {};
};

class HalTiming {
public:
    explicit HalTiming(double target_hz = GBA_REFRESH_HZ);

    void reset();
    double fixed_dt() const { return fixed_dt_; }

    // Advances fixed-timestep accumulator; returns number of logic ticks ready to run (clamped)
    u32 consume_ticks();

    // Runs one full GBA frame (228 scanlines: 0..159 visible, 160..227 VBlank)
    static void run_gba_frame(GbaBus& bus, const FrameCallbacks& callbacks);

    // Optional CPU throttle for non-vsync / headless pacing
    void sleep_until_next_tick() const;

private:
    using Clock = std::chrono::steady_clock;

    double target_hz_;
    double fixed_dt_;
    double accumulator_ = 0.0;
    Clock::time_point last_time_;
};

} // namespace hm::hal
