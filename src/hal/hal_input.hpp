#pragma once

#include "gba_types.hpp"

struct _SDL_GameController;
union SDL_Event;

namespace hm::hal {

class GbaBus;

struct HostActions {
    bool quit = false;
    int  scale_delta = 0;       // +/- scale or F1..F4
    int  set_scale = 0;         // explicit 1..6
    bool toggle_fullscreen = false;
    bool quick_save = false;
    bool quick_load = false;
    bool toggle_mute = false;
    bool start_host = false;
    bool join_client = false;
};

class HalInput {
public:
    HalInput() = default;
    ~HalInput();

    HalInput(HalInput const&) = delete;
    HalInput& operator=(HalInput const&) = delete;

    void init(bool headless = false);
    void shutdown();

    HostActions poll_events(GbaBus* bus = nullptr);

    // Direct programmatic state (for unit tests / remote sync)
    void set_pressed(u16 key_mask, bool pressed);
    void apply_to_bus(GbaBus& bus) const;

    u16 held_mask() const { return held_; }
    u16 just_pressed_mask() const { return pressed_edge_; }
    u16 just_released_mask() const { return released_edge_; }
    u16 keyinput_reg() const { return static_cast<u16>((~held_) & KEY_ALL); }

    bool is_held(KeyMask k) const { return (held_ & k) != 0; }
    bool is_just_pressed(KeyMask k) const { return (pressed_edge_ & k) != 0; }

private:
    void open_first_controller();
    u16 read_keyboard_state() const;
    u16 read_controller_state() const;

    _SDL_GameController* controller_ = nullptr;
    bool headless_ = false;
    u16 prev_held_ = 0;
    u16 held_ = 0;
    u16 pressed_edge_ = 0;
    u16 released_edge_ = 0;
    u16 override_mask_ = 0;
};

} // namespace hm::hal
