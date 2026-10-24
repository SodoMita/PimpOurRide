#version 300 es
// city.frag - the rainy night skyline seen through the far opening,
// generated entirely procedurally (no skyline texture to ship):
//   sky: vertical deep-blue -> violet gradient with a horizon glow
//   buildings: a 1D hash assigns each column a roof height (two depth
//              layers for parallax-ish silhouette variety)
//   windows: a per-cell hash on a fine grid; a fraction are lit, warm or
//              cool, and a few twinkle by re-hashing against u_time buckets
// This replaces the city region that was baked into garage.webp.
precision highp float;

in vec2 v_uv;
out vec4 frag_color;

uniform float u_time;

float hash11(float p) {
    p = fract(p * 0.1031);
    p *= p + 33.33;
    p *= p + p;
    return fract(p);
}
float hash21(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

// One skyline layer: returns building colour with windows for the columns.
vec3 skyline_layer(vec2 uv, float columns, float base, float tall,
                   vec3 silhouette, float lit_fraction, float seed) {
    float col = floor(uv.x * columns);
    float roof = base + tall * hash11(col * 1.37 + seed);
    if (uv.y > roof) return vec3(-1.0); // sentinel: sky here
    vec3 col3 = silhouette;
    // Window grid inside the building footprint.
    vec2 cell = vec2(uv.x * columns * 3.0, uv.y * 90.0);
    vec2 cell_id = floor(cell);
    vec2 f = fract(cell);
    float h = hash21(cell_id + seed);
    bool lit = h < lit_fraction;
    // Twinkle: cells near the threshold blink on a slow time bucket.
    float bucket = floor(u_time * 0.5 + h * 40.0);
    if (abs(h - lit_fraction) < 0.02) lit = hash11(bucket + h * 91.0) > 0.5;
    if (lit && f.x > 0.25 && f.x < 0.75 && f.y > 0.3 && f.y < 0.7) {
        vec3 warm = vec3(1.0, 0.72, 0.38);
        vec3 cool = vec3(0.65, 0.85, 1.0);
        col3 = mix(warm, cool, step(0.6, hash21(cell_id + 7.0))) * (0.55 + 0.45 * h / max(lit_fraction, 0.001));
    }
    return col3;
}

void main() {
    vec2 uv = v_uv;
    // Sky gradient + a soft sodium/city glow near the horizon.
    vec3 sky = mix(vec3(0.10, 0.13, 0.28), vec3(0.015, 0.03, 0.10), smoothstep(0.1, 0.95, uv.y));
    sky += vec3(0.20, 0.14, 0.05) * pow(max(0.0, 1.0 - uv.y * 2.6), 2.0);
    // Rare faint stars / distant aircraft in the upper sky.
    float star = step(0.9985, hash21(floor(uv * vec2(320.0, 180.0))));
    sky += star * smoothstep(0.55, 0.95, uv.y) * 0.5;

    vec3 result = sky;
    vec3 far_layer = skyline_layer(uv, 26.0, 0.18, 0.34, vec3(0.07, 0.09, 0.19), 0.22, 3.1);
    if (far_layer.r >= 0.0) result = far_layer;
    vec3 near_layer = skyline_layer(uv, 15.0, 0.05, 0.30, vec3(0.03, 0.04, 0.09), 0.30, 8.7);
    if (near_layer.r >= 0.0) result = near_layer;

    // Ground haze at the very bottom, blending into the mist sheets.
    result = mix(result, vec3(0.16, 0.12, 0.28), smoothstep(0.16, 0.0, uv.y) * 0.6);
    frag_color = vec4(result, 1.0);
}
