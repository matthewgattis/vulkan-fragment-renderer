#pragma once

#include <vulkan/vulkan_raii.hpp>
#include <vk_mem_alloc.h>

#include <functional>
#include <memory>
#include <vector>

namespace vfr {

class Window;

inline constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;

struct FrameSync {
    vk::raii::Fence in_flight;
    vk::raii::CommandBuffer command_buffer;
};

class Engine {
public:
    Engine(Window& window);
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Frame lifecycle — returns nullptr if swapchain unavailable
    vk::CommandBuffer begin_frame();
    void end_frame();

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
    void create_instance();
    void create_device();
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
