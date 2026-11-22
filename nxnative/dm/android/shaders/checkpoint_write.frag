#version 450
layout(binding=1, std430) readonly buffer Pixels { uint data[]; };
layout(push_constant) uniform Params { uint width, samples, sampleIndex, depth; } p;
layout(location=0) out vec4 color;
void main() {
    uvec2 pos = uvec2(gl_FragCoord.xy);
    uint v = data[(pos.y * p.width + pos.x) * p.samples + p.sampleIndex];
    gl_SampleMask[0] = int(1u << p.sampleIndex);
    uint rg = ((v & 0x7ffu) << 4) | (((v >> 11) & 0x7ffu) << 20);
    color = vec4(unpackHalf2x16(rg), unpackHalf2x16((v >> 22) << 5).x, 1.0);
    gl_FragDepth = p.depth != 0 ? uintBitsToFloat(v) : 0.0;
}
