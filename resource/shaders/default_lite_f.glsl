#ifdef GL_ARB_texture_gather
#define HAS_TEXTUREGATHER
#endif

#ifdef HAS_TEXTUREGATHER
vec4 impl_textureGather(sampler2D tex, vec2 uv)
{
    return textureGather(tex, uv);
}
#else
float modI(float a,float b) {
    float m=a-floor((a+0.5)/b)*b;
    return floor(m+0.5);
}

vec4 impl_textureGather(sampler2D tex, vec2 uv)
{
    ivec2 idims = textureSize(tex,0) - ivec2(1, 1);
    vec2 dims = vec2(idims);

    ivec2 base = ivec2(dims*uv);
    base.x = int(modI(float(base.x), dims.x));
    base.y = int(modI(float(base.y), dims.y));

    return vec4(texelFetch(tex,base+ivec2(0,1),0).x,
        texelFetch(tex,base+ivec2(1,1),0).x,
        texelFetch(tex,base+ivec2(1,0),0).x,
        texelFetch(tex,base+ivec2(0,0),0).x
    );
}
#endif

#define LIGHT_DIVISOR (6.0)
#define TEX_MODE_TEST 0
#define TEX_MODE_WORLDPAL 1
#define TEX_MODE_BILINEAR 2
#define TEX_MODE_16BPP 5
#define TEX_MODE_BILINEAR_16BPP 6

#define D3DBLEND_ONE             (2)
#define D3DBLEND_SRCALPHA        (5)
#define D3DBLEND_INVSRCALPHA     (6)

uniform sampler2D tex;
uniform sampler2D texEmiss;
uniform sampler2D worldPalette;
uniform sampler2D worldPaletteLights;
uniform int tex_mode;
uniform int blend_mode;
uniform vec3 colorEffects_tint;
uniform vec3 colorEffects_filter;
uniform float colorEffects_fade;
uniform vec3 colorEffects_add;
uniform vec3 emissiveFactor;
uniform vec4 albedoFactor;
uniform float light_mult;

in vec4 f_color;
in float f_light;
in vec2 f_uv;

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec4 fragColorEmiss;

float luminance(vec3 c_rgb)
{
    const vec3 W = vec3(0.2125, 0.7154, 0.0721);
    return dot(c_rgb, W);
}

#ifdef CAN_BILINEAR_FILTER
vec4 bilinear_paletted()
{
    vec2 colorTextureSize = vec2(textureSize(tex, 0));
    vec2 pixCoord = f_uv * colorTextureSize - 0.5f;
    vec2 originPixCoord = floor(pixCoord);
    vec2 gUV = (originPixCoord + 1.0f) / colorTextureSize;
    vec4 gIndex   = impl_textureGather(tex, gUV);
    vec4 c00   = texture(worldPalette, vec2(gIndex.w, 0.5));
    vec4 c01 = texture(worldPalette, vec2(gIndex.x, 0.5));
    vec4 c11  = texture(worldPalette, vec2(gIndex.y, 0.5));
    vec4 c10 = texture(worldPalette, vec2(gIndex.z, 0.5));
    if (blend_mode == D3DBLEND_SRCALPHA || blend_mode == D3DBLEND_INVSRCALPHA) {
        if (gIndex.x == 0.0) { c01.a = 0.0; }
        if (gIndex.y == 0.0) { c11.a = 0.0; }
        if (gIndex.z == 0.0) { c10.a = 0.0; }
        if (gIndex.w == 0.0) { c00.a = 0.0; }
    }
    vec2 filterWeight = pixCoord - originPixCoord;
    vec4 temp0 = mix(c01, c11, filterWeight.x);
    vec4 temp1 = mix(c00, c10, filterWeight.x);
    return vec4(mix(temp1, temp0, filterWeight.y).rgb, mix(temp1, temp0, filterWeight.y).a);
}

vec4 bilinear_paletted_light(float index)
{
    float light = clamp(f_light, 0.0, 1.0);
    float light_idx = light / LIGHT_DIVISOR;
    vec2 colorTextureSize = vec2(textureSize(tex, 0));
    vec2 pixCoord = f_uv * colorTextureSize - 0.5f;
    vec2 originPixCoord = floor(pixCoord);
    vec2 gUV = (originPixCoord + 1.0f) / colorTextureSize;
    vec4 gIndex   = impl_textureGather(tex, gUV);
    vec4 c00   = texture(worldPalette, vec2(texture(worldPaletteLights, vec2(gIndex.w, light_idx)).r, 0.5));
    vec4 c01 = texture(worldPalette, vec2(texture(worldPaletteLights, vec2(gIndex.x, light_idx)).r, 0.5));
    vec4 c11  = texture(worldPalette, vec2(texture(worldPaletteLights, vec2(gIndex.y, light_idx)).r, 0.5));
    vec4 c10 = texture(worldPalette, vec2(texture(worldPaletteLights, vec2(gIndex.z, light_idx)).r, 0.5));
    vec2 filterWeight = pixCoord - originPixCoord;
    vec4 temp0 = mix(c01, c11, filterWeight.x);
    vec4 temp1 = mix(c00, c10, filterWeight.x);
    vec4 blendColor = mix(temp1, temp0, filterWeight.y);
    vec4 light_mult_quad = vec4(light_mult, light_mult, light_mult, 1.0);
    return vec4(blendColor.rgb, 1.0) * light_mult_quad;
}
#endif

void main(void)
{
    vec4 sampled = texture(tex, f_uv);
    vec4 sampledEmiss = texture(texEmiss, f_uv);
    vec4 sampled_color = vec4(1.0, 1.0, 1.0, 1.0);
    vec4 vertex_color = f_color;
#ifdef GL_ES
    float index = floor(sampled.r * 255.0 + 0.5) / 255.0;
#else
    float index = sampled.r;
#endif
    vec4 palval = texture(worldPalette, vec2(index, 0.5));
    vec4 color_add = vec4(0.0, 0.0, 0.0, 1.0);
    vec4 color_add_emiss = vec4(0.0, 0.0, 0.0, 0.0);

    if (tex_mode == TEX_MODE_TEST) {
        sampled_color = vec4(1.0, 1.0, 1.0, 1.0);
    }
    else if (tex_mode == TEX_MODE_16BPP || tex_mode == TEX_MODE_BILINEAR_16BPP) {
        sampled_color = vec4(sampled.b, sampled.g, sampled.r, sampled.a);
    }
    else if (tex_mode == TEX_MODE_WORLDPAL
#ifndef CAN_BILINEAR_FILTER
    || tex_mode == TEX_MODE_BILINEAR
#endif
    ) {
        if (index == 0.0 && (blend_mode == D3DBLEND_SRCALPHA || blend_mode == D3DBLEND_INVSRCALPHA))
            discard;
        float light = clamp(f_light, 0.0, 1.0);
        float light_idx = light / LIGHT_DIVISOR;
        float light_worldpalidx = texture(worldPaletteLights, vec2(index, light_idx)).r;
        vec4 lightPalval = texture(worldPalette, vec2(light_worldpalidx, 0.5));
        color_add = (lightPalval * light_mult);
        sampled_color = palval;
    }
#ifdef CAN_BILINEAR_FILTER
    else if (tex_mode == TEX_MODE_BILINEAR) {
        sampled_color = bilinear_paletted();
        color_add = bilinear_paletted_light(index);
        if (sampled_color.a < 0.01)
            discard;
    }
#endif

    vec4 albedoFactor_copy = albedoFactor;

    if (blend_mode == D3DBLEND_INVSRCALPHA) {
        if (vertex_color.a < 0.01)
            discard;
    }

    if (blend_mode != D3DBLEND_SRCALPHA && blend_mode != D3DBLEND_INVSRCALPHA && vertex_color.a != 0.0)
        vertex_color.a = 1.0;

    vec4 main_color = (sampled_color * vertex_color);
    vec4 effectAdd_color = vec4(colorEffects_add.r, colorEffects_add.g, colorEffects_add.b, 0.0);
    main_color *= albedoFactor_copy;
    float orig_alpha = main_color.a;

    if (main_color.a < 0.01 && sampledEmiss.r == 0.0 && sampledEmiss.g == 0.0 && sampledEmiss.b == 0.0)
        discard;

    if (blend_mode == D3DBLEND_INVSRCALPHA) {
        main_color.rgb *= (1.0 - main_color.a);
        main_color.a = (1.0 - main_color.a);
    }

    color_add.rgb += sampledEmiss.rgb * emissiveFactor * 0.1;

    if (sampledEmiss.r != 0.0 || sampledEmiss.g != 0.0 || sampledEmiss.b != 0.0)
        color_add_emiss.rgb += sampledEmiss.rgb * 0.1;

    fragColor = main_color + effectAdd_color;
    color_add.a = orig_alpha;

    float luma = luminance(color_add.rgb) * 0.5;

    if (emissiveFactor.r != 0.0 || emissiveFactor.g != 0.0 || emissiveFactor.b != 0.0)
        luma = 1.0;
    else {
        color_add.r *= luma;
        color_add.g *= luma;
        color_add.b *= luma;
    }

    vec3 tint = normalize(colorEffects_tint + 1.0) * sqrt(3.0);
    color_add.r *= tint.r;
    color_add.g *= tint.g;
    color_add.b *= tint.b;
    color_add.r *= colorEffects_fade;
    color_add.g *= colorEffects_fade;
    color_add.b *= colorEffects_fade;
    color_add.r *= colorEffects_filter.r;
    color_add.g *= colorEffects_filter.g;
    color_add.b *= colorEffects_filter.b;

    if (luma < 0.01 && orig_alpha < 0.5 && (blend_mode == D3DBLEND_SRCALPHA || blend_mode == 6))
        color_add = vec4(0.0, 0.0, 0.0, 0.0);

    fragColorEmiss = color_add_emiss + color_add;
    gl_FragDepth = gl_FragCoord.z;
}
