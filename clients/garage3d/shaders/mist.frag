#version 300 es
// mist.frag - soft volumetric-looking mist from 2 sine octaves. True
// volumetrics (ray marching) would be absurd for four translucent quads on
// a phone; an animated value field with soft quad edges sells the mockup's
// purple doorway haze at negligible cost. Drawn additive, depth-write off.
precision highp float;

in vec2 v_uv;
out vec4 frag_color;

uniform vec3 u_color;
uniform float u_time;
uniform float u_seed;

void main() {
    vec2 uv = v_uv;
    float edge = sin(3.14159265 * uv.x) * sin(3.14159265 * uv.y);
    float field = 0.55
        + 0.28 * sin(uv.x * 9.0 + u_seed * 7.0 + u_time * 0.23)
        * sin(uv.y * 5.0 - u_time * 0.17 + u_seed);
    field += 0.17 * sin(uv.x * 23.0 - u_time * 0.31 + u_seed * 3.0)
        * sin(uv.y * 13.0 + u_time * 0.11);
    float alpha = clamp(edge, 0.0, 1.0) * clamp(field, 0.0, 1.0) * 0.16;
    frag_color = vec4(u_color * alpha, alpha);
}
