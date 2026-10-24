#version 300 es
// neon.frag - one shader, two modes:
//   u_halo = 0: solid emissive core (the glass tube itself, bulb, reflections)
//   u_halo = 1: soft additive glow quad. Alpha falls off across the quad's
//               width and fades at the ends, faking the bloom a real HDR
//               pipeline would produce - at zero post-processing cost, which
//               matters on mobile tile GPUs.
// Drawn with additive blending and depth-write off (see renderer.c), so
// overlapping halos accumulate light instead of occluding each other.
precision highp float;

in vec2 v_uv;
out vec4 frag_color;

uniform vec3 u_color;         // material emissive (can exceed 1.0)
uniform float u_emissive_scale; // flicker/pulse/reflection dimming
uniform float u_halo;

void main() {
    vec3 color = u_color * u_emissive_scale;
    float alpha = 1.0;
    if (u_halo > 0.5) {
        float across = 1.0 - abs(v_uv.x - 0.5) * 2.0;
        float along = sin(3.14159265 * clamp(v_uv.y, 0.0, 1.0));
        alpha = pow(max(across, 0.0), 2.2) * pow(max(along, 0.0), 0.7) * 0.20;
    }
    // Premultiplied-style output for GL_ONE, GL_ONE additive blending.
    frag_color = vec4(color * alpha, alpha);
}
