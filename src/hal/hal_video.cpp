#include "hal_video.hpp"

#include <SDL.h>

#include <algorithm>

namespace hm::hal {

HalVideo::~HalVideo() {
    shutdown();
}

bool HalVideo::init(const std::string& title, int initial_scale, bool headless) {
    shutdown();
    scale_ = std::clamp(initial_scale, 1, 6);
    headless_ = headless;
    clear();

    if (!headless_) {
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0"); // Nearest-neighbor crisp pixels

        const int win_w = static_cast<int>(SCREEN_WIDTH) * scale_;
        const int win_h = static_cast<int>(SCREEN_HEIGHT) * scale_;
        window_ = SDL_CreateWindow(
            title.c_str(),
            SDL_WINDOWPOS_CENTERED,
            SDL_WINDOWPOS_CENTERED,
            win_w,
            win_h,
            SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
        if (!window_) return false;

        SDL_SetWindowMinimumSize(window_, static_cast<int>(SCREEN_WIDTH), static_cast<int>(SCREEN_HEIGHT));

        renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED);
        if (!renderer_) {
            renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
        }
        if (!renderer_) {
            shutdown();
            return false;
        }

        texture_ = SDL_CreateTexture(
            renderer_,
            SDL_PIXELFORMAT_ABGR8888,
            SDL_TEXTUREACCESS_STREAMING,
            static_cast<int>(SCREEN_WIDTH),
            static_cast<int>(SCREEN_HEIGHT));
        if (!texture_) {
            shutdown();
            return false;
        }
    }

    initialized_ = true;
    return true;
}

void HalVideo::shutdown() {
    if (texture_) {
        SDL_DestroyTexture(texture_);
        texture_ = nullptr;
    }
    if (renderer_) {
        SDL_DestroyRenderer(renderer_);
        renderer_ = nullptr;
    }
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    initialized_ = false;
}

void HalVideo::set_integer_scale(int scale) {
    scale_ = std::clamp(scale, 1, 6);
    if (window_ && !fullscreen_) {
        SDL_SetWindowSize(window_, static_cast<int>(SCREEN_WIDTH) * scale_, static_cast<int>(SCREEN_HEIGHT) * scale_);
        SDL_SetWindowPosition(window_, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
}

void HalVideo::toggle_fullscreen() {
    if (!window_) return;
    fullscreen_ = !fullscreen_;
    SDL_SetWindowFullscreen(
        window_, fullscreen_ ? static_cast<Uint32>(SDL_WINDOW_FULLSCREEN_DESKTOP) : 0u);
}

void HalVideo::set_title(const std::string& title) {
    if (window_) {
        SDL_SetWindowTitle(window_, title.c_str());
    }
}

void HalVideo::clear(u32 rgba) {
    framebuffer_.fill(rgba);
}

void HalVideo::present() {
    if (headless_ || !renderer_ || !texture_) return;

    SDL_UpdateTexture(texture_, nullptr, framebuffer_.data(), static_cast<int>(SCREEN_WIDTH * sizeof(u32)));

    int out_w = 0, out_h = 0;
    SDL_GetRendererOutputSize(renderer_, &out_w, &out_h);

    const int max_sx = std::max(1, out_w / static_cast<int>(SCREEN_WIDTH));
    const int max_sy = std::max(1, out_h / static_cast<int>(SCREEN_HEIGHT));
    const int int_scale = std::min(max_sx, max_sy);

    SDL_Rect dst{};
    dst.w = static_cast<int>(SCREEN_WIDTH) * int_scale;
    dst.h = static_cast<int>(SCREEN_HEIGHT) * int_scale;
    dst.x = (out_w - dst.w) / 2;
    dst.y = (out_h - dst.h) / 2;

    SDL_SetRenderDrawColor(renderer_, 10, 12, 16, 255);
    SDL_RenderClear(renderer_);
    SDL_RenderCopy(renderer_, texture_, nullptr, &dst);
    SDL_RenderPresent(renderer_);
}

} // namespace hm::hal
