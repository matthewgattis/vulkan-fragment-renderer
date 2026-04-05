#version 450

layout(location = 0) in vec2 FragCoord;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform FrameUbo {
    mat4 View;
    mat4 Projection;
    vec4 Resolution;
    float Time;
};

layout(set = 1, binding = 0) uniform RayUbo {
    mat4 InvView;
    mat4 InvProjection;
};

layout(push_constant) uniform PushConstants {
    mat4 Model;
};

// --- Ray marching utilities ---

float sdBox(vec3 p, vec3 b) {
    vec3 d = abs(p) - b;
    return min(max(d.x, max(d.y, d.z)), 0.0) + length(max(d, 0.0));
}

float scene(vec3 p) {
    vec3 q = mod(p + 4.0, 8.0) - 4.0;
    float d = sdBox(q, vec3(0.8));
    d = min(d, sdBox(q - vec3(1.5, 0.0, 0.0), vec3(0.4)));
    return d;
}

vec3 calcNormal(vec3 p) {
    vec2 e = vec2(0.001, 0.0);
    return normalize(vec3(
        scene(p + e.xyy) - scene(p - e.xyy),
        scene(p + e.yxy) - scene(p - e.yxy),
        scene(p + e.yyx) - scene(p - e.yyx)));
}

// Inigo Quilez cosine palette
vec3 palette(float t) {
    vec3 a = vec3(0.5);
    vec3 b = vec3(0.5);
    vec3 c = vec3(1.0);
    vec3 d = vec3(0.00, 0.33, 0.67);
    return a + b * cos(6.28318 * (c * t + d));
}

void main() {
    // Ray direction from precomputed inverse matrices — handles asymmetric
    // XR frustums and avoids per-fragment matrix inversion.
    vec4 eye_dir = InvProjection * vec4(FragCoord, 0.0, 1.0);
    eye_dir.xyz /= eye_dir.w;

    vec3 ro = InvView[3].xyz;
    vec3 rd = normalize((InvView * vec4(normalize(eye_dir.xyz), 0.0)).xyz);

    // Ray march
    float t = 0.0;
    float d;
    for (int i = 0; i < 64; i++) {
        d = scene(ro + rd * t);
        if (d < 0.001) break;
        t += d;
        if (t > 100.0) break;
    }

    vec3 col = vec3(0.0);
    if (d < 0.01) {
        vec3 p = ro + rd * t;
        vec3 n = calcNormal(p);
        vec3 light = normalize(vec3(1.0, 3.0, 2.0));
        float diff = max(dot(n, light), 0.0);
        float halfLambert = diff * 0.5 + 0.5;
        col = palette(t * 0.05 + Time * 0.1) * halfLambert;
    }

    // Fog
    float fog = 1.0 - exp(-t * 0.03);
    col = mix(col, vec3(0.02, 0.02, 0.05), fog);

    outColor = vec4(col, 1.0);

    // Write depth: project hit point into clip space
    vec3 hitPos = ro + rd * t;
    vec4 clip = Projection * View * vec4(hitPos, 1.0);
    gl_FragDepth = clip.z / clip.w;
}
