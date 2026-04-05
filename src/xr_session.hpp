#pragma once

#include <vulkan/vulkan_raii.hpp>
#include <vk_mem_alloc.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <glm/glm.hpp>

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace vfr {

struct XrVulkanRequirements {
    std::vector<std::string> instance_extensions;
    std::vector<std::string> device_extensions;
};

struct XrEyeRenderInfo {
    glm::mat4 view;
    glm::mat4 projection;
    XrPosef pose;
    XrFovf fov;
};

class XrSession {
public:
    // Phase 1: before Vulkan init (static)
    static std::optional<XrVulkanRequirements> query_requirements();
    static VkPhysicalDevice query_physical_device(VkInstance instance);
    static bool has_pending_session();

    // Phase 2: after Vulkan init
    XrSession(VkInstance vk_instance, VkPhysicalDevice physical_device,
              VkDevice device, uint32_t queue_family, uint32_t queue_index,
              VmaAllocator allocator, vk::Format color_format, vk::Format depth_format,
              const vk::raii::Device& raii_device);
    ~XrSession();

    XrSession(const XrSession&) = delete;
    XrSession& operator=(const XrSession&) = delete;

    // Per-frame
    void poll_events();
    bool wait_and_begin_frame();
    std::array<XrEyeRenderInfo, 2> locate_views(const glm::vec3& body_position);
    void begin_eye_render(vk::CommandBuffer cmd, uint32_t eye);
    void end_eye_render(vk::CommandBuffer cmd, uint32_t eye);
    void end_frame();

    bool session_running() const { return session_running_; }
    vk::Extent2D eye_extent() const { return eye_extent_; }

private:
    void create_render_pass();
    void create_swapchains();

    glm::mat4 xr_pose_to_view_matrix(const XrPosef& pose, const glm::vec3& body_pos);
    glm::mat4 xr_fov_to_projection(const XrFovf& fov, float near_z, float far_z);

    // XR core
    XrInstance xr_instance_ = XR_NULL_HANDLE;
    XrSystemId xr_system_id_ = XR_NULL_SYSTEM_ID;
    ::XrSession xr_session_ = XR_NULL_HANDLE;
    XrSpace xr_space_ = XR_NULL_HANDLE;
    XrSessionState session_state_ = XR_SESSION_STATE_UNKNOWN;
    bool session_running_ = false;

    // Per-eye swapchain
    struct EyeSwapchain {
        XrSwapchain handle = XR_NULL_HANDLE;
        std::vector<XrSwapchainImageVulkanKHR> images;
        std::vector<vk::raii::ImageView> color_views;
        VkImage depth_image = VK_NULL_HANDLE;
        VmaAllocation depth_alloc = VK_NULL_HANDLE;
        vk::raii::ImageView depth_view{nullptr};
        std::vector<vk::raii::Framebuffer> framebuffers;
        uint32_t current_index = 0;
    };
    std::array<EyeSwapchain, 2> eyes_;
    vk::Extent2D eye_extent_{};

    // XR render pass (final layout = COLOR_ATTACHMENT_OPTIMAL)
    vk::raii::RenderPass render_pass_{nullptr};

    // Frame state
    ::XrFrameState frame_state_{};
    std::array<XrView, 2> xr_views_;
    std::array<XrCompositionLayerProjectionView, 2> projection_views_;

    // Vulkan handles (non-owning)
    VkDevice vk_device_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    const vk::raii::Device* raii_device_ = nullptr;
    vk::Format color_format_;
    vk::Format depth_format_;

    // Static state for two-phase init
    static XrInstance s_xr_instance_;
    static XrSystemId s_xr_system_id_;
};

} // namespace vfr
