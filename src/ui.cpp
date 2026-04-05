#define LOG_MODULE_NAME "ui"

#include "ui.hpp"
#include "engine.hpp"
#include "window.hpp"
#include "camera.hpp"
#include "log.hpp"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>
#include <SDL3/SDL.h>

static auto logger = spdlog::stdout_color_mt(LOG_MODULE_NAME);

namespace vfr {

Ui::Ui(Engine& engine, Window& window) : engine_(engine) {
    // Descriptor pool for ImGui
    std::array<vk::DescriptorPoolSize, 1> pool_sizes{{
        {vk::DescriptorType::eCombinedImageSampler, 1},
    }};

    vk::DescriptorPoolCreateInfo pool_info{};
    pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
    pool_info.pPoolSizes = pool_sizes.data();

    imgui_pool_ = vk::raii::DescriptorPool(engine_.device(), pool_info);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    auto& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui_ImplSDL3_InitForVulkan(window.handle());

    ImGui_ImplVulkan_InitInfo init_info{};
    init_info.ApiVersion = VK_API_VERSION_1_3;
    init_info.Instance = *engine_.instance();
    init_info.PhysicalDevice = *engine_.physical_device();
    init_info.Device = *engine_.device();
    init_info.QueueFamily = engine_.graphics_queue_family();
    init_info.Queue = engine_.graphics_queue();
    init_info.DescriptorPool = *imgui_pool_;
    init_info.MinImageCount = 2;
    init_info.ImageCount = MAX_FRAMES_IN_FLIGHT;
    init_info.PipelineInfoMain.RenderPass = engine_.render_pass();
    init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

    ImGui_ImplVulkan_Init(&init_info);

    LOG_INFO("ImGui initialized");
}

Ui::~Ui() {
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

void Ui::process_event(const SDL_Event& event) {
    ImGui_ImplSDL3_ProcessEvent(&event);
}

void Ui::begin_frame() {
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
}

void Ui::render(Camera& camera, float fps, bool mouse_trapped) {
    if (!visible_) return;

    ImGui::Begin("Info", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::Text("FPS: %.1f", fps);
    ImGui::Separator();

    auto pos = camera.position();
    ImGui::Text("Position: (%.2f, %.2f, %.2f)", pos.x, pos.y, pos.z);

    float dist = camera.pivot_distance();
    ImGui::Text("Pivot distance: %.2f", dist);

    ImGui::Separator();
    if (mouse_trapped) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Mouse captured - press Escape to release");
    } else {
        ImGui::TextDisabled("Click viewport to capture mouse");
    }
    ImGui::Separator();
    ImGui::TextDisabled("R: reload | G: toggle UI | F: fullscreen");
    ImGui::TextDisabled("C: reset camera | T: reset time");
    ImGui::End();
}

void Ui::end_frame(vk::CommandBuffer cmd) {
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
}

bool Ui::wants_input() const {
    if (!visible_) return false;
    auto& io = ImGui::GetIO();
    return io.WantCaptureMouse || io.WantCaptureKeyboard;
}

bool Ui::wants_mouse() const {
    if (!visible_) return false;
    return ImGui::GetIO().WantCaptureMouse;
}

} // namespace vfr
