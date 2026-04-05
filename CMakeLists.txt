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

# Compile fullscreen vertex shader to SPIR-V at build time
find_program(GLSLC glslc REQUIRED)

set(SHADER_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(SHADER_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUTPUT_DIR})

set(STATIC_SHADERS
    ${SHADER_SOURCE_DIR}/fullscreen.vert
)

foreach(SHADER ${STATIC_SHADERS})
    get_filename_component(SHADER_NAME ${SHADER} NAME)
    set(SHADER_OUTPUT ${SHADER_OUTPUT_DIR}/${SHADER_NAME}.spv)
    add_custom_command(
        OUTPUT ${SHADER_OUTPUT}
        COMMAND ${GLSLC} ${SHADER} -o ${SHADER_OUTPUT}
        DEPENDS ${SHADER}
        COMMENT "Compiling ${SHADER_NAME} to SPIR-V"
    )
    list(APPEND SHADER_OUTPUTS ${SHADER_OUTPUT})
endforeach()

add_custom_target(shaders DEPENDS ${SHADER_OUTPUTS})

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

target_include_directories(vulkan-fragment-renderer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_definitions(vulkan-fragment-renderer PRIVATE
    SHADER_DIR="${SHADER_OUTPUT_DIR}"
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
