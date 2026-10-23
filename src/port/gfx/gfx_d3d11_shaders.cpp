// Port-only (checkpoint 132mt1): the D3D11 backend's shader programs (gfx_programs.hpp lists them). The HLSL text
// of each renderer moved here unchanged (same text, entry points, profiles and compile flags as the renderers used,
// so the bytecode and the pixels are the same); the comments that explain each program's GS / VU1 origin stay
// with it.
#include "gfx_d3d11_shaders.hpp"

#include "geometry.hpp"

#include <cstddef>
#include <iterator>
#include <stdexcept>
#include <string>

namespace omega::gfx::d3d11 {
namespace {

// Port of scene_renderer.cpp's model shader (the level and character models). Moved from scene_renderer.cpp unchanged (checkpoint 132mt1).
constexpr char scene_source[] = R"(
cbuffer Scene : register(b0) {
    row_major float4x4 mvp;
    float4 options; // x textured, y collision colour, z vertex lighting only, w pass mode
    float4 eye;     // world-space eye (reflection passes)
    float4 up;      // model Z axis in world space (reflection passes)
    float4 alphaTest; // x GS TEST AREF (-1 = off), y 1 = draw the failing pixels,
                      // z 1 = the texture alpha is the GS byte (0x80 = 1.0) and the
                      // output is min(1, Cs * As / 128) for a ONE/ONE blend (0x48 / 0x42),
                      // w man model draw (Checkpoint 118e): RGBAQ.A + 1 (0 = off)
    float4 uvOffset;  // xy: shader 13 flag 2 q1 (VU1 program 0x595 adds it to the ST source)
};
Texture2D surfaceTexture : register(t0);
SamplerState surfaceSampler : register(s0);
struct Input {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float3 gore : TEXCOORD1;
};
struct Varying {
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float3 gore : TEXCOORD1;
};
Varying vertexMain(Input input) {
    Varying output;
    output.position = mul(float4(input.position, 1), mvp);
    output.normal = input.normal;
    output.uv = input.uv - uvOffset.xy;
    if (options.w > 0.5 && options.w < 1.5) {
        // VU1 program 0x4f8 (shaders 10/11): m = -normal, T = up x m,
        // B = m x T (unnormalised), V = normalize(eye - position),
        // uv = 0.5 + 0.5 * (T.V, B.V).
        float3 m = -input.normal;
        float3 t = cross(up.xyz, m);
        float3 b = cross(m, t);
        float3 v = normalize(eye.xyz - input.position);
        output.uv = 0.5 + 0.5 * float2(dot(t, v), dot(b, v));
    } else if (options.w > 2.5) {
        // VU1 program 0x5cd (shader 14): S and T minus Q/64.
        output.uv = input.uv - 1.0 / 64.0;
    }
    output.color = input.color;
    output.gore = input.gore;
    return output;
}
float4 pixelMain(Varying input) : SV_TARGET {
    if (options.y > 0.5) return float4(1, 0.35, 0.05, 1);
    float4 color = input.color;
    float4 texel = float4(1, 1, 1, 1);
    if (options.x > 0.5) {
        texel = surfaceTexture.Sample(surfaceSampler, input.uv);
        color *= texel;
    }
    bool raw = alphaTest.z > 0.5;
    if (alphaTest.w > 0.5) {
        // Man model draw (man_draw_pixel): At is the texel byte (decoded as
        // min(255, 2 * a) / 255), As = (At * Av) >> 7, ATST GEQUAL AREF; the
        // blend takes As / 128.
        float at = ceil(color.a * 127.5 - 0.001);
        float as = floor(at * (alphaTest.w - 1.0) / 128.0);
        bool passed = as >= alphaTest.x;
        if (passed == (alphaTest.y > 0.5)) discard;
        color.a = as / 128.0;
    } else if (alphaTest.x >= 0) {
        // GS TEST ATST GREATER on the modulated alpha (0x80 = 1.0; texture
        // alpha is decoded as min(255, 2 * a) / 255, or a / 255 when raw).
        bool passed = round(color.a * (raw ? 255.0 : 127.5)) > alphaTest.x;
        if (passed == (alphaTest.y > 0.5)) discard;
    }
    float lengthSquared = dot(input.normal, input.normal);
    float lighting = 0.28;
    float3 rgb = color.rgb;
    if (options.z <= 0.5) {
        if (lengthSquared > 0.00001)
            lighting += 0.72 * abs(dot(input.normal * rsqrt(lengthSquared), normalize(float3(-0.35, 0.5, 0.8))));
        rgb *= lighting;
    }
    if (any(input.gore > 0)) {
        // Character gore (VU1 micro 0x3058): the lit vertex colour loses the
        // subtract, clamped at 0, before TFX MODULATE with the texel.
        float3 lit = input.color.rgb * (options.z <= 0.5 ? lighting : 1.0);
        rgb = max(lit - input.gore, 0) * texel.rgb;
    }
    if (raw) {
        float as128 = color.a * 255.0 / 128.0;
        return float4(saturate(saturate(rgb) * as128), saturate(as128));
    }
    return saturate(float4(rgb, color.a));
}
)";
constexpr std::size_t scene_source_size = sizeof(scene_source) - 1;

// Character shadows (CHARACTER_SHADOWS.md sections 5, 6 and 9). Moved from scene_renderer.cpp unchanged (checkpoint 132mt1).
constexpr char shadow_source[] = R"(
cbuffer Scene : register(b0) {
    row_major float4x4 mvp;
    float4 options;
    float4 eye;
    float4 up;
};
cbuffer Shadow : register(b1) {
    row_major float4x4 clip;  // column-major game matrix: mul(v, clip) = clip * v
    float4 scale;
    float4 offset;
    float4 facing;
    float4 params;            // x silhouette colour (byte / 255), y slot type, z buffer layer,
                              // w type 3: 1 when t2 holds the raw GS alpha byte
};
Texture2DArray shadowBuffers : register(t1);
Texture2D projectorTexture : register(t2);
SamplerState shadowSampler : register(s1);
Texture2D surfaceTexture : register(t0);
SamplerState surfaceSampler : register(s0);
struct Input {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};
// Buffer render (003d2d10): the caster through the slot's own lookup. A point
// that the receiver pass maps to texel (u, v) = offset + scale * X / X.w is
// drawn at NDC (2u - 1, 1 - 2v); with the 126x126 viewport inside the 128x128
// buffer this keeps the original's 128/126 ratio between lookup and image.
struct BufferVarying {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};
BufferVarying bufferVertex(Input input) {
    BufferVarying o;
    float4 X = mul(float4(input.position, 1), clip);
    o.position = float4((2 * offset.x - 1) * X.w + 2 * scale.x * X.x,
                        (1 - 2 * offset.y) * X.w - 2 * scale.y * X.y, 0.5 * X.w, X.w);
    o.uv = input.uv;
    o.color = input.color;
    return o;
}
float4 bufferPixel(BufferVarying input) : SV_TARGET {
    if (params.y < 1.5) return float4(params.xxx, 128.0 / 255.0);  // type 1: silhouette, alpha 0x80
    // Type 2: the character drawn normally into the buffer (lit colour as the
    // vertex colour, textured when params.w is set); alpha on the PS2 scale
    // (1.0 = 0x80) so the receiver's >> 7 arithmetic sees the stored byte.
    float4 color = input.color;
    if (params.w > 0.5) color *= surfaceTexture.Sample(surfaceSampler, input.uv);
    return float4(saturate(color.rgb), saturate(color.a) * 128.0 / 255.0);
}
// Receiver pass (VU1 routine at 0047ae50).
struct PassVarying {
    float4 position : SV_POSITION;
    float4 X : TEXCOORD0;
    noperspective float intensity : TEXCOORD1;  // GS Gouraud colour: linear in screen space
    float3 normal : NORMAL;
};
PassVarying passVertex(Input input) {
    PassVarying o;
    o.position = mul(float4(input.position, 1), mvp);
    o.X = mul(float4(input.position, 1), clip);
    // FTOI0 per vertex; type 3 (VU1 1636..: vf30.w = offset.z, the RGB lanes
    // 128) takes the constant offset.z (003d2d10: 128 * strength).
    o.intensity = params.y > 2.5 ? trunc(offset.z) : trunc(offset.z + scale.z * o.X.z / o.X.w);
    o.normal = input.normal;
    return o;
}
bool outsideGuard(float4 X) { return any(abs(X.xyz) > abs(X.w)); }
// ADC suppression: any vertex outside the guard box, or the closing vertex's
// model-space normal facing away from the projector (dot >= 0).
[maxvertexcount(3)]
void passGeometry(triangle PassVarying v[3], inout TriangleStream<PassVarying> stream) {
    if (outsideGuard(v[0].X) || outsideGuard(v[1].X) || outsideGuard(v[2].X)) return;
    if (dot(facing.xyz, v[2].normal) >= 0) return;
    stream.Append(v[0]);
    stream.Append(v[1]);
    stream.Append(v[2]);
}
// MODULATE >> 7 on byte values, ATST GREATER 0; the blend state applies
// (Cd - Cs) with FIX 128 (type 1).
float4 passPixel(PassVarying input) : SV_TARGET {
    float2 uv = offset.xy + scale.xy * input.X.xy / input.X.w;
    if (params.y > 2.5) {
        // Type 3 (a blood decal projector): the slot's own texture, MODULATE
        // with RGB 128 (Cs = T), As = T.a * A >> 7, ATST GREATER 0, ALPHA 0x42:
        // Cd - (Cs * As >> 7) (the subtract blend state).
        float4 t = round(projectorTexture.SampleLevel(shadowSampler, uv, 0) * 255);
        float raw = params.w > 0.5 ? t.a : (t.a >= 254.5 ? 128 : floor(t.a / 2));
        float a3 = floor(raw * floor(input.intensity) / 128);
        if (a3 <= 0) discard;
        return float4(floor(t.rgb * a3 / 128) / 255, 0);
    }
    float4 texel = round(shadowBuffers.SampleLevel(shadowSampler, float3(uv, params.z), 0) * 255);
    float intensity = floor(input.intensity);
    float alpha = floor(texel.a * intensity / 128);
    if (alpha <= 0) discard;
    if (params.y < 1.5) return float4(floor(texel.rgb * intensity / 128) / 255, 0);  // type 1: Cs = B*I >> 7
    // Type 2: RGB lanes are 128, so Cs = B; ALPHA 0x44 blends (Cs - Cd) * As >> 7 + Cd.
    return float4(texel.rgb / 255, min(alpha / 128, 1));
}
)";
constexpr std::size_t shadow_source_size = sizeof(shadow_source) - 1;

// Port-only (checkpoint 132md8, research/MODDING.md "Speed"): a mod replacement part skinned on the GPU. Its bind
// vertices (gfx::GpuSkinVertex) are skinned by the palette rows in b2 (gfx::gpu_palette_rows: the position matrices'
// rows 0..2, then the normal matrices') as gfx::skin_replacement_vertex skins them on the CPU, then handed to
// the scene / shadow buffer vertex programs above unchanged: the colour is the part's base colour (white in thermal),
// no gore, and in thermal the ST from the skinned normal (skm_thermal_uv: 0.5 + 0.5 * (rows . n)). Appended to the
// scene and shadow texts (their programs keep their own text and bytecode).
constexpr char skin_source[] = R"(
cbuffer Skin : register(b2) {
    float4 skinColor;
    float4 skinFlags;   // x: 1 = thermal (colour 0x80, the ST from the normal)
    float4 thermalU;    // xyz: the thermal light rows' first column
    float4 thermalV;    // xyz: their second column
    row_major float3x4 bonePositions[128];  // gfx::gpu_palette_rows: per bone its position matrix's rows 0..2
    row_major float3x4 boneNormals[128];    // then per bone its normal matrix's (w 0)
};
struct SkinInput {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    uint4 bones0 : BLENDINDICES0;
    uint4 bones1 : BLENDINDICES1;
    float4 weights0 : BLENDWEIGHT0;
    float4 weights1 : BLENDWEIGHT1;
};
void skinInfluence(uint b, float w, float4 p, float3 n, inout float3 sp, inout float3 sn) {
    sp += w * mul(bonePositions[b], p);
    sn += w * mul((float3x3)boneNormals[b], n);
}
void skinVertex(SkinInput s, out float3 position, out float3 normal, out float2 uv, out float4 color) {
    float4 p = float4(s.position, 1);
    position = float3(0, 0, 0);
    normal = float3(0, 0, 0);
    skinInfluence(s.bones0.x, s.weights0.x, p, s.normal, position, normal);
    skinInfluence(s.bones0.y, s.weights0.y, p, s.normal, position, normal);
    skinInfluence(s.bones0.z, s.weights0.z, p, s.normal, position, normal);
    skinInfluence(s.bones0.w, s.weights0.w, p, s.normal, position, normal);
    skinInfluence(s.bones1.x, s.weights1.x, p, s.normal, position, normal);
    skinInfluence(s.bones1.y, s.weights1.y, p, s.normal, position, normal);
    skinInfluence(s.bones1.z, s.weights1.z, p, s.normal, position, normal);
    skinInfluence(s.bones1.w, s.weights1.w, p, s.normal, position, normal);
    bool thermal = skinFlags.x > 0.5;
    uv = thermal ? 0.5 + 0.5 * float2(dot(thermalU.xyz, normal), dot(thermalV.xyz, normal)) : s.uv;
    color = thermal ? float4(1, 1, 1, 1) : skinColor;
}
)";
constexpr char scene_skin_tail[] = R"(
Varying vertexSkinned(SkinInput s) {
    Input input;
    skinVertex(s, input.position, input.normal, input.uv, input.color);
    input.gore = float3(0, 0, 0);
    return vertexMain(input);
}
)";
constexpr char shadow_skin_tail[] = R"(
BufferVarying bufferSkinned(SkinInput s) {
    Input input;
    skinVertex(s, input.position, input.normal, input.uv, input.color);
    return bufferVertex(input);
}
)";
const std::string scene_skinned_source = std::string(scene_source) + skin_source + scene_skin_tail;
const std::string shadow_skinned_source = std::string(shadow_source) + skin_source + shadow_skin_tail;

// The GUI primitive shader (gui_renderer.hpp). Moved from gui_renderer.cpp unchanged (checkpoint 132mt1).
constexpr char gui_source[] = R"(
Texture2D image : register(t0);
SamplerState sampling : register(s0);
struct VertexIn { float2 position : POSITION; float2 uv : TEXCOORD0; float4 colour : COLOR0; float2 depth : TEXCOORD1; };
struct VertexOut { float4 position : SV_POSITION; float2 uv : TEXCOORD0; float4 colour : COLOR0; };
VertexOut vs_main(VertexIn input) {
    VertexOut output;
    output.position = float4(input.position.x / 320.0, input.position.y / 224.0, input.depth.x, input.depth.y);
    output.uv = input.uv;
    output.colour = input.colour;
    return output;
}
float4 ps_main(VertexOut input) : SV_TARGET {
    return saturate(image.Sample(sampling, input.uv) * input.colour);
}
)";
constexpr std::size_t gui_source_size = sizeof(gui_source) - 1;

// The particle, shatter, heat-haze and ScreenFxBuf shaders (particle_renderer.hpp). Moved from particle_renderer.cpp unchanged (checkpoint 132mt1).
constexpr char particle_source[] = R"(
Texture2D image : register(t0);
SamplerState sampling : register(s0);
struct VertexIn { float3 position : POSITION; float2 uv : TEXCOORD0; float4 colour : COLOR0; };
struct VertexOut { float4 position : SV_POSITION; float2 uv : TEXCOORD0; float4 colour : COLOR0; };
VertexOut vs_main(VertexIn input) {
    VertexOut output;
    output.position = float4(input.position, 1.0);
    output.uv = input.uv;
    output.colour = input.colour;
    return output;
}
struct ClipIn { float4 position : POSITION; float2 uv : TEXCOORD0; float4 colour : COLOR0; };
VertexOut vs_clip(ClipIn input) {
    VertexOut output;
    output.position = input.position;
    output.uv = input.uv;
    output.colour = input.colour;
    return output;
}
float4 ps_main(VertexOut input) : SV_TARGET {
    const float4 texel = image.Sample(sampling, input.uv);
    if (texel.a * 255.0 * input.colour.a < 2.0) discard;
    return saturate(texel * input.colour);
}
// Checkpoint 132cp4: one heat-haze particle (VU1 0x1cc8), its two GS sprites
// over the same corners and depth in one pass:
//   sprite A (context 2: TEX0_2 / TEX1_2 the particle texture, ALPHA_2 0x88,
//   FBMSK 0xffffff, TEST_2 0x51001 = ATST NEVER with AFAIL FB_ONLY and ZTST
//   GEQUAL; 0037d3d8) writes only the frame alpha, As = (At * Af) >> 7 with no
//   alpha test;
//   sprite B (context 1: ScreenFxBuf at the ST, MODULATE, the particle TEST_1,
//   ALPHA_1 0x54 = (Cs - Cd) * Ad + Cd) blends with that Ad. Sprite B passes
//   the depth test exactly where sprite A did, so Ad = sprite A's As there and
//   is computed here instead of read back. Checkpoint 132cp5: Ad reaches 0xff
//   (2.0), past a D3D blend factor, so the GS blend runs in the shader:
//   frame_cd (t2) holds the target's pixels under this sprite (draw() copies
//   them before each sprite), and the output is ((Cs - Cd) * Ad >> 7) + Cd
//   clamped to 0..0xff (COLCLAMP), written with blending off and the RGB mask.
//   Sprite B's alpha test GREATER 0 reads the ScreenFxBuf alpha (0x80, see
//   ps_copy) times Af. Checkpoint 132cp7: sprite A's frame alpha goes to the
//   scene's frame-alpha target (SV_Target1, red only), read later in the frame
//   by the corona reduce (003781b0); where sprite B's test fails the pixel
//   keeps cd instead of being discarded, so sprite A still writes its alpha.
Texture2D screen_fx : register(t1);
Texture2D frame_cd : register(t2);
struct HazeIn { float3 position : POSITION; float2 uv : TEXCOORD0; float2 st : TEXCOORD1; float4 colour : COLOR0; };
struct HazeOut { float4 position : SV_POSITION; float2 uv : TEXCOORD0; float2 st : TEXCOORD1; float4 colour : COLOR0; };
HazeOut vs_haze(HazeIn input) {
    HazeOut output;
    output.position = float4(input.position, 1.0);
    output.uv = input.uv;
    output.st = input.st;
    output.colour = input.colour;
    return output;
}
struct HazePixel { float4 colour : SV_Target0; float frame_alpha : SV_Target1; };
HazePixel ps_haze(HazeOut input) {
    HazePixel output;
    const float ad = min(floor(image.Sample(sampling, input.uv).a * 255.0 / 2.0 * input.colour.a), 255.0);
    output.frame_alpha = ad / 255.0;
    const float4 fx = screen_fx.Sample(sampling, input.st);
    const float3 cd = round(frame_cd.Load(int3(input.position.xy, 0)).rgb * 255.0);
    if (fx.a * 128.0 * input.colour.a < 1.0) {
        output.colour = float4(cd / 255.0, 0.0);
        return output;
    }
    const float3 cs = min(floor(fx.rgb * 255.0 * input.colour.rgb + 1.0 / 512.0), 255.0);
    output.colour = float4(clamp(floor((cs - cd) * ad / 128.0) + cd, 0.0, 255.0) / 255.0, 0.0);
    return output;
}
// 0037bd40: the frame to ScreenFxBuf with one bilinear sprite. TEX0_2 TCC 0
// with TFX DECAL: As is RGBAQ.A = 0x80 at every ScreenFxBuf pixel (1.0 here).
float4 ps_copy(VertexOut input) : SV_TARGET {
    return float4(image.Sample(sampling, input.uv).rgb, 1.0);
}
)";
constexpr std::size_t particle_source_size = sizeof(particle_source) - 1;

// The corona probe, reduce and element shaders (corona_renderer.hpp). Moved from corona_renderer.cpp unchanged (checkpoint 132mt1).
constexpr char corona_source[] = R"(
Texture2D image : register(t0);
Texture2D<float> visibility : register(t1);
SamplerState sampling : register(s0);
struct VertexIn { float4 position : POSITION; float2 uv : TEXCOORD0; float4 colour : COLOR0; float4 params : TEXCOORD1; };
struct VertexOut { float4 position : SV_POSITION; float2 uv : TEXCOORD0; float4 colour : COLOR0; float4 params : TEXCOORD1; };
VertexOut vs_main(VertexIn input) {
    VertexOut output;
    output.position = input.position;
    output.uv = input.uv;
    output.colour = input.colour;
    output.params = input.params;
    return output;
}
float ps_probe(VertexOut input) : SV_TARGET {
    return input.colour.a;
}
float sample32(float4 p, int x, int y) {
    return floor(image.SampleLevel(sampling, p.xy + p.zw * float2(x, y), 0).r * 255.0 + 1.0 / 512.0);
}
float ps_reduce(VertexOut input) : SV_TARGET {
    const float4 p = input.params;
    float s2 = 0.0;
    [loop] for (int a = 0; a < 4; ++a) {
        float s4 = 0.0;
        [loop] for (int b = 0; b < 4; ++b) {
            float s8 = 0.0;
            [loop] for (int c = 0; c < 4; ++c) {
                float s16 = 0.0;
                [loop] for (int d = 0; d < 4; ++d) {
                    const int x = (((a & 1) * 2 + (b & 1)) * 2 + (c & 1)) * 2 + (d & 1);
                    const int y = (((a >> 1) * 2 + (b >> 1)) * 2 + (c >> 1)) * 2 + (d >> 1);
                    float s32 = 0.0;
                    [unroll] for (int e = 0; e < 4; ++e) s32 += sample32(p, 2 * x + (e & 1), 2 * y + (e >> 1));
                    s16 += floor(s32 / 4.0);
                }
                s8 += floor(s16 / 4.0);
            }
            s4 += floor(s8 / 4.0);
        }
        s2 += floor(s4 / 4.0);
    }
    return floor(s2 / 4.0);
}
float4 ps_element(VertexOut input) : SV_TARGET {
    const float3 t = round(image.Sample(sampling, input.uv).rgb * 255.0);
    const float3 cs = min(floor(t * input.colour.rgb / 128.0), 255.0);
    const float vis = visibility.Load(int3(int(input.params.x), 0, 0));
    const float as = min(floor(vis * input.colour.a / 128.0), 255.0);
    return float4(floor(cs * as / 128.0) / 255.0, 0.0);
}
)";
constexpr std::size_t corona_source_size = sizeof(corona_source) - 1;

// The renderers' vertex structs (scene_renderer MeshVertex, gui_renderer / particle_renderer / corona_renderer
// HostVertex and the like), as their input layouts described them.
static_assert(sizeof(MeshVertex) == 60);
const D3D11_INPUT_ELEMENT_DESC mesh_elements[]{
    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(MeshVertex, position), D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(MeshVertex, normal), D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(MeshVertex, uv), D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(MeshVertex, color), D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(MeshVertex, gore), D3D11_INPUT_PER_VERTEX_DATA, 0},
};
// 132md8: gfx::GpuSkinVertex.
const D3D11_INPUT_ELEMENT_DESC skinned_elements[]{
    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"BLENDINDICES", 0, DXGI_FORMAT_R8G8B8A8_UINT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"BLENDINDICES", 1, DXGI_FORMAT_R8G8B8A8_UINT, 0, 36, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"BLENDWEIGHT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 40, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"BLENDWEIGHT", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 56, D3D11_INPUT_PER_VERTEX_DATA, 0},
};
const D3D11_INPUT_ELEMENT_DESC gui_elements[]{
    {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0},
};
const D3D11_INPUT_ELEMENT_DESC particle_elements[]{
    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0},
};
const D3D11_INPUT_ELEMENT_DESC clip_elements[]{
    {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
};
const D3D11_INPUT_ELEMENT_DESC haze_elements[]{
    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0},
};
const D3D11_INPUT_ELEMENT_DESC corona_elements[]{
    {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 40, D3D11_INPUT_PER_VERTEX_DATA, 0},
};

constexpr const char* scene_compile = "Compile native preview shader";
constexpr const char* scene_error = "Native preview shader compilation failed: ";
constexpr const char* particle_compile = "Compile particle shader";
constexpr const char* particle_error = "Particle shader compilation failed: ";
constexpr const char* corona_compile = "Compile corona shader";
constexpr const char* corona_error = "Corona shader compilation failed: ";

const HlslProgram programs[]{
    // scene_model
    {scene_source, scene_source_size, "native_preview_shader", "vertexMain", nullptr, "pixelMain", scene_compile,
     scene_error, "Create vertex shader", nullptr, "Create pixel shader"},
    // shadow_buffer
    {shadow_source, shadow_source_size, "native_preview_shader", "bufferVertex", nullptr, "bufferPixel", scene_compile,
     scene_error, "Create shadow buffer vertex shader", nullptr, "Create shadow buffer pixel shader"},
    // shadow_receiver
    {shadow_source, shadow_source_size, "native_preview_shader", "passVertex", "passGeometry", "passPixel", scene_compile,
     scene_error, "Create shadow pass vertex shader", "Create shadow pass geometry shader", "Create shadow pass pixel shader"},
    // gui
    {gui_source, gui_source_size, "gui_shader", "vs_main", nullptr, "ps_main", "Compile GUI shader",
     "GUI shader compilation failed: ", "Create GUI vertex shader", nullptr, "Create GUI pixel shader"},
    // particle_sprite
    {particle_source, particle_source_size, "particle_shader", "vs_main", nullptr, "ps_main", particle_compile,
     particle_error, "Create particle vertex shader", nullptr, "Create particle pixel shader"},
    // particle_clip
    {particle_source, particle_source_size, "particle_shader", "vs_clip", nullptr, "ps_main", particle_compile,
     particle_error, "Create shatter vertex shader", nullptr, "Create particle pixel shader"},
    // particle_haze
    {particle_source, particle_source_size, "particle_shader", "vs_haze", nullptr, "ps_haze", particle_compile,
     particle_error, "Create heat-haze vertex shader", nullptr, "Create heat-haze pixel shader"},
    // particle_copy
    {particle_source, particle_source_size, "particle_shader", "vs_main", nullptr, "ps_copy", particle_compile,
     particle_error, "Create particle vertex shader", nullptr, "Create ScreenFxBuf copy shader"},
    // corona_probe
    {corona_source, corona_source_size, "corona_shader", "vs_main", nullptr, "ps_probe", corona_compile, corona_error,
     "Create corona vertex shader", nullptr, "Create corona pixel shader"},
    // corona_reduce
    {corona_source, corona_source_size, "corona_shader", "vs_main", nullptr, "ps_reduce", corona_compile, corona_error,
     "Create corona vertex shader", nullptr, "Create corona pixel shader"},
    // corona_element
    {corona_source, corona_source_size, "corona_shader", "vs_main", nullptr, "ps_element", corona_compile, corona_error,
     "Create corona vertex shader", nullptr, "Create corona pixel shader"},
    // scene_skinned (132md8)
    {scene_skinned_source.c_str(), scene_skinned_source.size(), "native_preview_shader", "vertexSkinned", nullptr, "pixelMain",
     scene_compile, scene_error, "Create skinned vertex shader", nullptr, "Create pixel shader"},
    // shadow_skinned (132md8)
    {shadow_skinned_source.c_str(), shadow_skinned_source.size(), "native_preview_shader", "bufferSkinned", nullptr,
     "bufferPixel", scene_compile, scene_error, "Create skinned shadow buffer vertex shader", nullptr,
     "Create shadow buffer pixel shader"},
};
static_assert(std::size(programs) == program_count);

const HlslLayout layouts[]{
    {mesh_elements, static_cast<unsigned>(std::size(mesh_elements)), "Create model vertex layout"},
    {gui_elements, static_cast<unsigned>(std::size(gui_elements)), "Create GUI input layout"},
    {particle_elements, static_cast<unsigned>(std::size(particle_elements)), "Create particle input layout"},
    {clip_elements, static_cast<unsigned>(std::size(clip_elements)), "Create shatter input layout"},
    {haze_elements, static_cast<unsigned>(std::size(haze_elements)), "Create heat-haze input layout"},
    {corona_elements, static_cast<unsigned>(std::size(corona_elements)), "Create corona input layout"},
    {skinned_elements, static_cast<unsigned>(std::size(skinned_elements)), "Create skinned model input layout"},
};
static_assert(std::size(layouts) == static_cast<std::size_t>(VertexLayout::count));

}  // namespace

const HlslProgram& hlsl_program(Program program) {
    const auto k = static_cast<std::size_t>(program);
    if (k >= program_count) throw std::runtime_error("gfx: no such shader program");
    return programs[k];
}

const HlslLayout& hlsl_layout(VertexLayout layout) {
    const auto k = static_cast<std::size_t>(layout);
    if (k >= std::size(layouts)) throw std::runtime_error("gfx: no such vertex layout");
    return layouts[k];
}

}  // namespace omega::gfx::d3d11
