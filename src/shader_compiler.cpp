#define LOG_MODULE_NAME "shader"

#include "shader_compiler.hpp"
#include "log.hpp"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <shaderc/shaderc.hpp>
#include <fstream>
#include <stdexcept>

static auto logger = spdlog::stdout_color_mt(LOG_MODULE_NAME);

namespace vfr {

static shaderc_shader_kind to_shaderc_kind(vk::ShaderStageFlagBits stage) {
    switch (stage) {
        case vk::ShaderStageFlagBits::eVertex:   return shaderc_vertex_shader;
        case vk::ShaderStageFlagBits::eFragment: return shaderc_fragment_shader;
        case vk::ShaderStageFlagBits::eCompute:  return shaderc_compute_shader;
        default: throw std::runtime_error("unsupported shader stage");
    }
}

std::vector<uint32_t> ShaderCompiler::compile_glsl(
    const std::string& source,
    vk::ShaderStageFlagBits stage,
    const std::string& filename)
{
    shaderc::Compiler compiler;
    shaderc::CompileOptions options;
    options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
    options.SetOptimizationLevel(shaderc_optimization_level_performance);

    auto result = compiler.CompileGlslToSpv(
        source, to_shaderc_kind(stage), filename.c_str(), options);

    if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
        LOG_ERROR("shader compilation failed:\n{}", result.GetErrorMessage());
        throw std::runtime_error("shader compilation failed: " + result.GetErrorMessage());
    }

    if (result.GetNumWarnings() > 0) {
        LOG_WARN("shader warnings:\n{}", result.GetErrorMessage());
    }

    return {result.cbegin(), result.cend()};
}

std::vector<uint32_t> ShaderCompiler::load_spirv(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error("failed to open SPIR-V file: " + path.string());
    }

    auto size = file.tellg();
    file.seekg(0);

    std::vector<uint32_t> spirv(static_cast<size_t>(size) / sizeof(uint32_t));
    file.read(reinterpret_cast<char*>(spirv.data()), size);
    return spirv;
}

vk::raii::ShaderModule ShaderCompiler::create_module(
    const vk::raii::Device& device,
    const std::vector<uint32_t>& spirv)
{
    vk::ShaderModuleCreateInfo info{};
    info.codeSize = spirv.size() * sizeof(uint32_t);
    info.pCode = spirv.data();
    return vk::raii::ShaderModule(device, info);
}

} // namespace vfr
