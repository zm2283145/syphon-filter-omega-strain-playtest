// Port-only (checkpoint 132vt1): the TEMPORARY vitaGL backend of gfx::Device on the PS Vita. It implements the same
// interface as the designated GXM backend (gfx_gxm.hpp) over vitaGL's GL calls, using the same Cg programs
// (gfx_gxm_shaders.cpp). It was brought up because the direct GXM backend renders black on Vita3K
// (research/VITA.md "Renderer" and "The black-screen investigation"); the build selects it with
// OMEGA_VITA_RENDERER=vitagl (the default until the GXM backend shows pixels). The GXM backend stays in the tree.
//
// vitaGL differences handled here (VITA.md "Renderer: vitaGL backend"):
// - Render targets are GL framebuffer objects, one per (colour texture, depth texture) pair. vitaGL keeps a hidden
//   depth buffer per framebuffer object, so a D3D depth texture used with two colour targets is two depth buffers:
//   its clears are replayed on each (a clear serial per depth texture).
// - vitaGL flips framebuffer-object rendering vertically (GL's bottom-up rows): every vertex program gets an
//   injected omegaVgl uniform that negates clip-space y for framebuffer objects, so texture row 0 stays the D3D top
//   row everywhere (sampling, WPOS, read-back).
// - D3D's 0..1 clip depth: glDepthRangef(2 * min - max, max) gives window z = min + z * (max - min).
// - A vertex uniform's write also writes the same-named fragment uniform in vitaGL, so the fragment programs' Scene
//   and Shadow arrays are renamed SceneF and ShadowF.
// - The D3D blend factor, shadow_receiver's geometry stage and particle_haze's two targets are handled as in the
//   GXM backend (blend_for(), ShadowFilter, two passes).
// - All textures are GL_RGBA (bgra8 swizzled at upload and read-back; r8 expanded to (r, 0, 0, 1)); r32f is stored
//   as 8-bit (lossy: the corona visibility reduction saturates; VITA.md).
#include "gfx_gxm.hpp"

#include "gfx_gxm_shaders.hpp"
#include "zone_profile.hpp"

#include <psp2/common_dialog.h>
#include <psp2/io/stat.h>
#include <psp2/message_dialog.h>
#include <vitaGL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace omega::gfx::gxm {
namespace {

constexpr unsigned display_width = 960, display_height = 544;
std::size_t align_up(std::size_t v, std::size_t a) { return (v + a - 1) / a * a; }

std::recursive_mutex g_mutex;  // every vitaGL call (the render thread, the loader's uploads, captures)
class Impl;
Impl* g_impl = nullptr;

// ---- resources ----

struct VglBuffer final : Buffer {
    explicit VglBuffer(const BufferDesc& d) { desc_ = d; }
    ~VglBuffer() override;
    std::vector<std::uint8_t> cpu;  // every buffer's bytes (constants are only here; the shadow filter reads vertices)
    GLuint gl = 0;                  // vertex / index buffers
};

struct VglTexture final : Texture {
    VglTexture() = default;
    ~VglTexture() override;
    GLuint gl = 0;
    unsigned layer_rows = 0;  // rows per layer (layers stacked vertically, as the GXM backend)
    bool depth = false;       // d24s8: no GL object (vitaGL keeps the depth buffer in each framebuffer object)
    std::uint32_t clear_serial = 0;
    float clear_value = 1.0F;
    unsigned rows() const { return layer_rows * std::max(desc_.layers, 1u); }
    std::size_t stride() const { return align_up(std::max(desc_.width, 1u), 8) * 4; }  // vitaGL's linear RGBA rows
    void set_desc(const TextureDesc& d) { desc_ = d; }
};

struct VglBlend final : BlendState {
    BlendDesc desc;
};
struct VglDepth final : DepthState {
    DepthDesc desc;
};
struct VglRaster final : RasterState {
    RasterDesc desc;
};
struct VglSampler final : SamplerState {
    SamplerDesc desc;
};

// ---- programs ----

bool is_word(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

std::string replace_word(std::string s, const std::string& from, const std::string& to) {
    for (std::size_t k = s.find(from); k != std::string::npos; k = s.find(from, k)) {
        const bool start = k == 0 || !is_word(s[k - 1]);
        const bool end = k + from.size() >= s.size() || !is_word(s[k + from.size()]);
        if (start && end) {
            s.replace(k, from.size(), to);
            k += to.size();
        } else {
            k += from.size();
        }
    }
    return s;
}

// The element count of `uniform float4 name[N]` (1 when not an array, 0 when absent).
unsigned declared_count(const std::string& source, const std::string& name) {
    const std::string head = "uniform float4 " + name;
    for (std::size_t k = source.find(head); k != std::string::npos; k = source.find(head, k + 1)) {
        const std::size_t after = k + head.size();
        if (after < source.size() && is_word(source[after])) continue;
        if (after < source.size() && source[after] == '[') return static_cast<unsigned>(std::strtoul(source.c_str() + after + 1, nullptr, 10));
        return 1;
    }
    return 0;
}

// The vertex source with the framebuffer-object flip (omegaVgl.x = -1 in framebuffer objects, +1 on the display).
std::string vertex_source(const char* source) {
    std::string s = source;
    const std::size_t main = s.find("void main(");
    const std::size_t close = s.rfind('}');
    if (main == std::string::npos || close == std::string::npos || close < main)
        throw std::runtime_error("gfx vitagl: a vertex program without main()");
    s.insert(close, "    oPosition.y = oPosition.y * omegaVgl.x;\n");
    s.replace(main, 10, "void main(uniform float4 omegaVgl, ");
    return s;
}

std::string fragment_source(const char* source) {
    return replace_word(replace_word(source, "Scene", "SceneF"), "Shadow", "ShadowF");
}

struct CompiledShader {
    GLuint gl = 0;
    std::string source;  // as compiled
};

struct GlProgram {
    GLuint gl = 0;
    GLint vertex_constants[2]{-1, -1}, fragment_constants[2]{-1, -1};
    unsigned vertex_count[2]{}, fragment_count[2]{};
    GLint tex_size = -1, layers = -1, blend_mul = -1, colour = -1, clear = -1, flip = -1;
    int attribute[8]{-1, -1, -1, -1, -1, -1, -1, -1};
};

// ---- pipeline description of one draw ----

struct UniformBlock {
    const void* data = nullptr;
    std::size_t bytes = 0;
};
struct BoundTexture {
    const VglTexture* texture = nullptr;
    SamplerDesc sampler{};
};
struct Pipeline {
    unsigned vertex = 0, fragment = 0;
    const char* constants[2]{};
    const GxmAttribute* attributes = nullptr;
    unsigned attribute_count = 0;
    std::uint32_t stride = 0;
    GLuint vertex_buffer = 0;          // 0: client vertices
    std::size_t vertex_offset = 0;
    const void* client_vertices = nullptr;
    bool indexed = false;
    GLuint index_buffer = 0;           // 0: client indices
    const std::uint32_t* client_indices = nullptr;
    std::uint32_t first = 0, count = 0;
    bool blend = false;
    GLenum colour_src = GL_ONE, colour_dst = GL_ZERO, alpha_src = GL_ONE, alpha_dst = GL_ZERO;
    GLenum colour_op = GL_FUNC_ADD, alpha_op = GL_FUNC_ADD;
    std::uint8_t mask = 0xf;
    float blend_mul[4]{1, 1, 1, -1};
    bool depth_test = false;
    GLenum depth_func = GL_ALWAYS;
    bool depth_write = false;
    bool wireframe = false;
    Viewport viewport{};
    BoundTexture textures[4]{};
    UniformBlock vertex_constants[2]{}, fragment_constants[2]{};
    float colour[4]{};
    float clear[4]{};
};

const GxmAttribute clear_attributes[]{{"position", 0, 2}};
const GxmAttribute copy_attributes[]{{"position", 0, 2}, {"uv", 8, 2}};
const float clear_quad[]{-1, -1, 1, -1, -1, 1, -1, 1, 1, -1, 1, 1};

GLenum gl_factor(BlendFactor f) {
    switch (f) {
    case BlendFactor::zero: return GL_ZERO;
    case BlendFactor::one: return GL_ONE;
    case BlendFactor::src_alpha: return GL_SRC_ALPHA;
    case BlendFactor::inv_src_alpha: return GL_ONE_MINUS_SRC_ALPHA;
    default: return GL_ONE;
    }
}
bool is_factor(BlendFactor f) { return f == BlendFactor::factor || f == BlendFactor::inv_factor; }

// gfx_gxm.cpp blend_for() with GL factors: the same folding of the D3D blend factor into omegaBlendMul.
void blend_for(const TargetBlend& t, const float factor[4], Pipeline& p) {
    p.blend_mul[0] = p.blend_mul[1] = p.blend_mul[2] = 1.0F;
    p.blend_mul[3] = -1.0F;
    p.mask = t.write_mask & 0xf;
    p.blend = t.enable;
    if (!t.enable) return;
    p.colour_op = t.op == BlendOp::add ? GL_FUNC_ADD : GL_FUNC_REVERSE_SUBTRACT;
    p.alpha_op = t.op_alpha == BlendOp::add ? GL_FUNC_ADD : GL_FUNC_REVERSE_SUBTRACT;
    GLenum src = gl_factor(t.src), dst = gl_factor(t.dst);
    bool fix_over = false;
    if (is_factor(t.src)) {
        float f[3];
        for (int k = 0; k < 3; ++k) f[k] = t.src == BlendFactor::factor ? factor[k] : 1.0F - factor[k];
        if (!is_factor(t.dst)) {
            src = GL_ONE;
            for (int k = 0; k < 3; ++k) p.blend_mul[k] = f[k];
        } else if (t.src == BlendFactor::factor && t.dst == BlendFactor::inv_factor && factor[0] == factor[1] &&
                   factor[1] == factor[2]) {
            src = GL_SRC_ALPHA;
            dst = GL_ONE_MINUS_SRC_ALPHA;
            p.blend_mul[3] = factor[0];
            fix_over = true;
        } else {
            throw std::runtime_error("gfx vitagl: unported blend (factor / factor combination other than FIX over)");
        }
    } else if (is_factor(t.dst)) {
        throw std::runtime_error("gfx vitagl: unported blend (destination blend factor)");
    }
    p.colour_src = src;
    p.colour_dst = dst;
    GLenum asrc = gl_factor(t.src_alpha), adst = gl_factor(t.dst_alpha);
    if (is_factor(t.src_alpha) || is_factor(t.dst_alpha)) {
        if (!(t.write_mask & mask_a)) {
            asrc = GL_ONE;
            adst = GL_ZERO;
        } else if (fix_over) {
            asrc = GL_SRC_ALPHA;
            adst = GL_ONE_MINUS_SRC_ALPHA;
        } else {
            throw std::runtime_error("gfx vitagl: unported blend (alpha blend factor)");
        }
    }
    p.alpha_src = asrc;
    p.alpha_dst = adst;
}

GLenum gl_compare(Compare c) {
    switch (c) {
    case Compare::never: return GL_NEVER;
    case Compare::less: return GL_LESS;
    case Compare::less_equal: return GL_LEQUAL;
    default: return GL_ALWAYS;
    }
}

// gfx_gxm.cpp ShadowFilter: shadow_receiver's geometry shader on the CPU.
struct ShadowFilter {
    float clip[16];
    float facing[4];
    bool outside(const float* p) const {
        float X[4];
        for (int k = 0; k < 4; ++k) X[k] = p[0] * clip[k] + p[1] * clip[4 + k] + p[2] * clip[8 + k] + clip[12 + k];
        const float w = std::fabs(X[3]);
        return std::fabs(X[0]) > w || std::fabs(X[1]) > w || std::fabs(X[2]) > w;
    }
    bool keep(const std::uint8_t* v0, const std::uint8_t* v1, const std::uint8_t* v2) const {
        float a[3], b[3], c[3], n[3];
        std::memcpy(a, v0, sizeof a);
        std::memcpy(b, v1, sizeof b);
        std::memcpy(c, v2, sizeof c);
        if (outside(a) || outside(b) || outside(c)) return false;
        std::memcpy(n, v2 + 12, sizeof n);  // MeshVertex::normal
        return facing[0] * n[0] + facing[1] * n[1] + facing[2] * n[2] < 0.0F;
    }
};

// The draw state (D3D11-style: it persists across submitted lists until changed or reset).
struct State {
    const VglTexture* colour[2]{};
    unsigned layer[2]{};
    const VglTexture* depth = nullptr;
    Viewport viewport{};
    const VglRaster* raster = nullptr;
    const VglDepth* depth_state = nullptr;
    const VglBlend* blend = nullptr;
    float factor[4]{1, 1, 1, 1};
    Program program{};
    bool has_program = false;
    const VglBuffer* vertices = nullptr;
    std::uint32_t stride = 0, offset = 0;
    const VglBuffer* indices = nullptr;
    const VglBuffer* vertex_constants[4]{};
    const VglBuffer* geometry_constants[4]{};
    const VglBuffer* pixel_constants[4]{};
    const VglTexture* textures[4]{};
    const VglSampler* samplers[4]{};
};

struct Framebuffer {
    GLuint fbo = 0;
    GLuint depth_buffer = 0;
    std::shared_ptr<VglTexture> dummy_colour;  // a depth-only target's colour attachment
    std::uint32_t applied_clear = 0;           // the depth texture's clear serial last drawn into this buffer
};

bool file_exists(const char* path) {
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}

void make_directories(const std::string& path) {
    for (std::size_t k = path.find('/'); k != std::string::npos; k = path.find('/', k + 1))
        sceIoMkdir(path.substr(0, k).c_str(), 0777);
    sceIoMkdir(path.c_str(), 0777);
}

class Impl {
public:
    explicit Impl(const Options& o);

    Options options;
    MemoryStats stats;

    // ---- programs ----
    const CompiledShader& shader(bool vertex, unsigned index);
    GlProgram& program(const Pipeline& p);

    // ---- targets and draws ----
    void use_target(const VglTexture* colour, const VglTexture* depth);
    void bind_for_clear(const VglTexture* colour);
    void execute(Pipeline& p);
    void draw(bool indexed, std::uint32_t count, std::uint32_t first);
    void clear_colour(const VglTexture* target, unsigned layer, const float rgba[4], const Rect* rects, unsigned count);
    void clear_depth(const VglTexture* depth, float value);
    void draw_depth_clear(float value);
    void copy_region(const VglTexture* dst, unsigned dst_sub, unsigned x, unsigned y, const VglTexture* src,
                     unsigned src_sub, const Box* box);
    void submit(CommandList& list);
    void update_buffer(VglBuffer& b, const void* data, std::size_t bytes, std::size_t offset, bool keep_rest);
    void update_texture(VglTexture& t, const Box& box, const std::uint8_t* data, unsigned pitch);
    SwapChain::Presented present(unsigned sync_interval);

    std::shared_ptr<VglTexture> make_texture(const TextureDesc& desc, const Level* levels);
    std::shared_ptr<VglTexture> back_buffer() { return back_; }
    void read(const VglTexture& t, unsigned sub,
              const std::function<void(const std::uint8_t* data, unsigned row_pitch)>& reader);
    void message(const std::string& text);
    void forget(const VglTexture* t);  // a texture's framebuffer objects (it is being destroyed)
    MemoryStats snapshot();

private:
    void copy_from_display(VglTexture& dst, unsigned layer, unsigned x, unsigned y, const Box& b);
    void blit_from_display(VglTexture& dst, unsigned w, unsigned h, bool flip);
    bool check_display_blit(VglTexture& dst, unsigned w, unsigned h, const std::vector<std::uint8_t>& shown);
    void write_rows(VglTexture& t, unsigned left, unsigned top, unsigned right, unsigned bottom, unsigned layer,
                    const std::uint8_t* data, unsigned pitch);

    std::map<std::uint32_t, CompiledShader> shaders_;
    std::map<std::tuple<unsigned, unsigned, const GxmAttribute*>, GlProgram> programs_;
    bool compiler_checked_ = false;
    std::map<std::pair<const VglTexture*, const VglTexture*>, Framebuffer> framebuffers_;
    const VglTexture* bound_colour_ = nullptr;
    const VglTexture* bound_depth_ = nullptr;
    bool bound_ = false;      // a target is bound (else nothing yet)
    bool display_bound_ = false;  // the bound target is the display (framebuffer 0)
    std::uint32_t clear_serial_ = 0;
    unsigned enabled_attributes_ = 0;
    int vsync_ = -1;
    State state_;
    std::shared_ptr<VglTexture> back_;
    std::shared_ptr<VglTexture> dummy_;
    unsigned draws_ = 0, targets_ = 0, logged_draws_ = 0, presents_ = 0;
    // copy_from_display's GPU path: unchecked until its first use, then the row order it matched the read-back with.
    enum class DisplayBlit { unchecked, direct, flipped, failed } display_blit_ = DisplayBlit::unchecked;
    std::uint64_t display_blits_ = 0, display_readbacks_ = 0;
};

VglBuffer::~VglBuffer() {
    if (!gl) return;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    glDeleteBuffers(1, &gl);
}

VglTexture::~VglTexture() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (g_impl) g_impl->forget(this);
    if (gl) glDeleteTextures(1, &gl);
}

Impl::Impl(const Options& o) : options(o) {
    const std::string cache = o.shader_cache + "/vitagl";
    make_directories(cache + "/v");
    make_directories(cache + "/f");
    vglSetShaderCachePath(cache.c_str());
    // The GXM backend's compile settings (vitashark's shark_compile_shader defaults): no fast-math variants, so the
    // programs' floor / round arithmetic matches the D3D11 reference (VITA.md "Floating point").
    vglSetupRuntimeShaderCompiler(SHARK_OPT_DEFAULT, GL_FALSE, GL_FALSE, GL_FALSE);
    // RAM pool: the free user memory less 8 MB (thread stacks, audio, the kernel); all free CDRAM (VITA.md "Memory").
    // vglInitExtended returns GL_TRUE only when it fell back to a smaller display resolution.
    if (vglInitExtended(0x100000, display_width, display_height, 8 * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE))
        throw std::runtime_error("gfx vitagl: the display does not support 960x544");
    vglUseExtraMem(GL_TRUE);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    TextureDesc dummy;
    dummy.width = dummy.height = 1;
    dummy.bind = bind_sampled;
    dummy_ = make_texture(dummy, nullptr);
    TextureDesc back;
    back.width = display_width;
    back.height = display_height;
    back.format = Format::bgra8;  // what the D3D swap chain reports (stored RGBA, swizzled at read-back)
    back.bind = bind_target | bind_sampled;
    // The swap chain's buffer stands for vitaGL's display buffer: draws into it go straight to framebuffer 0 (no
    // framebuffer object; VITA.md "Renderer: vitaGL backend", the display freeze). It has no storage of its own:
    // sampling it throws, copies out of it read the display.
    back_ = std::make_shared<VglTexture>();
    back_->set_desc(back);
    back_->layer_rows = display_height;
    if (options.log) {
        char text[200];
        std::snprintf(text, sizeof text, "vitagl: initialised (VRAM %u KB free of %u KB, RAM %u KB free of %u KB)",
                      unsigned(vglMemFree(VGL_MEM_VRAM) >> 10), unsigned(vglMemTotal(VGL_MEM_VRAM) >> 10),
                      unsigned(vglMemFree(VGL_MEM_RAM) >> 10), unsigned(vglMemTotal(VGL_MEM_RAM) >> 10));
        options.log(text);
    }
}

MemoryStats Impl::snapshot() {
    MemoryStats s = stats;
    s.cdram_pool = vglMemTotal(VGL_MEM_VRAM);
    s.cdram_used = s.cdram_pool - std::min(s.cdram_pool, vglMemFree(VGL_MEM_VRAM));
    s.lpddr_pool = vglMemTotal(VGL_MEM_RAM);
    s.lpddr_used = s.lpddr_pool - std::min(s.lpddr_pool, vglMemFree(VGL_MEM_RAM));
    stats.cdram_peak = std::max(stats.cdram_peak, s.cdram_used);
    stats.lpddr_peak = std::max(stats.lpddr_peak, s.lpddr_used);
    s.cdram_peak = stats.cdram_peak;
    s.lpddr_peak = stats.lpddr_peak;
    return s;
}

// A Cg stage compiled by vitaGL (SceShaccCg through vitaShaRK; vitaGL caches the GXP under shader_cache/vitagl).
const CompiledShader& Impl::shader(bool vertex, unsigned index) {
    const std::uint32_t key = (vertex ? 0x10000u : 0u) | index;
    auto found = shaders_.find(key);
    if (found != shaders_.end()) return found->second;
    if (!compiler_checked_) {
        // vitaGL starts the compiler at every glCompileShader, cached GXP or not.
        if (!file_exists("ur0:data/libshacccg.suprx") && !file_exists("ur0:data/external/libshacccg.suprx") &&
            !file_exists(options.compiler.c_str()))
            throw ShaderCompilerMissing("The Cg shader compiler is missing: copy libshacccg.suprx to ur0:data/ "
                                        "(research/VITA.md \"Shader compiler\"); the vitaGL renderer needs it at every boot");
        compiler_checked_ = true;
    }
    const GxmStage& s = vertex ? gxm_vertex_stage(index) : gxm_fragment_stage(index);
    CompiledShader c;
    c.source = vertex ? vertex_source(s.source) : fragment_source(s.source);
    c.gl = glCreateShader(vertex ? GL_CG_VERTEX_SHADER_EXT : GL_CG_FRAGMENT_SHADER_EXT);
    const GLchar* text = c.source.c_str();
    const GLint length = static_cast<GLint>(c.source.size());
    glShaderSource(c.gl, 1, &text, &length);
    glCompileShader(c.gl);
    GLint ok = 0;
    glGetShaderiv(c.gl, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {};
        GLsizei n = 0;
        glGetShaderInfoLog(c.gl, sizeof log - 1, &n, log);
        throw std::runtime_error("gfx vitagl: Cg compile of " + std::string(s.name) + " failed:\n" + log);
    }
    ++stats.programs_compiled;
    if (options.log) options.log(std::string("vitagl: compiled ") + s.name);
    return shaders_.emplace(key, std::move(c)).first->second;
}

GlProgram& Impl::program(const Pipeline& p) {
    const auto key = std::make_tuple(p.vertex, p.fragment, p.attributes);
    auto found = programs_.find(key);
    if (found != programs_.end()) return found->second;
    const CompiledShader& vs = shader(true, p.vertex);
    const CompiledShader& fs = shader(false, p.fragment);
    GlProgram g;
    g.gl = glCreateProgram();
    glAttachShader(g.gl, vs.gl);
    glAttachShader(g.gl, fs.gl);
    for (unsigned k = 0; k < p.attribute_count && k < 8; ++k) glBindAttribLocation(g.gl, k, p.attributes[k].name);
    glLinkProgram(g.gl);
    GLint linked = 0;
    glGetProgramiv(g.gl, GL_LINK_STATUS, &linked);
    if (!linked) throw std::runtime_error("gfx vitagl: link failed (vertex stage " + std::to_string(p.vertex) +
                                          ", fragment stage " + std::to_string(p.fragment) + ")");
    glUseProgram(g.gl);
    for (int k = 0; k < 2; ++k) {
        if (!p.constants[k]) continue;
        const std::string name = p.constants[k], fname = name + "F";
        if ((g.vertex_count[k] = declared_count(vs.source, name)))
            g.vertex_constants[k] = glGetUniformLocation(g.gl, name.c_str());
        if ((g.fragment_count[k] = declared_count(fs.source, fname)))
            g.fragment_constants[k] = glGetUniformLocation(g.gl, fname.c_str());
    }
    auto uniform = [&](const std::string& source, const char* name) {
        return declared_count(source, name) ? glGetUniformLocation(g.gl, name) : -1;
    };
    g.tex_size = uniform(fs.source, "omegaTexSize");
    g.layers = uniform(fs.source, "omegaLayers");
    g.blend_mul = uniform(fs.source, "omegaBlendMul");
    g.colour = uniform(fs.source, "omegaColour");
    g.clear = uniform(vs.source, "omegaClear");
    g.flip = uniform(vs.source, "omegaVgl");
    // `uniform sampler2D name : TEXUNITk` binds texture unit k.
    for (std::size_t at = fs.source.find("sampler2D "); at != std::string::npos; at = fs.source.find("sampler2D ", at + 1)) {
        std::size_t b = at + 10, e = b;
        while (e < fs.source.size() && is_word(fs.source[e])) ++e;
        const std::size_t unit = fs.source.find("TEXUNIT", e);
        if (unit == std::string::npos || fs.source.find_first_of(",)", e) < unit) continue;
        const std::string name = fs.source.substr(b, e - b);
        const GLint loc = glGetUniformLocation(g.gl, name.c_str());
        if (loc != -1 && loc != 0) glUniform1i(loc, std::atoi(fs.source.c_str() + unit + 7));
    }
    for (unsigned k = 0; k < p.attribute_count && k < 8; ++k) g.attribute[k] = glGetAttribLocation(g.gl, p.attributes[k].name);
    if (options.log) {
        char text[200];
        std::snprintf(text, sizeof text, "vitagl: program %u/%u linked: attributes %d %d %d %d %d, flip %d, colour %d",
                      p.vertex, p.fragment, g.attribute[0], g.attribute[1], g.attribute[2], g.attribute[3],
                      g.attribute[4], int(g.flip != -1), int(g.colour != -1));
        options.log(text);
    }
    return programs_.emplace(key, g).first->second;
}

// ---- textures ----

void Impl::write_rows(VglTexture& t, unsigned left, unsigned top, unsigned right, unsigned bottom, unsigned layer,
                      const std::uint8_t* data, unsigned pitch) {
    glBindTexture(GL_TEXTURE_2D, t.gl);
    auto* base = static_cast<std::uint8_t*>(vglGetTexDataPointer(GL_TEXTURE_2D));
    if (!base) throw std::runtime_error("gfx vitagl: a texture without storage");
    const Format format = t.desc().format;
    if (format == Format::bc1 || format == Format::bc3)
        throw std::runtime_error("gfx vitagl: a block-compressed texture is immutable (no row writes)");
    for (unsigned y = top; y < bottom; ++y) {
        std::uint8_t* row = base + (std::size_t(layer) * t.layer_rows + y) * t.stride() + std::size_t(left) * 4;
        const std::uint8_t* in = data + std::size_t(y - top) * pitch;
        const unsigned n = right - left;
        switch (format) {
        case Format::r8:
            for (unsigned x = 0; x < n; ++x) {
                row[4 * x] = in[x];
                row[4 * x + 1] = row[4 * x + 2] = 0;
                row[4 * x + 3] = 0xff;
            }
            break;
        case Format::bgra8:
            for (unsigned x = 0; x < n; ++x) {
                row[4 * x] = in[4 * x + 2];
                row[4 * x + 1] = in[4 * x + 1];
                row[4 * x + 2] = in[4 * x];
                row[4 * x + 3] = in[4 * x + 3];
            }
            break;
        case Format::r32f:
            for (unsigned x = 0; x < n; ++x) {
                float f;
                std::memcpy(&f, in + 4 * x, 4);
                row[4 * x] = static_cast<std::uint8_t>(std::clamp(f * 255.0F + 0.5F, 0.0F, 255.0F));
                row[4 * x + 1] = row[4 * x + 2] = 0;
                row[4 * x + 3] = 0xff;
            }
            break;
        default: std::memcpy(row, in, std::size_t(n) * 4); break;
        }
    }
}

std::shared_ptr<VglTexture> Impl::make_texture(const TextureDesc& desc, const Level* levels) {
    if (desc.samples > 1) throw std::runtime_error("gfx vitagl: multisampled textures are unported");
    auto t = std::make_shared<VglTexture>();
    t->set_desc(desc);
    const unsigned layers = std::max(desc.layers, 1u);
    t->layer_rows = std::max(desc.height, 1u);
    switch (desc.format) {
    case Format::rgba8: case Format::bgra8: case Format::r8: case Format::r32f: break;
    case Format::d24s8: t->depth = true; break;
    case Format::bc1: case Format::bc3: {
        // 132md11 mod textures (Capabilities::block_textures): DXT1 / DXT5 blocks, which vitaGL keeps as UBC1 / UBC3.
        // Level 0 only, as every texture of this backend (the samplers do not filter between mips).
        if (layers != 1 || !levels || !levels[0].data || desc.width % 4 || desc.height % 4)
            throw std::runtime_error("gfx vitagl: a block-compressed texture needs one layer of 4 x 4 blocks with data");
        glGenTextures(1, &t->gl);
        glBindTexture(GL_TEXTURE_2D, t->gl);
        const GLsizei bytes = static_cast<GLsizei>(std::size_t(levels[0].row_pitch) * (desc.height / 4));
        glCompressedTexImage2D(GL_TEXTURE_2D, 0,
                               desc.format == Format::bc1 ? GL_COMPRESSED_RGBA_S3TC_DXT1_EXT : GL_COMPRESSED_RGBA_S3TC_DXT5_EXT,
                               desc.width, desc.height, 0, bytes, levels[0].data);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        if (!vglGetTexDataPointer(GL_TEXTURE_2D)) throw std::runtime_error("gfx vitagl: out of texture memory (VITA.md \"Memory\")");
        return t;
    }
    default: throw std::runtime_error("gfx vitagl: unknown texture format");
    }
    if (t->depth) {
        if (layers != 1) throw std::runtime_error("gfx vitagl: depth texture arrays are unported");
        return t;
    }
    if (desc.width > 4096 || t->rows() > 4096)
        throw std::runtime_error("gfx vitagl: a texture (with its stacked layers) above 4096 texels is unported");
    glGenTextures(1, &t->gl);
    glBindTexture(GL_TEXTURE_2D, t->gl);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, std::max(desc.width, 1u), t->rows(), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    auto* base = static_cast<std::uint8_t*>(vglGetTexDataPointer(GL_TEXTURE_2D));
    if (!base) throw std::runtime_error("gfx vitagl: out of texture memory (VITA.md \"Memory\")");
    std::memset(base, 0, t->stride() * t->rows());
    const unsigned mips = std::max(desc.mips, 1u);
    if (levels)
        for (unsigned l = 0; l < layers; ++l)
            if (levels[l * mips].data)
                write_rows(*t, 0, 0, desc.width, t->layer_rows, l, static_cast<const std::uint8_t*>(levels[l * mips].data),
                           levels[l * mips].row_pitch);
    return t;
}

void Impl::forget(const VglTexture* t) {
    for (auto it = framebuffers_.begin(); it != framebuffers_.end();) {
        if (it->first.first == t || it->first.second == t) {
            if (bound_ && bound_colour_ == it->first.first && bound_depth_ == it->first.second) {
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                bound_ = false;
            }
            if (it->second.depth_buffer) glDeleteRenderbuffers(1, &it->second.depth_buffer);
            glDeleteFramebuffers(1, &it->second.fbo);
            it = framebuffers_.erase(it);
        } else {
            ++it;
        }
    }
}

// ---- targets ----

void Impl::use_target(const VglTexture* colour, const VglTexture* depth) {
    if (!colour && !depth) throw std::runtime_error("gfx vitagl: a draw with no colour or depth target");
    if (colour && colour == back_.get()) {
        // The display: vitaGL's own colour and depth/stencil buffers (the depth texture stands for the latter).
        if (!bound_ || !display_bound_ || bound_depth_ != depth) {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            bound_ = display_bound_ = true;
            bound_colour_ = colour;
            bound_depth_ = depth;
            ++targets_;
        }
    } else if (!bound_ || bound_colour_ != colour || bound_depth_ != depth) {
        display_bound_ = false;
        Framebuffer& f = framebuffers_[std::make_pair(colour, depth)];
        if (!f.fbo) {
            glGenFramebuffers(1, &f.fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, f.fbo);
            const VglTexture* attach = colour;
            if (!attach) {
                TextureDesc d;
                d.width = depth->desc().width;
                d.height = depth->desc().height;
                d.bind = bind_target;
                f.dummy_colour = make_texture(d, nullptr);
                attach = f.dummy_colour.get();
            }
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, attach->gl, 0);
            if (depth) {
                glGenRenderbuffers(1, &f.depth_buffer);
                glBindRenderbuffer(GL_RENDERBUFFER, f.depth_buffer);
                glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, attach->desc().width, attach->rows());
                glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, f.depth_buffer);
            }
            const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            if (options.log) {
                char text[160];
                std::snprintf(text, sizeof text, "vitagl: framebuffer %u: colour %ux%u%s, status 0x%04x", f.fbo,
                              attach->desc().width, attach->rows(), depth ? " + depth" : "", unsigned(status));
                options.log(text);
            }
        } else {
            glBindFramebuffer(GL_FRAMEBUFFER, f.fbo);
        }
        bound_ = true;
        bound_colour_ = colour;
        bound_depth_ = depth;
        ++targets_;
    }
    if (depth) {
        Framebuffer& f = framebuffers_[std::make_pair(colour, depth)];
        if (f.applied_clear != depth->clear_serial) {
            f.applied_clear = depth->clear_serial;
            draw_depth_clear(depth->clear_value);
        }
    }
}

// The target a clear or copy into `colour` uses: the bound framebuffer when it already draws into this texture (its
// depth stays attached), the state's when the state names it, else the texture alone.
void Impl::bind_for_clear(const VglTexture* colour) {
    if (bound_ && bound_colour_ == colour) return;
    if (state_.colour[0] == colour) use_target(colour, state_.depth);
    else use_target(colour, nullptr);
}

// ---- draws ----

void Impl::execute(Pipeline& p) {
    GlProgram& g = program(p);
    glUseProgram(g.gl);
    const Viewport& v = p.viewport;
    const bool display = bound_ && display_bound_;
    const GLint vy = static_cast<GLint>(std::lround(v.y));
    const GLsizei vh = static_cast<GLsizei>(std::lround(v.height));
    // vitaGL's display viewport is bottom-up (GL); a framebuffer object's is top-down with the flip uniform below.
    glViewport(static_cast<GLint>(std::lround(v.x)), display ? static_cast<GLint>(display_height) - vy - vh : vy,
               static_cast<GLsizei>(std::lround(v.width)), vh);
    glDepthRangef(2.0F * v.min_depth - v.max_depth, v.max_depth);
    glPolygonMode(GL_FRONT_AND_BACK, p.wireframe ? GL_LINE : GL_FILL);
    if (p.depth_test || p.depth_write) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(p.depth_test ? p.depth_func : GL_ALWAYS);
        glDepthMask(p.depth_write ? GL_TRUE : GL_FALSE);
    } else {
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
    }
    glColorMask((p.mask & mask_r) ? GL_TRUE : GL_FALSE, (p.mask & mask_g) ? GL_TRUE : GL_FALSE,
                (p.mask & mask_b) ? GL_TRUE : GL_FALSE, (p.mask & mask_a) ? GL_TRUE : GL_FALSE);
    if (p.blend) {
        glEnable(GL_BLEND);
        glBlendFuncSeparate(p.colour_src, p.colour_dst, p.alpha_src, p.alpha_dst);
        glBlendEquationSeparate(p.colour_op, p.alpha_op);
    } else {
        glDisable(GL_BLEND);
    }

    float tex_size[16];
    float layers[4];
    for (unsigned k = 0; k < 4; ++k) {
        const VglTexture* t = p.textures[k].texture ? p.textures[k].texture : dummy_.get();
        if (t->depth) throw std::runtime_error("gfx vitagl: sampling a depth texture is unported");
        if (t == back_.get()) throw std::runtime_error("gfx vitagl: sampling the swap chain buffer (the display) is unported");
        glActiveTexture(GL_TEXTURE0 + k);
        glBindTexture(GL_TEXTURE_2D, t->gl);
        const SamplerDesc& s = p.textures[k].sampler;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, s.filter != Filter::linear ? GL_NEAREST : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, s.filter == Filter::point ? GL_NEAREST : GL_LINEAR);
        const GLint wrap = s.address == Address::wrap ? GL_REPEAT : GL_CLAMP_TO_EDGE;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
        const float w = static_cast<float>(std::max(t->desc().width, 1u)), h = static_cast<float>(t->layer_rows);
        tex_size[4 * k] = w;
        tex_size[4 * k + 1] = h;
        tex_size[4 * k + 2] = 1.0F / w;
        tex_size[4 * k + 3] = 1.0F / h;
        layers[k] = static_cast<float>(std::max(t->desc().layers, 1u));
    }
    glActiveTexture(GL_TEXTURE0);

    auto set = [](GLint location, const void* data, std::size_t floats, unsigned declared) {
        if (location == -1 || location == 0 || !data) return;
        const unsigned n = static_cast<unsigned>(std::min<std::size_t>(floats / 4, declared));
        if (n) glUniform4fv(location, static_cast<GLsizei>(n), static_cast<const GLfloat*>(data));
    };
    for (int k = 0; k < 2; ++k) {
        set(g.vertex_constants[k], p.vertex_constants[k].data, p.vertex_constants[k].bytes / 4, g.vertex_count[k]);
        set(g.fragment_constants[k], p.fragment_constants[k].data, p.fragment_constants[k].bytes / 4, g.fragment_count[k]);
    }
    const float flip[4]{bound_ && !display ? -1.0F : 1.0F, 0, 0, 0};
    set(g.flip, flip, 4, 1);
    set(g.clear, p.clear, 4, 1);
    set(g.tex_size, tex_size, 16, 4);
    set(g.layers, layers, 4, 1);
    set(g.blend_mul, p.blend_mul, 4, 1);
    set(g.colour, p.colour, 4, 1);

    glBindBuffer(GL_ARRAY_BUFFER, p.vertex_buffer);
    unsigned enabled = 0;
    for (unsigned k = 0; k < p.attribute_count && k < 8; ++k) {
        const int location = g.attribute[k];
        if (location < 0) continue;
        const std::size_t offset = p.vertex_offset + p.attributes[k].offset;
        const void* pointer = p.vertex_buffer ? reinterpret_cast<const void*>(offset)
                                              : static_cast<const std::uint8_t*>(p.client_vertices) + offset;
        glVertexAttribPointer(static_cast<GLuint>(location), p.attributes[k].components, GL_FLOAT, GL_FALSE,
                              static_cast<GLsizei>(p.stride), pointer);
        enabled |= 1u << location;
    }
    for (unsigned k = 0; k < 8; ++k) {
        const unsigned bit = 1u << k;
        if ((enabled & bit) && !(enabled_attributes_ & bit)) glEnableVertexAttribArray(k);
        if (!(enabled & bit) && (enabled_attributes_ & bit)) glDisableVertexAttribArray(k);
    }
    enabled_attributes_ = enabled;

    if (p.indexed) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, p.index_buffer);
        const void* indices = p.index_buffer ? reinterpret_cast<const void*>(std::size_t(p.first) * 4)
                                             : static_cast<const void*>(p.client_indices);
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(p.count), GL_UNSIGNED_INT, indices);
    } else {
        glDrawArrays(GL_TRIANGLES, static_cast<GLint>(p.first), static_cast<GLsizei>(p.count));
    }
    ++draws_;
    if (options.log && logged_draws_ < 16) {
        ++logged_draws_;
        char text[200];
        std::snprintf(text, sizeof text, "vitagl draw %u: vs %u fs %u count %u viewport %.0f,%.0f %.0fx%.0f target %p depth %p blend %d",
                      logged_draws_, p.vertex, p.fragment, p.count, v.x, v.y, v.width, v.height,
                      static_cast<const void*>(bound_colour_), static_cast<const void*>(bound_depth_), int(p.blend));
        options.log(text);
    }
}

// The layer's rows inside the stacked texture: the viewport's y offset.
Viewport in_layer(Viewport v, const VglTexture* target, unsigned layer) {
    if (target) v.y += static_cast<float>(layer * target->layer_rows);
    return v;
}

void Impl::draw(bool indexed, std::uint32_t count, std::uint32_t first) {
    OMEGA_ZONE("vgl.draw");
    if (!state_.has_program) throw std::runtime_error("gfx vitagl: a draw with no program");
    if (!count) return;
    const GxmProgram& g = gxm_program(state_.program);
    const GxmLayout& layout = gxm_layout(program_layout(state_.program));
    if (!state_.vertices || !state_.vertices->gl) throw std::runtime_error("gfx vitagl: a draw with no vertex buffer");
    Pipeline p;
    p.vertex = g.vertex;
    p.constants[0] = g.constants[0];
    p.constants[1] = g.constants[1];
    p.attributes = layout.attributes;
    p.attribute_count = layout.count;
    p.stride = state_.stride;
    p.vertex_buffer = state_.vertices->gl;
    p.vertex_offset = state_.offset;
    p.indexed = indexed;
    p.first = first;
    p.count = count;
    if (indexed) {
        if (!state_.indices || !state_.indices->gl) throw std::runtime_error("gfx vitagl: an indexed draw with no index buffer");
        if ((std::size_t(first) + count) * 4 > state_.indices->desc().bytes)
            throw std::runtime_error("gfx vitagl: an indexed draw past its index buffer");
        p.index_buffer = state_.indices->gl;
    }
    std::vector<std::uint32_t> kept;
    if (g.cpu_geometry) {
        const VglBuffer* shadow = state_.geometry_constants[1] ? state_.geometry_constants[1] : state_.vertex_constants[1];
        if (!shadow || shadow->cpu.size() < 112) throw std::runtime_error("gfx vitagl: shadow_receiver without Shadow constants");
        ShadowFilter filter;
        std::memcpy(filter.clip, shadow->cpu.data(), sizeof filter.clip);
        std::memcpy(filter.facing, shadow->cpu.data() + 96, sizeof filter.facing);
        const std::uint8_t* vertices = state_.vertices->cpu.data() + state_.offset;
        const auto* source = indexed ? reinterpret_cast<const std::uint32_t*>(state_.indices->cpu.data()) + first : nullptr;
        kept.reserve(count);
        for (std::uint32_t k = 0; k + 2 < count; k += 3) {
            const std::uint32_t i0 = source ? source[k] : first + k, i1 = source ? source[k + 1] : first + k + 1,
                                i2 = source ? source[k + 2] : first + k + 2;
            if (filter.keep(vertices + std::size_t(i0) * p.stride, vertices + std::size_t(i1) * p.stride,
                            vertices + std::size_t(i2) * p.stride)) {
                kept.push_back(i0);
                kept.push_back(i1);
                kept.push_back(i2);
            }
        }
        if (kept.empty()) return;
        p.indexed = true;
        p.index_buffer = 0;
        p.client_indices = kept.data();
        p.first = 0;
        p.count = static_cast<std::uint32_t>(kept.size());
    }
    const DepthDesc depth = state_.depth_state ? state_.depth_state->desc : DepthDesc{true, true, Compare::less};
    p.depth_test = depth.test && state_.depth;
    p.depth_func = gl_compare(depth.compare);
    p.depth_write = depth.test && depth.write && state_.depth;
    p.wireframe = state_.raster && state_.raster->desc.wireframe;
    for (unsigned k = 0; k < 4; ++k) {
        p.textures[k].texture = state_.textures[k];
        const VglSampler* s = state_.samplers[g.samplers[k]];
        p.textures[k].sampler = s ? s->desc : SamplerDesc{};
    }
    for (int k = 0; k < 2; ++k) {
        if (const VglBuffer* b = state_.vertex_constants[k]) p.vertex_constants[k] = {b->cpu.data(), b->cpu.size()};
        if (const VglBuffer* b = state_.pixel_constants[k]) p.fragment_constants[k] = {b->cpu.data(), b->cpu.size()};
    }
    const BlendDesc blend = state_.blend ? state_.blend->desc : BlendDesc{};
    // particle_haze's two targets: SV_Target0 and SV_Target1 are two fragment programs drawn in turn (as on GXM).
    const bool second = g.fragment_alt != ~0U && state_.colour[1];
    for (unsigned pass = 0; pass < (second ? 2u : 1u); ++pass) {
        if (!state_.colour[pass] && !state_.depth) return;
        use_target(state_.colour[pass], state_.depth);
        p.viewport = in_layer(state_.viewport, state_.colour[pass], state_.layer[pass]);
        p.fragment = pass ? g.fragment_alt : g.fragment;
        blend_for(blend.targets[pass && blend.independent ? 1 : 0], state_.factor, p);
        execute(p);
    }
}

// A colour clear: quads drawn with the clear program (as on GXM; vitaGL's glClear honours the scissor mask pass).
void Impl::clear_colour(const VglTexture* target, unsigned layer, const float rgba[4], const Rect* rects, unsigned count) {
    if (!target) return;
    bind_for_clear(target);
    Pipeline p;
    p.vertex = gxm_clear_vertex;
    p.fragment = gxm_clear_fragment;
    p.attributes = clear_attributes;
    p.attribute_count = 1;
    p.stride = 8;
    p.client_vertices = clear_quad;
    p.count = 6;
    std::memcpy(p.colour, rgba, sizeof p.colour);
    const unsigned n = rects ? count : 1;
    for (unsigned k = 0; k < n; ++k) {
        if (rects) {
            const Rect& r = rects[k];
            if (r.right <= r.left || r.bottom <= r.top) continue;
            p.viewport = {float(r.left), float(r.top), float(r.right - r.left), float(r.bottom - r.top), 0, 1};
        } else {
            p.viewport = {0, 0, float(target->desc().width), float(target->layer_rows), 0, 1};
        }
        p.viewport = in_layer(p.viewport, target, layer);
        execute(p);
    }
}

// A depth clear: drawn into every framebuffer object that uses the depth texture when it is next bound.
void Impl::clear_depth(const VglTexture* depth, float value) {
    if (!depth) return;
    auto* d = const_cast<VglTexture*>(depth);
    d->clear_serial = ++clear_serial_;
    d->clear_value = value;
    if (bound_ && bound_depth_ == depth) {
        framebuffers_[std::make_pair(bound_colour_, bound_depth_)].applied_clear = d->clear_serial;
        draw_depth_clear(value);
    }
}

void Impl::draw_depth_clear(float value) {
    const VglTexture* size = bound_colour_ ? bound_colour_ : bound_depth_;
    Pipeline p;
    p.vertex = gxm_clear_vertex;
    p.fragment = gxm_clear_fragment;
    p.attributes = clear_attributes;
    p.attribute_count = 1;
    p.stride = 8;
    p.client_vertices = clear_quad;
    p.count = 6;
    p.mask = 0;
    p.depth_test = false;
    p.depth_write = true;
    p.clear[0] = value;
    // Depth range (-1, 1): window z = clip z = the value.
    p.viewport = {0, 0, float(size->desc().width), float(size->rows()), -1, 1};
    p.viewport.min_depth = 0.0F;  // 2 * 0 - 1 = -1
    p.viewport.max_depth = 1.0F;
    execute(p);
    glDepthRangef(-1.0F, 1.0F);
}

// CopyResource / CopySubresourceRegion: a point-sampled quad drawn into the destination.
void Impl::copy_region(const VglTexture* dst, unsigned dst_sub, unsigned x, unsigned y, const VglTexture* src,
                       unsigned src_sub, const Box* box) {
    if (!dst || !src) return;
    if (dst->depth || src->depth) throw std::runtime_error("gfx vitagl: depth copies are unported");
    const unsigned dm = std::max(dst->desc().mips, 1u), sm = std::max(src->desc().mips, 1u);
    if (dst_sub % dm || src_sub % sm) throw std::runtime_error("gfx vitagl: copies of mip levels above 0 are unported");
    const unsigned dl = dst_sub / dm, sl = src_sub / sm;
    if (src == back_.get()) {
        const Box b = box ? *box : Box{0, 0, 0, display_width, display_height, 1};
        copy_from_display(*const_cast<VglTexture*>(dst), dl, x, y, b);
        return;
    }
    const Box b = box ? *box : Box{0, 0, 0, src->desc().width, src->layer_rows, 1};
    if (b.right <= b.left || b.bottom <= b.top) return;
    bind_for_clear(dst);
    const float sw = float(std::max(src->desc().width, 1u));
    const float sh = float(src->rows());
    const float u0 = float(b.left) / sw, u1 = float(b.right) / sw;
    const float v0 = (float(sl * src->layer_rows) + float(b.top)) / sh;
    const float v1 = (float(sl * src->layer_rows) + float(b.bottom)) / sh;
    const float quad[]{-1, -1, u0, v1, 1, -1, u1, v1, -1, 1, u0, v0, -1, 1, u0, v0, 1, -1, u1, v1, 1, 1, u1, v0};
    Pipeline p;
    p.vertex = gxm_copy_vertex;
    p.fragment = gxm_copy_fragment;
    p.attributes = copy_attributes;
    p.attribute_count = 2;
    p.stride = 16;
    p.client_vertices = quad;
    p.count = 6;
    p.viewport = in_layer({float(x), float(y), float(b.right - b.left), float(b.bottom - b.top), 0, 1}, dst, dl);
    p.textures[0].texture = src;
    p.textures[0].sampler.filter = Filter::point;
    p.textures[0].sampler.address = Address::clamp;
    execute(p);
}

// A copy out of the display. A copy of the whole display into a texture of its size (the 132ws wide side fill and the
// vision blur, every frame) is drawn on the GPU: glBlitFramebuffer samples the display's colour surface into the
// texture's framebuffer object, with no wait. Its first use is checked against the read-back below (research/VITA.md
// "Widescreen"); other copies, a failed check and Options::display_copy_readback (a test switch) read back:
// glReadPixels (vitaGL ends the scene and waits for the GPU, then the CPU reads the uncached display), rows bottom-up.
void Impl::copy_from_display(VglTexture& dst, unsigned layer, unsigned x, unsigned y, const Box& box) {
    OMEGA_ZONE("vgl.copy_from_display");
    const unsigned right = std::min(box.right, display_width), bottom = std::min(box.bottom, display_height);
    if (right <= box.left || bottom <= box.top) return;
    const Format format = dst.desc().format;
    if (format != Format::rgba8 && format != Format::bgra8)
        throw std::runtime_error("gfx vitagl: a copy from the display into a non-RGBA texture is unported");
    const unsigned w = std::min(right - box.left, dst.desc().width - std::min(x, dst.desc().width));
    const unsigned h = std::min(bottom - box.top, dst.layer_rows - std::min(y, dst.layer_rows));
    if (!w || !h) return;
    const bool whole = layer == 0 && x == 0 && y == 0 && box.left == 0 && box.top == 0 && w == dst.desc().width &&
                       h == dst.layer_rows && dst.rows() == dst.layer_rows;
    const bool gpu = whole && !options.display_copy_readback && display_blit_ != DisplayBlit::failed;
    if (gpu && display_blit_ == DisplayBlit::unchecked && options.display_copy_order)
        display_blit_ = options.display_copy_order == 2 ? DisplayBlit::flipped : DisplayBlit::direct;
    if (gpu && display_blit_ != DisplayBlit::unchecked) {
        blit_from_display(dst, w, h, display_blit_ == DisplayBlit::flipped);
        ++display_blits_;
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    bound_ = display_bound_ = true;
    bound_colour_ = back_.get();
    bound_depth_ = nullptr;
    std::vector<std::uint8_t> up(std::size_t(w) * h * 4), rows(up.size());
    glReadPixels(static_cast<GLint>(box.left), static_cast<GLint>(display_height - box.top - h), static_cast<GLsizei>(w),
                 static_cast<GLsizei>(h), GL_RGBA, GL_UNSIGNED_BYTE, up.data());
    for (unsigned r = 0; r < h; ++r) {
        const std::uint8_t* in = up.data() + std::size_t(h - 1 - r) * w * 4;
        std::uint8_t* out = rows.data() + std::size_t(r) * w * 4;
        for (unsigned k = 0; k < w; ++k) {
            // write_rows takes the texture's format: BGRA for bgra8.
            out[4 * k] = in[4 * k + (format == Format::bgra8 ? 2 : 0)];
            out[4 * k + 1] = in[4 * k + 1];
            out[4 * k + 2] = in[4 * k + (format == Format::bgra8 ? 0 : 2)];
            out[4 * k + 3] = in[4 * k + 3];
        }
    }
    if (gpu && check_display_blit(dst, w, h, up)) return;
    write_rows(dst, x, y, x + w, y + h, layer, rows.data(), w * 4);
    ++display_readbacks_;
}

// The GPU copy: the display's top-left w x h (GL rows display_height - h .. display_height) drawn over the whole
// destination, point-sampled; `flip` reverses the rows (the order the check found).
void Impl::blit_from_display(VglTexture& dst, unsigned w, unsigned h, bool flip) {
    use_target(&dst, nullptr);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    const GLint top = static_cast<GLint>(display_height), bottom = top - static_cast<GLint>(h);
    glBlitFramebuffer(0, flip ? top : bottom, static_cast<GLint>(w), flip ? bottom : top, 0, 0, static_cast<GLint>(w),
                      static_cast<GLint>(h), GL_COLOR_BUFFER_BIT, GL_NEAREST);
    bound_ = false;  // the read binding is the display now: the next target binds both again
}

// The first whole-display copy: the GPU copy in both row orders, each compared byte for byte with the read-back
// (`shown`, rows bottom-up as glReadPixels gives them; the texture keeps RGBA bytes for both formats). The matching
// order is kept; neither matching: the read-back path from then on. True when the texture holds the copy.
bool Impl::check_display_blit(VglTexture& dst, unsigned w, unsigned h, const std::vector<std::uint8_t>& shown) {
    std::size_t differ[2]{};
    char where[2][96]{};
    for (const bool flip : {false, true}) {
        blit_from_display(dst, w, h, flip);
        glFinish();
        glBindTexture(GL_TEXTURE_2D, dst.gl);
        const auto* base = static_cast<const std::uint8_t*>(vglGetTexDataPointer(GL_TEXTURE_2D));
        if (!base) {
            differ[flip] = std::size_t(w) * h * 4;
            continue;
        }
        unsigned rows = 0, first = h, last = 0;
        for (unsigned r = 0; r < h; ++r) {
            const std::uint8_t* got = base + std::size_t(r) * dst.stride();
            const std::uint8_t* want = shown.data() + std::size_t(h - 1 - r) * w * 4;
            std::size_t row_differ = 0;
            for (unsigned k = 0; k < w * 4; ++k) row_differ += got[k] != want[k];
            if (!row_differ) continue;
            if (first == h) {
                unsigned k = 0;
                while (got[k] == want[k]) ++k;
                k &= ~3u;
                std::snprintf(where[flip], sizeof where[flip], "first at (%u, %u) %02x%02x%02x%02x not %02x%02x%02x%02x", k / 4,
                              r, got[k], got[k + 1], got[k + 2], got[k + 3], want[k], want[k + 1], want[k + 2], want[k + 3]);
                first = r;
            }
            last = r;
            ++rows;
            differ[flip] += row_differ;
        }
        if (rows) {
            const std::size_t used = std::strlen(where[flip]);
            std::snprintf(where[flip] + used, sizeof where[flip] - used, ", rows %u..%u (%u)", first, last, rows);
        }
        if (!differ[flip]) {
            display_blit_ = flip ? DisplayBlit::flipped : DisplayBlit::direct;
            break;
        }
    }
    if (display_blit_ == DisplayBlit::unchecked) display_blit_ = DisplayBlit::failed;
    if (options.log) {
        char text[440];
        std::snprintf(text, sizeof text,
                      "vitagl: display copy %ux%u on the GPU (glBlitFramebuffer) checked against the read-back: %u / "
                      "%u bytes differ (rows as read / reversed; %s / %s): %s",
                      w, h, unsigned(differ[0]), unsigned(differ[1]), where[0], where[1],
                      display_blit_ == DisplayBlit::failed ? "the read-back path is kept" : "the GPU copy is used");
        options.log(text);
    }
    if (display_blit_ == DisplayBlit::failed) return false;
    ++display_blits_;  // the matching try was the last one drawn
    return true;
}

// ---- CPU updates and read-back ----

void Impl::update_buffer(VglBuffer& b, const void* data, std::size_t bytes, std::size_t offset, bool keep_rest) {
    OMEGA_ZONE("vgl.update_buffer");
    const std::size_t size = b.desc().bytes;
    if (offset > size || bytes > size - offset) throw std::runtime_error("gfx vitagl: a buffer update past its end");
    if (b.cpu.size() < size) b.cpu.resize(size);
    if (bytes) std::memcpy(b.cpu.data() + offset, data, bytes);
    if (!b.gl || !bytes) return;
    glBindBuffer(GL_ARRAY_BUFFER, b.gl);
    // vitaGL gives a buffer the GPU may still read fresh memory (the old freed by its garbage collector).
    if (!keep_rest || (offset == 0 && bytes == size))
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizei>(size), b.cpu.data(), GL_DYNAMIC_DRAW);
    else
        glBufferSubData(GL_ARRAY_BUFFER, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(bytes), b.cpu.data() + offset);
}

// Written in place (vitaGL's glTexSubImage2D does the same): an update of a texture drawn earlier in the frame is
// seen by those draws (VITA.md "Renderer: vitaGL backend").
void Impl::update_texture(VglTexture& t, const Box& box, const std::uint8_t* data, unsigned pitch) {
    if (t.depth) throw std::runtime_error("gfx vitagl: depth texture updates are unported");
    const unsigned right = std::min(box.right, t.desc().width), bottom = std::min(box.bottom, t.layer_rows);
    if (right <= box.left || bottom <= box.top) return;
    write_rows(t, box.left, box.top, right, bottom, 0, data, pitch);
}

void Impl::read(const VglTexture& t, unsigned sub,
                const std::function<void(const std::uint8_t* data, unsigned row_pitch)>& reader) {
    if (t.depth) throw std::runtime_error("gfx vitagl: depth read-back is unported");
    const unsigned mips = std::max(t.desc().mips, 1u);
    if (sub % mips) throw std::runtime_error("gfx vitagl: read-back of mip levels above 0 is unported");
    glFinish();
    glBindTexture(GL_TEXTURE_2D, t.gl);
    const auto* base = static_cast<const std::uint8_t*>(vglGetTexDataPointer(GL_TEXTURE_2D));
    if (!base) throw std::runtime_error("gfx vitagl: read-back of a texture without storage");
    base += std::size_t(sub / mips) * t.layer_rows * t.stride();
    const unsigned w = t.desc().width;
    switch (t.desc().format) {
    case Format::rgba8: reader(base, static_cast<unsigned>(t.stride())); return;
    case Format::r8: {
        std::vector<std::uint8_t> out(std::size_t(w) * t.layer_rows);
        for (unsigned y = 0; y < t.layer_rows; ++y)
            for (unsigned x = 0; x < w; ++x) out[std::size_t(y) * w + x] = base[y * t.stride() + x * 4];
        reader(out.data(), w);
        return;
    }
    case Format::r32f: {
        std::vector<float> out(std::size_t(w) * t.layer_rows);
        for (unsigned y = 0; y < t.layer_rows; ++y)
            for (unsigned x = 0; x < w; ++x) out[std::size_t(y) * w + x] = base[y * t.stride() + x * 4] / 255.0F;
        reader(reinterpret_cast<const std::uint8_t*>(out.data()), w * 4);
        return;
    }
    default: {  // bgra8
        std::vector<std::uint8_t> out(std::size_t(w) * 4 * t.layer_rows);
        for (unsigned y = 0; y < t.layer_rows; ++y) {
            const std::uint8_t* in = base + y * t.stride();
            std::uint8_t* o = out.data() + std::size_t(y) * w * 4;
            for (unsigned x = 0; x < w; ++x) {
                o[4 * x] = in[4 * x + 2];
                o[4 * x + 1] = in[4 * x + 1];
                o[4 * x + 2] = in[4 * x];
                o[4 * x + 3] = in[4 * x + 3];
            }
        }
        reader(out.data(), w * 4);
        return;
    }
    }
}

// ---- command lists ----

const VglBuffer* buffer_of(const void* p) { return static_cast<const VglBuffer*>(static_cast<const Buffer*>(p)); }
const VglTexture* texture_of(const void* p) { return static_cast<const VglTexture*>(static_cast<const Texture*>(p)); }

void Impl::submit(CommandList& list) {
    for (const Command& c : list.commands()) {
        switch (c.op) {
        case Command::Op::set_targets:
            for (unsigned k = 0; k < 2; ++k) {
                state_.colour[k] = k < c.count ? texture_of(c.objects[k]) : nullptr;
                state_.layer[k] = k < c.count ? c.layers[k] : 0;
            }
            state_.depth = texture_of(c.objects[2]);
            break;
        case Command::Op::set_viewport:
            state_.viewport = {c.values[0], c.values[1], c.values[2], c.values[3], c.values[4], c.values[5]};
            break;
        case Command::Op::clear_target:
            clear_colour(texture_of(c.objects[0]), c.layers[0], c.values, nullptr, 0);
            break;
        case Command::Op::clear_depth:
            if (c.flags & CommandList::clear_depth_bit) clear_depth(texture_of(c.objects[0]), c.values[0]);
            break;
        case Command::Op::clear_rects: {
            Rect rects[2]{};
            std::memcpy(rects, list.arena(c.c), sizeof(Rect) * std::min<unsigned>(c.count, 2));
            clear_colour(texture_of(c.objects[0]), 0, c.values, rects, std::min<unsigned>(c.count, 2));
            break;
        }
        case Command::Op::set_raster: state_.raster = static_cast<const VglRaster*>(static_cast<const RasterState*>(c.objects[0])); break;
        case Command::Op::set_depth: state_.depth_state = static_cast<const VglDepth*>(static_cast<const DepthState*>(c.objects[0])); break;
        case Command::Op::set_blend:
            state_.blend = static_cast<const VglBlend*>(static_cast<const BlendState*>(c.objects[0]));
            for (int k = 0; k < 4; ++k) state_.factor[k] = (c.flags & 1) ? c.values[k] : 1.0F;
            break;
        case Command::Op::set_program:
            state_.program = c.program;
            state_.has_program = true;
            break;
        case Command::Op::set_vertex_buffer:
            state_.vertices = buffer_of(c.objects[0]);
            state_.stride = c.a;
            state_.offset = c.b;
            break;
        case Command::Op::set_index_buffer: state_.indices = buffer_of(c.objects[0]); break;
        case Command::Op::set_constants:
            for (unsigned k = 0; k < c.count && c.slot + k < 4; ++k) {
                const VglBuffer* b = buffer_of(c.objects[k]);
                if (c.flags & stage_vertex) state_.vertex_constants[c.slot + k] = b;
                if (c.flags & stage_geometry) state_.geometry_constants[c.slot + k] = b;
                if (c.flags & stage_pixel) state_.pixel_constants[c.slot + k] = b;
            }
            break;
        case Command::Op::set_textures:
            for (unsigned k = 0; k < c.count && c.slot + k < 4; ++k) state_.textures[c.slot + k] = texture_of(c.objects[k]);
            break;
        case Command::Op::set_sampler:
            if (c.slot < 4) state_.samplers[c.slot] = static_cast<const VglSampler*>(static_cast<const SamplerState*>(c.objects[0]));
            break;
        case Command::Op::update_buffer:
            update_buffer(*const_cast<VglBuffer*>(buffer_of(c.objects[0])), list.arena(c.a), c.b, c.c, (c.flags & 1) != 0);
            break;
        case Command::Op::update_texture: {
            Box box;
            std::memcpy(&box, list.arena(c.d), sizeof box);
            update_texture(*const_cast<VglTexture*>(texture_of(c.objects[0])), box, list.arena(c.a), c.b);
            break;
        }
        case Command::Op::draw: draw(false, c.a, c.b); break;
        case Command::Op::draw_indexed: draw(true, c.a, c.b); break;
        case Command::Op::copy: {
            const VglTexture* dst = texture_of(c.objects[0]);
            const VglTexture* src = texture_of(c.objects[1]);
            const unsigned layers = std::max(src->desc().layers, 1u);
            for (unsigned l = 0; l < layers; ++l)
                copy_region(dst, l * std::max(dst->desc().mips, 1u), 0, 0, src, l * std::max(src->desc().mips, 1u), nullptr);
            break;
        }
        case Command::Op::copy_region: {
            Box box;
            if (c.flags & 1) std::memcpy(&box, list.arena(c.slot), sizeof box);
            copy_region(texture_of(c.objects[0]), c.a, c.b, c.c, texture_of(c.objects[1]), c.d, (c.flags & 1) ? &box : nullptr);
            break;
        }
        case Command::Op::flush: break;
        case Command::Op::reset_state: state_ = State{}; break;
        }
    }
    list.clear();
}

// The frame was drawn straight into vitaGL's display buffer: the flip. Every 600th frame logs four display pixels
// (glReadPixels; the bring-up evidence that the picture reached the display buffer).
SwapChain::Presented Impl::present(unsigned sync_interval) {
    OMEGA_ZONE("vgl.present");
    if (options.log && presents_++ % 600 == 0) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        std::uint8_t px[4][4]{};
        const GLint at[4][2]{{480, 272}, {240, 136}, {720, 408}, {120, 450}};
        for (int k = 0; k < 4; ++k)
            glReadPixels(at[k][0], at[k][1], 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px[k]);
        char text[260];
        std::snprintf(text, sizeof text,
                      "vitagl: present %u, display pixels (480,272) %02x%02x%02x (240,136) %02x%02x%02x (720,408) %02x%02x%02x "
                      "(120,450) %02x%02x%02x, %u draws; display copies %llu on the GPU, %llu read back",
                      presents_ - 1, px[0][0], px[0][1], px[0][2], px[1][0], px[1][1], px[1][2], px[2][0], px[2][1],
                      px[2][2], px[3][0], px[3][1], px[3][2], draws_, static_cast<unsigned long long>(display_blits_),
                      static_cast<unsigned long long>(display_readbacks_));
        options.log(text);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    bound_ = display_bound_ = false;
    bound_colour_ = bound_depth_ = nullptr;
    const int vsync = sync_interval > 0 ? 1 : 0;
    if (vsync != vsync_) {
        vglWaitVblankStart(vsync ? GL_TRUE : GL_FALSE);
        vsync_ = vsync;
    }
    vglSwapBuffers(GL_FALSE);
    stats.scenes_last_frame = targets_;
    stats.draws_last_frame = draws_;
    targets_ = draws_ = 0;
    snapshot();
    return SwapChain::Presented::ok;
}

void Impl::message(const std::string& text) {
    glFinish();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    bound_ = display_bound_ = false;
    bound_colour_ = bound_depth_ = nullptr;
    SceMsgDialogUserMessageParam user{};
    user.buttonType = SCE_MSG_DIALOG_BUTTON_TYPE_OK;
    user.msg = reinterpret_cast<const SceChar8*>(text.c_str());
    SceMsgDialogParam param;
    sceMsgDialogParamInit(&param);
    param.mode = SCE_MSG_DIALOG_MODE_USER_MSG;
    param.userMsgParam = &user;
    if (sceMsgDialogInit(&param) < 0) return;
    while (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) {
        glViewport(0, 0, display_width, display_height);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        vglSwapBuffers(GL_TRUE);
    }
    sceMsgDialogTerm();
}

// ---- the API objects ----

class VglDevice final : public Device {
public:
    explicit VglDevice(Impl& impl) : impl_(impl) {}
    const std::string& name() const override { return name_; }
    Capabilities capabilities() const override { return {.block_textures = true}; }   // 132md11 (DXT1 / DXT5)
    std::shared_ptr<Buffer> create_buffer(const BufferDesc& desc, const void* initial, const char*) override {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        auto b = std::make_shared<VglBuffer>(desc);
        b->cpu.assign(align_up(std::max<std::size_t>(desc.bytes, 16), 16), 0);
        if (initial) std::memcpy(b->cpu.data(), initial, desc.bytes);
        if (!(desc.bind & bind_constants)) {
            glGenBuffers(1, &b->gl);
            glBindBuffer(GL_ARRAY_BUFFER, b->gl);
            glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizei>(b->cpu.size()), b->cpu.data(),
                         desc.usage == Usage::immutable ? GL_STATIC_DRAW : GL_DYNAMIC_DRAW);
        }
        return b;
    }
    std::shared_ptr<Texture> create_texture(const TextureDesc& desc, const Level* levels, const TextureLabels&) override {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        return impl_.make_texture(desc, levels);
    }
    std::shared_ptr<BlendState> create_blend(const BlendDesc& desc, const char*) override {
        auto s = std::make_shared<VglBlend>();
        s->desc = desc;
        return s;
    }
    std::shared_ptr<DepthState> create_depth(const DepthDesc& desc, const char*) override {
        auto s = std::make_shared<VglDepth>();
        s->desc = desc;
        return s;
    }
    std::shared_ptr<RasterState> create_raster(const RasterDesc& desc, const char*) override {
        auto s = std::make_shared<VglRaster>();
        s->desc = desc;
        return s;
    }
    std::shared_ptr<SamplerState> create_sampler(const SamplerDesc& desc, const char*) override {
        auto s = std::make_shared<VglSampler>();
        s->desc = desc;
        return s;
    }
    void prepare(Program program) override {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        const GxmProgram& g = gxm_program(program);
        impl_.shader(true, g.vertex);
        impl_.shader(false, g.fragment);
        if (g.fragment_alt != ~0U) impl_.shader(false, g.fragment_alt);
    }
    void submit(CommandList& list) override {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        impl_.submit(list);
    }
    void write(const Buffer& buffer, const void* data, std::size_t bytes, const char*) override {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        impl_.update_buffer(const_cast<VglBuffer&>(static_cast<const VglBuffer&>(buffer)), data, bytes, 0, false);
    }
    void read(const Texture& staging, unsigned subresource,
              const std::function<void(const std::uint8_t* data, unsigned row_pitch)>& reader, const char*) override {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        impl_.read(static_cast<const VglTexture&>(staging), subresource, reader);
    }
    bool removed() const override { return false; }
    bool wait_idle() override {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        glFinish();
        return true;
    }

private:
    Impl& impl_;
    std::string name_ = "vitaGL (temporary, PS Vita)";
};

class VglSwapChain final : public SwapChain {
public:
    explicit VglSwapChain(Impl& impl) : impl_(impl) {}
    void size(unsigned& width, unsigned& height, const char*) override {
        width = display_width;
        height = display_height;
    }
    std::shared_ptr<Texture> buffer(bool, const char*, const char*) override {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        return impl_.back_buffer();
    }
    void resize(unsigned, unsigned, const char*) override {}  // the Vita display is fixed at 960x544
    Presented present(unsigned sync_interval, const char*) override {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        return impl_.present(sync_interval);
    }

private:
    Impl& impl_;
};

std::shared_ptr<VglDevice> g_device;
std::shared_ptr<VglSwapChain> g_swap_chain;

void ensure_created(const Options& options) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (g_impl) return;
    g_impl = new Impl(options);  // one per process (vglInit); kept for the process lifetime
    g_device = std::make_shared<VglDevice>(*g_impl);
    g_swap_chain = std::make_shared<VglSwapChain>(*g_impl);
}

}  // namespace

Display create_display(const Options& options) {
    ensure_created(options);
    Display d;
    d.device = g_device;
    d.swap_chain = g_swap_chain;
    d.backend = "vitaGL (temporary)";
    return d;
}

MemoryStats memory_stats() {
    if (!g_impl) return {};
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    return g_impl->snapshot();
}

void show_message(const std::string& text) {
    if (!g_impl) return;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    g_impl->message(text);
}

}  // namespace omega::gfx::gxm

namespace omega::gfx {

// One vitaGL context per process: the GUI renderer's captures share it.
std::shared_ptr<Device> create_offscreen_device(const char*) {
    gxm::ensure_created(gxm::Options{});
    return gxm::g_device;
}

}  // namespace omega::gfx
