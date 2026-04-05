#version 450

// Generates a fullscreen triangle with no vertex buffer.
// Vertex IDs 0, 1, 2 produce a triangle covering the entire screen.

layout(location = 0) out vec2 FragCoord;

void main() {
    // Generate positions: (-1,-1), (3,-1), (-1,3)
    vec2 pos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);

    // Output normalized coordinates for the fragment shader.
    // FragCoord ranges from (-1,-1) to (1,1) in Vulkan NDC.
    // The projection matrix handles Y-flip via negated [1][1].
    FragCoord = pos * 2.0 - 1.0;
}
