# vulkan-fragment-renderer

A real-time GLSL fragment shader viewer built on Vulkan with OpenXR HMD support. Load any fragment shader and explore it interactively with an orbit/free-look camera, or in VR with full 6DoF head tracking.

## Features

- **Vulkan rendering** with VMA for memory management, sRGB swapchain (hardware gamma correction)
- **Automatic window sizing** — selects the largest 4:3 resolution that fits the display's usable area
- **OpenXR HMD support** — stereo rendering with per-eye asymmetric frustums, seated/LOCAL reference space, 6DoF head tracking. XR rig is a child of the camera (full transform). Left eye mirrored to desktop window via blit (aspect-preserving fill) with ImGui overlay. Falls back to desktop rendering when HMD is idle/not worn. WASD moves in HMD space when headset is active. Enabled by default when a headset is available; disable with `--no-xr`
- **Runtime GLSL compilation** via shaderc — no offline shader compilation step for user shaders
- **Hot-reload** — press `R` to recompile and reload the shader from disk
- **Depth buffer writes** — fragment shaders write `gl_FragDepth` via projection matrix, enabling future compositing with geometry
- **Quaternion camera** — gimbal-lock-free orbit and free-look modes with acceleration/friction physics, pivot-distance-scaled movement
- **Mouse capture** — click viewport to capture, Escape to release. Camera controls require capture when UI is visible
- **Dear ImGui overlay** — FPS, camera info, capture state, and controls reference (auto-resizing)
- **Two-set descriptor layout** — set 0 (common frame data) for all shaders, set 1 (precomputed inverse matrices) for ray marching
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
./build/vulkan-fragment-renderer --no-xr example.glsl   # force desktop-only
```

## Controls

| Input | Action |
|---|---|
| Left-click drag | Free-look (rotate camera in place) |
| Right-click drag | Orbit (rotate around pivot) |
| Middle-click drag | Pan |
| Both buttons drag | Zoom (forward/back) |
| Scroll wheel | Zoom |
| Ctrl + both buttons | Adjust pivot distance (mouse) |
| Ctrl + scroll | Adjust pivot distance (scroll) |
| WASD | Move (camera-relative, or HMD-relative in VR) |
| Space / Shift | Up / Down |
| R | Reload shader |
| Q | Unload shader (also releases mouse) |
| T | Reset time |
| C | Reset camera |
| G | Toggle UI |
| F / F11 | Toggle fullscreen |
| Escape | Release mouse (or quit if not captured) |

## Writing Shaders

Fragment shaders receive these inputs:

```glsl
// Set 0: common frame data (all shaders)
layout(set = 0, binding = 0) uniform FrameUbo {
    mat4 View;
    mat4 Projection;
    vec4 Resolution;  // x, y, aspect, unused
    float Time;
};

// Set 1: precomputed inverses (ray marching shaders)
layout(set = 1, binding = 0) uniform RayUbo {
    mat4 InvView;
    mat4 InvProjection;
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

Ray marching shaders should construct rays from the inverse matrices. This correctly handles asymmetric frustums (e.g. per-eye XR) and avoids per-fragment matrix inversion:

```glsl
vec4 eye_dir = InvProjection * vec4(FragCoord, 0.0, 1.0);
eye_dir.xyz /= eye_dir.w;
vec3 ro = InvView[3].xyz;
vec3 rd = normalize((InvView * vec4(normalize(eye_dir.xyz), 0.0)).xyz);
```

To write depth (for future compositing with geometry):

```glsl
vec3 hitPos = ro + rd * distance;
vec4 clip = Projection * View * vec4(hitPos, 1.0);
gl_FragDepth = clip.z / clip.w;
```

See `example.glsl`, `kaleidoscopic-ifs.glsl`, and `mandelbox.glsl` for complete examples.

## Dependencies

Managed via vcpkg (submoduled):

- Vulkan 1.3
- VMA (Vulkan Memory Allocator)
- SDL3 (windowing, input)
- OpenXR (HMD support via `XR_KHR_vulkan_enable`)
- shaderc (runtime GLSL to SPIR-V)
- Dear ImGui (debug UI)
- GLM (math, with `GLM_FORCE_DEPTH_ZERO_TO_ONE`)
- spdlog (logging)
- argparse (CLI)
