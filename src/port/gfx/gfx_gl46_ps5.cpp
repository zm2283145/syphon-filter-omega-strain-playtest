// Port-only (132ps5b / 132ps5c / 132ps5d, research/PS5.md "Renderer"): the PS5 backend of the renderer abstraction
// (gfx_device.hpp, created through gfx_gl46.hpp) on the PS5 homebrew OpenGL core driver (Mesa over AGC, EGL; since
// 132ps5d cybervisi0n/ps5-gl-runtime, linked statically).
//
// Semantics follow the vitaGL backend (gfx_vitagl.cpp: the same Cg programs, constants, blend folding, CPU shadow
// receiver filter and particle haze passes); the GL is plain desktop GL core, 4.6 where the runtime makes a 4.6
// context, else 3.3 with each 4.x call it uses (texture storage, clear texture, copy image, texture sub-image read,
// clip control, debug output) replaced next to it by its 3.3 equivalent (Impl::init lists which are present):
// - The Cg sources of gfx_gxm_shaders.cpp are converted to GLSL at run time (to_glsl below). A program that does
//   not compile or link is logged (with the GL info log and the GLSL) and its draws are skipped; nothing else stops.
// - Every GL call runs on one thread (GlThread) that owns the EGL context: the render thread, the loader and the
//   capture code all call the device, and an EGL context is current to one thread only.
// - Every texture is a GL texture with its array layers stacked vertically (as on the Vita). Row 0 of every texture
//   and target is the D3D top row: the vertex programs negate clip y, so gl_FragCoord.y and viewports are D3D's and
//   nothing read back needs a flip. The present draws the back buffer into the (bottom-up) window flipped.
// - Depth is [0, 1] (glClipControl; without it the vertex wrapper remaps z).
#include "gfx_gl46.hpp"
#include "gfx_gxm_shaders.hpp"

#include "ps5_log.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>

#include <pthread.h>
#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <future>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace omega::gfx::gl46 {
namespace {

using gxm::GxmAttribute;
using gxm::GxmLayout;
using gxm::GxmProgram;
using gxm::GxmStage;

void logf(const char* format, ...) __attribute__((format(printf, 1, 2)));
void logf(const char* format, ...) {
    char text[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof text, format, args);
    va_end(args);
    ps5::log("[gl] %s", text);
}
// Long texts (GLSL sources, info logs) line by line (the log's lines are bounded).
void log_lines(const char* prefix, const std::string& text) {
    std::size_t at = 0;
    unsigned line = 1;
    while (at < text.size()) {
        std::size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        ps5::log("[gl] %s %3u| %s", prefix, line++, text.substr(at, std::min<std::size_t>(end - at, 400)).c_str());
        at = end + 1;
    }
}

// ---- the GL thread ----

class GlThread {
public:
    // Starts the thread and runs `init` on it first (its exception, if any, is rethrown here).
    void start(std::function<void()> init) {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 16u << 20);
        std::packaged_task<void()> task(std::move(init));
        auto done = task.get_future();
        post([&task] { task(); });
        if (pthread_create(&thread_, &attr, &GlThread::entry, this) != 0)
            throw std::runtime_error("gfx gl46: pthread_create of the GL thread failed");
        pthread_attr_destroy(&attr);
        started_ = true;
        done.get();
    }
    // True on the GL thread itself (set at the top of its loop, before the first job, so init's own calls run inline).
    static bool on_thread() { return t_on_thread; }
    // Runs f on the GL thread and waits for it (its result or exception); inline when already there.
    template <class F>
    auto call(F&& f) -> decltype(f()) {
        using R = decltype(f());
        if (on_thread()) return f();
        std::packaged_task<R()> task(std::forward<F>(f));
        auto result = task.get_future();
        post([&task] { task(); });
        return result.get();
    }
    // Queued without waiting (deletes): in order with every call before and after it.
    void post(std::function<void()> job) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            jobs_.push_back(std::move(job));
        }
        ready_.notify_one();
    }

private:
    static void* entry(void* self) {
        static_cast<GlThread*>(self)->run();
        return nullptr;
    }
    void run() {
        t_on_thread = true;
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                ready_.wait(lock, [this] { return !jobs_.empty(); });
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
            try {
                job();
            } catch (const std::exception& e) {
                logf("a queued GL job threw: %s", e.what());
            } catch (...) {
                logf("a queued GL job threw");
            }
        }
    }
    static inline thread_local bool t_on_thread = false;
    pthread_t thread_{};
    bool started_ = false;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::function<void()>> jobs_;
};

GlThread g_thread;

// Runs on the GL thread now if called there, else queued (resource destructors).
void on_gl_thread(std::function<void()> job) {
    if (g_thread.on_thread()) {
        try {
            job();
        } catch (...) {
        }
    } else {
        g_thread.post(std::move(job));
    }
}

// ---- Cg (gfx_gxm_shaders.cpp) to GLSL (4.60, or 3.30 on a 3.3 context) ----

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

std::string replace_all(std::string s, const std::string& from, const std::string& to) {
    for (std::size_t k = s.find(from); k != std::string::npos; k = s.find(from, k + to.size())) s.replace(k, from.size(), to);
    return s;
}

std::string trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// The element count of `uniform float4 name[N]` in a Cg source (1 when not an array, 0 when absent).
unsigned declared_count(const std::string& source, const std::string& name) {
    const std::string head = "uniform float4 " + name;
    for (std::size_t k = source.find(head); k != std::string::npos; k = source.find(head, k + 1)) {
        const std::size_t after = k + head.size();
        if (after < source.size() && is_word(source[after])) continue;
        if (after < source.size() && source[after] == '[')
            return static_cast<unsigned>(std::strtoul(source.c_str() + after + 1, nullptr, 10));
        return 1;
    }
    return 0;
}

struct CgParam {
    bool uniform = false, out = false;
    std::string type, name, array, semantic;
};

CgParam parse_param(std::string text) {
    CgParam p;
    const std::size_t colon = text.find(':');
    if (colon != std::string::npos) {
        p.semantic = trim(text.substr(colon + 1));
        text = text.substr(0, colon);
    }
    std::istringstream in(text);
    std::vector<std::string> words;
    for (std::string w; in >> w;) words.push_back(w);
    std::size_t k = 0;
    if (k < words.size() && words[k] == "uniform") p.uniform = true, ++k;
    else if (k < words.size() && words[k] == "out") p.out = true, ++k;
    else if (k < words.size() && words[k] == "in") ++k;
    if (k + 1 >= words.size()) throw std::runtime_error("unparsed Cg parameter: " + text);
    p.type = words[k];
    std::string name = words[k + 1];
    for (std::size_t j = k + 2; j < words.size(); ++j) name += words[j];  // "name [4]"
    const std::size_t bracket = name.find('[');
    if (bracket != std::string::npos) {
        p.array = name.substr(bracket);
        name = name.substr(0, bracket);
    }
    p.name = name;
    return p;
}

std::vector<std::string> split_top_level(const std::string& s) {
    std::vector<std::string> parts;
    int depth = 0;
    std::size_t start = 0;
    for (std::size_t k = 0; k < s.size(); ++k) {
        const char c = s[k];
        if (c == '(' || c == '[') ++depth;
        else if (c == ')' || c == ']') --depth;
        else if (c == ',' && depth == 0) {
            parts.push_back(trim(s.substr(start, k - start)));
            start = k + 1;
        }
    }
    const std::string last = trim(s.substr(start));
    if (!last.empty()) parts.push_back(last);
    return parts;
}

unsigned texcoord_index(const std::string& semantic) {
    return static_cast<unsigned>(std::strtoul(semantic.c_str() + 8, nullptr, 10));  // "TEXCOORDn"
}

// The GLSL version line, chosen when the context is made (Impl::init): 4.60 on a 4.6 context, else 3.30 (the
// programs use nothing past GLSL 3.30 core; the version only follows the context).
std::string g_glsl_version = "#version 460 core\n";

const char* const glsl_header =
    "#define float2 vec2\n"
    "#define float3 vec3\n"
    "#define float4 vec4\n"
    "#define float4x4 mat4\n"
    "#define tex2D texture\n"
    "#define rsqrt inversesqrt\n"
    "#define lerp mix\n"
    "#define frac fract\n"
    "float saturate(float x) { return clamp(x, 0.0, 1.0); }\n"
    "vec2 saturate(vec2 x) { return clamp(x, 0.0, 1.0); }\n"
    "vec3 saturate(vec3 x) { return clamp(x, 0.0, 1.0); }\n"
    "vec4 saturate(vec4 x) { return clamp(x, 0.0, 1.0); }\n"
    "float fmod(float a, float b) { return a - b * trunc(a / b); }\n";

struct Converted {
    std::string glsl;
    std::vector<std::pair<std::string, int>> samplers;  // fragment: `sampler2D name : TEXUNITk`
};

// One Cg stage as GLSL. Vertex: the attributes become `in`s (bound to locations by name at link), the TEXCOORDn
// outputs `out omega_tcN`, the POSITION output a global written to gl_Position with y negated (row 0 = the D3D top)
// and, without clip control, z remapped from [0, 1] to [-1, 1]. Fragment: the TEXCOORDn inputs `in omega_tcN`, WPOS
// gl_FragCoord, the result `omega_frag`.
Converted to_glsl(const std::string& cg_in, bool vertex, bool clip_control) {
    std::string cg = replace_word(cg_in, "const", "");
    cg = replace_all(cg, "any(vGore > 0.0)", "any(greaterThan(vGore, vec3(0.0)))");
    std::size_t main = cg.find(" main(");
    if (main == std::string::npos) throw std::runtime_error("no main()");
    const std::size_t line = cg.rfind('\n', main) == std::string::npos ? 0 : cg.rfind('\n', main) + 1;
    const std::string prefix = cg.substr(0, line);
    const std::string return_type = trim(cg.substr(line, main - line));
    const std::size_t open = main + 5;
    int depth = 0;
    std::size_t close = open;
    for (; close < cg.size(); ++close) {
        if (cg[close] == '(') ++depth;
        else if (cg[close] == ')' && --depth == 0) break;
    }
    if (close >= cg.size()) throw std::runtime_error("main() without its closing parenthesis");
    const std::size_t body = cg.find('{', close);
    if (body == std::string::npos) throw std::runtime_error("main() without a body");
    const std::vector<std::string> params = split_top_level(cg.substr(open + 1, close - open - 1));

    Converted out;
    std::string decl;
    std::string position;
    for (const std::string& text : params) {
        const CgParam p = parse_param(text);
        if (p.uniform) {
            if (p.type == "sampler2D") {
                decl += "uniform sampler2D " + p.name + ";\n";
                const std::size_t unit = p.semantic.find("TEXUNIT");
                out.samplers.emplace_back(p.name, unit == std::string::npos ? 0 : std::atoi(p.semantic.c_str() + unit + 7));
            } else {
                decl += "uniform " + p.type + " " + p.name + p.array + ";\n";
            }
        } else if (vertex && p.out && p.semantic == "POSITION") {
            decl += p.type + " " + p.name + ";\n";
            position = p.name;
        } else if (p.semantic.rfind("TEXCOORD", 0) == 0) {
            const std::string varying = "omega_tc" + std::to_string(texcoord_index(p.semantic));
            decl += std::string(vertex ? "out " : "in ") + p.type + " " + varying + ";\n#define " + p.name + " " + varying + "\n";
        } else if (!vertex && p.semantic == "WPOS") {
            decl += "#define " + p.name + " gl_FragCoord\n";
        } else if (vertex && !p.out && p.semantic.empty()) {
            decl += "in " + p.type + " " + p.name + ";\n";
        } else {
            throw std::runtime_error("unported Cg parameter: " + text);
        }
    }
    std::string s = g_glsl_version + glsl_header;
    s += prefix;
    s += decl;
    if (vertex) {
        if (position.empty()) throw std::runtime_error("a vertex program without a POSITION output");
        s += "void omega_main() " + cg.substr(body) + "\n";
        s += "void main() {\n    omega_main();\n    gl_Position = vec4(" + position + ".x, -" + position + ".y, " +
             (clip_control ? position + ".z" : "2.0 * " + position + ".z - " + position + ".w") + ", " + position + ".w);\n}\n";
    } else {
        s += "layout(location = 0) out vec4 omega_frag;\n";
        s += return_type + " omega_main() " + cg.substr(body) + "\n";
        s += "void main() { omega_frag = vec4(omega_main()); }\n";
    }
    out.glsl = std::move(s);
    return out;
}

// ---- resources ----

std::size_t align_up(std::size_t v, std::size_t a) { return (v + a - 1) / a * a; }

// Live object counts and bytes (stats_line).
std::atomic<std::uint64_t> g_buffers{0}, g_buffer_bytes{0}, g_textures{0}, g_texture_bytes{0};

struct GlBuffer final : Buffer {
    explicit GlBuffer(const BufferDesc& d) { desc_ = d; }
    ~GlBuffer() override;
    std::vector<std::uint8_t> cpu;  // every buffer's bytes (constants are only here; the shadow filter reads vertices)
    GLuint gl = 0;                  // vertex / index buffers
};

struct GlTexture final : Texture {
    GlTexture() = default;
    ~GlTexture() override;
    GLuint gl = 0;
    unsigned layer_rows = 0;  // rows per layer (layers stacked vertically)
    bool depth = false;       // d24s8: a GL_DEPTH24_STENCIL8 texture
    bool compressed = false;  // bc1 / bc3
    GLenum internal = GL_RGBA8, format = GL_RGBA, type = GL_UNSIGNED_BYTE;
    unsigned bytes_per_texel = 4;
    std::size_t bytes = 0;
    unsigned rows() const { return layer_rows * std::max(desc_.layers, 1u); }
    void set_desc(const TextureDesc& d) { desc_ = d; }
};

struct GlBlend final : BlendState {
    BlendDesc desc;
};
struct GlDepth final : DepthState {
    DepthDesc desc;
};
struct GlRaster final : RasterState {
    RasterDesc desc;
};
struct GlSampler final : SamplerState {
    SamplerDesc desc;
};

const GlBuffer* buffer_of(const void* p) { return static_cast<const GlBuffer*>(static_cast<const Buffer*>(p)); }
const GlTexture* texture_of(const void* p) { return static_cast<const GlTexture*>(static_cast<const Texture*>(p)); }

// ---- programs ----

struct CompiledShader {
    GLuint gl = 0;
    bool ok = false;
    std::string glsl;
    std::vector<std::pair<std::string, int>> samplers;
};

struct GlProgram {
    GLuint gl = 0;
    bool ok = false;
    GLint vertex_constants[2]{-1, -1}, fragment_constants[2]{-1, -1};
    unsigned vertex_count[2]{}, fragment_count[2]{};
    GLint tex_size = -1, layers = -1, blend_mul = -1;
    unsigned skipped = 0;
    // 132ps5d: the values last set (vertex constants 0-1, fragment constants 0-1, tex_size, layers, blend_mul).
    std::vector<float> last[7];
};

// 132ps5d: the GL state execute last set (Impl::cache_). valid = false: unknown, everything is set again.
struct GlCache {
    bool valid = false;
    GLuint program = ~0u;
    GLint viewport[4]{};
    bool viewport_known = false;
    double depth_range[2]{};
    bool depth_range_known = false;
    int polygon = -1, depth_test = -1, depth_mask = -1, colour_mask = -1, blend = -1;
    GLenum depth_func = 0;
    GLenum blend_funcs[6]{};
    bool blend_known = false;
    unsigned active = ~0u;
    GLuint textures[4]{~0u, ~0u, ~0u, ~0u}, samplers[4]{~0u, ~0u, ~0u, ~0u};
    GLuint array_buffer = ~0u, element_buffer = ~0u;
    struct Attribute {
        int enabled = -1;
        GLuint buffer = ~0u;
        GLint size = 0;
        GLsizei stride = 0;
        std::size_t offset = ~std::size_t(0);
    } attributes[8];
};
// ---- pipeline description of one draw (gfx_vitagl.cpp's) ----

struct UniformBlock {
    const void* data = nullptr;
    std::size_t bytes = 0;
};
struct BoundTexture {
    const GlTexture* texture = nullptr;
    SamplerDesc sampler{};
};
struct Pipeline {
    unsigned vertex = 0, fragment = 0;
    const char* constants[2]{};
    const GxmAttribute* attributes = nullptr;
    unsigned attribute_count = 0;
    std::uint32_t stride = 0;
    GLuint vertex_buffer = 0;
    std::size_t vertex_offset = 0;
    bool indexed = false;
    GLuint index_buffer = 0;           // 0: client indices (the shadow filter's kept triangles)
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
};

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

// gfx_vitagl.cpp's blend_for(): the same folding of the D3D blend factor into omegaBlendMul (the Cg programs'
// epilogue), so every backend computes the same colours.
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
            throw std::runtime_error("gfx gl46: unported blend (factor / factor combination other than FIX over)");
        }
    } else if (is_factor(t.dst)) {
        throw std::runtime_error("gfx gl46: unported blend (destination blend factor)");
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
            throw std::runtime_error("gfx gl46: unported blend (alpha blend factor)");
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

// gfx_gxm.cpp's ShadowFilter: shadow_receiver's geometry shader (the guard box and the facing test) on the CPU.
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
        float t[9], n[3];
        std::memcpy(t, v0, 12);
        std::memcpy(t + 3, v1, 12);
        std::memcpy(t + 6, v2, 12);
        if (outside(t) || outside(t + 3) || outside(t + 6)) return false;
        std::memcpy(n, v2 + 12, sizeof n);  // the third vertex's normal (MeshVertex::normal)
        return facing[0] * n[0] + facing[1] * n[1] + facing[2] * n[2] < 0.0F;
    }
};

// The draw state (D3D11-style: it persists across submitted lists until changed or reset).
struct State {
    const GlTexture* colour[2]{};
    unsigned layer[2]{};
    const GlTexture* depth = nullptr;
    Viewport viewport{};
    const GlRaster* raster = nullptr;
    const GlDepth* depth_state = nullptr;
    const GlBlend* blend = nullptr;
    float factor[4]{1, 1, 1, 1};
    Program program{};
    bool has_program = false;
    const GlBuffer* vertices = nullptr;
    std::uint32_t stride = 0, offset = 0;
    const GlBuffer* indices = nullptr;
    const GlBuffer* vertex_constants[4]{};
    const GlBuffer* geometry_constants[4]{};
    const GlBuffer* pixel_constants[4]{};
    const GlTexture* textures[4]{};
    const GlSampler* samplers[4]{};
};

// The program the present draws (back buffer to the window, flipped: the window is bottom-up).
const char* const present_vertex_glsl =
    "out vec2 uv;\n"
    "void main() {\n"
    "    vec2 p = vec2(float(gl_VertexID & 1) * 2.0, float(gl_VertexID >> 1) * 2.0);\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "    uv = vec2(p.x, 1.0 - p.y);\n"
    "}\n";
const char* const present_fragment_glsl =
    "uniform sampler2D image;\n"
    "in vec2 uv;\n"
    "layout(location = 0) out vec4 colour;\n"
    "void main() { colour = vec4(texture(image, uv).rgb, 1.0); }\n";

// 132ps5d: copy_region's draw. The viewport is the destination rectangle; each fragment fetches the source texel at its
// own position plus the source-to-destination offset (exact texels, no filtering; rows as stored on both sides).
const char* const copy_fragment_glsl =
    "uniform sampler2D source;\n"
    "uniform ivec2 offset;\n"
    "layout(location = 0) out vec4 colour;\n"
    "void main() { colour = texelFetch(source, ivec2(gl_FragCoord.xy) + offset, 0); }\n";

void GLAPIENTRY debug_message(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei, const GLchar* message,
                              const void*) {
    static std::atomic<unsigned> count{0};
    const unsigned n = ++count;
    if (n > 200) return;
    logf("debug %u: source 0x%x type 0x%x id %u severity 0x%x: %s%s", n, unsigned(source), unsigned(type), id,
         unsigned(severity), message, n == 200 ? " (no more debug messages are logged)" : "");
}

// 132ps5d hw2: switches for A/B tests on the console, read from ps5_gl.txt in the working folder (/data/omega_strain)
// at start and whenever the file changes (checked every 2 s; FTP can replace it while the game runs). One "name=0/1"
// per line: copy_draw (copy_region as a draw; 0: glCopyImageSubData), barrier (glTextureBarrier around the copy draws),
// state_cache (execute skips unchanged state and uniforms), partial_updates (a partial "whole" buffer update writes
// only its range), overlay_reuse (SceneRenderer keeps unchanged overlay images). All default to 1.
std::atomic<bool> g_copy_draw{true}, g_barrier{true}, g_state_cache{true}, g_partial_updates{true};

// 132ps5d hw2 (hw1: still about 520 textures made per 10 s): textures made per creation label since the last stats line.
std::mutex g_label_mutex;
std::map<std::string, unsigned> g_label_counts;
void count_texture_label(const char* label) {
    std::lock_guard lock(g_label_mutex);
    ++g_label_counts[label ? label : "(none)"];
}
std::string take_texture_labels() {
    std::map<std::string, unsigned> counts;
    {
        std::lock_guard lock(g_label_mutex);
        counts.swap(g_label_counts);
    }
    std::vector<std::pair<unsigned, std::string>> sorted;
    for (auto& [label, n] : counts) sorted.emplace_back(n, label);
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::string text;
    for (std::size_t k = 0; k < sorted.size() && k < 4; ++k)
        text += (k ? ", " : "") + std::to_string(sorted[k].first) + " \"" + sorted[k].second + "\"";
    return text;
}
class Impl;
Impl* g_impl = nullptr;

}  // namespace
}  // namespace omega::gfx::gl46
// scene_renderer.cpp (OMEGA_PS5): whether set_overlay_textures reuses unchanged images (ps5_gl.txt overlay_reuse).
extern "C" int omega_ps5_overlay_reuse;
namespace omega::gfx::gl46 {
namespace {

// Everything below runs on the GL thread (the API objects at the end post their calls there).
class Impl {
public:
    explicit Impl(const Options& o) : options(o) {}
    void init();

    Options options;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    int width = 0, height = 0;
    bool clip_control = false, s3tc = false;
    // 132ps5d: the context's version (major * 10 + minor) and the 4.x features the backend uses, each from the core
    // version or its extension; without one, the 3.3 path next to its use runs (cybervisi0n/ps5-gl-runtime is named
    // "core33" and may give a 3.3 context).
    int gl_version = 0;
    bool has_storage = false, has_clear_texture = false, has_copy_image = false, has_get_sub_image = false;
    bool has_debug = false;
    GLuint copy_fbo_[2]{};
    std::string renderer, version;
    std::shared_ptr<GlTexture> back;
    std::uint64_t draws_total = 0, presents = 0, skipped_draws = 0;
    unsigned draws_frame = 0, draws_last_frame = 0, program_failures = 0;
    // 132ps5c hw10: where the GL thread's time goes (nanoseconds, read by stats_line on another thread).
    std::atomic<std::uint64_t> submit_ns{0}, swap_ns{0}, upload_ns{0}, upload_bytes{0}, timed_presents{0};
    // 132ps5c hw11 (stalls of about 2 s under load in hw10): texture creation, texture updates and read-backs.
    std::atomic<std::uint64_t> texture_create_ns{0}, texture_creates{0}, texture_update_ns{0}, read_ns{0}, reads{0};
    // 132ps5d: copy_region draws (since the last stats line) and out-of-memory retries (in all).
    std::atomic<std::uint64_t> gpu_copies_{0}, oom_retries_{0};

    std::shared_ptr<GlBuffer> make_buffer(const BufferDesc& desc, const void* initial);
    std::shared_ptr<GlTexture> make_texture(const TextureDesc& desc, const Level* levels);
    void forget(const GlTexture* t);  // a texture is being destroyed: its framebuffers and state go
    void prepare(Program program);
    void submit(CommandList& list);
    void update_buffer(GlBuffer& b, const void* data, std::size_t bytes, std::size_t offset, bool whole);
    void read(const GlTexture& t, unsigned sub, std::vector<std::uint8_t>& out, unsigned& pitch);
    void present(unsigned sync_interval);
    void finish() { glFinish(); }
    // 132ps5d: GL state was set outside execute, or an object was deleted (its name may come back): cache_ is unknown.
    void invalidate_state() { cache_.valid = false; }
    GlCache cache_;

private:
    const CompiledShader& shader(bool vertex, unsigned index);
    GlProgram& program(unsigned vertex, unsigned fragment, const GxmAttribute* attributes, unsigned count);
    GLuint framebuffer(const GlTexture* colour, const GlTexture* depth);
    void bind_target(const GlTexture* colour, const GlTexture* depth);
    void draw(bool indexed, std::uint32_t count, std::uint32_t first);
    void execute(Pipeline& p);
    void clear_colour(const GlTexture* target, unsigned layer, const float rgba[4], const Rect* rects, unsigned count);
    void clear_depth(const GlTexture* depth, std::uint8_t flags, float value, std::uint8_t stencil);
    void copy_region(const GlTexture* dst, unsigned dst_sub, unsigned x, unsigned y, const GlTexture* src,
                     unsigned src_sub, const Box* box);
    void update_texture(GlTexture& t, const Box& box, const std::uint8_t* data, unsigned pitch);
    void check_errors(const char* where);
    bool out_of_memory(const char* where);
    void poll_options();
    double options_checked_ = -10.0;
    long long options_mtime_ = -1;

    std::map<std::uint32_t, CompiledShader> shaders_;
    std::map<std::tuple<unsigned, unsigned, const GxmAttribute*>, GlProgram> programs_;
    std::map<std::pair<const GlTexture*, const GlTexture*>, GLuint> framebuffers_;
    GLuint bound_fbo_ = ~0u;
    State state_;
    GLuint vao_ = 0, stream_indices_ = 0;
    GLuint samplers_[3][2]{};
    GLuint present_program_ = 0;
    GLint present_image_ = -1;
    // 132ps5d: copy_region as a draw (texelFetch of the source), see copy_region.
    GLuint copy_program_ = 0;
    GLint copy_source_ = -1, copy_offset_ = -1;
    // 132ps5d (hw11 stalls): commands slower than 50 ms are logged with what they did.
    unsigned slow_logged_ = 0;
    int swap_interval_ = -1;
    std::shared_ptr<GlTexture> dummy_;
    std::vector<std::uint32_t> kept_;
    unsigned errors_logged_ = 0, draws_logged_ = 0;
};

GlBuffer::~GlBuffer() {
    --g_buffers;
    g_buffer_bytes -= cpu.size();
    if (!gl) return;
    const GLuint name = gl;
    on_gl_thread([name] {
        if (g_impl) g_impl->invalidate_state();  // 132ps5d: the name may come back for another buffer
        glDeleteBuffers(1, &name);
    });
}

GlTexture::~GlTexture() {
    --g_textures;
    g_texture_bytes -= bytes;
    const GLuint name = gl;
    const GlTexture* self = this;
    on_gl_thread([name, self] {
        if (g_impl) g_impl->invalidate_state();  // 132ps5d: the name may come back for another texture
        if (g_impl) g_impl->forget(self);
        if (name) glDeleteTextures(1, &name);
    });
}

GLuint compile_glsl(GLenum kind, const std::string& source, const char* name, bool& ok) {
    logf("compile %s (%s, %zu bytes): glCreateShader", name, kind == GL_VERTEX_SHADER ? "vertex" : "fragment", source.size());
    ps5::log_flush();
    const GLuint s = glCreateShader(kind);
    const GLchar* text = source.c_str();
    const GLint length = static_cast<GLint>(source.size());
    glShaderSource(s, 1, &text, &length);
    logf("compile %s: glCompileShader (shader %u)", name, s);
    ps5::log_flush();
    glCompileShader(s);
    logf("compile %s: returned", name);
    GLint status = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &status);
    ok = status != 0;
    if (!ok) {
        GLint size = 0;
        glGetShaderiv(s, GL_INFO_LOG_LENGTH, &size);
        std::string info(std::size_t(std::max(size, 1)), '\0');
        glGetShaderInfoLog(s, size, nullptr, info.data());
        logf("GLSL compile of %s FAILED:", name);
        log_lines("info", info);
        log_lines(name, source);
    }
    return s;
}

void Impl::init() {
    const int made = mkdir(options.shader_cache.c_str(), 0777);
    const int made_errno = errno;
    setenv("PS5_SHADER_CACHE_DIR", options.shader_cache.c_str(), 1);
    struct stat cache_stat{};
    logf("EGL init on the GL thread: shader cache %s (mkdir %d errno %d, exists %d)", options.shader_cache.c_str(), made,
         made ? made_errno : 0, int(stat(options.shader_cache.c_str(), &cache_stat) == 0));
    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) throw std::runtime_error("gfx gl46: eglGetDisplay failed");
    EGLint major = 0, minor = 0;
    if (!eglInitialize(display, &major, &minor))
        throw std::runtime_error("gfx gl46: eglInitialize failed (0x" + std::to_string(eglGetError()) + ")");
    logf("EGL %d.%d initialised", major, minor);
    if (!eglBindAPI(EGL_OPENGL_API)) throw std::runtime_error("gfx gl46: eglBindAPI(EGL_OPENGL_API) failed");
    static const EGLint config_attributes[]{EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                                            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                            EGL_NONE};
    EGLConfig config = nullptr;
    EGLint configs = 0;
    if (!eglChooseConfig(display, config_attributes, &config, 1, &configs) || configs != 1)
        throw std::runtime_error("gfx gl46: eglChooseConfig found no RGBA8 window config");
    surface = eglCreateWindowSurface(display, config, (EGLNativeWindowType)0, nullptr);
    if (surface == EGL_NO_SURFACE) throw std::runtime_error("gfx gl46: eglCreateWindowSurface failed");
    // The newest core context the runtime makes: 4.6 (the former ps5-opengl SDK), else 4.5 / 4.3, else 3.3 (the core
    // profile ps5-app-sdk's sample asks for).
    static const EGLint versions[][2]{{4, 6}, {4, 5}, {4, 3}, {3, 3}};
    for (const auto& v : versions) {
        const EGLint context_attributes[]{EGL_CONTEXT_MAJOR_VERSION_KHR, v[0], EGL_CONTEXT_MINOR_VERSION_KHR, v[1],
                                          EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR,
                                          EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR, EGL_NONE};
        context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
        logf("eglCreateContext %d.%d core: %s", int(v[0]), int(v[1]),
             context == EGL_NO_CONTEXT ? ("failed (0x" + std::to_string(eglGetError()) + ")").c_str() : "made");
        if (context != EGL_NO_CONTEXT) break;
    }
    if (context == EGL_NO_CONTEXT) throw std::runtime_error("gfx gl46: eglCreateContext (core 4.6 to 3.3) failed");
    if (!eglMakeCurrent(display, surface, surface, context)) throw std::runtime_error("gfx gl46: eglMakeCurrent failed");
    eglQuerySurface(display, surface, EGL_WIDTH, &width);
    eglQuerySurface(display, surface, EGL_HEIGHT, &height);
    if (width <= 0 || height <= 0) width = 1920, height = 1080;
    eglSwapInterval(display, 1);
    swap_interval_ = 1;
    renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    logf("context: %s / %s, window %dx%d", version.c_str(), renderer.c_str(), width, height);
    ps5::log_flush();

    while (glGetError() != GL_NO_ERROR) {
    }
    GLint gl_major = 0, gl_minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &gl_major);
    glGetIntegerv(GL_MINOR_VERSION, &gl_minor);
    gl_version = int(gl_major) * 10 + int(gl_minor);
    GLint extensions = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &extensions);
    bool clip_control_ext = false;
    for (GLint k = 0; k < extensions; ++k) {
        const char* e = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, GLuint(k)));
        if (!e) continue;
        const std::string name = e;
        if (name == "GL_EXT_texture_compression_s3tc") s3tc = true;
        else if (name == "GL_ARB_texture_storage") has_storage = true;
        else if (name == "GL_ARB_clear_texture") has_clear_texture = true;
        else if (name == "GL_ARB_copy_image") has_copy_image = true;
        else if (name == "GL_ARB_get_texture_sub_image") has_get_sub_image = true;
        else if (name == "GL_KHR_debug") has_debug = true;
        else if (name == "GL_ARB_clip_control") clip_control_ext = true;
    }
    has_storage = has_storage || gl_version >= 42;
    has_clear_texture = has_clear_texture || gl_version >= 44;
    has_copy_image = has_copy_image || gl_version >= 43;
    has_get_sub_image = has_get_sub_image || gl_version >= 45;
    has_debug = has_debug || gl_version >= 43;
    g_glsl_version = gl_version >= 46 ? "#version 460 core\n" : "#version 330 core\n";
    if (gl_version >= 45 || clip_control_ext) {
        glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
        clip_control = glGetError() == GL_NO_ERROR;
    }
    logf("GL %d.%d core, GLSL %s; glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE): %s", int(gl_major), int(gl_minor),
         gl_version >= 46 ? "4.60" : "3.30", clip_control ? "on" : "absent or FAILED (z remapped in the vertex programs)");
    logf("%d extensions; S3TC %s; texture storage %d, clear texture %d, copy image %d, texture sub-image read %d, "
         "debug output %d (0: the GL 3.3 path)", int(extensions),
         s3tc ? "present (block textures on)" : "absent (block textures decoded)", int(has_storage),
         int(has_clear_texture), int(has_copy_image), int(has_get_sub_image), int(has_debug));
    glGenFramebuffers(2, copy_fbo_);
    if (options.debug_output && has_debug) {
        glEnable(GL_DEBUG_OUTPUT);
        glDebugMessageCallback(debug_message, nullptr);
        logf("GL debug output on (the first 200 messages are logged)");
    }

    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);
    glGenBuffers(1, &stream_indices_);
    for (unsigned f = 0; f < 3; ++f)
        for (unsigned a = 0; a < 2; ++a) {
            GLuint& s = samplers_[f][a];
            glGenSamplers(1, &s);
            glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, f != unsigned(Filter::linear) ? GL_NEAREST : GL_LINEAR);
            glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER, f == unsigned(Filter::point) ? GL_NEAREST : GL_LINEAR);
            const GLint wrap = a ? GL_REPEAT : GL_CLAMP_TO_EDGE;
            glSamplerParameteri(s, GL_TEXTURE_WRAP_S, wrap);
            glSamplerParameteri(s, GL_TEXTURE_WRAP_T, wrap);
        }
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);

    {
        // 132ps5c hw4 diagnostic: the smallest program the driver can compile and link, before any of ours.
        bool a = false, b = false;
        const GLuint tv = compile_glsl(GL_VERTEX_SHADER,
                                       g_glsl_version + "void main() { gl_Position = vec4(0.0, 0.0, 0.0, 1.0); }\n",
                                       "trivial_vs", a);
        const GLuint tf = compile_glsl(GL_FRAGMENT_SHADER,
                                       g_glsl_version + "layout(location = 0) out vec4 c;\nvoid main() { c = vec4(1.0); }\n",
                                       "trivial_fs", b);
        const GLuint tp = glCreateProgram();
        glAttachShader(tp, tv);
        glAttachShader(tp, tf);
        logf("trivial program: glLinkProgram");
        ps5::log_flush();
        glLinkProgram(tp);
        GLint linked = 0;
        glGetProgramiv(tp, GL_LINK_STATUS, &linked);
        logf("trivial program: compiled %d/%d, linked %d", int(a), int(b), int(linked));
    }
    bool vs_ok = false, fs_ok = false;
    const GLuint vs = compile_glsl(GL_VERTEX_SHADER, g_glsl_version + present_vertex_glsl, "present_vs", vs_ok);
    const GLuint fs = compile_glsl(GL_FRAGMENT_SHADER, g_glsl_version + present_fragment_glsl, "present_fs", fs_ok);
    present_program_ = glCreateProgram();
    glAttachShader(present_program_, vs);
    glAttachShader(present_program_, fs);
    glLinkProgram(present_program_);
    GLint linked = 0;
    glGetProgramiv(present_program_, GL_LINK_STATUS, &linked);
    if (!vs_ok || !fs_ok || !linked) throw std::runtime_error("gfx gl46: the present program did not build");
    present_image_ = glGetUniformLocation(present_program_, "image");
    glUseProgram(present_program_);
    if (present_image_ >= 0) glUniform1i(present_image_, 0);
    {
        bool cv = false, cf = false;
        const GLuint v = compile_glsl(GL_VERTEX_SHADER, g_glsl_version + present_vertex_glsl, "copy_vs", cv);
        const GLuint fr = compile_glsl(GL_FRAGMENT_SHADER, g_glsl_version + copy_fragment_glsl, "copy_fs", cf);
        copy_program_ = glCreateProgram();
        glAttachShader(copy_program_, v);
        glAttachShader(copy_program_, fr);
        glLinkProgram(copy_program_);
        GLint copy_linked = 0;
        glGetProgramiv(copy_program_, GL_LINK_STATUS, &copy_linked);
        if (!cv || !cf || !copy_linked) throw std::runtime_error("gfx gl46: the copy program did not build");
        copy_source_ = glGetUniformLocation(copy_program_, "source");
        copy_offset_ = glGetUniformLocation(copy_program_, "offset");
        glUseProgram(copy_program_);
        if (copy_source_ >= 0) glUniform1i(copy_source_, 0);
        glUseProgram(present_program_);
    }
    TextureDesc dummy;
    dummy.width = dummy.height = 1;
    dummy.bind = bind_sampled;
    const std::uint32_t white = 0xffffffffu;
    const Level level{&white, 4};
    dummy_ = make_texture(dummy, &level);
    TextureDesc back_desc;
    back_desc.width = unsigned(width);
    back_desc.height = unsigned(height);
    back_desc.format = Format::bgra8;  // what the D3D swap chain reports
    back_desc.bind = gfx::bind_target | gfx::bind_sampled;
    back = make_texture(back_desc, nullptr);
    check_errors("init");
    logf("initialised: back buffer %dx%d (bgra8 texture %u), present program %u", width, height, back->gl, present_program_);
}

void Impl::check_errors(const char* where) {
    for (GLenum e = glGetError(); e != GL_NO_ERROR; e = glGetError())
        if (errors_logged_ < 64) {
            ++errors_logged_;
            logf("glGetError 0x%04x after %s%s", unsigned(e), where, errors_logged_ == 64 ? " (no more are logged)" : "");
        }
}

// ---- programs ----

// A Cg stage as GLSL, compiled; a failure is logged with the GLSL (the program's draws are then skipped).
const CompiledShader& Impl::shader(bool vertex, unsigned index) {
    const std::uint32_t key = (vertex ? 0x10000u : 0u) | index;
    auto found = shaders_.find(key);
    if (found != shaders_.end()) return found->second;
    const GxmStage& s = vertex ? gxm::gxm_vertex_stage(index) : gxm::gxm_fragment_stage(index);
    CompiledShader c;
    try {
        // The fragment programs' constant arrays are named apart from the vertex programs' (one GL program
        // namespace; gfx_vitagl.cpp's fragment_source).
        const std::string cg =
            vertex ? std::string(s.source) : replace_word(replace_word(s.source, "Scene", "SceneF"), "Shadow", "ShadowF");
        Converted converted = to_glsl(cg, vertex, clip_control);
        c.glsl = std::move(converted.glsl);
        c.samplers = std::move(converted.samplers);
        c.gl = compile_glsl(vertex ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER, c.glsl, s.name, c.ok);
    } catch (const std::exception& e) {
        logf("Cg to GLSL conversion of %s FAILED: %s", s.name, e.what());
        c.ok = false;
    }
    if (c.ok) logf("compiled %s", s.name);
    else ++program_failures;
    return shaders_.emplace(key, std::move(c)).first->second;
}

GlProgram& Impl::program(unsigned vertex, unsigned fragment, const GxmAttribute* attributes, unsigned count) {
    const auto key = std::make_tuple(vertex, fragment, attributes);
    auto found = programs_.find(key);
    if (found != programs_.end()) return found->second;
    GlProgram g;
    const CompiledShader& vs = shader(true, vertex);
    const CompiledShader& fs = shader(false, fragment);
    if (vs.ok && fs.ok) {
        g.gl = glCreateProgram();
        glAttachShader(g.gl, vs.gl);
        glAttachShader(g.gl, fs.gl);
        for (unsigned k = 0; k < count && k < 8; ++k) glBindAttribLocation(g.gl, k, attributes[k].name);
        glLinkProgram(g.gl);
        GLint linked = 0;
        glGetProgramiv(g.gl, GL_LINK_STATUS, &linked);
        g.ok = linked != 0;
        if (!g.ok) {
            GLint size = 0;
            glGetProgramiv(g.gl, GL_INFO_LOG_LENGTH, &size);
            std::string info(std::size_t(std::max(size, 1)), '\0');
            glGetProgramInfoLog(g.gl, size, nullptr, info.data());
            logf("link of program %u/%u FAILED:", vertex, fragment);
            log_lines("info", info);
            ++program_failures;
        }
    }
    if (g.ok) {
        invalidate_state();  // 132ps5d: the program is made current here
        glUseProgram(g.gl);
        const GxmProgram* owner = nullptr;
        for (std::size_t p = 0; p < gxm::gxm_program_count && !owner; ++p) {
            const GxmProgram& candidate = gxm::gxm_program(static_cast<Program>(p));
            if (candidate.vertex == vertex && (candidate.fragment == fragment || candidate.fragment_alt == fragment))
                owner = &candidate;
        }
        for (int k = 0; k < 2 && owner; ++k) {
            if (!owner->constants[k]) continue;
            const std::string name = owner->constants[k], fname = name + "F";
            if ((g.vertex_count[k] = declared_count(vs.glsl, name)))
                g.vertex_constants[k] = glGetUniformLocation(g.gl, name.c_str());
            if ((g.fragment_count[k] = declared_count(fs.glsl, fname)))
                g.fragment_constants[k] = glGetUniformLocation(g.gl, fname.c_str());
        }
        g.tex_size = glGetUniformLocation(g.gl, "omegaTexSize");
        g.layers = glGetUniformLocation(g.gl, "omegaLayers");
        g.blend_mul = glGetUniformLocation(g.gl, "omegaBlendMul");
        for (const auto& [name, unit] : fs.samplers) {
            const GLint loc = glGetUniformLocation(g.gl, name.c_str());
            if (loc >= 0) glUniform1i(loc, unit);
        }
        logf("program %u/%u linked (GL %u): constants %d/%d %d/%d, texture sizes %d, blend %d", vertex, fragment, g.gl,
             g.vertex_constants[0], g.fragment_constants[0], g.vertex_constants[1], g.fragment_constants[1], g.tex_size,
             g.blend_mul);
    }
    return programs_.emplace(key, g).first->second;
}

// Every stage of the program (and its second fragment program) with its real vertex layout, so every GLSL error shows
// at the start.
void Impl::prepare(Program p) {
    const GxmProgram& g = gxm::gxm_program(p);
    const GxmLayout& layout = gxm::gxm_layout(program_layout(p));
    program(g.vertex, g.fragment, layout.attributes, layout.count);
    if (g.fragment_alt != ~0U) program(g.vertex, g.fragment_alt, layout.attributes, layout.count);
}

// ---- resources ----

std::shared_ptr<GlBuffer> Impl::make_buffer(const BufferDesc& desc, const void* initial) {
    invalidate_state();  // 132ps5d: GL state set outside execute (cache_)
    auto b = std::make_shared<GlBuffer>(desc);
    b->cpu.assign(align_up(std::max<std::size_t>(desc.bytes, 16), 16), 0);
    if (initial) std::memcpy(b->cpu.data(), initial, desc.bytes);
    ++g_buffers;
    g_buffer_bytes += b->cpu.size();
    if (!(desc.bind & bind_constants)) {
        glGenBuffers(1, &b->gl);
        glBindBuffer(GL_ARRAY_BUFFER, b->gl);
        check_errors("the commands before a buffer creation");
        const GLenum usage = desc.usage == Usage::immutable ? GL_STATIC_DRAW : GL_DYNAMIC_DRAW;
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(b->cpu.size()), b->cpu.data(), usage);
        if (out_of_memory("a buffer creation")) {
            glFinish();
            ++oom_retries_;
            glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(b->cpu.size()), b->cpu.data(), usage);
            const bool failed = out_of_memory("a buffer creation, retried after glFinish");
            logf("out of memory making a %zu KB buffer: %s", b->cpu.size() >> 10,
                 failed ? "FAILED again, the buffer is undefined" : "made after glFinish");
        }
    }
    return b;
}

// Adds the scope's time to a counter (nanoseconds).
struct NsTimer {
    std::atomic<std::uint64_t>& total;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~NsTimer() {
        total += std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
    }
};

std::shared_ptr<GlTexture> Impl::make_texture(const TextureDesc& desc, const Level* levels) {
    invalidate_state();  // 132ps5d: GL state set outside execute (cache_)
    NsTimer timer{texture_create_ns};
    ++texture_creates;
    if (desc.samples > 1) throw std::runtime_error("gfx gl46: multisampled textures are unported");
    auto t = std::make_shared<GlTexture>();
    t->set_desc(desc);
    const unsigned layers = std::max(desc.layers, 1u);
    const unsigned mips = std::max(desc.mips, 1u);
    t->layer_rows = std::max(desc.height, 1u);
    const unsigned w = std::max(desc.width, 1u);
    switch (desc.format) {
    case Format::rgba8: t->internal = GL_RGBA8, t->format = GL_RGBA, t->type = GL_UNSIGNED_BYTE, t->bytes_per_texel = 4; break;
    case Format::bgra8: t->internal = GL_RGBA8, t->format = GL_BGRA, t->type = GL_UNSIGNED_BYTE, t->bytes_per_texel = 4; break;
    case Format::r8: t->internal = GL_R8, t->format = GL_RED, t->type = GL_UNSIGNED_BYTE, t->bytes_per_texel = 1; break;
    case Format::r32f: t->internal = GL_R32F, t->format = GL_RED, t->type = GL_FLOAT, t->bytes_per_texel = 4; break;
    case Format::d24s8:
        t->depth = true;
        t->internal = GL_DEPTH24_STENCIL8, t->format = GL_DEPTH_STENCIL, t->type = GL_UNSIGNED_INT_24_8;
        t->bytes_per_texel = 4;
        break;
    case Format::bc1: case Format::bc3: {
        // 132md11 mod textures (Capabilities::block_textures, only with S3TC): level 0 only, as every texture here.
        if (!s3tc) throw std::runtime_error("gfx gl46: a block-compressed texture without S3TC");
        if (layers != 1 || !levels || !levels[0].data || desc.width % 4 || desc.height % 4)
            throw std::runtime_error("gfx gl46: a block-compressed texture needs one layer of 4 x 4 blocks with data");
        t->compressed = true;
        t->internal = desc.format == Format::bc1 ? GL_COMPRESSED_RGBA_S3TC_DXT1_EXT : GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
        glGenTextures(1, &t->gl);
        glBindTexture(GL_TEXTURE_2D, t->gl);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        const GLsizei bytes = GLsizei(std::size_t(levels[0].row_pitch) * (desc.height / 4));
        glCompressedTexImage2D(GL_TEXTURE_2D, 0, t->internal, GLsizei(desc.width), GLsizei(desc.height), 0, bytes,
                               levels[0].data);
        t->bytes = std::size_t(bytes);
        ++g_textures;
        g_texture_bytes += t->bytes;
        return t;
    }
    default: throw std::runtime_error("gfx gl46: unknown texture format");
    }
    if (t->depth && layers != 1) throw std::runtime_error("gfx gl46: depth texture arrays are unported");
    GLint limit = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &limit);
    if (int(w) > limit || int(t->rows()) > limit)
        throw std::runtime_error("gfx gl46: a texture (with its stacked layers) above GL_MAX_TEXTURE_SIZE is unported");
    t->bytes = std::size_t(w) * t->rows() * t->bytes_per_texel;
    // Zeroed, as a new D3D texture: glClearTexImage (4.4), else the storage is made from zero bytes (GL 3.3 path).
    const bool zero_upload = !t->depth && !has_clear_texture;
    std::vector<std::uint8_t> zeros(zero_upload ? t->bytes : 0);
    const auto fill = [&] {
        glGenTextures(1, &t->gl);
        glBindTexture(GL_TEXTURE_2D, t->gl);
        if (has_storage && !zero_upload) {
            glTexStorage2D(GL_TEXTURE_2D, 1, t->internal, GLsizei(w), GLsizei(t->rows()));
        } else {
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            glTexImage2D(GL_TEXTURE_2D, 0, GLint(t->internal), GLsizei(w), GLsizei(t->rows()), 0, t->format, t->type,
                         zero_upload ? zeros.data() : nullptr);
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        if (t->depth) return;
        if (!zero_upload) glClearTexImage(t->gl, 0, t->format, t->type, nullptr);
        if (levels)
            for (unsigned l = 0; l < layers; ++l) {
                const Level& level = levels[l * mips];
                if (!level.data) continue;
                glPixelStorei(GL_UNPACK_ROW_LENGTH, GLint(level.row_pitch / t->bytes_per_texel));
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, GLint(l * t->layer_rows), GLsizei(w), GLsizei(t->layer_rows),
                                t->format, t->type, level.data);
            }
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    };
    check_errors("the commands before a texture creation");
    fill();
    if (out_of_memory("a texture creation")) {
        // 132ps5d (hw11: GL_OUT_OF_MEMORY, then characters without textures): let the GPU retire what it still holds
        // (the driver frees deleted objects after their last use), then make the texture again, once.
        glDeleteTextures(1, &t->gl);
        glFinish();
        ++oom_retries_;
        fill();
        const bool failed = out_of_memory("a texture creation, retried after glFinish");
        logf("out of memory making a %ux%u texture (format %u, %zu KB; %llu textures, %llu KB live): %s", w, t->rows(),
             unsigned(desc.format), t->bytes >> 10, static_cast<unsigned long long>(g_textures.load()),
             static_cast<unsigned long long>(g_texture_bytes.load() >> 10),
             failed ? "FAILED again, the texture is undefined" : "made after glFinish");
    }
    ++g_textures;
    g_texture_bytes += t->bytes;
    return t;
}

// 132ps5d hw2: re-reads ps5_gl.txt (the A/B switches above) when its modification time changes, every 2 s at most.
void Impl::poll_options() {
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now - options_checked_ < 2.0) return;
    options_checked_ = now;
    struct stat st{};
    const long long mtime = stat("ps5_gl.txt", &st) == 0 ? (long long)st.st_mtime : -2;
    if (mtime == options_mtime_) return;
    options_mtime_ = mtime;
    bool copy_draw = true, barrier = true, state_cache = true, partial = true, reuse = true;
    if (FILE* f = std::fopen("ps5_gl.txt", "r")) {
        char line[128];
        while (std::fgets(line, sizeof line, f)) {
            char name[64]{};
            int value = 1;
            if (std::sscanf(line, " %63[a-z_] = %d", name, &value) != 2) continue;
            const std::string n = name;
            if (n == "copy_draw") copy_draw = value != 0;
            else if (n == "barrier") barrier = value != 0;
            else if (n == "state_cache") state_cache = value != 0;
            else if (n == "partial_updates") partial = value != 0;
            else if (n == "overlay_reuse") reuse = value != 0;
        }
        std::fclose(f);
    }
    g_copy_draw = copy_draw;
    g_barrier = barrier;
    g_state_cache = state_cache;
    g_partial_updates = partial;
    omega_ps5_overlay_reuse = reuse ? 1 : 0;
    invalidate_state();
    logf("ps5_gl.txt %s: copy_draw %d, barrier %d, state_cache %d, partial_updates %d, overlay_reuse %d",
         mtime >= 0 ? "read" : "absent (defaults)", int(copy_draw), int(barrier), int(state_cache), int(partial),
         int(reuse));
}

// 132ps5d: true when the GL calls since the last check ran out of memory (other errors are logged as check_errors does).
bool Impl::out_of_memory(const char* where) {
    bool oom = false;
    for (GLenum e = glGetError(); e != GL_NO_ERROR; e = glGetError()) {
        if (e == GL_OUT_OF_MEMORY) oom = true;
        else if (errors_logged_ < 64) {
            ++errors_logged_;
            logf("glGetError 0x%04x after %s", unsigned(e), where);
        }
    }
    return oom;
}

void Impl::forget(const GlTexture* t) {
    for (auto it = framebuffers_.begin(); it != framebuffers_.end();) {
        if (it->first.first == t || it->first.second == t) {
            if (bound_fbo_ == it->second) {
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                bound_fbo_ = 0;
            }
            glDeleteFramebuffers(1, &it->second);
            it = framebuffers_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto& c : state_.colour)
        if (c == t) c = nullptr;
    if (state_.depth == t) state_.depth = nullptr;
    for (auto& x : state_.textures)
        if (x == t) x = nullptr;
}

// ---- targets ----

// The framebuffer object of a (colour, depth) pair (either may be null, not both), made at its first use.
GLuint Impl::framebuffer(const GlTexture* colour, const GlTexture* depth) {
    const auto key = std::make_pair(colour, depth);
    auto found = framebuffers_.find(key);
    if (found != framebuffers_.end()) return found->second;
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    bound_fbo_ = fbo;
    if (colour) {
        if (colour->depth || colour->compressed) throw std::runtime_error("gfx gl46: a colour target of a depth or block format");
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colour->gl, 0);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
    } else {
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
    }
    if (depth) {
        if (!depth->depth) throw std::runtime_error("gfx gl46: a depth target that is not d24s8");
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, depth->gl, 0);
    }
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    logf("framebuffer %u: colour %s%ux%u%s, status 0x%04x%s", fbo, colour ? "" : "none ", colour ? colour->desc().width : 0,
         colour ? colour->rows() : 0, depth ? (" + depth " + std::to_string(depth->desc().width) + "x" +
                                               std::to_string(depth->rows())).c_str() : "",
         unsigned(status), status == GL_FRAMEBUFFER_COMPLETE ? "" : " (INCOMPLETE)");
    if (colour && depth && depth->rows() < colour->rows())
        logf("framebuffer %u: the depth texture has fewer rows than the stacked colour layers (draws below row %u are "
             "clipped)", fbo, depth->rows());
    return framebuffers_.emplace(key, fbo).first->second;
}

void Impl::bind_target(const GlTexture* colour, const GlTexture* depth) {
    if (!colour && !depth) throw std::runtime_error("gfx gl46: a draw with no colour or depth target");
    const GLuint fbo = framebuffer(colour, depth);
    if (fbo != bound_fbo_) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        bound_fbo_ = fbo;
    }
}

Viewport in_layer(Viewport v, const GlTexture* target, unsigned layer) {
    if (target) v.y += static_cast<float>(layer * target->layer_rows);
    return v;
}

// ---- draws ----

void Impl::draw(bool indexed, std::uint32_t count, std::uint32_t first) {
    if (!state_.has_program) throw std::runtime_error("gfx gl46: a draw with no program");
    if (!count) return;
    const GxmProgram& g = gxm::gxm_program(state_.program);
    const GxmLayout& layout = gxm::gxm_layout(program_layout(state_.program));
    if (!state_.vertices || !state_.vertices->gl) throw std::runtime_error("gfx gl46: a draw with no vertex buffer");
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
        if (!state_.indices || !state_.indices->gl) throw std::runtime_error("gfx gl46: an indexed draw with no index buffer");
        if ((std::size_t(first) + count) * 4 > state_.indices->desc().bytes)
            throw std::runtime_error("gfx gl46: an indexed draw past its index buffer");
        p.index_buffer = state_.indices->gl;
    }
    if (g.cpu_geometry) {
        // shadow_receiver's geometry stage on the CPU: the kept triangles' indices are drawn instead.
        const GlBuffer* shadow = state_.geometry_constants[1] ? state_.geometry_constants[1] : state_.vertex_constants[1];
        if (!shadow || shadow->cpu.size() < 112) throw std::runtime_error("gfx gl46: shadow_receiver without Shadow constants");
        ShadowFilter filter;
        std::memcpy(filter.clip, shadow->cpu.data(), sizeof filter.clip);
        std::memcpy(filter.facing, shadow->cpu.data() + 96, sizeof filter.facing);
        kept_.clear();
        const std::uint8_t* vertices = state_.vertices->cpu.data() + state_.offset;
        const std::size_t vertex_bytes = state_.vertices->cpu.size() - std::min<std::size_t>(state_.offset, state_.vertices->cpu.size());
        const auto* source = indexed ? reinterpret_cast<const std::uint32_t*>(state_.indices->cpu.data()) + first : nullptr;
        for (std::uint32_t k = 0; k + 2 < count; k += 3) {
            const std::uint32_t i0 = source ? source[k] : first + k, i1 = source ? source[k + 1] : first + k + 1,
                                i2 = source ? source[k + 2] : first + k + 2;
            const std::size_t last = std::size_t(std::max({i0, i1, i2})) * p.stride + 24;
            if (last > vertex_bytes) throw std::runtime_error("gfx gl46: a shadow receiver vertex past its buffer");
            if (filter.keep(vertices + std::size_t(i0) * p.stride, vertices + std::size_t(i1) * p.stride,
                            vertices + std::size_t(i2) * p.stride)) {
                kept_.push_back(i0);
                kept_.push_back(i1);
                kept_.push_back(i2);
            }
        }
        if (kept_.empty()) return;
        p.indexed = true;
        p.index_buffer = 0;
        p.client_indices = kept_.data();
        p.first = 0;
        p.count = static_cast<std::uint32_t>(kept_.size());
    }
    const DepthDesc depth = state_.depth_state ? state_.depth_state->desc : DepthDesc{true, true, Compare::less};
    p.depth_test = depth.test && state_.depth;
    p.depth_func = gl_compare(depth.compare);
    p.depth_write = depth.test && depth.write && state_.depth;
    p.wireframe = state_.raster && state_.raster->desc.wireframe;
    for (unsigned k = 0; k < 4; ++k) {
        p.textures[k].texture = state_.textures[k];
        const GlSampler* s = state_.samplers[g.samplers[k]];
        p.textures[k].sampler = s ? s->desc : SamplerDesc{};
    }
    for (int k = 0; k < 2; ++k) {
        if (const GlBuffer* b = state_.vertex_constants[k]) p.vertex_constants[k] = {b->cpu.data(), b->cpu.size()};
        if (const GlBuffer* b = state_.pixel_constants[k]) p.fragment_constants[k] = {b->cpu.data(), b->cpu.size()};
    }
    const BlendDesc blend = state_.blend ? state_.blend->desc : BlendDesc{};
    // particle_haze's two targets: SV_Target0 and SV_Target1 are two fragment programs drawn in turn (as on GXM).
    const bool second = g.fragment_alt != ~0U && state_.colour[1];
    for (unsigned pass = 0; pass < (second ? 2u : 1u); ++pass) {
        if (!state_.colour[pass]) {
            if (second) continue;  // a two-target draw without its first target draws only the second
            if (!state_.depth) return;
        }
        bind_target(state_.colour[pass], state_.depth);
        p.viewport = in_layer(state_.viewport, state_.colour[pass], state_.layer[pass]);
        p.fragment = pass ? g.fragment_alt : g.fragment;
        blend_for(blend.targets[pass && blend.independent ? 1 : 0], state_.factor, p);
        execute(p);
    }
}

void Impl::execute(Pipeline& p) {
    GlProgram& g = program(p.vertex, p.fragment, p.attributes, p.attribute_count);
    if (!g.ok) {
        ++skipped_draws;
        if (g.skipped++ < 3) logf("draw skipped: program %u/%u did not build", p.vertex, p.fragment);
        return;
    }
    // 132ps5d (hw11: 700-1000 draws at 90-130 ms of GL calls a frame): the GL state of the previous draw is kept in
    // cache_ and a draw makes only the calls whose state differs (the PS5 driver revalidates and flushes what a call
    // marks dirty). Code that sets GL state outside execute clears cache_ (invalidate_state).
    GlCache& s = cache_;
    const bool caching = g_state_cache.load(std::memory_order_relaxed);
    if (!s.valid || !caching) s = GlCache{.valid = true};
    if (s.program != g.gl) {
        glUseProgram(g.gl);
        s.program = g.gl;
    }
    const Viewport& v = p.viewport;
    const GLint vp[4]{GLint(std::lround(v.x)), GLint(std::lround(v.y)), GLint(std::lround(v.width)), GLint(std::lround(v.height))};
    if (!s.viewport_known || std::memcmp(vp, s.viewport, sizeof vp) != 0) {
        glViewport(vp[0], vp[1], vp[2], vp[3]);
        std::memcpy(s.viewport, vp, sizeof vp);
        s.viewport_known = true;
    }
    if (!s.depth_range_known || s.depth_range[0] != v.min_depth || s.depth_range[1] != v.max_depth) {
        glDepthRange(v.min_depth, v.max_depth);  // GL 1.0 (glDepthRangef is 4.1)
        s.depth_range[0] = v.min_depth;
        s.depth_range[1] = v.max_depth;
        s.depth_range_known = true;
    }
    if (s.polygon != int(p.wireframe)) {
        glPolygonMode(GL_FRONT_AND_BACK, p.wireframe ? GL_LINE : GL_FILL);
        s.polygon = int(p.wireframe);
    }
    const bool depth_on = p.depth_test || p.depth_write;
    if (s.depth_test != int(depth_on)) {
        if (depth_on) glEnable(GL_DEPTH_TEST);
        else glDisable(GL_DEPTH_TEST);
        s.depth_test = int(depth_on);
    }
    if (depth_on) {
        const GLenum func = p.depth_test ? p.depth_func : GL_ALWAYS;
        if (s.depth_func != func) {
            glDepthFunc(func);
            s.depth_func = func;
        }
    }
    const int depth_mask = depth_on && p.depth_write ? 1 : 0;
    if (s.depth_mask != depth_mask) {
        glDepthMask(depth_mask ? GL_TRUE : GL_FALSE);
        s.depth_mask = depth_mask;
    }
    if (s.colour_mask != int(p.mask)) {
        glColorMask((p.mask & mask_r) ? GL_TRUE : GL_FALSE, (p.mask & mask_g) ? GL_TRUE : GL_FALSE,
                    (p.mask & mask_b) ? GL_TRUE : GL_FALSE, (p.mask & mask_a) ? GL_TRUE : GL_FALSE);
        s.colour_mask = int(p.mask);
    }
    if (s.blend != int(p.blend)) {
        if (p.blend) glEnable(GL_BLEND);
        else glDisable(GL_BLEND);
        s.blend = int(p.blend);
    }
    if (p.blend) {
        const GLenum funcs[6]{p.colour_src, p.colour_dst, p.alpha_src, p.alpha_dst, p.colour_op, p.alpha_op};
        if (!s.blend_known || std::memcmp(funcs, s.blend_funcs, sizeof funcs) != 0) {
            glBlendFuncSeparate(p.colour_src, p.colour_dst, p.alpha_src, p.alpha_dst);
            glBlendEquationSeparate(p.colour_op, p.alpha_op);
            std::memcpy(s.blend_funcs, funcs, sizeof funcs);
            s.blend_known = true;
        }
    }
    float tex_size[16];
    float layers[4];
    for (unsigned k = 0; k < 4; ++k) {
        const GlTexture* t = p.textures[k].texture ? p.textures[k].texture : dummy_.get();
        if (t->depth) throw std::runtime_error("gfx gl46: sampling a depth texture is unported");
        const SamplerDesc& sd = p.textures[k].sampler;
        const GLuint sampler = samplers_[std::min(unsigned(sd.filter), 2u)][sd.address == Address::wrap ? 1 : 0];
        if (s.textures[k] != t->gl) {
            if (s.active != k) {
                glActiveTexture(GL_TEXTURE0 + k);
                s.active = k;
            }
            glBindTexture(GL_TEXTURE_2D, t->gl);
            s.textures[k] = t->gl;
        }
        if (s.samplers[k] != sampler) {
            glBindSampler(k, sampler);
            s.samplers[k] = sampler;
        }
        const float w = static_cast<float>(std::max(t->desc().width, 1u)), h = static_cast<float>(t->layer_rows);
        tex_size[4 * k] = w;
        tex_size[4 * k + 1] = h;
        tex_size[4 * k + 2] = 1.0F / w;
        tex_size[4 * k + 3] = 1.0F / h;
        layers[k] = static_cast<float>(std::max(t->desc().layers, 1u));
    }
    // Uniform values stay in the program object: a value equal to the program's last one is not set again.
    auto set = [&g, caching](unsigned slot, GLint location, const void* data, std::size_t bytes, unsigned declared) {
        if (location < 0 || !data) return;
        const unsigned n = static_cast<unsigned>(std::min<std::size_t>(bytes / 16, declared));
        if (!n) return;
        std::vector<float>& last = g.last[slot];
        const std::size_t floats = std::size_t(n) * 4;
        if (caching && last.size() == floats && std::memcmp(last.data(), data, floats * sizeof(float)) == 0) return;
        last.assign(static_cast<const float*>(data), static_cast<const float*>(data) + floats);
        glUniform4fv(location, GLsizei(n), static_cast<const GLfloat*>(data));
    };
    for (unsigned k = 0; k < 2; ++k) {
        set(k, g.vertex_constants[k], p.vertex_constants[k].data, p.vertex_constants[k].bytes, g.vertex_count[k]);
        set(2 + k, g.fragment_constants[k], p.fragment_constants[k].data, p.fragment_constants[k].bytes, g.fragment_count[k]);
    }
    set(4, g.tex_size, tex_size, sizeof tex_size, 4);
    set(5, g.layers, layers, sizeof layers, 1);
    set(6, g.blend_mul, p.blend_mul, sizeof p.blend_mul, 1);

    for (unsigned k = 0; k < 8; ++k) {
        GlCache::Attribute& a = s.attributes[k];
        if (k < p.attribute_count) {
            const GLint size = GLint(p.attributes[k].components);
            const std::size_t offset = p.vertex_offset + p.attributes[k].offset;
            if (a.enabled != 1) {
                glEnableVertexAttribArray(k);
                a.enabled = 1;
            }
            if (a.buffer != p.vertex_buffer || a.size != size || a.stride != GLsizei(p.stride) || a.offset != offset) {
                if (s.array_buffer != p.vertex_buffer) {
                    glBindBuffer(GL_ARRAY_BUFFER, p.vertex_buffer);
                    s.array_buffer = p.vertex_buffer;
                }
                glVertexAttribPointer(k, size, GL_FLOAT, GL_FALSE, GLsizei(p.stride), reinterpret_cast<const void*>(offset));
                a.buffer = p.vertex_buffer;
                a.size = size;
                a.stride = GLsizei(p.stride);
                a.offset = offset;
            }
        } else if (a.enabled != 0) {
            glDisableVertexAttribArray(k);
            a.enabled = 0;
        }
    }
    if (p.indexed) {
        if (p.index_buffer) {
            if (s.element_buffer != p.index_buffer) {
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, p.index_buffer);
                s.element_buffer = p.index_buffer;
            }
            glDrawElements(GL_TRIANGLES, GLsizei(p.count), GL_UNSIGNED_INT, reinterpret_cast<const void*>(std::size_t(p.first) * 4));
        } else {
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, stream_indices_);
            s.element_buffer = stream_indices_;
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(std::size_t(p.count) * 4), p.client_indices, GL_STREAM_DRAW);
            glDrawElements(GL_TRIANGLES, GLsizei(p.count), GL_UNSIGNED_INT, nullptr);
        }
    } else {
        glDrawArrays(GL_TRIANGLES, GLint(p.first), GLsizei(p.count));
    }
    ++draws_total;
    ++draws_frame;
    if (draws_logged_ < 16) {
        ++draws_logged_;
        logf("draw %u: program %u/%u count %u viewport %.0f,%.0f %.0fx%.0f fbo %u blend %d depth %d/%d", draws_logged_,
             p.vertex, p.fragment, p.count, v.x, v.y, v.width, v.height, bound_fbo_, int(p.blend), int(p.depth_test),
             int(p.depth_write));
    }
}

// ---- clears, copies, updates, read-back ----

// A colour clear (D3D ClearRenderTargetView / ClearView): glClear inside a scissor of the layer's rows.
void Impl::clear_colour(const GlTexture* target, unsigned layer, const float rgba[4], const Rect* rects, unsigned count) {
    invalidate_state();  // 132ps5d: GL state set outside execute (cache_)
    if (!target) return;
    bind_target(target, nullptr);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(rgba[0], rgba[1], rgba[2], rgba[3]);
    glEnable(GL_SCISSOR_TEST);
    const unsigned n = rects ? count : 1;
    for (unsigned k = 0; k < n; ++k) {
        int left = 0, top = 0, right = int(target->desc().width), bottom = int(target->layer_rows);
        if (rects) {
            left = std::max(rects[k].left, 0);
            top = std::max(rects[k].top, 0);
            right = std::min(rects[k].right, right);
            bottom = std::min(rects[k].bottom, bottom);
        }
        if (right <= left || bottom <= top) continue;
        glScissor(left, top + int(layer * target->layer_rows), right - left, bottom - top);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glDisable(GL_SCISSOR_TEST);
}

void Impl::clear_depth(const GlTexture* depth, std::uint8_t flags, float value, std::uint8_t stencil) {
    invalidate_state();  // 132ps5d: GL state set outside execute (cache_)
    if (!depth) return;
    bind_target(nullptr, depth);
    GLbitfield bits = 0;
    if (flags & CommandList::clear_depth_bit) {
        glDepthMask(GL_TRUE);
        glClearDepth(value);
        bits |= GL_DEPTH_BUFFER_BIT;
    }
    if (flags & CommandList::clear_stencil_bit) {
        glStencilMask(0xff);
        glClearStencil(stencil);
        bits |= GL_STENCIL_BUFFER_BIT;
    }
    if (bits) glClear(bits);
}

// CopyResource / CopySubresourceRegion: glCopyImageSubData between the stacked layers' rows.
void Impl::copy_region(const GlTexture* dst, unsigned dst_sub, unsigned x, unsigned y, const GlTexture* src,
                       unsigned src_sub, const Box* box) {
    invalidate_state();  // 132ps5d: GL state set outside execute (cache_)
    if (!dst || !src) return;
    if (dst->depth || src->depth) throw std::runtime_error("gfx gl46: depth copies are unported");
    const unsigned dm = std::max(dst->desc().mips, 1u), sm = std::max(src->desc().mips, 1u);
    if (dst_sub % dm || src_sub % sm) throw std::runtime_error("gfx gl46: copies of mip levels above 0 are unported");
    const unsigned dl = dst_sub / dm, sl = src_sub / sm;
    const Box b = box ? *box : Box{0, 0, 0, src->desc().width, src->layer_rows, 1};
    const unsigned right = std::min(b.right, src->desc().width), bottom = std::min(b.bottom, src->layer_rows);
    if (right <= b.left || bottom <= b.top) return;
    const unsigned w = std::min(right - b.left, dst->desc().width - std::min(x, dst->desc().width));
    const unsigned h = std::min(bottom - b.top, dst->layer_rows - std::min(y, dst->layer_rows));
    if (!w || !h) return;
    const GLint sx = GLint(b.left), sy = GLint(sl * src->layer_rows + b.top);
    const GLint dx = GLint(x), dy = GLint(dl * dst->layer_rows + y);
    if (src->compressed || dst->compressed || (!g_copy_draw.load(std::memory_order_relaxed) && has_copy_image)) {
        if (!has_copy_image) throw std::runtime_error("gfx gl46: block-texture copies need GL_ARB_copy_image");
        glCopyImageSubData(src->gl, GL_TEXTURE_2D, 0, sx, sy, 0, dst->gl, GL_TEXTURE_2D, 0, dx, dy, 0, GLsizei(w),
                           GLsizei(h), 1);
        return;
    }
    // 132ps5d: a draw into the destination that fetches the source's texels, on the GPU. The PS5 driver runs
    // glCopyImageSubData through CPU maps of both whole textures (132ps5c hw11 gl-stdout.log: the read maps of
    // ps5_screen.c 7059 and the write flushes of 7217, 3.7-10.5 MB each, 5 a present) and waits for the GPU each time;
    // the particle haze copies a box of the scene for every haze sprite. The source is not bound as a target here.
    ++gpu_copies_;
    // 132ps5d hw2 (the sky showed old frames, the pause screen's among them): the source was just rendered and the
    // destination is sampled next; the driver must not serve either from stale GPU caches.
    const bool barrier = g_barrier.load(std::memory_order_relaxed) && gl_version >= 45;
    if (barrier) glTextureBarrier();
    if (src == dst) throw std::runtime_error("gfx gl46: a copy within one texture is unported");
    bind_target(dst, nullptr);
    glViewport(dx, dy, GLsizei(w), GLsizei(h));
    glDepthRange(0.0, 1.0);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(copy_program_);
    if (copy_offset_ >= 0) glUniform2i(copy_offset_, sx - dx, sy - dy);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src->gl);
    glBindSampler(0, samplers_[unsigned(Filter::point)][0]);
    for (unsigned k = 0; k < 8; ++k) glDisableVertexAttribArray(k);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (barrier) glTextureBarrier();
}

void Impl::update_buffer(GlBuffer& b, const void* data, std::size_t bytes, std::size_t offset, bool whole) {
    invalidate_state();  // 132ps5d: GL state set outside execute (cache_)
    const std::size_t size = b.desc().bytes;
    if (offset > size || bytes > size - offset) throw std::runtime_error("gfx gl46: a buffer update past its end");
    if (bytes) std::memcpy(b.cpu.data() + offset, data, bytes);
    if (!b.gl || !bytes) return;
    // 132ps5d: a "whole" update of part of the buffer (SceneRenderer::refresh_vertices' ranges of the 34.5 MB level
    // mesh) writes only that part; new storage (orphaning) is for an update of all of it.
    if (whole && (offset != 0 || bytes < size) && g_partial_updates.load(std::memory_order_relaxed)) whole = false;
    upload_bytes += whole ? b.cpu.size() : bytes;
    const auto upload_start = std::chrono::steady_clock::now();
    glBindBuffer(GL_ARRAY_BUFFER, b.gl);
    if (whole) {
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(b.cpu.size()), b.cpu.data(), GL_DYNAMIC_DRAW);  // new storage
        if (out_of_memory("a buffer's new storage")) {
            glFinish();
            ++oom_retries_;
            glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(b.cpu.size()), b.cpu.data(), GL_DYNAMIC_DRAW);
            const bool failed = out_of_memory("a buffer's new storage, retried after glFinish");
            logf("out of memory renewing a %zu KB buffer: %s", b.cpu.size() >> 10,
                 failed ? "FAILED again, the buffer is undefined" : "made after glFinish");
        }
    } else {
        glBufferSubData(GL_ARRAY_BUFFER, GLintptr(offset), GLsizeiptr(bytes), b.cpu.data() + offset);
    }
    upload_ns += std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - upload_start).count());
}

void Impl::update_texture(GlTexture& t, const Box& box, const std::uint8_t* data, unsigned pitch) {
    invalidate_state();  // 132ps5d: GL state set outside execute (cache_)
    NsTimer timer{texture_update_ns};
    if (t.depth || t.compressed) throw std::runtime_error("gfx gl46: depth / block texture updates are unported");
    const unsigned right = std::min(box.right, t.desc().width), bottom = std::min(box.bottom, t.layer_rows);
    if (right <= box.left || bottom <= box.top) return;
    glBindTexture(GL_TEXTURE_2D, t.gl);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, GLint(pitch / t.bytes_per_texel));
    glTexSubImage2D(GL_TEXTURE_2D, 0, GLint(box.left), GLint(box.top), GLsizei(right - box.left), GLsizei(bottom - box.top),
                    t.format, t.type, data);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

void Impl::read(const GlTexture& t, unsigned sub, std::vector<std::uint8_t>& out, unsigned& pitch) {
    invalidate_state();  // 132ps5d: GL state set outside execute (cache_)
    NsTimer timer{read_ns};
    ++reads;
    if (t.depth || t.compressed) throw std::runtime_error("gfx gl46: depth / block read-back is unported");
    const unsigned mips = std::max(t.desc().mips, 1u);
    if (sub % mips) throw std::runtime_error("gfx gl46: read-back of mip levels above 0 is unported");
    const unsigned w = std::max(t.desc().width, 1u);
    pitch = w * t.bytes_per_texel;
    out.resize(std::size_t(pitch) * t.layer_rows);
    if (has_get_sub_image) {
        glGetTextureSubImage(t.gl, 0, 0, GLint((sub / mips) * t.layer_rows), 0, GLsizei(w), GLsizei(t.layer_rows), 1,
                             t.format, t.type, GLsizei(out.size()), out.data());
        return;
    }
    // GL 3.3 path: the layer's rows read through an attachment-only framebuffer.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, copy_fbo_[0]);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.gl, 0);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glReadPixels(0, GLint((sub / mips) * t.layer_rows), GLsizei(w), GLsizei(t.layer_rows), t.format, t.type, out.data());
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    bound_fbo_ = ~0u;
}

// ---- command lists ----

void Impl::submit(CommandList& list) {
    const auto submit_start = std::chrono::steady_clock::now();
    std::size_t command_index = 0;
    for (const Command& c : list.commands()) {
        const auto command_start = std::chrono::steady_clock::now();
        const unsigned draws_before = draws_frame;
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
        case Command::Op::clear_target: clear_colour(texture_of(c.objects[0]), c.layers[0], c.values, nullptr, 0); break;
        case Command::Op::clear_depth:
            clear_depth(texture_of(c.objects[0]), c.flags, c.values[0], static_cast<std::uint8_t>(c.a));
            break;
        case Command::Op::clear_rects: {
            Rect rects[2]{};
            std::memcpy(rects, list.arena(c.c), sizeof(Rect) * std::min<unsigned>(c.count, 2));
            clear_colour(texture_of(c.objects[0]), 0, c.values, rects, std::min<unsigned>(c.count, 2));
            break;
        }
        case Command::Op::set_raster: state_.raster = static_cast<const GlRaster*>(static_cast<const RasterState*>(c.objects[0])); break;
        case Command::Op::set_depth: state_.depth_state = static_cast<const GlDepth*>(static_cast<const DepthState*>(c.objects[0])); break;
        case Command::Op::set_blend:
            state_.blend = static_cast<const GlBlend*>(static_cast<const BlendState*>(c.objects[0]));
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
                const GlBuffer* b = buffer_of(c.objects[k]);
                if (c.flags & stage_vertex) state_.vertex_constants[c.slot + k] = b;
                if (c.flags & stage_geometry) state_.geometry_constants[c.slot + k] = b;
                if (c.flags & stage_pixel) state_.pixel_constants[c.slot + k] = b;
            }
            break;
        case Command::Op::set_textures:
            for (unsigned k = 0; k < c.count && c.slot + k < 4; ++k) state_.textures[c.slot + k] = texture_of(c.objects[k]);
            break;
        case Command::Op::set_sampler:
            if (c.slot < 4) state_.samplers[c.slot] = static_cast<const GlSampler*>(static_cast<const SamplerState*>(c.objects[0]));
            break;
        case Command::Op::update_buffer:
            update_buffer(*const_cast<GlBuffer*>(buffer_of(c.objects[0])), list.arena(c.a), c.b, c.c, (c.flags & 1) == 0);
            break;
        case Command::Op::update_texture: {
            Box box;
            std::memcpy(&box, list.arena(c.d), sizeof box);
            update_texture(*const_cast<GlTexture*>(texture_of(c.objects[0])), box, list.arena(c.a), c.b);
            break;
        }
        case Command::Op::draw: draw(false, c.a, c.b); break;
        case Command::Op::draw_indexed: draw(true, c.a, c.b); break;
        case Command::Op::copy: {
            const GlTexture* dst = texture_of(c.objects[0]);
            const GlTexture* src = texture_of(c.objects[1]);
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
        case Command::Op::flush: glFlush(); break;
        case Command::Op::reset_state: state_ = State{}; break;
        }
        // 132ps5d (hw11: stalls of 1.5-2.5 s): a command slower than 50 ms is logged with what it did (the first 200).
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - command_start).count();
        if (ms > 50.0 && slow_logged_ < 200) {
            ++slow_logged_;
            const GlTexture* t0 = (c.op == Command::Op::copy || c.op == Command::Op::copy_region ||
                                   c.op == Command::Op::update_texture || c.op == Command::Op::clear_target)
                                      ? texture_of(c.objects[0]) : nullptr;
            const GlBuffer* b0 = c.op == Command::Op::update_buffer ? buffer_of(c.objects[0]) : nullptr;
            logf("slow command: op %u (#%zu of %zu in the list) %.1f ms; draws %u, a %u b %u c %u flags %u; texture %ux%u, "
                 "buffer %zu bytes; program %u (slow log %u of 200)",
                 unsigned(c.op), command_index, list.commands().size(), ms, draws_frame - draws_before, unsigned(c.a),
                 unsigned(c.b), unsigned(c.c), unsigned(c.flags), t0 ? t0->desc().width : 0u, t0 ? t0->rows() : 0u,
                 b0 ? b0->cpu.size() : std::size_t(0), unsigned(state_.program), slow_logged_);
        }
        ++command_index;
    }
    check_errors("a submitted list");
    list.clear();
    submit_ns += std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - submit_start).count());
}

// The back buffer drawn into the window (flipped: the window is bottom-up, the back buffer's row 0 is the D3D top),
// then the swap. Every 600th present logs four back-buffer pixels and the frame's draws (bring-up evidence).
void Impl::present(unsigned sync_interval) {
    poll_options();
    invalidate_state();  // 132ps5d: GL state set outside execute (cache_)
    if (presents % 600 == 0 && presents < 1200) {  // two samples only: a read-back stalls the pipeline
        bind_target(back.get(), nullptr);
        std::uint8_t px[4][4]{};
        const GLint at[4][2]{{width / 2, height / 2}, {width / 4, height / 4}, {3 * width / 4, 3 * height / 4}, {width / 8, height - 8}};
        for (int k = 0; k < 4; ++k) glReadPixels(at[k][0], at[k][1], 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px[k]);
        logf("present %llu: back buffer pixels (%d,%d) %02x%02x%02x (%d,%d) %02x%02x%02x (%d,%d) %02x%02x%02x (%d,%d) "
             "%02x%02x%02x, %u draws this frame",
             static_cast<unsigned long long>(presents), at[0][0], at[0][1], px[0][0], px[0][1], px[0][2], at[1][0], at[1][1],
             px[1][0], px[1][1], px[1][2], at[2][0], at[2][1], px[2][0], px[2][1], px[2][2], at[3][0], at[3][1], px[3][0],
             px[3][1], px[3][2], draws_frame);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    bound_fbo_ = 0;
    glViewport(0, 0, width, height);
    glDepthRange(0.0, 1.0);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(present_program_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, back->gl);
    glBindSampler(0, samplers_[unsigned(Filter::linear)][0]);
    for (unsigned k = 0; k < 8; ++k) glDisableVertexAttribArray(k);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    const int interval = static_cast<int>(std::min(sync_interval, 4u));
    if (interval != swap_interval_) {
        eglSwapInterval(display, interval);
        swap_interval_ = interval;
    }
    const auto swap_start = std::chrono::steady_clock::now();
    const bool swapped = eglSwapBuffers(display, surface);
    const auto swap_time = std::chrono::steady_clock::now() - swap_start;
    swap_ns += std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(swap_time).count());
    if (swap_time > std::chrono::milliseconds(50) && slow_logged_ < 200) {
        ++slow_logged_;
        logf("slow present: eglSwapBuffers %.1f ms (present %llu, %u draws) (slow log %u of 200)",
             std::chrono::duration<double, std::milli>(swap_time).count(), static_cast<unsigned long long>(presents),
             draws_frame, slow_logged_);
    }
    ++timed_presents;
    if (!swapped && errors_logged_ < 64) {
        ++errors_logged_;
        logf("eglSwapBuffers failed: 0x%04x", unsigned(eglGetError()));
    }
    check_errors("the present");
    ++presents;
    draws_last_frame = draws_frame;
    draws_frame = 0;
}

// ---- the API objects ----

class GlDevice final : public Device {
public:
    const std::string& name() const override { return name_; }
    Capabilities capabilities() const override { return {.gpu_skinning = false, .block_textures = g_impl->s3tc}; }
    std::shared_ptr<Buffer> create_buffer(const BufferDesc& desc, const void* initial, const char*) override {
        return g_thread.call([&] { return std::shared_ptr<Buffer>(g_impl->make_buffer(desc, initial)); });
    }
    std::shared_ptr<Texture> create_texture(const TextureDesc& desc, const Level* levels, const TextureLabels& labels) override {
        count_texture_label(labels.texture);
        return g_thread.call([&] { return std::shared_ptr<Texture>(g_impl->make_texture(desc, levels)); });
    }
    std::shared_ptr<BlendState> create_blend(const BlendDesc& desc, const char*) override {
        auto s = std::make_shared<GlBlend>();
        s->desc = desc;
        return s;
    }
    std::shared_ptr<DepthState> create_depth(const DepthDesc& desc, const char*) override {
        auto s = std::make_shared<GlDepth>();
        s->desc = desc;
        return s;
    }
    std::shared_ptr<RasterState> create_raster(const RasterDesc& desc, const char*) override {
        auto s = std::make_shared<GlRaster>();
        s->desc = desc;
        return s;
    }
    std::shared_ptr<SamplerState> create_sampler(const SamplerDesc& desc, const char*) override {
        auto s = std::make_shared<GlSampler>();
        s->desc = desc;
        return s;
    }
    void prepare(Program program) override { g_thread.call([&] { g_impl->prepare(program); }); }
    void submit(CommandList& list) override { g_thread.call([&] { g_impl->submit(list); }); }
    void write(const Buffer& buffer, const void* data, std::size_t bytes, const char*) override {
        g_thread.call([&] {
            g_impl->update_buffer(const_cast<GlBuffer&>(static_cast<const GlBuffer&>(buffer)), data, bytes, 0, true);
        });
    }
    void read(const Texture& staging, unsigned subresource,
              const std::function<void(const std::uint8_t* data, unsigned row_pitch)>& reader, const char*) override {
        std::vector<std::uint8_t> bytes;
        unsigned pitch = 0;
        g_thread.call([&] { g_impl->read(static_cast<const GlTexture&>(staging), subresource, bytes, pitch); });
        reader(bytes.data(), pitch);
    }
    bool removed() const override { return false; }
    bool wait_idle() override {
        g_thread.call([] { g_impl->finish(); });
        return true;
    }

private:
    std::string name_ = "OpenGL 4.6 (PS5)";
};

class GlSwapChain final : public SwapChain {
public:
    explicit GlSwapChain(Options options) : options_(std::move(options)) {}
    void size(unsigned& width, unsigned& height, const char*) override {
        width = unsigned(g_impl->width);
        height = unsigned(g_impl->height);
    }
    std::shared_ptr<Texture> buffer(bool, const char*, const char*) override { return g_impl->back; }
    void resize(unsigned, unsigned, const char*) override {}  // the EGL window surface is fixed
    Presented present(unsigned sync_interval, const char*) override {
        g_thread.call([&] { g_impl->present(sync_interval); });
        if (options_.presented) options_.presented();
        return Presented::ok;
    }

private:
    Options options_;
};

std::mutex g_create_mutex;
std::shared_ptr<GlDevice> g_device;
std::shared_ptr<GlSwapChain> g_swap_chain;

}  // namespace

Display create_display(const Options& options) {
    std::lock_guard lock(g_create_mutex);
    if (!g_impl) {
        auto* impl = new Impl(options);  // one per process (the EGL context); kept for the process lifetime
        g_thread.start([impl] {
            g_impl = impl;
            impl->init();
        });
        g_device = std::make_shared<GlDevice>();
        g_swap_chain = std::make_shared<GlSwapChain>(options);
    }
    Display d;
    d.device = g_device;
    d.swap_chain = g_swap_chain;
    d.backend = "OpenGL 4.6 (PS5): " + g_impl->version + " / " + g_impl->renderer;
    return d;
}

}  // namespace omega::gfx::gl46

extern "C" std::size_t omega_ps5_heap_live_bytes();  // src/port/ps5/ps5_heap.cpp: the title heap's live bytes
extern "C" int sceKernelAvailableFlexibleMemorySize(std::size_t* size);

namespace omega::gfx::gl46 {

std::string stats_line() {
    if (!g_impl) return "GL 4.6: not started";
    // Per present since the last line: the GL thread's command-list time (buffer uploads included) and the swap.
    const std::uint64_t n = g_impl->timed_presents.exchange(0);
    const double per = n ? 1e-6 / double(n) : 0.0;
    std::size_t flexible = 0;
    sceKernelAvailableFlexibleMemorySize(&flexible);
    char timing[600];
    std::snprintf(timing, sizeof timing,
                  "; per present: lists %.2f ms (uploads %.2f ms, %.0f KB), swap %.2f ms; in 10 s: %llu textures made "
                  "(%.0f ms), texture updates %.0f ms, %llu read-backs (%.0f ms); runtime heap %.0f MB live, flexible "
                  "memory %.0f MB free; %llu copies drawn, %llu out-of-memory retries",
                  double(g_impl->submit_ns.exchange(0)) * per, double(g_impl->upload_ns.exchange(0)) * per,
                  n ? double(g_impl->upload_bytes.exchange(0)) / 1024.0 / double(n) : 0.0,
                  double(g_impl->swap_ns.exchange(0)) * per, (unsigned long long)g_impl->texture_creates.exchange(0),
                  double(g_impl->texture_create_ns.exchange(0)) * 1e-6, double(g_impl->texture_update_ns.exchange(0)) * 1e-6,
                  (unsigned long long)g_impl->reads.exchange(0), double(g_impl->read_ns.exchange(0)) * 1e-6,
                  double(omega_ps5_heap_live_bytes()) / 1048576.0, double(flexible) / 1048576.0,
                  (unsigned long long)g_impl->gpu_copies_.exchange(0), (unsigned long long)g_impl->oom_retries_.load());
    char text[320];
    std::snprintf(text, sizeof text,
                  "GL 4.6: %llu buffers (%llu KB), %llu textures (%llu KB), %llu presents, %u draws last frame, %llu draws, "
                  "%u program failures, %llu draws skipped",
                  static_cast<unsigned long long>(g_buffers.load()), static_cast<unsigned long long>(g_buffer_bytes.load() >> 10),
                  static_cast<unsigned long long>(g_textures.load()), static_cast<unsigned long long>(g_texture_bytes.load() >> 10),
                  static_cast<unsigned long long>(g_impl->presents), g_impl->draws_last_frame,
                  static_cast<unsigned long long>(g_impl->draws_total), g_impl->program_failures,
                  static_cast<unsigned long long>(g_impl->skipped_draws));
    const std::string labels = take_texture_labels();
    return std::string(text) + timing + (labels.empty() ? std::string() : "; textures made by label: " + labels);
}

}  // namespace omega::gfx::gl46

namespace omega::gfx {

// One EGL context per process: the GUI renderer's offscreen captures share it.
std::shared_ptr<Device> create_offscreen_device(const char*) { return gl46::create_display().device; }

}  // namespace omega::gfx
