#include "app.hpp"
#include "shader_compiler.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <glm/gtc/matrix_transform.hpp>
#include <SDL3/SDL.h>
#include <imgui.h>
#include <fstream>
#include <sstream>
#include <cstring>

static auto logger = spdlog::stdout_color_mt("app");

namespace vfr {

static ExternalVulkanRequirements build_xr_requirements(bool xr_enabled) {
    ExternalVulkanRequirements ext_reqs;
    if (!xr_enabled) return ext_reqs;

    auto reqs = XrSession::query_requirements();
    if (!reqs) return ext_reqs;

    ext_reqs.instance_extensions = std::move(reqs->instance_extensions);
    ext_reqs.device_extensions = std::move(reqs->device_extensions);
    ext_reqs.physical_device_query = XrSession::query_physical_device;
    return ext_reqs;
}

App::App(const std::filesystem::path& shader_path, bool high_dpi, bool xr_enabled)
    : window_("vulkan-fragment-renderer", 1280, 720, high_dpi)
    , engine_(window_, [&]() -> const ExternalVulkanRequirements* {
        static auto ext_reqs = build_xr_requirements(xr_enabled);
        if (ext_reqs.instance_extensions.empty() && ext_reqs.device_extensions.empty()
            && !ext_reqs.physical_device_query)
            return nullptr;
        return &ext_reqs;
    }())
    , ui_(engine_, window_)
    , shader_path_(shader_path)
{
    // Phase 2: create XR session after Vulkan device exists
    if (XrSession::has_pending_session()) {
        xr_session_ = std::make_unique<XrSession>(
            static_cast<VkInstance>(*engine_.instance()),
            static_cast<VkPhysicalDevice>(*engine_.physical_device()),
            static_cast<VkDevice>(*engine_.device()),
            engine_.graphics_queue_family(), 0,
            engine_.allocator(),
            engine_.swapchain_format(), vk::Format::eD32Sfloat,
            engine_.device());
        xr_mode_ = true;
    }

    create_frame_descriptors();

    vert_spirv_ = ShaderCompiler::load_spirv(
        std::filesystem::path{SHADER_DIR} / "fullscreen.vert.spv");

    reload_shader();

    last_tick_ = SDL_GetPerformanceCounter();
    fps_tick_ = last_tick_;
}

App::~App() {
    engine_.device().waitIdle();

    // Clean up VMA buffers
    auto destroy = [&](MappedBuffer& buf) {
        if (buf.buffer) vmaDestroyBuffer(engine_.allocator(), buf.buffer, buf.allocation);
    };
    for (auto& buf : frame_buffers_) destroy(buf);
    for (auto& buf : ray_buffers_) destroy(buf);
    for (auto& eye_bufs : xr_eye_frame_buffers_) for (auto& buf : eye_bufs) destroy(buf);
    for (auto& eye_bufs : xr_eye_ray_buffers_) for (auto& buf : eye_bufs) destroy(buf);
}

void App::create_frame_descriptors() {
    // Set 0 layout: FrameUbo at binding 0 (vertex + fragment)
    vk::DescriptorSetLayoutBinding frame_binding{};
    frame_binding.binding = 0;
    frame_binding.descriptorType = vk::DescriptorType::eUniformBuffer;
    frame_binding.descriptorCount = 1;
    frame_binding.stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;

    vk::DescriptorSetLayoutCreateInfo frame_layout_info{};
    frame_layout_info.bindingCount = 1;
    frame_layout_info.pBindings = &frame_binding;
    frame_layout_ = vk::raii::DescriptorSetLayout(engine_.device(), frame_layout_info);

    // Set 1 layout: RayUbo at binding 0 (fragment only)
    vk::DescriptorSetLayoutBinding ray_binding{};
    ray_binding.binding = 0;
    ray_binding.descriptorType = vk::DescriptorType::eUniformBuffer;
    ray_binding.descriptorCount = 1;
    ray_binding.stageFlags = vk::ShaderStageFlagBits::eFragment;

    vk::DescriptorSetLayoutCreateInfo ray_layout_info{};
    ray_layout_info.bindingCount = 1;
    ray_layout_info.pBindings = &ray_binding;
    ray_layout_ = vk::raii::DescriptorSetLayout(engine_.device(), ray_layout_info);

    // Descriptor pool: 2 sets per view (frame + ray), per frame-in-flight
    // Desktop: 2 * MAX_FRAMES_IN_FLIGHT
    // XR adds: 2 eyes * 2 * MAX_FRAMES_IN_FLIGHT
    uint32_t total_sets = 2 * MAX_FRAMES_IN_FLIGHT;
    if (xr_mode_) total_sets += 2 * 2 * MAX_FRAMES_IN_FLIGHT;

    vk::DescriptorPoolSize pool_size{vk::DescriptorType::eUniformBuffer, total_sets};
    vk::DescriptorPoolCreateInfo pool_info{};
    pool_info.maxSets = total_sets;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    descriptor_pool_ = vk::raii::DescriptorPool(engine_.device(), pool_info);

    // Helper: allocate N descriptor sets for a given layout
    auto alloc_sets = [&](vk::DescriptorSetLayout layout, uint32_t count) {
        std::vector<vk::DescriptorSetLayout> layouts(count, layout);
        vk::DescriptorSetAllocateInfo ai{};
        ai.descriptorPool = *descriptor_pool_;
        ai.descriptorSetCount = count;
        ai.pSetLayouts = layouts.data();
        return (*engine_.device()).allocateDescriptorSets(ai);
    };

    // Helper: create a persistently-mapped UBO buffer and bind it to a descriptor set
    auto create_ubo = [&](VkDeviceSize size, vk::DescriptorSet set, MappedBuffer& buf) {
        VkBufferCreateInfo buf_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buf_info.size = size;
        buf_info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

        VmaAllocationCreateInfo alloc{};
        alloc.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        alloc.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                      VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;

        VmaAllocationInfo map_info{};
        vmaCreateBuffer(engine_.allocator(), &buf_info, &alloc,
                        &buf.buffer, &buf.allocation, &map_info);
        buf.mapped = map_info.pMappedData;

        vk::DescriptorBufferInfo desc_buf{buf.buffer, 0, size};
        vk::WriteDescriptorSet write{};
        write.dstSet = set;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = vk::DescriptorType::eUniformBuffer;
        write.pBufferInfo = &desc_buf;
        (*engine_.device()).updateDescriptorSets(write, nullptr);
    };

    // Desktop sets
    auto desktop_frame_sets = alloc_sets(*frame_layout_, MAX_FRAMES_IN_FLIGHT);
    auto desktop_ray_sets = alloc_sets(*ray_layout_, MAX_FRAMES_IN_FLIGHT);
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        frame_sets_[i] = desktop_frame_sets[i];
        ray_sets_[i] = desktop_ray_sets[i];
        create_ubo(sizeof(FrameUbo), frame_sets_[i], frame_buffers_[i]);
        create_ubo(sizeof(RayUbo), ray_sets_[i], ray_buffers_[i]);
    }

    // Per-eye sets for XR
    if (xr_mode_) {
        for (uint32_t eye = 0; eye < 2; ++eye) {
            auto eye_frame_sets = alloc_sets(*frame_layout_, MAX_FRAMES_IN_FLIGHT);
            auto eye_ray_sets = alloc_sets(*ray_layout_, MAX_FRAMES_IN_FLIGHT);
            for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
                xr_eye_frame_sets_[eye][i] = eye_frame_sets[i];
                xr_eye_ray_sets_[eye][i] = eye_ray_sets[i];
                create_ubo(sizeof(FrameUbo), xr_eye_frame_sets_[eye][i], xr_eye_frame_buffers_[eye][i]);
                create_ubo(sizeof(RayUbo), xr_eye_ray_sets_[eye][i], xr_eye_ray_buffers_[eye][i]);
            }
        }
    }

    logger->info("frame descriptors created");
}

void App::update_frame_ubo(uint32_t frame_index) {
    auto extent = engine_.swapchain_extent();
    float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);

    FrameUbo ubo{};
    ubo.view = camera_.view_matrix();
    ubo.projection = glm::perspective(glm::radians(60.0f), aspect, 0.01f, 1000.0f);
    ubo.projection[1][1] *= -1.0f; // Vulkan Y-flip
    ubo.resolution = glm::vec4(
        static_cast<float>(extent.width),
        static_cast<float>(extent.height),
        aspect, 0.0f);
    ubo.time = time_;
    std::memcpy(frame_buffers_[frame_index].mapped, &ubo, sizeof(ubo));

    RayUbo ray{};
    ray.inv_view = glm::inverse(ubo.view);
    ray.inv_projection = glm::inverse(ubo.projection);
    std::memcpy(ray_buffers_[frame_index].mapped, &ray, sizeof(ray));
}

void App::run() {
    while (running_) {
        process_events();

        // Delta time
        uint64_t now = SDL_GetPerformanceCounter();
        float dt = static_cast<float>(now - last_tick_) /
                   static_cast<float>(SDL_GetPerformanceFrequency());
        last_tick_ = now;
        time_ += dt;

        // FPS
        ++fps_frames_;
        float fps_elapsed = static_cast<float>(now - fps_tick_) /
                            static_cast<float>(SDL_GetPerformanceFrequency());
        if (fps_elapsed >= 0.5f) {
            fps_ = static_cast<float>(fps_frames_) / fps_elapsed;
            fps_frames_ = 0;
            fps_tick_ = now;
        }

        // Keyboard movement (continuous, once per frame)
        if (mouse_trapped_ || !ui_.wants_input()) {
            const bool* keys = SDL_GetKeyboardState(nullptr);
            glm::vec3 dir{0.0f};

            if (keys[SDL_SCANCODE_W]) dir.y += 1.0f;
            if (keys[SDL_SCANCODE_S]) dir.y -= 1.0f;
            if (keys[SDL_SCANCODE_A]) dir.x -= 1.0f;
            if (keys[SDL_SCANCODE_D]) dir.x += 1.0f;
            if (keys[SDL_SCANCODE_SPACE])  dir.z += 1.0f;
            if (keys[SDL_SCANCODE_LSHIFT]) dir.z -= 1.0f;

            if (glm::length(dir) > 0.0f) {
                dir = glm::normalize(dir);
                if (hmd_active_) {
                    glm::vec3 world_dir = hmd_right_ * dir.x + hmd_forward_ * dir.y + hmd_up_ * dir.z;
                    camera_.set_move_direction_world(world_dir);
                } else {
                    camera_.set_move_direction(dir);
                }
            }
        }

        camera_.update(dt);

        if (xr_mode_) {
            run_xr_frame(dt);
        } else {
            run_desktop_frame(dt);
        }
    }
}

void App::run_desktop_frame(float /*dt*/) {
    auto cmd = engine_.begin_frame();
    if (!cmd) return;

    uint32_t fi = engine_.frame_index();
    update_frame_ubo(fi);

    if (pipeline_) {
        pipeline_->bind(cmd);
        std::array<vk::DescriptorSet, 2> sets = {frame_sets_[fi], ray_sets_[fi]};
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                               pipeline_->layout(), 0, sets, nullptr);
        glm::mat4 model{1.0f};
        cmd.pushConstants(pipeline_->layout(),
            vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
            0, sizeof(model), &model);
        cmd.draw(3, 1, 0, 0);
    }

    ui_.begin_frame();
    ui_.render(camera_, fps_, mouse_trapped_);
    ui_.end_frame(cmd);

    engine_.end_frame();
}

void App::run_xr_frame(float dt) {
    xr_session_->poll_events();

    if (!xr_session_->session_running()) {
        // Session not active (HMD disconnected, runtime stopped, etc.) — desktop only
        hmd_active_ = false;
        run_desktop_frame(dt);
        return;
    }

    bool should_render = xr_session_->wait_and_begin_frame();

    if (!should_render) {
        // HMD idle / not worn — render to desktop instead
        hmd_active_ = false;
        run_desktop_frame(dt);
        xr_session_->end_frame();
        return;
    }

    // --- HMD active: XR stereo rendering with desktop mirror ---

    auto views = xr_session_->locate_views(glm::inverse(camera_.view_matrix()));

    // Store HMD world-space basis for next frame's input processing.
    // Extract from left eye view matrix (both eyes share the same head orientation).
    glm::mat4 hmd_world = glm::inverse(views[0].view);
    hmd_right_   = glm::normalize(glm::vec3(hmd_world[0]));
    hmd_up_      = glm::normalize(glm::vec3(hmd_world[1]));
    hmd_forward_ = -glm::normalize(glm::vec3(hmd_world[2]));  // -Z is forward
    hmd_active_ = true;

    auto cmd = engine_.begin_command_buffer();
    uint32_t fi = engine_.frame_index();

    if (pipeline_) {
        for (uint32_t eye = 0; eye < 2; ++eye) {
            update_frame_ubo_xr(eye, fi, views[eye].view, views[eye].projection,
                                xr_session_->eye_extent());

            xr_session_->begin_eye_render(cmd, eye);

            pipeline_->bind(cmd);
            std::array<vk::DescriptorSet, 2> sets = {
                xr_eye_frame_sets_[eye][fi], xr_eye_ray_sets_[eye][fi]};
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                   pipeline_->layout(), 0,
                                   sets, nullptr);
            glm::mat4 model{1.0f};
            cmd.pushConstants(pipeline_->layout(),
                vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
                0, sizeof(model), &model);
            cmd.draw(3, 1, 0, 0);

            if (eye == 0) {
                // Left eye: end render pass but hold the image for mirror blit
                xr_session_->end_eye_render_pass(cmd, eye);
            } else {
                xr_session_->end_eye_render(cmd, eye);
            }
        }
    } else {
        // No pipeline — still need to acquire/release swapchain images
        for (uint32_t eye = 0; eye < 2; ++eye) {
            xr_session_->begin_eye_render(cmd, eye);
            if (eye == 0) {
                xr_session_->end_eye_render_pass(cmd, eye);
            } else {
                xr_session_->end_eye_render(cmd, eye);
            }
        }
    }

    // Mirror left eye to desktop window + overlay pass for ImGui
    bool mirrored = blit_xr_mirror(cmd);
    xr_session_->release_eye(0);

    if (mirrored) {
        // Overlay pass: composites ImGui on top of the blitted mirror image.
        // The pass transitions the desktop image from COLOR_ATTACHMENT_OPTIMAL
        // to PRESENT_SRC_KHR via its finalLayout.
        engine_.begin_desktop_overlay_pass(cmd);
        ui_.begin_frame();
        ui_.render(camera_, fps_, mouse_trapped_);
        ui_.end_frame(cmd);
        engine_.end_render_pass(cmd);

        engine_.submit_and_present();
    } else {
        engine_.submit_xr_only();
    }

    xr_session_->end_frame();
}

bool App::blit_xr_mirror(vk::CommandBuffer cmd) {
    if (!engine_.acquire_desktop_image()) return false;

    vk::Image src = xr_session_->eye_color_image(0);
    vk::Image dst = engine_.desktop_image();
    vk::Extent2D src_extent = xr_session_->eye_extent();
    vk::Extent2D dst_extent = engine_.swapchain_extent();

    // Transition src (XR left eye): COLOR_ATTACHMENT_OPTIMAL → TRANSFER_SRC_OPTIMAL
    vk::ImageMemoryBarrier src_to_transfer{};
    src_to_transfer.oldLayout = vk::ImageLayout::eColorAttachmentOptimal;
    src_to_transfer.newLayout = vk::ImageLayout::eTransferSrcOptimal;
    src_to_transfer.image = src;
    src_to_transfer.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    src_to_transfer.srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
    src_to_transfer.dstAccessMask = vk::AccessFlagBits::eTransferRead;

    // Transition dst (desktop): UNDEFINED → TRANSFER_DST_OPTIMAL
    vk::ImageMemoryBarrier dst_to_transfer{};
    dst_to_transfer.oldLayout = vk::ImageLayout::eUndefined;
    dst_to_transfer.newLayout = vk::ImageLayout::eTransferDstOptimal;
    dst_to_transfer.image = dst;
    dst_to_transfer.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    dst_to_transfer.srcAccessMask = {};
    dst_to_transfer.dstAccessMask = vk::AccessFlagBits::eTransferWrite;

    std::array pre_barriers = {src_to_transfer, dst_to_transfer};
    cmd.pipelineBarrier(
        vk::PipelineStageFlagBits::eColorAttachmentOutput,
        vk::PipelineStageFlagBits::eTransfer,
        {}, nullptr, nullptr, pre_barriers);

    // Blit with aspect-ratio-preserving fill (crop source to match destination aspect)
    float src_aspect = static_cast<float>(src_extent.width) / static_cast<float>(src_extent.height);
    float dst_aspect = static_cast<float>(dst_extent.width) / static_cast<float>(dst_extent.height);

    int32_t sx0 = 0, sy0 = 0;
    int32_t sx1 = static_cast<int32_t>(src_extent.width);
    int32_t sy1 = static_cast<int32_t>(src_extent.height);

    if (src_aspect > dst_aspect) {
        // Source is wider — crop sides
        int32_t crop_w = static_cast<int32_t>(src_extent.height * dst_aspect);
        sx0 = (static_cast<int32_t>(src_extent.width) - crop_w) / 2;
        sx1 = sx0 + crop_w;
    } else {
        // Source is taller — crop top/bottom
        int32_t crop_h = static_cast<int32_t>(src_extent.width / dst_aspect);
        sy0 = (static_cast<int32_t>(src_extent.height) - crop_h) / 2;
        sy1 = sy0 + crop_h;
    }

    vk::ImageBlit region{};
    region.srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
    region.srcOffsets[0] = vk::Offset3D{sx0, sy0, 0};
    region.srcOffsets[1] = vk::Offset3D{sx1, sy1, 1};
    region.dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
    region.dstOffsets[0] = vk::Offset3D{0, 0, 0};
    region.dstOffsets[1] = vk::Offset3D{
        static_cast<int32_t>(dst_extent.width),
        static_cast<int32_t>(dst_extent.height), 1};

    cmd.blitImage(
        src, vk::ImageLayout::eTransferSrcOptimal,
        dst, vk::ImageLayout::eTransferDstOptimal,
        region, vk::Filter::eLinear);

    // Transition src back: TRANSFER_SRC → COLOR_ATTACHMENT_OPTIMAL (for XR runtime)
    vk::ImageMemoryBarrier src_back{};
    src_back.oldLayout = vk::ImageLayout::eTransferSrcOptimal;
    src_back.newLayout = vk::ImageLayout::eColorAttachmentOptimal;
    src_back.image = src;
    src_back.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    src_back.srcAccessMask = vk::AccessFlagBits::eTransferRead;
    src_back.dstAccessMask = {};

    // Transition dst: TRANSFER_DST → COLOR_ATTACHMENT_OPTIMAL (overlay pass handles → PRESENT_SRC)
    vk::ImageMemoryBarrier dst_to_attach{};
    dst_to_attach.oldLayout = vk::ImageLayout::eTransferDstOptimal;
    dst_to_attach.newLayout = vk::ImageLayout::eColorAttachmentOptimal;
    dst_to_attach.image = dst;
    dst_to_attach.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    dst_to_attach.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
    dst_to_attach.dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;

    std::array post_barriers = {src_back, dst_to_attach};
    cmd.pipelineBarrier(
        vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eColorAttachmentOutput,
        {}, nullptr, nullptr, post_barriers);

    return true;
}

void App::update_frame_ubo_xr(uint32_t eye, uint32_t frame_index,
                                const glm::mat4& view, const glm::mat4& projection,
                                vk::Extent2D extent) {
    float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);

    FrameUbo ubo{};
    ubo.view = view;
    ubo.projection = projection;
    ubo.resolution = glm::vec4(
        static_cast<float>(extent.width),
        static_cast<float>(extent.height),
        aspect, 0.0f);
    ubo.time = time_;
    std::memcpy(xr_eye_frame_buffers_[eye][frame_index].mapped, &ubo, sizeof(ubo));

    RayUbo ray{};
    ray.inv_view = glm::inverse(view);
    ray.inv_projection = glm::inverse(projection);
    std::memcpy(xr_eye_ray_buffers_[eye][frame_index].mapped, &ray, sizeof(ray));
}

void App::process_events() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        // Trap mouse on click in viewport — before ImGui sees the down event
        if (!mouse_trapped_ && event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
            && !ui_.wants_mouse() && pipeline_) {
            trap_mouse();
        }

        if (!mouse_trapped_) ui_.process_event(event);

        switch (event.type) {
        case SDL_EVENT_QUIT:
            running_ = false;
            break;

        case SDL_EVENT_KEY_DOWN:
            if (!mouse_trapped_ && ui_.wants_input()) break;
            switch (event.key.scancode) {
            case SDL_SCANCODE_ESCAPE:
                if (mouse_trapped_) { untrap_mouse(); }
                else { running_ = false; }
                break;
            case SDL_SCANCODE_R: reload_shader(); break;
            case SDL_SCANCODE_Q: unload_shader(); untrap_mouse(); break;
            case SDL_SCANCODE_T: time_ = 0.0f; break;
            case SDL_SCANCODE_C: camera_.reset(); break;
            case SDL_SCANCODE_G: ui_.toggle(); break;
            case SDL_SCANCODE_F:
            case SDL_SCANCODE_F11:
                window_.toggle_fullscreen();
                break;
            default: break;
            }
            break;

        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (event.button.button == SDL_BUTTON_LEFT)   mouse_left_ = true;
            if (event.button.button == SDL_BUTTON_RIGHT)  mouse_right_ = true;
            if (event.button.button == SDL_BUTTON_MIDDLE) mouse_middle_ = true;
            break;

        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (event.button.button == SDL_BUTTON_LEFT)   mouse_left_ = false;
            if (event.button.button == SDL_BUTTON_RIGHT)  mouse_right_ = false;
            if (event.button.button == SDL_BUTTON_MIDDLE) mouse_middle_ = false;
            break;

        case SDL_EVENT_MOUSE_MOTION:
            if (!mouse_trapped_ && ui_.wants_input()) break;
            if (mouse_left_ && mouse_right_) {
                float zoom = -event.motion.yrel * 0.1f;
                if (SDL_GetModState() & SDL_KMOD_CTRL) {
                    camera_.adjust_pivot_distance(zoom, Camera::ZOOM_MOUSE_SPEED);
                } else {
                    camera_.on_scroll(zoom, Camera::ZOOM_MOUSE_SPEED);
                }
            } else {
                camera_.on_mouse_move(event.motion.xrel, event.motion.yrel,
                                      mouse_left_, mouse_right_, mouse_middle_);
            }
            break;

        case SDL_EVENT_MOUSE_WHEEL:
            if (!mouse_trapped_ && ui_.wants_input()) break;
            if (SDL_GetModState() & SDL_KMOD_CTRL) {
                camera_.adjust_pivot_distance(event.wheel.y, Camera::ZOOM_SCROLL_SPEED);
            } else {
                camera_.on_scroll(event.wheel.y, Camera::ZOOM_SCROLL_SPEED);
            }
            break;

        case SDL_EVENT_WINDOW_RESIZED:
            engine_.recreate_swapchain();
            break;

        default:
            break;
        }
    }

}

void App::reload_shader() {
    if (shader_path_.empty()) {
        logger->warn("no shader path specified");
        return;
    }

    std::ifstream file(shader_path_);
    if (!file) {
        logger->error("failed to open shader: {}", shader_path_.string());
        return;
    }

    std::ostringstream ss;
    ss << file.rdbuf();
    std::string source = ss.str();

    try {
        auto frag_spirv = ShaderCompiler::compile_glsl(
            source, vk::ShaderStageFlagBits::eFragment, shader_path_.string());
        build_pipeline(frag_spirv);
        logger->info("shader loaded: {}", shader_path_.string());
    } catch (const std::exception& e) {
        logger->error("shader reload failed: {}", e.what());
    }
}

void App::trap_mouse() {
    SDL_SetWindowRelativeMouseMode(window_.handle(), true);
    mouse_trapped_ = true;
    ImGui::SetWindowFocus(nullptr);
}

void App::untrap_mouse() {
    SDL_SetWindowRelativeMouseMode(window_.handle(), false);
    mouse_trapped_ = false;
    mouse_left_ = false;
    mouse_right_ = false;
    mouse_middle_ = false;
}

void App::unload_shader() {
    if (pipeline_) {
        engine_.device().waitIdle();
        pipeline_.reset();
        logger->info("shader unloaded");
    }
}

void App::build_pipeline(const std::vector<uint32_t>& frag_spirv) {
    engine_.device().waitIdle();
    std::array<vk::DescriptorSetLayout, 2> layouts = {*frame_layout_, *ray_layout_};
    pipeline_ = std::make_unique<Pipeline>(
        engine_.device(), engine_.render_pass(),
        vert_spirv_, frag_spirv,
        layouts);
}

} // namespace vfr
