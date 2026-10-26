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
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/message_dialog.h>
#include <vitashark.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
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
void forget_buffer(const void* b);  // 132vg2: the shadow filter's cached triangles of a buffer being destroyed

struct GxmBuffer final : Buffer {
    explicit GxmBuffer(const BufferDesc& d) { desc_ = d; }
    ~GxmBuffer() override {
        forget_buffer(this);
        release_memory(data);
        for (const auto& r : rebased) release_memory(r.second);
        for (const Patch& p : patches) {
            release_memory(p.data);
            for (const auto& s : p.spares) release_memory(s.first);
        }
    }
    std::vector<std::uint8_t> cpu;  // constant buffers: copied into the default uniform buffers per draw
    std::uint8_t* data = nullptr;   // vertex / index buffers (LPDDR)
    std::uint32_t used_epoch = 0;
    // 132vg2 (research/VITA.md "Vita render path (132vg2)"): a large buffer's partial updates while the GPU may read
    // it (the level mesh's pathed models, every frame) go to patches: fresh memory holding the byte range
    // [begin, end) that supersedes the buffer's own bytes there. A draw whose vertices all lie in one patch reads it
    // (its stream address moved by the patch's offset); the buffer is never rewritten under the GPU and no
    // sceGxmFinish is needed. Sorted, disjoint.
    struct Patch {
        std::size_t begin = 0, end = 0;
        std::uint8_t* data = nullptr;
        mutable std::uint32_t used_epoch = 0;  // the last epoch a draw read it (0: not yet: writable in place)
        // the same bytes in cached RAM: the CPU reads them (the next patch's fill, the shadow filter) instead of the
        // uncached GPU copy
        std::shared_ptr<std::vector<std::uint8_t>> cpu;
        // step 3b: earlier GPU copies of the patch, each with the epoch that last read it, reused once the GPU is
        // done with them (a rotation instead of a new allocation per frame)
        std::vector<std::pair<std::uint8_t*, std::uint32_t>> spares;
    };
    std::vector<Patch> patches;
    // 132vg2: index buffers: the vertex index runs of each (first, count) range drawn (read once from the buffer,
    // dropped when it is written), for the patch test.
    std::unordered_map<std::uint64_t, std::vector<std::pair<std::uint32_t, std::uint32_t>>> index_ranges;
    // 132vg2: index buffers: the ranges drawn from a patch, rebased to its first vertex (GPU memory, kept until the
    // buffer is written or the cache is full): key (first, count, first vertex).
    struct RebasedKey {
        std::uint32_t first, count, base;
        bool operator==(const RebasedKey& o) const { return first == o.first && count == o.count && base == o.base; }
    };
    struct RebasedHash {
        std::size_t operator()(const RebasedKey& k) const {
            return std::size_t((std::uint64_t(k.first) * 0x9E3779B97F4A7C15ULL) ^ (std::uint64_t(k.count) << 20) ^
                               (std::uint64_t(k.base) * 0xC2B2AE3D27D4EB4FULL));
        }
    };
    std::unordered_map<RebasedKey, std::uint32_t*, RebasedHash> rebased;
    // 132vg2 step 3d: counts the writes to the buffer (an index buffer's, in the key below).
    std::uint32_t serial = 0;
    // 132vg2 step 3d: vertex buffers: the vertex colour alpha range [lo, hi] (lo > hi: unusable) of each range drawn
    // from the buffer's own bytes (not a patch), read once; cleared when those bytes are written (alpha_passes_all).
    struct AlphaKey {
        const void* indices;
        std::uint32_t serial, first, count, offset, stride;
        bool operator==(const AlphaKey& o) const {
            return indices == o.indices && serial == o.serial && first == o.first && count == o.count &&
                   offset == o.offset && stride == o.stride;
        }
    };
    struct AlphaHash {
        std::size_t operator()(const AlphaKey& k) const {
            return std::size_t((std::uint64_t(k.first) * 0x9E3779B97F4A7C15ULL) ^ (std::uint64_t(k.count) << 21) ^
                               (std::uint64_t(k.offset) * 0xC2B2AE3D27D4EB4FULL) ^ std::uint64_t(k.stride) ^
                               (reinterpret_cast<std::uintptr_t>(k.indices) >> 4) ^ (std::uint64_t(k.serial) << 40));
        }
    };
    std::unordered_map<AlphaKey, std::pair<float, float>, AlphaHash> alpha_ranges;
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
    // 132vg2: stored in GXM's swizzled layout (a sampled-only texture with power-of-two sides of 16 or more, when the
    // start-up self test confirmed the layout; research/VITA.md "Vita render path (132vg2)"). Render targets stay
    // linear, as GXM requires.
    bool swizzled = false;
    // 132vg2: the GXM texture descriptor last built for this texture (its memory and the sampler it was built for).
    mutable SceGxmTexture descriptor{};
    mutable const std::uint8_t* descriptor_data = nullptr;
    mutable std::uint8_t descriptor_sampler = 0xff;
    // 132vg2 step 3d: the range of the stored texels' alpha bytes (exact after a whole write, widened by partial
    // writes); unknown once the GPU renders or copies into the texture.
    bool alpha_known = false;
    std::uint8_t alpha_lo = 0, alpha_hi = 255;
    bool alpha_binary = false;  // step 3g (census): every texel's alpha is 0 or alpha_hi (a whole write)

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

// ---- 132vg2: swizzled textures ----

// The texel order of SCE_GXM_TEXTURE_SWIZZLED: the low log2(min(w, h)) bits of x and y interleaved (Morton order),
// the larger side's remaining bits above them. Which coordinate takes bit 0 is picked by the start-up self test
// (Impl::swizzle_self_test: 0 x, 1 y); -1 keeps every texture linear (the test failed, or has not run).
int g_swizzle_order = -1;
std::uint32_t spread_bits(std::uint32_t v) {  // the low 16 bits of v into the even bits
    v &= 0xffffu;
    v = (v | (v << 8)) & 0x00ff00ffu;
    v = (v | (v << 4)) & 0x0f0f0f0fu;
    v = (v | (v << 2)) & 0x33333333u;
    v = (v | (v << 1)) & 0x55555555u;
    return v;
}
std::size_t swizzle_index(unsigned x, unsigned y, unsigned width, unsigned height) {
    const unsigned m = std::min(width, height);
    const unsigned lx = x & (m - 1), ly = y & (m - 1);
    std::size_t i = g_swizzle_order == 1 ? (spread_bits(ly) | (spread_bits(lx) << 1))
                                         : (spread_bits(lx) | (spread_bits(ly) << 1));
    if (width > height) i += std::size_t(x / m) * m * m;
    else if (height > width) i += std::size_t(y / m) * m * m;
    return i;
}
bool power_of_two(unsigned v) { return v && !(v & (v - 1)); }

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
    // 132vg2: each uniform's room in floats (array size x components; 0 when the program has none) and whether the
    // program has a default uniform buffer, computed once instead of per draw.
    unsigned room_constants[2]{}, room_tex_size = 0, room_layers = 0, room_blend_mul = 0, room_colour = 0, room_clear = 0;
    bool uniform_buffer = false;
    const SceGxmProgram* program() const { return reinterpret_cast<const SceGxmProgram*>(code.data()); }
};
unsigned uniform_room(const SceGxmProgramParameter* p) {
    return p ? std::max(sceGxmProgramParameterGetArraySize(p), 1u) * sceGxmProgramParameterGetComponentCount(p) : 0u;
}

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
    bool no_alpha_test = false;  // 132vg2: scene_ps's variant without the alpha test (no discard)
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
    bool faces(const float* n) const { return facing[0] * n[0] + facing[1] * n[1] + facing[2] * n[2] < 0.0F; }
    // 132vg2: one triangle as 12 floats (the 3 positions, then the closing vertex's normal), from the cached copy.
    bool keep_floats(const float* t) const {
        if (outside(t) || outside(t + 3) || outside(t + 6)) return false;
        return faces(t + 9);
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

// 132vg2: the shadow receivers' triangles in cached RAM (research/VITA.md "Vita render path (132vg2)"): per
// receiver draw (vertex buffer, offset, stride, index buffer, first, count), each triangle's 3 positions and its
// closing vertex's normal (12 floats) and its 3 indices, copied at the draw's first use from the GPU buffers
// (uncached memory, slow to read on the CPU); dropped when a write touches its vertices or indices.
struct ShadowTriangles {
    const void* vertices = nullptr;
    const void* indices = nullptr;
    std::uint32_t offset = 0, stride = 0, first = 0, count = 0;
    std::vector<float> floats;
    std::vector<std::uint32_t> triangle_indices;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> runs;  // the vertex indices used
    std::size_t bytes() const { return floats.size() * 4 + triangle_indices.size() * 4 + runs.size() * 8; }
};
// 132vg2: a receiver range's triangle indices and exact vertex runs (static while its index buffer is not written).
struct ShadowIndices {
    const void* indices = nullptr;
    std::uint32_t first = 0, count = 0;
    std::vector<std::uint32_t> triangle_indices;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> runs;
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

// ---- 132vg2: timing (port-only diagnostics; research/VITA.md "Vita render path (132vg2)") ----

// The process clock (microseconds): about 10 times cheaper than std::chrono::steady_clock on the handheld (VITA.md
// "Profiling", the clock).
inline std::uint64_t now_us() { return sceKernelGetProcessTimeWide(); }

// Each scene's completion: a fragment notification in its own slot of the notification region (slots
// notify_slot0 .. +notify_slots, written with the scene's serial; slot 0 keeps the present's epoch), waited for by a
// thread (Impl::gpu_wait_loop) that timestamps it.
constexpr unsigned notify_slot0 = 64, notify_slots = 256, scene_ring = 1024;
struct SceneRecord {
    std::uint32_t serial = 0;
    std::uint16_t width = 0, height = 0;
    std::uint8_t kind = 0;    // GxmTimers::GpuKind
    std::uint8_t depth = 0;   // 0 none, 1 cleared at the start, 2 loaded
    std::uint32_t draws = 0, clears = 0, copies = 0;
    std::uint64_t begin_us = 0, kick_us = 0;
    std::uint32_t notify_index = 0, notify_value = 0;
    std::atomic<std::uint64_t> done_us{0};
};

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
        char text[200];
        std::snprintf(text, sizeof text,
                      "gfx gxm: out of GPU memory (%u bytes; pools CDRAM %u / %u KB used, LPDDR %u / %u KB used; "
                      "VITA.md \"Memory\")",
                      static_cast<unsigned>(bytes), unsigned(cdram_.used() >> 10), unsigned(cdram_.size() >> 10),
                      unsigned(lpddr_.used() >> 10), unsigned(lpddr_.size() >> 10));
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
        const std::uint64_t t0 = now_us();
        end_scene(false);
        sceGxmFinish(context_);
        completed_ = epoch_;
        ++epoch_;
        collect();
        ++stats.gxm.drains;
        stats.gxm.drain_us += now_us() - t0;
    }
    bool idle(std::uint32_t used) const { return used == 0 || used <= completed_; }
    std::uint32_t epoch() const { return epoch_; }

    // ---- programs ----
    CompiledStage& stage(bool vertex, unsigned index, bool float_out, bool variant = false);
    unsigned scene_fragment_ = ~0U;  // 132vg2: scene_ps's stage index (the alpha-test-free variant)
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
        s.gxm.gpu_scenes = gpu_scenes_.load();
        s.gxm.gpu_busy_us = gpu_busy_.load();
        s.gxm.gpu_resyncs = gpu_resyncs_.load();
        for (unsigned k = 0; k < GxmTimers::gpu_kinds; ++k) {
            s.gxm.gpu_kind_scenes[k] = gpu_kind_scenes_[k].load();
            s.gxm.gpu_kind_us[k] = gpu_kind_us_[k].load();
        }
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
    std::unordered_map<std::uint64_t, SceGxmFragmentProgram*> fragment_programs_;
    // 132vg2 per-draw cuts: the stages by (vertex, float output, index) for indices below 32 (std::map nodes do not
    // move), and the last vertex / fragment program found.
    static constexpr unsigned stage_cache_size = 32;
    CompiledStage* stage_cache_[2][2][2][stage_cache_size]{};
    std::uint64_t last_vp_key_ = ~0ULL, last_fp_key_ = ~0ULL;
    SceGxmVertexProgram* last_vp_ = nullptr;
    SceGxmFragmentProgram* last_fp_ = nullptr;
    // 132vg2: the context state set in the open scene (sceGxmSet* calls whose value is unchanged are skipped);
    // invalidated at each begin_scene.
    struct ContextCache {
        SceGxmVertexProgram* vp = nullptr;
        SceGxmFragmentProgram* fp = nullptr;
        bool viewport_set = false;
        float viewport[6]{};
        bool clip_set = false;
        unsigned clip[4]{};
        int polygon = -1, depth_func = -1, depth_write = -1;
        bool texture_set[4]{};
        SceGxmTexture texture[4]{};
    } ctx_;
    std::vector<std::uint32_t> shadow_kept_;  // 132vg2: the shadow filter's scratch list (keeps its capacity)
    std::unordered_map<std::uint64_t, ShadowTriangles> shadow_cache_;  // 132vg2
    std::unordered_map<std::uint64_t, ShadowIndices> shadow_indices_;  // 132vg2
    ShadowTriangles shadow_scratch_;                                   // 132vg2: a patched receiver's, per draw
    std::size_t shadow_cache_bytes_ = 0;
    unsigned logged_folds_ = 0, logged_shadow_ = 0, logged_shadow_drops_ = 0;
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
    unsigned large_waits_ = 0;

    // ---- 132vg2: timing ----
public:
    void draw_body(bool indexed, std::uint32_t count, std::uint32_t first);
    const ShadowIndices& shadow_index_entry(const Pipeline& p, bool indexed, std::uint32_t first, std::uint32_t count);
    std::vector<std::uint8_t> shadow_flags_;  // 132vg2 step 3e: the per-vertex flags of a patched receiver (scratch)
    unsigned shadow_flag_checks_ = 0, shadow_flag_mismatches_ = 0;  // step 3e: vita_test_shadow_flags_check.txt
    std::vector<float> probe_constants_;  // step 3f: the GPU probe's copy of a draw's Scene constants
    float last_vlo_ = 0.0F, last_vhi_ = 0.0F;  // step 3g: draw_alpha_range's last vertex alpha range
    void census_blended(const Pipeline& p, const GxmLayout& layout, bool indexed, std::uint32_t first,
                        std::uint32_t count, bool patched);
    bool alpha_passes_all(const float* scene, const GxmLayout& layout, bool indexed, std::uint32_t first,
                          std::uint32_t count);
    bool blend_is_noop(const float* scene, const GxmLayout& layout, bool indexed, std::uint32_t first,
                       std::uint32_t count, const Pipeline& p);
    bool alpha_discards_all(const float* scene, const GxmLayout& layout, bool indexed, std::uint32_t first,
                            std::uint32_t count);
    bool draw_alpha_range(const float* scene, const GxmLayout& layout, bool indexed, std::uint32_t first,
                          std::uint32_t count, float& amin_out, float& amax_out,
                          const std::uint8_t* patched_stream = nullptr);
    const std::uint8_t* proof_stream_ = nullptr;  // step 3g: the patched draw's cached-RAM vertices (or null)
    const ShadowTriangles& shadow_triangles(const Pipeline& p, bool indexed, std::uint32_t first, std::uint32_t count,
                                            bool scratch);
    void forget_shadow(const GxmBuffer* b, std::size_t begin, std::size_t end);
    const std::uint32_t* rebased_indices(const GxmBuffer& ib, std::uint32_t first, std::uint32_t count,
                                         std::uint32_t base_vertex);
    void patch_update(GxmBuffer& b, const std::uint8_t* data, std::size_t bytes, std::size_t offset,
                      std::size_t widen_lo = ~std::size_t(0), std::size_t widen_hi = 0);
    const std::vector<std::pair<std::uint32_t, std::uint32_t>>& index_runs(const GxmBuffer& ib, std::uint32_t first,
                                                                          std::uint32_t count);
    const GxmBuffer::Patch* patched_target(GxmBuffer& b, std::uint32_t offset, std::uint32_t stride,
                                           const std::pair<std::uint32_t, std::uint32_t>* runs, std::size_t run_count);
    void gpu_wait_loop();
    std::string scene_dump();
    void swizzle_self_test();

private:
    std::unique_ptr<SceneRecord[]> records_{new SceneRecord[scene_ring]};
    SceneRecord* record_ = nullptr;          // the open scene's
    std::uint32_t serial_ = 0;               // the last scene begun
    std::atomic<std::uint32_t> submitted_{0};  // the last scene ended (its record complete)
    SceUID gpu_sema_ = -1, gpu_thread_ = -1;
    bool sample_ = false;                    // this draw is one of the sampled 1 in 8
    std::uint32_t draw_calls_ = 0;
    struct FrameRange {
        std::uint32_t first = 0, last = 0, epoch = 0;
    };
    FrameRange frames_[4]{};
    std::uint32_t frame_first_ = 1, presents_ = 0;
    std::atomic<std::uint64_t> gpu_scenes_{0}, gpu_busy_{0}, gpu_resyncs_{0};
    std::atomic<std::uint64_t> gpu_kind_scenes_[GxmTimers::gpu_kinds]{}, gpu_kind_us_[GxmTimers::gpu_kinds]{};
};

int gpu_waiter_entry(SceSize, void* argp) {
    (*static_cast<Impl**>(argp))->gpu_wait_loop();
    return 0;
}

void release_memory(void* p) {
    if (p && g_impl) {
        std::lock_guard<std::recursive_mutex> lock(g_impl->mutex);
        g_impl->release(p);
    }
}

void forget_buffer(const void* b) {
    if (b && g_impl) {
        std::lock_guard<std::recursive_mutex> lock(g_impl->mutex);
        g_impl->forget_shadow(static_cast<const GxmBuffer*>(b), 0, ~std::size_t(0));
    }
}

// scratch: the vertices are a patch's cached-RAM copy, rewritten every frame (TORONTO1's receivers are the pathed
// models): the floats are gathered into a scratch entry each draw (cheap from cached RAM) and only the triangles'
// indices and vertex runs are kept (shadow_indices_, until the index buffer is written).
const ShadowIndices& Impl::shadow_index_entry(const Pipeline& p, bool indexed, std::uint32_t first,
                                              std::uint32_t count) {
    const void* indices = indexed ? static_cast<const void*>(state_.indices) : nullptr;
    // the triangles' indices and exact vertex runs (sorted once per range)
    std::uint64_t index_key = reinterpret_cast<std::uintptr_t>(indices);
    for (const std::uint64_t v : {std::uint64_t(first), std::uint64_t(count)})
        index_key = (index_key ^ v) * 0x100000001b3ULL + 0x9e3779b97f4a7c15ULL;
    auto ix = shadow_indices_.find(index_key);
    if (ix == shadow_indices_.end() || ix->second.indices != indices || ix->second.first != first ||
        ix->second.count != count) {
        ShadowIndices made;
        made.indices = indices;
        made.first = first;
        made.count = count;
        made.triangle_indices.assign(p.indices, p.indices + std::size_t(count / 3) * 3);
        std::vector<std::uint32_t> v(made.triangle_indices);
        if (!indexed)
            for (std::uint32_t& i : v) i += first;
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
        for (const std::uint32_t i : v) {
            if (!made.runs.empty() && made.runs.back().second + 1 == i) made.runs.back().second = i;
            else made.runs.emplace_back(i, i);
        }
        if (shadow_indices_.size() > 4096) shadow_indices_.clear();
        ix = shadow_indices_.insert_or_assign(index_key, std::move(made)).first;
    }
    return ix->second;
}

const ShadowTriangles& Impl::shadow_triangles(const Pipeline& p, bool indexed, std::uint32_t first,
                                              std::uint32_t count, bool scratch) {
    const void* indices = indexed ? static_cast<const void*>(state_.indices) : nullptr;
    std::uint64_t key = reinterpret_cast<std::uintptr_t>(state_.vertices);
    for (const std::uint64_t v : {std::uint64_t(reinterpret_cast<std::uintptr_t>(indices)), std::uint64_t(state_.offset),
                                  std::uint64_t(state_.stride), std::uint64_t(first), std::uint64_t(count)})
        key = (key ^ v) * 0x100000001b3ULL + 0x9e3779b97f4a7c15ULL;
    const ShadowIndices* ixe = &shadow_index_entry(p, indexed, first, count);
    if (scratch) {
        ShadowTriangles& s = shadow_scratch_;
        s.vertices = state_.vertices;
        s.indices = indices;
        s.offset = state_.offset;
        s.stride = state_.stride;
        s.first = first;
        s.count = count;
        s.triangle_indices = ixe->triangle_indices;
        s.runs.clear();
        const auto* vertices = static_cast<const std::uint8_t*>(p.stream);
        const std::size_t triangles = s.triangle_indices.size() / 3;
        s.floats.resize(triangles * 12);
        for (std::size_t t = 0; t < triangles; ++t) {
            float* out = &s.floats[t * 12];
            for (int v = 0; v < 3; ++v)
                std::memcpy(out + 3 * v, vertices + std::size_t(s.triangle_indices[t * 3 + v]) * p.stride, 12);
            std::memcpy(out + 9, vertices + std::size_t(s.triangle_indices[t * 3 + 2]) * p.stride + 12, 12);
        }
        return s;
    }
    auto found = shadow_cache_.find(key);
    if (found != shadow_cache_.end()) {
        const ShadowTriangles& s = found->second;
        if (s.vertices == state_.vertices && s.indices == indices && s.offset == state_.offset &&
            s.stride == state_.stride && s.first == first && s.count == count) {
            stats.shadow_cached += s.triangle_indices.size() / 3;
            return s;
        }
        shadow_cache_bytes_ -= s.bytes();
        shadow_cache_.erase(found);
    }
    if (logged_shadow_ < 12 && options.log) {
        ++logged_shadow_;
        char text[200];
        std::snprintf(text, sizeof text, "gfx gxm: shadow triangles copied: vertex buffer %p (%u KB), indices %p, first %u, "
                      "count %u, cache %u KB", static_cast<const void*>(state_.vertices),
                      unsigned(state_.vertices->desc().bytes >> 10), indices, first, count,
                      unsigned(shadow_cache_bytes_ >> 10));
        options.log(text);
    }
    if (shadow_cache_bytes_ > (16u << 20)) {  // a cap: start again
        shadow_cache_.clear();
        shadow_cache_bytes_ = 0;
    }
    ShadowTriangles s;
    s.vertices = state_.vertices;
    s.indices = indices;
    s.offset = state_.offset;
    s.stride = state_.stride;
    s.first = first;
    s.count = count;
    s.runs = ixe->runs;  // the exact vertex runs, for forget_shadow's binary search
    const auto* vertices = static_cast<const std::uint8_t*>(p.stream);
    const std::uint32_t triangles = count / 3;
    s.floats.resize(std::size_t(triangles) * 12);
    s.triangle_indices = ixe->triangle_indices;
    for (std::uint32_t t = 0; t < triangles; ++t) {
        float* out = &s.floats[std::size_t(t) * 12];
        for (int v = 0; v < 3; ++v)
            std::memcpy(out + 3 * v, vertices + std::size_t(s.triangle_indices[std::size_t(t) * 3 + v]) * p.stride, 12);
        std::memcpy(out + 9, vertices + std::size_t(s.triangle_indices[std::size_t(t) * 3 + 2]) * p.stride + 12, 12);
    }
    shadow_cache_bytes_ += s.bytes();
    return shadow_cache_.emplace(key, std::move(s)).first->second;
}

// 132vg2: drops the cached triangles that read the bytes [begin, end) of buffer b (as vertices or as indices).
void Impl::forget_shadow(const GxmBuffer* b, std::size_t begin, std::size_t end) {
    for (auto it = shadow_indices_.begin(); it != shadow_indices_.end();) {
        if (it->second.indices == b) it = shadow_indices_.erase(it);
        else ++it;
    }
    for (auto it = shadow_cache_.begin(); it != shadow_cache_.end();) {
        const ShadowTriangles& s = it->second;
        bool drop = s.indices == b;
        if (!drop && s.vertices == b && !s.runs.empty()) {
            // the exact runs, sorted: the first run ending after `begin`
            const auto r = std::partition_point(s.runs.begin(), s.runs.end(), [&](const std::pair<std::uint32_t, std::uint32_t>& x) {
                return s.offset + (std::size_t(x.second) + 1) * s.stride <= begin;
            });
            drop = r != s.runs.end() && s.offset + std::size_t(r->first) * s.stride < end;
        }
        if (drop) {
            if (logged_shadow_drops_ < 8 && options.log) {
                ++logged_shadow_drops_;
                char text[200];
                std::snprintf(text, sizeof text, "gfx gxm: shadow triangles dropped (first %u count %u): a write to %p bytes "
                              "%u-%u (%s)", s.first, s.count, static_cast<const void*>(b), unsigned(begin),
                              unsigned(std::min<std::size_t>(end, 0xffffffffu)), s.indices == b ? "its indices" : "its vertices");
                options.log(text);
            }
            shadow_cache_bytes_ -= s.bytes();
            it = shadow_cache_.erase(it);
        } else {
            ++it;
        }
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

    // The pools: the requested size, capped at what the kernel has free less a headroom (other users of the memory
    // type), then 4 MB smaller until the kernel grants it (VITA.md "Memory"). Halving left 42 MB of CDRAM unused on
    // the handheld and a level's 35 MB mesh buffer fit in neither pool.
    SceKernelFreeMemorySizeInfo free_info{};
    free_info.size = sizeof free_info;
    sceKernelGetFreeMemorySize(&free_info);
    const auto pool_size = [](std::size_t requested, std::size_t free_bytes) {
        constexpr std::size_t headroom = 8u << 20;
        const std::size_t room = free_bytes > headroom ? free_bytes - headroom : 0;
        return std::min(requested, room) & ~std::size_t((1u << 20) - 1);
    };
    for (std::size_t bytes = pool_size(o.cdram_pool_bytes, std::size_t(free_info.size_cdram));
         bytes >= (8u << 20) && !cdram_.create("omega-gxm-cdram", cdram, bytes); bytes -= 4u << 20) {}
    for (std::size_t bytes = pool_size(o.lpddr_pool_bytes, std::size_t(free_info.size_user));
         bytes >= (8u << 20) && !lpddr_.create("omega-gxm-lpddr", lpddr, bytes); bytes -= 4u << 20) {}
    if (!lpddr_.size()) fail("allocate the LPDDR pool", -1);
    if (o.log) {
        char text[200];
        std::snprintf(text, sizeof text, "gfx gxm: pools CDRAM %u MB (requested %u, kernel free %u), LPDDR %u MB "
                      "(requested %u, kernel free %u)", unsigned(cdram_.size() >> 20), unsigned(o.cdram_pool_bytes >> 20),
                      unsigned(free_info.size_cdram >> 20), unsigned(lpddr_.size() >> 20),
                      unsigned(o.lpddr_pool_bytes >> 20), unsigned(free_info.size_user >> 20));
        o.log(text);
    }

    for (std::size_t k = 0; k < gxm_program_count; ++k) {  // 132vg2
        const unsigned fragment = gxm_program(static_cast<Program>(k)).fragment;
        if (fragment != ~0U && std::strcmp(gxm_fragment_stage(fragment).name, "scene_ps") == 0) scene_fragment_ = fragment;
    }
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

    // 132vg2: the GPU timing's waiter (high priority, any core; it sleeps in sceGxmNotificationWait).
    for (unsigned k = 0; k < notify_slots; ++k) notify_[notify_slot0 + k] = 0;
    if (o.gpu_timing) {
        gpu_sema_ = sceKernelCreateSema("omega-gxm-gpu", 0, 0, 0x7fffffff, nullptr);
        if (gpu_sema_ >= 0) gpu_thread_ = sceKernelCreateThread("omega-gxm-gpu", gpu_waiter_entry, 0x40, 0x4000, 0, 0, nullptr);
        if (gpu_thread_ >= 0) {
            Impl* self = this;
            if (sceKernelStartThread(gpu_thread_, sizeof self, &self) < 0) gpu_thread_ = -1;
        }
        if (gpu_thread_ < 0 && o.log) o.log("gfx gxm: GPU timing off (the waiter thread did not start)");
    }
}

// 132vg2: timestamps each scene's fragment notification in order (the scenes the render thread has ended: one
// semaphore count each). A scene's GPU time runs from its kick (sceGxmEndScene's return) or the previous scene's end,
// whichever is later, to its notification: it overestimates when the waiter wakes late (a busy core).
void Impl::gpu_wait_loop() {
    std::uint32_t next = 1;
    std::uint64_t last_done = 0;
    for (;;) {
        if (sceKernelWaitSema(gpu_sema_, 1, nullptr) < 0) return;
        const std::uint32_t submitted = submitted_.load(std::memory_order_acquire);
        if (next > submitted) continue;  // counts left from a resync
        if (submitted - next >= notify_slots * 3 / 4) {
            // Fallen behind: a slot may already hold a later serial (its wait would never end). Restart at the newest.
            next = submitted;
            ++gpu_resyncs_;
        }
        SceneRecord& r = records_[next % scene_ring];
        if (r.serial == next) {
            volatile unsigned int* slot = notify_ + r.notify_index;
            if (*slot != r.notify_value) {
                SceGxmNotification n{};
                n.address = slot;
                n.value = r.notify_value;
                sceGxmNotificationWait(&n);
            }
            const std::uint64_t done = now_us();
            r.done_us.store(done, std::memory_order_release);
            const std::uint64_t start = std::max(last_done, r.kick_us);
            const std::uint64_t busy = done > start ? done - start : 0;
            last_done = done;
            gpu_busy_ += busy;
            ++gpu_scenes_;
            gpu_kind_us_[r.kind] += busy;
            ++gpu_kind_scenes_[r.kind];
        }
        ++next;
    }
}

// 132vg2: picks the swizzled layout's bit order on this GPU (or keeps every texture linear). For each order a
// texture of distinct texels is uploaded swizzled (a square, a wide and a tall one), copied by the GPU with a
// point-sampled quad into a linear target and read back; the order whose read-back equals the texels is kept.
void Impl::swizzle_self_test() {
    const unsigned sizes[3][2]{{16, 16}, {64, 16}, {16, 64}};
    // order -2: the control (the source linear, as every texture before): a read-back that differs there means the
    // copy cannot be read back here (Vita3K with surface sync off), so the test is inconclusive.
    for (const int order : {-2, 0, 1}) {
        g_swizzle_order = order == -2 ? -1 : order;
        bool ok = true;
        std::string seen;
        for (const auto& size : sizes) {
            const unsigned w = size[0], h = size[1];
            std::vector<std::uint32_t> texels(std::size_t(w) * h);
            for (unsigned y = 0; y < h; ++y)
                for (unsigned x = 0; x < w; ++x)
                    texels[std::size_t(y) * w + x] = 0xff000000u | (((x * 7 + y * 13) & 0xffu) << 16) | (y << 8) | x;
            TextureDesc sd;
            sd.width = w;
            sd.height = h;
            sd.usage = Usage::immutable;
            sd.bind = bind_sampled;
            const Level level{texels.data(), w * 4};
            const auto source = make_texture(sd, &level);
            TextureDesc td = sd;
            td.usage = Usage::gpu;
            td.bind = bind_target | bind_sampled;
            const auto target = make_texture(td, nullptr);
            if (source->swizzled != (order >= 0) || target->swizzled) {
                ok = false;
                break;
            }
            copy_region(target.get(), 0, 0, 0, source.get(), 0, nullptr);
            read(*target, 0, [&](const std::uint8_t* data, unsigned pitch) {
                for (unsigned y = 0; y < h && ok; ++y)
                    if (std::memcmp(data + std::size_t(y) * pitch, &texels[std::size_t(y) * w], std::size_t(w) * 4) != 0)
                        ok = false;
                if (ok) return;
                // Where a few texels came from (the source texel (x, y) is encoded in its red / green bytes): enough
                // to work out this GPU's layout from the log.
                char text[64];
                std::snprintf(text, sizeof text, " %ux%u:", w, h);
                seen += text;
                const unsigned probe[][2]{{1, 0}, {2, 0}, {4, 0}, {8, 0}, {0, 1}, {0, 2}, {0, 4}, {0, 8}, {15, 15}, {w - 1, 0}, {0, h - 1}};
                for (const auto& at : probe) {
                    std::uint32_t got;
                    std::memcpy(&got, data + std::size_t(at[1]) * pitch + std::size_t(at[0]) * 4, 4);
                    std::snprintf(text, sizeof text, " (%u,%u)<-%08x", at[0], at[1], got);
                    seen += text;
                }
            });
            if (!ok) break;
        }
        if (order == -2) {
            if (!ok) {
                g_swizzle_order = -1;
                if (options.log)
                    options.log("gfx gxm: swizzled textures off (the self test cannot read back the GPU's copy here:" +
                                seen + ")");
                return;
            }
            continue;
        }
        if (!ok && options.log)
            options.log(std::string("gfx gxm: swizzle self test, ") + (order ? "y" : "x") + " in the low bit: differs;" + seen);
        if (ok) {
            if (options.log)
                options.log(std::string("gfx gxm: swizzled textures on (self test passed, ") +
                            (order ? "y" : "x") + " in the low bit)");
            return;
        }
    }
    g_swizzle_order = -1;
    if (options.log) options.log("gfx gxm: swizzled textures off (the self test's read-back differed): textures stay linear");
}

// 132vg2: the frame presented two frames ago (its GPU work is normally done), one scene a line.
std::string Impl::scene_dump() {
    if (presents_ < 3) return {};
    const FrameRange& f = frames_[(presents_ - 3) % 4];
    if (!f.first || f.last < f.first || f.last - f.first >= scene_ring / 2) return {};
    std::string out;
    char line[200];
    std::snprintf(line, sizeof line, "epoch %u, %u scenes", f.epoch, f.last - f.first + 1);
    out += line;
    std::uint64_t prev_done = 0;
    if (f.first > 1) {
        const SceneRecord& p = records_[(f.first - 1) % scene_ring];
        if (p.serial == f.first - 1) prev_done = p.done_us.load();
    }
    static const char* const depth_names[]{"no depth", "depth clear", "depth load"};
    for (std::uint32_t s = f.first; s <= f.last; ++s) {
        const SceneRecord& r = records_[s % scene_ring];
        if (r.serial != s) break;
        const std::uint64_t done = r.done_us.load();
        const std::uint64_t start = std::max(prev_done, r.kick_us);
        char gpu[32];
        if (done) std::snprintf(gpu, sizeof gpu, "%.2f", done > start ? double(done - start) / 1000.0 : 0.0);
        else std::snprintf(gpu, sizeof gpu, "?");
        std::snprintf(line, sizeof line, "\n  %u: %s %ux%u, %s: %u draws, %u clears, %u copies; CPU open %.2f ms, GPU %s ms",
                      s - f.first + 1, gpu_kind_name(r.kind), r.width, r.height, depth_names[std::min<unsigned>(r.depth, 2)],
                      r.draws, r.clears, r.copies, double(r.kick_us - r.begin_us) / 1000.0, gpu);
        out += line;
        if (done) prev_done = done;
    }
    return out;
}

// A Cg stage's GXP: the cached file when present and valid, else compiled by SceShaccCg and cached.
// variant (132vg2): scene_ps compiled with OMEGA_NO_ALPHA_TEST (no discard), for the draws without an alpha test.
CompiledStage& Impl::stage(bool vertex, unsigned index, bool float_out, bool variant) {
    if (index < stage_cache_size)
        if (CompiledStage* cached = stage_cache_[variant][vertex][float_out][index]) return *cached;
    const std::uint32_t key = (vertex ? 0x10000u : 0u) | (float_out ? 0x8000u : 0u) | (variant ? 0x4000u : 0u) | index;
    auto found = stages_.find(key);
    if (found != stages_.end()) {
        if (index < stage_cache_size) stage_cache_[variant][vertex][float_out][index] = &found->second;
        return found->second;
    }
    const GxmStage& s = vertex ? gxm_vertex_stage(index) : gxm_fragment_stage(index);
    std::string source = float_out ? "#define OMEGA_FLOAT_OUT\n" : "";
    if (variant) source += "#define OMEGA_NO_ALPHA_TEST\n";
    source += s.source;
    char hash[24];
    std::snprintf(hash, sizeof hash, "%016llx", static_cast<unsigned long long>(fnv64(source)));
    const std::string path = options.shader_cache + "/" + s.name + (float_out ? "_f" : "") + (variant ? "_na" : "") +
                             "-" + hash + ".gxp";

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
        if (options.log) options.log(std::string("gfx gxm: compiling ") + s.name + (float_out ? " (float out)" : ""));
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
        std::snprintf(text, sizeof text, "gxm program %s%s%s: %u bytes, default uniform buffer %u bytes, %u parameters, "
                      "discard %s", s.name, float_out ? " (float out)" : "", variant ? " (no alpha test)" : "",
                      sceGxmProgramGetSize(p), sceGxmProgramGetDefaultUniformBufferSize(p),
                      sceGxmProgramGetParameterCount(p), sceGxmProgramIsDiscardUsed(p) ? "yes" : "no");
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
    for (int k = 0; k < 2; ++k) c.room_constants[k] = uniform_room(c.constants[k]);
    c.room_tex_size = uniform_room(c.tex_size);
    c.room_layers = uniform_room(c.layers);
    c.room_blend_mul = uniform_room(c.blend_mul);
    c.room_colour = uniform_room(c.colour);
    c.room_clear = uniform_room(c.clear);
    c.uniform_buffer = sceGxmProgramGetDefaultUniformBufferSize(p) != 0;
    CompiledStage& stored = stages_.emplace(key, std::move(c)).first->second;
    if (index < stage_cache_size) stage_cache_[variant][vertex][float_out][index] = &stored;
    return stored;
}

SceGxmVertexProgram* Impl::vertex_program(unsigned index, const GxmAttribute* attributes, unsigned count,
                                          std::uint32_t stride) {
    const std::uint64_t key = (std::uint64_t(index) << 32) | stride;
    if (key == last_vp_key_) return last_vp_;
    auto found = vertex_programs_.find(key);
    if (found != vertex_programs_.end()) {
        last_vp_key_ = key;
        last_vp_ = found->second;
        return found->second;
    }
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
    last_vp_key_ = key;
    last_vp_ = program;
    return program;
}

// 132vg2: the key is a POD word (was a std::string built every call): the blend info's 4 bytes, the fragment and
// vertex stage indices (the vertex stage's GXP is what the patcher links against, one per index), the output format
// and whether blending is given.
static_assert(sizeof(SceGxmBlendInfo) == 4, "the fragment program key packs the blend info into 32 bits");
SceGxmFragmentProgram* Impl::fragment_program(unsigned index, bool float_out, const Pipeline& p, const CompiledStage& vs) {
    if (index > 0xff || p.vertex > 0xff) throw std::runtime_error("gfx gxm: a stage index above 255");
    std::uint32_t blend_bits = 0;
    std::memcpy(&blend_bits, &p.blend, sizeof blend_bits);
    const std::uint64_t key = std::uint64_t(blend_bits) | (std::uint64_t(index) << 32) | (std::uint64_t(p.vertex) << 40) |
                              (std::uint64_t(float_out) << 48) | (std::uint64_t(p.blend_info) << 49) |
                              (std::uint64_t(p.no_alpha_test) << 50);
    if (key == last_fp_key_) {
        ++stats.gxm.program_cache_hits;
        return last_fp_;
    }
    auto found = fragment_programs_.find(key);
    if (found != fragment_programs_.end()) {
        last_fp_key_ = key;
        last_fp_ = found->second;
        return found->second;
    }
    const CompiledStage& s = stage(false, index, float_out, p.no_alpha_test);
    SceGxmFragmentProgram* program = nullptr;
    check(sceGxmShaderPatcherCreateFragmentProgram(
              patcher_, s.id, float_out ? SCE_GXM_OUTPUT_REGISTER_FORMAT_FLOAT : SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
              SCE_GXM_MULTISAMPLE_NONE, p.blend_info ? &p.blend : nullptr, vs.program(), &program),
          "sceGxmShaderPatcherCreateFragmentProgram");
    fragment_programs_.emplace(key, program);
    last_fp_key_ = key;
    last_fp_ = program;
    return program;
}

// ---- textures ----

void Impl::write_rows(GxmTexture& t, unsigned left, unsigned top, unsigned right, unsigned bottom,
                      const std::uint8_t* data, unsigned pitch) {
    {
        // 132vg2 step 3d: the alpha range of the texels written (alpha_passes_all)
        std::uint8_t lo = 255, hi = 0;
        bool binary = true;
        if (t.desc().format == Format::r8 || (t.desc().format != Format::rgba8 && t.desc().format != Format::bgra8)) {
            lo = hi = 255;
            if (t.desc().format != Format::r8) {
                lo = 0;  // (no 8-bit alpha: never used by the proof)
                binary = false;
            }
        } else {
            for (unsigned y = top; y < bottom; ++y) {
                const std::uint8_t* in = data + std::size_t(y - top) * pitch + 3;
                for (unsigned x = left; x < right; ++x, in += 4) {
                    lo = std::min(lo, *in);
                    hi = std::max(hi, *in);
                }
            }
            for (unsigned y = top; y < bottom && binary; ++y) {
                const std::uint8_t* in = data + std::size_t(y - top) * pitch + 3;
                for (unsigned x = left; x < right; ++x, in += 4)
                    if (*in != 0 && *in != hi) {
                        binary = false;
                        break;
                    }
            }
        }
        t.alpha_binary = binary && left == 0 && top == 0 && right >= t.desc().width &&
                         bottom >= t.layer_rows * std::max(t.desc().layers, 1u);
        const bool whole = left == 0 && top == 0 && right >= t.desc().width &&
                           bottom >= t.layer_rows * std::max(t.desc().layers, 1u);
        if (right > left && bottom > top) {
            if (whole) {
                t.alpha_known = true;
                t.alpha_lo = lo;
                t.alpha_hi = hi;
            } else if (t.alpha_known) {
                t.alpha_lo = std::min(t.alpha_lo, lo);
                t.alpha_hi = std::max(t.alpha_hi, hi);
            }
        }
    }
    if (t.swizzled) {
        // 132vg2: each texel to its swizzled place (the stacked layers are one tall texture)
        const unsigned width = t.desc().width, height = t.layer_rows * std::max(t.desc().layers, 1u);
        const bool r8 = t.desc().format == Format::r8;
        auto* out = reinterpret_cast<std::uint32_t*>(t.data);
        for (unsigned y = top; y < bottom; ++y) {
            const std::uint8_t* in = data + std::size_t(y - top) * pitch;
            for (unsigned x = left; x < right; ++x) {
                std::uint32_t texel;
                if (r8) texel = 0xff000000u | in[x - left];
                else std::memcpy(&texel, in + std::size_t(x - left) * 4, 4);
                out[swizzle_index(x, y, width, height)] = texel;
            }
        }
        return;
    }
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
    // 132vg2: sampled-only textures in the swizzled layout (faster sampling than linear; VITA.md)
    t->swizzled = g_swizzle_order >= 0 && !t->depth && !t->float_target && !(desc.bind & bind_target) &&
                  desc.usage != Usage::staging && desc.width >= 16 && t->layer_rows >= 16 &&
                  power_of_two(desc.width) && power_of_two(t->layer_rows * layers);
    t->data = allocate(t->bytes(), desc.usage != Usage::staging);
    if (levels && levels[0].data && !t->depth)
        write_rows(*t, 0, 0, desc.width, desc.height, static_cast<const std::uint8_t*>(levels[0].data), levels[0].row_pitch);
    else
        std::memset(t->data, 0, t->bytes());
    if (!(levels && levels[0].data && !t->depth)) {
        t->alpha_known = !t->depth;  // 132vg2 step 3d: zeroed
        t->alpha_lo = t->alpha_hi = 0;
    }
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
    const std::uint64_t t0 = now_us();
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
        const_cast<GxmTexture*>(colour)->alpha_known = false;  // 132vg2 step 3d: the GPU writes it
    }
    SceGxmDepthStencilSurface ds{};
    std::uint8_t depth_mode = 0;
    if (depth) {
        auto* d = const_cast<GxmTexture*>(depth);
        depth_mode = d->clear_pending ? 1 : 2;
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
    sceGxmSetCullMode(context_, SCE_GXM_CULL_NONE);  // 132vg2: once a scene (every draw used CULL_NONE)
    ctx_ = ContextCache{};                            // 132vg2: nothing is known to be set in the new scene
    scene_.open = true;
    scene_.colour = colour ? colour->layer(layer) : nullptr;
    scene_.depth = depth ? depth->data : nullptr;
    scene_.float_target = colour && colour->float_target;
    scene_.width = width;
    scene_.height = height;
    ++scenes_;
    // 132vg2: the scene's record (scene_dump, the GPU timing).
    ++serial_;
    SceneRecord& r = records_[serial_ % scene_ring];
    r.serial = serial_;
    r.width = static_cast<std::uint16_t>(width);
    r.height = static_cast<std::uint16_t>(height);
    r.kind = !colour ? GxmTimers::gpu_depth_only
             : colour->display >= 0 ? GxmTimers::gpu_display
             : colour->float_target ? GxmTimers::gpu_float
             : depth ? GxmTimers::gpu_colour_depth : GxmTimers::gpu_colour;
    r.depth = depth_mode;
    r.draws = r.clears = r.copies = 0;
    r.begin_us = t0;
    r.kick_us = 0;
    r.notify_index = r.notify_value = 0;
    r.done_us.store(0, std::memory_order_relaxed);
    record_ = &r;
    ++stats.gxm.scenes;
    stats.gxm.begin_scene_us += now_us() - t0;
}

void Impl::end_scene(bool notify) {
    if (!scene_.open) return;
    const std::uint64_t t0 = now_us();
    const bool timed = gpu_thread_ >= 0 && record_;
    SceGxmNotification n{};
    if (notify) {
        n.address = notify_;
        n.value = epoch_;
    } else if (timed) {
        // 132vg2: the scene's own slot (the GPU timing)
        n.address = notify_ + notify_slot0 + serial_ % notify_slots;
        n.value = serial_;
    }
    check(sceGxmEndScene(context_, nullptr, notify || timed ? &n : nullptr), "sceGxmEndScene");
    scene_.open = false;
    const std::uint64_t t1 = now_us();
    stats.gxm.end_scene_us += t1 - t0;
    if (record_) {
        record_->kick_us = t1;
        record_->notify_index = static_cast<std::uint32_t>(n.address ? n.address - notify_ : 0);
        record_->notify_value = n.value;
        record_ = nullptr;
        if (timed) {
            submitted_.store(serial_, std::memory_order_release);
            sceKernelSignalSema(gpu_sema_, 1);
        }
    }
}

void Impl::ensure_scene(const GxmTexture* colour, unsigned layer, const GxmTexture* depth) {
    const std::uint8_t* c = colour ? colour->layer(layer) : nullptr;
    const std::uint8_t* d = depth ? depth->data : nullptr;
    if (scene_.open && scene_.colour == c && scene_.depth == d && !(depth && depth->clear_pending)) return;
    end_scene(false);
    begin_scene(colour, layer, depth);
}

// ---- draws ----

// room: the parameter's array size x components (CompiledStage, 132vg2).
void set_uniform(void* buffer, const SceGxmProgramParameter* p, unsigned room, const void* data, std::size_t floats) {
    if (!p || !data || !floats) return;
    sceGxmSetUniformDataF(buffer, p, 0, static_cast<unsigned>(std::min<std::size_t>(floats, room)),
                          static_cast<const float*>(data));
}

// 132vg2: a texture's GXM descriptor for a sampler, rebuilt only when the texture's memory or the sampler changed.
const SceGxmTexture& descriptor_for(const GxmTexture& t, const SamplerDesc& s) {
    const bool point_min = s.filter != Filter::linear, point_mag = s.filter == Filter::point;
    const bool wrap = s.address == Address::wrap;
    const std::uint8_t key = std::uint8_t((point_min ? 1 : 0) | (point_mag ? 2 : 0) | (wrap ? 4 : 0));
    if (t.descriptor_data == t.data && t.descriptor_sampler == key) return t.descriptor;
    SceGxmTexture g{};
    if (t.swizzled)
        check(sceGxmTextureInitSwizzled(&g, t.data, t.texture_format, t.desc().width,
                                        t.layer_rows * std::max(t.desc().layers, 1u), 1),
              "sceGxmTextureInitSwizzled");
    else
        check(sceGxmTextureInitLinear(&g, t.data, t.texture_format, std::max(t.desc().width, 1u),
                                      t.layer_rows * std::max(t.desc().layers, 1u), 1),
              "sceGxmTextureInitLinear");
    sceGxmTextureSetMinFilter(&g, point_min ? SCE_GXM_TEXTURE_FILTER_POINT : SCE_GXM_TEXTURE_FILTER_LINEAR);
    sceGxmTextureSetMagFilter(&g, point_mag ? SCE_GXM_TEXTURE_FILTER_POINT : SCE_GXM_TEXTURE_FILTER_LINEAR);
    const SceGxmTextureAddrMode a = wrap ? SCE_GXM_TEXTURE_ADDR_REPEAT : SCE_GXM_TEXTURE_ADDR_CLAMP;
    sceGxmTextureSetUAddrMode(&g, a);
    sceGxmTextureSetVAddrMode(&g, a);
    t.descriptor = g;
    t.descriptor_data = t.data;
    t.descriptor_sampler = key;
    return t.descriptor;
}

void Impl::execute(Pipeline& p) {
    const bool sample = sample_;
    std::uint64_t t0 = sample ? now_us() : 0, t1 = 0;
    const CompiledStage& vs = stage(true, p.vertex, false);
    const CompiledStage& fs = stage(false, p.fragment, scene_.float_target, p.no_alpha_test);
    SceGxmVertexProgram* vp = vertex_program(p.vertex, p.attributes, p.attribute_count, p.stride);
    SceGxmFragmentProgram* fp = fragment_program(p.fragment, scene_.float_target, p, vs);
    // 132vg2: the context state cache (ctx_): a sceGxmSet* call whose value is already set in this scene is skipped.
    GxmTimers& tm = stats.gxm;
    tm.state_calls += 2;
    if (vp != ctx_.vp) {
        sceGxmSetVertexProgram(context_, vp);
        ctx_.vp = vp;
    } else {
        ++tm.state_skipped;
    }
    if (fp != ctx_.fp) {
        sceGxmSetFragmentProgram(context_, fp);
        ctx_.fp = fp;
    } else {
        ++tm.state_skipped;
    }
    if (sample) {
        t1 = now_us();
        stats.gxm.sample_program_us += t1 - t0;
        t0 = t1;
    }

    const Viewport& v = p.viewport;
    const float viewport[6]{v.x + v.width * 0.5F, v.width * 0.5F, v.y + v.height * 0.5F, -v.height * 0.5F,
                            v.min_depth, v.max_depth - v.min_depth};
    tm.state_calls += 5;
    if (!ctx_.viewport_set || std::memcmp(viewport, ctx_.viewport, sizeof viewport) != 0) {
        sceGxmSetViewport(context_, viewport[0], viewport[1], viewport[2], viewport[3], viewport[4], viewport[5]);
        std::memcpy(ctx_.viewport, viewport, sizeof viewport);
        ctx_.viewport_set = true;
    } else {
        ++tm.state_skipped;
    }
    // Region clipping is tile-granular on GXM; D3D clips to the viewport exactly (VITA.md).
    const int x0 = std::clamp(static_cast<int>(v.x), 0, static_cast<int>(scene_.width) - 1);
    const int y0 = std::clamp(static_cast<int>(v.y), 0, static_cast<int>(scene_.height) - 1);
    const int x1 = std::clamp(static_cast<int>(std::ceil(v.x + v.width)) - 1, x0, static_cast<int>(scene_.width) - 1);
    const int y1 = std::clamp(static_cast<int>(std::ceil(v.y + v.height)) - 1, y0, static_cast<int>(scene_.height) - 1);
    const unsigned clip[4]{static_cast<unsigned>(x0), static_cast<unsigned>(y0), static_cast<unsigned>(x1),
                           static_cast<unsigned>(y1)};
    if (!ctx_.clip_set || std::memcmp(clip, ctx_.clip, sizeof clip) != 0) {
        sceGxmSetRegionClip(context_, SCE_GXM_REGION_CLIP_OUTSIDE, clip[0], clip[1], clip[2], clip[3]);
        std::memcpy(ctx_.clip, clip, sizeof clip);
        ctx_.clip_set = true;
    } else {
        ++tm.state_skipped;
    }
    // (the cull mode, CULL_NONE for every draw, is set once a scene in begin_scene)
    const int polygon = p.wireframe ? SCE_GXM_POLYGON_MODE_TRIANGLE_LINE : SCE_GXM_POLYGON_MODE_TRIANGLE_FILL;
    if (polygon != ctx_.polygon) {
        sceGxmSetFrontPolygonMode(context_, static_cast<SceGxmPolygonMode>(polygon));
        ctx_.polygon = polygon;
    } else {
        ++tm.state_skipped;
    }
    const int depth_func = scene_.depth ? p.depth_func : SCE_GXM_DEPTH_FUNC_ALWAYS;
    if (depth_func != ctx_.depth_func) {
        sceGxmSetFrontDepthFunc(context_, static_cast<SceGxmDepthFunc>(depth_func));
        ctx_.depth_func = depth_func;
    } else {
        ++tm.state_skipped;
    }
    const int depth_write = scene_.depth && p.depth_write ? SCE_GXM_DEPTH_WRITE_ENABLED : SCE_GXM_DEPTH_WRITE_DISABLED;
    if (depth_write != ctx_.depth_write) {
        sceGxmSetFrontDepthWriteEnable(context_, static_cast<SceGxmDepthWriteMode>(depth_write));
        ctx_.depth_write = depth_write;
    } else {
        ++tm.state_skipped;
    }
    if (sample) {
        t1 = now_us();
        stats.gxm.sample_state_us += t1 - t0;
        t0 = t1;
    }

    float tex_size[16];
    float layers[4];
    std::uint64_t t_skip_textures = 0;
    for (unsigned k = 0; k < 4; ++k) {
        const GxmTexture* t = p.textures[k].texture ? p.textures[k].texture : dummy_.get();
        if (t->depth) throw std::runtime_error("gfx gxm: sampling a depth texture is unported");
        const unsigned layer_count = std::max(t->desc().layers, 1u);
        // 132vg2: the descriptor cached in the texture; an unchanged slot is not set again
        const SceGxmTexture& g = descriptor_for(*t, p.textures[k].sampler);
        if (!ctx_.texture_set[k] || std::memcmp(&g, &ctx_.texture[k], sizeof g) != 0) {
            sceGxmSetFragmentTexture(context_, k, &g);
            ctx_.texture[k] = g;
            ctx_.texture_set[k] = true;
        } else {
            ++t_skip_textures;
        }
        const_cast<GxmTexture*>(t)->used_epoch = epoch_;
        const float w = static_cast<float>(std::max(t->desc().width, 1u)), h = static_cast<float>(t->layer_rows);
        tex_size[4 * k] = w;
        tex_size[4 * k + 1] = h;
        tex_size[4 * k + 2] = 1.0F / w;
        tex_size[4 * k + 3] = 1.0F / h;
        layers[k] = static_cast<float>(layer_count);
    }
    tm.texture_slots_skipped += t_skip_textures;
    if (sample) {
        t1 = now_us();
        stats.gxm.sample_texture_us += t1 - t0;
        t0 = t1;
    }

    if (vs.uniform_buffer) {
        void* buffer = nullptr;
        check(sceGxmReserveVertexDefaultUniformBuffer(context_, &buffer), "sceGxmReserveVertexDefaultUniformBuffer");
        for (int k = 0; k < 2; ++k)
            set_uniform(buffer, vs.constants[k], vs.room_constants[k], p.vertex_constants[k].data,
                        p.vertex_constants[k].bytes / 4);
        set_uniform(buffer, vs.clear, vs.room_clear, p.clear, 4);
    }
    if (fs.uniform_buffer) {
        void* buffer = nullptr;
        check(sceGxmReserveFragmentDefaultUniformBuffer(context_, &buffer), "sceGxmReserveFragmentDefaultUniformBuffer");
        for (int k = 0; k < 2; ++k)
            set_uniform(buffer, fs.constants[k], fs.room_constants[k], p.fragment_constants[k].data,
                        p.fragment_constants[k].bytes / 4);
        set_uniform(buffer, fs.tex_size, fs.room_tex_size, tex_size, 16);
        set_uniform(buffer, fs.layers, fs.room_layers, layers, 4);
        set_uniform(buffer, fs.blend_mul, fs.room_blend_mul, p.blend_mul, 4);
        set_uniform(buffer, fs.colour, fs.room_colour, p.colour, 4);
    }
    if (sample) {
        t1 = now_us();
        stats.gxm.sample_uniform_us += t1 - t0;
        t0 = t1;
    }
    sceGxmSetVertexStream(context_, 0, p.stream);
    check(sceGxmDraw(context_, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U32, p.indices, p.count), "sceGxmDraw");
    if (sample) stats.gxm.sample_draw_us += now_us() - t0;
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

// 132vg2: one draw in 8 is timed whole and split (GxmTimers' sample fields).
void Impl::draw(bool indexed, std::uint32_t count, std::uint32_t first) {
    sample_ = (++draw_calls_ & 7u) == 0;
    if (!sample_) {
        draw_body(indexed, count, first);
        return;
    }
    const std::uint64_t t0 = now_us();
    try {
        draw_body(indexed, count, first);
    } catch (...) {
        sample_ = false;
        throw;
    }
    sample_ = false;
    ++stats.gxm.sampled;
    stats.gxm.sample_total_us += now_us() - t0;
}

void Impl::draw_body(bool indexed, std::uint32_t count, std::uint32_t first) {
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
    const GxmBuffer::Patch* patch = nullptr;  // 132vg2: the patch the draw reads (patched_target)
    std::uint32_t patch_vertex = 0;           // its first vertex: the indices are rebased by it
    if (!state_.vertices->patches.empty()) {
        // 132vg2: the vertices may be in a patch (patch_update)
        const std::pair<std::uint32_t, std::uint32_t> range{first, first + count - 1};
        const std::pair<std::uint32_t, std::uint32_t>* runs = &range;
        std::size_t run_count = 1;
        if (indexed) {
            if (!state_.indices || !state_.indices->data) throw std::runtime_error("gfx gxm: an indexed draw with no index buffer");
            const auto& r = index_runs(*state_.indices, first, count);
            runs = r.data();
            run_count = r.size();
        }
        patch = patched_target(*const_cast<GxmBuffer*>(state_.vertices), state_.offset, state_.stride, runs, run_count);
        if (patch) patch_vertex = static_cast<std::uint32_t>((patch->begin - state_.offset) / state_.stride);
    }
    const_cast<GxmBuffer*>(state_.vertices)->used_epoch = epoch_;
    // cpu_stream / cpu_indices: the vertices and indices as the CPU reads them (the shadow filter), vertex i of the
    // indices at cpu_stream + i * stride; for a patched indexed draw that address is below the patch (CPU only).
    const std::uint8_t* cpu_stream = nullptr;
    const std::uint32_t* cpu_indices = nullptr;
    if (indexed) {
        if (!state_.indices || !state_.indices->data) throw std::runtime_error("gfx gxm: an indexed draw with no index buffer");
        const_cast<GxmBuffer*>(state_.indices)->used_epoch = epoch_;
        cpu_indices = reinterpret_cast<const std::uint32_t*>(state_.indices->data) + first;
        if (patch) {
            p.stream = patch->data;
            cpu_stream = reinterpret_cast<const std::uint8_t*>(reinterpret_cast<std::uintptr_t>(patch->cpu->data()) -
                                                               std::uintptr_t(patch_vertex) * state_.stride);
            if (!g.cpu_geometry) p.indices = rebased_indices(*state_.indices, first, count, patch_vertex);
        } else {
            p.stream = base;
            cpu_stream = base;
            p.indices = cpu_indices;
        }
    } else {
        p.stream = patch ? patch->data + std::size_t(first - patch_vertex) * state_.stride
                         : base + std::size_t(first) * state_.stride;
        p.indices = sequence(count);
        cpu_stream = patch ? patch->cpu->data() + std::size_t(first - patch_vertex) * state_.stride
                           : static_cast<const std::uint8_t*>(p.stream);
        cpu_indices = p.indices;
        patch_vertex = 0;  // the sequence's indices are relative to the stream already
    }
    p.count = count;
    if (g.cpu_geometry) {
        const std::uint64_t filter_t0 = now_us();
        ++stats.gxm.shadow_draws;
        const GxmBuffer* shadow = state_.geometry_constants[1] ? state_.geometry_constants[1] : state_.vertex_constants[1];
        if (!shadow || shadow->cpu.size() < 112) throw std::runtime_error("gfx gxm: shadow_receiver without Shadow constants");
        ShadowFilter filter;
        std::memcpy(filter.clip, shadow->cpu.data(), sizeof filter.clip);
        std::memcpy(filter.facing, shadow->cpu.data() + 96, sizeof filter.facing);
        std::vector<std::uint32_t>& kept = shadow_kept_;  // 132vg2: a member scratch list (was a vector a draw)
        kept.clear();
        kept.reserve(count);
        // 132vg2: the triangles from their copy in cached RAM (shadow_triangles); the same test, the same order
        Pipeline cpu = p;
        cpu.stream = cpu_stream;
        cpu.indices = cpu_indices;
        std::size_t triangles = 0;
        const ShadowIndices* flagged = nullptr;
        if (patch && indexed && options.shadow_vertex_flags) {
            // 132vg2 step 3e: a patched receiver (the vertices in cached RAM): each vertex of the draw's exact runs is
            // tested once (outside the guard box; its normal facing the projector), then each triangle reads its 3
            // vertices' flags: the same tests on the same floats as keep_floats, without gathering 12 floats a
            // triangle (each vertex is shared by several triangles).
            const ShadowIndices& ix = shadow_index_entry(cpu, indexed, first, count);
            if (!ix.runs.empty() && ix.runs.back().second - ix.runs.front().first < 4u * count + 64u) {
                flagged = &ix;
                const std::uint32_t lo = ix.runs.front().first;
                shadow_flags_.resize(std::size_t(ix.runs.back().second - lo) + 1);
                for (const auto& r : ix.runs)
                    for (std::uint32_t i = r.first; i <= r.second; ++i) {
                        float v[6];
                        std::memcpy(v, cpu_stream + std::size_t(i) * state_.stride, sizeof v);  // position, normal
                        shadow_flags_[i - lo] =
                            std::uint8_t((filter.outside(v) ? 1u : 0u) | (filter.faces(v + 3) ? 2u : 0u));
                    }
                const std::uint8_t* f = shadow_flags_.data() - lo;
                const std::vector<std::uint32_t>& ti = ix.triangle_indices;
                triangles = ti.size() / 3;
                for (std::size_t t = 0; t < triangles; ++t) {
                    const std::uint32_t a = ti[3 * t], b = ti[3 * t + 1], c = ti[3 * t + 2];
                    if (!(f[a] & 1u) && !(f[b] & 1u) && f[c] == 2u) {
                        kept.push_back(a - patch_vertex);
                        kept.push_back(b - patch_vertex);
                        kept.push_back(c - patch_vertex);
                    }
                }
                ++stats.gxm.shadow_flagged_draws;
                if (options.shadow_flags_check) {
                    // the A/B on the same draw: the per-triangle gather must keep the same list
                    std::vector<std::uint32_t> old;
                    const ShadowTriangles& tris = shadow_triangles(cpu, indexed, first, count, true);
                    for (std::size_t t = 0; t < tris.triangle_indices.size() / 3; ++t)
                        if (filter.keep_floats(&tris.floats[t * 12]))
                            for (int k = 0; k < 3; ++k) old.push_back(tris.triangle_indices[3 * t + k] - patch_vertex);
                    ++shadow_flag_checks_;
                    if (old != kept) {
                        ++shadow_flag_mismatches_;
                        if (shadow_flag_mismatches_ <= 4)
                            options.log("gfx gxm: shadow flags check: MISMATCH (first " + std::to_string(first) +
                                        ", count " + std::to_string(count) + ": " + std::to_string(kept.size() / 3) +
                                        " vs " + std::to_string(old.size() / 3) + " triangles)");
                    }
                    if ((shadow_flag_checks_ & 1023u) == 0)
                        options.log("gfx gxm: shadow flags check: " + std::to_string(shadow_flag_checks_) +
                                    " draws compared, " + std::to_string(shadow_flag_mismatches_) + " mismatches");
                }
            }
        }
        if (!flagged) {
            const ShadowTriangles& tris = shadow_triangles(cpu, indexed, first, count, patch != nullptr);
            triangles = tris.triangle_indices.size() / 3;
            for (std::size_t t = 0; t < triangles; ++t)
                if (filter.keep_floats(&tris.floats[t * 12])) {
                    kept.push_back(tris.triangle_indices[3 * t] - patch_vertex);
                    kept.push_back(tris.triangle_indices[3 * t + 1] - patch_vertex);
                    kept.push_back(tris.triangle_indices[3 * t + 2] - patch_vertex);
                }
        }
        stats.shadow_tested += triangles;
        stats.shadow_kept += kept.size() / 3;
        stats.shadow_cache_bytes = shadow_cache_bytes_;
        if (kept.empty()) {
            stats.gxm.shadow_filter_us += now_us() - filter_t0;
            return;
        }
        auto* list = reinterpret_cast<std::uint32_t*>(allocate(kept.size() * 4, false, 16));
        std::memcpy(list, kept.data(), kept.size() * 4);
        release(list);
        transient_ += kept.size() * 4;
        p.indices = list;
        p.count = static_cast<std::uint32_t>(kept.size());
        stats.gxm.shadow_filter_us += now_us() - filter_t0;
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
        if (!state_.colour[pass]) {
            if (second) continue;   // 132vt3: a two-target draw without its first target draws only the second
            if (!state_.depth) return;
        }
        ensure_scene(state_.colour[pass], state_.layer[pass], state_.depth);
        p.fragment = pass ? g.fragment_alt : g.fragment;
        // 132vg2: scene_ps without an alpha test this draw (Scene[7]: w <= 0.5 and x < 0, the shader's own test) uses
        // the variant compiled without it: no discard in the program, so the GPU's hidden-surface removal works.
        p.no_alpha_test = false;
        proof_stream_ = patch && indexed && options.patched_proofs ? cpu_stream : nullptr;  // step 3g
        if (options.alpha_test_variant && p.fragment == scene_fragment_ && p.fragment_constants[0].data &&
            p.fragment_constants[0].bytes >= 8 * 16) {
            const float* alpha_test = static_cast<const float*>(p.fragment_constants[0].data) + 7 * 4;
            p.no_alpha_test = !(alpha_test[3] > 0.5F) && !(alpha_test[0] >= 0.0F);
            if (p.no_alpha_test) ++stats.gxm.no_alpha_test_draws;
            // 132vg2 step 3d: an alpha test that cannot fail on this draw's vertices and texture: the same variant.
            if (!p.no_alpha_test && options.alpha_proof && (!patch || proof_stream_) &&
                alpha_passes_all(static_cast<const float*>(p.fragment_constants[0].data), layout, indexed, first,
                                 count)) {
                p.no_alpha_test = true;
                ++stats.gxm.alpha_proven_draws;
            }
            // step 3g: an alpha test that discards every pixel of the draw (the FB_ONLY pairs' second draw over a
            // texture with no alpha at or below the reference): the draw produces nothing and is skipped.
            if (!p.no_alpha_test && options.alpha_proof && (!patch || proof_stream_) &&
                alpha_discards_all(static_cast<const float*>(p.fragment_constants[0].data), layout, indexed, first,
                                   count)) {
                ++stats.gxm.alpha_skipped_draws;
                continue;
            }
        }
        blend_for(blend.targets[pass && blend.independent ? 1 : 0], state_.factor, p);
        if (p.fragment == scene_fragment_) {
            bool blended = p.blend_info && p.blend.colorFunc != SCE_GXM_BLEND_FUNC_NONE;
            // 132vg2 step 3f: a blend that cannot change a pixel (blend_is_noop) is not set: the GPU's
            // hidden-surface removal then covers the draw (an opaque one when it has no discard either).
            if (blended && options.blend_proof && (!patch || proof_stream_) && p.fragment_constants[0].data &&
                p.fragment_constants[0].bytes >= 8 * 16 &&
                blend_is_noop(static_cast<const float*>(p.fragment_constants[0].data), layout, indexed, first, count,
                              p)) {
                const std::uint8_t mask = p.blend.colorMask;
                p.blend = {};
                p.blend.colorMask = mask;
                p.blend.colorFunc = p.blend.alphaFunc = SCE_GXM_BLEND_FUNC_NONE;
                p.blend.colorSrc = p.blend.alphaSrc = SCE_GXM_BLEND_FACTOR_ONE;
                p.blend.colorDst = p.blend.alphaDst = SCE_GXM_BLEND_FACTOR_ZERO;
                p.blend_info = mask != SCE_GXM_COLOR_MASK_ALL;
                blended = false;
                ++stats.gxm.blend_proven_draws;
            }
            if (blended && options.census) census_blended(p, layout, indexed, first, count, patch != nullptr);
            const int probe = g_gpu_probe_mode.load(std::memory_order_relaxed);
            if (probe) {
                // 132vg2 step 3f: the GPU probe (gfx_gxm.hpp; diagnostic, the picture is wrong while it runs)
                if (probe == 5 || (probe == 4 && blended)) continue;
                if (probe == 1 && p.fragment_constants[0].data && p.fragment_constants[0].bytes >= 5 * 16) {
                    probe_constants_.assign(static_cast<const float*>(p.fragment_constants[0].data),
                                            static_cast<const float*>(p.fragment_constants[0].data) +
                                                p.fragment_constants[0].bytes / 4);
                    probe_constants_[16] = 0.0F;  // Scene[4].x: no texture
                    p.fragment_constants[0] = {probe_constants_.data(), probe_constants_.size() * 4};
                }
                if (probe == 2) {
                    p.blend = {};
                    p.blend.colorMask = SCE_GXM_COLOR_MASK_ALL;
                    p.blend.colorFunc = p.blend.alphaFunc = SCE_GXM_BLEND_FUNC_NONE;
                    p.blend.colorSrc = p.blend.alphaSrc = SCE_GXM_BLEND_FACTOR_ONE;
                    p.blend.colorDst = p.blend.alphaDst = SCE_GXM_BLEND_FACTOR_ZERO;
                    p.blend_info = false;
                    p.blend_mul[0] = p.blend_mul[1] = p.blend_mul[2] = 1.0F;
                    p.blend_mul[3] = -1.0F;
                }
                if (probe == 3) p.no_alpha_test = true;
                if (probe == 6) {
                    p.viewport.width = 1.0F;
                    p.viewport.height = 1.0F;
                }
            }
            ++stats.gxm.scene_draws;
            if (blended) ++stats.gxm.scene_blended;
            if (!p.no_alpha_test) ++stats.gxm.scene_discard;
            if (state_.colour[pass] && state_.colour[pass]->display >= 0) ++stats.gxm.scene_display;
        }
        execute(p);
        if (record_) ++record_->draws;
    }
}

// 132vg2 step 3d (research/VITA.md "Vita render path (132vg2)"): true when scene_ps's plain alpha test (Scene[7]:
// w <= 0.5, x >= 0) cannot discard a pixel of this draw, so the variant without the test (no discard, the GPU's
// hidden-surface removal kept) draws the same pixels. The shader's alpha is vColor.a, interpolated between the
// vertices' values (a convex combination: within their range), times the filtered texel alpha (within the texture's
// alpha range) when Scene[4].x textures the draw; the test discards where (round(a * k) > x) == (y > 0.5), k = 255 raw
// (Scene[7].z), else 127.5; round is monotonic, so the range's end decides. A margin of 0.25 (in a * k) covers the
// interpolation's and the filter's rounding. Drawn vertices are bounded by the draw's index runs (index_runs; joined
// runs only add vertices, widening the range).
bool Impl::alpha_passes_all(const float* scene, const GxmLayout& layout, bool indexed, std::uint32_t first,
                            std::uint32_t count) {
    const float* test = scene + 7 * 4;
    if (test[3] > 0.5F || !(test[0] >= 0.0F)) return false;
    float amin = 0.0F, amax = 0.0F;
    if (!draw_alpha_range(scene, layout, indexed, first, count, amin, amax, proof_stream_)) return false;
    const float k = test[2] > 0.5F ? 255.0F : 127.5F;
    const float x = std::floor(test[0]);
    constexpr float margin = 0.25F;
    if (test[1] > 0.5F) return amax * k <= x + 0.5F - margin;  // discards a pixel that passes: none may pass
    return amin * k >= x + 0.5F + margin;                       // discards a pixel that fails: none may fail
}

// 132vg2 step 3g: true when scene_ps's plain alpha test discards every pixel of this draw (the mirror of
// alpha_passes_all, with the same range and margin).
bool Impl::alpha_discards_all(const float* scene, const GxmLayout& layout, bool indexed, std::uint32_t first,
                              std::uint32_t count) {
    const float* test = scene + 7 * 4;
    if (test[3] > 0.5F || !(test[0] >= 0.0F)) return false;
    float amin = 0.0F, amax = 0.0F;
    if (!draw_alpha_range(scene, layout, indexed, first, count, amin, amax, proof_stream_)) return false;
    const float k = test[2] > 0.5F ? 255.0F : 127.5F;
    const float x = std::floor(test[0]);
    constexpr float margin = 0.25F;
    if (test[1] > 0.5F) return amin * k >= x + 0.5F + margin;  // discards a pixel that passes: every pixel passes
    return amax * k <= x + 0.5F - margin;                       // discards a pixel that fails: every pixel fails
}

// 132vg2 step 3f: true when blending this scene_ps draw cannot change a pixel: the colour blend is
// SRC_ALPHA / ONE_MINUS_SRC_ALPHA (add) and the output alpha is 1.0 at every pixel (saturate(a * 255 / 128) raw,
// saturate(a) otherwise, a = vertex alpha x texel alpha as in alpha_passes_all; the Scene[7].w test path, which
// rewrites the alpha, is excluded), so the blend yields the source colour; the alpha channel's blend must then also
// yield 1.0 (or alpha is not written). Drawn without blending, the pixels are the same.
bool Impl::blend_is_noop(const float* scene, const GxmLayout& layout, bool indexed, std::uint32_t first,
                         std::uint32_t count, const Pipeline& p) {
    const SceGxmBlendInfo& b = p.blend;
    if (b.colorFunc != SCE_GXM_BLEND_FUNC_ADD || b.colorSrc != SCE_GXM_BLEND_FACTOR_SRC_ALPHA ||
        b.colorDst != SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA || !(p.blend_mul[3] < 0.0F))
        return false;
    if (b.colorMask & SCE_GXM_COLOR_MASK_A) {
        const bool src_ok = b.alphaSrc == SCE_GXM_BLEND_FACTOR_ONE || b.alphaSrc == SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
        const bool dst_ok = b.alphaDst == SCE_GXM_BLEND_FACTOR_ZERO ||
                            b.alphaDst == SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA ||
                            b.alphaDst == SCE_GXM_BLEND_FACTOR_ONE;  // 1 + d clamps to 1 in the U8 target
        if (b.alphaFunc != SCE_GXM_BLEND_FUNC_ADD || !src_ok || !dst_ok) return false;
    }
    const float* test = scene + 7 * 4;
    if (test[3] > 0.5F) return false;
    float amin = 0.0F, amax = 0.0F;
    if (!draw_alpha_range(scene, layout, indexed, first, count, amin, amax, proof_stream_)) return false;
    const float k = test[2] > 0.5F ? 255.0F / 128.0F : 1.0F;
    return amin * k >= 1.0F;
}

// 132vg2 step 3g (diagnostic, vita_test_census.txt): why a scene draw stays blended after blend_is_noop. GxmTimers
// census: 0 other blend factors (additive and the rest), 1 FIX over, 2 alpha range unknown (patched, a rendered
// texture, ...), 3 cut-out (texture alpha only 0 or its top, and vertex alpha x that top gives 1.0), 4 translucent
// (the rest); then, of all those blended: 5 with discard, 6 writing depth, 7 sampled with a linear filter, 8 with the
// full vertex alpha (vertex alpha alone gives 1.0), 9 untextured.
void Impl::census_blended(const Pipeline& p, const GxmLayout& layout, bool indexed, std::uint32_t first,
                          std::uint32_t count, bool patched) {
    auto& c = stats.gxm.census;
    const float* scene = static_cast<const float*>(p.fragment_constants[0].data);
    if (!p.no_alpha_test) ++c[5];
    if (p.depth_write) ++c[6];
    if (p.textures[0].sampler.filter != Filter::point) ++c[7];
    const SceGxmBlendInfo& b = p.blend;
    if (b.colorFunc != SCE_GXM_BLEND_FUNC_ADD || b.colorSrc != SCE_GXM_BLEND_FACTOR_SRC_ALPHA ||
        b.colorDst != SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA) {
        ++c[0];
        return;
    }
    if (!(p.blend_mul[3] < 0.0F)) {
        ++c[1];
        return;
    }
    float amin = 0.0F, amax = 0.0F;
    if ((patched && !proof_stream_) || !scene || p.fragment_constants[0].bytes < 8 * 16 ||
        !draw_alpha_range(scene, layout, indexed, first, count, amin, amax, proof_stream_)) {
        ++c[2];
        return;
    }
    const float k = scene[7 * 4 + 2] > 0.5F ? 255.0F / 128.0F : 1.0F;
    if (last_vlo_ * k >= 1.0F) ++c[8];
    const bool textured = scene[4 * 4] > 0.5F;
    if (!textured) ++c[9];
    const GxmTexture* t = state_.textures[0];
    if (textured && t && t->alpha_binary && last_vlo_ * (float(t->alpha_hi) / 255.0F) * k >= 1.0F) ++c[3];
    else ++c[4];
}

// 132vg2 step 3d / 3f: the range [amin, amax] of scene_ps's alpha (vertex colour alpha x texel alpha) over this draw.
bool Impl::draw_alpha_range(const float* scene, const GxmLayout& layout, bool indexed, std::uint32_t first,
                            std::uint32_t count, float& amin_out, float& amax_out,
                            const std::uint8_t* patched_stream) {
    const float* options = scene + 4 * 4;
    if (options[1] > 0.5F) return false;
    float tlo = 1.0F, thi = 1.0F;
    if (options[0] > 0.5F) {
        const GxmTexture* t = state_.textures[0];
        if (!t || t->back_proxy || t->display >= 0 || !t->alpha_known) return false;
        tlo = float(t->alpha_lo) / 255.0F;
        thi = float(t->alpha_hi) / 255.0F;
    }
    unsigned colour = ~0U;
    for (unsigned k = 0; k < layout.count; ++k)
        if (layout.attributes[k].components == 4 && std::strcmp(layout.attributes[k].name, "color") == 0)
            colour = layout.attributes[k].offset;
    GxmBuffer& vb = *const_cast<GxmBuffer*>(state_.vertices);
    if (colour == ~0U || !vb.data || state_.stride < colour + 16) return false;
    if (indexed && (!state_.indices || !state_.indices->data)) return false;
    float vlo = 1.0F, vhi = 0.0F;
    if (patched_stream) {
        // step 3g: a patched draw's vertices from the patch's cached-RAM copy (vertex i at patched_stream + i *
        // stride), read each draw (the patch changes every frame); its index runs lie in the patch (patched_target).
        if (!indexed) return false;
        const auto& r = index_runs(*state_.indices, first, count);
        if (r.empty()) return false;
        float a_lo = INFINITY, a_hi = -INFINITY;
        for (const auto& run : r)
            for (std::uint32_t i = run.first; i <= run.second; ++i) {
                float a;
                std::memcpy(&a, patched_stream + std::size_t(i) * state_.stride + colour + 12, 4);
                if (!(a == a) || std::isinf(a)) return false;
                a_lo = std::min(a_lo, a);
                a_hi = std::max(a_hi, a);
            }
        ++stats.gxm.alpha_patched_reads;
        vlo = a_lo;
        vhi = a_hi;
    } else {
    const GxmBuffer::AlphaKey key{indexed ? static_cast<const void*>(state_.indices) : nullptr,
                                  indexed ? state_.indices->serial : 0U, first, count, state_.offset, state_.stride};
    auto found = vb.alpha_ranges.find(key);
    if (found == vb.alpha_ranges.end()) {
        float lo = 1.0F, hi = 0.0F;  // lo > hi: unusable
        const std::pair<std::uint32_t, std::uint32_t> range{first, first + count - 1};
        const std::pair<std::uint32_t, std::uint32_t>* runs = &range;
        std::size_t run_count = 1;
        if (indexed) {
            const auto& r = index_runs(*state_.indices, first, count);
            runs = r.data();
            run_count = r.size();
        }
        bool ok = run_count > 0;
        float a_lo = INFINITY, a_hi = -INFINITY;
        for (std::size_t r = 0; ok && r < run_count; ++r) {
            if (std::size_t(state_.offset) + (std::size_t(runs[r].second) + 1) * state_.stride > vb.desc().bytes) {
                ok = false;
                break;
            }
            const std::uint8_t* v = vb.data + state_.offset + std::size_t(runs[r].first) * state_.stride + colour + 12;
            for (std::uint32_t i = runs[r].first; i <= runs[r].second; ++i, v += state_.stride) {
                float a;
                std::memcpy(&a, v, 4);
                if (!(a == a) || std::isinf(a)) {
                    ok = false;
                    break;
                }
                a_lo = std::min(a_lo, a);
                a_hi = std::max(a_hi, a);
            }
        }
        if (ok) {
            lo = a_lo;
            hi = a_hi;
        }
        if (vb.alpha_ranges.size() >= 8192) vb.alpha_ranges.clear();
        found = vb.alpha_ranges.emplace(key, std::make_pair(lo, hi)).first;
        ++stats.gxm.alpha_scans;
    }
    vlo = found->second.first;
    vhi = found->second.second;
    }
    if (vlo > vhi) return false;
    last_vlo_ = vlo;
    last_vhi_ = vhi;
    const float c[4]{vlo * tlo, vlo * thi, vhi * tlo, vhi * thi};
    amin_out = std::min(std::min(c[0], c[1]), std::min(c[2], c[3]));
    amax_out = std::max(std::max(c[0], c[1]), std::max(c[2], c[3]));
    return true;
}

// A colour clear (ClearRenderTargetView / ClearView rectangles): quads drawn with the clear program.
void Impl::clear_colour(const GxmTexture* target, unsigned layer, const float rgba[4], const Rect* rects, unsigned count) {
    if (!target) return;
    const std::uint64_t t0 = now_us();
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
        if (record_) ++record_->clears;
    }
    ++stats.gxm.clears;
    stats.gxm.clear_us += now_us() - t0;
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
    const std::uint64_t t0 = now_us();
    execute(p);
    if (record_) ++record_->clears;
    ++stats.gxm.clears;
    stats.gxm.clear_us += now_us() - t0;
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
    const std::uint64_t t0 = now_us();
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
    if (record_) ++record_->copies;
    ++stats.gxm.copies;
    stats.gxm.copy_us += now_us() - t0;
}

// ---- CPU updates and read-back ----

void Impl::update_buffer(GxmBuffer& b, const void* data, std::size_t bytes, std::size_t offset, bool keep_rest) {
    const std::size_t size = b.desc().bytes;
    if (offset > size || bytes > size - offset) throw std::runtime_error("gfx gxm: a buffer update past its end");
    if (b.desc().bind & bind_constants) {
        if (b.cpu.size() < size) b.cpu.resize(size);
        if (bytes) std::memcpy(b.cpu.data() + offset, data, bytes);
        ++stats.gxm.constant_updates;
        return;
    }
    const std::uint64_t t0 = now_us();
    if (!b.data) b.data = allocate(size, false, 16);
    b.index_ranges.clear();
    ++b.serial;  // 132vg2 step 3d
    if (!b.rebased.empty()) {
        for (const auto& r : b.rebased) release(r.second);
        b.rebased.clear();
    }
    if (!shadow_cache_.empty()) forget_shadow(&b, offset, offset + std::max<std::size_t>(bytes, 1));
    const bool large = size >= (1u << 20);
    if (options.large_buffer_patches && large && bytes && bytes < size && (!idle(b.used_epoch) || !b.patches.empty())) {
        // 132vg2: a part of a large buffer the GPU may still read: a patch (fresh memory for the range), no wait.
        patch_update(b, static_cast<const std::uint8_t*>(data), bytes, offset);
        ++stats.gxm.updates;
        stats.gxm.update_bytes += bytes;
        const std::uint64_t spent = now_us() - t0;
        stats.gxm.update_us += spent;
        stats.gxm.upd_patch_us += spent;
        return;
    }
    if (!idle(b.used_epoch) && large) {
        // A large buffer (a level's 34 MB mesh) rewritten whole: a second copy may not fit the pools (VITA.md
        // "Memory"), so wait for the GPU and write in place.
        if (large_waits_++ < 3)
            options.log("gfx gxm: waited for the GPU to update a " + std::to_string(size >> 10) + " KB buffer in place");
        drain();
    }
    b.alpha_ranges.clear();  // 132vg2 step 3d: the buffer's own bytes change below
    if (!b.patches.empty()) {
        // 132vg2: rewritten whole (the GPU is done with it): the buffer's own bytes are current again.
        for (const GxmBuffer::Patch& p : b.patches) {
            release(p.data);
            for (const auto& s : p.spares) release(s.first);
        }
        b.patches.clear();
    }
    if (!idle(b.used_epoch)) {
        // The GPU may still read the old bytes: new memory, the old block freed when its epoch completes.
        const std::uint64_t f0 = now_us();
        std::uint8_t* fresh = allocate(size, false, 16);
        if (keep_rest) std::memcpy(fresh, b.data, size);
        release(b.data);
        b.data = fresh;
        b.used_epoch = 0;
        transient_ += size;
        ++stats.gxm.upd_fresh;
        if (keep_rest) stats.gxm.upd_fresh_copied += size;
        stats.gxm.upd_fresh_us += now_us() - f0;
    }
    const std::uint64_t w0 = now_us();
    if (bytes) std::memcpy(b.data + offset, data, bytes);
    stats.gxm.upd_write_us += now_us() - w0;
    ++stats.gxm.updates;
    stats.gxm.update_bytes += bytes;
    stats.gxm.update_us += now_us() - t0;
}

// 132vg2: the range [offset, offset + bytes) of a large buffer into a new patch. Patches it overlaps or touches are
// merged into it; the parts of the merged range the update does not cover come from the old patches, else from the
// buffer (which the GPU never writes). The old patches' memory is freed once the GPU is done with it.
// widen_lo / widen_hi: a range the new patch must also cover (a draw's vertices; patched_base), its bytes taken from
// the old patches and the buffer like the rest outside the update.
void Impl::patch_update(GxmBuffer& b, const std::uint8_t* data, std::size_t bytes, std::size_t offset,
                        std::size_t widen_lo, std::size_t widen_hi) {
    std::size_t lo = std::min(offset, widen_lo), hi = std::max(offset + bytes, widen_hi);
    if (bytes && widen_hi == 0)
        for (GxmBuffer::Patch& p : b.patches)
            if (p.begin <= lo && p.end >= hi && idle(p.used_epoch)) {
                // inside a patch no draw in flight reads (made earlier this frame, typically): written in place
                const std::uint64_t i0 = now_us();
                std::memcpy(p.data + (offset - p.begin), data, bytes);
                const std::uint64_t i1 = now_us();
                std::memcpy(p.cpu->data() + (offset - p.begin), data, bytes);
                ++stats.gxm.patch_updates;
                ++stats.gxm.upd_patch_in_place;
                stats.gxm.upd_patch_gpu_write_us += i1 - i0;
                return;
            }
    if (bytes && widen_hi == 0)
        for (GxmBuffer::Patch& p : b.patches)
            if (p.begin <= lo && p.end >= hi) {
                // step 3b: inside a patch the GPU may still read: the next GPU copy of it (a spare the GPU is done
                // with, else a new one while there are fewer than 3), written whole from the cached copy
                std::memcpy(p.cpu->data() + (offset - p.begin), data, bytes);
                std::uint8_t* next = nullptr;
                for (auto it = p.spares.begin(); it != p.spares.end(); ++it)
                    if (idle(it->second)) {
                        next = it->first;
                        p.spares.erase(it);
                        break;
                    }
                const std::uint64_t a0 = now_us();
                if (!next) next = allocate(p.end - p.begin, false, 16);
                const std::uint64_t a1 = now_us();
                std::memcpy(next, p.cpu->data(), p.end - p.begin);
                const std::uint64_t a2 = now_us();
                p.spares.emplace_back(p.data, p.used_epoch);
                if (p.spares.size() > 3) {  // keep at most 3 earlier copies
                    release(p.spares.front().first);
                    p.spares.erase(p.spares.begin());
                }
                p.data = next;
                p.used_epoch = 0;
                ++stats.gxm.patch_updates;
                ++stats.gxm.upd_patch_rotated;
                stats.gxm.patch_bytes += p.end - p.begin;
                stats.gxm.upd_patch_alloc_us += a1 - a0;
                stats.gxm.upd_patch_gpu_write_us += a2 - a1;
                return;
            }
    for (bool grown = true; grown;) {
        grown = false;
        for (const GxmBuffer::Patch& p : b.patches)
            if (p.begin <= hi && p.end >= lo && (p.begin < lo || p.end > hi)) {
                lo = std::min(lo, p.begin);
                hi = std::max(hi, p.end);
                grown = true;
            }
    }
    // Built in cached RAM, then written once to the GPU copy.
    auto cpu = std::make_shared<std::vector<std::uint8_t>>(hi - lo);
    std::uint8_t* out = cpu->data();
    // The bytes outside the update: [lo, offset) and [offset + bytes, hi): the old patches' cached copies, and the
    // buffer's own bytes (uncached) only between them.
    const auto fill = [&](std::size_t from, std::size_t to) {
        if (to <= from) return;
        std::size_t at = from;
        for (const GxmBuffer::Patch& p : b.patches) {  // sorted
            const std::size_t s = std::max(at, p.begin), e = std::min(to, p.end);
            if (s >= e) continue;
            if (s > at) {
                std::memcpy(out + (at - lo), b.data + at, s - at);
                stats.gxm.patch_fill_bytes += s - at;
            }
            std::memcpy(out + (s - lo), p.cpu->data() + (s - p.begin), e - s);
            at = e;
        }
        if (to > at) {
            std::memcpy(out + (at - lo), b.data + at, to - at);
            stats.gxm.patch_fill_bytes += to - at;
        }
    };
    fill(lo, offset);
    fill(offset + bytes, hi);
    if (bytes) std::memcpy(out + (offset - lo), data, bytes);
    const std::uint64_t a0 = now_us();
    std::uint8_t* fresh = allocate(hi - lo, false, 16);
    const std::uint64_t a1 = now_us();
    std::memcpy(fresh, out, hi - lo);
    const std::uint64_t a2 = now_us();
    stats.gxm.upd_patch_alloc_us += a1 - a0;
    stats.gxm.upd_patch_gpu_write_us += a2 - a1;
    stats.gxm.upd_patch_in_cdram += cdram_.owns(fresh) ? 1 : 0;
    std::vector<GxmBuffer::Patch> kept;
    kept.reserve(b.patches.size() + 1);
    for (const GxmBuffer::Patch& p : b.patches) {
        if (p.begin >= lo && p.end <= hi) {  // merged into the new patch
            release(p.data);
            for (const auto& s : p.spares) release(s.first);
        }
        else kept.push_back(p);
    }
    const auto at = std::lower_bound(kept.begin(), kept.end(), lo,
                                     [](const GxmBuffer::Patch& p, std::size_t v) { return p.begin < v; });
    GxmBuffer::Patch made;
    made.begin = lo;
    made.end = hi;
    made.data = fresh;
    made.cpu = std::move(cpu);
    kept.insert(at, std::move(made));
    b.patches = std::move(kept);
    stats.gxm.patches_live = b.patches.size();
    transient_ += hi - lo;
    ++stats.gxm.patch_updates;
    stats.gxm.patch_bytes += hi - lo;
}

// 132vg2: indices[first, first + count) minus base_vertex, kept in GPU memory per (first, count, base_vertex) until the
// index buffer is written (a patched draw's indices; read once from the uncached buffer).
const std::uint32_t* Impl::rebased_indices(const GxmBuffer& ib, std::uint32_t first, std::uint32_t count,
                                           std::uint32_t base_vertex) {
    auto& cache = const_cast<GxmBuffer&>(ib).rebased;
    const GxmBuffer::RebasedKey key{first, count, base_vertex};
    const auto found = cache.find(key);
    if (found != cache.end()) return found->second;
    if (cache.size() >= 4096) {  // a cap (patches that moved leave old entries): start again
        for (const auto& r : cache) release(r.second);
        cache.clear();
    }
    std::vector<std::uint32_t> list(reinterpret_cast<const std::uint32_t*>(ib.data) + first,
                                    reinterpret_cast<const std::uint32_t*>(ib.data) + first + count);
    for (std::uint32_t& i : list) i -= base_vertex;
    auto* gpu = reinterpret_cast<std::uint32_t*>(allocate(std::size_t(count) * 4, false, 16));
    std::memcpy(gpu, list.data(), std::size_t(count) * 4);
    ++stats.gxm.rebased_lists;
    return cache.emplace(key, gpu).first->second;
}

// 132vg2: the vertex indices used by indices[first, first + count) of an index buffer as sorted runs [a, b]
// (cached per range; at most 64 runs, the closest ones joined beyond that, which only widens them).
const std::vector<std::pair<std::uint32_t, std::uint32_t>>& Impl::index_runs(const GxmBuffer& ib, std::uint32_t first,
                                                                            std::uint32_t count) {
    const std::uint64_t key = (std::uint64_t(first) << 32) | count;
    auto& cache = const_cast<GxmBuffer&>(ib).index_ranges;
    const auto found = cache.find(key);
    if (found != cache.end()) return found->second;
    const auto* in = reinterpret_cast<const std::uint32_t*>(ib.data) + first;
    std::vector<std::uint32_t> v(in, in + count);
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    std::vector<std::pair<std::uint32_t, std::uint32_t>> runs;
    for (const std::uint32_t i : v) {
        if (!runs.empty() && runs.back().second + 1 == i) runs.back().second = i;
        else runs.emplace_back(i, i);
    }
    while (runs.size() > 64) {  // join the two runs with the smallest gap
        std::size_t best = 0;
        for (std::size_t k = 1; k + 1 < runs.size(); ++k)
            if (runs[k + 1].first - runs[k].second < runs[best + 1].first - runs[best].second) best = k;
        runs[best].second = runs[best + 1].second;
        runs.erase(runs.begin() + std::ptrdiff_t(best) + 1);
    }
    return cache.emplace(key, std::move(runs)).first->second;
}

// 132vg2: the patch a draw of a patched buffer reads, its vertices being `runs`: none (the buffer) when no run touches
// a patch; the patch when every run lies inside that one patch and it starts on a vertex (the draw then reads it with
// its indices rebased by the patch's first vertex: no stream address outside the patch's memory). A draw mixing a
// patch with other bytes widens the patch over itself when that is small, else folds every patch back into the
// buffer after a GPU wait (counted: patch_folds).
const GxmBuffer::Patch* Impl::patched_target(GxmBuffer& b, std::uint32_t offset, std::uint32_t stride,
                                             const std::pair<std::uint32_t, std::uint32_t>* runs, std::size_t run_count) {
    const GxmBuffer::Patch* target = nullptr;
    bool outside = false, mixed = false;
    // The first patch ending after `at` (the patches are sorted and disjoint, so their ends are sorted too).
    const auto first_after = [&](std::size_t at) {
        return std::partition_point(b.patches.begin(), b.patches.end(),
                                    [at](const GxmBuffer::Patch& p) { return p.end <= at; });
    };
    if (!run_count) return nullptr;
    {
        // the whole draw's range first: no patch in it (most draws), or one patch over all of it
        const std::size_t lo = offset + std::size_t(runs[0].first) * stride;
        const std::size_t hi = offset + (std::size_t(runs[run_count - 1].second) + 1) * stride;
        const auto it = first_after(lo);
        if (it == b.patches.end() || it->begin >= hi) return nullptr;
        if (it->begin <= lo && it->end >= hi) target = &*it;
    }
    for (std::size_t r = 0; r < run_count && !mixed && !target; ++r) {
        const std::size_t begin = offset + std::size_t(runs[r].first) * stride;
        const std::size_t end = offset + (std::size_t(runs[r].second) + 1) * stride;
        const auto it = first_after(begin);
        const bool touched = it != b.patches.end() && it->begin < end;
        const GxmBuffer::Patch* in = touched && it->begin <= begin && it->end >= end ? &*it : nullptr;
        if (!touched) outside = true;
        else if (!in) mixed = true;
        else if (target && target != in) mixed = true;
        else target = in;
        if (outside && target) mixed = true;
    }
    if (!mixed && !target) return nullptr;
    if (mixed && run_count) {
        // A draw reading a patch and bytes beside it (a model only partly posed, or ranges joined): widen a patch over
        // the whole draw when that is small (its other bytes from the buffer, which the GPU never writes).
        const std::size_t lo = offset + std::size_t(runs[0].first) * stride;
        const std::size_t hi = offset + (std::size_t(runs[run_count - 1].second) + 1) * stride;
        if (hi > lo && hi - lo <= (256u << 10) && hi <= b.desc().bytes) {
            patch_update(b, nullptr, 0, lo, lo, hi);
            ++stats.gxm.patch_widened;
            for (const GxmBuffer::Patch& p : b.patches)
                if (p.begin <= lo && p.end >= hi) {
                    target = &p;
                    mixed = false;
                    break;
                }
        }
    }
    if (!mixed && target->begin >= offset && (target->begin - offset) % stride == 0) {
        ++stats.gxm.patch_draws;
        target->used_epoch = epoch_;
        return target;
    }
    // Mixed (or the moved address would wrap): fold the patches into the buffer once the GPU is done with it.
    if (logged_folds_ < 6 && options.log) {
        ++logged_folds_;
        char text[400];
        int n = std::snprintf(text, sizeof text, "gfx gxm: patch fold (%s): draw runs", mixed ? "mixed" : "alignment");
        for (std::size_t r = 0; r < run_count && r < 8 && n < 300; ++r)
            n += std::snprintf(text + n, sizeof text - n, " %u-%u", runs[r].first, runs[r].second);
        n += std::snprintf(text + n, sizeof text - n, " (%u runs, stride %u, offset %u); patches", unsigned(run_count),
                           stride, offset);
        for (std::size_t k = 0; k < b.patches.size() && k < 8 && n < 380; ++k)
            n += std::snprintf(text + n, sizeof text - n, " %u-%u", unsigned(b.patches[k].begin / stride),
                               unsigned(b.patches[k].end / stride));
        options.log(text);
    }
    drain();
    b.alpha_ranges.clear();  // 132vg2 step 3d: the fold writes the buffer's own bytes
    for (const GxmBuffer::Patch& q : b.patches) {
        std::memcpy(b.data + q.begin, q.data, q.end - q.begin);
        release(q.data);
        for (const auto& s : q.spares) release(s.first);
    }
    b.patches.clear();
    ++stats.gxm.patch_folds;
    return nullptr;
}
void Impl::update_texture(GxmTexture& t, const Box& box, const std::uint8_t* data, unsigned pitch) {
    if (t.depth) throw std::runtime_error("gfx gxm: depth texture updates are unported");
    const unsigned right = std::min(box.right, t.desc().width), bottom = std::min(box.bottom, t.layer_rows);
    if (right <= box.left || bottom <= box.top) return;
    const std::uint64_t t0 = now_us();
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
    ++stats.gxm.updates;
    stats.gxm.update_bytes += std::size_t(right - box.left) * (bottom - box.top) * t.bpp;
    stats.gxm.update_us += now_us() - t0;
}

void Impl::read(const GxmTexture& t, unsigned sub,
                const std::function<void(const std::uint8_t* data, unsigned row_pitch)>& reader) {
    if (t.depth) throw std::runtime_error("gfx gxm: depth read-back is unported");
    const unsigned mips = std::max(t.desc().mips, 1u);
    if (sub % mips) throw std::runtime_error("gfx gxm: read-back of mip levels above 0 is unported");
    drain();
    if (t.swizzled) {
        // 132vg2: back to rows (layer sub / mips of the stacked texture)
        const unsigned width = t.desc().width, height = t.layer_rows * std::max(t.desc().layers, 1u);
        const unsigned layer = sub / mips;
        const bool r8 = t.desc().format == Format::r8;
        const auto* in = reinterpret_cast<const std::uint32_t*>(t.data);
        std::vector<std::uint8_t> bytes(std::size_t(width) * t.layer_rows * (r8 ? 1 : 4));
        for (unsigned y = 0; y < t.layer_rows; ++y)
            for (unsigned x = 0; x < width; ++x) {
                const std::uint32_t texel = in[swizzle_index(x, layer * t.layer_rows + y, width, height)];
                if (r8) bytes[std::size_t(y) * width + x] = static_cast<std::uint8_t>(texel);
                else std::memcpy(&bytes[(std::size_t(y) * width + x) * 4], &texel, 4);
            }
        reader(bytes.data(), width * (r8 ? 1 : 4));
        return;
    }
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
    const std::uint64_t submit_t0 = now_us();
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
    ++stats.gxm.submits;
    stats.gxm.submit_us += now_us() - submit_t0;
}

// Ends the frame's last scene on the back buffer (its notification marks the epoch complete) and queues the flip.
SwapChain::Presented Impl::present(unsigned sync_interval) {
    ensure_scene(displays_[back_].texture.get(), 0, nullptr);
    end_scene(true);
    // 132vg2: the frame's scenes (scene_dump)
    frames_[presents_ % 4] = {frame_first_, serial_, epoch_};
    ++presents_;
    frame_first_ = serial_ + 1;
    DisplayRequest request{displays_[back_].block.base, sync_interval > 0 ? 1 : 0};
    const std::uint64_t q0 = now_us();
    check(sceGxmDisplayQueueAddEntry(displays_[front_].sync, displays_[back_].sync, &request),
          "sceGxmDisplayQueueAddEntry");
    const std::uint64_t q1 = now_us();
    front_ = back_;
    back_ = (back_ + 1) % static_cast<unsigned>(displays_.size());
    ++epoch_;
    collect();
    // At most two frames in flight (the deferred frees and the transient copies wait on them).
    while (epoch_ - 1 - completed_ > 2) {
        sceKernelDelayThread(100);
        collect();
    }
    stats.gxm.display_queue_us += q1 - q0;
    stats.gxm.present_wait_us += now_us() - q1;
    stats.scenes_last_frame = scenes_;
    stats.draws_last_frame = draws_;
    stats.scenes_total += scenes_;
    stats.draws_total += draws_;
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
    std::shared_ptr<Buffer> create_buffer(const BufferDesc& desc, const void* initial, const char* what) override {
        std::lock_guard<std::recursive_mutex> lock(impl_.mutex);
        if (desc.bytes >= (4u << 20) && impl_.options.log)
            impl_.options.log("gfx gxm: large buffer " + std::to_string(desc.bytes >> 10) + " KB (" +
                              (what ? what : "unnamed") + ")");
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
    {
        // Port-only (2026-10-26, hardware): every Cg stage (both fragment targets) is compiled or read from the GXP cache
        // here, on the main thread (its 8 MB stack). A level's first float-target fragment program, compiled by
        // SceShaccCg on the render host thread (a small std::thread stack), faulted inside the compiler.
        std::lock_guard<std::recursive_mutex> lock(g_impl->mutex);
        const auto start = std::chrono::steady_clock::now();
        const unsigned compiled = g_impl->stats.programs_compiled;
        const auto warm = [](bool vertex, unsigned index, bool float_out) {
            try {
                g_impl->stage(vertex, index, float_out);
            } catch (const ShaderCompilerMissing&) {
                throw;
            } catch (const std::exception& e) {  // the draw that needs it reports it
                if (g_impl->options.log) g_impl->options.log(std::string("gfx gxm: stage not prepared: ") + e.what());
            }
        };
        try {
            for (std::size_t k = 0; k < gxm_program_count; ++k) {
                const GxmProgram& g = gxm_program(static_cast<Program>(k));
                warm(true, g.vertex, false);
                for (const bool float_out : {false, true}) {
                    warm(false, g.fragment, float_out);
                    if (g.fragment_alt != ~0U) warm(false, g.fragment_alt, float_out);
                }
            }
            if (g_impl->scene_fragment_ != ~0U)  // 132vg2: scene_ps without the alpha test
                for (const bool float_out : {false, true}) {
                    try {
                        g_impl->stage(false, g_impl->scene_fragment_, float_out, true);
                    } catch (const ShaderCompilerMissing&) {
                        throw;
                    } catch (const std::exception& e) {
                        if (g_impl->options.log) g_impl->options.log(std::string("gfx gxm: stage not prepared: ") + e.what());
                    }
                }
            warm(true, gxm_clear_vertex, false);
            warm(true, gxm_copy_vertex, false);
            for (const bool float_out : {false, true}) {
                warm(false, gxm_clear_fragment, float_out);
                warm(false, gxm_copy_fragment, float_out);
            }
        } catch (const ShaderCompilerMissing&) {
            // Reported by the first draw (App's message box), as before.
        }
        // 132vg2: the swizzled layout (needs the copy program); off unless the self test passes.
        static bool swizzle_tested = false;
        if (!swizzle_tested && !options.linear_textures) {
            swizzle_tested = true;
            try {
                g_impl->swizzle_self_test();
            } catch (const std::exception& e) {
                g_swizzle_order = -1;
                if (g_impl->options.log) g_impl->options.log(std::string("gfx gxm: swizzled textures off: ") + e.what());
            }
        } else if (options.linear_textures && g_impl->options.log) {
            g_impl->options.log("gfx gxm: swizzled textures off (vita_test_linear_textures.txt)");
        }
        if (g_impl->options.log)
            g_impl->options.log("gfx gxm: shader stages ready in " +
                                std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                   std::chrono::steady_clock::now() - start).count()) +
                                " ms (" + std::to_string(g_impl->stats.programs_compiled - compiled) + " compiled)");
    }
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

std::string scene_dump() {
    if (!g_impl) return {};
    std::lock_guard<std::recursive_mutex> lock(g_impl->mutex);
    return g_impl->scene_dump();
}

std::string placement_line() { return "GPU objects: placed by this backend in its own pools (MemoryStats)"; }

// The call trace (vita_test_gl_trace.txt) is the vitaGL backend's; this backend has none.
void arm_trace() {}

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

