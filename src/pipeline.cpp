#define LOG_MODULE_NAME "pipeline"

#include "pipeline.hpp"
#include "shader_compiler.hpp"
#include "log.hpp"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <glm/glm.hpp>

static auto logger = spdlog::stdout_color_mt(LOG_MODULE_NAME);

namespace vfr {

Pipeline::Pipeline(const vk::raii::Device& device,
                   vk::RenderPass render_pass,
                   const std::vector<uint32_t>& vert_spirv,
                   const std::vector<uint32_t>& frag_spirv,
                   std::span<const vk::DescriptorSetLayout> descriptor_layouts,
                   bool depth_write)
{
    auto vert_module = ShaderCompiler::create_module(device, vert_spirv);
    auto frag_module = ShaderCompiler::create_module(device, frag_spirv);

    std::array<vk::PipelineShaderStageCreateInfo, 2> stages{{
        {{}, vk::ShaderStageFlagBits::eVertex, *vert_module, "main"},
        {{}, vk::ShaderStageFlagBits::eFragment, *frag_module, "main"},
    }};

    // No vertex input — fullscreen triangle generated in vertex shader
    vk::PipelineVertexInputStateCreateInfo vertex_input{};

    vk::PipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.topology = vk::PrimitiveTopology::eTriangleList;

    // Dynamic viewport/scissor
    std::array dynamic_states = {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    vk::PipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
    dynamic_state.pDynamicStates = dynamic_states.data();

    vk::PipelineViewportStateCreateInfo viewport_state{};
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    vk::PipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.polygonMode = vk::PolygonMode::eFill;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = vk::CullModeFlagBits::eNone;

    vk::PipelineMultisampleStateCreateInfo multisampling{};
    multisampling.rasterizationSamples = vk::SampleCountFlagBits::e1;

    vk::PipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.depthTestEnable = depth_write ? VK_TRUE : VK_FALSE;
    depth_stencil.depthWriteEnable = depth_write ? VK_TRUE : VK_FALSE;
    depth_stencil.depthCompareOp = vk::CompareOp::eLess;

    vk::PipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask =
        vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
        vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;

    vk::PipelineColorBlendStateCreateInfo color_blending{};
    color_blending.attachmentCount = 1;
    color_blending.pAttachments = &blend_attachment;

    // Push constant: per-object model matrix (64 bytes, vertex + fragment)
    vk::PushConstantRange push_range{};
    push_range.stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;
    push_range.offset = 0;
    push_range.size = sizeof(glm::mat4);

    vk::PipelineLayoutCreateInfo layout_info{};
    layout_info.setLayoutCount = static_cast<uint32_t>(descriptor_layouts.size());
    layout_info.pSetLayouts = descriptor_layouts.data();
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_range;

    layout_ = vk::raii::PipelineLayout(device, layout_info);

    vk::GraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.stageCount = static_cast<uint32_t>(stages.size());
    pipeline_info.pStages = stages.data();
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterizer;
    pipeline_info.pMultisampleState = &multisampling;
    pipeline_info.pDepthStencilState = &depth_stencil;
    pipeline_info.pColorBlendState = &color_blending;
    pipeline_info.pDynamicState = &dynamic_state;
    pipeline_info.layout = *layout_;
    pipeline_info.renderPass = render_pass;

    pipeline_ = vk::raii::Pipeline(device, nullptr, pipeline_info);

    LOG_INFO("graphics pipeline created");
}

void Pipeline::bind(vk::CommandBuffer cmd) const {
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline_);
}

} // namespace vfr
