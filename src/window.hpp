#pragma once

#include <SDL3/SDL.h>
#include <vulkan/vulkan.hpp>
#include <string>
#include <vector>

namespace vfr {

class Window {
public:
    Window(const std::string& title, bool high_dpi);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    SDL_Window* handle() const { return window_; }
    std::vector<const char*> required_extensions() const;
    VkSurfaceKHR create_surface(VkInstance instance) const;

    int width() const;
    int height() const;
    int drawable_width() const;
    int drawable_height() const;
    float dpi_scale() const;

    void set_size(int w, int h);
    void toggle_fullscreen();

private:
    SDL_Window* window_ = nullptr;
};

} // namespace vfr
