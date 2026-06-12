uniform sampler2D tex;
uniform sampler2D tex2;
uniform float param3;
in vec4 f_color;
in vec2 f_uv;
out vec4 fragColor;

void main(void)
{
    vec4 albedo = texture(tex, f_uv) * f_color;
    vec4 emiss = texture(tex2, f_uv) * f_color;
    albedo.rgb = pow(albedo.rgb, vec3(1.0 / param3));
    emiss.rgb = pow(emiss.rgb, vec3(1.0 / param3));
    fragColor = clamp(albedo + emiss, 0.0, 1.0);
}
