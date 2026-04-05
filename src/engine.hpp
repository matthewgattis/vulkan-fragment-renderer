#pragma once

#include <vulkan/vulkan_raii.hpp>
#include <vk_mem_alloc.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vfr {

class Window;

inline constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;

// Extra Vulkan requirements injected by external systems (e.g. OpenXR)
struct ExternalVulkanRequirements {
    std::vector<std::string> instance_extensions;
    std::vector<std::string> device_extensions;
    // Callback to query physical device after VkInstance is created
    std::function<VkPhysicalDevice(VkInstance)> physical_device_query;
};

struct FrameSync {
    vk::raii::Fence in_flight;
    vk::raii::CommandBuffer command_buffer;
};

class Engine {
public:
    Engine(Window& window, const ExternalVulkanRequirements* ext_reqs = nullptr);
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Frame lifecycle — returns nullptr if swapchain unavailable
    vk::CommandBuffer begin_frame();
    void end_frame();

    // Decomposed frame lifecycle for XR mode
    vk::CommandBuffer begin_command_buffer();          // fence wait + cmd begin (no swapchain)
    bool acquire_desktop_image();                       // acquire swapchain image, returns false if out of date
    void begin_desktop_render_pass(vk::CommandBuffer cmd); // begin render pass on desktop framebuffer
    void end_render_pass(vk::CommandBuffer cmd);        // end current render pass
    void submit_and_present();                          // submit cmd + present desktop
    void submit_xr_only();                              // submit cmd (no swapchain semaphores or present)

    // Swapchain recreation
    void recreate_swapchain();

    // Accessors
    const vk::raii::Device& device() const { return device_; }
    const vk::raii::PhysicalDevice& physical_device() const { return physical_device_; }
    const vk::raii::Instance& instance() const { return instance_; }
    VmaAllocator allocator() const { return allocator_; }
    vk::RenderPass render_pass() const { return *render_pass_; }
    vk::Extent2D swapchain_extent() const { return swapchain_extent_; }
    vk::Format swapchain_format() const { return swapchain_format_; }
    vk::Image desktop_image() const { return swapchain_images_[image_index_]; }
    uint32_t frame_index() const { return frame_index_; }
    uint32_t graphics_queue_family() const { return graphics_family_; }
    vk::Queue graphics_queue() const { return *graphics_queue_; }

    // One-shot command submission
    void submit_immediate(std::function<void(vk::CommandBuffer)> fn);

    // Deferred resource destruction
    template <typename T>
    void defer_destroy(T resource) {
        deferred_.push_back({frame_counter_ + MAX_FRAMES_IN_FLIGHT + 1,
                             [r = std::move(resource)]() mutable { r = {}; }});
    }

    void flush_deferred();

private:
    void create_instance(const ExternalVulkanRequirements* ext_reqs);
    void create_device(const ExternalVulkanRequirements* ext_reqs);
    void create_allocator();
    void create_swapchain();
    void create_render_pass();
    void create_framebuffers();
    void create_sync_objects();
    void create_command_pool();

    Window& window_;

    vk::raii::Context context_;
    vk::raii::Instance instance_{nullptr};
    vk::raii::SurfaceKHR surface_{nullptr};
    vk::raii::PhysicalDevice physical_device_{nullptr};
    vk::raii::Device device_{nullptr};
    vk::raii::Queue graphics_queue_{nullptr};
    uint32_t graphics_family_ = 0;

    VmaAllocator allocator_ = VK_NULL_HANDLE;

    // Swapchain
    vk::raii::SwapchainKHR swapchain_{nullptr};
    vk::Format swapchain_format_;
    vk::Extent2D swapchain_extent_;
    std::vector<vk::Image> swapchain_images_;
    std::vector<vk::raii::ImageView> swapchain_views_;

    // Depth buffer
    vk::raii::Image depth_image_{nullptr};
    vk::raii::ImageView depth_view_{nullptr};
    VmaAllocation depth_allocation_ = VK_NULL_HANDLE;

    // Render pass & framebuffers
    vk::raii::RenderPass render_pass_{nullptr};
    std::vector<vk::raii::Framebuffer> framebuffers_;

    // Commands & sync
    vk::raii::CommandPool command_pool_{nullptr};
    std::vector<FrameSync> frames_;
    // Semaphore ring — sized to swapchain image count to avoid reuse conflicts
    std::vector<vk::raii::Semaphore> acquire_semaphores_;
    std::vector<vk::raii::Semaphore> render_semaphores_;
    uint32_t semaphore_index_ = 0;

    uint32_t frame_index_ = 0;
    uint64_t frame_counter_ = 0;
    uint32_t image_index_ = 0;
    vk::Semaphore current_acquire_sem_;
    vk::Semaphore current_render_sem_;

    // Deferred destruction
    struct DeferredDestroy {
        uint64_t deadline;
        std::function<void()> destroy;
    };
    std::vector<DeferredDestroy> deferred_;
};

} // namespace vfr
