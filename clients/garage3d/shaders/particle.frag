#version 300 es
// particle.frag - flat pale-blue rain, additive over the city backdrop.
precision highp float;

out vec4 frag_color;

uniform vec4 u_color;

void main() {
    frag_color = u_color;
}
