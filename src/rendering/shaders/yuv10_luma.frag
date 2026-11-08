#version 440

// RGB to 10-bit limited-range luma (BT.601), for the export planes. options.x is 64 for MSB-aligned output
// (P010, code in the top 10 bits) and 1 for low-aligned output (yuv420p10le). The value is written as unorm16.

layout(std140, binding = 1) uniform YuvParams {
    vec4 options;
};

layout(binding = 2) uniform sampler2D tex;

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 fragColor;

void main() {
    vec3 c = clamp(texture(tex, vTexCoord).rgb, 0.0, 1.0);
    float y = dot(c, vec3(0.299, 0.587, 0.114));
    float code = floor(64.0 + 876.0 * y + 0.5);  // 10-bit limited range: 64 to 940
    fragColor = vec4(code * options.x / 65535.0, 0.0, 0.0, 1.0);
}
