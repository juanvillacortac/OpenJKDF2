uniform sampler2D tex;
uniform sampler2D worldPalette;
uniform sampler2D displayPalette;
uniform int u_menuIndexed;
in vec4 f_color;
in vec2 f_uv;
out vec4 fragColor;

void main(void)
{
    vec4 sampled = texture(tex, f_uv);
    vec4 vertex_color = f_color;

    if (u_menuIndexed == 0) {
        if (sampled.a < 0.01)
            discard;
        fragColor = vec4(sampled.rgb, 1.0) * vertex_color;
        return;
    }

    float index = sampled.r;
    float palU = (index * 255.0 + 0.5) / 256.0;
    vec4 palvald = texture(displayPalette, vec2(palU, 0.5));

    if (index < 0.001)
        discard;

    fragColor = vec4(palvald.rgb, 1.0) * vertex_color;
}
