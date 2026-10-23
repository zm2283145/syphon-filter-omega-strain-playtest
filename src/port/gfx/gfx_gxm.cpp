// Port-only (checkpoint 132vt1): the PS Vita GXM backend of gfx::Device (gfx_gxm.hpp; research/RENDERER.md "The GXM
// backend", research/VITA.md "Renderer"). sceGxm directly: one context, scenes begun lazily on the targets a draw
// names, the Cg programs of gfx_gxm_shaders.cpp compiled by SceShaccCg at first use and cached as GXP files.
//
// GXM differences from D3D11 handled here (each listed in VITA.md "Renderer: known differences"):
// - Every draw and clear runs inside a scene; a change of colour / depth target ends the scene and begins another.
//   Clears and texture copies are quads drawn with internal programs (GXM has no ClearView / CopyResource).
// - Blend state is part of the patched fragment program; the D3D blend factor has no GXM factor, so it is folded
//   into the program output (omegaBlendMul, blend_for()).
// - No geometry stage: shadow_receiver's guard-box / facing test filters the triangle list on the CPU.
// - Texture arrays are layers stacked vertically; only mip 0 is kept (textures sample their top level).
// - Resources the GPU may still read are never rewritten in place: a CPU update gets fresh memory and the old block
//   is freed once the epoch that used it has completed (notification at each present, or sceGxmFinish).
#include "gfx_gxm.hpp"

#include "gfx_gxm_shaders.hpp"

#include <psp2/common_dialog.h>
#include <psp2/display.h>
#include <psp2/gxm.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/message_dialog.h>
#include <vitashark.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace omega::gfx::gxm {
namespace {

[[noreturn]] void fail(const char* what, int code) {
    char text[160];
    std::snprintf(text, sizeof text, "gfx gxm: %s failed (0x%08x)", what, static_cast<unsigned>(code));
    throw std::runtime_error(text);
}
void check(int code, const char* what) {
    if (code < 0) fail(what, code);
}
std::size_t align_up(std::size_t v, std::size_t a) { return (v + a - 1) / a * a; }

// ---- memory ----

// A kernel memory block mapped for the GPU.
struct Block {
    SceUID uid = -1;
    void* base = nullptr;
    std::size_t bytes = 0;
};
enum class Map : std::uint8_t { gpu, vertex_usse, fragment_usse };
bool allocate_block(Block& b, const char* name, SceKernelMemBlockType type, std::size_t bytes, Map map,
                    unsigned* usse_offset = nullptr) {
    const bool cdram = type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW;
    const std::size_t size = align_up(bytes, cdram ? 256u * 1024u : 4096u);
    b = Block{};
    b.uid = sceKernelAllocMemBlock(name, type, size, nullptr);
    if (b.uid < 0) return false;
    if (sceKernelGetMemBlockBase(b.uid, &b.base) < 0) {
        sceKernelFreeMemBlock(b.uid);
        b = Block{};
        return false;
    }
    b.bytes = size;
    int r = 0;
    if (map == Map::gpu) r = sceGxmMapMemory(b.base, b.bytes, SCE_GXM_MEMORY_ATTRIB_RW);
    else if (map == Map::vertex_usse) r = sceGxmMapVertexUsseMemory(b.base, b.bytes, usse_offset);
    else r = sceGxmMapFragmentUsseMemory(b.base, b.bytes, usse_offset);
    if (r < 0) {
        sceKernelFreeMemBlock(b.uid);
        b = Block{};
        return false;
    }
    return true;
}

// A GPU-mapped heap (sceClibMspace over one block).
class Pool {
public:
    bool create(const char* name, SceKernelMemBlockType type, std::size_t bytes) {
        if (!allocate_block(block_, name, type, bytes, Map::gpu)) return false;
        space_ = sceClibMspaceCreate(block_.base, block_.bytes);
        return space_ != nullptr;
    }
    void* allocate(std::size_t bytes, std::size_t alignment) {
        if (!space_) return nullptr;
        void* p = sceClibMspaceMemalign(space_, alignment, bytes ? bytes : 1);
        if (!p) return nullptr;
        sizes_[p] = bytes;
        used_ += bytes;
        peak_ = std::max(peak_, used_);
        return p;
    }
    bool owns(const void* p) const {
        const auto* c = static_cast<const std::uint8_t*>(p);
        const auto* b = static_cast<const std::uint8_t*>(block_.base);
        return block_.base && c >= b && c < b + block_.bytes;
    }
    void release(void* p) {
        const auto it = sizes_.find(p);
        if (it == sizes_.end()) return;
        used_ -= it->second;
        sizes_.erase(it);
        sceClibMspaceFree(space_, p);
    }
    std::size_t size() const { return block_.bytes; }
    std::size_t used() const { return used_; }
    std::size_t peak() const { return peak_; }

private:
    Block block_;
    void* space_ = nullptr;
    std::unordered_map<void*, std::size_t> sizes_;
    std::size_t used_ = 0, peak_ = 0;
};

// ---- resources ----

class Impl;
Impl* g_impl = nullptr;
void release_memory(void* p);  // deferred to the epoch's completion

struct GxmBuffer final : Buffer {
    explicit GxmBuffer(const BufferDesc& d) { desc_ = d; }
    ~GxmBuffer() override { release_memory(data); }
    std::vector<std::uint8_t> cpu;  // constant buffers: copied into the default uniform buffers per draw
    std::uint8_t* data = nullptr;   // vertex / index buffers (LPDDR)
    std::uint32_t used_epoch = 0;
};

struct GxmTexture final : Texture {
    GxmTexture() = default;
    ~GxmTexture() override {
        if (owned) release_memory(data);
    }
    std::uint8_t* data = nullptr;
    bool owned = true;
    unsigned stride = 0;      // pixels per row (width aligned to 8, depth: to 32)
    unsigned bpp = 4;         // stored bytes per pixel (r8 is stored as U8U8U8U8, VITA.md)
    unsigned layer_rows = 0;  // rows per layer (the stacked layers)
    SceGxmTextureFormat texture_format = SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR;
    SceGxmColorFormat colour_format = SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR;
    bool float_target = false;   // r32f: FLOAT output register, the OMEGA_FLOAT_OUT programs
    bool depth = false;
    int display = -1;            // a display buffer's index (its sync object)
    bool back_proxy = false;     // SwapChain::buffer's texture: the current back buffer when a list executes
    bool clear_pending = false;  // depth: the next scene starts from clear_value
    float clear_value = 1.0F;
    std::uint32_t used_epoch = 0;

    std::size_t layer_bytes() const { return std::size_t(stride) * layer_rows * bpp; }
    std::size_t bytes() const { return layer_bytes() * std::max(desc_.layers, 1u); }
    std::uint8_t* layer(unsigned k) const { return data + layer_bytes() * k; }
    void set_desc(const TextureDesc& d) { desc_ = d; }
};

struct GxmBlend final : BlendState {
    BlendDesc desc;
};
struct GxmDepth final : DepthState {
    DepthDesc desc;
};
struct GxmRaster final : RasterState {
    RasterDesc desc;
};
struct GxmSampler final : SamplerState {
    SamplerDesc desc;
};

// ---- programs ----

// One compiled Cg stage (its GXP, registered with the shader patcher) and the uniforms the backend sets.
struct CompiledStage {
    std::vector<std::uint32_t> code;
    SceGxmShaderPatcherId id = nullptr;
    const SceGxmProgramParameter* constants[2]{};
    const SceGxmProgramParameter* tex_size = nullptr;
    const SceGxmProgramParameter* layers = nullptr;
    const SceGxmProgramParameter* blend_mul = nullptr;
    const SceGxmProgramParameter* colour = nullptr;
    const SceGxmProgramParameter* clear = nullptr;
    const SceGxmProgram* program() const { return reinterpret_cast<const SceGxmProgram*>(code.data()); }
};

std::uint64_t fnv64(const std::string& s) {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned char c : s) h = (h ^ c) * 0x100000001b3ULL;
    return h;
}

std::string g_compile_log;
void compile_log(const char* message, shark_log_level, int line) {
    char head[32];
    std::snprintf(head, sizeof head, "line %d: ", line);
    g_compile_log += head;
    g_compile_log += message ? message : "";
    g_compile_log += '\n';
}

void make_directories(const std::string& path) {
    for (std::size_t k = path.find('/'); k != std::string::npos; k = path.find('/', k + 1))
        sceIoMkdir(path.substr(0, k).c_str(), 0777);
    sceIoMkdir(path.c_str(), 0777);
}

// ---- pipeline description of one draw (the API draws and the internal clear / copy quads) ----

struct UniformBlock {
    const void* data = nullptr;
    std::size_t bytes = 0;
};
struct BoundTexture {
    const GxmTexture* texture = nullptr;
    SamplerDesc sampler{};
};
struct Pipeline {
    unsigned vertex = 0, fragment = 0;
    const GxmAttribute* attributes = nullptr;
    unsigned attribute_count = 0;
    std::uint32_t stride = 0;
    bool blend_info = false;  // false: no blending, every channel
    SceGxmBlendInfo blend{};
    float blend_mul[4]{1, 1, 1, -1};
    SceGxmDepthFunc depth_func = SCE_GXM_DEPTH_FUNC_ALWAYS;
    bool depth_write = false;
    bool wireframe = false;
    Viewport viewport{};
    BoundTexture textures[4]{};
    UniformBlock vertex_constants[2]{}, fragment_constants[2]{};
    float colour[4]{};
    float clear[4]{};
    const void* stream = nullptr;
    const std::uint32_t* indices = nullptr;
    std::uint32_t count = 0;
};

const GxmAttribute clear_attributes[]{{"position", 0, 2}};
const GxmAttribute copy_attributes[]{{"position", 0, 2}, {"uv", 8, 2}};

SceGxmBlendFactor blend_factor(BlendFactor f) {
    switch (f) {
    case BlendFactor::zero: return SCE_GXM_BLEND_FACTOR_ZERO;
    case BlendFactor::one: return SCE_GXM_BLEND_FACTOR_ONE;
    case BlendFactor::src_alpha: return SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
    case BlendFactor::inv_src_alpha: return SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    default: return SCE_GXM_BLEND_FACTOR_ONE;
    }
}
std::uint8_t colour_mask(std::uint8_t d3d) {
    std::uint8_t m = 0;
    if (d3d & mask_r) m |= SCE_GXM_COLOR_MASK_R;
    if (d3d & mask_g) m |= SCE_GXM_COLOR_MASK_G;
    if (d3d & mask_b) m |= SCE_GXM_COLOR_MASK_B;
    if (d3d & mask_a) m |= SCE_GXM_COLOR_MASK_A;
    return m;
}
bool is_factor(BlendFactor f) { return f == BlendFactor::factor || f == BlendFactor::inv_factor; }

// The D3D blend of one target as a GXM blend info plus the program-side factor (omegaBlendMul). D3D's constant
// blend factor has no GXM equivalent: src = (inv_)factor with a plain dst multiplies the output colour; src = factor,
// dst = inv_factor (the PS2 FIX blend, a grey factor) becomes SRC_ALPHA / ONE_MINUS_SRC_ALPHA with the output alpha
// replaced by the factor (the stored alpha then differs: VITA.md). Any other use is unported.
void blend_for(const TargetBlend& t, const float factor[4], Pipeline& p) {
    p.blend_mul[0] = p.blend_mul[1] = p.blend_mul[2] = 1.0F;
    p.blend_mul[3] = -1.0F;
    const std::uint8_t mask = colour_mask(t.write_mask);
    if (!t.enable) {
        p.blend_info = mask != SCE_GXM_COLOR_MASK_ALL;
        p.blend = {};
        p.blend.colorMask = mask;
        p.blend.colorFunc = SCE_GXM_BLEND_FUNC_NONE;
        p.blend.alphaFunc = SCE_GXM_BLEND_FUNC_NONE;
        p.blend.colorSrc = p.blend.alphaSrc = SCE_GXM_BLEND_FACTOR_ONE;
        p.blend.colorDst = p.blend.alphaDst = SCE_GXM_BLEND_FACTOR_ZERO;
        return;
    }
    p.blend_info = true;
    p.blend = {};
    p.blend.colorMask = mask;
    p.blend.colorFunc = t.op == BlendOp::add ? SCE_GXM_BLEND_FUNC_ADD : SCE_GXM_BLEND_FUNC_REVERSE_SUBTRACT;
    p.blend.alphaFunc = t.op_alpha == BlendOp::add ? SCE_GXM_BLEND_FUNC_ADD : SCE_GXM_BLEND_FUNC_REVERSE_SUBTRACT;
    SceGxmBlendFactor src = blend_factor(t.src), dst = blend_factor(t.dst);
    bool fix_over = false;
    if (is_factor(t.src)) {
        float f[3];
        for (int k = 0; k < 3; ++k) f[k] = t.src == BlendFactor::factor ? factor[k] : 1.0F - factor[k];
        if (!is_factor(t.dst)) {
            src = SCE_GXM_BLEND_FACTOR_ONE;
            for (int k = 0; k < 3; ++k) p.blend_mul[k] = f[k];
        } else if (t.src == BlendFactor::factor && t.dst == BlendFactor::inv_factor && factor[0] == factor[1] &&
                   factor[1] == factor[2]) {
            src = SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
            dst = SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            p.blend_mul[3] = factor[0];
            fix_over = true;
        } else {
            throw std::runtime_error("gfx gxm: unported blend (factor / factor combination other than FIX over)");
        }
    } else if (is_factor(t.dst)) {
        throw std::runtime_error("gfx gxm: unported blend (destination blend factor)");
    }
    p.blend.colorSrc = src;
    p.blend.colorDst = dst;
    SceGxmBlendFactor asrc = blend_factor(t.src_alpha), adst = blend_factor(t.dst_alpha);
    if (is_factor(t.src_alpha) || is_factor(t.dst_alpha)) {
        if (!(t.write_mask & mask_a)) {
            asrc = SCE_GXM_BLEND_FACTOR_ONE;
            adst = SCE_GXM_BLEND_FACTOR_ZERO;
        } else if (fix_over) {
            asrc = SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
            adst = SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        } else {
            throw std::runtime_error("gfx gxm: unported blend (alpha blend factor)");
        }
    }
    p.blend.alphaSrc = asrc;
    p.blend.alphaDst = adst;
}

SceGxmDepthFunc depth_func(Compare c) {
    switch (c) {
    case Compare::never: return SCE_GXM_DEPTH_FUNC_NEVER;
    case Compare::less: return SCE_GXM_DEPTH_FUNC_LESS;
    case Compare::less_equal: return SCE_GXM_DEPTH_FUNC_LESS_EQUAL;
    default: return SCE_GXM_DEPTH_FUNC_ALWAYS;
    }
}

// shadow_receiver's geometry shader (gfx_d3d11_shaders.cpp passGeometry) on the CPU: a triangle is dropped when any
// vertex lies outside the projector's guard box (|X.xyz| > |X.w|, X = (p, 1) * clip) or the closing vertex's
// model-space normal faces away from the projector (dot(facing.xyz, n) >= 0).
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

// ---- the device state ----

struct DisplayRequest {
    void* address;
    int wait;
};
void display_callback(const void* data) {
    const auto& r = *static_cast<const DisplayRequest*>(data);
    SceDisplayFrameBuf frame{};
    frame.size = sizeof frame;
    frame.base = r.address;
    frame.pitch = 960;
    frame.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    frame.width = 960;
    frame.height = 544;
    sceDisplaySetFrameBuf(&frame, SCE_DISPLAY_SETBUF_NEXTFRAME);
    if (r.wait) sceDisplayWaitVblankStart();
}
void* patcher_alloc(void*, unsigned int bytes) { return std::calloc(1, bytes); }
void patcher_free(void*, void* p) { std::free(p); }

constexpr unsigned display_width = 960, display_height = 544;

// The draw state of the context (D3D11-style: it persists across submitted lists until changed or reset).
struct State {
    const GxmTexture* colour[2]{};
    unsigned layer[2]{};
    const GxmTexture* depth = nullptr;
    Viewport viewport{};
    const GxmRaster* raster = nullptr;
    const GxmDepth* depth_state = nullptr;
    const GxmBlend* blend = nullptr;
    float factor[4]{1, 1, 1, 1};
    Program program{};
    bool has_program = false;
    const GxmBuffer* vertices = nullptr;
    std::uint32_t stride = 0, offset = 0;
    const GxmBuffer* indices = nullptr;
    const GxmBuffer* vertex_constants[4]{};
    const GxmBuffer* geometry_constants[4]{};
    const GxmBuffer* pixel_constants[4]{};
    const GxmTexture* textures[4]{};
    const GxmSampler* samplers[4]{};
};

// The open scene (keys by memory, so the swap chain's per-call back-buffer objects match).
struct Scene {
    bool open = false;
    const std::uint8_t* colour = nullptr;
    const std::uint8_t* depth = nullptr;
    bool float_target = false;
    unsigned width = 0, height = 0;
};

class Impl {
public:
    explicit Impl(const Options& o);

    std::recursive_mutex mutex;
    Options options;
    MemoryStats stats;

    // ---- memory and epochs ----
    std::uint8_t* allocate(std::size_t bytes, bool cdram_first, std::size_t alignment = 256) {
        for (int attempt = 0; attempt < 2; ++attempt) {
            Pool& a = cdram_first ? cdram_ : lpddr_;
            Pool& b = cdram_first ? lpddr_ : cdram_;
            if (void* p = a.allocate(bytes, alignment)) return static_cast<std::uint8_t*>(p);
            if (void* p = b.allocate(bytes, alignment)) return static_cast<std::uint8_t*>(p);
            drain();  // free what the GPU has finished with, then retry once
        }
        char text[128];
        std::snprintf(text, sizeof text, "gfx gxm: out of GPU memory (%u bytes; VITA.md \"Memory\")",
                      static_cast<unsigned>(bytes));
        throw std::runtime_error(text);
    }
    void release(void* p) {
        if (p) deferred_.emplace_back(epoch_, p);
    }
    void collect() {
        const std::uint32_t signalled = notify_[0];
        const std::uint32_t done = std::max<std::uint32_t>(completed_, signalled);
        completed_ = done;
        std::size_t kept = 0;
        for (auto& d : deferred_) {
            if (d.first <= done) free_now(d.second);
            else deferred_[kept++] = d;
        }
        deferred_.resize(kept);
    }
    void free_now(void* p) {
        if (cdram_.owns(p)) cdram_.release(p);
        else lpddr_.release(p);
    }
    // Waits for everything recorded so far (the open scene is ended).
    void drain() {
        end_scene(false);
        sceGxmFinish(context_);
        completed_ = epoch_;
        ++epoch_;
        collect();
    }
    bool idle(std::uint32_t used) const { return used == 0 || used <= completed_; }
    std::uint32_t epoch() const { return epoch_; }

    // ---- programs ----
    CompiledStage& stage(bool vertex, unsigned index, bool float_out);
    SceGxmVertexProgram* vertex_program(unsigned index, const GxmAttribute* attributes, unsigned count, std::uint32_t stride);
    SceGxmFragmentProgram* fragment_program(unsigned index, bool float_out, const Pipeline& p, const CompiledStage& vs);

    // ---- scenes and draws ----
    void ensure_scene(const GxmTexture* colour, unsigned layer, const GxmTexture* depth);
    void end_scene(bool notify);
    void execute(Pipeline& p);
    void draw(bool indexed, std::uint32_t count, std::uint32_t first);
    void clear_colour(const GxmTexture* target, unsigned layer, const float rgba[4], const Rect* rects, unsigned count);
    void clear_depth(const GxmTexture* depth, float value);
    void copy_region(const GxmTexture* dst, unsigned dst_sub, unsigned x, unsigned y, const GxmTexture* src,
                     unsigned src_sub, const Box* box);
    void submit(CommandList& list);
    void update_buffer(GxmBuffer& b, const void* data, std::size_t bytes, std::size_t offset, bool keep_rest);
    void update_texture(GxmTexture& t, const Box& box, const std::uint8_t* data, unsigned pitch);
    const std::uint32_t* sequence(std::uint32_t count);
    SwapChain::Presented present(unsigned sync_interval);

    std::shared_ptr<GxmTexture> make_texture(const TextureDesc& desc, const Level* levels);
    std::shared_ptr<GxmTexture> back_buffer();
    const GxmTexture* resolve(const void* p) const;  // a Texture* of a list: the back buffer proxy resolved
    void read(const GxmTexture& t, unsigned sub,
              const std::function<void(const std::uint8_t* data, unsigned row_pitch)>& reader);
    void message(const std::string& text);
    MemoryStats snapshot() const {
        MemoryStats s = stats;
        s.cdram_pool = cdram_.size();
        s.cdram_used = cdram_.used();
        s.cdram_peak = cdram_.peak();
        s.lpddr_pool = lpddr_.size();
        s.lpddr_used = lpddr_.used();
        s.lpddr_peak = lpddr_.peak();
        return s;
    }

private:
    SceGxmContext* context_ = nullptr;
    SceGxmShaderPatcher* patcher_ = nullptr;
    SceGxmRenderTarget* render_target(unsigned width, unsigned height);
    void begin_scene(const GxmTexture* colour, unsigned layer, const GxmTexture* depth);
    void write_rows(GxmTexture& t, unsigned left, unsigned top, unsigned right, unsigned bottom,
                    const std::uint8_t* data, unsigned pitch);

    Block vdm_, vertex_ring_, fragment_ring_, fragment_usse_ring_, patcher_buffer_, vertex_usse_, fragment_usse_;
    unsigned fragment_ring_offset_ = 0, vertex_usse_offset_ = 0, fragment_usse_offset_ = 0;
    void* host_memory_ = nullptr;
    Pool cdram_, lpddr_;
    struct DisplayBuffer {
        Block block;
        SceGxmSyncObject* sync = nullptr;
        std::shared_ptr<GxmTexture> texture;
    };
    std::vector<DisplayBuffer> displays_;
    unsigned back_ = 0, front_ = 0;
    std::shared_ptr<GxmTexture> back_proxy_;  // back_buffer(): one texture for every frame (D3D's buffer 0)
    std::map<std::pair<unsigned, unsigned>, SceGxmRenderTarget*> targets_;
    std::map<const void*, SceGxmSyncObject*> offscreen_syncs_;
    volatile unsigned int* notify_ = nullptr;
    std::uint32_t epoch_ = 1, completed_ = 0;
    std::vector<std::pair<std::uint32_t, void*>> deferred_;
    std::map<std::uint32_t, CompiledStage> stages_;
    std::map<std::uint64_t, SceGxmVertexProgram*> vertex_programs_;
    std::map<std::string, SceGxmFragmentProgram*> fragment_programs_;
    bool compiler_ready_ = false;
    Scene scene_;
    State state_;
    std::shared_ptr<GxmTexture> dummy_;
    GxmBuffer* clear_quad_ = nullptr;
    std::shared_ptr<GxmBuffer> clear_quad_owner_;
    std::uint32_t* sequence_ = nullptr;
    std::uint32_t sequence_count_ = 0;
    unsigned scenes_ = 0, draws_ = 0, logged_draws_ = 0;
    std::size_t transient_ = 0;
};

void release_memory(void* p) {
    if (p && g_impl) {
        std::lock_guard<std::recursive_mutex> lock(g_impl->mutex);
        g_impl->release(p);
    }
}

Impl::Impl(const Options& o) : options(o) {
    const unsigned buffers = std::clamp(o.display_buffers, 2u, 3u);
    SceGxmInitializeParams init{};
    init.displayQueueMaxPendingCount = buffers - 1;
    init.displayQueueCallback = display_callback;
    init.displayQueueCallbackDataSize = sizeof(DisplayRequest);
    init.parameterBufferSize = SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE;
    check(sceGxmInitialize(&init), "sceGxmInitialize");
    notify_ = sceGxmGetNotificationRegion();
    notify_[0] = 0;

    const auto lpddr = SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE;
    const auto cdram = SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW;
    if (!allocate_block(vdm_, "omega-gxm-vdm", lpddr, SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE, Map::gpu) ||
        !allocate_block(vertex_ring_, "omega-gxm-vertex", lpddr, SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE, Map::gpu) ||
        !allocate_block(fragment_ring_, "omega-gxm-fragment", lpddr, SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE, Map::gpu) ||
        !allocate_block(fragment_usse_ring_, "omega-gxm-fusse-ring", lpddr, SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE,
                        Map::fragment_usse, &fragment_ring_offset_))
        fail("allocate the GXM ring buffers", -1);
    // Zeroed: Vita3K constructs its context state inside hostMem without initialising every field, so heap
    // leftovers here made rendering fail at random (VITA.md "The black-screen investigation").
    host_memory_ = std::calloc(1, SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE);
    SceGxmContextParams context{};
    context.hostMem = host_memory_;
    context.hostMemSize = SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE;
    context.vdmRingBufferMem = vdm_.base;
    context.vdmRingBufferMemSize = vdm_.bytes;
    context.vertexRingBufferMem = vertex_ring_.base;
    context.vertexRingBufferMemSize = vertex_ring_.bytes;
    context.fragmentRingBufferMem = fragment_ring_.base;
    context.fragmentRingBufferMemSize = fragment_ring_.bytes;
    context.fragmentUsseRingBufferMem = fragment_usse_ring_.base;
    context.fragmentUsseRingBufferMemSize = fragment_usse_ring_.bytes;
    context.fragmentUsseRingBufferOffset = fragment_ring_offset_;
    check(sceGxmCreateContext(&context, &context_), "sceGxmCreateContext");

    if (!allocate_block(patcher_buffer_, "omega-gxm-patcher", lpddr, 1u << 20, Map::gpu) ||
        !allocate_block(vertex_usse_, "omega-gxm-vusse", lpddr, 1u << 20, Map::vertex_usse, &vertex_usse_offset_) ||
        !allocate_block(fragment_usse_, "omega-gxm-fusse", lpddr, 2u << 20, Map::fragment_usse, &fragment_usse_offset_))
        fail("allocate the shader patcher memory", -1);
    SceGxmShaderPatcherParams patcher{};
    patcher.hostAllocCallback = patcher_alloc;
    patcher.hostFreeCallback = patcher_free;
    patcher.bufferMem = patcher_buffer_.base;
    patcher.bufferMemSize = patcher_buffer_.bytes;
    patcher.vertexUsseMem = vertex_usse_.base;
    patcher.vertexUsseMemSize = vertex_usse_.bytes;
    patcher.vertexUsseOffset = vertex_usse_offset_;
    patcher.fragmentUsseMem = fragment_usse_.base;
    patcher.fragmentUsseMemSize = fragment_usse_.bytes;
    patcher.fragmentUsseOffset = fragment_usse_offset_;
    check(sceGxmShaderPatcherCreate(&patcher, &patcher_), "sceGxmShaderPatcherCreate");

    displays_.resize(buffers);
    for (unsigned k = 0; k < buffers; ++k) {
        DisplayBuffer& d = displays_[k];
        if (!allocate_block(d.block, "omega-gxm-display", cdram, std::size_t(display_width) * display_height * 4, Map::gpu))
            fail("allocate a display buffer (CDRAM)", -1);
        std::memset(d.block.base, 0, d.block.bytes);
        check(sceGxmSyncObjectCreate(&d.sync), "sceGxmSyncObjectCreate");
        auto t = std::make_shared<GxmTexture>();
        TextureDesc desc;
        desc.width = display_width;
        desc.height = display_height;
        desc.format = Format::bgra8;  // what the D3D swap chain reports; stored A8B8G8R8 (the display's format)
        desc.bind = bind_target | bind_sampled;
        t->set_desc(desc);
        t->data = static_cast<std::uint8_t*>(d.block.base);
        t->owned = false;
        t->stride = display_width;
        t->layer_rows = display_height;
        t->display = static_cast<int>(k);
        d.texture = t;
        stats.fixed_bytes += d.block.bytes;
    }
    front_ = buffers - 1;
    back_ = 0;
    stats.fixed_bytes += vdm_.bytes + vertex_ring_.bytes + fragment_ring_.bytes + fragment_usse_ring_.bytes +
                         patcher_buffer_.bytes + vertex_usse_.bytes + fragment_usse_.bytes;

    // The pools: the requested size, halved until the kernel grants it (VITA.md "Memory").
    for (std::size_t bytes = o.cdram_pool_bytes; bytes >= (8u << 20) && !cdram_.size(); bytes /= 2)
        cdram_.create("omega-gxm-cdram", cdram, bytes);
    for (std::size_t bytes = o.lpddr_pool_bytes; bytes >= (8u << 20) && !lpddr_.size(); bytes /= 2)
        lpddr_.create("omega-gxm-lpddr", lpddr, bytes);
    if (!lpddr_.size()) fail("allocate the LPDDR pool", -1);

    TextureDesc dummy;
    dummy.width = dummy.height = 1;
    dummy.bind = bind_sampled;
    dummy_ = make_texture(dummy, nullptr);
    const float quad[]{-1, -1, 1, -1, -1, 1, -1, 1, 1, -1, 1, 1};
    BufferDesc qd;
    qd.bytes = sizeof quad;
    qd.usage = Usage::immutable;
    qd.bind = bind_vertices;
    clear_quad_owner_ = std::make_shared<GxmBuffer>(qd);
    clear_quad_ = clear_quad_owner_.get();
    clear_quad_->data = allocate(sizeof quad, false, 16);
    std::memcpy(clear_quad_->data, quad, sizeof quad);
}

// A Cg stage's GXP: the cached file when present and valid, else compiled by SceShaccCg and cached.
CompiledStage& Impl::stage(bool vertex, unsigned index, bool float_out) {
    const std::uint32_t key = (vertex ? 0x10000u : 0u) | (float_out ? 0x8000u : 0u) | index;
    auto found = stages_.find(key);
    if (found != stages_.end()) return found->second;
    const GxmStage& s = vertex ? gxm_vertex_stage(index) : gxm_fragment_stage(index);
    std::string source = float_out ? "#define OMEGA_FLOAT_OUT\n" : "";
    source += s.source;
    char hash[24];
    std::snprintf(hash, sizeof hash, "%016llx", static_cast<unsigned long long>(fnv64(source)));
    const std::string path = options.shader_cache + "/" + s.name + (float_out ? "_f" : "") + "-" + hash + ".gxp";

    CompiledStage c;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        std::fseek(f, 0, SEEK_END);
        const long bytes = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (bytes > 0) {
            c.code.resize((static_cast<std::size_t>(bytes) + 3) / 4);
            if (std::fread(c.code.data(), 1, static_cast<std::size_t>(bytes), f) != static_cast<std::size_t>(bytes) ||
                sceGxmProgramCheck(c.program()) < 0 || sceGxmProgramGetSize(c.program()) > static_cast<unsigned>(bytes))
                c.code.clear();
        }
        std::fclose(f);
        if (!c.code.empty()) ++stats.programs_cached;
    }
    static const bool verify = [] {
        SceIoStat st;
        return sceIoGetstat("ux0:data/omega_strain/debug_verify_cache", &st) >= 0;
    }();
    if (verify && !c.code.empty()) {  // DIAGNOSTIC (temporary)
        if (!compiler_ready_) {
            shark_init(options.compiler.c_str());
            shark_install_log_cb(compile_log);
            shark_set_warnings_level(SHARK_WARN_SILENT);
            compiler_ready_ = true;
        }
        std::uint32_t bytes = static_cast<std::uint32_t>(source.size());
        const auto* program =
            shark_compile_shader(source.c_str(), &bytes, vertex ? SHARK_VERTEX_SHADER : SHARK_FRAGMENT_SHADER);
        const unsigned size = sceGxmProgramGetSize(c.program());
        unsigned first = ~0u;
        if (program)
            for (unsigned k = 0; k < std::min<unsigned>(size, bytes); ++k)
                if (reinterpret_cast<const std::uint8_t*>(program)[k] != reinterpret_cast<const std::uint8_t*>(c.code.data())[k]) {
                    first = k;
                    break;
                }
        if (options.log) {
            char text[160];
            std::snprintf(text, sizeof text, "verify %s: cached %u bytes, compiled %u bytes (program size %u), first difference %d",
                          s.name, size, unsigned(bytes), program ? sceGxmProgramGetSize(program) : 0u, int(first));
            options.log(text);
        }
        if (program) {  // DIAGNOSTIC: use the fresh compiler output (with its padding)
            c.code.assign((bytes + 3) / 4, 0u);
            std::memcpy(c.code.data(), program, bytes);
        }
        shark_clear_output();
    }
    if (c.code.empty()) {
        if (!compiler_ready_) {
            if (shark_init(options.compiler.c_str()) < 0 && shark_init_simple(options.compiler.c_str()) < 0 &&
                shark_init(nullptr) < 0)
                throw ShaderCompilerMissing(
                    "The Cg shader compiler is missing: copy libshacccg.suprx to " + options.compiler +
                    " (research/VITA.md \"Shader compiler\"); it is needed once, to build the shader cache in " +
                    options.shader_cache);
            shark_install_log_cb(compile_log);
            shark_set_warnings_level(SHARK_WARN_SILENT);
            compiler_ready_ = true;
        }
        g_compile_log.clear();
        std::uint32_t bytes = static_cast<std::uint32_t>(source.size());
        const SceGxmProgram* program =
            shark_compile_shader(source.c_str(), &bytes, vertex ? SHARK_VERTEX_SHADER : SHARK_FRAGMENT_SHADER);
        if (!program || !bytes) {
            shark_clear_output();
            throw std::runtime_error("gfx gxm: Cg compile of " + std::string(s.name) + " failed:\n" + g_compile_log);
        }
        c.code.resize((bytes + 3) / 4);
        std::memcpy(c.code.data(), program, bytes);
        shark_clear_output();
        check(sceGxmProgramCheck(c.program()), "sceGxmProgramCheck (compiled GXP)");
        make_directories(options.shader_cache);
        sceIoRemove(path.c_str());  // Vita3K's "wb" open does not truncate an existing file
        if (FILE* f = std::fopen(path.c_str(), "wb")) {
            std::fwrite(c.code.data(), 1, sceGxmProgramGetSize(c.program()), f);  // the compiler's length may include padding
            std::fclose(f);
        }
        ++stats.programs_compiled;
    }
    check(sceGxmShaderPatcherRegisterProgram(patcher_, c.program(), &c.id), "sceGxmShaderPatcherRegisterProgram");
    const SceGxmProgram* p = c.program();
    if (options.log) {
        char text[200];
        std::snprintf(text, sizeof text, "gxm program %s%s: %u bytes, default uniform buffer %u bytes, %u parameters",
                      s.name, float_out ? " (float out)" : "", sceGxmProgramGetSize(p),
                      sceGxmProgramGetDefaultUniformBufferSize(p), sceGxmProgramGetParameterCount(p));
        options.log(text);
        for (unsigned k = 0; k < sceGxmProgramGetParameterCount(p); ++k) {
            const SceGxmProgramParameter* q = sceGxmProgramGetParameter(p, k);
            std::snprintf(text, sizeof text, "  %-16s category %d components %u array %u resource %u container %u",
                          sceGxmProgramParameterGetName(q), static_cast<int>(sceGxmProgramParameterGetCategory(q)),
                          sceGxmProgramParameterGetComponentCount(q), sceGxmProgramParameterGetArraySize(q),
                          sceGxmProgramParameterGetResourceIndex(q), sceGxmProgramParameterGetContainerIndex(q));
            options.log(text);
        }
    }
    if (vertex) {
        const GxmProgram* owner = nullptr;
        for (std::size_t k = 0; k < gxm_program_count; ++k)
            if (gxm_program(static_cast<Program>(k)).vertex == index) owner = &gxm_program(static_cast<Program>(k));
        for (int k = 0; k < 2; ++k)
            if (owner && owner->constants[k]) c.constants[k] = sceGxmProgramFindParameterByName(p, owner->constants[k]);
        c.clear = sceGxmProgramFindParameterByName(p, "omegaClear");
    } else {
        const GxmProgram* owner = nullptr;
        for (std::size_t k = 0; k < gxm_program_count; ++k) {
            const GxmProgram& g = gxm_program(static_cast<Program>(k));
            if (g.fragment == index || g.fragment_alt == index) owner = &g;
        }
        for (int k = 0; k < 2; ++k)
            if (owner && owner->constants[k]) c.constants[k] = sceGxmProgramFindParameterByName(p, owner->constants[k]);
        c.tex_size = sceGxmProgramFindParameterByName(p, "omegaTexSize");
        c.layers = sceGxmProgramFindParameterByName(p, "omegaLayers");
        c.blend_mul = sceGxmProgramFindParameterByName(p, "omegaBlendMul");
        c.colour = sceGxmProgramFindParameterByName(p, "omegaColour");
    }
    return stages_.emplace(key, std::move(c)).first->second;
}

SceGxmVertexProgram* Impl::vertex_program(unsigned index, const GxmAttribute* attributes, unsigned count,
                                          std::uint32_t stride) {
    const std::uint64_t key = (std::uint64_t(index) << 32) | stride;
    auto found = vertex_programs_.find(key);
    if (found != vertex_programs_.end()) return found->second;
    const CompiledStage& s = stage(true, index, false);
    std::vector<SceGxmVertexAttribute> out;
    for (unsigned k = 0; k < count; ++k) {
        const SceGxmProgramParameter* p = sceGxmProgramFindParameterByName(s.program(), attributes[k].name);
        if (!p) continue;  // optimised out by the compiler
        SceGxmVertexAttribute a{};
        a.streamIndex = 0;
        a.offset = attributes[k].offset;
        a.format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
        a.componentCount = attributes[k].components;
        a.regIndex = static_cast<unsigned short>(sceGxmProgramParameterGetResourceIndex(p));
        out.push_back(a);
    }
    SceGxmVertexStream stream{};
    stream.stride = static_cast<unsigned short>(stride);
    stream.indexSource = SCE_GXM_INDEX_SOURCE_INDEX_32BIT;
    SceGxmVertexProgram* program = nullptr;
    check(sceGxmShaderPatcherCreateVertexProgram(patcher_, s.id, out.data(), static_cast<unsigned>(out.size()), &stream,
                                                 1, &program),
          "sceGxmShaderPatcherCreateVertexProgram");
    vertex_programs_.emplace(key, program);
    return program;
}

SceGxmFragmentProgram* Impl::fragment_program(unsigned index, bool float_out, const Pipeline& p, const CompiledStage& vs) {
    std::string key(reinterpret_cast<const char*>(&index), sizeof index);
    key += float_out ? 'f' : 'u';
    key += p.blend_info ? 'b' : 'n';
    key.append(reinterpret_cast<const char*>(&p.blend), sizeof p.blend);
    const void* vp = vs.program();
    key.append(reinterpret_cast<const char*>(&vp), sizeof vp);
    auto found = fragment_programs_.find(key);
    if (found != fragment_programs_.end()) return found->second;
    const CompiledStage& s = stage(false, index, float_out);
    SceGxmFragmentProgram* program = nullptr;
    check(sceGxmShaderPatcherCreateFragmentProgram(
              patcher_, s.id, float_out ? SCE_GXM_OUTPUT_REGISTER_FORMAT_FLOAT : SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
              SCE_GXM_MULTISAMPLE_NONE, p.blend_info ? &p.blend : nullptr, vs.program(), &program),
          "sceGxmShaderPatcherCreateFragmentProgram");
    fragment_programs_.emplace(std::move(key), program);
    return program;
}

// ---- textures ----

void Impl::write_rows(GxmTexture& t, unsigned left, unsigned top, unsigned right, unsigned bottom,
                      const std::uint8_t* data, unsigned pitch) {
    for (unsigned y = top; y < bottom; ++y) {
        std::uint8_t* row = t.data + (std::size_t(y) * t.stride + left) * t.bpp;
        const std::uint8_t* in = data + std::size_t(y - top) * pitch;
        if (t.desc().format == Format::r8) {
            for (unsigned x = 0; x < right - left; ++x) {
                row[4 * x] = in[x];
                row[4 * x + 1] = row[4 * x + 2] = 0;
                row[4 * x + 3] = 0xff;
            }
        } else {
            std::memcpy(row, in, std::size_t(right - left) * t.bpp);
        }
    }
}

std::shared_ptr<GxmTexture> Impl::make_texture(const TextureDesc& desc, const Level* levels) {
    if (desc.samples > 1) throw std::runtime_error("gfx gxm: multisampled textures are unported");
    auto t = std::make_shared<GxmTexture>();
    t->set_desc(desc);
    const unsigned layers = std::max(desc.layers, 1u);
    switch (desc.format) {
    case Format::rgba8: case Format::r8: break;
    case Format::bgra8:
        t->texture_format = SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ARGB;
        t->colour_format = SCE_GXM_COLOR_FORMAT_U8U8U8U8_ARGB;
        break;
    case Format::r32f:
        t->texture_format = SCE_GXM_TEXTURE_FORMAT_F32_R;
        t->colour_format = SCE_GXM_COLOR_FORMAT_F32_R;
        t->float_target = true;
        break;
    case Format::d24s8: t->depth = true; break;
    default: throw std::runtime_error("gfx gxm: unknown texture format");
    }
    if (t->depth) {
        if (layers != 1) throw std::runtime_error("gfx gxm: depth texture arrays are unported");
        t->stride = static_cast<unsigned>(align_up(desc.width, 32));
        t->layer_rows = static_cast<unsigned>(align_up(desc.height, 32));
    } else {
        t->stride = static_cast<unsigned>(align_up(std::max(desc.width, 1u), 8));
        t->layer_rows = std::max(desc.height, 1u);
        if (desc.width > 4096 || t->layer_rows * layers > 4096)
            throw std::runtime_error("gfx gxm: a texture (with its stacked layers) above 4096 texels is unported");
    }
    t->bpp = 4;
    t->data = allocate(t->bytes(), desc.usage != Usage::staging);
    if (levels && levels[0].data && !t->depth)
        write_rows(*t, 0, 0, desc.width, desc.height, static_cast<const std::uint8_t*>(levels[0].data), levels[0].row_pitch);
    else
        std::memset(t->data, 0, t->bytes());
    return t;
}

// The swap chain's buffer is one texture standing for the current back buffer (as D3D11's flip-model buffer 0 does),
// resolved when a list executes: a list recorded on the simulation thread and executed later on the render thread
// (gfx_render_thread.hpp) draws into the buffer current at its execution, and the recorder need not drain to get it.
std::shared_ptr<GxmTexture> Impl::back_buffer() {
    if (!back_proxy_) {
        back_proxy_ = std::make_shared<GxmTexture>(*displays_[0].texture);
        back_proxy_->owned = false;
        back_proxy_->back_proxy = true;
    }
    return back_proxy_;
}

const GxmTexture* Impl::resolve(const void* p) const {
    const auto* t = static_cast<const GxmTexture*>(static_cast<const Texture*>(p));
    return t && t->back_proxy ? displays_[back_].texture.get() : t;
}

// ---- scenes ----

SceGxmRenderTarget* Impl::render_target(unsigned width, unsigned height) {
    const auto key = std::make_pair(width, height);
    auto found = targets_.find(key);
    if (found != targets_.end()) return found->second;
    SceGxmRenderTargetParams params{};
    params.width = static_cast<std::uint16_t>(width);
    params.height = static_cast<std::uint16_t>(height);
    params.scenesPerFrame = 8;
    params.multisampleMode = SCE_GXM_MULTISAMPLE_NONE;
    params.driverMemBlock = -1;
    SceGxmRenderTarget* target = nullptr;
    check(sceGxmCreateRenderTarget(&params, &target), "sceGxmCreateRenderTarget");
    targets_.emplace(key, target);
    return target;
}

void Impl::begin_scene(const GxmTexture* colour, unsigned layer, const GxmTexture* depth) {
    const GxmTexture* size = colour ? colour : depth;
    if (!size) throw std::runtime_error("gfx gxm: a draw with no colour or depth target");
    const unsigned width = size->desc().width, height = colour ? colour->layer_rows : size->desc().height;
    SceGxmColorSurface cs{};
    if (colour) {
        check(sceGxmColorSurfaceInit(&cs, colour->colour_format, SCE_GXM_COLOR_SURFACE_LINEAR,
                                     SCE_GXM_COLOR_SURFACE_SCALE_NONE, SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, width, height,
                                     colour->stride, colour->layer(layer)),
              "sceGxmColorSurfaceInit");
        const_cast<GxmTexture*>(colour)->used_epoch = epoch_;
    }
    SceGxmDepthStencilSurface ds{};
    if (depth) {
        auto* d = const_cast<GxmTexture*>(depth);
        check(sceGxmDepthStencilSurfaceInit(&ds, SCE_GXM_DEPTH_STENCIL_FORMAT_S8D24, SCE_GXM_DEPTH_STENCIL_SURFACE_TILED,
                                            d->stride, d->data, nullptr),
              "sceGxmDepthStencilSurfaceInit");
        sceGxmDepthStencilSurfaceSetForceStoreMode(&ds, SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED);
        if (d->clear_pending) {
            sceGxmDepthStencilSurfaceSetBackgroundDepth(&ds, d->clear_value);
            sceGxmDepthStencilSurfaceSetForceLoadMode(&ds, SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_DISABLED);
            d->clear_pending = false;
        } else {
            sceGxmDepthStencilSurfaceSetForceLoadMode(&ds, SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED);
        }
        d->used_epoch = epoch_;
    }
    SceGxmSyncObject* sync = nullptr;
    if (colour && colour->display >= 0) {
        sync = displays_[colour->display].sync;
    } else if (colour) {
        SceGxmSyncObject*& s = offscreen_syncs_[colour->data];
        if (!s) check(sceGxmSyncObjectCreate(&s), "sceGxmSyncObjectCreate");
        sync = s;
    }
    check(sceGxmBeginScene(context_, 0, render_target(width, height), nullptr, nullptr, sync, colour ? &cs : nullptr,
                           depth ? &ds : nullptr),
          "sceGxmBeginScene");
    // Explicit per-scene state: GXM's context defaults are not relied on (Vita3K's differ between boots).
    sceGxmSetViewportEnable(context_, SCE_GXM_VIEWPORT_ENABLED);
    sceGxmSetTwoSidedEnable(context_, SCE_GXM_TWO_SIDED_DISABLED);
    sceGxmSetFrontFragmentProgramEnable(context_, SCE_GXM_FRAGMENT_PROGRAM_ENABLED);
    sceGxmSetFrontStencilFunc(context_, SCE_GXM_STENCIL_FUNC_ALWAYS, SCE_GXM_STENCIL_OP_KEEP, SCE_GXM_STENCIL_OP_KEEP,
                              SCE_GXM_STENCIL_OP_KEEP, 0, 0);
    sceGxmSetFrontVisibilityTestEnable(context_, SCE_GXM_VISIBILITY_TEST_DISABLED);
    sceGxmSetFrontDepthBias(context_, 0, 0);
    scene_.open = true;
    scene_.colour = colour ? colour->layer(layer) : nullptr;
    scene_.depth = depth ? depth->data : nullptr;
    scene_.float_target = colour && colour->float_target;
    scene_.width = width;
    scene_.height = height;
    ++scenes_;
}

void Impl::end_scene(bool notify) {
    if (!scene_.open) return;
    SceGxmNotification n{};
    n.address = notify_;
    n.value = epoch_;
    check(sceGxmEndScene(context_, nullptr, notify ? &n : nullptr), "sceGxmEndScene");
    scene_.open = false;
}

void Impl::ensure_scene(const GxmTexture* colour, unsigned layer, const GxmTexture* depth) {
    const std::uint8_t* c = colour ? colour->layer(layer) : nullptr;
    const std::uint8_t* d = depth ? depth->data : nullptr;
    if (scene_.open && scene_.colour == c && scene_.depth == d && !(depth && depth->clear_pending)) return;
    end_scene(false);
    begin_scene(colour, layer, depth);
}

// ---- draws ----

void set_uniform(void* buffer, const SceGxmProgramParameter* p, const void* data, std::size_t floats) {
    if (!p || !data || !floats) return;
    const std::size_t room =
        std::size_t(std::max(sceGxmProgramParameterGetArraySize(p), 1u)) * sceGxmProgramParameterGetComponentCount(p);
    sceGxmSetUniformDataF(buffer, p, 0, static_cast<unsigned>(std::min(floats, room)), static_cast<const float*>(data));
}

void Impl::execute(Pipeline& p) {
    const CompiledStage& vs = stage(true, p.vertex, false);
    const CompiledStage& fs = stage(false, p.fragment, scene_.float_target);
    SceGxmVertexProgram* vp = vertex_program(p.vertex, p.attributes, p.attribute_count, p.stride);
    SceGxmFragmentProgram* fp = fragment_program(p.fragment, scene_.float_target, p, vs);
    sceGxmSetVertexProgram(context_, vp);
    sceGxmSetFragmentProgram(context_, fp);

    const Viewport& v = p.viewport;
    sceGxmSetViewport(context_, v.x + v.width * 0.5F, v.width * 0.5F, v.y + v.height * 0.5F, -v.height * 0.5F,
                      v.min_depth, v.max_depth - v.min_depth);
    // Region clipping is tile-granular on GXM; D3D clips to the viewport exactly (VITA.md).
    const int x0 = std::clamp(static_cast<int>(v.x), 0, static_cast<int>(scene_.width) - 1);
    const int y0 = std::clamp(static_cast<int>(v.y), 0, static_cast<int>(scene_.height) - 1);
    const int x1 = std::clamp(static_cast<int>(std::ceil(v.x + v.width)) - 1, x0, static_cast<int>(scene_.width) - 1);
    const int y1 = std::clamp(static_cast<int>(std::ceil(v.y + v.height)) - 1, y0, static_cast<int>(scene_.height) - 1);
    sceGxmSetRegionClip(context_, SCE_GXM_REGION_CLIP_OUTSIDE, static_cast<unsigned>(x0), static_cast<unsigned>(y0),
                        static_cast<unsigned>(x1), static_cast<unsigned>(y1));
    sceGxmSetCullMode(context_, SCE_GXM_CULL_NONE);
    sceGxmSetFrontPolygonMode(context_, p.wireframe ? SCE_GXM_POLYGON_MODE_TRIANGLE_LINE : SCE_GXM_POLYGON_MODE_TRIANGLE_FILL);
    sceGxmSetFrontDepthFunc(context_, scene_.depth ? p.depth_func : SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetFrontDepthWriteEnable(context_, scene_.depth && p.depth_write ? SCE_GXM_DEPTH_WRITE_ENABLED
                                                                           : SCE_GXM_DEPTH_WRITE_DISABLED);

    float tex_size[16];
    float layers[4];
    for (unsigned k = 0; k < 4; ++k) {
        const GxmTexture* t = p.textures[k].texture ? p.textures[k].texture : dummy_.get();
        if (t->depth) throw std::runtime_error("gfx gxm: sampling a depth texture is unported");
        const unsigned layer_count = std::max(t->desc().layers, 1u);
        SceGxmTexture g{};
        check(sceGxmTextureInitLinear(&g, t->data, t->texture_format, std::max(t->desc().width, 1u),
                                      t->layer_rows * layer_count, 1),
              "sceGxmTextureInitLinear");
        const SamplerDesc& s = p.textures[k].sampler;
        const bool point_min = s.filter != Filter::linear, point_mag = s.filter == Filter::point;
        sceGxmTextureSetMinFilter(&g, point_min ? SCE_GXM_TEXTURE_FILTER_POINT : SCE_GXM_TEXTURE_FILTER_LINEAR);
        sceGxmTextureSetMagFilter(&g, point_mag ? SCE_GXM_TEXTURE_FILTER_POINT : SCE_GXM_TEXTURE_FILTER_LINEAR);
        const SceGxmTextureAddrMode a = s.address == Address::wrap ? SCE_GXM_TEXTURE_ADDR_REPEAT : SCE_GXM_TEXTURE_ADDR_CLAMP;
        sceGxmTextureSetUAddrMode(&g, a);
        sceGxmTextureSetVAddrMode(&g, a);
        sceGxmSetFragmentTexture(context_, k, &g);
        const_cast<GxmTexture*>(t)->used_epoch = epoch_;
        const float w = static_cast<float>(std::max(t->desc().width, 1u)), h = static_cast<float>(t->layer_rows);
        tex_size[4 * k] = w;
        tex_size[4 * k + 1] = h;
        tex_size[4 * k + 2] = 1.0F / w;
        tex_size[4 * k + 3] = 1.0F / h;
        layers[k] = static_cast<float>(layer_count);
    }

    if (sceGxmProgramGetDefaultUniformBufferSize(vs.program())) {
        void* buffer = nullptr;
        check(sceGxmReserveVertexDefaultUniformBuffer(context_, &buffer), "sceGxmReserveVertexDefaultUniformBuffer");
        for (int k = 0; k < 2; ++k)
            set_uniform(buffer, vs.constants[k], p.vertex_constants[k].data, p.vertex_constants[k].bytes / 4);
        set_uniform(buffer, vs.clear, p.clear, 4);
    }
    if (sceGxmProgramGetDefaultUniformBufferSize(fs.program())) {
        void* buffer = nullptr;
        check(sceGxmReserveFragmentDefaultUniformBuffer(context_, &buffer), "sceGxmReserveFragmentDefaultUniformBuffer");
        for (int k = 0; k < 2; ++k)
            set_uniform(buffer, fs.constants[k], p.fragment_constants[k].data, p.fragment_constants[k].bytes / 4);
        set_uniform(buffer, fs.tex_size, tex_size, 16);
        set_uniform(buffer, fs.layers, layers, 4);
        set_uniform(buffer, fs.blend_mul, p.blend_mul, 4);
        set_uniform(buffer, fs.colour, p.colour, 4);
    }
    sceGxmSetVertexStream(context_, 0, p.stream);
    check(sceGxmDraw(context_, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U32, p.indices, p.count), "sceGxmDraw");
    ++draws_;
    if (options.log && logged_draws_ < 24) {
        ++logged_draws_;
        char text[240];
        std::snprintf(text, sizeof text,
                      "gxm draw %u: vs %u fs %u count %u viewport %.0f,%.0f %.0fx%.0f scene %ux%u colour %p depth %p "
                      "uniforms vs %u fs %u colour %p blend_mul %p",
                      logged_draws_, p.vertex, p.fragment, p.count, v.x, v.y, v.width, v.height, scene_.width,
                      scene_.height, static_cast<const void*>(scene_.colour), static_cast<const void*>(scene_.depth),
                      sceGxmProgramGetDefaultUniformBufferSize(vs.program()),
                      sceGxmProgramGetDefaultUniformBufferSize(fs.program()), static_cast<const void*>(fs.colour),
                      static_cast<const void*>(fs.blend_mul));
        options.log(text);
    }
}

const std::uint32_t* Impl::sequence(std::uint32_t count) {
    if (count > sequence_count_) {
        const std::uint32_t n = std::max<std::uint32_t>(count, std::max<std::uint32_t>(sequence_count_ * 2, 4096));
        auto* s = reinterpret_cast<std::uint32_t*>(allocate(std::size_t(n) * 4, false, 16));
        for (std::uint32_t k = 0; k < n; ++k) s[k] = k;
        release(sequence_);
        sequence_ = s;
        sequence_count_ = n;
    }
    return sequence_;
}

void Impl::draw(bool indexed, std::uint32_t count, std::uint32_t first) {
    if (!state_.has_program) throw std::runtime_error("gfx gxm: a draw with no program");
    if (!count) return;
    const GxmProgram& g = gxm_program(state_.program);
    const GxmLayout& layout = gxm_layout(program_layout(state_.program));
    if (!state_.vertices || !state_.vertices->data) throw std::runtime_error("gfx gxm: a draw with no vertex buffer");
    Pipeline p;
    p.vertex = g.vertex;
    p.attributes = layout.attributes;
    p.attribute_count = layout.count;
    p.stride = state_.stride;
    const std::uint8_t* base = state_.vertices->data + state_.offset;
    const_cast<GxmBuffer*>(state_.vertices)->used_epoch = epoch_;
    if (indexed) {
        if (!state_.indices || !state_.indices->data) throw std::runtime_error("gfx gxm: an indexed draw with no index buffer");
        const_cast<GxmBuffer*>(state_.indices)->used_epoch = epoch_;
        p.stream = base;
        p.indices = reinterpret_cast<const std::uint32_t*>(state_.indices->data) + first;
    } else {
        p.stream = base + std::size_t(first) * state_.stride;
        p.indices = sequence(count);
    }
    p.count = count;
    if (g.cpu_geometry) {
        const GxmBuffer* shadow = state_.geometry_constants[1] ? state_.geometry_constants[1] : state_.vertex_constants[1];
        if (!shadow || shadow->cpu.size() < 112) throw std::runtime_error("gfx gxm: shadow_receiver without Shadow constants");
        ShadowFilter filter;
        std::memcpy(filter.clip, shadow->cpu.data(), sizeof filter.clip);
        std::memcpy(filter.facing, shadow->cpu.data() + 96, sizeof filter.facing);
        std::vector<std::uint32_t> kept;
        kept.reserve(count);
        const auto* vertices = static_cast<const std::uint8_t*>(p.stream);
        for (std::uint32_t k = 0; k + 2 < count; k += 3) {
            const std::uint32_t i0 = p.indices[k], i1 = p.indices[k + 1], i2 = p.indices[k + 2];
            if (filter.keep(vertices + std::size_t(i0) * p.stride, vertices + std::size_t(i1) * p.stride,
                            vertices + std::size_t(i2) * p.stride)) {
                kept.push_back(i0);
                kept.push_back(i1);
                kept.push_back(i2);
            }
        }
        if (kept.empty()) return;
        auto* list = reinterpret_cast<std::uint32_t*>(allocate(kept.size() * 4, false, 16));
        std::memcpy(list, kept.data(), kept.size() * 4);
        release(list);
        transient_ += kept.size() * 4;
        p.indices = list;
        p.count = static_cast<std::uint32_t>(kept.size());
    }
    const DepthDesc depth = state_.depth_state ? state_.depth_state->desc : DepthDesc{true, true, Compare::less};
    p.depth_func = depth.test ? depth_func(depth.compare) : SCE_GXM_DEPTH_FUNC_ALWAYS;
    p.depth_write = depth.test && depth.write;
    p.wireframe = state_.raster && state_.raster->desc.wireframe;
    p.viewport = state_.viewport;
    for (unsigned k = 0; k < 4; ++k) {
        p.textures[k].texture = state_.textures[k];
        const GxmSampler* s = state_.samplers[g.samplers[k]];
        p.textures[k].sampler = s ? s->desc : SamplerDesc{};
    }
    for (int k = 0; k < 2; ++k) {
        if (const GxmBuffer* b = state_.vertex_constants[k]) p.vertex_constants[k] = {b->cpu.data(), b->cpu.size()};
        if (const GxmBuffer* b = state_.pixel_constants[k]) p.fragment_constants[k] = {b->cpu.data(), b->cpu.size()};
    }
    const BlendDesc blend = state_.blend ? state_.blend->desc : BlendDesc{};
    // particle_haze's two targets: GXM has one colour surface per scene, so SV_Target0 and SV_Target1 are two
    // fragment programs drawn in turn (VITA.md).
    const bool second = g.fragment_alt != ~0U && state_.colour[1];
    for (unsigned pass = 0; pass < (second ? 2u : 1u); ++pass) {
        if (!state_.colour[pass] && !state_.depth) return;
        ensure_scene(state_.colour[pass], state_.layer[pass], state_.depth);
        p.fragment = pass ? g.fragment_alt : g.fragment;
        blend_for(blend.targets[pass && blend.independent ? 1 : 0], state_.factor, p);
        execute(p);
    }
}

// A colour clear (ClearRenderTargetView / ClearView rectangles): quads drawn with the clear program.
void Impl::clear_colour(const GxmTexture* target, unsigned layer, const float rgba[4], const Rect* rects, unsigned count) {
    if (!target) return;
    if (!(scene_.open && scene_.colour == target->layer(layer))) ensure_scene(target, layer, nullptr);
    Pipeline p;
    p.vertex = gxm_clear_vertex;
    p.fragment = gxm_clear_fragment;
    p.attributes = clear_attributes;
    p.attribute_count = 1;
    p.stride = 8;
    p.stream = clear_quad_->data;
    p.indices = sequence(6);
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
        execute(p);
    }
}

// A depth clear: drawn into the open scene that uses this depth buffer, else the next scene starts from the value.
void Impl::clear_depth(const GxmTexture* depth, float value) {
    if (!depth) return;
    if (!(scene_.open && scene_.depth == depth->data)) {
        auto* d = const_cast<GxmTexture*>(depth);
        d->clear_pending = true;
        d->clear_value = value;
        return;
    }
    Pipeline p;
    p.vertex = gxm_clear_vertex;
    p.fragment = gxm_clear_fragment;
    p.attributes = clear_attributes;
    p.attribute_count = 1;
    p.stride = 8;
    p.stream = clear_quad_->data;
    p.indices = sequence(6);
    p.count = 6;
    p.blend_info = true;
    p.blend.colorMask = SCE_GXM_COLOR_MASK_NONE;
    p.blend.colorFunc = p.blend.alphaFunc = SCE_GXM_BLEND_FUNC_NONE;
    p.blend.colorSrc = p.blend.alphaSrc = SCE_GXM_BLEND_FACTOR_ONE;
    p.blend.colorDst = p.blend.alphaDst = SCE_GXM_BLEND_FACTOR_ZERO;
    p.depth_func = SCE_GXM_DEPTH_FUNC_ALWAYS;
    p.depth_write = true;
    p.clear[0] = value;
    p.viewport = {0, 0, float(scene_.width), float(scene_.height), 0, 1};
    execute(p);
}

// CopyResource / CopySubresourceRegion: a point-sampled quad drawn into the destination (formats convert through
// the shader, so the A8B8G8R8 display buffers copy into B8G8R8A8 staging textures as D3D's do).
void Impl::copy_region(const GxmTexture* dst, unsigned dst_sub, unsigned x, unsigned y, const GxmTexture* src,
                       unsigned src_sub, const Box* box) {
    if (!dst || !src) return;
    if (dst->depth || src->depth) throw std::runtime_error("gfx gxm: depth copies are unported");
    const unsigned dm = std::max(dst->desc().mips, 1u), sm = std::max(src->desc().mips, 1u);
    if (dst_sub % dm || src_sub % sm) throw std::runtime_error("gfx gxm: copies of mip levels above 0 are unported");
    const unsigned dl = dst_sub / dm, sl = src_sub / sm;
    const Box b = box ? *box : Box{0, 0, 0, src->desc().width, src->layer_rows, 1};
    if (b.right <= b.left || b.bottom <= b.top) return;
    if (!(scene_.open && scene_.colour == dst->layer(dl))) ensure_scene(dst, dl, nullptr);
    const float sw = float(std::max(src->desc().width, 1u));
    const float sh = float(src->layer_rows) * float(std::max(src->desc().layers, 1u));
    const float u0 = float(b.left) / sw, u1 = float(b.right) / sw;
    const float v0 = (float(sl * src->layer_rows) + float(b.top)) / sh;
    const float v1 = (float(sl * src->layer_rows) + float(b.bottom)) / sh;
    const float quad[]{-1, -1, u0, v1, 1, -1, u1, v1, -1, 1, u0, v0, -1, 1, u0, v0, 1, -1, u1, v1, 1, 1, u1, v0};
    auto* vertices = allocate(sizeof quad, false, 16);
    std::memcpy(vertices, quad, sizeof quad);
    release(vertices);
    transient_ += sizeof quad;
    Pipeline p;
    p.vertex = gxm_copy_vertex;
    p.fragment = gxm_copy_fragment;
    p.attributes = copy_attributes;
    p.attribute_count = 2;
    p.stride = 16;
    p.stream = vertices;
    p.indices = sequence(6);
    p.count = 6;
    p.viewport = {float(x), float(y), float(b.right - b.left), float(b.bottom - b.top), 0, 1};
    p.textures[0].texture = src;
    p.textures[0].sampler.filter = Filter::point;
    p.textures[0].sampler.address = Address::clamp;
    execute(p);
}

// ---- CPU updates and read-back ----

void Impl::update_buffer(GxmBuffer& b, const void* data, std::size_t bytes, std::size_t offset, bool keep_rest) {
    const std::size_t size = b.desc().bytes;
    if (offset > size || bytes > size - offset) throw std::runtime_error("gfx gxm: a buffer update past its end");
    if (b.desc().bind & bind_constants) {
        if (b.cpu.size() < size) b.cpu.resize(size);
        if (bytes) std::memcpy(b.cpu.data() + offset, data, bytes);
        return;
    }
    if (!b.data) b.data = allocate(size, false, 16);
    if (!idle(b.used_epoch)) {
        // The GPU may still read the old bytes: new memory, the old block freed when its epoch completes.
        std::uint8_t* fresh = allocate(size, false, 16);
        if (keep_rest) std::memcpy(fresh, b.data, size);
        release(b.data);
        b.data = fresh;
        b.used_epoch = 0;
        transient_ += size;
    }
    if (bytes) std::memcpy(b.data + offset, data, bytes);
}

void Impl::update_texture(GxmTexture& t, const Box& box, const std::uint8_t* data, unsigned pitch) {
    if (t.depth) throw std::runtime_error("gfx gxm: depth texture updates are unported");
    const unsigned right = std::min(box.right, t.desc().width), bottom = std::min(box.bottom, t.layer_rows);
    if (right <= box.left || bottom <= box.top) return;
    if (!idle(t.used_epoch) && t.owned) {
        if (scene_.open && scene_.colour >= t.data && scene_.colour < t.data + t.bytes()) end_scene(false);
        std::uint8_t* fresh = allocate(t.bytes(), t.desc().usage != Usage::staging);
        const bool whole = box.left == 0 && box.top == 0 && right == t.desc().width && bottom == t.layer_rows &&
                           std::max(t.desc().layers, 1u) == 1;
        if (!whole) std::memcpy(fresh, t.data, t.bytes());
        release(t.data);
        t.data = fresh;
        t.used_epoch = 0;
        transient_ += t.bytes();
    }
    write_rows(t, box.left, box.top, right, bottom, data, pitch);
}

void Impl::read(const GxmTexture& t, unsigned sub,
                const std::function<void(const std::uint8_t* data, unsigned row_pitch)>& reader) {
    if (t.depth) throw std::runtime_error("gfx gxm: depth read-back is unported");
    const unsigned mips = std::max(t.desc().mips, 1u);
    if (sub % mips) throw std::runtime_error("gfx gxm: read-back of mip levels above 0 is unported");
    drain();
    const std::uint8_t* base = t.layer(sub / mips);
    if (t.desc().format == Format::r8) {
        std::vector<std::uint8_t> bytes(std::size_t(t.desc().width) * t.layer_rows);
        for (unsigned y = 0; y < t.layer_rows; ++y)
            for (unsigned x = 0; x < t.desc().width; ++x)
                bytes[std::size_t(y) * t.desc().width + x] = base[(std::size_t(y) * t.stride + x) * 4];
        reader(bytes.data(), t.desc().width);
        return;
    }
    reader(base, t.stride * t.bpp);
}

// ---- command lists ----

const GxmBuffer* buffer_of(const void* p) { return static_cast<const GxmBuffer*>(static_cast<const Buffer*>(p)); }

void Impl::submit(CommandList& list) {
    for (const Command& c : list.commands()) {
        switch (c.op) {
        case Command::Op::set_targets:
            for (unsigned k = 0; k < 2; ++k) {
                state_.colour[k] = k < c.count ? resolve(c.objects[k]) : nullptr;
                state_.layer[k] = k < c.count ? c.layers[k] : 0;
            }
            state_.depth = resolve(c.objects[2]);
            break;
        case Command::Op::set_viewport:
            state_.viewport = {c.values[0], c.values[1], c.values[2], c.values[3], c.values[4], c.values[5]};
            break;
        case Command::Op::clear_target:
            clear_colour(resolve(c.objects[0]), c.layers[0], c.values, nullptr, 0);
            break;
        case Command::Op::clear_depth:
            if (c.flags & CommandList::clear_depth_bit) clear_depth(resolve(c.objects[0]), c.values[0]);
            break;
        case Command::Op::clear_rects: {
            Rect rects[2]{};
            std::memcpy(rects, list.arena(c.c), sizeof(Rect) * std::min<unsigned>(c.count, 2));
            clear_colour(resolve(c.objects[0]), 0, c.values, rects, std::min<unsigned>(c.count, 2));
            break;
        }
        case Command::Op::set_raster: state_.raster = static_cast<const GxmRaster*>(static_cast<const RasterState*>(c.objects[0])); break;
        case Command::Op::set_depth: state_.depth_state = static_cast<const GxmDepth*>(static_cast<const DepthState*>(c.objects[0])); break;
        case Command::Op::set_blend:
            state_.blend = static_cast<const GxmBlend*>(static_cast<const BlendState*>(c.objects[0]));
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
                const GxmBuffer* b = buffer_of(c.objects[k]);
                if (c.flags & stage_vertex) state_.vertex_constants[c.slot + k] = b;
                if (c.flags & stage_geometry) state_.geometry_constants[c.slot + k] = b;
                if (c.flags & stage_pixel) state_.pixel_constants[c.slot + k] = b;
            }
            break;
        case Command::Op::set_textures:
            for (unsigned k = 0; k < c.count && c.slot + k < 4; ++k) state_.textures[c.slot + k] = resolve(c.objects[k]);
            break;
        case Command::Op::set_sampler:
            if (c.slot < 4) state_.samplers[c.slot] = static_cast<const GxmSampler*>(static_cast<const SamplerState*>(c.objects[0]));
            break;
        case Command::Op::update_buffer:
            update_buffer(*const_cast<GxmBuffer*>(buffer_of(c.objects[0])), list.arena(c.a), c.b, c.c, (c.flags & 1) != 0);
            break;
        case Command::Op::update_texture: {
            Box box;
            std::memcpy(&box, list.arena(c.d), sizeof box);
            update_texture(*const_cast<GxmTexture*>(resolve(c.objects[0])), box, list.arena(c.a), c.b);
            break;
        }
        case Command::Op::draw: draw(false, c.a, c.b); break;
        case Command::Op::draw_indexed: draw(true, c.a, c.b); break;
        case Command::Op::copy: {
            const GxmTexture* dst = resolve(c.objects[0]);
            const GxmTexture* src = resolve(c.objects[1]);
            const unsigned layers = std::max(src->desc().layers, 1u);
            for (unsigned l = 0; l < layers; ++l)
                copy_region(dst, l * std::max(dst->desc().mips, 1u), 0, 0, src, l * std::max(src->desc().mips, 1u), nullptr);
            break;
        }
        case Command::Op::copy_region: {
            Box box;
            if (c.flags & 1) std::memcpy(&box, list.arena(c.slot), sizeof box);
            copy_region(resolve(c.objects[0]), c.a, c.b, c.c, resolve(c.objects[1]), c.d, (c.flags & 1) ? &box : nullptr);
            break;
        }
        case Command::Op::flush: break;  // GXM kicks scenes at sceGxmEndScene
        case Command::Op::reset_state: state_ = State{}; break;
        }
    }
    list.clear();
}

// Ends the frame's last scene on the back buffer (its notification marks the epoch complete) and queues the flip.
SwapChain::Presented Impl::present(unsigned sync_interval) {
    ensure_scene(displays_[back_].texture.get(), 0, nullptr);
    end_scene(true);
    DisplayRequest request{displays_[back_].block.base, sync_interval > 0 ? 1 : 0};
    check(sceGxmDisplayQueueAddEntry(displays_[front_].sync, displays_[back_].sync, &request),
          "sceGxmDisplayQueueAddEntry");
    front_ = back_;
    back_ = (back_ + 1) % static_cast<unsigned>(displays_.size());
    ++epoch_;
    collect();
    // At most two frames in flight (the deferred frees and the transient copies wait on them).
    while (epoch_ - 1 - completed_ > 2) {
        sceKernelDelayThread(100);
        collect();
    }
    stats.scenes_last_frame = scenes_;
    stats.draws_last_frame = draws_;
    stats.transient_bytes = transient_;
    scenes_ = draws_ = 0;
    transient_ = 0;
    return SwapChain::Presented::ok;
}

// ---- the API objects ----

void Impl::message(const std::string& text) {
    drain();
    TextureDesc dd;
    dd.width = display_width;
    dd.height = display_height;
    dd.format = Format::d24s8;
    dd.bind = bind_depth;
    const auto depth = make_texture(dd, nullptr);
    SceMsgDialogUserMessageParam user{};
    user.buttonType = SCE_MSG_DIALOG_BUTTON_TYPE_OK;
    user.msg = reinterpret_cast<const SceChar8*>(text.c_str());
    SceMsgDialogParam param;
    sceMsgDialogParamInit(&param);
    param.mode = SCE_MSG_DIALOG_MODE_USER_MSG;
    param.userMsgParam = &user;
    if (sceMsgDialogInit(&param) < 0) return;
    while (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) {
        DisplayBuffer& b = displays_[back_];
        std::memset(b.block.base, 0, std::size_t(display_width) * display_height * 4);
        SceCommonDialogUpdateParam update{};
        update.renderTarget.colorFormat = SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR;
        update.renderTarget.surfaceType = SCE_GXM_COLOR_SURFACE_LINEAR;
        update.renderTarget.colorSurfaceData = b.block.base;
        update.renderTarget.depthSurfaceData = depth->data;
        update.renderTarget.width = display_width;
        update.renderTarget.height = display_height;
        update.renderTarget.strideInPixels = display_width;
        update.displaySyncObject = b.sync;
        sceCommonDialogUpdate(&update);
        DisplayRequest request{b.block.base, 1};
        sceGxmDisplayQueueAddEntry(displays_[front_].sync, b.sync, &request);
        front_ = back_;
        back_ = (back_ + 1) % static_cast<unsigned>(displays_.size());
    }
    sceMsgDialogTerm();
    sceGxmDisplayQueueFinish();
}

class GxmDevice final : public Device {
public:
    explicit GxmDevice(Impl& impl) : impl_(impl) {}
    const std::string& name() const override { return name_; }
    std::shared_ptr<Buffer> create_buffer(const BufferDesc& desc, const void* initial, const char*) override {
        std::lock_guard<std::recursive_mutex> lock(impl_.mutex);
        auto b = std::make_shared<GxmBuffer>(desc);
        if (desc.bind & bind_constants) {
            b->cpu.assign(align_up(std::max<std::size_t>(desc.bytes, 16), 16), 0);
            if (initial) std::memcpy(b->cpu.data(), initial, desc.bytes);
        } else {
            b->data = impl_.allocate(std::max<std::size_t>(desc.bytes, 16), false, 16);
            if (initial) std::memcpy(b->data, initial, desc.bytes);
            else std::memset(b->data, 0, desc.bytes);
        }
        return b;
    }
    std::shared_ptr<Texture> create_texture(const TextureDesc& desc, const Level* levels, const TextureLabels&) override {
        std::lock_guard<std::recursive_mutex> lock(impl_.mutex);
        return impl_.make_texture(desc, levels);
    }
    std::shared_ptr<BlendState> create_blend(const BlendDesc& desc, const char*) override {
        auto s = std::make_shared<GxmBlend>();
        s->desc = desc;
        return s;
    }
    std::shared_ptr<DepthState> create_depth(const DepthDesc& desc, const char*) override {
        auto s = std::make_shared<GxmDepth>();
        s->desc = desc;
        return s;
    }
    std::shared_ptr<RasterState> create_raster(const RasterDesc& desc, const char*) override {
        auto s = std::make_shared<GxmRaster>();
        s->desc = desc;
        return s;
    }
    std::shared_ptr<SamplerState> create_sampler(const SamplerDesc& desc, const char*) override {
        auto s = std::make_shared<GxmSampler>();
        s->desc = desc;
        return s;
    }
    void prepare(Program program) override {
        std::lock_guard<std::recursive_mutex> lock(impl_.mutex);
        const GxmProgram& g = gxm_program(program);
        impl_.stage(true, g.vertex, false);
        impl_.stage(false, g.fragment, false);
        if (g.fragment_alt != ~0U) impl_.stage(false, g.fragment_alt, false);
    }
    void submit(CommandList& list) override {
        std::lock_guard<std::recursive_mutex> lock(impl_.mutex);
        impl_.submit(list);
    }
    void write(const Buffer& buffer, const void* data, std::size_t bytes, const char*) override {
        std::lock_guard<std::recursive_mutex> lock(impl_.mutex);
        impl_.update_buffer(const_cast<GxmBuffer&>(static_cast<const GxmBuffer&>(buffer)), data, bytes, 0, false);
    }
    void read(const Texture& staging, unsigned subresource,
              const std::function<void(const std::uint8_t* data, unsigned row_pitch)>& reader, const char*) override {
        std::lock_guard<std::recursive_mutex> lock(impl_.mutex);
        impl_.read(*impl_.resolve(&staging), subresource, reader);
    }
    bool removed() const override { return false; }
    bool wait_idle() override {
        std::lock_guard<std::recursive_mutex> lock(impl_.mutex);
        impl_.drain();
        return true;
    }

private:
    Impl& impl_;
    std::string name_ = "GXM (SceGxm, PS Vita)";
};

class GxmSwapChain final : public SwapChain {
public:
    explicit GxmSwapChain(Impl& impl) : impl_(impl) {}
    void size(unsigned& width, unsigned& height, const char*) override {
        width = display_width;
        height = display_height;
    }
    std::shared_ptr<Texture> buffer(bool, const char*, const char*) override {
        std::lock_guard<std::recursive_mutex> lock(impl_.mutex);
        return impl_.back_buffer();
    }
    void resize(unsigned, unsigned, const char*) override {}  // the Vita display is fixed at 960x544
    Presented present(unsigned sync_interval, const char*) override {
        std::lock_guard<std::recursive_mutex> lock(impl_.mutex);
        return impl_.present(sync_interval);
    }

private:
    Impl& impl_;
};

std::mutex g_create_mutex;
std::shared_ptr<GxmDevice> g_device;
std::shared_ptr<GxmSwapChain> g_swap_chain;

void ensure_created(const Options& options) {
    std::lock_guard<std::mutex> lock(g_create_mutex);
    if (g_impl) return;
    g_impl = new Impl(options);  // one per process (sceGxmInitialize); kept for the process lifetime
    g_device = std::make_shared<GxmDevice>(*g_impl);
    g_swap_chain = std::make_shared<GxmSwapChain>(*g_impl);
}

}  // namespace

Display create_display(const Options& options) {
    ensure_created(options);
    Display d;
    d.device = g_device;
    d.swap_chain = g_swap_chain;
    d.backend = "GXM";
    return d;
}

MemoryStats memory_stats() {
    if (!g_impl) return {};
    std::lock_guard<std::recursive_mutex> lock(g_impl->mutex);
    return g_impl->snapshot();
}

void show_message(const std::string& text) {
    if (!g_impl) return;
    std::lock_guard<std::recursive_mutex> lock(g_impl->mutex);
    g_impl->message(text);
}

}  // namespace omega::gfx::gxm

namespace omega::gfx {

// The GXM device has no separate offscreen device: the GUI renderer's captures share the one context.
std::shared_ptr<Device> create_offscreen_device(const char*) {
    gxm::ensure_created(gxm::Options{});
    return gxm::g_device;
}

}  // namespace omega::gfx

