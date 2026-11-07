#version 440

layout(std140, binding = 1) uniform FragUniforms {
    int format_type;  // 0 = YUV420P, 1 = NV12, 2 = YUV420P10LE, 3 = P010LE
    int color_space;  // 0 = BT.709, 1 = BT.601
    int color_range;  // 0 = limited (Y 16-235), 1 = full (Y 0-255)
};

layout(binding = 2) uniform sampler2D y_tex;
layout(binding = 3) uniform sampler2D u_tex;
layout(binding = 4) uniform sampler2D v_tex;

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 fragColor;

// Hash of the pixel position, uniform in [0, 1)
float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

void main() {
    // 10-bit formats are uploaded to 16-bit textures, which sample as code/65535. Rescale so that a
    // sample reads as code/1023 either way. P010 keeps its 10 bits in the top bits (code << 6), so it
    // needs a different factor from the low-aligned yuv420p10le.
    bool ten_bit = format_type >= 2;
    bool msb_aligned = format_type == 3;
    float tex_scale = 1.0;
    if (ten_bit) {
        tex_scale = msb_aligned ? 65535.0 / 65472.0 : 65535.0 / 1023.0;
    }

    float y = texture(y_tex, vTexCoord).r * tex_scale;
    float u, v;

    if (format_type == 1 || format_type == 3) {
        // NV12 / P010: UV interleaved in one texture (RG8 or RG16)
        vec2 uv = texture(u_tex, vTexCoord).rg * tex_scale;
        u = uv.r;
        v = uv.g;
    } else {
        // YUV420P / YUV420P10LE: separate U and V planes
        u = texture(u_tex, vTexCoord).r * tex_scale;
        v = texture(v_tex, vTexCoord).r * tex_scale;
    }

    // Chroma is always centred on mid-scale; luma only carries the black pedestal in limited range.
    // 10-bit limited range: Y 64-940, chroma 64-960 (centre 512), on the same 0-1 scale as 8-bit.
    float y_offset = (color_range == 1) ? 0.0 : (ten_bit ? 64.0/1023.0 : 16.0/255.0);
    float c_offset = ten_bit ? 512.0/1023.0 : 128.0/255.0;
    vec3 yuv = vec3(y, u, v) - vec3(y_offset, c_offset, c_offset);

    // GLSL mat3 is column-major: mat3(col0, col1, col2)
    // BT.709 full range YCbCr to RGB
    mat3 bt709 = mat3(
        1.0,  1.0,       1.0,
        0.0, -0.187324,  1.855600,
        1.574800, -0.468124,  0.0
    );
    // BT.601 full range YCbCr to RGB
    mat3 bt601 = mat3(
        1.0,  1.0,       1.0,
        0.0, -0.344136,  1.772000,
        1.402000, -0.714136,  0.0
    );

    mat3 conv = (color_space == 1) ? bt601 : bt709;
    // Limited range stores the same colours on a narrower scale (Y on 219 codes, chroma on 224),
    // so stretch each input component back up before the matrix. Scaling the columns of conv does
    // exactly that: (conv * S) * yuv == conv * (S * yuv).
    if (color_range != 1) {
        float y_scale = ten_bit ? 1023.0/876.0 : 255.0/219.0;
        float c_scale = ten_bit ? 1023.0/896.0 : 255.0/224.0;
        conv = conv * mat3(y_scale, 0.0, 0.0,
                           0.0, c_scale, 0.0,
                           0.0, 0.0, c_scale);
    }
    vec3 rgb = clamp(conv * yuv, 0.0, 1.0);

    // An 8-bit source only has steps of 1/255. Half a step of noise turns those steps into a dithered
    // transition in the float composite, so smooth areas do not band in a 10-bit export. 10-bit sources have
    // steps far finer than that, so they are left alone.
    if (!ten_bit) {
        rgb = clamp(rgb + (hash12(gl_FragCoord.xy) - 0.5) / 255.0, 0.0, 1.0);
    }
    fragColor = vec4(rgb, 1.0);
}
