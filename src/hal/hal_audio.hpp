#pragma once

#include "gba_types.hpp"

#include <array>
#include <cstddef>
#include <functional>
#include <mutex>

namespace hm::hal {

class HalAudio {
public:
    static constexpr u32 DEFAULT_SAMPLE_RATE = 48000;
    static constexpr std::size_t FIFO_CAPACITY = 64;
    static constexpr std::size_t RING_CAPACITY = 32768;

    using FifoRefillCallback = std::function<void(int fifo_index)>;

    HalAudio() = default;
    ~HalAudio();

    HalAudio(HalAudio const&) = delete;
    HalAudio& operator=(HalAudio const&) = delete;

    bool init(u32 sample_rate = DEFAULT_SAMPLE_RATE, bool headless = false);
    void shutdown();
    void reset();

    void set_fifo_refill_callback(FifoRefillCallback cb);

    // MMIO integration
    void write_register(u32 io_offset, u16 value, bool high_byte_only = false);
    void write_wave_ram(u32 offset_from_090, u32 value);
    void push_fifo_word(int fifo_index, u32 word);
    void clear_fifo(int fifo_index);

    // Advance SPU clock synchronously with emulated GBA CPU cycles (16.777216 MHz)
    void step_cycles(u32 cpu_cycles);
    void flush_frame();

    // Called when Timer 0 or Timer 1 overflows (drives DirectSound FIFO sample rate)
    void on_timer_overflow(int timer_id, u32 timer_sample_hz);

    // Deterministic offline / callback audio synthesis (interleaved stereo i16)
    void render_samples(i16* stereo_out, std::size_t frame_count);

    void set_muted(bool muted);
    bool is_muted() const { return muted_; }
    u32 sample_rate() const { return sample_rate_; }
    std::size_t fifo_count(int fifo_index) const;
    std::size_t buffered_frames() const;
    bool has_active_device() const { return !headless_ && device_id_ != 0 && !muted_; }

private:
    struct SquareChannel {
        bool enabled = false;
        bool has_sweep = false;
        u8 sweep_period = 0;
        bool sweep_negate = false;
        u8 sweep_shift = 0;
        u8 sweep_timer = 0;
        u16 sweep_shadow_freq = 0;

        u8 duty = 2;
        u8 duty_step = 0;
        u8 length_counter = 0;
        bool length_enable = false;

        u8 env_init_vol = 0;
        bool env_add = false;
        u8 env_period = 0;
        u8 env_timer = 0;
        u8 volume = 0;

        u16 raw_freq = 0;
        double phase = 0.0;
    };

    struct WaveChannel {
        bool dac_enabled = false;
        bool enabled = false;
        bool dimension_64 = false;
        u8 bank_select = 0;
        u8 vol_code = 0;
        u16 length_counter = 0;
        bool length_enable = false;
        u16 raw_freq = 0;
        double phase = 0.0;
        std::array<u8, 32> ram{}; // Two 16-byte banks
    };

    struct NoiseChannel {
        bool enabled = false;
        u8 length_counter = 0;
        bool length_enable = false;
        u8 env_init_vol = 0;
        bool env_add = false;
        u8 env_period = 0;
        u8 env_timer = 0;
        u8 volume = 0;
        u8 clock_shift = 0;
        bool width_7bit = false;
        u8 divisor_code = 0;
        u16 lfsr = 0x7FFFu;
        double phase = 0.0;
    };

    struct DirectFifo {
        std::array<i8, FIFO_CAPACITY> buffer{};
        std::size_t head = 0;
        std::size_t tail = 0;
        std::size_t count = 0;
        i8 prev_sample = 0;
        i8 current_sample = 0;
        u32 period_cycles = 1254;
        u32 cycles_since_pop = 0;
        u32 last_rate_hz = 13379;
    };

    static void sdl_audio_callback(void* userdata, u8* stream, int len);

    void reset_locked();
    void clear_fifo_locked(int fifo_index);
    void flush_cycles_locked();
    void synthesize_one_sample_locked(double dt, i16& out_l, i16& out_r);
    void push_ring_locked(i16 l, i16 r);
    void drain_ring_for_sdl(i16* stereo_out, std::size_t frame_count);

    void step_frame_sequencer();
    void step_square_envelope(SquareChannel& ch);
    void step_square_sweep(SquareChannel& ch);
    i32 sample_square(SquareChannel& ch, double dt);
    i32 sample_wave(double dt);
    i32 sample_noise(double dt);

    u32 sample_rate_ = DEFAULT_SAMPLE_RATE;
    u32 device_id_ = 0;
    bool initialized_ = false;
    bool headless_ = false;
    bool muted_ = false;

    mutable std::mutex mutex_;
    FifoRefillCallback fifo_refill_cb_;

    SquareChannel ch1_{};
    SquareChannel ch2_{};
    WaveChannel ch3_{};
    NoiseChannel ch4_{};
    std::array<DirectFifo, 2> fifos_{};

    u16 soundcnt_l_ = 0;
    u16 soundcnt_h_ = 0;
    u16 soundcnt_x_ = 0;
    u16 soundbias_  = 0x0200;

    u32 pending_cycles_ = 0;
    u64 sample_cycle_accum_ = 0;

    double seq_accum_ = 0.0;
    u8 seq_step_ = 0;

    // Anti-aliasing one-pole low-pass + DC blocker + soft-fade underrun recovery
    double lp_l_ = 0.0;
    double lp_r_ = 0.0;
    double dc_in_prev_l_ = 0.0;
    double dc_out_prev_l_ = 0.0;
    double dc_in_prev_r_ = 0.0;
    double dc_out_prev_r_ = 0.0;
    double underrun_fade_l_ = 0.0;
    double underrun_fade_r_ = 0.0;

    // Interleaved stereo PCM ring buffer ([L0, R0, L1, R1, ...])
    std::array<i16, RING_CAPACITY * 2> ring_{};
    std::size_t ring_head_ = 0;
    std::size_t ring_tail_ = 0;
    std::size_t ring_count_ = 0;
};

} // namespace hm::hal
