#version 440

// Averages the compositing target down to the export size. Each output pixel takes evenly spaced taps across the
// source pixels it covers, so a 2:1 reduction averages each 2x2 block and larger reductions do not alias.
// When dithering (8-bit output), half an 8-bit step of noise is added before the write rounds to RGBA8.

layout(std140, binding = 1) uniform DownscaleParams {
    vec4 footprint_taps;  // xy: size of one output pixel in texture coordinates, zw: taps per axis
    vec4 options;         // x: 1 to add dither noise
};

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
    vec2 footprint = footprint_taps.xy;
    vec2 taps = footprint_taps.zw;

    vec4 sum = vec4(0.0);
    for (int j = 0; j < 8; j++) {
        if (float(j) >= taps.y) break;
        float oy = ((float(j) + 0.5) / taps.y - 0.5) * footprint.y;
        for (int i = 0; i < 8; i++) {
            if (float(i) >= taps.x) break;
            float ox = ((float(i) + 0.5) / taps.x - 0.5) * footprint.x;
            sum += texture(tex, vTexCoord + vec2(ox, oy));
        }
    }
    vec4 c = sum / (taps.x * taps.y);

    if (options.x > 0.5) {
        float noise = hash12(gl_FragCoord.xy) - 0.5;
        c.rgb = clamp(c.rgb + noise / 255.0, 0.0, 1.0);
    }
    fragColor = c;
}
