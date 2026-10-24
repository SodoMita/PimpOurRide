#version 300 es
// lit.frag - Blinn-Phong with 1 directional + 6 point lights, exponential
// fog, procedural concrete panelling and a rippled wet-floor highlight.
//
// Why Blinn-Phong and not full PBR: this scene is dark, stylised, and must
// hold 60 fps on mobile GPUs and llvmpipe alike. One specular lobe plus
// strong coloured lights reproduces the mockup's look at a fraction of the
// cost of GGX + environment probes, and there are no textures to sample -
// surface detail is procedural (grid seams + value noise), so the client
// ships zero image assets.
precision highp float;

in vec3 v_world_pos;
in vec3 v_normal;
in vec3 v_local_pos;
in vec2 v_uv;
out vec4 frag_color;

uniform vec3 u_albedo;
uniform float u_alpha;
uniform vec3 u_emissive;
uniform float u_emissive_scale;
uniform float u_specular;
uniform float u_shininess;
uniform float u_wet;      // 1.0 on the floor
uniform float u_pattern;  // 1.0 on the physics balls (visible rotation)
uniform float u_time;

uniform vec3 u_camera_pos;
uniform vec3 u_dir_dir;   // direction the directional light travels
uniform vec3 u_dir_color;
uniform float u_dir_intensity;

#define MAX_POINTS 6
uniform vec3 u_point_pos[MAX_POINTS];
uniform vec3 u_point_color[MAX_POINTS];
uniform float u_point_intensity[MAX_POINTS];

uniform vec3 u_fog_color;
uniform float u_fog_density;

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

// Concrete panel seams every 2 m on the surface's dominant plane, plus a
// whisper of per-panel value variation. fwidth() (core in ES 3.0) keeps the
// seams anti-aliased instead of shimmering at grazing angles.
float concrete_detail(vec3 wp, vec3 n) {
    vec2 plane_uv = abs(n.y) > 0.6 ? wp.xz : (abs(n.x) > 0.6 ? wp.zy : wp.xy);
    vec2 grid = plane_uv / 3.0; // 3 m panels, like the poured slabs in the photo
    vec2 g = abs(fract(grid - 0.5) - 0.5) / fwidth(grid);
    float seam = 1.0 - min(min(g.x, g.y), 1.0);
    float panel = 0.95 + 0.05 * hash12(floor(grid));
    return panel * (1.0 - seam * 0.16);
}

// Diffuse is tinted by the albedo; specular is NOT (a wet dark floor must
// still throw a bright neon-coloured streak - multiplying the specular by
// a 0.05 albedo would kill exactly the effect the mockup is famous for).
vec3 light_contrib(vec3 albedo, vec3 n, vec3 view_dir, vec3 light_dir,
                   vec3 color, float intensity) {
    float diff = max(dot(n, light_dir), 0.0);
    vec3 half_dir = normalize(light_dir + view_dir);
    float spec = pow(max(dot(n, half_dir), 0.0), u_shininess) * u_specular;
    return color * intensity * (albedo * diff + spec);
}

void main() {
    vec3 n = normalize(v_normal);
    vec3 view_dir = normalize(u_camera_pos - v_world_pos);
    // Two-sided planes (walls/ceiling are single quads): flip the normal
    // when we see the back face so lighting never goes black from behind.
    if (dot(n, view_dir) < 0.0) n = -n;

    // Wet floor: perturb the up-normal with two moving sine ripples. This
    // breaks the point-light specular into the streaky smear the mockup
    // shows, without any normal-map texture.
    if (u_wet > 0.5) {
        vec2 ripple = vec2(
            sin(v_world_pos.x * 6.1 + u_time * 0.7) * 0.035 +
            sin(v_world_pos.z * 9.3 - u_time * 0.5) * 0.025,
            sin(v_world_pos.z * 5.2 - u_time * 0.6) * 0.035 +
            sin(v_world_pos.x * 8.1 + u_time * 0.4) * 0.025);
        n = normalize(n + vec3(ripple.x, 0.0, ripple.y));
    }

    vec3 albedo = u_albedo * concrete_detail(v_world_pos, n);
    if (u_pattern > 0.5) {
        // Latitude band + cap dot, attached to the (unit) local sphere, so
        // rolling rotation integrated by the physics is actually visible.
        float band = smoothstep(0.04, 0.09, abs(v_local_pos.y));
        float caps = smoothstep(0.30, 0.36, abs(v_local_pos.x));
        albedo *= mix(0.45, 1.0, min(band, 0.35 + 0.65 * caps));
    }

    // Ambient: a very low blue-violet base so shadowed concrete is never
    // pure black (the mockup's shadows are deep purple, not void).
    vec3 result = albedo * vec3(0.07, 0.07, 0.12);

    // Directional light: city-night glow entering through the far opening.
    result += light_contrib(albedo, n, view_dir, normalize(-u_dir_dir),
                            u_dir_color, u_dir_intensity);

    // Point lights with physical-style inverse-square attenuation, softened
    // by +1.0 to avoid a singularity at the tube surface:
    //   att = intensity / (1 + 0.08 d + 0.08 d^2)
    // The quadratic term is what makes the neon splash fall off across the
    // 22 m hall - teal genuinely dies before the right wall, as in the photo.
    for (int i = 0; i < MAX_POINTS; i++) {
        vec3 to_light = u_point_pos[i] - v_world_pos;
        float dist = length(to_light);
        float att = u_point_intensity[i] / (1.0 + 0.08 * dist + 0.08 * dist * dist);
        if (att < 0.003) continue; // beyond useful range: skip the math
        result += light_contrib(albedo, n, view_dir, to_light / dist,
                                u_point_color[i], att);
    }

    result += u_emissive * u_emissive_scale;

    // Exponential height-less fog towards the deep violet of the opening.
    float fog_dist = length(u_camera_pos - v_world_pos);
    float fog_f = 1.0 - exp(-u_fog_density * fog_dist);
    result = mix(result, u_fog_color, clamp(fog_f, 0.0, 1.0));

    frag_color = vec4(result, u_alpha);
}
