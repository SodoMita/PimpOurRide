#version 120
// SSIM-guided downscale as a ChromeDrive post shader.
//
// Adapted from igv's SSimDownscaler (mpv user shader, LGPL-3.0-or-later).
// This is our own single-pass GLSL approximation, not a copy of the mpv hook
// directives or dispatch machinery. The original resolves in multiple passes
// (separable L2, mean/R statistics, then a second smoothing); here the L2
// footprint and the 3x3 locality statistics are combined in one fragment
// pass, so the R ratio is only smoothed once through the 3x3 neighbourhood.
//
// Use with render_scale > 1 and a mipmap render_downscale filter (the default
// linear_mipmap_linear). u_world must be allowed to choose an implicit mip
// level so the base L samples are properly prefiltered; with a non-mipmap
// min filter this degrades to an unfiltered full-resolution sample.
// At ratio <= 1 the shader passes u_src through.
//
// LOD-0 trick: strict GLSL 1.20 has no textureLod and no way to change the
// sampler's state from inside a shader. The E[x^2] footprint therefore uses a
// huge negative bias on texture2D, which clamps the computed LOD to the base
// level on conformant drivers (Mesa and the GL spec's bias semantics). If a
// driver ignored the bias under minification the footprint would be sampled
// from a mip level and contrast restoration would be too weak.
//
// Support cap: above a 4x maximum ratio the shader returns the centre base
// sample instead of running the footprint loop. The loop bounds are constant
// with early breaks, as required by GLSL 1.20.
//
// Contract: ChromeDrive supplies the vertex shader and gl_TexCoord[0].xy.
//   sampler2D u_src        - resolved world image at output size
//   vec2 u_size            - output size in pixels
//   float u_time           - seconds
//   sampler2D u_world      - scaled world image at render size (unit 1)
//   vec2 u_world_size      - render size in pixels
// The result is written to gl_FragColor.

uniform sampler2D u_src;
uniform vec2 u_size;
uniform float u_time;
uniform sampler2D u_world;
uniform vec2 u_world_size;

float luma(vec3 c) {
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

// Mitchell-Netravali kernel with B = 0, C = 0.5 (Catmull-Rom).
float mn(float b, float c, float x) {
    if (x < 1.0)
        return ((2.0 - 1.5 * b - c) * x + (-3.0 + 2.0 * b + c)) * x * x + (1.0 - b / 3.0);
    return (((-b / 6.0 - c) * x + (b + 5.0 * c)) * x + (-2.0 * b - 8.0 * c)) * x + (4.0 / 3.0 * b + 4.0 * c);
}

void main() {
    vec2 uv = gl_TexCoord[0].xy;
    vec2 ratio = u_world_size / u_size;
    float rmax = max(ratio.x, ratio.y);
    if (rmax <= 1.0) {
        gl_FragColor = texture2D(u_src, uv);
        return;
    }

    vec4 centre = texture2D(u_world, uv);
    if (rmax > 4.0) {
        gl_FragColor = centre;
        return;
    }

    // Local 3x3 statistics over output pixels, weights (1/2)^(|i|+|j|).
    vec3 m = vec3(0.0), m2 = vec3(0.0);
    float W = 0.0;
    for (int j = -1; j <= 1; j++) {
        for (int i = -1; i <= 1; i++) {
            vec2 suv = clamp(uv + vec2(float(i), float(j)) / u_size, vec2(0.0), vec2(1.0));
            vec3 L = texture2D(u_world, suv).rgb;
            float w = exp2(-(abs(float(i)) + abs(float(j))));
            m += w * L;
            m2 += w * L * L;
            W += w;
        }
    }
    m /= W;
    m2 /= W;
    float Sl = luma(max(m2 - m * m, vec3(0.0)));

    // E[x^2] over the source footprint at LOD 0, Catmull-Rom taps = 2 output px.
    vec3 l2 = vec3(0.0);
    float W2 = 0.0;
    float rx = ceil(2.0 * ratio.x);
    float ry = ceil(2.0 * ratio.y);
    vec2 centre_texel = uv * u_world_size - 0.5;
    vec2 base_texel = floor(centre_texel);
    for (int j = -9; j <= 9; j++) {
        float fj = float(j);
        if (fj < -ry)
            continue;
        if (fj > ry)
            break;
        for (int i = -9; i <= 9; i++) {
            float fi = float(i);
            if (fi < -rx)
                continue;
            if (fi > rx)
                break;
            vec2 tap = base_texel + vec2(fi, fj);
            vec2 suv = clamp((tap + 0.5) / u_world_size, vec2(0.0), vec2(1.0));
            vec2 rel = (tap - centre_texel) / ratio;
            float w = mn(0.0, 0.5, abs(rel.x)) * mn(0.0, 0.5, abs(rel.y));
            vec4 s = texture2D(u_world, suv, -16.0);
            l2 += w * s.rgb * s.rgb;
            W2 += w;
        }
    }
    l2 /= W2;
    float Sh = luma(max(l2 - m * m, vec3(0.0)));

    float sigma = 10.0 / 65025.0;
    float R = (Sl > Sh) ? clamp(Sh / Sl, 0.0, 1.0) : sqrt((Sh + sigma) / (Sl + sigma));
    gl_FragColor = vec4(m + R * (centre.rgb - m), centre.a);
}
