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
#include <psp2/gxm.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/message_dialog.h>
#include <vitaGL.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
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

// vitaGL's display depth / stencil surface (source/gxm.c; the library exports it).
extern "C" SceGxmDepthStencilSurface gxm_depth_stencil_surface;

namespace omega::gfx::gxm {
namespace {

constexpr unsigned display_width = 960, display_height = 544;
std::size_t align_up(std::size_t v, std::size_t a) { return (v + a - 1) / a * a; }

std::recursive_mutex g_mutex;  // every vitaGL call (the render thread, the loader's uploads, captures)
class Impl;
Impl* g_impl = nullptr;

// Port-only test switch (VITA.md "Vita3K"): ux0:data/omega_strain/vita_test_gl_trace.txt logs the backend's resource
// calls, submits and presents (the first 6000 after the play's window is made: arm_trace), to place an emulator abort.
int g_trace_left = 0;
bool g_trace_switch = false;
std::function<void(const std::string&)> g_trace_log;
void trace(const char* format, ...) {
    if (g_trace_left <= 0 || !g_trace_log) return;
    --g_trace_left;
    char text[200];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof text, format, args);
    va_end(args);
    g_trace_log(std::string("vgl trace: ") + text);
}

// 132vt2c (VITA.md "Memory"): the GPU memory the backend's buffers and textures hold, by the memory block vitaGL
// placed each in (its pools fall back CDRAM -> RAM -> phycont -> cdlg -> the newlib heap). Logged with the memory
// line, and each placement of 1 MB or more as it happens.
enum Pool : unsigned { pool_cdram, pool_ram, pool_phycont, pool_cdlg, pool_heap, pool_unknown, pool_count };
const char* const pool_names[pool_count] = {"CDRAM", "RAM pool", "phycont", "cdlg", "newlib heap", "unplaced"};
constexpr std::size_t placed_bytes = 64u << 10;  // smaller objects are not looked up (a syscall each): "unplaced"
struct Placement {
    std::size_t live[pool_count]{}, peak[pool_count]{};
    std::size_t depth_live = 0, depth_peak = 0;  // the framebuffer objects' depth buffers (vitaGL's own; estimated)
    std::size_t largest = 0;
    Pool largest_pool = pool_unknown;
};
Placement g_placement;

Pool pool_of(const void* p) {
    if (!p) return pool_unknown;
    SceKernelMemBlockInfo info{};
    info.size = sizeof info;
    if (sceKernelGetMemBlockInfoByAddr(const_cast<void*>(p), &info) < 0) return pool_unknown;
    const unsigned type = unsigned(info.type);
    if ((type & 0xff000000u) == 0x09000000u) return pool_cdram;
    if (type == SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW || type == SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW)
        return pool_phycont;
    if (type == SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_CDIALOG_RW || type == SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_CDIALOG_NC_RW ||
        type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDIALOG_RW || type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDIALOG_NC_RW)
        return pool_cdlg;
    // Both vitaGL's RAM pool and the newlib heap (which vglUseExtraMem maps for the GPU) are main RW memory blocks.
    return info.mappedSize == vglMemTotal(VGL_MEM_RAM) ? pool_ram : pool_heap;
}

void place(std::uint8_t& pool, std::size_t& held, Pool now, std::size_t bytes) {
    if (held) g_placement.live[pool] -= std::min(g_placement.live[pool], held);
    pool = std::uint8_t(now);
    held = bytes;
    g_placement.live[now] += bytes;
    g_placement.peak[now] = std::max(g_placement.peak[now], g_placement.live[now]);
    if (bytes > g_placement.largest) {
        g_placement.largest = bytes;
        g_placement.largest_pool = now;
    }
}
void unplace(std::uint8_t pool, std::size_t& held) {
    g_placement.live[pool] -= std::min(g_placement.live[pool], held);
    held = 0;
}

// ---- resources ----

struct VglBuffer final : Buffer {
    explicit VglBuffer(const BufferDesc& d) { desc_ = d; }
    ~VglBuffer() override;
    std::vector<std::uint8_t> cpu;  // the buffer's bytes (constants are only here; the shadow filter reads vertices)
    GLuint gl = 0;                  // vertex / index buffers
    // 132vt2 (research/VITA.md "Memory"): a large vertex / index buffer (the level's scene mesh, 34 MB in TORONTO1)
    // keeps no copy in the heap: it is written and read through vitaGL's own memory (glMapBuffer), `cpu` empty.
    bool gl_only = false;
    // 132vt2e (Device::cpu_mirror): the owner's copy of a gl_only buffer's bytes in cached RAM; the shadow filter reads
    // it instead of the GL memory (CDRAM, uncached for the CPU: research/VITA.md "Shadow receivers").
    const std::uint8_t* mirror = nullptr;
    std::uint8_t pool = pool_unknown;  // where vitaGL placed it (g_placement)
    std::size_t placed = 0;
};

constexpr std::size_t gl_only_bytes = 1u << 20;

// The bytes of a vertex / index buffer (its GL memory when it keeps no heap copy).
const std::uint8_t* buffer_bytes(const VglBuffer& b) {
    if (!b.gl_only) return b.cpu.data();
    if (b.mirror) return b.mirror;
    glBindBuffer(GL_ARRAY_BUFFER, b.gl);
    const auto* p = static_cast<const std::uint8_t*>(glMapBuffer(GL_ARRAY_BUFFER, GL_READ_ONLY));
    glUnmapBuffer(GL_ARRAY_BUFFER);
    if (!p) throw std::runtime_error("gfx vitagl: glMapBuffer failed");
    return p;
}

// Writes bytes into a heap-less buffer's GL memory in place (vitaGL's glBufferSubData would copy the whole buffer
// each frame the GPU has used it: 34 MB per pathed-object update).
void write_gl_only(VglBuffer& b, const void* data, std::size_t bytes, std::size_t offset) {
    glBindBuffer(GL_ARRAY_BUFFER, b.gl);
    auto* p = static_cast<std::uint8_t*>(glMapBuffer(GL_ARRAY_BUFFER, GL_WRITE_ONLY));
    if (!p) throw std::runtime_error("gfx vitagl: glMapBuffer failed");
    if (data) std::memcpy(p + offset, data, bytes);
    else std::memset(p + offset, 0, bytes);
    glUnmapBuffer(GL_ARRAY_BUFFER);
}

struct VglTexture final : Texture {
    VglTexture() = default;
    ~VglTexture() override;
    GLuint gl = 0;
    unsigned layer_rows = 0;  // rows per layer (layers stacked vertically, as the GXM backend)
    bool depth = false;       // d24s8: no GL object (vitaGL keeps the depth buffer in each framebuffer object)
    std::uint32_t clear_serial = 0;
    float clear_value = 1.0F;
    std::uint8_t pool = pool_unknown;  // where vitaGL placed it (g_placement)
    std::size_t placed = 0;
    std::uint8_t applied_sampler = 0xff;  // 132vt2e: the filter / wrap execute() last set on it (0xff: unknown)
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
    // 132vt2e: the values last given to each uniform (GL keeps them in the program): an unchanged one is not set again.
    std::vector<float> sent[10];
    bool sent_valid[10]{};
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
    std::size_t depth_bytes = 0;               // g_placement's estimate of the depth buffer
};

bool file_exists(const char* path) {
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}

// research/VITA.md "Depth across scenes": vitaGL begins a GXM scene at each target switch and each copy out of the
// display, and its depth / stencil surfaces neither load nor store, so every scene starts with a cleared depth buffer:
// geometry drawn after a mid-frame scene break is not hidden by what was drawn before it (the hardware's see-through
// walls, 132vt2e). Force load / store keeps the surface's memory across the break.
void keep_depth(SceGxmDepthStencilSurface* surface) {
    sceGxmDepthStencilSurfaceSetForceLoadMode(surface, SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED);
    sceGxmDepthStencilSurfaceSetForceStoreMode(surface, SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED);
}

// A framebuffer object's depth surface: vitaGL creates it at the object's first scene, inside its framebuffer struct
// (the object's name is the struct's address; this vitaGL's layout: the surface at +0x38, its pointer at +0x4c, as in
// source/shared.h and the library's scene_reset). Only a pointer to the struct's own surface is taken.
void keep_fbo_depth(Framebuffer& f) {
    auto* base = reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(f.fbo));
    auto* const own = reinterpret_cast<SceGxmDepthStencilSurface*>(base + 0x38);
    SceGxmDepthStencilSurface* surface = nullptr;
    std::memcpy(&surface, base + 0x4c, sizeof surface);
    if (surface != own || sceGxmDepthStencilSurfaceGetForceLoadMode(own) == SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED)
        return;
    keep_depth(own);
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
    void execute(Pipeline& p, bool textures_only = false);
    void draw(bool indexed, std::uint32_t count, std::uint32_t first);
    // 132vt2e: an indexed draw whose only recorded change since the previous one is its textures (the level's
    // terrain and models: about 400 of TORONTO1's 940 draws a frame): the previous draw's pipeline with the new
    // textures, its sizes and the range; the rest of the GL state is the same (research/VITA.md "Draw batching").
    void draw_textures_only(std::uint32_t count, std::uint32_t first);
    Pipeline last_pipeline_;
    GlProgram* last_program_ = nullptr;
    bool textures_only_ready_ = false;
    std::uint64_t textures_only_draws_ = 0;
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
    // Records where vitaGL put an object's memory (g_placement); logs placements of 1 MB or more.
    void placed(std::uint8_t& pool, std::size_t& held, const void* p, std::size_t bytes, const char* what);
    // The same for a heap-less buffer just given its memory (bound): throws when vitaGL found none.
    void placed_buffer(VglBuffer& b, std::size_t bytes);

private:
    void copy_from_display(VglTexture& dst, unsigned layer, unsigned x, unsigned y, const Box& b);
    void blit_from_display(VglTexture& dst, unsigned sx, unsigned sy, unsigned dx, unsigned dy, unsigned w, unsigned h,
                           bool flip);
    bool check_display_blit(VglTexture& dst, unsigned sx, unsigned sy, unsigned dx, unsigned dy, unsigned w, unsigned h,
                            const std::vector<std::uint8_t>& shown);
    void write_rows(VglTexture& t, unsigned left, unsigned top, unsigned right, unsigned bottom, unsigned layer,
                    const std::uint8_t* data, unsigned pitch);

    std::map<std::uint32_t, CompiledShader> shaders_;
    std::map<std::tuple<unsigned, unsigned, const GxmAttribute*>, GlProgram> programs_;
    GlProgram* cached_program_ = nullptr;  // program()'s last result and key
    std::tuple<unsigned, unsigned, const GxmAttribute*> cached_program_key_{};
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
    // 132vt2e: what separates consecutive indexed draws (the command ops recorded between them, bit 31: the index
    // ranges continue), logged every 150 presents: the evidence for which state changes keep draws apart.
    std::uint32_t ops_since_draw_ = ~0u, last_draw_end_ = ~0u;
    std::map<std::uint32_t, std::uint32_t> draw_breaks_;
    unsigned break_frames_ = 0;
    std::vector<std::uint32_t> filter_kept_;  // the shadow filter's kept indices (reused: no allocation per draw)
    // copy_from_display's GPU path: unchecked until its first use, then the row order it matched the read-back with.
    enum class DisplayBlit { unchecked, direct, flipped, failed } display_blit_ = DisplayBlit::unchecked;
    std::uint64_t display_blits_ = 0, display_readbacks_ = 0;
    // 132vt2e (research/VITA.md "Draw batching"): the GL state execute() last set. vitaGL turns most state calls into
    // GXM commands, and glUseProgram marks every uniform dirty (a new uniform buffer and a copy per draw), so state
    // that equals the last draw's is not set again. Any other GL use (a framebuffer bind, texture writes, the
    // display copy, a present) forgets it; Options::gl_state_cache false forgets it before every draw (the old calls).
    struct Applied {
        bool valid = false;
        GLuint program = 0;
        GLint viewport[4]{};
        float depth_range[2]{};
        bool wireframe = false;
        bool depth_enabled = false;
        GLenum depth_func = 0;
        bool depth_write = false;
        std::uint8_t mask = 0;
        bool blend = false;
        GLenum blend_func[4]{}, blend_op[2]{};
        GLuint texture[4]{};
        bool vertices = false;  // the attribute pointers below are set
        const GlProgram* vertex_program = nullptr;
        GLuint vertex_buffer = 0;
        std::size_t vertex_offset = 0;
        std::uint32_t stride = 0;
    } applied_;
    void forget_gl_state() {
        applied_.valid = false;
        textures_only_ready_ = false;
    }
    void count_state(bool set) { ++(set ? stats.gl_state_set : stats.gl_state_skipped); }
};

VglBuffer::~VglBuffer() {
    if (!gl) return;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    unplace(pool, placed);
    glDeleteBuffers(1, &gl);
}

VglTexture::~VglTexture() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (g_impl) g_impl->forget(this);
    unplace(pool, placed);
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
    if (options.keep_depth) keep_depth(&gxm_depth_stencil_surface);
    if (options.log) options.log(options.keep_depth ? "vitagl: depth kept across scenes (force load / store)"
                                                    : "vitagl: depth cleared at every scene (vitaGL's default)");
    if (options.log && file_exists("ux0:data/omega_strain/vita_test_gl_trace.txt")) {
        g_trace_switch = true;
        g_trace_log = options.log;
    }
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

void Impl::placed(std::uint8_t& pool, std::size_t& held, const void* p, std::size_t bytes, const char* what) {
    const Pool now = bytes >= placed_bytes ? pool_of(p) : pool_unknown;
    place(pool, held, now, bytes);
    if (bytes >= (1u << 20) && options.log) {
        char text[200];
        std::snprintf(text, sizeof text, "vitagl: %s of %u KB placed in %s (vitaGL free: VRAM %u KB, RAM %u KB)", what,
                      unsigned(bytes >> 10), pool_names[now], unsigned(vglMemFree(VGL_MEM_VRAM) >> 10),
                      unsigned(vglMemFree(VGL_MEM_RAM) >> 10));
        options.log(text);
    }
}

void Impl::placed_buffer(VglBuffer& b, std::size_t bytes) {
    const void* p = glMapBuffer(GL_ARRAY_BUFFER, GL_READ_ONLY);
    glUnmapBuffer(GL_ARRAY_BUFFER);
    if (!p) {
        char text[160];
        std::snprintf(text, sizeof text, "gfx vitagl: out of GPU memory for a %u KB buffer (VITA.md \"Memory\")",
                      unsigned(bytes >> 10));
        throw std::runtime_error(text);
    }
    const bool indices = b.desc().bind & bind_indices;
    placed(b.pool, b.placed, p, bytes, indices ? "an index buffer" : "a vertex buffer");
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
    if (cached_program_ && key == cached_program_key_) return *cached_program_;  // std::map nodes stay put
    auto found = programs_.find(key);
    if (found != programs_.end()) {
        cached_program_key_ = key;
        return *(cached_program_ = &found->second);
    }
    const CompiledShader& vs = shader(true, p.vertex);
    const CompiledShader& fs = shader(false, p.fragment);
    GlProgram g;
    g.gl = glCreateProgram();
    glAttachShader(g.gl, vs.gl);
    glAttachShader(g.gl, fs.gl);
    for (unsigned k = 0; k < p.attribute_count && k < 8; ++k) glBindAttribLocation(g.gl, k, p.attributes[k].name);
    forget_gl_state();  // glUseProgram below
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
    cached_program_key_ = key;
    return *(cached_program_ = &programs_.emplace(key, g).first->second);
}

// ---- textures ----

void Impl::write_rows(VglTexture& t, unsigned left, unsigned top, unsigned right, unsigned bottom, unsigned layer,
                      const std::uint8_t* data, unsigned pitch) {
    forget_gl_state();
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
    forget_gl_state();
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
        const void* data = vglGetTexDataPointer(GL_TEXTURE_2D);
        if (!data) throw std::runtime_error("gfx vitagl: out of texture memory (VITA.md \"Memory\")");
        placed(t->pool, t->placed, data, std::size_t(bytes), "a texture");
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
    placed(t->pool, t->placed, base, t->stride() * t->rows(), (desc.bind & bind_target) ? "a target" : "a texture");
    const unsigned mips = std::max(desc.mips, 1u);
    if (levels)
        for (unsigned l = 0; l < layers; ++l)
            if (levels[l * mips].data)
                write_rows(*t, 0, 0, desc.width, t->layer_rows, l, static_cast<const std::uint8_t*>(levels[l * mips].data),
                           levels[l * mips].row_pitch);
    return t;
}

void Impl::forget(const VglTexture* t) {
    forget_gl_state();  // its name may be bound to a unit
    for (auto it = framebuffers_.begin(); it != framebuffers_.end();) {
        if (it->first.first == t || it->first.second == t) {
            if (bound_ && bound_colour_ == it->first.first && bound_depth_ == it->first.second) {
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                bound_ = false;
            }
            if (it->second.depth_buffer) glDeleteRenderbuffers(1, &it->second.depth_buffer);
            g_placement.depth_live -= std::min(g_placement.depth_live, it->second.depth_bytes);
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
            forget_gl_state();
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
                f.depth_bytes = std::size_t(align_up(attach->desc().width, 32)) * align_up(attach->rows(), 32) * 5;
                g_placement.depth_live += f.depth_bytes;
                g_placement.depth_peak = std::max(g_placement.depth_peak, g_placement.depth_live);
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
        forget_gl_state();
    }
    if (depth) {
        Framebuffer& f = framebuffers_[std::make_pair(colour, depth)];
        if (options.keep_depth && !display_bound_ && f.fbo) keep_fbo_depth(f);
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

void Impl::execute(Pipeline& p, bool textures_only) {
    OMEGA_ZONE_LAPS(lap);
    textures_only = textures_only && textures_only_ready_ && last_program_ && options.gl_state_cache;
    textures_only_ready_ = false;
    GlProgram& g = textures_only ? *last_program_ : program(p);
    last_program_ = &g;
    OMEGA_ZONE_LAP(lap, "vgl.program_lookup");
    if (!options.gl_state_cache) forget_gl_state();
    const bool all = !applied_.valid;
    Applied& a = applied_;
    a.valid = true;
    const Viewport& v = p.viewport;
    const bool display = bound_ && display_bound_;
    if (!textures_only) {
    const bool use = all || a.program != g.gl;
    count_state(use);
    if (use) {
        glUseProgram(g.gl);
        a.program = g.gl;
    }
    const GLint vy = static_cast<GLint>(std::lround(v.y));
    const GLsizei vh = static_cast<GLsizei>(std::lround(v.height));
    // vitaGL's display viewport is bottom-up (GL); a framebuffer object's is top-down with the flip uniform below.
    const GLint viewport[4]{static_cast<GLint>(std::lround(v.x)), display ? static_cast<GLint>(display_height) - vy - vh : vy,
                            static_cast<GLint>(std::lround(v.width)), vh};
    const bool new_viewport = all || std::memcmp(viewport, a.viewport, sizeof viewport) != 0;
    count_state(new_viewport);
    if (new_viewport) {
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        std::memcpy(a.viewport, viewport, sizeof viewport);
    }
    const float range[2]{2.0F * v.min_depth - v.max_depth, v.max_depth};
    const bool new_range = all || range[0] != a.depth_range[0] || range[1] != a.depth_range[1];
    count_state(new_range);
    if (new_range) {
        glDepthRangef(range[0], range[1]);
        a.depth_range[0] = range[0];
        a.depth_range[1] = range[1];
    }
    const bool new_mode = all || p.wireframe != a.wireframe;
    count_state(new_mode);
    if (new_mode) {
        glPolygonMode(GL_FRONT_AND_BACK, p.wireframe ? GL_LINE : GL_FILL);
        a.wireframe = p.wireframe;
    }
    // GL's depth enable, function and mask are separate state; each is set when it differs.
    const bool depth_enabled = p.depth_test || p.depth_write;
    const GLenum depth_func = depth_enabled ? (p.depth_test ? p.depth_func : GL_ALWAYS) : all ? GLenum(~0u) : a.depth_func;
    const bool depth_write = depth_enabled && p.depth_write;
    const bool new_depth = all || depth_enabled != a.depth_enabled || (depth_enabled && depth_func != a.depth_func) ||
                           depth_write != a.depth_write;
    count_state(new_depth);
    if (new_depth) {
        if (depth_enabled) {
            if (all || !a.depth_enabled) glEnable(GL_DEPTH_TEST);
            if (all || depth_func != a.depth_func) glDepthFunc(depth_func);
            if (all || depth_write != a.depth_write) glDepthMask(depth_write ? GL_TRUE : GL_FALSE);
        } else {
            if (all || a.depth_enabled) glDisable(GL_DEPTH_TEST);
            if (all || a.depth_write) glDepthMask(GL_FALSE);
        }
        a.depth_enabled = depth_enabled;
        a.depth_func = depth_func;
        a.depth_write = depth_write;
    }
    const bool new_mask = all || p.mask != a.mask;
    count_state(new_mask);
    if (new_mask) {
        glColorMask((p.mask & mask_r) ? GL_TRUE : GL_FALSE, (p.mask & mask_g) ? GL_TRUE : GL_FALSE,
                    (p.mask & mask_b) ? GL_TRUE : GL_FALSE, (p.mask & mask_a) ? GL_TRUE : GL_FALSE);
        a.mask = p.mask;
    }
    if (p.blend) {
        const GLenum func[4]{p.colour_src, p.colour_dst, p.alpha_src, p.alpha_dst}, op[2]{p.colour_op, p.alpha_op};
        const bool new_func = all || std::memcmp(func, a.blend_func, sizeof func) != 0;
        const bool new_op = all || op[0] != a.blend_op[0] || op[1] != a.blend_op[1];
        count_state(all || !a.blend || new_func || new_op);
        if (all || !a.blend) glEnable(GL_BLEND);
        if (new_func) glBlendFuncSeparate(func[0], func[1], func[2], func[3]);
        if (new_op) glBlendEquationSeparate(op[0], op[1]);
        a.blend = true;
        std::memcpy(a.blend_func, func, sizeof func);
        a.blend_op[0] = op[0];
        a.blend_op[1] = op[1];
    } else {
        count_state(all || a.blend);
        if (all || a.blend) glDisable(GL_BLEND);
        if (all) {  // the functions GL holds are unknown
            std::fill(std::begin(a.blend_func), std::end(a.blend_func), GLenum(~0u));
            a.blend_op[0] = a.blend_op[1] = GLenum(~0u);
        }
        a.blend = false;
    }
    }

    float tex_size[16];
    float layers[4];
    bool unit_moved = false;
    OMEGA_ZONE_LAP(lap, "vgl.fixed_state");
    for (unsigned k = 0; k < 4; ++k) {
        const VglTexture* t = p.textures[k].texture ? p.textures[k].texture : dummy_.get();
        if (t->depth) throw std::runtime_error("gfx vitagl: sampling a depth texture is unported");
        if (t == back_.get()) throw std::runtime_error("gfx vitagl: sampling the swap chain buffer (the display) is unported");
        const SamplerDesc& s = p.textures[k].sampler;
        const std::uint8_t sampler = static_cast<std::uint8_t>(unsigned(s.filter) * 2 + (s.address == Address::wrap ? 1 : 0));
        const bool bind = all || a.texture[k] != t->gl;
        const bool params = all || t->applied_sampler != sampler;
        count_state(bind || params);
        if (bind || params) {
            glActiveTexture(GL_TEXTURE0 + k);
            unit_moved = unit_moved || k != 0;
        }
        if (bind) {
            glBindTexture(GL_TEXTURE_2D, t->gl);
            a.texture[k] = t->gl;
        }
        if (params) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, s.filter != Filter::linear ? GL_NEAREST : GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, s.filter == Filter::point ? GL_NEAREST : GL_LINEAR);
            const GLint wrap = s.address == Address::wrap ? GL_REPEAT : GL_CLAMP_TO_EDGE;
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
            const_cast<VglTexture*>(t)->applied_sampler = options.gl_state_cache ? sampler : 0xff;
        }
        const float w = static_cast<float>(std::max(t->desc().width, 1u)), h = static_cast<float>(t->layer_rows);
        tex_size[4 * k] = w;
        tex_size[4 * k + 1] = h;
        tex_size[4 * k + 2] = 1.0F / w;
        tex_size[4 * k + 3] = 1.0F / h;
        layers[k] = static_cast<float>(std::max(t->desc().layers, 1u));
    }
    if (all || unit_moved) glActiveTexture(GL_TEXTURE0);

    OMEGA_ZONE_LAP(lap, "vgl.textures");
    const bool cache = options.gl_state_cache;
    auto set = [&](unsigned slot, GLint location, const void* data, std::size_t floats, unsigned declared) {
        if (location == -1 || location == 0 || !data) return;
        const unsigned n = static_cast<unsigned>(std::min<std::size_t>(floats / 4, declared));
        if (!n) return;
        std::vector<float>& sent = g.sent[slot];
        const std::size_t bytes = std::size_t(n) * 16;
        const bool same = cache && g.sent_valid[slot] && sent.size() == std::size_t(n) * 4 &&
                          std::memcmp(sent.data(), data, bytes) == 0;
        count_state(!same);
        if (same) return;
        glUniform4fv(location, static_cast<GLsizei>(n), static_cast<const GLfloat*>(data));
        sent.resize(std::size_t(n) * 4);
        std::memcpy(sent.data(), data, bytes);
        g.sent_valid[slot] = cache;
    };
    if (!textures_only) {
        for (unsigned k = 0; k < 2; ++k) {
            set(k, g.vertex_constants[k], p.vertex_constants[k].data, p.vertex_constants[k].bytes / 4, g.vertex_count[k]);
            set(2 + k, g.fragment_constants[k], p.fragment_constants[k].data, p.fragment_constants[k].bytes / 4, g.fragment_count[k]);
        }
        const float flip[4]{bound_ && !display ? -1.0F : 1.0F, 0, 0, 0};
        set(4, g.flip, flip, 4, 1);
        set(5, g.clear, p.clear, 4, 1);
    }
    set(6, g.tex_size, tex_size, 16, 4);
    set(7, g.layers, layers, 4, 1);
    if (!textures_only) {
        set(8, g.blend_mul, p.blend_mul, 4, 1);
        set(9, g.colour, p.colour, 4, 1);
    }

    // The attribute pointers (vitaGL keeps each one's buffer name and offset): set again when the program, buffer,
    // offset or stride differ, and always for client vertices.
    OMEGA_ZONE_LAP(lap, "vgl.uniforms");
    const bool new_vertices = !textures_only && (all || !a.vertices || !p.vertex_buffer || a.vertex_program != &g ||
                              a.vertex_buffer != p.vertex_buffer || a.vertex_offset != p.vertex_offset ||
                              a.stride != p.stride);
    count_state(new_vertices);
    if (new_vertices) {
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
        a.vertices = p.vertex_buffer != 0;
        a.vertex_program = &g;
        a.vertex_buffer = p.vertex_buffer;
        a.vertex_offset = p.vertex_offset;
        a.stride = p.stride;
    }
    OMEGA_ZONE_LAP(lap, "vgl.attributes");

    if (p.indexed) {
        OMEGA_ZONE("vgl.gl_draw");
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, p.index_buffer);
        const void* indices = p.index_buffer ? reinterpret_cast<const void*>(std::size_t(p.first) * 4)
                                             : static_cast<const void*>(p.client_indices);
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(p.count), GL_UNSIGNED_INT, indices);
    } else {
        OMEGA_ZONE("vgl.gl_draw");
        glDrawArrays(GL_TRIANGLES, static_cast<GLint>(p.first), static_cast<GLsizei>(p.count));
    }
    ++draws_;
    stats.triangles_total += p.count / 3;
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
    std::vector<std::uint32_t>& kept = filter_kept_;
    kept.clear();
    if (g.cpu_geometry) {
        OMEGA_ZONE("vgl.shadow_filter");
        const VglBuffer* shadow = state_.geometry_constants[1] ? state_.geometry_constants[1] : state_.vertex_constants[1];
        if (!shadow || shadow->cpu.size() < 112) throw std::runtime_error("gfx vitagl: shadow_receiver without Shadow constants");
        ShadowFilter filter;
        std::memcpy(filter.clip, shadow->cpu.data(), sizeof filter.clip);
        std::memcpy(filter.facing, shadow->cpu.data() + 96, sizeof filter.facing);
        const std::uint8_t* vertices = buffer_bytes(*state_.vertices) + state_.offset;
        const auto* source = indexed ? reinterpret_cast<const std::uint32_t*>(buffer_bytes(*state_.indices)) + first : nullptr;
        kept.reserve(count);
        stats.shadow_tested += count / 3;
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
        stats.shadow_kept += kept.size() / 3;
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
        {
            OMEGA_ZONE("vgl.target");
            use_target(state_.colour[pass], state_.depth);
        }
        p.viewport = in_layer(state_.viewport, state_.colour[pass], state_.layer[pass]);
        p.fragment = pass ? g.fragment_alt : g.fragment;
        blend_for(blend.targets[pass && blend.independent ? 1 : 0], state_.factor, p);
        OMEGA_ZONE("vgl.execute");
        execute(p);
    }
    if (!second && p.indexed && p.index_buffer && !g.cpu_geometry) {
        last_pipeline_ = p;
        textures_only_ready_ = true;
    }
}

void Impl::draw_textures_only(std::uint32_t count, std::uint32_t first) {
    OMEGA_ZONE("vgl.draw");
    if (!count) return;
    if (!state_.indices || state_.indices->gl != last_pipeline_.index_buffer ||
        (std::size_t(first) + count) * 4 > state_.indices->desc().bytes)
        throw std::runtime_error("gfx vitagl: an indexed draw past its index buffer");
    Pipeline& p = last_pipeline_;
    p.first = first;
    p.count = count;
    for (unsigned k = 0; k < 4; ++k) p.textures[k].texture = state_.textures[k];
    {
        OMEGA_ZONE("vgl.execute");
        execute(p, true);
    }
    ++textures_only_draws_;
    textures_only_ready_ = true;
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
    applied_.depth_range[0] = -1.0F;
    applied_.depth_range[1] = 1.0F;
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

// A copy out of the display. A copy into a single-layer texture (the 132ws wide side fill and the vision blur: the
// whole display, every frame; 132vt2: the heat-haze sprites' boxes, one per sprite) is drawn on the GPU:
// glBlitFramebuffer samples the display's colour surface into the texture's framebuffer object, with no wait. Its
// first use is checked against the read-back below (research/VITA.md "Widescreen"); copies into array layers, a failed
// check and Options::display_copy_readback (a test switch) read back: glReadPixels (vitaGL ends the scene and waits
// for the GPU, then the CPU reads the uncached display), rows bottom-up.
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
    const bool single = layer == 0 && dst.rows() == dst.layer_rows;
    const bool gpu = single && !options.display_copy_readback && display_blit_ != DisplayBlit::failed;
    if (gpu && display_blit_ == DisplayBlit::unchecked && options.display_copy_order)
        display_blit_ = options.display_copy_order == 2 ? DisplayBlit::flipped : DisplayBlit::direct;
    if (gpu && display_blit_ != DisplayBlit::unchecked) {
        blit_from_display(dst, box.left, box.top, x, y, w, h, display_blit_ == DisplayBlit::flipped);
        ++display_blits_;
        return;
    }
    forget_gl_state();
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
    if (gpu && check_display_blit(dst, box.left, box.top, x, y, w, h, up)) return;
    write_rows(dst, x, y, x + w, y + h, layer, rows.data(), w * 4);
    ++display_readbacks_;
}

// The GPU copy: the display's w x h at (sx, sy) (top-left origin; GL rows display_height - sy - h .. display_height -
// sy, as vitaGL samples the display) drawn at (dx, dy) of the destination, point-sampled. vitaGL's destination rows
// are in the order the check found: `flip` (the handheld's) has the destination's GL row = its row from the top, else
// GL row = rows() - its row from the top. The whole-display copy is sx = sy = dx = dy = 0, w x h = the texture.
void Impl::blit_from_display(VglTexture& dst, unsigned sx, unsigned sy, unsigned dx, unsigned dy, unsigned w,
                             unsigned h, bool flip) {
    use_target(&dst, nullptr);
    forget_gl_state();
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    const GLint top = static_cast<GLint>(display_height - sy), bottom = top - static_cast<GLint>(h);
    const GLint x0 = static_cast<GLint>(sx), x1 = x0 + static_cast<GLint>(w);
    const GLint d0 = static_cast<GLint>(dx), d1 = d0 + static_cast<GLint>(w);
    if (flip)
        glBlitFramebuffer(x0, top, x1, bottom, d0, static_cast<GLint>(dy), d1, static_cast<GLint>(dy + h),
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
    else
        glBlitFramebuffer(x0, bottom, x1, top, d0, static_cast<GLint>(dst.rows() - dy - h),
                          d1, static_cast<GLint>(dst.rows() - dy), GL_COLOR_BUFFER_BIT, GL_NEAREST);
    bound_ = false;  // the read binding is the display now: the next target binds both again
}

// The first whole-display copy: the GPU copy in both row orders, each compared byte for byte with the read-back
// (`shown`, rows bottom-up as glReadPixels gives them; the texture keeps RGBA bytes for both formats). The matching
// order is kept; neither matching: the read-back path from then on. True when the texture holds the copy.
bool Impl::check_display_blit(VglTexture& dst, unsigned sx, unsigned sy, unsigned dx, unsigned dy, unsigned w,
                              unsigned h, const std::vector<std::uint8_t>& shown) {
    std::size_t differ[2]{};
    char where[2][96]{};
    for (const bool flip : {false, true}) {
        blit_from_display(dst, sx, sy, dx, dy, w, h, flip);
        glFinish();
        forget_gl_state();
        glBindTexture(GL_TEXTURE_2D, dst.gl);
        const auto* base = static_cast<const std::uint8_t*>(vglGetTexDataPointer(GL_TEXTURE_2D));
        if (!base) {
            differ[flip] = std::size_t(w) * h * 4;
            continue;
        }
        unsigned rows = 0, first = h, last = 0;
        for (unsigned r = 0; r < h; ++r) {
            const std::uint8_t* got = base + std::size_t(dy + r) * dst.stride() + std::size_t(dx) * 4;
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
    stats.buffer_bytes += bytes;
    const bool whole = !keep_rest || (offset == 0 && bytes == size);
    if (b.gl_only) {
        if (!bytes) return;
        if (b.desc().usage == Usage::dynamic) {
            glBindBuffer(GL_ARRAY_BUFFER, b.gl);
            if (whole) {
                ++stats.buffer_reallocs;
                glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizei>(align_up(std::max<std::size_t>(size, 16), 16)), nullptr,
                             GL_DYNAMIC_DRAW);
                placed_buffer(b, b.placed);
                write_gl_only(b, data, bytes, offset);
            } else {
                ++stats.buffer_writes;
                glBufferSubData(GL_ARRAY_BUFFER, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(bytes), data);
            }
        } else {
            ++stats.buffer_writes;
            write_gl_only(b, data, bytes, offset);
        }
        return;
    }
    if (b.cpu.size() < size) b.cpu.resize(size);
    if (bytes) std::memcpy(b.cpu.data() + offset, data, bytes);
    if (!b.gl || !bytes) {
        ++stats.buffer_writes;
        return;
    }
    glBindBuffer(GL_ARRAY_BUFFER, b.gl);
    // vitaGL gives a buffer the GPU may still read fresh memory (the old freed by its garbage collector).
    ++(whole ? stats.buffer_reallocs : stats.buffer_writes);
    if (whole)
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
    forget_gl_state();
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
        const std::uint32_t between = ops_since_draw_;
        if (c.op == Command::Op::draw_indexed) {
            if (ops_since_draw_ != ~0u)
                ++draw_breaks_[ops_since_draw_ | (c.b == last_draw_end_ ? 0x80000000u : 0u)];
            ops_since_draw_ = 0;
            last_draw_end_ = c.b + c.a;
        } else if (ops_since_draw_ != ~0u) {
            ops_since_draw_ |= 1u << unsigned(c.op);
        }
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
        case Command::Op::draw_indexed:
            if (between == 1u << unsigned(Command::Op::set_textures) && textures_only_ready_ && options.gl_state_cache)
                draw_textures_only(c.a, c.b);
            else
                draw(true, c.a, c.b);
            break;
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
    ops_since_draw_ = ~0u;  // another list may follow work recorded outside the lists
    list.clear();
}

// The frame was drawn straight into vitaGL's display buffer: the flip. Every 600th frame logs four display pixels
// (glReadPixels; the bring-up evidence that the picture reached the display buffer).
SwapChain::Presented Impl::present(unsigned sync_interval) {
    OMEGA_ZONE("vgl.present");
    forget_gl_state();
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
    if (options.log && (++break_frames_ >= 150 || (break_frames_ >= 3 && draws_ * break_frames_ > 6000)) && !draw_breaks_.empty()) {
        static const char* const names[]{"targets", "viewport", "clear", "clear_depth", "clear_rects", "raster", "depth",
                                         "blend", "program", "vertices", "indices", "constants", "textures", "sampler",
                                         "update_buffer", "update_texture", "draw", "draw_indexed", "copy", "copy_region",
                                         "flush", "reset"};
        std::vector<std::pair<std::uint32_t, std::uint32_t>> top(draw_breaks_.begin(), draw_breaks_.end());
        std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        std::string text = "vitagl draw breaks per frame over " + std::to_string(break_frames_) + " presents (" + std::to_string(textures_only_draws_ / break_frames_) + " drawn with only new textures; the ops between consecutive indexed draws):";
        textures_only_draws_ = 0;
        for (std::size_t k = 0; k < top.size() && k < 10; ++k) {
            char item[40];
            std::snprintf(item, sizeof item, " %.1f x", top[k].second / double(break_frames_));
            text += k ? ";" : "";
            text += item;
            for (unsigned bit = 0; bit < 22; ++bit)
                if (top[k].first & (1u << bit)) (text += ' ') += names[bit];
            if (!(top[k].first & 0x7fffffffu)) text += " none";
            if (top[k].first & 0x80000000u) text += " (contiguous)";
        }
        options.log(text.c_str());
        draw_breaks_.clear();
        break_frames_ = 0;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    bound_ = display_bound_ = false;
    bound_colour_ = bound_depth_ = nullptr;
    // The sync interval in vertical blanks (2 for the level's 30 fps steps: vitaGL's display callback waits that many).
    const int vsync = static_cast<int>(std::min(sync_interval, 4u));
    if (vsync != vsync_) {
        eglSwapInterval(nullptr, vsync);
        vsync_ = vsync;
    }
    vglSwapBuffers(GL_FALSE);
    if (options.presented) options.presented();
    stats.scenes_last_frame = targets_;
    stats.draws_last_frame = draws_;
    stats.scenes_total += targets_;
    stats.draws_total += draws_;
    targets_ = draws_ = 0;
    snapshot();
    return SwapChain::Presented::ok;
}

void Impl::message(const std::string& text) {
    forget_gl_state();
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
        trace("create_buffer %u bytes, bind %u", unsigned(desc.bytes), unsigned(desc.bind));
        auto b = std::make_shared<VglBuffer>(desc);
        const std::size_t padded = align_up(std::max<std::size_t>(desc.bytes, 16), 16);
        if (!(desc.bind & bind_constants) && padded >= gl_only_bytes) {
            b->gl_only = true;
            glGenBuffers(1, &b->gl);
            glBindBuffer(GL_ARRAY_BUFFER, b->gl);
            glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizei>(padded), nullptr,
                         desc.usage == Usage::dynamic ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
            impl_.placed_buffer(*b, padded);
            write_gl_only(*b, initial, initial ? desc.bytes : padded, 0);
            if (initial && padded > desc.bytes) write_gl_only(*b, nullptr, padded - desc.bytes, desc.bytes);
            return b;
        }
        b->cpu.assign(padded, 0);
        if (initial) std::memcpy(b->cpu.data(), initial, desc.bytes);
        if (!(desc.bind & bind_constants)) {
            glGenBuffers(1, &b->gl);
            glBindBuffer(GL_ARRAY_BUFFER, b->gl);
            glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizei>(b->cpu.size()), b->cpu.data(),
                         desc.usage == Usage::immutable ? GL_STATIC_DRAW : GL_DYNAMIC_DRAW);
            impl_.placed(b->pool, b->placed, nullptr, b->cpu.size(), "a buffer");
        }
        return b;
    }
    std::shared_ptr<Texture> create_texture(const TextureDesc& desc, const Level* levels, const TextureLabels&) override {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        trace("create_texture %ux%u format %u, %u mips, %u layers, bind %u", desc.width, desc.height,
              unsigned(desc.format), unsigned(desc.mips), unsigned(desc.layers), unsigned(desc.bind));
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
        trace("prepare program %u (vertex %u, fragment %u)", unsigned(program), g.vertex, g.fragment);
        impl_.shader(true, g.vertex);
        impl_.shader(false, g.fragment);
        if (g.fragment_alt != ~0U) impl_.shader(false, g.fragment_alt);
    }
    void submit(CommandList& list) override {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        trace("submit");
        impl_.submit(list);
    }
    void write(const Buffer& buffer, const void* data, std::size_t bytes, const char*) override {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        impl_.update_buffer(const_cast<VglBuffer&>(static_cast<const VglBuffer&>(buffer)), data, bytes, 0, false);
    }
    void cpu_mirror(const Buffer& buffer, const void* data) override {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        const_cast<VglBuffer&>(static_cast<const VglBuffer&>(buffer)).mirror = static_cast<const std::uint8_t*>(data);
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
        trace("present (interval %u)", sync_interval);
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

void arm_trace() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (g_trace_switch) g_trace_left = 6000;
}

MemoryStats memory_stats() {
    if (!g_impl) return {};
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    return g_impl->snapshot();
}

std::string placement_line() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    const Placement& p = g_placement;
    std::string line = "GPU objects KB (live/peak):";
    for (unsigned i = 0; i < pool_count; ++i)
        if (p.peak[i]) line += " " + std::string(pool_names[i]) + " " + std::to_string(p.live[i] >> 10) + "/" +
                               std::to_string(p.peak[i] >> 10) + ",";
    line += " depth buffers ~" + std::to_string(p.depth_live >> 10) + "/" + std::to_string(p.depth_peak >> 10) +
            "; largest " + std::to_string(p.largest >> 10) + " in " + pool_names[p.largest_pool];
    return line;
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
