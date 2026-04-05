# Development Guidelines

## Project Overview

Vulkan-based real-time fragment shader viewer. Single executable, flat namespace (`vfr::`), no multi-layer engine architecture. Designed for simplicity with room to grow into a render engine.

## Build

```bash
cmake --preset default && cmake --build build
```

## Architecture

- **Engine** (`engine.hpp/cpp`) — Vulkan instance, device, VMA allocator, swapchain, render pass, frame lifecycle (acquire/submit/present), deferred destruction, semaphore ring for sync. Accepts `ExternalVulkanRequirements` for XR extension/device negotiation.
- **Window** (`window.hpp/cpp`) — SDL3 window, Vulkan surface, DPI, fullscreen.
- **Pipeline** (`pipeline.hpp/cpp`) — Graphics pipeline builder. Accepts `span<DescriptorSetLayout>` for multi-set layouts, push constants (per-object model matrix). Depth test/write enabled.
- **ShaderCompiler** (`shader_compiler.hpp/cpp`) — Runtime GLSL-to-SPIR-V via shaderc, SPIR-V file loader.
- **Camera** (`camera.hpp/cpp`) — Quaternion-based orbit/free-look camera. No gimbal lock. View matrix built directly from quaternion (no `lookAt`).
- **Ui** (`ui.hpp/cpp`) — Dear ImGui initialization and rendering within the main render pass.
- **App** (`app.hpp/cpp`) — Main loop, event dispatch, frame UBO management, shader hot-reload. Branches to desktop or XR render path. Manages per-eye descriptor sets and UBO buffers for XR stereo rendering.
- **XrSession** (`xr_session.hpp/cpp`) — OpenXR session lifecycle via `XR_KHR_vulkan_enable` (v1). Two-phase init: static `query_requirements()`/`query_physical_device()` before Vulkan device creation, constructor after. Manages XR instance, session, LOCAL reference space, per-eye swapchains, depth buffers, framebuffers, and frame submission. Provides per-eye view/projection matrices with asymmetric frustum support.

## Conventions

- **C++23**, modern CMake (3.25+).
- **Namespace:** `vfr::`.
- **Files:** `lowercase_with_underscores.hpp/.cpp`.
- **Types:** `PascalCase`. **Variables:** `snake_case_`. **Constants:** `UPPER_CASE`.
- **RAII:** `vk::raii::*` types exclusively. No manual Vulkan cleanup except VMA resources.
- **VMA:** Single `vma_impl.cpp` compilation unit. Dynamic Vulkan function loading.
- **Logging:** Per-module via spdlog. Create a named logger with `spdlog::stdout_color_mt("module_name")` as a file-level static, then call `logger->info(...)` etc. directly.
- **Dependencies:** vcpkg submodule at `./vcpkg/`. All deps in `vcpkg.json`.

## Rendering Pipeline

### Desktop path
1. `begin_frame()` — fence wait, acquire swapchain image, begin render pass.
2. Bind pipeline, bind descriptor sets (set 0 + set 1), push model matrix, draw.
3. ImGui render.
4. `end_frame()` — end render pass, submit, present.

### XR path
1. `poll_events()` — XR session state machine.
2. `wait_and_begin_frame()` — synchronize with runtime.
3. Left eye: update UBOs, `begin_eye_render()`, bind/draw, `end_eye_render_pass()` (render pass only — image held for mirror).
4. Right eye: update UBOs, `begin_eye_render()`, bind/draw, `end_eye_render()` (full release).
5. `blit_xr_mirror()` — acquire desktop swapchain image, blit left eye with aspect-preserving fill crop, release left eye XR image.
6. `submit_and_present()` — submit command buffer + present desktop mirror. Falls back to `submit_xr_only()` if window unavailable.
7. `end_frame()` — submit composition layers to runtime.

### Descriptor sets
- **Set 0** (binding 0, `FrameUbo`): `mat4 view`, `mat4 projection`, `vec4 resolution`, `float time` — common to all shaders.
- **Set 1** (binding 0, `RayUbo`): `mat4 inv_view`, `mat4 inv_projection` — precomputed inverses for ray marching shaders.
- **Push constants** (per-object): `mat4 model` — reserved for future geometry rendering.

## Synchronization

- **Frames in flight:** 2 (`MAX_FRAMES_IN_FLIGHT`).
- **Semaphore ring:** Sized to swapchain image count (typically 3) to avoid reuse conflicts between acquire/present.
- **UBO updates:** Must happen AFTER `begin_frame()` fence wait, never before.
- **Deferred destruction:** `engine.defer_destroy()` holds resources for `MAX_FRAMES_IN_FLIGHT + 1` frames.

## Shader Interface

Fragment shaders must use `#version 450` and declare:
- `layout(set = 0, binding = 0) uniform FrameUbo { ... }` for common frame data.
- `layout(set = 1, binding = 0) uniform RayUbo { mat4 InvView; mat4 InvProjection; }` for ray marching.
- `layout(push_constant) uniform PushConstants { mat4 Model; }` (unused for fullscreen).
- `layout(location = 0) in vec2 FragCoord` — screen coords from vertex shader (Vulkan NDC, -1 to 1).
- `layout(location = 0) out vec4 outColor`.
- Ray direction must be constructed via `InvProjection * vec4(FragCoord, 0.0, 1.0)` to support asymmetric XR frustums.
- Optionally write `gl_FragDepth` using `Projection * View * vec4(hitPos, 1.0)`.

## Key Design Decisions

- **Fullscreen triangle** over fullscreen quad — 3 vertices, no vertex buffer, generated in vertex shader.
- **Runtime shader compilation** over offline — enables hot-reload workflow, shaderc linked via vcpkg.
- **Push constants for per-object** — model matrix lives in push constants so it can change per draw call without descriptor set updates. Frame-level data in UBO.
- **No `lookAt` in camera** — view matrix built from quaternion conjugate to avoid gimbal lock degeneration.
- **Right-handed Y-up world space** — matches GLM and OpenXR conventions. `GLM_FORCE_DEPTH_ZERO_TO_ONE` for Vulkan [0,1] depth range.
- **Vulkan Y-flip** — handled in projection matrix (`proj[1][1] *= -1`). Fragment shaders go through `inverse(Projection)` which naturally undoes the flip.
- **Two descriptor sets** — set 0 for common frame data (usable by future rasterized geometry), set 1 for ray-marching-specific inverse matrices.
- **OpenXR v1 API** (`XR_KHR_vulkan_enable`) — two-phase init to satisfy Vulkan/XR device negotiation. Seated/LOCAL reference space. Per-eye asymmetric frustum via `glm::frustum`.
