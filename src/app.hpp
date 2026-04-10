#pragma once

#include "window.hpp"
#include "engine.hpp"
#include "camera.hpp"
#include "pipeline.hpp"
#include "ui.hpp"
#include "xr_session.hpp"

#include <glm/glm.hpp>
#include <filesystem>
#include <array>
#include <memory>

namespace vfr {

// Set 0: common frame data — used by all shaders (rasterized geometry, ray marching, etc.)
struct FrameUbo {
    glm::mat4 view;        // 64
    glm::mat4 projection;  // 64
    glm::vec4 resolution;  // 16 — x, y, aspect, unused
    float time;            // 4
    float _pad[3];         // 12
};

static_assert(sizeof(FrameUbo) == 160);

// Set 1: ray marching data — precomputed inverses to avoid per-fragment inverse()
struct RayUbo {
    glm::mat4 inv_view;        // 64
    glm::mat4 inv_projection;  // 64
};

static_assert(sizeof(RayUbo) == 128);

class App {
public:
    App(const std::filesystem::path& shader_path, bool high_dpi, bool xr_enabled);
    ~App();
    void run();

private:
    void process_events();
    void reload_shader();
    void unload_shader();
    void build_pipeline(const std::vector<uint32_t>& frag_spirv);
    void create_frame_descriptors();
    void update_frame_ubo(uint32_t frame_index);
    void update_frame_ubo_xr(uint32_t eye, uint32_t frame_index,
                              const glm::mat4& view, const glm::mat4& projection,
                              vk::Extent2D extent);
    void run_desktop_frame(float dt);
    void run_xr_frame(float dt);
    bool blit_xr_mirror(vk::CommandBuffer cmd);

    bool xr_mode_ = false;
    std::unique_ptr<XrSession> xr_session_;

    Window window_;
    Engine engine_;
    Ui ui_;
    Camera camera_;

    std::filesystem::path shader_path_;
    std::unique_ptr<Pipeline> pipeline_;
    // Fullscreen vertex SPIR-V is embedded via generated header (fullscreen_vert_spv.h).

    // UBO resources
    struct MappedBuffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        void* mapped = nullptr;
    };

    // Set 0: FrameUbo (common frame data)
    vk::raii::DescriptorSetLayout frame_layout_{nullptr};
    // Set 1: RayUbo (inverse matrices for ray marching)
    vk::raii::DescriptorSetLayout ray_layout_{nullptr};

    vk::raii::DescriptorPool descriptor_pool_{nullptr};

    // Desktop descriptor sets and buffers
    std::array<vk::DescriptorSet, MAX_FRAMES_IN_FLIGHT> frame_sets_{};
    std::array<MappedBuffer, MAX_FRAMES_IN_FLIGHT> frame_buffers_{};
    std::array<vk::DescriptorSet, MAX_FRAMES_IN_FLIGHT> ray_sets_{};
    std::array<MappedBuffer, MAX_FRAMES_IN_FLIGHT> ray_buffers_{};

    // Per-eye descriptor sets and buffers for XR stereo rendering
    std::array<std::array<vk::DescriptorSet, MAX_FRAMES_IN_FLIGHT>, 2> xr_eye_frame_sets_{};
    std::array<std::array<MappedBuffer, MAX_FRAMES_IN_FLIGHT>, 2> xr_eye_frame_buffers_{};
    std::array<std::array<vk::DescriptorSet, MAX_FRAMES_IN_FLIGHT>, 2> xr_eye_ray_sets_{};
    std::array<std::array<MappedBuffer, MAX_FRAMES_IN_FLIGHT>, 2> xr_eye_ray_buffers_{};

    // HMD world-space basis (from previous frame, for input processing)
    bool hmd_active_ = false;
    glm::vec3 hmd_forward_{0.0f, 0.0f, -1.0f};
    glm::vec3 hmd_right_{1.0f, 0.0f, 0.0f};
    glm::vec3 hmd_up_{0.0f, 1.0f, 0.0f};

    void trap_mouse();
    void untrap_mouse();

    bool running_ = true;
    bool mouse_trapped_ = false;
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
