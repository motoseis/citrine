#version 440

// RGB to 10-bit limited-range Cb and Cr (BT.601), at half resolution for 4:2:0 export planes. Each output sample
// averages the 2x2 full-resolution pixels it covers. options.x is 64 for MSB-aligned output (P010) and 1 for
// low-aligned output (yuv420p10le). options.yz is half of one full-resolution pixel in texture coordinates.

layout(std140, binding = 1) uniform YuvParams {
    vec4 options;
};

layout(binding = 2) uniform sampler2D tex;

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 fragColor;

void main() {
    vec2 o = options.yz;
    vec3 c = texture(tex, vTexCoord + vec2(-o.x, -o.y)).rgb
           + texture(tex, vTexCoord + vec2( o.x, -o.y)).rgb
           + texture(tex, vTexCoord + vec2(-o.x,  o.y)).rgb
           + texture(tex, vTexCoord + vec2( o.x,  o.y)).rgb;
    c = clamp(c * 0.25, 0.0, 1.0);

    float y = dot(c, vec3(0.299, 0.587, 0.114));
    float cb = (c.b - y) * 0.564;  // (B - Y) / 1.772
    float cr = (c.r - y) * 0.713;  // (R - Y) / 1.402

    // 10-bit limited range: chroma is centred on 512 and spans 64 to 960
    float u = clamp(floor(512.0 + 896.0 * cb + 0.5), 0.0, 1023.0);
    float v = clamp(floor(512.0 + 896.0 * cr + 0.5), 0.0, 1023.0);
    fragColor = vec4(u * options.x / 65535.0, v * options.x / 65535.0, 0.0, 1.0);
}
