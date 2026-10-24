#version 300 es
// lit.vert - vertex stage for every solid surface (walls, floor, props).
// Attribute locations are fixed-by-convention and must match renderer.c /
// mesh.h: 0=position, 1=normal, 2=uv (interleaved pos/normal/uv).
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;

uniform mat4 u_mvp;
uniform mat4 u_model;
uniform mat3 u_normal_matrix; // inverse-transpose of model, computed CPU-side

out vec3 v_world_pos;
out vec3 v_normal;
out vec3 v_local_pos; // for attached-surface patterns (rolling balls)
out vec2 v_uv;

void main() {
    vec4 world = u_model * vec4(a_pos, 1.0);
    v_world_pos = world.xyz;
    // The inverse-transpose (not mat3(model)) is required because nodes use
    // non-uniform scale - a pillar scaled (1.3, 8.5, 1.1) would otherwise
    // get visibly skewed lighting.
    v_normal = normalize(u_normal_matrix * a_normal);
    v_local_pos = a_pos;
    v_uv = a_uv;
    gl_Position = u_mvp * vec4(a_pos, 1.0);
}
