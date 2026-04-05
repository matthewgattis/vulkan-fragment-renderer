#include "window.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <SDL3/SDL_vulkan.h>
#include <array>
#include <stdexcept>

static auto logger = spdlog::stdout_color_mt("window");

namespace vfr {

Window::Window(const std::string& title, bool high_dpi) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        throw std::runtime_error(std::string("SDL_Init failed: ") + SDL_GetError());
    }

    // Pick the largest 4:3 resolution that fits the primary display's usable area
    struct Resolution { int w, h; };
    constexpr std::array<Resolution, 9> candidates = {{
        {3200, 2400}, {2560, 1920}, {2048, 1536}, {1600, 1200}, {1440, 1080},
        {1280, 960}, {1024, 768}, {800, 600}, {640, 480},
    }};

    int width = 1280;
    int height = 960;

    SDL_DisplayID primary = SDL_GetPrimaryDisplay();
    SDL_Rect usable;
    if (primary && SDL_GetDisplayUsableBounds(primary, &usable)) {
        bool found = false;
        for (const auto& res : candidates) {
            if (res.w < usable.w && res.h < usable.h) {
                width = res.w;
                height = res.h;
                found = true;
                break;
            }
        }
        if (!found) {
            width = candidates.back().w;
            height = candidates.back().h;
        }
        logger->info("display usable area: {}x{}, selected window size: {}x{}",
                     usable.w, usable.h, width, height);
    } else {
        logger->warn("could not query display, using fallback size: {}x{}", width, height);
    }

    SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE;
    if (high_dpi) {
        flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
    }

    window_ = SDL_CreateWindow(title.c_str(), width, height, flags);
    if (!window_) {
        throw std::runtime_error(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
    }

    logger->info("created {}x{} window (dpi scale: {:.1f})", width, height, dpi_scale());
}

Window::~Window() {
    if (window_) SDL_DestroyWindow(window_);
    SDL_Quit();
}

std::vector<const char*> Window::required_extensions() const {
    Uint32 count = 0;
    const char* const* names = SDL_Vulkan_GetInstanceExtensions(&count);
    return {names, names + count};
}

VkSurfaceKHR Window::create_surface(VkInstance instance) const {
    VkSurfaceKHR surface;
    if (!SDL_Vulkan_CreateSurface(window_, instance, nullptr, &surface)) {
        throw std::runtime_error(std::string("SDL_Vulkan_CreateSurface failed: ") + SDL_GetError());
    }
    return surface;
}

int Window::width() const {
    int w, h;
    SDL_GetWindowSize(window_, &w, &h);
    return w;
}

int Window::height() const {
    int w, h;
    SDL_GetWindowSize(window_, &w, &h);
    return h;
}

int Window::drawable_width() const {
    int w, h;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    return w;
}

int Window::drawable_height() const {
    int w, h;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    return h;
}

float Window::dpi_scale() const {
    return SDL_GetWindowDisplayScale(window_);
}

void Window::set_size(int w, int h) {
    SDL_SetWindowSize(window_, w, h);
}

void Window::toggle_fullscreen() {
    bool is_fullscreen = SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN;
    SDL_SetWindowFullscreen(window_, !is_fullscreen);
}

} // namespace vfr
