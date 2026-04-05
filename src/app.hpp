#pragma once

#include "window.hpp"
#include "engine.hpp"
#include "camera.hpp"
#include "pipeline.hpp"
#include "ui.hpp"

#include <glm/glm.hpp>
#include <filesystem>
#include <array>
#include <memory>

namespace vfr {

// Frame-level uniform data (descriptor set 0, binding 0)
struct FrameUbo {
    glm::mat4 view;        // 64
    glm::mat4 projection;  // 64
    glm::vec4 resolution;  // 16 — x, y, aspect, unused
    float time;            // 4
    float _pad[3];         // 12
};

static_assert(sizeof(FrameUbo) == 160);

class App {
public:
    App(const std::filesystem::path& shader_path, bool high_dpi);
    ~App();
    void run();

private:
    void process_events();
    void reload_shader();
    void unload_shader();
    void build_pipeline(const std::vector<uint32_t>& frag_spirv);
    void create_frame_descriptors();
    void update_frame_ubo(uint32_t frame_index);

    Window window_;
    Engine engine_;
    Ui ui_;
    Camera camera_;

    std::filesystem::path shader_path_;
    std::unique_ptr<Pipeline> pipeline_;
    std::vector<uint32_t> vert_spirv_;

    // Frame UBO resources
    vk::raii::DescriptorSetLayout frame_layout_{nullptr};
    vk::raii::DescriptorPool frame_pool_{nullptr};
    std::array<vk::DescriptorSet, MAX_FRAMES_IN_FLIGHT> frame_sets_{};
    struct MappedBuffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        void* mapped = nullptr;
    };
    std::array<MappedBuffer, MAX_FRAMES_IN_FLIGHT> frame_buffers_{};

    bool running_ = true;
    bool mouse_left_ = false;
    bool mouse_right_ = false;
    bool mouse_middle_ = false;

    float time_ = 0.0f;
    float fps_ = 0.0f;
    uint64_t last_tick_ = 0;
    uint64_t fps_tick_ = 0;
    uint32_t fps_frames_ = 0;
};

} // namespace vfr
