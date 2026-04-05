#define LOG_MODULE_NAME "xr"

#include "xr_session.hpp"
#include "log.hpp"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <stdexcept>
#include <cstring>
#include <cmath>

static auto logger = spdlog::stdout_color_mt(LOG_MODULE_NAME);

namespace vfr {

// Static state for two-phase initialization
XrInstance XrSession::s_xr_instance_ = XR_NULL_HANDLE;
XrSystemId XrSession::s_xr_system_id_ = XR_NULL_SYSTEM_ID;

// Helper to split space-delimited extension string (v1 API)
static std::vector<std::string> split_extensions(const std::string& str) {
    std::vector<std::string> result;
    size_t start = 0;
    while (start < str.size()) {
        size_t end = str.find(' ', start);
        if (end == std::string::npos) end = str.size();
        if (end > start) result.emplace_back(str.substr(start, end - start));
        start = end + 1;
    }
    return result;
}

// Helper macro for XR result checking
#define XR_CHECK(call, msg)                                     \
    do {                                                        \
        XrResult _r = (call);                                   \
        if (XR_FAILED(_r)) {                                    \
            LOG_ERROR("{}: XrResult {}", msg, static_cast<int>(_r)); \
            throw std::runtime_error(msg);                      \
        }                                                       \
    } while (0)

// ──────────────────────────────────────────────────────────────
// Phase 1: Static methods (before Vulkan init)
// ──────────────────────────────────────────────────────────────

std::optional<XrVulkanRequirements> XrSession::query_requirements() {
    // Create XR instance
    XrInstanceCreateInfo instance_ci{};
    instance_ci.type = XR_TYPE_INSTANCE_CREATE_INFO;
    strncpy(instance_ci.applicationInfo.applicationName,
            "vulkan-fragment-renderer", XR_MAX_APPLICATION_NAME_SIZE);
    instance_ci.applicationInfo.applicationVersion = 1;
    strncpy(instance_ci.applicationInfo.engineName, "vfr", XR_MAX_ENGINE_NAME_SIZE);
    instance_ci.applicationInfo.engineVersion = 1;
    instance_ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);

    const char* extensions[] = {"XR_KHR_vulkan_enable"};
    instance_ci.enabledExtensionCount = 1;
    instance_ci.enabledExtensionNames = extensions;

    XrResult result = xrCreateInstance(&instance_ci, &s_xr_instance_);
    if (XR_FAILED(result)) {
        LOG_INFO("no OpenXR runtime available (result: {}), proceeding desktop-only",
                 static_cast<int>(result));
        s_xr_instance_ = XR_NULL_HANDLE;
        return std::nullopt;
    }

    // Log runtime info
    XrInstanceProperties props{};
    props.type = XR_TYPE_INSTANCE_PROPERTIES;
    xrGetInstanceProperties(s_xr_instance_, &props);
    LOG_INFO("OpenXR runtime: {} v{}.{}.{}", props.runtimeName,
             XR_VERSION_MAJOR(props.runtimeVersion),
             XR_VERSION_MINOR(props.runtimeVersion),
             XR_VERSION_PATCH(props.runtimeVersion));

    // Get HMD system
    XrSystemGetInfo system_info{};
    system_info.type = XR_TYPE_SYSTEM_GET_INFO;
    system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

    result = xrGetSystem(s_xr_instance_, &system_info, &s_xr_system_id_);
    if (XR_FAILED(result)) {
        LOG_INFO("no HMD detected (result: {}), proceeding desktop-only",
                 static_cast<int>(result));
        xrDestroyInstance(s_xr_instance_);
        s_xr_instance_ = XR_NULL_HANDLE;
        return std::nullopt;
    }

    XrSystemProperties sys_props{};
    sys_props.type = XR_TYPE_SYSTEM_PROPERTIES;
    xrGetSystemProperties(s_xr_instance_, s_xr_system_id_, &sys_props);
    LOG_INFO("HMD: {}", sys_props.systemName);

    // Get Vulkan graphics requirements
    PFN_xrGetVulkanGraphicsRequirementsKHR getReqs = nullptr;
    xrGetInstanceProcAddr(s_xr_instance_, "xrGetVulkanGraphicsRequirementsKHR",
                          reinterpret_cast<PFN_xrVoidFunction*>(&getReqs));

    XrGraphicsRequirementsVulkanKHR gfx_reqs{};
    gfx_reqs.type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR;
    getReqs(s_xr_instance_, s_xr_system_id_, &gfx_reqs);

    LOG_INFO("XR Vulkan requirements: min {}.{}.{}, max {}.{}.{}",
             XR_VERSION_MAJOR(gfx_reqs.minApiVersionSupported),
             XR_VERSION_MINOR(gfx_reqs.minApiVersionSupported),
             XR_VERSION_PATCH(gfx_reqs.minApiVersionSupported),
             XR_VERSION_MAJOR(gfx_reqs.maxApiVersionSupported),
             XR_VERSION_MINOR(gfx_reqs.maxApiVersionSupported),
             XR_VERSION_PATCH(gfx_reqs.maxApiVersionSupported));

    // Get required Vulkan instance extensions
    PFN_xrGetVulkanInstanceExtensionsKHR getInstExts = nullptr;
    xrGetInstanceProcAddr(s_xr_instance_, "xrGetVulkanInstanceExtensionsKHR",
                          reinterpret_cast<PFN_xrVoidFunction*>(&getInstExts));

    uint32_t inst_ext_size = 0;
    getInstExts(s_xr_instance_, s_xr_system_id_, 0, &inst_ext_size, nullptr);
    std::string inst_ext_str(inst_ext_size, '\0');
    getInstExts(s_xr_instance_, s_xr_system_id_, inst_ext_size, &inst_ext_size,
                inst_ext_str.data());

    // Get required Vulkan device extensions
    PFN_xrGetVulkanDeviceExtensionsKHR getDevExts = nullptr;
    xrGetInstanceProcAddr(s_xr_instance_, "xrGetVulkanDeviceExtensionsKHR",
                          reinterpret_cast<PFN_xrVoidFunction*>(&getDevExts));

    uint32_t dev_ext_size = 0;
    getDevExts(s_xr_instance_, s_xr_system_id_, 0, &dev_ext_size, nullptr);
    std::string dev_ext_str(dev_ext_size, '\0');
    getDevExts(s_xr_instance_, s_xr_system_id_, dev_ext_size, &dev_ext_size,
               dev_ext_str.data());

    XrVulkanRequirements reqs;
    reqs.instance_extensions = split_extensions(inst_ext_str);
    reqs.device_extensions = split_extensions(dev_ext_str);

    for (auto& ext : reqs.instance_extensions)
        LOG_INFO("  XR requires VK instance ext: {}", ext);
    for (auto& ext : reqs.device_extensions)
        LOG_INFO("  XR requires VK device ext: {}", ext);

    return reqs;
}

VkPhysicalDevice XrSession::query_physical_device(VkInstance vk_instance) {
    if (s_xr_instance_ == XR_NULL_HANDLE) return VK_NULL_HANDLE;

    PFN_xrGetVulkanGraphicsDeviceKHR getDevice = nullptr;
    xrGetInstanceProcAddr(s_xr_instance_, "xrGetVulkanGraphicsDeviceKHR",
                          reinterpret_cast<PFN_xrVoidFunction*>(&getDevice));

    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    getDevice(s_xr_instance_, s_xr_system_id_, vk_instance, &physical_device);
    return physical_device;
}

bool XrSession::has_pending_session() {
    return s_xr_instance_ != XR_NULL_HANDLE;
}

// ──────────────────────────────────────────────────────────────
// Phase 2: Constructor (after Vulkan init)
// ──────��─────────────────────────���─────────────────────────────

XrSession::XrSession(VkInstance vk_instance, VkPhysicalDevice physical_device,
                     VkDevice device, uint32_t queue_family, uint32_t queue_index,
                     VmaAllocator allocator, vk::Format color_format, vk::Format depth_format,
                     const vk::raii::Device& raii_device)
    : xr_instance_(s_xr_instance_)
    , xr_system_id_(s_xr_system_id_)
    , vk_device_(device)
    , allocator_(allocator)
    , raii_device_(&raii_device)
    , color_format_(color_format)
    , depth_format_(depth_format)
{
    // Take ownership from static state
    s_xr_instance_ = XR_NULL_HANDLE;
    s_xr_system_id_ = XR_NULL_SYSTEM_ID;

    // Initialize XR views
    for (auto& v : xr_views_) {
        v.type = XR_TYPE_VIEW;
        v.next = nullptr;
    }

    // Create XR session with Vulkan binding
    XrGraphicsBindingVulkanKHR binding{};
    binding.type = XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR;
    binding.instance = vk_instance;
    binding.physicalDevice = physical_device;
    binding.device = device;
    binding.queueFamilyIndex = queue_family;
    binding.queueIndex = queue_index;

    XrSessionCreateInfo session_ci{};
    session_ci.type = XR_TYPE_SESSION_CREATE_INFO;
    session_ci.next = &binding;
    session_ci.systemId = xr_system_id_;

    XR_CHECK(xrCreateSession(xr_instance_, &session_ci, &xr_session_),
             "failed to create XR session");
    LOG_INFO("XR session created");

    // Create LOCAL reference space (seated, no room boundaries)
    XrReferenceSpaceCreateInfo space_ci{};
    space_ci.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
    space_ci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    space_ci.poseInReferenceSpace.orientation.w = 1.0f;

    XR_CHECK(xrCreateReferenceSpace(xr_session_, &space_ci, &xr_space_),
             "failed to create XR reference space");

    create_render_pass();
    create_swapchains();

    LOG_INFO("XR initialized: {}x{} per eye", eye_extent_.width, eye_extent_.height);
}

XrSession::~XrSession() {
    // Destroy eye resources
    for (auto& eye : eyes_) {
        eye.framebuffers.clear();
        eye.color_views.clear();
        eye.depth_view = vk::raii::ImageView{nullptr};
        if (eye.depth_alloc) {
            vmaDestroyImage(allocator_, eye.depth_image, eye.depth_alloc);
        }
        if (eye.handle != XR_NULL_HANDLE) {
            xrDestroySwapchain(eye.handle);
        }
    }

    render_pass_ = vk::raii::RenderPass{nullptr};

    if (xr_space_ != XR_NULL_HANDLE) xrDestroySpace(xr_space_);
    if (xr_session_ != XR_NULL_HANDLE) xrDestroySession(xr_session_);
    if (xr_instance_ != XR_NULL_HANDLE) xrDestroyInstance(xr_instance_);
}

// ──────────────────────────────────────────────────────────────
// Render pass & swapchains
// ──���──────────────��────────────────────────────────────��───────

void XrSession::create_render_pass() {
    // XR render pass: final layout is COLOR_ATTACHMENT_OPTIMAL (runtime consumes directly)
    vk::AttachmentDescription color_attachment{};
    color_attachment.format = color_format_;
    color_attachment.samples = vk::SampleCountFlagBits::e1;
    color_attachment.loadOp = vk::AttachmentLoadOp::eClear;
    color_attachment.storeOp = vk::AttachmentStoreOp::eStore;
    color_attachment.stencilLoadOp = vk::AttachmentLoadOp::eDontCare;
    color_attachment.stencilStoreOp = vk::AttachmentStoreOp::eDontCare;
    color_attachment.initialLayout = vk::ImageLayout::eUndefined;
    color_attachment.finalLayout = vk::ImageLayout::eColorAttachmentOptimal;

    vk::AttachmentDescription depth_attachment{};
    depth_attachment.format = depth_format_;
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

    render_pass_ = vk::raii::RenderPass(*raii_device_, rp_info);
}

void XrSession::create_swapchains() {
    // Query view configuration
    uint32_t view_count = 0;
    xrEnumerateViewConfigurationViews(xr_instance_, xr_system_id_,
                                       XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                       0, &view_count, nullptr);
    if (view_count != 2) {
        throw std::runtime_error("expected 2 stereo views, got " + std::to_string(view_count));
    }

    std::array<XrViewConfigurationView, 2> config_views;
    for (auto& cv : config_views) {
        cv.type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
        cv.next = nullptr;
    }
    xrEnumerateViewConfigurationViews(xr_instance_, xr_system_id_,
                                       XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                       2, &view_count, config_views.data());

    eye_extent_ = vk::Extent2D{
        config_views[0].recommendedImageRectWidth,
        config_views[0].recommendedImageRectHeight};

    LOG_INFO("recommended eye resolution: {}x{}", eye_extent_.width, eye_extent_.height);

    for (uint32_t eye = 0; eye < 2; ++eye) {
        // Create color swapchain
        XrSwapchainCreateInfo swapchain_ci{};
        swapchain_ci.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
        swapchain_ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        swapchain_ci.format = static_cast<int64_t>(color_format_);
        swapchain_ci.sampleCount = 1;
        swapchain_ci.width = eye_extent_.width;
        swapchain_ci.height = eye_extent_.height;
        swapchain_ci.faceCount = 1;
        swapchain_ci.arraySize = 1;
        swapchain_ci.mipCount = 1;

        XR_CHECK(xrCreateSwapchain(xr_session_, &swapchain_ci, &eyes_[eye].handle),
                 "failed to create XR swapchain");

        // Enumerate swapchain images
        uint32_t image_count = 0;
        xrEnumerateSwapchainImages(eyes_[eye].handle, 0, &image_count, nullptr);

        eyes_[eye].images.resize(image_count);
        for (auto& img : eyes_[eye].images) {
            img.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
            img.next = nullptr;
        }
        xrEnumerateSwapchainImages(eyes_[eye].handle, image_count, &image_count,
                                   reinterpret_cast<XrSwapchainImageBaseHeader*>(
                                       eyes_[eye].images.data()));

        LOG_INFO("eye {} swapchain: {} images", eye, image_count);

        // Create image views for each swapchain image
        for (uint32_t i = 0; i < image_count; ++i) {
            vk::ImageViewCreateInfo view_ci{};
            view_ci.image = eyes_[eye].images[i].image;
            view_ci.viewType = vk::ImageViewType::e2D;
            view_ci.format = color_format_;
            view_ci.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
            eyes_[eye].color_views.emplace_back(*raii_device_, view_ci);
        }

        // Create depth buffer for this eye (one shared across swapchain images)
        VkImageCreateInfo depth_ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        depth_ci.imageType = VK_IMAGE_TYPE_2D;
        depth_ci.format = static_cast<VkFormat>(depth_format_);
        depth_ci.extent = {eye_extent_.width, eye_extent_.height, 1};
        depth_ci.mipLevels = 1;
        depth_ci.arrayLayers = 1;
        depth_ci.samples = VK_SAMPLE_COUNT_1_BIT;
        depth_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        depth_ci.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

        VmaAllocationCreateInfo alloc_ci{};
        alloc_ci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

        vmaCreateImage(allocator_, &depth_ci, &alloc_ci,
                       &eyes_[eye].depth_image, &eyes_[eye].depth_alloc, nullptr);

        vk::ImageViewCreateInfo dv_ci{};
        dv_ci.image = eyes_[eye].depth_image;
        dv_ci.viewType = vk::ImageViewType::e2D;
        dv_ci.format = depth_format_;
        dv_ci.subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1};
        eyes_[eye].depth_view = vk::raii::ImageView(*raii_device_, dv_ci);

        // Create framebuffers (one per swapchain image)
        for (uint32_t i = 0; i < image_count; ++i) {
            std::array<vk::ImageView, 2> attachments = {
                *eyes_[eye].color_views[i], *eyes_[eye].depth_view};

            vk::FramebufferCreateInfo fb_ci{};
            fb_ci.renderPass = *render_pass_;
            fb_ci.attachmentCount = static_cast<uint32_t>(attachments.size());
            fb_ci.pAttachments = attachments.data();
            fb_ci.width = eye_extent_.width;
            fb_ci.height = eye_extent_.height;
            fb_ci.layers = 1;

            eyes_[eye].framebuffers.emplace_back(*raii_device_, fb_ci);
        }
    }
}

// ───────────────────────────��──────────────────────────────────
// Session state machine
// ─���────────────────────────���───────────────────────────────────

void XrSession::poll_events() {
    XrEventDataBuffer event{};
    event.type = XR_TYPE_EVENT_DATA_BUFFER;

    while (xrPollEvent(xr_instance_, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* state_event = reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
            session_state_ = state_event->state;

            LOG_INFO("XR session state: {}", static_cast<int>(session_state_));

            if (session_state_ == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo begin_info{};
                begin_info.type = XR_TYPE_SESSION_BEGIN_INFO;
                begin_info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                XR_CHECK(xrBeginSession(xr_session_, &begin_info),
                         "failed to begin XR session");
                session_running_ = true;
                LOG_INFO("XR session started");
            } else if (session_state_ == XR_SESSION_STATE_STOPPING) {
                xrEndSession(xr_session_);
                session_running_ = false;
                LOG_INFO("XR session stopped");
            }
        }

        event.type = XR_TYPE_EVENT_DATA_BUFFER;
    }
}

// ───────────────────────────────��──────────────────────────────
// Frame loop
// ───────���───────────────��──────────────────────────────────────

bool XrSession::wait_and_begin_frame() {
    XrFrameWaitInfo wait_info{};
    wait_info.type = XR_TYPE_FRAME_WAIT_INFO;
    frame_state_ = {};
    frame_state_.type = XR_TYPE_FRAME_STATE;

    XR_CHECK(xrWaitFrame(xr_session_, &wait_info, &frame_state_),
             "xrWaitFrame failed");

    XrFrameBeginInfo begin_info{};
    begin_info.type = XR_TYPE_FRAME_BEGIN_INFO;
    XR_CHECK(xrBeginFrame(xr_session_, &begin_info),
             "xrBeginFrame failed");

    return frame_state_.shouldRender == XR_TRUE;
}

std::array<XrEyeRenderInfo, 2> XrSession::locate_views(const glm::mat4& camera_world) {
    XrViewLocateInfo locate_info{};
    locate_info.type = XR_TYPE_VIEW_LOCATE_INFO;
    locate_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate_info.displayTime = frame_state_.predictedDisplayTime;
    locate_info.space = xr_space_;

    XrViewState view_state{};
    view_state.type = XR_TYPE_VIEW_STATE;

    uint32_t view_count = 2;
    XR_CHECK(xrLocateViews(xr_session_, &locate_info, &view_state,
                           2, &view_count, xr_views_.data()),
             "xrLocateViews failed");

    std::array<XrEyeRenderInfo, 2> result;
    for (uint32_t eye = 0; eye < 2; ++eye) {
        result[eye].pose = xr_views_[eye].pose;
        result[eye].fov = xr_views_[eye].fov;
        result[eye].view = xr_pose_to_view_matrix(xr_views_[eye].pose, camera_world);
        result[eye].projection = xr_fov_to_projection(xr_views_[eye].fov, 0.01f, 1000.0f);
    }
    return result;
}

void XrSession::begin_eye_render(vk::CommandBuffer cmd, uint32_t eye) {
    auto& sc = eyes_[eye];

    // Acquire swapchain image
    XrSwapchainImageAcquireInfo acquire_info{};
    acquire_info.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
    XR_CHECK(xrAcquireSwapchainImage(sc.handle, &acquire_info, &sc.current_index),
             "xrAcquireSwapchainImage failed");

    // Wait for runtime to release it
    XrSwapchainImageWaitInfo wait_info{};
    wait_info.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
    wait_info.timeout = XR_INFINITE_DURATION;
    XR_CHECK(xrWaitSwapchainImage(sc.handle, &wait_info),
             "xrWaitSwapchainImage failed");

    // Begin render pass
    std::array<vk::ClearValue, 2> clear_values;
    clear_values[0].color = vk::ClearColorValue{std::array{0.0f, 0.0f, 0.0f, 1.0f}};
    clear_values[1].depthStencil = vk::ClearDepthStencilValue{1.0f, 0};

    vk::RenderPassBeginInfo rp_info{};
    rp_info.renderPass = *render_pass_;
    rp_info.framebuffer = *sc.framebuffers[sc.current_index];
    rp_info.renderArea = vk::Rect2D{{0, 0}, eye_extent_};
    rp_info.clearValueCount = static_cast<uint32_t>(clear_values.size());
    rp_info.pClearValues = clear_values.data();

    cmd.beginRenderPass(rp_info, vk::SubpassContents::eInline);

    vk::Viewport viewport{
        0.0f, 0.0f,
        static_cast<float>(eye_extent_.width),
        static_cast<float>(eye_extent_.height),
        0.0f, 1.0f};
    cmd.setViewport(0, viewport);

    vk::Rect2D scissor{{0, 0}, eye_extent_};
    cmd.setScissor(0, scissor);
}

void XrSession::end_eye_render(vk::CommandBuffer cmd, uint32_t eye) {
    end_eye_render_pass(cmd, eye);
    release_eye(eye);
}

void XrSession::end_eye_render_pass(vk::CommandBuffer cmd, uint32_t /*eye*/) {
    cmd.endRenderPass();
}

void XrSession::release_eye(uint32_t eye) {
    XrSwapchainImageReleaseInfo release_info{};
    release_info.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
    XR_CHECK(xrReleaseSwapchainImage(eyes_[eye].handle, &release_info),
             "xrReleaseSwapchainImage failed");
}

vk::Image XrSession::eye_color_image(uint32_t eye) const {
    auto& sc = eyes_[eye];
    return vk::Image(sc.images[sc.current_index].image);
}

void XrSession::end_frame() {
    XrFrameEndInfo end_info{};
    end_info.type = XR_TYPE_FRAME_END_INFO;
    end_info.displayTime = frame_state_.predictedDisplayTime;
    end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

    XrCompositionLayerProjection projection_layer{};
    const XrCompositionLayerBaseHeader* layers_ptr = nullptr;

    if (frame_state_.shouldRender) {
        for (uint32_t eye = 0; eye < 2; ++eye) {
            projection_views_[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
            projection_views_[eye].next = nullptr;
            projection_views_[eye].pose = xr_views_[eye].pose;
            projection_views_[eye].fov = xr_views_[eye].fov;
            projection_views_[eye].subImage.swapchain = eyes_[eye].handle;
            projection_views_[eye].subImage.imageRect.offset = {0, 0};
            projection_views_[eye].subImage.imageRect.extent = {
                static_cast<int32_t>(eye_extent_.width),
                static_cast<int32_t>(eye_extent_.height)};
            projection_views_[eye].subImage.imageArrayIndex = 0;
        }

        projection_layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
        projection_layer.space = xr_space_;
        projection_layer.viewCount = 2;
        projection_layer.views = projection_views_.data();

        layers_ptr = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection_layer);
        end_info.layerCount = 1;
        end_info.layers = &layers_ptr;
    }

    XR_CHECK(xrEndFrame(xr_session_, &end_info), "xrEndFrame failed");
}

// ────────────────────────��─────────────────────────────────────
// Coordinate transforms
// ──────────────────────────────────────────────────────────────

glm::mat4 XrSession::xr_pose_to_view_matrix(const XrPosef& pose,
                                              const glm::mat4& camera_world) {
    // XR quaternion (xyzw) → glm quaternion (wxyz)
    glm::quat q(pose.orientation.w, pose.orientation.x,
                pose.orientation.y, pose.orientation.z);
    glm::vec3 xr_pos(pose.position.x, pose.position.y, pose.position.z);

    // Eye transform in XR local space (LOCAL reference space)
    glm::mat4 eye_local = glm::translate(glm::mat4(1.0f), xr_pos) * glm::mat4_cast(q);

    // XR rig is a child of the camera: camera_world * eye_local
    glm::mat4 eye_world = camera_world * eye_local;

    return glm::inverse(eye_world);
}

glm::mat4 XrSession::xr_fov_to_projection(const XrFovf& fov,
                                            float near_z, float far_z) {
    float left   = near_z * std::tan(fov.angleLeft);
    float right  = near_z * std::tan(fov.angleRight);
    float up     = near_z * std::tan(fov.angleUp);
    float down   = near_z * std::tan(fov.angleDown);

    glm::mat4 proj = glm::frustum(left, right, down, up, near_z, far_z);

    // Vulkan Y-flip: negate both scale AND offset for asymmetric frustum.
    // Forgetting proj[2][1] causes vertical warping during head rotation.
    proj[1][1] *= -1.0f;
    proj[2][1] *= -1.0f;

    return proj;
}

} // namespace vfr
