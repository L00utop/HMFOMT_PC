#include "hal_audio.hpp"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace hm::hal {

namespace {

constexpr u64 GBA_CLOCK_HZ_U64 = 16777216ULL;
constexpr double PI = 3.14159265358979323846;

constexpr u8 DUTY_TABLE[4][8] = {
    {0, 0, 0, 0, 0, 0, 0, 1}, // 12.5%
    {1, 0, 0, 0, 0, 0, 0, 1}, // 25%
    {1, 0, 0, 0, 0, 1, 1, 1}, // 50%
    {0, 1, 1, 1, 1, 1, 1, 0}, // 75%
};

constexpr u32 NOISE_DIVISORS[8] = {8, 16, 32, 48, 64, 80, 96, 112};

double soft_limit(double x) {
    constexpr double KNEE = 28000.0;
    constexpr double HEAD = 4500.0;
    const double ax = std::abs(x);
    if (ax <= KNEE) return x;
    const double excess = (ax - KNEE) / 8000.0;
    const double compressed = KNEE + HEAD * std::tanh(excess);
    return (x < 0.0) ? -compressed : compressed;
}

} // namespace

HalAudio::~HalAudio() {
    shutdown();
}

bool HalAudio::init(u32 sample_rate, bool headless) {
    shutdown();
    std::lock_guard<std::mutex> lock(mutex_);
    sample_rate_ = sample_rate > 0 ? sample_rate : DEFAULT_SAMPLE_RATE;
    headless_ = headless;
    reset_locked();

    if (!headless_) {
        SDL_AudioSpec desired{};
        desired.freq = static_cast<int>(sample_rate_);
        desired.format = AUDIO_S16SYS;
        desired.channels = 2;
        desired.samples = 512;
        desired.callback = &HalAudio::sdl_audio_callback;
        desired.userdata = this;

        SDL_AudioSpec obtained{};
        device_id_ = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
        if (device_id_ != 0) {
            if (obtained.freq > 0) {
                sample_rate_ = static_cast<u32>(obtained.freq);
            }
            SDL_PauseAudioDevice(device_id_, 0);
        }
    }

    initialized_ = true;
    return true;
}

void HalAudio::shutdown() {
    if (device_id_ != 0) {
        SDL_CloseAudioDevice(device_id_);
        device_id_ = 0;
    }
    initialized_ = false;
}

void HalAudio::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    reset_locked();
}

void HalAudio::reset_locked() {
    ch1_ = SquareChannel{};
    ch1_.has_sweep = true;
    ch2_ = SquareChannel{};
    ch2_.has_sweep = false;
    ch3_ = WaveChannel{};
    ch4_ = NoiseChannel{};
    fifos_[0] = DirectFifo{};
    fifos_[1] = DirectFifo{};
    soundcnt_l_ = 0;
    soundcnt_h_ = 0;
    soundcnt_x_ = 0x0080;
    soundbias_ = 0x0200;
    pending_cycles_ = 0;
    sample_cycle_accum_ = 0;
    seq_accum_ = 0.0;
    seq_step_ = 0;
    lp_l_ = 0.0;
    lp_r_ = 0.0;
    dc_in_prev_l_ = 0.0;
    dc_out_prev_l_ = 0.0;
    dc_in_prev_r_ = 0.0;
    dc_out_prev_r_ = 0.0;
    underrun_fade_l_ = 0.0;
    underrun_fade_r_ = 0.0;
    ring_head_ = 0;
    ring_tail_ = 0;
    ring_count_ = 0;
}

void HalAudio::set_fifo_refill_callback(FifoRefillCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    fifo_refill_cb_ = std::move(cb);
}

void HalAudio::set_muted(bool muted) {
    std::lock_guard<std::mutex> lock(mutex_);
    muted_ = muted;
}

std::size_t HalAudio::fifo_count(int fifo_index) const {
    if (fifo_index < 0 || fifo_index > 1) return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    return fifos_[fifo_index].count;
}

std::size_t HalAudio::buffered_frames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ring_count_;
}

void HalAudio::clear_fifo(int fifo_index) {
    if (fifo_index < 0 || fifo_index > 1) return;
    std::lock_guard<std::mutex> lock(mutex_);
    clear_fifo_locked(fifo_index);
}

void HalAudio::clear_fifo_locked(int fifo_index) {
    auto& fifo = fifos_[fifo_index];
    fifo.head = 0;
    fifo.tail = 0;
    fifo.count = 0;
    fifo.cycles_since_pop = 0;
}

void HalAudio::push_fifo_word(int fifo_index, u32 word) {
    if (fifo_index < 0 || fifo_index > 1) return;
    std::lock_guard<std::mutex> lock(mutex_);
    flush_cycles_locked();
    auto& fifo = fifos_[fifo_index];
    for (int i = 0; i < 4; ++i) {
        if (fifo.count >= FIFO_CAPACITY) {
            fifo.head = (fifo.head + 1) % FIFO_CAPACITY;
            fifo.count--;
        }
        fifo.buffer[fifo.tail] = static_cast<i8>((word >> (i * 8)) & 0xFFu);
        fifo.tail = (fifo.tail + 1) % FIFO_CAPACITY;
        fifo.count++;
    }
}

void HalAudio::step_cycles(u32 cpu_cycles) {
    pending_cycles_ += cpu_cycles;
    if (pending_cycles_ >= 256u) {
        std::lock_guard<std::mutex> lock(mutex_);
        flush_cycles_locked();
    }
}

void HalAudio::flush_frame() {
    std::lock_guard<std::mutex> lock(mutex_);
    flush_cycles_locked();
}

void HalAudio::push_ring_locked(i16 l, i16 r) {
    if (ring_count_ >= RING_CAPACITY) {
        // Drop oldest frame only on overflow (headless unconsumed buffer or hard spike)
        ring_head_ = (ring_head_ + 1) % RING_CAPACITY;
        --ring_count_;
    }
    ring_[ring_tail_ * 2 + 0] = l;
    ring_[ring_tail_ * 2 + 1] = r;
    ring_tail_ = (ring_tail_ + 1) % RING_CAPACITY;
    ++ring_count_;
}

void HalAudio::flush_cycles_locked() {
    u32 cy = pending_cycles_;
    pending_cycles_ = 0;
    if (cy == 0) return;

    u32 eff_rate = sample_rate_ > 0 ? sample_rate_ : DEFAULT_SAMPLE_RATE;
    if (!headless_ && device_id_ != 0 && ring_count_ > 0) {
        const std::size_t target = std::clamp<std::size_t>(
            (static_cast<std::size_t>(sample_rate_) * 45u) / 1000u, 1024u, 4096u);
        const double err = static_cast<double>(target) - static_cast<double>(ring_count_);
        const double adj = std::clamp(err / (static_cast<double>(target) * 2.0), -0.006, 0.006);
        eff_rate = static_cast<u32>(std::max<long long>(8000LL,
            std::llround(static_cast<double>(sample_rate_) * (1.0 + adj))));
    }
    const double dt = 1.0 / static_cast<double>(eff_rate);

    while (cy > 0) {
        const u64 needed_num = (sample_cycle_accum_ < GBA_CLOCK_HZ_U64)
            ? (GBA_CLOCK_HZ_U64 - sample_cycle_accum_)
            : 0ULL;
        const u32 cy_to_next = static_cast<u32>((needed_num + eff_rate - 1ULL) / eff_rate);
        if (cy < cy_to_next) {
            fifos_[0].cycles_since_pop += cy;
            fifos_[1].cycles_since_pop += cy;
            sample_cycle_accum_ += static_cast<u64>(cy) * eff_rate;
            break;
        }
        cy -= cy_to_next;
        fifos_[0].cycles_since_pop += cy_to_next;
        fifos_[1].cycles_since_pop += cy_to_next;
        sample_cycle_accum_ = (sample_cycle_accum_ + static_cast<u64>(cy_to_next) * eff_rate) - GBA_CLOCK_HZ_U64;

        i16 sl = 0, sr = 0;
        synthesize_one_sample_locked(dt, sl, sr);
        push_ring_locked(sl, sr);
    }
}

void HalAudio::on_timer_overflow(int timer_id, u32 timer_sample_hz) {
    FifoRefillCallback refill;
    int need_refill[2] = {0, 0};

    {
        std::lock_guard<std::mutex> lock(mutex_);
        flush_cycles_locked();

        const int timer_a = (soundcnt_h_ >> 10) & 1;
        const int timer_b = (soundcnt_h_ >> 14) & 1;

        for (int idx = 0; idx < 2; ++idx) {
            const int target_timer = (idx == 0) ? timer_a : timer_b;
            if (target_timer != timer_id) continue;

            auto& fifo = fifos_[idx];
            if (timer_sample_hz > 0) {
                fifo.last_rate_hz = timer_sample_hz;
                fifo.period_cycles = std::max<u32>(1u,
                    static_cast<u32>((GBA_CLOCK_HZ_U64 + timer_sample_hz / 2u) / timer_sample_hz));
            }
            fifo.prev_sample = fifo.current_sample;
            if (fifo.count > 0) {
                fifo.current_sample = fifo.buffer[fifo.head];
                fifo.head = (fifo.head + 1) % FIFO_CAPACITY;
                fifo.count--;
            }
            fifo.cycles_since_pop = 0;

            if (fifo.count <= 16 && fifo_refill_cb_) {
                need_refill[idx] = 1;
                refill = fifo_refill_cb_;
            }
        }
    }

    if (refill) {
        if (need_refill[0]) refill(0);
        if (need_refill[1]) refill(1);
    }
}

void HalAudio::write_wave_ram(u32 offset_from_090, u32 value) {
    std::lock_guard<std::mutex> lock(mutex_);
    flush_cycles_locked();
    const u32 bank_base = (ch3_.bank_select != 0) ? 0u : 16u; // Write goes to inactive bank
    const u32 byte_off = (offset_from_090 & 0x0Cu);
    for (u32 i = 0; i < 4; ++i) {
        if (bank_base + byte_off + i < ch3_.ram.size()) {
            ch3_.ram[bank_base + byte_off + i] = static_cast<u8>((value >> (i * 8)) & 0xFFu);
        }
    }
}

void HalAudio::write_register(u32 io_offset, u16 value, bool high_byte_only) {
    std::lock_guard<std::mutex> lock(mutex_);
    flush_cycles_locked();
    (void)high_byte_only;

    switch (io_offset) {
        case reg::SOUND1CNT_L: {
            ch1_.sweep_period = static_cast<u8>((value >> 4) & 0x07u);
            ch1_.sweep_negate = (value & 0x08u) != 0;
            ch1_.sweep_shift = static_cast<u8>(value & 0x07u);
            break;
        }
        case reg::SOUND1CNT_H: {
            ch1_.duty = static_cast<u8>((value >> 6) & 0x03u);
            ch1_.length_counter = static_cast<u8>(64u - (value & 0x3Fu));
            ch1_.env_init_vol = static_cast<u8>((value >> 12) & 0x0Fu);
            ch1_.env_add = (value & 0x0800u) != 0;
            ch1_.env_period = static_cast<u8>((value >> 8) & 0x07u);
            if ((value & 0xF800u) == 0u) {
                ch1_.enabled = false;
            }
            break;
        }
        case reg::SOUND1CNT_X: {
            ch1_.raw_freq = value & 0x07FFu;
            ch1_.length_enable = (value & 0x4000u) != 0;
            if (value & 0x8000u) {
                if (ch1_.env_init_vol > 0 || ch1_.env_add) {
                    ch1_.enabled = true;
                }
                ch1_.volume = ch1_.env_init_vol;
                ch1_.env_timer = ch1_.env_period;
                ch1_.sweep_shadow_freq = ch1_.raw_freq;
                ch1_.sweep_timer = ch1_.sweep_period ? ch1_.sweep_period : 8;
                if (ch1_.length_counter == 0) ch1_.length_counter = 64;
            }
            break;
        }
        case reg::SOUND2CNT_L: {
            ch2_.duty = static_cast<u8>((value >> 6) & 0x03u);
            ch2_.length_counter = static_cast<u8>(64u - (value & 0x3Fu));
            ch2_.env_init_vol = static_cast<u8>((value >> 12) & 0x0Fu);
            ch2_.env_add = (value & 0x0800u) != 0;
            ch2_.env_period = static_cast<u8>((value >> 8) & 0x07u);
            if ((value & 0xF800u) == 0u) {
                ch2_.enabled = false;
            }
            break;
        }
        case reg::SOUND2CNT_H: {
            ch2_.raw_freq = value & 0x07FFu;
            ch2_.length_enable = (value & 0x4000u) != 0;
            if (value & 0x8000u) {
                if (ch2_.env_init_vol > 0 || ch2_.env_add) {
                    ch2_.enabled = true;
                }
                ch2_.volume = ch2_.env_init_vol;
                ch2_.env_timer = ch2_.env_period;
                if (ch2_.length_counter == 0) ch2_.length_counter = 64;
            }
            break;
        }
        case reg::SOUND3CNT_L: {
            ch3_.dimension_64 = (value & 0x0020u) != 0;
            ch3_.bank_select = static_cast<u8>((value >> 6) & 1u);
            ch3_.dac_enabled = (value & 0x0080u) != 0;
            if (!ch3_.dac_enabled) ch3_.enabled = false;
            break;
        }
        case reg::SOUND3CNT_H: {
            ch3_.length_counter = static_cast<u16>(256u - (value & 0xFFu));
            ch3_.vol_code = static_cast<u8>((value >> 13) & 0x07u);
            break;
        }
        case reg::SOUND3CNT_X: {
            ch3_.raw_freq = value & 0x07FFu;
            ch3_.length_enable = (value & 0x4000u) != 0;
            if ((value & 0x8000u) && ch3_.dac_enabled) {
                ch3_.enabled = true;
                ch3_.phase = 0.0;
                if (ch3_.length_counter == 0) ch3_.length_counter = 256;
            }
            break;
        }
        case reg::SOUND4CNT_L: {
            ch4_.length_counter = static_cast<u8>(64u - (value & 0x3Fu));
            ch4_.env_init_vol = static_cast<u8>((value >> 12) & 0x0Fu);
            ch4_.env_add = (value & 0x0800u) != 0;
            ch4_.env_period = static_cast<u8>((value >> 8) & 0x07u);
            if ((value & 0xF800u) == 0u) {
                ch4_.enabled = false;
            }
            break;
        }
        case reg::SOUND4CNT_H: {
            ch4_.divisor_code = static_cast<u8>(value & 0x07u);
            ch4_.width_7bit = (value & 0x08u) != 0;
            ch4_.clock_shift = static_cast<u8>((value >> 4) & 0x0Fu);
            ch4_.length_enable = (value & 0x4000u) != 0;
            if (value & 0x8000u) {
                if (ch4_.env_init_vol > 0 || ch4_.env_add) {
                    ch4_.enabled = true;
                }
                ch4_.volume = ch4_.env_init_vol;
                ch4_.env_timer = ch4_.env_period;
                ch4_.lfsr = 0x7FFFu;
                if (ch4_.length_counter == 0) ch4_.length_counter = 64;
            }
            break;
        }
        case reg::SOUNDCNT_L:
            soundcnt_l_ = value;
            break;
        case reg::SOUNDCNT_H:
            soundcnt_h_ = value & static_cast<u16>(~0x8800u);
            if (value & 0x0800u) clear_fifo_locked(0);
            if (value & 0x8000u) clear_fifo_locked(1);
            break;
        case reg::SOUNDCNT_X:
            soundcnt_x_ = (soundcnt_x_ & 0x0Fu) | (value & 0x0080u);
            if (!(soundcnt_x_ & 0x0080u)) {
                ch1_.enabled = false;
                ch2_.enabled = false;
                ch3_.enabled = false;
                ch4_.enabled = false;
            }
            break;
        case reg::SOUNDBIAS:
            soundbias_ = value;
            break;
        default:
            break;
    }
}

void HalAudio::step_square_envelope(SquareChannel& ch) {
    if (!ch.enabled || ch.env_period == 0) return;
    if (ch.env_timer > 0) ch.env_timer--;
    if (ch.env_timer == 0) {
        ch.env_timer = ch.env_period;
        if (ch.env_add && ch.volume < 15) ch.volume++;
        else if (!ch.env_add && ch.volume > 0) ch.volume--;
    }
}

void HalAudio::step_square_sweep(SquareChannel& ch) {
    if (!ch.enabled || !ch.has_sweep || ch.sweep_period == 0) return;
    if (ch.sweep_timer > 0) ch.sweep_timer--;
    if (ch.sweep_timer == 0) {
        ch.sweep_timer = ch.sweep_period;
        if (ch.sweep_shift > 0) {
            u16 delta = ch.sweep_shadow_freq >> ch.sweep_shift;
            i32 next = ch.sweep_negate
                ? static_cast<i32>(ch.sweep_shadow_freq) - delta
                : static_cast<i32>(ch.sweep_shadow_freq) + delta;
            if (next >= 2048) {
                ch.enabled = false;
            } else if (next >= 0) {
                ch.sweep_shadow_freq = static_cast<u16>(next);
                ch.raw_freq = ch.sweep_shadow_freq;
            }
        }
    }
}

void HalAudio::step_frame_sequencer() {
    // 512 Hz frame sequencer:
    // Step 0,2,4,6: Length; Step 2,6: Sweep; Step 7: Envelope
    if ((seq_step_ % 2) == 0) {
        auto tick_len = [](auto& ch) {
            if (ch.enabled && ch.length_enable && ch.length_counter > 0) {
                if (--ch.length_counter == 0) ch.enabled = false;
            }
        };
        tick_len(ch1_);
        tick_len(ch2_);
        tick_len(ch3_);
        tick_len(ch4_);
    }
    if (seq_step_ == 2 || seq_step_ == 6) {
        step_square_sweep(ch1_);
    }
    if (seq_step_ == 7) {
        step_square_envelope(ch1_);
        step_square_envelope(ch2_);
        if (ch4_.enabled && ch4_.env_period > 0) {
            if (ch4_.env_timer > 0) ch4_.env_timer--;
            if (ch4_.env_timer == 0) {
                ch4_.env_timer = ch4_.env_period;
                if (ch4_.env_add && ch4_.volume < 15) ch4_.volume++;
                else if (!ch4_.env_add && ch4_.volume > 0) ch4_.volume--;
            }
        }
    }
    seq_step_ = static_cast<u8>((seq_step_ + 1) & 7u);
}

i32 HalAudio::sample_square(SquareChannel& ch, double dt) {
    if (!ch.enabled || ch.volume == 0) return 0;
    const double freq_hz = 131072.0 / static_cast<double>(2048u - (ch.raw_freq & 0x07FFu));
    if (freq_hz > 18000.0) return 0;
    ch.phase += freq_hz * 8.0 * dt;
    if (ch.phase >= 8.0) {
        ch.phase = std::fmod(ch.phase, 8.0);
    }
    while (ch.phase >= 1.0) {
        ch.phase -= 1.0;
        ch.duty_step = static_cast<u8>((ch.duty_step + 1) & 7u);
    }
    const i32 polarity = DUTY_TABLE[ch.duty & 3u][ch.duty_step] ? 1 : -1;
    return polarity * static_cast<i32>(ch.volume) * 140;
}

i32 HalAudio::sample_wave(double dt) {
    if (!ch3_.enabled || !ch3_.dac_enabled || ch3_.vol_code == 0) return 0;
    const double freq_hz = 2097152.0 / static_cast<double>(2048u - (ch3_.raw_freq & 0x07FFu));
    const u32 sample_count = ch3_.dimension_64 ? 64u : 32u;
    if (freq_hz / static_cast<double>(sample_count) > 18000.0) return 0;
    ch3_.phase += freq_hz * dt;
    if (ch3_.phase >= static_cast<double>(sample_count)) {
        ch3_.phase = std::fmod(ch3_.phase, static_cast<double>(sample_count));
    }

    const u32 idx = static_cast<u32>(ch3_.phase) % sample_count;
    const u32 bank_off = ch3_.dimension_64 ? 0u : static_cast<u32>(ch3_.bank_select) * 16u;
    const u8 byte_val = ch3_.ram[bank_off + (idx >> 1)];
    const i32 nibble = (idx & 1u) ? (byte_val & 0x0Fu) : ((byte_val >> 4) & 0x0Fu);
    const i32 centered = nibble - 8;

    switch (ch3_.vol_code) {
        case 1: return centered * 180;
        case 2: return (centered * 180) >> 1;
        case 3: return (centered * 180) >> 2;
        case 4: return (centered * 180 * 3) >> 2;
        default: return 0;
    }
}

i32 HalAudio::sample_noise(double dt) {
    if (!ch4_.enabled || ch4_.volume == 0 || ch4_.clock_shift >= 14u) return 0;
    const u32 div = NOISE_DIVISORS[ch4_.divisor_code & 7u];
    const double rate_hz = 4194304.0 / (static_cast<double>(div) * static_cast<double>(1u << ch4_.clock_shift));
    ch4_.phase += rate_hz * dt;
    int steps = 0;
    while (ch4_.phase >= 1.0 && steps < 32) {
        ch4_.phase -= 1.0;
        ++steps;
        const u16 xor_bit = static_cast<u16>((ch4_.lfsr ^ (ch4_.lfsr >> 1)) & 1u);
        ch4_.lfsr = static_cast<u16>((ch4_.lfsr >> 1) | (xor_bit << 14));
        if (ch4_.width_7bit) {
            ch4_.lfsr = static_cast<u16>((ch4_.lfsr & ~(1u << 6)) | (xor_bit << 6));
        }
    }
    if (ch4_.phase >= 1.0) ch4_.phase = std::fmod(ch4_.phase, 1.0);
    const i32 polarity = (ch4_.lfsr & 1u) ? -1 : 1;
    return polarity * static_cast<i32>(ch4_.volume) * 130;
}

void HalAudio::synthesize_one_sample_locked(double dt, i16& out_l, i16& out_r) {
    if (muted_ || !(soundcnt_x_ & 0x0080u)) {
        // Gentle filter relaxation avoids abrupt clicks when toggling mute/master switch
        lp_l_ *= 0.99;
        lp_r_ *= 0.99;
        dc_out_prev_l_ *= 0.99;
        dc_out_prev_r_ *= 0.99;
        dc_in_prev_l_ = lp_l_;
        dc_in_prev_r_ = lp_r_;
        out_l = static_cast<i16>(std::lround(dc_out_prev_l_));
        out_r = static_cast<i16>(std::lround(dc_out_prev_r_));
        return;
    }

    const double seq_period = 1.0 / 512.0;
    seq_accum_ += dt;
    while (seq_accum_ >= seq_period) {
        seq_accum_ -= seq_period;
        step_frame_sequencer();
    }

    const i32 s1 = sample_square(ch1_, dt);
    const i32 s2 = sample_square(ch2_, dt);
    const i32 s3 = sample_wave(dt);
    const i32 s4 = sample_noise(dt);

    i32 dmg_l = 0;
    i32 dmg_r = 0;
    if (soundcnt_l_ & 0x1000u) dmg_l += s1;
    if (soundcnt_l_ & 0x2000u) dmg_l += s2;
    if (soundcnt_l_ & 0x4000u) dmg_l += s3;
    if (soundcnt_l_ & 0x8000u) dmg_l += s4;

    if (soundcnt_l_ & 0x0100u) dmg_r += s1;
    if (soundcnt_l_ & 0x0200u) dmg_r += s2;
    if (soundcnt_l_ & 0x0400u) dmg_r += s3;
    if (soundcnt_l_ & 0x0800u) dmg_r += s4;

    const u32 dmg_vol_right = (soundcnt_l_ & 0x07u) + 1u;
    const u32 dmg_vol_left  = ((soundcnt_l_ >> 4) & 0x07u) + 1u;
    const u32 dmg_mix = soundcnt_h_ & 0x03u; // 0=25%, 1=50%, 2=100%

    dmg_l = (dmg_l * static_cast<i32>(dmg_vol_left)) >> 3;
    dmg_r = (dmg_r * static_cast<i32>(dmg_vol_right)) >> 3;
    if (dmg_mix == 0) { dmg_l >>= 2; dmg_r >>= 2; }
    else if (dmg_mix == 1) { dmg_l >>= 1; dmg_r >>= 1; }

    auto interp_fifo = [](const DirectFifo& f) -> double {
        const double p = static_cast<double>(f.prev_sample);
        const double c = static_cast<double>(f.current_sample);
        if (f.period_cycles == 0u) return c;
        const double frac = std::clamp(
            static_cast<double>(f.cycles_since_pop) / static_cast<double>(f.period_cycles),
            0.0, 1.0);
        return p + (c - p) * frac;
    };

    const double dsa = interp_fifo(fifos_[0]) * ((soundcnt_h_ & 0x0004u) ? 140.0 : 70.0);
    const double dsb = interp_fifo(fifos_[1]) * ((soundcnt_h_ & 0x0008u) ? 140.0 : 70.0);

    double mix_l = static_cast<double>(dmg_l);
    double mix_r = static_cast<double>(dmg_r);
    if (soundcnt_h_ & 0x0200u) mix_l += dsa;
    if (soundcnt_h_ & 0x0100u) mix_r += dsa;
    if (soundcnt_h_ & 0x2000u) mix_l += dsb;
    if (soundcnt_h_ & 0x1000u) mix_r += dsb;

    // 14.5 kHz one-pole low-pass filter to attenuate high-frequency DirectSound aliasing
    const double rc = 1.0 / (2.0 * PI * 14500.0);
    const double lp_alpha = dt / (rc + dt);
    lp_l_ += lp_alpha * (mix_l - lp_l_);
    lp_r_ += lp_alpha * (mix_r - lp_r_);

    // 12 Hz DC blocker removes DC offsets & prevents song/SFX start-stop clicks
    const double dc_r = std::exp(-2.0 * PI * 12.0 * dt);
    const double hp_l = lp_l_ - dc_in_prev_l_ + dc_r * dc_out_prev_l_;
    const double hp_r = lp_r_ - dc_in_prev_r_ + dc_r * dc_out_prev_r_;
    dc_in_prev_l_ = lp_l_;
    dc_out_prev_l_ = hp_l;
    dc_in_prev_r_ = lp_r_;
    dc_out_prev_r_ = hp_r;

    const double lim_l = soft_limit(hp_l);
    const double lim_r = soft_limit(hp_r);

    out_l = static_cast<i16>(std::clamp<i32>(static_cast<i32>(std::lround(lim_l)), -32767, 32767));
    out_r = static_cast<i16>(std::clamp<i32>(static_cast<i32>(std::lround(lim_r)), -32767, 32767));
}

void HalAudio::render_samples(i16* stereo_out, std::size_t frame_count) {
    if (!stereo_out || frame_count == 0) return;

    std::lock_guard<std::mutex> lock(mutex_);
    flush_cycles_locked();

    std::size_t i = 0;
    while (i < frame_count && ring_count_ > 0) {
        stereo_out[i * 2 + 0] = ring_[ring_head_ * 2 + 0];
        stereo_out[i * 2 + 1] = ring_[ring_head_ * 2 + 1];
        ring_head_ = (ring_head_ + 1) % RING_CAPACITY;
        --ring_count_;
        ++i;
    }

    if (i < frame_count) {
        const double dt = 1.0 / static_cast<double>(sample_rate_ > 0 ? sample_rate_ : DEFAULT_SAMPLE_RATE);
        for (; i < frame_count; ++i) {
            i16 sl = 0, sr = 0;
            synthesize_one_sample_locked(dt, sl, sr);
            stereo_out[i * 2 + 0] = sl;
            stereo_out[i * 2 + 1] = sr;
        }
    }
}

void HalAudio::drain_ring_for_sdl(i16* stereo_out, std::size_t frame_count) {
    if (!stereo_out || frame_count == 0) return;

    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t i = 0;
    if (!muted_) {
        while (i < frame_count && ring_count_ > 0) {
            const i16 l = ring_[ring_head_ * 2 + 0];
            const i16 r = ring_[ring_head_ * 2 + 1];
            stereo_out[i * 2 + 0] = l;
            stereo_out[i * 2 + 1] = r;
            underrun_fade_l_ = static_cast<double>(l);
            underrun_fade_r_ = static_cast<double>(r);
            ring_head_ = (ring_head_ + 1) % RING_CAPACITY;
            --ring_count_;
            ++i;
        }
    } else {
        // Consume silently while muted so ring doesn't stall when unmuted later
        ring_head_ = (ring_head_ + ring_count_) % RING_CAPACITY;
        ring_count_ = 0;
    }

    // Exponential fade-to-zero on any buffer underrun (e.g. window drag freeze) eliminates pops
    for (; i < frame_count; ++i) {
        underrun_fade_l_ *= 0.990;
        underrun_fade_r_ *= 0.990;
        stereo_out[i * 2 + 0] = static_cast<i16>(std::lround(underrun_fade_l_));
        stereo_out[i * 2 + 1] = static_cast<i16>(std::lround(underrun_fade_r_));
    }
}

void HalAudio::sdl_audio_callback(void* userdata, u8* stream, int len) {
    auto* self = static_cast<HalAudio*>(userdata);
    if (!self || !stream || len <= 0) return;
    const std::size_t frames = static_cast<std::size_t>(len) / (2 * sizeof(i16));
    self->drain_ring_for_sdl(reinterpret_cast<i16*>(stream), frames);
}

} // namespace hm::hal
