#pragma once

#include <vulkan/vulkan_raii.hpp>
#include <span>
#include <vector>

namespace vfr {

class Pipeline {
public:
    Pipeline(const vk::raii::Device& device,
             vk::RenderPass render_pass,
             const std::vector<uint32_t>& vert_spirv,
             const std::vector<uint32_t>& frag_spirv,
             std::span<const vk::DescriptorSetLayout> descriptor_layouts,
             bool depth_write = true);

    void bind(vk::CommandBuffer cmd) const;
    vk::PipelineLayout layout() const { return *layout_; }

private:
    vk::raii::PipelineLayout layout_{nullptr};
    vk::raii::Pipeline pipeline_{nullptr};
};

} // namespace vfr
