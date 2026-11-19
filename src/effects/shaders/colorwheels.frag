#version 440
layout(std140, binding = 1) uniform FragParams {
    vec3 lift_wheel;
    float lift_master;
    vec3 gamma_wheel;
    float gamma_master;
    vec3 gain_wheel;
    float gain_master;
};
layout(binding = 2) uniform sampler2D myTexture;
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 fragColor;

// A wheel's tint as a zero-mean RGB offset, so the wheel changes colour without changing brightness.
// Brightness is set by the master value for that control.
vec3 tint(vec3 c) {
	return c - vec3(dot(c, vec3(1.0 / 3.0)));
}

void main(void) {
	vec4 textureColor = texture(myTexture, vTexCoord);

	// input is premultiplied: work on the straight colour, premultiply again on output
	vec3 rgb = textureColor.a > 0.0 ? textureColor.rgb / textureColor.a : vec3(0.0);

	vec3 lift = tint(lift_wheel) * 0.5 + vec3(lift_master) * 0.5;
	vec3 gamma = exp2(tint(gamma_wheel) + vec3(gamma_master));
	vec3 gain = exp2(tint(gain_wheel) + vec3(gain_master));

	// lift raises the shadows most, gain scales the whole range, gamma bends the midtones
	rgb = rgb + lift * (1.0 - rgb);
	rgb = rgb * gain;
	rgb = pow(max(rgb, vec3(0.0)), 1.0 / gamma);

	rgb = clamp(rgb, 0.0, 1.0);
	fragColor = vec4(rgb * textureColor.a, textureColor.a);
}
