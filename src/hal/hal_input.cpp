#include "hal_input.hpp"
#include "gba_bus.hpp"

#include <SDL.h>

namespace hm::hal {

HalInput::~HalInput() {
    shutdown();
}

void HalInput::init(bool headless) {
    shutdown();
    headless_ = headless;
    prev_held_ = 0;
    held_ = 0;
    pressed_edge_ = 0;
    released_edge_ = 0;
    override_mask_ = 0;

    if (!headless_) {
        open_first_controller();
    }
}

void HalInput::shutdown() {
    if (controller_) {
        SDL_GameControllerClose(controller_);
        controller_ = nullptr;
    }
}

void HalInput::open_first_controller() {
    const int num = SDL_NumJoysticks();
    for (int i = 0; i < num; ++i) {
        if (SDL_IsGameController(i)) {
            controller_ = SDL_GameControllerOpen(i);
            if (controller_) break;
        }
    }
}

u16 HalInput::read_keyboard_state() const {
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    if (!keys) return 0;

    u16 mask = 0;
    if (keys[SDL_SCANCODE_Z] || keys[SDL_SCANCODE_K]) mask |= KEY_A;
    if (keys[SDL_SCANCODE_X] || keys[SDL_SCANCODE_J]) mask |= KEY_B;
    if (keys[SDL_SCANCODE_BACKSPACE] || keys[SDL_SCANCODE_RSHIFT]) mask |= KEY_SELECT;
    if (keys[SDL_SCANCODE_RETURN] || keys[SDL_SCANCODE_SPACE]) mask |= KEY_START;
    if (keys[SDL_SCANCODE_RIGHT] || keys[SDL_SCANCODE_D]) mask |= KEY_RIGHT;
    if (keys[SDL_SCANCODE_LEFT]  || keys[SDL_SCANCODE_A]) mask |= KEY_LEFT;
    if (keys[SDL_SCANCODE_UP]    || keys[SDL_SCANCODE_W]) mask |= KEY_UP;
    if (keys[SDL_SCANCODE_DOWN]  || keys[SDL_SCANCODE_S]) mask |= KEY_DOWN;
    if (keys[SDL_SCANCODE_E] || keys[SDL_SCANCODE_L]) mask |= KEY_R;
    if (keys[SDL_SCANCODE_Q] || keys[SDL_SCANCODE_U]) mask |= KEY_L;
    return mask;
}

u16 HalInput::read_controller_state() const {
    if (!controller_) return 0;
    u16 mask = 0;
    if (SDL_GameControllerGetButton(controller_, SDL_CONTROLLER_BUTTON_A)) mask |= KEY_A;
    if (SDL_GameControllerGetButton(controller_, SDL_CONTROLLER_BUTTON_B)) mask |= KEY_B;
    if (SDL_GameControllerGetButton(controller_, SDL_CONTROLLER_BUTTON_BACK)) mask |= KEY_SELECT;
    if (SDL_GameControllerGetButton(controller_, SDL_CONTROLLER_BUTTON_START)) mask |= KEY_START;
    if (SDL_GameControllerGetButton(controller_, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) mask |= KEY_RIGHT;
    if (SDL_GameControllerGetButton(controller_, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) mask |= KEY_LEFT;
    if (SDL_GameControllerGetButton(controller_, SDL_CONTROLLER_BUTTON_DPAD_UP)) mask |= KEY_UP;
    if (SDL_GameControllerGetButton(controller_, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) mask |= KEY_DOWN;
    if (SDL_GameControllerGetButton(controller_, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) mask |= KEY_R;
    if (SDL_GameControllerGetButton(controller_, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) mask |= KEY_L;

    const Sint16 lx = SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_LEFTX);
    const Sint16 ly = SDL_GameControllerGetAxis(controller_, SDL_CONTROLLER_AXIS_LEFTY);
    constexpr Sint16 DEADZONE = 12000;
    if (lx > DEADZONE) mask |= KEY_RIGHT;
    if (lx < -DEADZONE) mask |= KEY_LEFT;
    if (ly > DEADZONE) mask |= KEY_DOWN;
    if (ly < -DEADZONE) mask |= KEY_UP;

    return mask;
}

HostActions HalInput::poll_events(GbaBus* bus) {
    HostActions actions{};
    if (!headless_) {
        SDL_Event ev{};
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                actions.quit = true;
            } else if (ev.type == SDL_CONTROLLERDEVICEADDED && !controller_) {
                open_first_controller();
            } else if (ev.type == SDL_CONTROLLERDEVICEREMOVED && controller_) {
                SDL_Joystick* joy = SDL_GameControllerGetJoystick(controller_);
                if (joy && SDL_JoystickInstanceID(joy) == ev.cdevice.which) {
                    SDL_GameControllerClose(controller_);
                    controller_ = nullptr;
                }
            } else if (ev.type == SDL_KEYDOWN && !ev.key.repeat) {
                switch (ev.key.keysym.sym) {
                    case SDLK_ESCAPE: actions.quit = true; break;
                    case SDLK_1: actions.set_scale = 1; break;
                    case SDLK_2: actions.set_scale = 2; break;
                    case SDLK_3: actions.set_scale = 3; break;
                    case SDLK_4: actions.set_scale = 4; break;
                    case SDLK_5: actions.set_scale = 5; break;
                    case SDLK_F11: actions.toggle_fullscreen = true; break;
                    case SDLK_F5: actions.quick_save = true; break;
                    case SDLK_F7: actions.quick_load = true; break;
                    case SDLK_F8: actions.toggle_mute = true; break;
                    case SDLK_F9: actions.start_host = true; break;
                    case SDLK_F10: actions.join_client = true; break;
                    default: break;
                }
            }
        }
    }

    prev_held_ = held_;
    if (headless_) {
        held_ = override_mask_;
    } else {
        held_ = static_cast<u16>((read_keyboard_state() | read_controller_state() | override_mask_) & KEY_ALL);
    }
    pressed_edge_  = static_cast<u16>(held_ & ~prev_held_);
    released_edge_ = static_cast<u16>(~held_ & prev_held_);

    if (bus) {
        apply_to_bus(*bus);
    }
    return actions;
}

void HalInput::set_pressed(u16 key_mask, bool pressed) {
    if (pressed) override_mask_ |= (key_mask & KEY_ALL);
    else override_mask_ &= static_cast<u16>(~(key_mask & KEY_ALL));
}

void HalInput::apply_to_bus(GbaBus& bus) const {
    const u16 active_low = keyinput_reg();
    bus.set_keyinput(active_low);

    // Check KEYCNT (0x04000132) IRQ condition
    const u16 keycnt = bus.io_read16(reg::KEYCNT);
    if (keycnt & 0x4000u) {
        const u16 watch = keycnt & KEY_ALL;
        const bool is_and = (keycnt & 0x8000u) != 0;
        const bool match = is_and ? ((held_ & watch) == watch && watch != 0) : ((held_ & watch) != 0);
        if (match) {
            bus.raise_irq(IRQ_KEYPAD);
        }
    }
}

} // namespace hm::hal
