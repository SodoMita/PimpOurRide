#version 120
// Hermite (Mitchell-Netravali B = 0, C = 0) downscale, mipmap-assisted
// approximation, as a ChromeDrive post shader.
//
// The plain variant (shaders/hermite-downscale.frag) fetches every source texel
// in the footprint, which grows ~ratio^2. Here the hardware mip chain does the
// inner averaging and a fixed 7x7 grid of level-l texels carries the outer
// Hermite weights. It is an approximation, not the exact separable gather: the
// mip box average is not a Hermite inner filter, so very high-contrast detail
// can ring slightly differently.
//
// Level choice: l = clamp(floor(log2(rmax)) - 1, 0, 8). The "full footprint"
// level for a ratio r is about log2(r); using one level sharper leaves the
// Hermite kernel a support of roughly 2..4 level-l texels per output pixel,
// while the 7x7 (k = -3..3) grid covers the central part of that support and
// zero weights discard the rest. step = exp2(l) source texels per level-l
// texel.
//
// Sampling bias: the fullscreen quad's implicit LOD is ~log2(ratio), the
// standard situation for a post pass that covers the whole target. The taps
// ask for bias = l - log2(rmax), so the resolved LOD is l regardless of the
// actual ratio (assuming the implicit derivative estimate matches the ratio).
//
// Needs a mipmap render_downscale filter on u_world (the default
// linear_mipmap_linear); with a non-mipmap filter the bias is ignored and the
// samples expose unfiltered full-resolution texels. Fixed cost: 49 taps per
// output pixel, no support cap and no ratio^2 blow-up.
//
// Use with render_scale > 1 and
// post_shader = shaders/hermite-downscale-mipmap.frag. At ratio <= 1 it passes
// u_src through unchanged.
//
// Loop bounds are constant, as required by GLSL 1.20.
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

    // One mip level sharper than the nominal full footprint.
    float l = clamp(floor(log2(rmax)) - 1.0, 0.0, 8.0);
    float step = exp2(l);

    // Work in level-l texel coordinates. c is continuous and texel centres are
    // at integers; base is the containing level-l texel.
    vec2 c = uv * u_world_size / step - 0.5;
    vec2 base = floor(c);
    float bias = l - log2(rmax);

    vec4 sum = vec4(0.0);
    float W = 0.0;
    for (int j = -3; j <= 3; j++) {
        float fj = float(j);
        for (int i = -3; i <= 3; i++) {
            float fi = float(i);
            vec2 tap = base + vec2(fi, fj);
            vec2 rel = ((tap - c) * step) / ratio;
            float w = hermite(rel.x) * hermite(rel.y);
            if (w == 0.0)
                continue;
            vec2 suv = clamp(((tap + 0.5) * step) / u_world_size,
                             vec2(0.0), vec2(1.0));
            sum += w * texture2D(u_world, suv, bias);
            W += w;
        }
    }
    gl_FragColor = sum / W;
}
