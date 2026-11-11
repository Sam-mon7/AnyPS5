#version 450
layout(set = 0, binding = 0, std430) readonly buffer Guest { uint words[]; } guest[1];
layout(set = 0, binding = 1, std430) readonly buffer Srt { uint words[]; } srt;
layout(set = 0, binding = 2, std430) readonly buffer Data { uint words[]; } data;
layout(location = 0) flat out vec3 color;
out gl_PerVertex { vec4 gl_Position; };
void main() {
    vec2 position = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(position * 2.0 - 1.0, 0.5, 1.0);
    uint value = guest[0].words[(data.words[0] & 0xffu) / 4u];
    color = vec3(float(value & 0xffu), float((value >> 8u) & 0xffu), float(srt.words[0] & 0xffu)) / 255.0;
}
