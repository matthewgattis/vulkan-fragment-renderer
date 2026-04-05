#pragma once

#include <vulkan/vulkan_raii.hpp>
#include <filesystem>
#include <string>
#include <vector>

namespace vfr {

class ShaderCompiler {
public:
    // Compile GLSL source to SPIR-V
    static std::vector<uint32_t> compile_glsl(
        const std::string& source,
        vk::ShaderStageFlagBits stage,
        const std::string& filename = "<inline>");

    // Load pre-compiled SPIR-V from file
    static std::vector<uint32_t> load_spirv(const std::filesystem::path& path);

    // Create a shader module from SPIR-V
    static vk::raii::ShaderModule create_module(
        const vk::raii::Device& device,
        const std::vector<uint32_t>& spirv);
};

} // namespace vfr
