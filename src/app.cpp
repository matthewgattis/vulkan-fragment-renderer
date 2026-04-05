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

App::App(const std::filesystem::path& shader_path, bool high_dpi)
    : window_("vulkan-fragment-renderer", 1280, 720, high_dpi)
    , engine_(window_)
    , ui_(engine_, window_)
    , shader_path_(shader_path)
{
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
    for (auto& buf : frame_buffers_) {
        if (buf.buffer) {
            vmaDestroyBuffer(engine_.allocator(), buf.buffer, buf.allocation);
        }
    }
}

void App::create_frame_descriptors() {
    // Descriptor set layout: one UBO at binding 0, visible to vertex + fragment
    vk::DescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = vk::DescriptorType::eUniformBuffer;
    binding.descriptorCount = 1;
    binding.stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;

    vk::DescriptorSetLayoutCreateInfo layout_info{};
    layout_info.bindingCount = 1;
    layout_info.pBindings = &binding;
    frame_layout_ = vk::raii::DescriptorSetLayout(engine_.device(), layout_info);

    // Descriptor pool
    vk::DescriptorPoolSize pool_size{vk::DescriptorType::eUniformBuffer, MAX_FRAMES_IN_FLIGHT};
    vk::DescriptorPoolCreateInfo pool_info{};
    pool_info.maxSets = MAX_FRAMES_IN_FLIGHT;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    frame_pool_ = vk::raii::DescriptorPool(engine_.device(), pool_info);

    // Allocate descriptor sets
    std::array<vk::DescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> layouts;
    layouts.fill(*frame_layout_);

    vk::DescriptorSetAllocateInfo alloc_info{};
    alloc_info.descriptorPool = *frame_pool_;
    alloc_info.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
    alloc_info.pSetLayouts = layouts.data();
    auto sets = (*engine_.device()).allocateDescriptorSets(alloc_info);
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        frame_sets_[i] = sets[i];
    }

    // Create per-frame UBO buffers (persistently mapped)
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        VkBufferCreateInfo buf_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buf_info.size = sizeof(FrameUbo);
        buf_info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

        VmaAllocationCreateInfo alloc{};
        alloc.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        alloc.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                      VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;

        VmaAllocationInfo map_info{};
        vmaCreateBuffer(engine_.allocator(), &buf_info, &alloc,
                        &frame_buffers_[i].buffer, &frame_buffers_[i].allocation, &map_info);
        frame_buffers_[i].mapped = map_info.pMappedData;

        // Point descriptor set at this buffer
        vk::DescriptorBufferInfo desc_buf{frame_buffers_[i].buffer, 0, sizeof(FrameUbo)};
        vk::WriteDescriptorSet write{};
        write.dstSet = frame_sets_[i];
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = vk::DescriptorType::eUniformBuffer;
        write.pBufferInfo = &desc_buf;
        (*engine_.device()).updateDescriptorSets(write, nullptr);
    }

    LOG_INFO("frame descriptors created");
}

void App::update_frame_ubo(uint32_t frame_index) {
    auto extent = engine_.swapchain_extent();
    float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);

    FrameUbo ubo{};
    ubo.view = camera_.view_matrix();

    // Perspective projection — Vulkan depth range [0,1], Y-flip
    ubo.projection = glm::perspective(glm::radians(60.0f), aspect, 0.01f, 1000.0f);
    ubo.projection[1][1] *= -1.0f; // Vulkan Y-flip

    ubo.resolution = glm::vec4(
        static_cast<float>(extent.width),
        static_cast<float>(extent.height),
        aspect, 0.0f);
    ubo.time = time_;

    std::memcpy(frame_buffers_[frame_index].mapped, &ubo, sizeof(ubo));
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

        auto cmd = engine_.begin_frame();
        if (!cmd) continue;

        // Update UBO *after* begin_frame's fence wait guarantees the GPU
        // is done reading this frame's buffer from its previous use.
        uint32_t fi = engine_.frame_index();
        update_frame_ubo(fi);

        if (pipeline_) {
            pipeline_->bind(cmd);

            // Bind frame UBO (set 0)
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                                   pipeline_->layout(), 0, frame_sets_[fi], nullptr);

            // Push identity model matrix
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
    pipeline_ = std::make_unique<Pipeline>(
        engine_.device(), engine_.render_pass(),
        vert_spirv_, frag_spirv,
        *frame_layout_);
}

} // namespace vfr
