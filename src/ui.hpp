#pragma once

#include <vulkan/vulkan_raii.hpp>
#include <SDL3/SDL.h>

namespace vfr {

class Engine;
class Camera;
class Window;

class Ui {
public:
    Ui(Engine& engine, Window& window);
    ~Ui();

    Ui(const Ui&) = delete;
    Ui& operator=(const Ui&) = delete;

    void process_event(const SDL_Event& event);
    void begin_frame();
    void render(Camera& camera, float fps, bool mouse_trapped);
    void end_frame(vk::CommandBuffer cmd);

    bool visible() const { return visible_; }
    void toggle() { visible_ = !visible_; }
    bool wants_input() const;
    bool wants_mouse() const;

private:
    Engine& engine_;
    bool visible_ = true;

    vk::raii::DescriptorPool imgui_pool_{nullptr};
};

} // namespace vfr
