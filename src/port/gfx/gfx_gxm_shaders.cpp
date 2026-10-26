// Port-only (checkpoint 132vt1): the GXM backend's Cg programs, transcribed from the D3D11 HLSL of
// gfx_d3d11_shaders.cpp (same arithmetic, same constants; the comments there explain each program's GS / VU1
// origin). Differences forced by GXM are marked here: no geometry stage (shadow_receiver's guard-box / facing test
// runs on the CPU in gfx_gxm.cpp), one colour surface per scene (particle_haze's two targets are two fragment
// programs drawn in turn), no texture arrays (layers stacked vertically) and no Load() (texel-centre samples), no
// noperspective qualifier (I * w and w interpolated, divided per pixel = the screen-linear value).
#include "gfx_gxm_shaders.hpp"

#include "geometry.hpp"

#include <cstddef>
#include <iterator>
#include <stdexcept>

namespace omega::gfx::gxm {
namespace {

// The fragment epilogue every program shares (gfx_gxm_shaders.hpp: omegaBlendMul, OMEGA_FLOAT_OUT).
#define OMEGA_FRAGMENT_PRELUDE                                                                        \
    "float4 omega_blend(float4 c, float4 m) {\n"                                                      \
    "    float s = step(0.0, m.a);\n"                                                                   \
    "    c.rgb *= m.rgb; c.a = c.a * (1.0 - s) + m.a * s; return c;\n"                                  \
    "}\n"                                                                                               \
    "#ifdef OMEGA_FLOAT_OUT\n"                                                                          \
    "#define OMEGA_OUT float\n"                                                                         \
    "#define OMEGA_RETURN(c) return omega_blend(c, omegaBlendMul).r\n"                                  \
    "#else\n"                                                                                           \
    "#define OMEGA_OUT float4\n"                                                                        \
    "#define OMEGA_RETURN(c) return omega_blend(c, omegaBlendMul)\n"                                    \
    "#endif\n"

// ---- scene_model (scene_source vertexMain / pixelMain) ----
constexpr char scene_vs[] = R"(
void main(float3 position, float3 normal, float2 uv, float4 color, float3 gore,
          uniform float4 Scene[9],
          out float4 oPosition : POSITION, out float3 oNormal : TEXCOORD0, out float2 oUv : TEXCOORD1,
          out float4 oColor : TEXCOORD2, out float3 oGore : TEXCOORD3) {
    const float4 options = Scene[4];
    const float4 eye = Scene[5];
    const float4 up = Scene[6];
    const float4 uvOffset = Scene[8];
    oPosition = position.x * Scene[0] + position.y * Scene[1] + position.z * Scene[2] + Scene[3];
    oNormal = normal;
    oUv = uv - uvOffset.xy;
    if (options.w > 0.5 && options.w < 1.5) {
        float3 m = -normal;
        float3 t = cross(up.xyz, m);
        float3 b = cross(m, t);
        float3 v = normalize(eye.xyz - position);
        oUv = 0.5 + 0.5 * float2(dot(t, v), dot(b, v));
    } else if (options.w > 2.5) {
        oUv = uv - 1.0 / 64.0;
    }
    oColor = color;
    oGore = gore;
}
)";
constexpr char scene_ps[] = OMEGA_FRAGMENT_PRELUDE R"(
OMEGA_OUT main(float3 vNormal : TEXCOORD0, float2 vUv : TEXCOORD1, float4 vColor : TEXCOORD2, float3 vGore : TEXCOORD3,
               uniform sampler2D surfaceTexture : TEXUNIT0, uniform float4 Scene[9], uniform float4 omegaBlendMul) : COLOR {
    const float4 options = Scene[4];
    const float4 alphaTest = Scene[7];
    if (options.y > 0.5) { OMEGA_RETURN(float4(1.0, 0.35, 0.05, 1.0)); }
    float4 color = vColor;
    float4 texel = float4(1.0, 1.0, 1.0, 1.0);
    if (options.x > 0.5) {
        texel = tex2D(surfaceTexture, vUv);
        color *= texel;
    }
    bool raw = alphaTest.z > 0.5;
#ifndef OMEGA_NO_ALPHA_TEST  // 132vg2: the GXM backend's variant for draws without an alpha test (no discard)
    if (alphaTest.w > 0.5) {
        float at = ceil(color.a * 127.5 - 0.001);
        float as = floor(at * (alphaTest.w - 1.0) / 128.0);
        bool passed = as >= alphaTest.x;
        if (passed == (alphaTest.y > 0.5)) discard;
        color.a = as / 128.0;
    } else if (alphaTest.x >= 0.0) {
        bool passed = round(color.a * (raw ? 255.0 : 127.5)) > alphaTest.x;
        if (passed == (alphaTest.y > 0.5)) discard;
    }
#endif
    float lengthSquared = dot(vNormal, vNormal);
    float lighting = 0.28;
    float3 rgb = color.rgb;
    if (options.z <= 0.5) {
        if (lengthSquared > 0.00001)
            lighting += 0.72 * abs(dot(vNormal * rsqrt(lengthSquared), normalize(float3(-0.35, 0.5, 0.8))));
        rgb *= lighting;
    }
    if (any(vGore > 0.0)) {
        float3 lit = vColor.rgb * (options.z <= 0.5 ? lighting : 1.0);
        rgb = max(lit - vGore, 0.0) * texel.rgb;
    }
    if (raw) {
        float as128 = color.a * 255.0 / 128.0;
        OMEGA_RETURN(float4(saturate(saturate(rgb) * as128), saturate(as128)));
    }
    OMEGA_RETURN(saturate(float4(rgb, color.a)));
}
)";

// ---- shadow_buffer (shadow_source bufferVertex / bufferPixel) ----
constexpr char shadow_buffer_vs[] = R"(
void main(float3 position, float2 uv, float4 color,
          uniform float4 Shadow[8],
          out float4 oPosition : POSITION, out float2 oUv : TEXCOORD0, out float4 oColor : TEXCOORD1) {
    const float4 scale = Shadow[4];
    const float4 offset = Shadow[5];
    float4 X = position.x * Shadow[0] + position.y * Shadow[1] + position.z * Shadow[2] + Shadow[3];
    oPosition = float4((2.0 * offset.x - 1.0) * X.w + 2.0 * scale.x * X.x,
                       (1.0 - 2.0 * offset.y) * X.w - 2.0 * scale.y * X.y, 0.5 * X.w, X.w);
    oUv = uv;
    oColor = color;
}
)";
constexpr char shadow_buffer_ps[] = OMEGA_FRAGMENT_PRELUDE R"(
OMEGA_OUT main(float2 vUv : TEXCOORD0, float4 vColor : TEXCOORD1,
               uniform sampler2D surfaceTexture : TEXUNIT0, uniform float4 Shadow[8], uniform float4 omegaBlendMul) : COLOR {
    const float4 params = Shadow[7];
    if (params.y < 1.5) { OMEGA_RETURN(float4(params.xxx, 128.0 / 255.0)); }
    float4 color = vColor;
    if (params.w > 0.5) color *= tex2D(surfaceTexture, vUv);
    OMEGA_RETURN(float4(saturate(color.rgb), saturate(color.a) * 128.0 / 255.0));
}
)";

// ---- shadow_receiver (shadow_source passVertex / passPixel; passGeometry runs on the CPU) ----
// HLSL `noperspective float intensity`: oIntensityW = I * w and oW = w are interpolated perspective-correct, so
// their ratio per pixel is I interpolated linearly in screen space.
constexpr char shadow_pass_vs[] = R"(
void main(float3 position,
          uniform float4 Scene[9], uniform float4 Shadow[8],
          out float4 oPosition : POSITION, out float4 oX : TEXCOORD0, out float oIntensityW : TEXCOORD1,
          out float oW : TEXCOORD2) {
    const float4 scale = Shadow[4];
    const float4 offset = Shadow[5];
    const float4 params = Shadow[7];
    oPosition = position.x * Scene[0] + position.y * Scene[1] + position.z * Scene[2] + Scene[3];
    oX = position.x * Shadow[0] + position.y * Shadow[1] + position.z * Shadow[2] + Shadow[3];
    float intensity = params.y > 2.5 ? trunc(offset.z) : trunc(offset.z + scale.z * oX.z / oX.w);
    oIntensityW = intensity * oPosition.w;
    oW = oPosition.w;
}
)";
constexpr char shadow_pass_ps[] = OMEGA_FRAGMENT_PRELUDE R"(
OMEGA_OUT main(float4 vX : TEXCOORD0, float vIntensityW : TEXCOORD1, float vW : TEXCOORD2,
               uniform sampler2D shadowBuffers : TEXUNIT1, uniform sampler2D projectorTexture : TEXUNIT2,
               uniform float4 Shadow[8], uniform float4 omegaTexSize[4], uniform float4 omegaLayers,
               uniform float4 omegaBlendMul) : COLOR {
    const float4 scale = Shadow[4];
    const float4 offset = Shadow[5];
    const float4 params = Shadow[7];
    float inputIntensity = vIntensityW / vW;
    float2 uv = offset.xy + scale.xy * vX.xy / vX.w;
    if (params.y > 2.5) {
        float4 t = round(tex2D(projectorTexture, uv) * 255.0);
        float raw = params.w > 0.5 ? t.a : (t.a >= 254.5 ? 128.0 : floor(t.a / 2.0));
        float a3 = floor(raw * floor(inputIntensity) / 128.0);
        if (a3 <= 0.0) discard;
        OMEGA_RETURN(float4(floor(t.rgb * a3 / 128.0) / 255.0, 0.0));
    }
    // Texture2DArray layer params.z: the layers are stacked vertically (clamp addressing kept inside the layer).
    float layers = max(omegaLayers.y, 1.0);
    float layer = clamp(round(params.z), 0.0, layers - 1.0);
    float halfTexel = 0.5 * omegaTexSize[1].w;
    float v = clamp(uv.y, halfTexel, 1.0 - halfTexel);
    float4 texel = round(tex2D(shadowBuffers, float2(uv.x, (v + layer) / layers)) * 255.0);
    float intensity = floor(inputIntensity);
    float alpha = floor(texel.a * intensity / 128.0);
    if (alpha <= 0.0) discard;
    if (params.y < 1.5) { OMEGA_RETURN(float4(floor(texel.rgb * intensity / 128.0) / 255.0, 0.0)); }
    OMEGA_RETURN(float4(texel.rgb / 255.0, min(alpha / 128.0, 1.0)));
}
)";

// ---- gui (gui_source) ----
constexpr char gui_vs[] = R"(
void main(float2 position, float2 uv, float4 colour, float2 depth,
          out float4 oPosition : POSITION, out float2 oUv : TEXCOORD0, out float4 oColour : TEXCOORD1) {
    oPosition = float4(position.x / 320.0, position.y / 224.0, depth.x, depth.y);
    oUv = uv;
    oColour = colour;
}
)";
constexpr char gui_ps[] = OMEGA_FRAGMENT_PRELUDE R"(
OMEGA_OUT main(float2 vUv : TEXCOORD0, float4 vColour : TEXCOORD1,
               uniform sampler2D image : TEXUNIT0, uniform float4 omegaBlendMul) : COLOR {
    OMEGA_RETURN(saturate(tex2D(image, vUv) * vColour));
}
)";

// ---- particles (particle_source) ----
constexpr char particle_vs[] = R"(
void main(float3 position, float2 uv, float4 colour,
          out float4 oPosition : POSITION, out float2 oUv : TEXCOORD0, out float4 oColour : TEXCOORD1) {
    oPosition = float4(position, 1.0);
    oUv = uv;
    oColour = colour;
}
)";
constexpr char clip_vs[] = R"(
void main(float4 position, float2 uv, float4 colour,
          out float4 oPosition : POSITION, out float2 oUv : TEXCOORD0, out float4 oColour : TEXCOORD1) {
    oPosition = position;
    oUv = uv;
    oColour = colour;
}
)";
constexpr char particle_ps[] = OMEGA_FRAGMENT_PRELUDE R"(
OMEGA_OUT main(float2 vUv : TEXCOORD0, float4 vColour : TEXCOORD1,
               uniform sampler2D image : TEXUNIT0, uniform float4 omegaBlendMul) : COLOR {
    float4 texel = tex2D(image, vUv);
    if (texel.a * 255.0 * vColour.a < 2.0) discard;
    OMEGA_RETURN(saturate(texel * vColour));
}
)";
constexpr char haze_vs[] = R"(
void main(float3 position, float2 uv, float2 st, float4 colour,
          out float4 oPosition : POSITION, out float2 oUv : TEXCOORD0, out float2 oSt : TEXCOORD1,
          out float4 oColour : TEXCOORD2) {
    oPosition = float4(position, 1.0);
    oUv = uv;
    oSt = st;
    oColour = colour;
}
)";
// ps_haze's SV_Target0 (the blended colour). frame_cd.Load(position.xy) is the texel-centre sample at the pixel.
constexpr char haze_colour_ps[] = OMEGA_FRAGMENT_PRELUDE R"(
OMEGA_OUT main(float2 vUv : TEXCOORD0, float2 vSt : TEXCOORD1, float4 vColour : TEXCOORD2, float4 wpos : WPOS,
               uniform sampler2D image : TEXUNIT0, uniform sampler2D screen_fx : TEXUNIT1,
               uniform sampler2D frame_cd : TEXUNIT2, uniform float4 omegaTexSize[4], uniform float4 omegaBlendMul) : COLOR {
    float ad = min(floor(tex2D(image, vUv).a * 255.0 / 2.0 * vColour.a), 255.0);
    float4 fx = tex2D(screen_fx, vSt);
    float2 pixel = (floor(wpos.xy) + 0.5) * omegaTexSize[2].zw;
    float3 cd = round(tex2D(frame_cd, pixel).rgb * 255.0);
    if (fx.a * 128.0 * vColour.a < 1.0) { OMEGA_RETURN(float4(cd / 255.0, 0.0)); }
    float3 cs = min(floor(fx.rgb * 255.0 * vColour.rgb + 1.0 / 512.0), 255.0);
    OMEGA_RETURN(float4(clamp(floor((cs - cd) * ad / 128.0) + cd, 0.0, 255.0) / 255.0, 0.0));
}
)";
// ps_haze's SV_Target1 (the frame alpha, red only).
constexpr char haze_alpha_ps[] = OMEGA_FRAGMENT_PRELUDE R"(
OMEGA_OUT main(float2 vUv : TEXCOORD0, float2 vSt : TEXCOORD1, float4 vColour : TEXCOORD2,
               uniform sampler2D image : TEXUNIT0, uniform float4 omegaBlendMul) : COLOR {
    float ad = min(floor(tex2D(image, vUv).a * 255.0 / 2.0 * vColour.a), 255.0);
    OMEGA_RETURN(float4(ad / 255.0, 0.0, 0.0, 0.0));
}
)";
constexpr char particle_copy_ps[] = OMEGA_FRAGMENT_PRELUDE R"(
OMEGA_OUT main(float2 vUv : TEXCOORD0, float4 vColour : TEXCOORD1,
               uniform sampler2D image : TEXUNIT0, uniform float4 omegaBlendMul) : COLOR {
    OMEGA_RETURN(float4(tex2D(image, vUv).rgb, 1.0));
}
)";

// ---- corona (corona_source) ----
constexpr char corona_vs[] = R"(
void main(float4 position, float2 uv, float4 colour, float4 params,
          out float4 oPosition : POSITION, out float2 oUv : TEXCOORD0, out float4 oColour : TEXCOORD1,
          out float4 oParams : TEXCOORD2) {
    oPosition = position;
    oUv = uv;
    oColour = colour;
    oParams = params;
}
)";
constexpr char probe_ps[] = OMEGA_FRAGMENT_PRELUDE R"(
OMEGA_OUT main(float4 vColour : TEXCOORD1, uniform float4 omegaBlendMul) : COLOR {
    OMEGA_RETURN(float4(vColour.a, 0.0, 0.0, 0.0));
}
)";
constexpr char reduce_ps[] = OMEGA_FRAGMENT_PRELUDE R"(
float sample32(sampler2D image, float4 p, float x, float y) {
    return floor(tex2D(image, p.xy + p.zw * float2(x, y)).r * 255.0 + 1.0 / 512.0);
}
OMEGA_OUT main(float4 vParams : TEXCOORD2, uniform sampler2D image : TEXUNIT0, uniform float4 omegaBlendMul) : COLOR {
    const float4 p = vParams;
    float s2 = 0.0;
    for (float a = 0.0; a < 3.5; a += 1.0) {
        float s4 = 0.0;
        for (float b = 0.0; b < 3.5; b += 1.0) {
            float s8 = 0.0;
            for (float c = 0.0; c < 3.5; c += 1.0) {
                float s16 = 0.0;
                for (float d = 0.0; d < 3.5; d += 1.0) {
                    // HLSL: x from the low bits (& 1), y from the high bits (>> 1) of a, b, c, d.
                    float x = fmod(a, 2.0) * 8.0 + fmod(b, 2.0) * 4.0 + fmod(c, 2.0) * 2.0 + fmod(d, 2.0);
                    float y = floor(a / 2.0) * 8.0 + floor(b / 2.0) * 4.0 + floor(c / 2.0) * 2.0 + floor(d / 2.0);
                    float s32 = sample32(image, p, 2.0 * x, 2.0 * y) + sample32(image, p, 2.0 * x + 1.0, 2.0 * y) +
                                sample32(image, p, 2.0 * x, 2.0 * y + 1.0) +
                                sample32(image, p, 2.0 * x + 1.0, 2.0 * y + 1.0);
                    s16 += floor(s32 / 4.0);
                }
                s8 += floor(s16 / 4.0);
            }
            s4 += floor(s8 / 4.0);
        }
        s2 += floor(s4 / 4.0);
    }
    OMEGA_RETURN(float4(floor(s2 / 4.0), 0.0, 0.0, 0.0));
}
)";
constexpr char element_ps[] = OMEGA_FRAGMENT_PRELUDE R"(
OMEGA_OUT main(float2 vUv : TEXCOORD0, float4 vColour : TEXCOORD1, float4 vParams : TEXCOORD2,
               uniform sampler2D image : TEXUNIT0, uniform sampler2D visibility : TEXUNIT1,
               uniform float4 omegaTexSize[4], uniform float4 omegaBlendMul) : COLOR {
    float3 t = round(tex2D(image, vUv).rgb * 255.0);
    float3 cs = min(floor(t * vColour.rgb / 128.0), 255.0);
    float vis = tex2D(visibility, float2((floor(vParams.x) + 0.5) * omegaTexSize[1].z, 0.5 * omegaTexSize[1].w)).r;
    float as = min(floor(vis * vColour.a / 128.0), 255.0);
    OMEGA_RETURN(float4(floor(cs * as / 128.0) / 255.0, 0.0));
}
)";

// ---- backend-internal: clears and texture copies ----
constexpr char clear_vs[] = R"(
void main(float2 position, uniform float4 omegaClear, out float4 oPosition : POSITION) {
    oPosition = float4(position, omegaClear.x, 1.0);
}
)";
constexpr char clear_ps[] = OMEGA_FRAGMENT_PRELUDE R"(
OMEGA_OUT main(uniform float4 omegaColour, uniform float4 omegaBlendMul) : COLOR {
    OMEGA_RETURN(omegaColour);
}
)";
constexpr char copy_vs[] = R"(
void main(float2 position, float2 uv, out float4 oPosition : POSITION, out float2 oUv : TEXCOORD0) {
    oPosition = float4(position, 0.0, 1.0);
    oUv = uv;
}
)";
constexpr char copy_ps[] = OMEGA_FRAGMENT_PRELUDE R"(
OMEGA_OUT main(float2 vUv : TEXCOORD0, uniform sampler2D image : TEXUNIT0, uniform float4 omegaBlendMul) : COLOR {
    OMEGA_RETURN(tex2D(image, vUv));
}
)";

const GxmStage vertex_stages[]{
    {"scene_vs", scene_vs},   {"shadow_buffer_vs", shadow_buffer_vs}, {"shadow_pass_vs", shadow_pass_vs},
    {"gui_vs", gui_vs},       {"particle_vs", particle_vs},           {"clip_vs", clip_vs},
    {"haze_vs", haze_vs},     {"corona_vs", corona_vs},
};
const GxmStage fragment_stages[]{
    {"scene_ps", scene_ps},           {"shadow_buffer_ps", shadow_buffer_ps}, {"shadow_pass_ps", shadow_pass_ps},
    {"gui_ps", gui_ps},               {"particle_ps", particle_ps},           {"haze_colour_ps", haze_colour_ps},
    {"haze_alpha_ps", haze_alpha_ps}, {"particle_copy_ps", particle_copy_ps}, {"probe_ps", probe_ps},
    {"reduce_ps", reduce_ps},         {"element_ps", element_ps},
};
const GxmStage clear_vertex{"clear_vs", clear_vs};
const GxmStage clear_fragment{"clear_ps", clear_ps};
const GxmStage copy_vertex{"copy_vs", copy_vs};
const GxmStage copy_fragment{"copy_ps", copy_ps};

constexpr unsigned none = ~0U;
const GxmProgram programs[]{
    {0, 0, none, {"Scene", nullptr}, false, {0, 0, 0, 0}},     // scene_model
    {1, 1, none, {"Scene", "Shadow"}, false, {0, 0, 0, 0}},    // shadow_buffer
    {2, 2, none, {"Scene", "Shadow"}, true, {0, 1, 1, 0}},     // shadow_receiver (t1, t2 with s1)
    {3, 3, none, {nullptr, nullptr}, false, {0, 0, 0, 0}},     // gui
    {4, 4, none, {nullptr, nullptr}, false, {0, 0, 0, 0}},     // particle_sprite
    {5, 4, none, {nullptr, nullptr}, false, {0, 0, 0, 0}},     // particle_clip
    {6, 5, 6, {nullptr, nullptr}, false, {0, 0, 0, 0}},        // particle_haze
    {4, 7, none, {nullptr, nullptr}, false, {0, 0, 0, 0}},     // particle_copy
    {7, 8, none, {nullptr, nullptr}, false, {0, 0, 0, 0}},     // corona_probe
    {7, 9, none, {nullptr, nullptr}, false, {0, 0, 0, 0}},     // corona_reduce
    {7, 10, none, {nullptr, nullptr}, false, {0, 0, 0, 0}},    // corona_element
};
static_assert(std::size(programs) == gxm_program_count);

static_assert(sizeof(MeshVertex) == 60);
const GxmAttribute mesh_attributes[]{
    {"position", offsetof(MeshVertex, position), 3}, {"normal", offsetof(MeshVertex, normal), 3},
    {"uv", offsetof(MeshVertex, uv), 2},             {"color", offsetof(MeshVertex, color), 4},
    {"gore", offsetof(MeshVertex, gore), 3},
};
const GxmAttribute gui_attributes[]{{"position", 0, 2}, {"uv", 8, 2}, {"colour", 16, 4}, {"depth", 32, 2}};
const GxmAttribute particle_attributes[]{{"position", 0, 3}, {"uv", 12, 2}, {"colour", 20, 4}};
const GxmAttribute clip_attributes[]{{"position", 0, 4}, {"uv", 16, 2}, {"colour", 24, 4}};
const GxmAttribute haze_attributes[]{{"position", 0, 3}, {"uv", 12, 2}, {"st", 20, 2}, {"colour", 28, 4}};
const GxmAttribute corona_attributes[]{{"position", 0, 4}, {"uv", 16, 2}, {"colour", 24, 4}, {"params", 40, 4}};

const GxmLayout layouts[]{
    {mesh_attributes, static_cast<unsigned>(std::size(mesh_attributes))},
    {gui_attributes, static_cast<unsigned>(std::size(gui_attributes))},
    {particle_attributes, static_cast<unsigned>(std::size(particle_attributes))},
    {clip_attributes, static_cast<unsigned>(std::size(clip_attributes))},
    {haze_attributes, static_cast<unsigned>(std::size(haze_attributes))},
    {corona_attributes, static_cast<unsigned>(std::size(corona_attributes))},
};
static_assert(std::size(layouts) == gxm_layout_count);

}  // namespace

const GxmProgram& gxm_program(Program program) {
    const auto k = static_cast<std::size_t>(program);
    if (k >= gxm_program_count)
        throw std::runtime_error("gfx (GXM / vitaGL): program not ported (the GPU skinning programs; gpu_skinning is off)");
    return programs[k];
}

const GxmLayout& gxm_layout(VertexLayout layout) {
    const auto k = static_cast<std::size_t>(layout);
    if (k >= gxm_layout_count)
        throw std::runtime_error("gfx (GXM / vitaGL): vertex layout not ported (the GPU skinning layout)");
    return layouts[k];
}

const GxmStage& gxm_vertex_stage(unsigned index) {
    if (index == gxm_clear_vertex) return clear_vertex;
    if (index == gxm_copy_vertex) return copy_vertex;
    if (index >= std::size(vertex_stages)) throw std::runtime_error("gfx: no such GXM vertex stage");
    return vertex_stages[index];
}

const GxmStage& gxm_fragment_stage(unsigned index) {
    if (index == gxm_clear_fragment) return clear_fragment;
    if (index == gxm_copy_fragment) return copy_fragment;
    if (index >= std::size(fragment_stages)) throw std::runtime_error("gfx: no such GXM fragment stage");
    return fragment_stages[index];
}

}  // namespace omega::gfx::gxm
