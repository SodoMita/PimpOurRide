#version 120
// Khronos PBR Neutral tone mapping.
//
// Algorithm: KhronosGroup/ToneMapping, "PBR Neutral" tone mapper,
// Apache-2.0. See https://github.com/KhronosGroup/ToneMapping
// This GLSL implementation is written for ChromeDrive's post_shader contract.
//
// Honest caveat: ChromeDrive's 3D scene is rendered display-referred LDR
// (0..1), not scene-linear HDR, so this is not doing PBR Neutral's intended
// HDR-to-LDR range compression here. It acts as a gentle highlight rolloff
// and demonstrates the runtime post-shader framework.
//
// Contract: fragment-only GLSL 1.20. ChromeDrive supplies the vertex shader
// and sets gl_TexCoord[0].xy. Uniforms:
//   sampler2D u_src        - world image at output size
//   vec2 u_size            - output size in pixels
//   float u_time           - seconds
//   sampler2D u_world      - scaled world image at native render size (unit 1)
//   vec2 u_world_size      - render size in pixels
// The result must be written to gl_FragColor.

uniform sampler2D u_src;
uniform vec2 u_size;
uniform float u_time;

vec3 pbr_neutral(vec3 color) {
    float start_compression = 0.8 - 0.04;
    float desaturation = 0.15;
    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;
    float peak = max(color.r, max(color.g, color.b));
    if (peak < start_compression) {
        return color;
    }
    float d = 1.0 - start_compression;
    float new_peak = 1.0 - d * d / (peak + d - start_compression);
    color *= new_peak / peak;
    float g = 1.0 - 1.0 / (desaturation * (peak - new_peak) + 1.0);
    return mix(color, vec3(new_peak), g);
}

void main() {
    vec4 src = texture2D(u_src, gl_TexCoord[0].xy);
    gl_FragColor = vec4(pbr_neutral(src.rgb), src.a);
}
