#version 300 es
// particle.vert - rain streaks. The CPU physics (animation.c) owns the
// particle positions; renderer.c expands each particle into a short line
// segment (pos -> pos - vel*streak) and streams it with GL_DYNAMIC_DRAW.
// Lines, not points: gl_PointSize is capped differently across mobile GPUs
// and round sprites don't read as rain - a velocity-aligned streak does.
layout(location = 0) in vec3 a_pos;

uniform mat4 u_mvp;

void main() {
    gl_Position = u_mvp * vec4(a_pos, 1.0);
}
