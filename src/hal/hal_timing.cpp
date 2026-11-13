#include "hal_timing.hpp"
#include "gba_bus.hpp"
#include "hal_audio.hpp"

#include <algorithm>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

namespace hm::hal {

HalTiming::HalTiming(double target_hz)
    : target_hz_(target_hz > 1.0 ? target_hz : GBA_REFRESH_HZ)
    , fixed_dt_(1.0 / target_hz_)
    , last_time_(Clock::now()) {
}

void HalTiming::reset() {
    accumulator_ = 0.0;
    last_time_ = Clock::now();
}

u32 HalTiming::consume_ticks() {
    const auto now = Clock::now();
    const double elapsed = std::chrono::duration<double>(now - last_time_).count();
    last_time_ = now;

    // Clamp elapsed frame delta to prevent spiral of death during pauses/drags
    accumulator_ += std::min(elapsed, fixed_dt_ * 5.0);

    u32 ticks = 0;
    while (accumulator_ >= fixed_dt_ && ticks < 4) {
        accumulator_ -= fixed_dt_;
        ++ticks;
    }
    return ticks;
}

void HalTiming::run_gba_frame(GbaBus& bus, const FrameCallbacks& callbacks) {
    constexpr u32 HDRAW_CYCLES = 960u;
    constexpr u32 HBLANK_CYCLES = CPU_CYCLES_PER_LINE - HDRAW_CYCLES; // 272u

    for (u16 y = 0; y < TOTAL_SCANLINES; ++y) {
        bus.set_vcount(y);

        const bool in_vblank = (y >= SCREEN_HEIGHT);
        bus.set_dispstat_flags(0x0001u, in_vblank); // Bit 0: VBlank flag
        bus.set_dispstat_flags(0x0002u, false);     // Clear HBlank during active draw

        if (y == SCREEN_HEIGHT) {
            const u16 dispstat = bus.io_read16(reg::DISPSTAT);
            if (dispstat & 0x0008u) bus.raise_irq(IRQ_VBLANK);
            bus.trigger_dma_by_timing(DmaTiming::VBlank);
            if (callbacks.on_vblank) callbacks.on_vblank();
        }

        if (callbacks.on_cycles) {
            callbacks.on_cycles(HDRAW_CYCLES);
        }

        if (y < SCREEN_HEIGHT && callbacks.on_scanline) {
            callbacks.on_scanline(y);
        }

        // Enter HBlank
        bus.set_dispstat_flags(0x0002u, true);
        const u16 dispstat = bus.io_read16(reg::DISPSTAT);
        if (dispstat & 0x0010u) bus.raise_irq(IRQ_HBLANK);
        if (y < SCREEN_HEIGHT) {
            bus.trigger_dma_by_timing(DmaTiming::HBlank);
        }

        if (callbacks.on_cycles) {
            callbacks.on_cycles(HBLANK_CYCLES);
        } else {
            bus.step_timers(CPU_CYCLES_PER_LINE);
        }
    }

    if (bus.audio()) {
        bus.audio()->flush_frame();
    }
}

void HalTiming::sleep_until_next_tick() const {
    const double target_remaining = fixed_dt_ - accumulator_;
    if (target_remaining <= 0.0002) {
        return;
    }

    const auto deadline = last_time_ + std::chrono::duration<double>(fixed_dt_ - accumulator_);
#ifdef _WIN32
    if (target_remaining > 0.0012) {
        // Sub-millisecond Windows high-resolution waitable timer avoids 15.6 ms sleep_for quantums
        struct HighResTimer {
            HANDLE h = nullptr;
            HighResTimer() {
                h = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
                if (!h) {
                    h = CreateWaitableTimerW(nullptr, TRUE, nullptr);
                }
            }
            ~HighResTimer() {
                if (h) CloseHandle(h);
            }
        };
        thread_local HighResTimer timer;
        if (timer.h) {
            const LONGLONG wait_100ns = -static_cast<LONGLONG>((target_remaining - 0.0008) * 10000000.0);
            if (wait_100ns < 0) {
                LARGE_INTEGER due{};
                due.QuadPart = wait_100ns;
                if (SetWaitableTimer(timer.h, &due, 0, nullptr, nullptr, FALSE)) {
                    WaitForSingleObject(timer.h, INFINITE);
                }
            }
        } else {
            std::this_thread::sleep_for(std::chrono::duration<double>(target_remaining - 0.001));
        }
    }
    while (Clock::now() < deadline) {
        std::this_thread::yield();
    }
#else
    if (target_remaining > 0.0015) {
        std::this_thread::sleep_for(std::chrono::duration<double>(target_remaining - 0.001));
    }
    while (Clock::now() < deadline) {
        std::this_thread::yield();
    }
#endif
}

} // namespace hm::hal
