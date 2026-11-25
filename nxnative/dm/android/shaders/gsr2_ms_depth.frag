#version 450
#extension GL_EXT_samplerless_texture_functions : require
layout(binding=0) uniform texture2DMS src;
layout(push_constant) uniform Scale { vec2 srcPerDst; } pc;
void main() {
    gl_FragDepth = texelFetch(src, ivec2(gl_FragCoord.xy * pc.srcPerDst), gl_SampleID).r;
}
