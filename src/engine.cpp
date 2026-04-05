#define LOG_MODULE_NAME "engine"

#include "engine.hpp"
#include "window.hpp"
#include "log.hpp"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <algorithm>

static auto logger = spdlog::stdout_color_mt(LOG_MODULE_NAME);

namespace vfr {

Engine::Engine(Window& window) : window_(window) {
    create_instance();

    VkSurfaceKHR raw_surface = window_.create_surface(*instance_);
    surface_ = vk::raii::SurfaceKHR(instance_, raw_surface);

    create_device();
    create_allocator();
    create_swapchain();
    create_render_pass();
    create_framebuffers();
    create_command_pool();
    create_sync_objects();

    LOG_INFO("engine initialized");
}

Engine::~Engine() {
    if (*device_) {
        device_.waitIdle();
    }
    flush_deferred();

    if (depth_allocation_) {
        vmaDestroyImage(allocator_, static_cast<VkImage>(*depth_image_), depth_allocation_);
        // Release RAII handle without double-free
        depth_image_.release();
    }

    if (allocator_) {
        vmaDestroyAllocator(allocator_);
    }
}

void Engine::create_instance() {
    vk::ApplicationInfo app_info{
        "vulkan-fragment-renderer", VK_MAKE_VERSION(0, 1, 0),
        "vfr", VK_MAKE_VERSION(0, 1, 0),
        VK_API_VERSION_1_3
    };

    auto sdl_exts = window_.required_extensions();

    // Add portability extension on macOS (MoltenVK)
#ifdef __APPLE__
    sdl_exts.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
#endif

    vk::InstanceCreateInfo create_info{};
    create_info.pApplicationInfo = &app_info;
    create_info.enabledExtensionCount = static_cast<uint32_t>(sdl_exts.size());
    create_info.ppEnabledExtensionNames = sdl_exts.data();

#ifdef __APPLE__
    create_info.flags |= vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR;
#endif

    // Enable validation in debug builds
#ifndef NDEBUG
    static const char* validation_layer = "VK_LAYER_KHRONOS_validation";
    auto available = context_.enumerateInstanceLayerProperties();
    bool has_validation = std::ranges::any_of(available, [](const auto& p) {
        return std::string_view(p.layerName) == validation_layer;
    });
    if (has_validation) {
        create_info.enabledLayerCount = 1;
        create_info.ppEnabledLayerNames = &validation_layer;
        LOG_INFO("validation layers enabled");
    }
#endif

    instance_ = vk::raii::Instance(context_, create_info);
}

void Engine::create_device() {
    auto physical_devices = instance_.enumeratePhysicalDevices();
    if (physical_devices.empty()) {
        throw std::runtime_error("no Vulkan-capable GPU found");
    }

    // Pick first discrete GPU, or fall back to first device
    size_t chosen = 0;
    for (size_t i = 0; i < physical_devices.size(); ++i) {
        if (physical_devices[i].getProperties().deviceType == vk::PhysicalDeviceType::eDiscreteGpu) {
            chosen = i;
            break;
        }
    }
    physical_device_ = std::move(physical_devices[chosen]);

    LOG_INFO("using GPU: {}", std::string(physical_device_.getProperties().deviceName));

    // Find graphics queue family
    auto families = physical_device_.getQueueFamilyProperties();
    for (uint32_t i = 0; i < families.size(); ++i) {
        if ((families[i].queueFlags & vk::QueueFlagBits::eGraphics) &&
            physical_device_.getSurfaceSupportKHR(i, *surface_)) {
            graphics_family_ = i;
            break;
        }
    }

    float priority = 1.0f;
    vk::DeviceQueueCreateInfo queue_info{{}, graphics_family_, 1, &priority};

    std::vector<const char*> device_extensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
#ifdef __APPLE__
        "VK_KHR_portability_subset",
#endif
    };

    vk::PhysicalDeviceFeatures features{};

    vk::DeviceCreateInfo device_info{};
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = static_cast<uint32_t>(device_extensions.size());
    device_info.ppEnabledExtensionNames = device_extensions.data();
    device_info.pEnabledFeatures = &features;

    device_ = vk::raii::Device(physical_device_, device_info);
    graphics_queue_ = device_.getQueue(graphics_family_, 0);
}

void Engine::create_allocator() {
    VmaVulkanFunctions vk_fns{};
    vk_fns.vkGetInstanceProcAddr = context_.getDispatcher()->vkGetInstanceProcAddr;
    vk_fns.vkGetDeviceProcAddr = instance_.getDispatcher()->vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo info{};
    info.vulkanApiVersion = VK_API_VERSION_1_3;
    info.instance = *instance_;
    info.physicalDevice = *physical_device_;
    info.device = *device_;
    info.pVulkanFunctions = &vk_fns;

    if (vmaCreateAllocator(&info, &allocator_) != VK_SUCCESS) {
        throw std::runtime_error("failed to create VMA allocator");
    }
}

void Engine::create_swapchain() {
    auto caps = physical_device_.getSurfaceCapabilitiesKHR(*surface_);
    auto formats = physical_device_.getSurfaceFormatsKHR(*surface_);
    auto modes = physical_device_.getSurfacePresentModesKHR(*surface_);

    // Prefer SRGB
    swapchain_format_ = formats[0].format;
    vk::ColorSpaceKHR color_space = formats[0].colorSpace;
    for (auto& f : formats) {
        if (f.format == vk::Format::eB8G8R8A8Srgb &&
            f.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear) {
            swapchain_format_ = f.format;
            color_space = f.colorSpace;
            break;
        }
    }

    // Extent
    if (caps.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
        swapchain_extent_ = caps.currentExtent;
    } else {
        swapchain_extent_.width = std::clamp(
            static_cast<uint32_t>(window_.drawable_width()),
            caps.minImageExtent.width, caps.maxImageExtent.width);
        swapchain_extent_.height = std::clamp(
            static_cast<uint32_t>(window_.drawable_height()),
            caps.minImageExtent.height, caps.maxImageExtent.height);
    }

    uint32_t image_count = std::min(caps.minImageCount + 1,
        caps.maxImageCount > 0 ? caps.maxImageCount : caps.minImageCount + 1);

    // Prefer mailbox, fall back to FIFO
    vk::PresentModeKHR present_mode = vk::PresentModeKHR::eFifo;
    for (auto m : modes) {
        if (m == vk::PresentModeKHR::eMailbox) { present_mode = m; break; }
    }

    vk::SwapchainCreateInfoKHR create_info{};
    create_info.surface = *surface_;
    create_info.minImageCount = image_count;
    create_info.imageFormat = swapchain_format_;
    create_info.imageColorSpace = color_space;
    create_info.imageExtent = swapchain_extent_;
    create_info.imageArrayLayers = 1;
    create_info.imageUsage = vk::ImageUsageFlagBits::eColorAttachment;
    create_info.imageSharingMode = vk::SharingMode::eExclusive;
    create_info.preTransform = caps.currentTransform;
    create_info.compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
    create_info.presentMode = present_mode;
    create_info.clipped = VK_TRUE;
    create_info.oldSwapchain = *swapchain_;

    vk::raii::SwapchainKHR new_swapchain(device_, create_info);
    swapchain_ = std::move(new_swapchain);
    swapchain_images_ = swapchain_.getImages();

    swapchain_views_.clear();
    for (auto img : swapchain_images_) {
        vk::ImageViewCreateInfo view_info{};
        view_info.image = img;
        view_info.viewType = vk::ImageViewType::e2D;
        view_info.format = swapchain_format_;
        view_info.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
        swapchain_views_.emplace_back(device_, view_info);
    }

    // Depth buffer
    if (depth_allocation_) {
        vmaDestroyImage(allocator_, static_cast<VkImage>(*depth_image_), depth_allocation_);
        depth_image_.release();
        depth_view_ = vk::raii::ImageView{nullptr};
    }

    vk::ImageCreateInfo depth_info{};
    depth_info.imageType = vk::ImageType::e2D;
    depth_info.format = vk::Format::eD32Sfloat;
    depth_info.extent = vk::Extent3D{swapchain_extent_.width, swapchain_extent_.height, 1};
    depth_info.mipLevels = 1;
    depth_info.arrayLayers = 1;
    depth_info.samples = vk::SampleCountFlagBits::e1;
    depth_info.tiling = vk::ImageTiling::eOptimal;
    depth_info.usage = vk::ImageUsageFlagBits::eDepthStencilAttachment;

    VmaAllocationCreateInfo alloc_info{};
    alloc_info.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    VkImage raw_depth;
    auto ci = static_cast<VkImageCreateInfo>(depth_info);
    vmaCreateImage(allocator_, &ci, &alloc_info, &raw_depth, &depth_allocation_, nullptr);
    depth_image_ = vk::raii::Image(device_, raw_depth);

    vk::ImageViewCreateInfo dv_info{};
    dv_info.image = raw_depth;
    dv_info.viewType = vk::ImageViewType::e2D;
    dv_info.format = vk::Format::eD32Sfloat;
    dv_info.subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1};
    depth_view_ = vk::raii::ImageView(device_, dv_info);

    LOG_INFO("swapchain created: {}x{}, {} images",
             swapchain_extent_.width, swapchain_extent_.height, swapchain_images_.size());
}

void Engine::create_render_pass() {
    vk::AttachmentDescription color_attachment{};
    color_attachment.format = swapchain_format_;
    color_attachment.samples = vk::SampleCountFlagBits::e1;
    color_attachment.loadOp = vk::AttachmentLoadOp::eClear;
    color_attachment.storeOp = vk::AttachmentStoreOp::eStore;
    color_attachment.stencilLoadOp = vk::AttachmentLoadOp::eDontCare;
    color_attachment.stencilStoreOp = vk::AttachmentStoreOp::eDontCare;
    color_attachment.initialLayout = vk::ImageLayout::eUndefined;
    color_attachment.finalLayout = vk::ImageLayout::ePresentSrcKHR;

    vk::AttachmentDescription depth_attachment{};
    depth_attachment.format = vk::Format::eD32Sfloat;
    depth_attachment.samples = vk::SampleCountFlagBits::e1;
    depth_attachment.loadOp = vk::AttachmentLoadOp::eClear;
    depth_attachment.storeOp = vk::AttachmentStoreOp::eDontCare;
    depth_attachment.stencilLoadOp = vk::AttachmentLoadOp::eDontCare;
    depth_attachment.stencilStoreOp = vk::AttachmentStoreOp::eDontCare;
    depth_attachment.initialLayout = vk::ImageLayout::eUndefined;
    depth_attachment.finalLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;

    vk::AttachmentReference color_ref{0, vk::ImageLayout::eColorAttachmentOptimal};
    vk::AttachmentReference depth_ref{1, vk::ImageLayout::eDepthStencilAttachmentOptimal};

    vk::SubpassDescription subpass{};
    subpass.pipelineBindPoint = vk::PipelineBindPoint::eGraphics;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;
    subpass.pDepthStencilAttachment = &depth_ref;

    vk::SubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput |
                              vk::PipelineStageFlagBits::eEarlyFragmentTests;
    dependency.dstStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput |
                              vk::PipelineStageFlagBits::eEarlyFragmentTests;
    dependency.dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite |
                               vk::AccessFlagBits::eDepthStencilAttachmentWrite;

    std::array attachments = {color_attachment, depth_attachment};

    vk::RenderPassCreateInfo rp_info{};
    rp_info.attachmentCount = static_cast<uint32_t>(attachments.size());
    rp_info.pAttachments = attachments.data();
    rp_info.subpassCount = 1;
    rp_info.pSubpasses = &subpass;
    rp_info.dependencyCount = 1;
    rp_info.pDependencies = &dependency;

    render_pass_ = vk::raii::RenderPass(device_, rp_info);
}

void Engine::create_framebuffers() {
    framebuffers_.clear();
    for (auto& view : swapchain_views_) {
        std::array<vk::ImageView, 2> attachments = {*view, *depth_view_};

        vk::FramebufferCreateInfo fb_info{};
        fb_info.renderPass = *render_pass_;
        fb_info.attachmentCount = static_cast<uint32_t>(attachments.size());
        fb_info.pAttachments = attachments.data();
        fb_info.width = swapchain_extent_.width;
        fb_info.height = swapchain_extent_.height;
        fb_info.layers = 1;

        framebuffers_.emplace_back(device_, fb_info);
    }
}

void Engine::create_command_pool() {
    vk::CommandPoolCreateInfo pool_info{};
    pool_info.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
    pool_info.queueFamilyIndex = graphics_family_;
    command_pool_ = vk::raii::CommandPool(device_, pool_info);
}

void Engine::create_sync_objects() {
    vk::CommandBufferAllocateInfo alloc_info{};
    alloc_info.commandPool = *command_pool_;
    alloc_info.level = vk::CommandBufferLevel::ePrimary;
    alloc_info.commandBufferCount = MAX_FRAMES_IN_FLIGHT;

    auto cmd_buffers = device_.allocateCommandBuffers(alloc_info);

    frames_.clear();
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        frames_.push_back({
            vk::raii::Fence(device_, vk::FenceCreateInfo{vk::FenceCreateFlagBits::eSignaled}),
            std::move(cmd_buffers[i]),
        });
    }

    // Semaphore ring — one per swapchain image to avoid reuse conflicts
    acquire_semaphores_.clear();
    render_semaphores_.clear();
    for (size_t i = 0; i < swapchain_images_.size(); ++i) {
        acquire_semaphores_.emplace_back(device_, vk::SemaphoreCreateInfo{});
        render_semaphores_.emplace_back(device_, vk::SemaphoreCreateInfo{});
    }
    semaphore_index_ = 0;
}

vk::CommandBuffer Engine::begin_frame() {
    auto& frame = frames_[frame_index_];

    auto wait_result = device_.waitForFences({*frame.in_flight}, VK_TRUE, UINT64_MAX);
    (void)wait_result;

    // Flush old deferred destroys
    std::erase_if(deferred_, [this](auto& d) {
        if (d.deadline <= frame_counter_) { d.destroy(); return true; }
        return false;
    });

    current_acquire_sem_ = *acquire_semaphores_[semaphore_index_];
    current_render_sem_ = *render_semaphores_[semaphore_index_];
    semaphore_index_ = (semaphore_index_ + 1) % static_cast<uint32_t>(acquire_semaphores_.size());

    auto [result, idx] = swapchain_.acquireNextImage(UINT64_MAX, current_acquire_sem_);
    if (result == vk::Result::eErrorOutOfDateKHR) {
        recreate_swapchain();
        return nullptr;
    }
    image_index_ = idx;

    device_.resetFences({*frame.in_flight});

    frame.command_buffer.reset();
    frame.command_buffer.begin(vk::CommandBufferBeginInfo{});

    std::array<vk::ClearValue, 2> clear_values;
    clear_values[0].color = vk::ClearColorValue{std::array{0.0f, 0.0f, 0.0f, 1.0f}};
    clear_values[1].depthStencil = vk::ClearDepthStencilValue{1.0f, 0};

    vk::RenderPassBeginInfo rp_begin{};
    rp_begin.renderPass = *render_pass_;
    rp_begin.framebuffer = *framebuffers_[image_index_];
    rp_begin.renderArea = vk::Rect2D{{0, 0}, swapchain_extent_};
    rp_begin.clearValueCount = static_cast<uint32_t>(clear_values.size());
    rp_begin.pClearValues = clear_values.data();

    frame.command_buffer.beginRenderPass(rp_begin, vk::SubpassContents::eInline);

    // Set dynamic viewport/scissor
    vk::Viewport viewport{0, 0,
        static_cast<float>(swapchain_extent_.width),
        static_cast<float>(swapchain_extent_.height),
        0.0f, 1.0f};
    frame.command_buffer.setViewport(0, viewport);

    vk::Rect2D scissor{{0, 0}, swapchain_extent_};
    frame.command_buffer.setScissor(0, scissor);

    return *frame.command_buffer;
}

void Engine::end_frame() {
    auto& frame = frames_[frame_index_];

    frame.command_buffer.endRenderPass();
    frame.command_buffer.end();

    vk::PipelineStageFlags wait_stage = vk::PipelineStageFlagBits::eColorAttachmentOutput;
    vk::SubmitInfo submit{};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &current_acquire_sem_;
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &*frame.command_buffer;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &current_render_sem_;

    graphics_queue_.submit(submit, *frame.in_flight);

    vk::PresentInfoKHR present{};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &current_render_sem_;
    present.swapchainCount = 1;
    present.pSwapchains = &*swapchain_;
    present.pImageIndices = &image_index_;

    auto result = graphics_queue_.presentKHR(present);
    if (result == vk::Result::eErrorOutOfDateKHR || result == vk::Result::eSuboptimalKHR) {
        recreate_swapchain();
    }

    frame_index_ = (frame_index_ + 1) % MAX_FRAMES_IN_FLIGHT;
    ++frame_counter_;
}

void Engine::recreate_swapchain() {
    device_.waitIdle();
    framebuffers_.clear();
    swapchain_views_.clear();
    create_swapchain();
    create_framebuffers();

    // Recreate semaphore ring for new swapchain image count
    acquire_semaphores_.clear();
    render_semaphores_.clear();
    for (size_t i = 0; i < swapchain_images_.size(); ++i) {
        acquire_semaphores_.emplace_back(device_, vk::SemaphoreCreateInfo{});
        render_semaphores_.emplace_back(device_, vk::SemaphoreCreateInfo{});
    }
    semaphore_index_ = 0;
    LOG_INFO("swapchain recreated");
}

void Engine::submit_immediate(std::function<void(vk::CommandBuffer)> fn) {
    vk::CommandBufferAllocateInfo alloc_info{};
    alloc_info.commandPool = *command_pool_;
    alloc_info.level = vk::CommandBufferLevel::ePrimary;
    alloc_info.commandBufferCount = 1;

    auto buffers = device_.allocateCommandBuffers(alloc_info);
    auto& cmd = buffers[0];

    cmd.begin(vk::CommandBufferBeginInfo{vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
    fn(*cmd);
    cmd.end();

    vk::SubmitInfo submit{};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &*cmd;
    graphics_queue_.submit(submit);
    graphics_queue_.waitIdle();
}

void Engine::flush_deferred() {
    for (auto& d : deferred_) d.destroy();
    deferred_.clear();
}

} // namespace vfr
