cmake_minimum_required(VERSION 3.25)
set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake")

project(vulkan-fragment-renderer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Vulkan REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(SDL3 CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
find_package(unofficial-shaderc CONFIG REQUIRED)
find_package(argparse CONFIG REQUIRED)
find_package(OpenXR CONFIG REQUIRED)

# Compile fullscreen vertex shader to SPIR-V, then embed as a C++ header
find_program(GLSLC glslc REQUIRED)

set(SHADER_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(SHADER_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
set(GENERATED_INCLUDE_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${SHADER_OUTPUT_DIR})
file(MAKE_DIRECTORY ${GENERATED_INCLUDE_DIR})

set(FULLSCREEN_VERT_SPV ${SHADER_OUTPUT_DIR}/fullscreen.vert.spv)
set(FULLSCREEN_VERT_HDR ${GENERATED_INCLUDE_DIR}/fullscreen_vert_spv.h)

add_custom_command(
    OUTPUT ${FULLSCREEN_VERT_SPV}
    COMMAND ${GLSLC} ${SHADER_SOURCE_DIR}/fullscreen.vert -o ${FULLSCREEN_VERT_SPV}
    DEPENDS ${SHADER_SOURCE_DIR}/fullscreen.vert
    COMMENT "Compiling fullscreen.vert to SPIR-V"
)

add_custom_command(
    OUTPUT ${FULLSCREEN_VERT_HDR}
    COMMAND ${CMAKE_COMMAND}
        -DSPIRV_FILE=${FULLSCREEN_VERT_SPV}
        -DHEADER_FILE=${FULLSCREEN_VERT_HDR}
        -DARRAY_NAME=fullscreen_vert_spv
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/spirv_to_header.cmake
    DEPENDS ${FULLSCREEN_VERT_SPV}
    COMMENT "Generating fullscreen_vert_spv.h"
)

add_custom_target(shaders DEPENDS ${FULLSCREEN_VERT_HDR})

add_executable(vulkan-fragment-renderer
    src/main.cpp
    src/app.cpp
    src/engine.cpp
    src/window.cpp
    src/shader_compiler.cpp
    src/pipeline.cpp
    src/ui.cpp
    src/camera.cpp
    src/vma_impl.cpp
    src/xr_session.cpp
)

add_dependencies(vulkan-fragment-renderer shaders)

target_include_directories(vulkan-fragment-renderer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${GENERATED_INCLUDE_DIR}
)

target_compile_definitions(vulkan-fragment-renderer PRIVATE
    XR_USE_GRAPHICS_API_VULKAN
    GLM_FORCE_DEPTH_ZERO_TO_ONE
)

target_link_libraries(vulkan-fragment-renderer PRIVATE
    Vulkan::Vulkan
    glm::glm
    SDL3::SDL3
    spdlog::spdlog
    imgui::imgui
    GPUOpen::VulkanMemoryAllocator
    unofficial::shaderc::shaderc
    argparse::argparse
    OpenXR::openxr_loader
)
