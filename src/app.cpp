#define LOG_MODULE_NAME "app"

#include "app.hpp"
#include "shader_compiler.hpp"
#include "log.hpp"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <glm/gtc/matrix_transform.hpp>
#include <SDL3/SDL.h>
#include <fstream>
#include <sstream>
#include <cstring>

static auto logger = spdlog::stdout_color_mt(LOG_MODULE_NAME);

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

    LOG_INFO("frame descriptors created");
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
    ui_.render(camera_, fps_);
    ui_.end_frame(cmd);

    engine_.end_frame();
}

void App::run_xr_frame(float /*dt*/) {
    xr_session_->poll_events();

    if (!xr_session_->session_running()) return;

    bool should_render = xr_session_->wait_and_begin_frame();

    if (should_render) {
        auto views = xr_session_->locate_views(camera_.position());

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

                xr_session_->end_eye_render(cmd, eye);
            }
        } else {
            // No pipeline — still need to acquire/release swapchain images
            for (uint32_t eye = 0; eye < 2; ++eye) {
                xr_session_->begin_eye_render(cmd, eye);
                xr_session_->end_eye_render(cmd, eye);
            }
        }

        // Submit the command buffer (no desktop present in XR mode for now)
        engine_.submit_xr_only();
    }

    xr_session_->end_frame();
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
        ui_.process_event(event);

        switch (event.type) {
        case SDL_EVENT_QUIT:
            running_ = false;
            break;

        case SDL_EVENT_KEY_DOWN:
            if (ui_.wants_input()) break;
            switch (event.key.scancode) {
            case SDL_SCANCODE_ESCAPE: running_ = false; break;
            case SDL_SCANCODE_R: reload_shader(); break;
            case SDL_SCANCODE_Q: unload_shader(); break;
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
            if (ui_.wants_input()) break;
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
            if (ui_.wants_input()) break;
            if (mouse_left_ && mouse_right_) {
                camera_.on_scroll(-event.motion.yrel * 0.1f);
            } else {
                camera_.on_mouse_move(event.motion.xrel, event.motion.yrel,
                                      mouse_left_, mouse_right_, mouse_middle_);
            }
            break;

        case SDL_EVENT_MOUSE_WHEEL:
            if (ui_.wants_input()) break;
            camera_.on_scroll(event.wheel.y);
            break;

        case SDL_EVENT_WINDOW_RESIZED:
            engine_.recreate_swapchain();
            break;

        default:
            break;
        }
    }

    // Keyboard movement (continuous)
    if (!ui_.wants_input()) {
        const bool* keys = SDL_GetKeyboardState(nullptr);
        glm::vec3 dir{0.0f};
        float dt = 1.0f / 60.0f;

        if (keys[SDL_SCANCODE_W]) dir.y += 1.0f;
        if (keys[SDL_SCANCODE_S]) dir.y -= 1.0f;
        if (keys[SDL_SCANCODE_A]) dir.x -= 1.0f;
        if (keys[SDL_SCANCODE_D]) dir.x += 1.0f;
        if (keys[SDL_SCANCODE_SPACE])  dir.z += 1.0f;
        if (keys[SDL_SCANCODE_LSHIFT]) dir.z -= 1.0f;

        if (glm::length(dir) > 0.0f) {
            camera_.on_key_move(glm::normalize(dir), dt);
        }
    }
}

void App::reload_shader() {
    if (shader_path_.empty()) {
        LOG_WARN("no shader path specified");
        return;
    }

    std::ifstream file(shader_path_);
    if (!file) {
        LOG_ERROR("failed to open shader: {}", shader_path_.string());
        return;
    }

    std::ostringstream ss;
    ss << file.rdbuf();
    std::string source = ss.str();

    try {
        auto frag_spirv = ShaderCompiler::compile_glsl(
            source, vk::ShaderStageFlagBits::eFragment, shader_path_.string());
        build_pipeline(frag_spirv);
        LOG_INFO("shader loaded: {}", shader_path_.string());
    } catch (const std::exception& e) {
        LOG_ERROR("shader reload failed: {}", e.what());
    }
}

void App::unload_shader() {
    if (pipeline_) {
        engine_.device().waitIdle();
        pipeline_.reset();
        LOG_INFO("shader unloaded");
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
