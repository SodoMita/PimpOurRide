#version 120
// Hermite (Mitchell-Netravali B = 0, C = 0) downscale as a ChromeDrive post
// shader.
//
// ImageMagick calls this kernel "Hermite": w(x) = (2*|x| - 3)*x*x + 1 for
// |x| < 1, zero otherwise. It is an interpolating, non-negative cubic with a
// one-output-pixel support, so the exact gather is a separable 2D footprint
// centred continuously between source texels rather than on a texel corner.
//
// This is the plain/exact variant: every source texel inside the footprint is
// fetched at LOD 0, weighted, and normalized by the weight sum (alpha uses the
// same weights). Cost grows ~ratio^2 per output pixel, so use it for inspection
// or modest supersampling, not as a general-purpose filter. Above a 4x maximum
// ratio it returns the already-resolved u_src image instead of running the
// footprint loop.
//
// Use with render_scale > 1 and
// post_shader = shaders/hermite-downscale.frag. At ratio <= 1 it passes u_src
// through unchanged.
//
// LOD-0 trick: strict GLSL 1.20 has no textureLod and no way to change the
// sampler's state from inside a shader. The footprint therefore uses a huge
// negative bias on texture2D, which clamps the computed LOD to the base level
// on conformant drivers (Mesa and the GL spec's bias semantics). If a driver
// ignored the bias under minification the taps would come from a mip level and
// the result would be prefiltered twice.
//
// Loop bounds are constant with early breaks, as required by GLSL 1.20.
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

// Mitchell-Netravali B = 0, C = 0 ("Hermite"): interpolating cubic,
// non-negative on [-1, 1], zero outside.
float hermite(float x) {
    float ax = abs(x);
    if (ax < 1.0)
        return (2.0 * ax - 3.0) * x * x + 1.0;
    return 0.0;
}

void main() {
    vec2 uv = gl_TexCoord[0].xy;
    vec2 ratio = u_world_size / u_size;
    float rmax = max(ratio.x, ratio.y);
    if (rmax <= 1.0) {
        gl_FragColor = texture2D(u_src, uv);
        return;
    }
    if (rmax > 4.0) {
        gl_FragColor = texture2D(u_src, uv);
        return;
    }

    // Continuous texel alignment: uv = 0 maps to the left edge of texel 0, so
    // the texel centre is at integer coordinates.
    vec2 centre = uv * u_world_size - 0.5;
    vec2 base = floor(centre);
    // One output-pixel support, widened by the ratio when measured in source
    // texels: rel = (tap - centre) / ratio. ceil() covers the fractional part.
    float rx = ceil(ratio.x);
    float ry = ceil(ratio.y);

    vec4 sum = vec4(0.0);
    float W = 0.0;
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
            vec2 tap = base + vec2(fi, fj);
            vec2 rel = (tap - centre) / ratio;
            float w = hermite(rel.x) * hermite(rel.y);
            if (w == 0.0)
                continue;
            vec2 suv = clamp((tap + 0.5) / u_world_size, vec2(0.0), vec2(1.0));
            sum += w * texture2D(u_world, suv, -16.0);
            W += w;
        }
    }
    gl_FragColor = sum / W;
}
