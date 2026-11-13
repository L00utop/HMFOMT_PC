#pragma once

#include "gba_types.hpp"

#include <array>
#include <string>

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;

namespace hm::hal {

class HalVideo {
public:
    static constexpr std::size_t FRAMEBUFFER_PIXELS = SCREEN_WIDTH * SCREEN_HEIGHT;

    HalVideo() = default;
    ~HalVideo();

    HalVideo(HalVideo const&) = delete;
    HalVideo& operator=(HalVideo const&) = delete;

    bool init(const std::string& title, int initial_scale = 3, bool headless = false);
    void shutdown();

    void set_integer_scale(int scale);
    int integer_scale() const { return scale_; }
    void toggle_fullscreen();
    void set_title(const std::string& title);

    // Virtual 240x160 RGBA8888 framebuffer (ABGR8888 on little-endian)
    u32* framebuffer() { return framebuffer_.data(); }
    const u32* framebuffer() const { return framebuffer_.data(); }
    void clear(u32 rgba = 0xFF000000u);

    // Present virtual framebuffer with integer scaling and 3:2 aspect ratio
    void present();

    bool is_headless() const { return headless_; }
    bool is_initialized() const { return initialized_; }

private:
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* texture_ = nullptr;

    std::array<u32, FRAMEBUFFER_PIXELS> framebuffer_{};
    int scale_ = 3;
    bool fullscreen_ = false;
    bool headless_ = false;
    bool initialized_ = false;
};

} // namespace hm::hal
