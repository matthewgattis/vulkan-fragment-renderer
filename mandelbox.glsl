#version 450

layout(location = 0) in vec2 FragCoord;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform FrameUbo {
    mat4 View;
    mat4 Projection;
    vec4 Resolution;
    float Time;
};

layout(push_constant) uniform PushConstants {
    mat4 Model;
};

#define MAX_DISTANCE    (48.0)
#define MIN_DELTA       (0.005 / Resolution.y)
#define MAX_DELTA       (0.5 / Resolution.y)
#define MAX_ITERATIONS  (128)

struct surface {
    int object;
    int iteration;
    float orbit;
};

// Mandelbox
#define DE3_ITER            (32)
#define DE3_SCALE           (2.0)
#define DE3_MIN_RADIUS      (0.5)
#define DE3_FIXED_RADIUS    (1.0)
#define DE3_FOLDING_LIMIT   (1.0)

void sphereFold(inout vec3 z, inout float dz) {
    float r2 = dot(z, z);
    if (r2 < DE3_MIN_RADIUS) {
        float temp = DE3_FIXED_RADIUS / DE3_MIN_RADIUS;
        z *= temp; dz *= temp;
    } else if (r2 < DE3_FIXED_RADIUS) {
        float temp = DE3_FIXED_RADIUS / r2;
        z *= temp; dz *= temp;
    }
}

void boxFold(inout vec3 z) {
    vec3 limit = vec3(DE3_FOLDING_LIMIT);
    z = clamp(z, -limit, limit) * 2.0 - z;
}

float DE3(vec3 z, out float orbit) {
    vec3 offset = z;
    float dr = 1.0;
    float min_dist = -1e9;

    for (int n = 0; n < DE3_ITER; n++) {
        boxFold(z);
        sphereFold(z, dr);
        z = DE3_SCALE * z + offset;
        dr = dr * abs(DE3_SCALE) + 1.0;
        min_dist = max(min_dist, length(z));
    }

    orbit = min_dist;
    return length(z) / abs(dr);
}

float getMap(in vec3 p, out surface o) {
    float orbit;
    float d = DE3(p, orbit);
    o = surface(0, 1, orbit);
    return d;
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

vec3 pal(in float t, in vec3 a, in vec3 b, in vec3 c, in vec3 d) {
    return a + b*cos(6.28318*(c*t+d));
}

void main() {
    vec2 uv = FragCoord * vec2(Resolution.z, 1.0);

    mat4 inv = inverse(View);
    vec3 origin = inv[3].xyz;
    vec3 direction = normalize((inv * vec4(normalize(vec3(uv, -2.0)), 0.0)).xyz);

    surface object;
    float dist = castRay(origin, direction, object);

    float cheap_ao = 1.0 - float(object.iteration) / float(MAX_ITERATIONS);

    vec3 color = pal(
        pow(cheap_ao, 0.8),
        vec3(0.5), vec3(0.5), vec3(1.0), vec3(0.3, 0.20, 0.20));
    color = mix(vec3(0.0), color, cheap_ao);
    color = pow(clamp(color, vec3(0.0), vec3(1.0)), vec3(1.0 / 2.2));

    outColor = vec4(color, 1.0);

    // Write depth
    vec3 hitPos = origin + direction * dist;
    vec4 clip = Projection * View * vec4(hitPos, 1.0);
    gl_FragDepth = clip.z / clip.w;
}
