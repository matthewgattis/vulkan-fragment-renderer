# Development Guidelines

## Project Overview

Vulkan-based real-time fragment shader viewer. Single executable, flat namespace (`vfr::`), no multi-layer engine architecture. Designed for simplicity with room to grow into a render engine.

## Build

```bash
cmake --preset default && cmake --build build
```

## Architecture

- **Engine** (`engine.hpp/cpp`) — Vulkan instance, device, VMA allocator, swapchain, render pass, frame lifecycle (acquire/submit/present), deferred destruction, semaphore ring for sync.
- **Window** (`window.hpp/cpp`) — SDL3 window, Vulkan surface, DPI, fullscreen.
- **Pipeline** (`pipeline.hpp/cpp`) — Graphics pipeline builder. Descriptor set layout (set 0 = frame UBO), push constants (per-object model matrix). Depth test/write enabled.
- **ShaderCompiler** (`shader_compiler.hpp/cpp`) — Runtime GLSL-to-SPIR-V via shaderc, SPIR-V file loader.
- **Camera** (`camera.hpp/cpp`) — Quaternion-based orbit/free-look camera. No gimbal lock. View matrix built directly from quaternion (no `lookAt`).
- **Ui** (`ui.hpp/cpp`) — Dear ImGui initialization and rendering within the main render pass.
- **App** (`app.hpp/cpp`) — Main loop, event dispatch, frame UBO management, shader hot-reload.

## Conventions

- **C++23**, modern CMake (3.25+).
- **Namespace:** `vfr::`.
- **Files:** `lowercase_with_underscores.hpp/.cpp`.
- **Types:** `PascalCase`. **Variables:** `snake_case_`. **Constants:** `UPPER_CASE`.
- **RAII:** `vk::raii::*` types exclusively. No manual Vulkan cleanup except VMA resources.
- **VMA:** Single `vma_impl.cpp` compilation unit. Dynamic Vulkan function loading.
- **Logging:** Per-module via spdlog. Define `LOG_MODULE_NAME` before including `log.hpp`. Create logger with `spdlog::stdout_color_mt(LOG_MODULE_NAME)` as file-level static.
- **Dependencies:** vcpkg submodule at `./vcpkg/`. All deps in `vcpkg.json`.

## Rendering Pipeline

1. `begin_frame()` — fence wait, acquire swapchain image, begin render pass.
2. Bind pipeline, bind frame UBO descriptor set (set 0), push model matrix, draw.
3. ImGui render.
4. `end_frame()` — end render pass, submit, present.

**Frame UBO** (set 0, binding 0): `mat4 view`, `mat4 projection`, `vec4 resolution`, `float time`.
**Push constants** (per-object): `mat4 model` — reserved for future geometry rendering.

## Synchronization

- **Frames in flight:** 2 (`MAX_FRAMES_IN_FLIGHT`).
- **Semaphore ring:** Sized to swapchain image count (typically 3) to avoid reuse conflicts between acquire/present.
- **UBO updates:** Must happen AFTER `begin_frame()` fence wait, never before.
- **Deferred destruction:** `engine.defer_destroy()` holds resources for `MAX_FRAMES_IN_FLIGHT + 1` frames.

## Shader Interface

Fragment shaders must use `#version 450` and declare:
- `layout(set = 0, binding = 0) uniform FrameUbo { ... }` for frame data.
- `layout(push_constant) uniform PushConstants { mat4 Model; }` (unused for fullscreen).
- `layout(location = 0) in vec2 FragCoord` — screen coords from vertex shader.
- `layout(location = 0) out vec4 outColor`.
- Optionally write `gl_FragDepth` using `Projection * View * vec4(hitPos, 1.0)`.

## Key Design Decisions

- **Fullscreen triangle** over fullscreen quad — 3 vertices, no vertex buffer, generated in vertex shader.
- **Runtime shader compilation** over offline — enables hot-reload workflow, shaderc linked via vcpkg.
- **Push constants for per-object** — model matrix lives in push constants so it can change per draw call without descriptor set updates. Frame-level data in UBO.
- **No `lookAt` in camera** — view matrix built from quaternion conjugate to avoid gimbal lock degeneration.
- **Vulkan Y-flip** — handled in vertex shader (`FragCoord.y` negated) and projection matrix (`proj[1][1] *= -1`).
