#version 440

// Resolves the 16-bit float compositing target to 8-bit for the CPU readback. Half an 8-bit step of
// noise is added before the write rounds to RGBA8, so smooth gradients break up instead of banding.

layout(binding = 2) uniform sampler2D tex;

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 fragColor;

// Hash of the pixel position, uniform in [0, 1)
float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

void main() {
    vec4 c = texture(tex, vTexCoord);
    float noise = hash12(gl_FragCoord.xy) - 0.5;
    fragColor = vec4(clamp(c.rgb + noise / 255.0, 0.0, 1.0), c.a);
}
