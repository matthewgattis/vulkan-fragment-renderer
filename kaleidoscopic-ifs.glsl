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

#define M_PI (3.1415926535897932384626433832795)

#define MIN_DELTA       (0.01 / Resolution.y)
#define MAX_DELTA       (2.0 / Resolution.y)
#define MAX_DISTANCE    (8.0)
#define MAX_ITERATIONS  (48)

struct surface {
    int object;
    int iteration;
};

vec3 vRotateX(vec3 p, float angle) {
    float c = cos(angle), s = sin(angle);
    return vec3(p.x, c*p.y+s*p.z, -s*p.y+c*p.z);
}

vec3 vRotateY(vec3 p, float angle) {
    float c = cos(angle), s = sin(angle);
    return vec3(c*p.x-s*p.z, p.y, s*p.x+c*p.z);
}

vec3 vRotateZ(vec3 p, float angle) {
    float c = cos(angle), s = sin(angle);
    return vec3(c*p.x+s*p.y, -s*p.x+c*p.y, p.z);
}

// Kaleidoscopic (escape time) IFS — knighty
#define FRACT_ITER      (22)
#define FRACT_SCALE     (1.8)
#define FRACT_OFFSET    (1.0)

float DE(vec3 z) {
    for (int n = 0; n < FRACT_ITER; n++) {
        float rotate = M_PI * 0.5;
        z = vRotateX(z, rotate);
        z = vRotateY(z, rotate);
        z = vRotateZ(z, rotate);

        z.xy = abs(z.xy);
        if (z.x+z.y<0.0) z.xy = -z.yx;
        if (z.x+z.z<0.0) z.xz = -z.zx;
        if (z.y+z.z<0.0) z.zy = -z.yz;
        z = z*FRACT_SCALE - FRACT_OFFSET*(FRACT_SCALE-1.0);
    }
    return length(z) * pow(FRACT_SCALE, -float(FRACT_ITER));
}

float getMap(in vec3 p, out surface o) {
    o = surface(0, 1);
    return DE(p);
}

vec3 getNormal(in vec3 p) {
    float h = MIN_DELTA;
    surface o;
    return normalize(vec3(
        getMap(p + vec3(h, 0.0, 0.0), o) - getMap(p - vec3(h, 0.0, 0.0), o),
        getMap(p + vec3(0.0, h, 0.0), o) - getMap(p - vec3(0.0, h, 0.0), o),
        getMap(p + vec3(0.0, 0.0, h), o) - getMap(p - vec3(0.0, 0.0, h), o)));
}

float castRay(in vec3 p, in vec3 d, out surface o) {
    float distance = 0.0;
    for (int i = 0; i < MAX_ITERATIONS; i++) {
        float delta = getMap(p + distance * d, o);
        distance += delta;
        if (distance > MAX_DISTANCE) { o.object = 0; o.iteration = i; return MAX_DISTANCE; }
        float m = mix(MIN_DELTA, MAX_DELTA, distance / 4.0);
        if (delta < m) { o.iteration = i; return distance; }
    }
    o.object = 0;
    o.iteration = MAX_ITERATIONS;
    return MAX_DISTANCE;
}

// Inigo Quilez palette
vec3 pal(in float t, in vec3 a, in vec3 b, in vec3 c, in vec3 d) {
    return a + b*cos(6.28318*(c*t+d));
}

void main() {
    // Ray direction from precomputed inverse matrices — handles asymmetric
    // XR frustums and avoids per-fragment matrix inversion.
    vec4 eye_dir = InvProjection * vec4(FragCoord, 0.0, 1.0);
    eye_dir.xyz /= eye_dir.w;

    vec3 origin = InvView[3].xyz;
    vec3 direction = normalize((InvView * vec4(normalize(eye_dir.xyz), 0.0)).xyz);

    surface object;
    float dist = castRay(origin, direction, object);

    float cheap_ao = 1.0 - float(object.iteration) / float(MAX_ITERATIONS);

    vec3 color = pal(
        pow(cheap_ao, 0.8),
        vec3(0.5), vec3(0.5), vec3(1.0), vec3(0.0, 0.10, 0.20));
    color = mix(vec3(0.0), color, cheap_ao);

    outColor = vec4(color, 1.0);

    // Write depth
    vec3 hitPos = origin + direction * dist;
    vec4 clip = Projection * View * vec4(hitPos, 1.0);
    gl_FragDepth = clip.z / clip.w;
}
