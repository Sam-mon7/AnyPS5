#version 450
#ifdef STENCIL
#extension GL_ARB_shader_stencil_export : require
#endif

#ifdef REPLAY_MOVED
layout(std430, binding = 0) readonly buffer Values {
    uint values[];
} buffers[1];
layout(push_constant) uniform ReplayMoved {
    layout(offset = 128) uint offsets[32];
} replayMoved;
#else
layout(std430, binding = 0) readonly buffer Values {
    uvec2 values[];
};
#endif

#ifdef REPLAY_MOVED
layout(location = 0) out vec4 movedColor;
#else
layout(push_constant) uniform Fill {
    uint width;
    uint count;
} fill;
#endif

void main() {
    const uvec2 at = uvec2(gl_FragCoord.xy);
#ifdef REPLAY_MOVED
    const uint slot = (replayMoved.offsets[0] >> 2u) + at.y * 64u + at.x;
    movedColor = vec4(float(buffers[0].values[slot] & 255u) / 255.0, 0.0, 0.0, 1.0);
#else
    const uint slot = (at.y * fill.width + at.x) * fill.count + uint(gl_SampleID);
    gl_FragDepth = uintBitsToFloat(values[slot].x);
#ifdef STENCIL
    gl_FragStencilRefARB = int(values[slot].y);
#endif
#endif
}
