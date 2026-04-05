# vulkan-fragment-renderer

A real-time GLSL fragment shader viewer built on Vulkan. Load any fragment shader and explore it interactively with an orbit/free-look camera.

## Features

- **Vulkan rendering** with VMA for memory management
- **Runtime GLSL compilation** via shaderc — no offline shader compilation step for user shaders
- **Hot-reload** — press `R` to recompile and reload the shader from disk
- **Depth buffer writes** — fragment shaders write `gl_FragDepth` via projection matrix, enabling future compositing with geometry
- **Quaternion camera** — gimbal-lock-free orbit and free-look modes with physics-based movement
- **Dear ImGui overlay** — FPS, camera info, and controls reference
- **Frame UBO** — view, projection, resolution, and time uniforms available to all shaders
- **Push constants** — reserved for per-object model matrix (identity for fullscreen shaders)

## Building

Requires CMake 3.25+, a C++23 compiler, and the Vulkan SDK.

```bash
# Bootstrap vcpkg (first time only)
./vcpkg/bootstrap-vcpkg.sh

# Configure and build
cmake --preset default
cmake --build build
```

## Usage

```bash
./build/vulkan-fragment-renderer <shader.glsl>
./build/vulkan-fragment-renderer example.glsl
./build/vulkan-fragment-renderer --low-dpi example.glsl
```

## Controls

| Input | Action |
|---|---|
| Left-click drag | Free-look (rotate camera in place) |
| Right-click drag | Orbit (rotate around pivot) |
| Middle-click drag | Pan |
| Both buttons drag | Zoom (forward/back) |
| Scroll wheel | Zoom |
| WASD | Move (view-relative) |
| Space / Shift | Up / Down |
| R | Reload shader |
| Q | Unload shader |
| T | Reset time |
| C | Reset camera |
| G | Toggle UI |
| F / F11 | Toggle fullscreen |
| Escape | Quit |

## Writing Shaders

Fragment shaders receive these inputs:

```glsl
// Frame uniforms (descriptor set 0, binding 0)
layout(set = 0, binding = 0) uniform FrameUbo {
    mat4 View;
    mat4 Projection;
    vec4 Resolution;  // x, y, aspect, unused
    float Time;
};

// Per-object (push constant) — identity for fullscreen shaders
layout(push_constant) uniform PushConstants {
    mat4 Model;
};

// Screen coordinates from vertex shader
layout(location = 0) in vec2 FragCoord;  // (-1,-1) to (1,1)

// Output
layout(location = 0) out vec4 outColor;
```

To write depth (for future compositing with geometry):

```glsl
vec3 hitPos = rayOrigin + rayDir * distance;
vec4 clip = Projection * View * vec4(hitPos, 1.0);
gl_FragDepth = clip.z / clip.w;
```

See `example.glsl`, `kaleidoscopic-ifs.glsl`, and `mandelbox.glsl` for complete examples.

## Dependencies

Managed via vcpkg (submoduled):

- Vulkan 1.3
- VMA (Vulkan Memory Allocator)
- SDL3 (windowing, input)
- shaderc (runtime GLSL to SPIR-V)
- Dear ImGui (debug UI)
- GLM (math)
- spdlog (logging)
- argparse (CLI)
