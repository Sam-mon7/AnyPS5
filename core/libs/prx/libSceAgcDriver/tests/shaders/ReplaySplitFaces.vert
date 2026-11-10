#version 450
out gl_PerVertex { vec4 gl_Position; };
void main() {
    const vec2 positions[6] = vec2[6](
        vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0),
        vec2(-1.0, -1.0), vec2(-1.0, 3.0), vec2(3.0, -1.0));
    gl_Position = vec4(positions[gl_VertexIndex], 0.5, 1.0);
}
