#pragma once
// Port-only (checkpoint 132mt1, not the original): the renderer abstraction layer. The game-side drawing code (the
// scene, GUI, particle and corona renderers, the frame overlay and the play / front-end hosts) records its GPU work
// against this interface; a backend (gfx_d3d11.cpp: Direct3D 11 for the PC; a GXM backend for the PS Vita later)
// executes it. research/THREADING.md "Step 1" and research/RENDERER.md "The renderer abstraction layer".
//
// Design rules (so a GXM backend and a render thread fit later):
// - No hidden global device state. Every resource and pipeline-state object belongs to one Device; a draw names
//   all its state through an explicit CommandList (targets, viewport, program, buffers, textures, samplers, blend,
//   depth, raster) that the owner submits with Device::submit. Lists are plain data (commands plus a byte arena for
//   inline constant / vertex updates): they can be built on one thread and executed on another.
// - Shader programs are named by gfx::Program (gfx_programs.hpp), the one list of every shader variant the port
//   uses; a backend maps each to its own code (the HLSL text lives only in the D3D11 backend).
// - Resources are shared_ptr objects: a command list holds plain pointers, so the resources it names must stay
//   alive until it is submitted (every renderer submits before it returns or calls out).
// - Immediate device operations (Device::write, Device::read, Device::removed, Device::wait_idle) run in order after
//   everything already submitted: a renderer submits its list before one of them.
// - Device loss: a backend call that sees the device lost throws DeviceLost (gfx_device_lost.hpp); the renderers
//   catch it at their public entry points as before (checkpoint 132win3).
//
// The D3D11 backend reproduces the earlier direct calls one to one (same states, same shaders, same order), so the
// refactor is pixel-identical (THREADING.md "Step 1 evidence").
#include "gfx_programs.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace omega::gfx {

// bc1 / bc3 (port-only, 132md11): sampled-only block-compressed textures (DXT1 / DXT5; texture_blocks.hpp), created
// only on a device whose capabilities() has block_textures; a level's row_pitch is one row of 4 x 4 blocks.
enum class Format : std::uint8_t { unknown, rgba8, bgra8, r8, r32f, d24s8, bc1, bc3 };
enum class Usage : std::uint8_t {
    immutable,  // initial data only
    gpu,        // GPU read / write; updated with CommandList::update
    dynamic,    // CPU rewritten whole (Device::write)
    staging     // CPU read-back (Device::read)
};
enum Bind : std::uint8_t {
    bind_vertices = 1, bind_indices = 2, bind_constants = 4,
    bind_sampled = 8,   // a shader resource view
    bind_target = 16,   // render target view(s): one per array layer
    bind_depth = 32     // a depth-stencil view
};

struct BufferDesc {
    std::size_t bytes = 0;
    Usage usage = Usage::gpu;
    unsigned bind = 0;
};
struct TextureDesc {
    unsigned width = 0, height = 0;
    unsigned mips = 1, layers = 1;
    Format format = Format::rgba8;
    Usage usage = Usage::gpu;
    unsigned bind = 0;
    unsigned samples = 1;
};
// One initial mip level (layer 0): rows of row_pitch bytes.
struct Level {
    const void* data = nullptr;
    unsigned row_pitch = 0;
};
// The checked-call labels of a texture's creation (device_lost.hpp names the call that saw a loss).
struct TextureLabels {
    const char* texture = "Create texture";
    const char* target = "Create render target view";
    const char* sampled = "Create shader resource view";
    const char* depth = "Create depth view";
};
inline unsigned subresource(unsigned mip, unsigned layer, unsigned mips) { return mip + layer * mips; }

class Buffer {
public:
    virtual ~Buffer() = default;
    const BufferDesc& desc() const { return desc_; }

protected:
    BufferDesc desc_;
};
class Texture {
public:
    virtual ~Texture() = default;
    const TextureDesc& desc() const { return desc_; }

protected:
    TextureDesc desc_;
};

// Pipeline state, by value; the backend makes an object of each (cached by the caller).
enum class BlendFactor : std::uint8_t { zero, one, src_alpha, inv_src_alpha, factor, inv_factor };
enum class BlendOp : std::uint8_t { add, rev_subtract };
enum ColourMask : std::uint8_t { mask_r = 1, mask_g = 2, mask_b = 4, mask_a = 8, mask_rgb = 7, mask_all = 15 };
struct TargetBlend {
    bool enable = false;
    BlendFactor src = BlendFactor::one, dst = BlendFactor::zero;
    BlendOp op = BlendOp::add;
    BlendFactor src_alpha = BlendFactor::one, dst_alpha = BlendFactor::zero;
    BlendOp op_alpha = BlendOp::add;
    std::uint8_t write_mask = mask_all;
};
struct BlendDesc {
    bool independent = false;            // targets[1] used (else every target takes targets[0])
    std::array<TargetBlend, 2> targets{};
};
enum class Compare : std::uint8_t { never, less, less_equal, always };
struct DepthDesc {
    bool test = false, write = false;
    Compare compare = Compare::less_equal;
};
struct RasterDesc {
    bool wireframe = false;  // every pass of the port: no culling, depth clipping on
};
enum class Filter : std::uint8_t { linear, point, min_point_mag_linear };
enum class Address : std::uint8_t { wrap, clamp };
struct SamplerDesc {
    Filter filter = Filter::linear;
    Address address = Address::clamp;
    float max_lod = 0.0F;            // 0: the top level only; a large value: every mip
    bool compare_always = false;     // the comparison function word (not used by these filters)
};
class BlendState { public: virtual ~BlendState() = default; };
class DepthState { public: virtual ~DepthState() = default; };
class RasterState { public: virtual ~RasterState() = default; };
class SamplerState { public: virtual ~SamplerState() = default; };

struct Viewport {
    float x = 0, y = 0, width = 0, height = 0, min_depth = 0, max_depth = 1;
};
struct Box {
    unsigned left = 0, top = 0, front = 0, right = 0, bottom = 0, back = 1;
};
struct Rect {
    int left = 0, top = 0, right = 0, bottom = 0;
};
// A render target: a texture's view, or one layer of an array texture.
struct TargetView {
    const Texture* texture = nullptr;
    unsigned layer = 0;
};
enum Stage : std::uint8_t { stage_vertex = 1, stage_geometry = 2, stage_pixel = 4 };

// One recorded command (CommandList). Plain data: pointers name objects of the list's device.
struct Command {
    enum class Op : std::uint8_t {
        set_targets, set_viewport, clear_target, clear_depth, clear_rects, set_raster, set_depth, set_blend,
        set_program, set_vertex_buffer, set_index_buffer, set_constants, set_textures, set_sampler, update_buffer,
        update_texture, draw, draw_indexed, copy, copy_region, flush, reset_state
    };
    Op op{};
    std::uint8_t count = 0;        // targets / constants / textures / rects
    std::uint8_t flags = 0;        // stages, clear flags, has_box, has_factor
    Program program{};
    std::uint32_t slot = 0;
    std::uint32_t a = 0, b = 0, c = 0, d = 0;  // draw counts, strides, offsets, subresources, arena offsets
    const void* objects[4]{};      // textures / buffers / states / targets
    std::uint32_t layers[2]{};     // target layers
    float values[6]{};             // colour, blend factor, viewport, depth
    // Boxes and rectangles are stored in the list's arena.
};

class CommandList {
public:
    void clear() {
        commands_.clear();
        arena_.clear();
    }
    bool empty() const { return commands_.empty(); }
    const std::vector<Command>& commands() const { return commands_; }
    const std::uint8_t* arena(std::uint32_t offset) const { return arena_.data() + offset; }

    // Colour targets (count 0..2; a null texture unbinds that slot) and the depth target (null: none).
    void set_targets(unsigned count, const TargetView* colour, const Texture* depth) {
        Command& c = push(Command::Op::set_targets);
        c.count = static_cast<std::uint8_t>(count);
        for (unsigned k = 0; k < count && k < 2; ++k) {
            c.objects[k] = colour[k].texture;
            c.layers[k] = colour[k].layer;
        }
        c.objects[2] = depth;
    }
    void set_target(const Texture* colour, const Texture* depth = nullptr) {
        const TargetView view{colour, 0};
        set_targets(1, &view, depth);
    }
    void set_viewport(const Viewport& v) {
        Command& c = push(Command::Op::set_viewport);
        c.values[0] = v.x; c.values[1] = v.y; c.values[2] = v.width; c.values[3] = v.height;
        c.values[4] = v.min_depth; c.values[5] = v.max_depth;
    }
    void clear_target(TargetView target, const float rgba[4]) {
        Command& c = push(Command::Op::clear_target);
        c.objects[0] = target.texture;
        c.layers[0] = target.layer;
        for (int k = 0; k < 4; ++k) c.values[k] = rgba[k];
    }
    enum : std::uint8_t { clear_depth_bit = 1, clear_stencil_bit = 2 };
    void clear_depth(const Texture* depth, std::uint8_t flags, float value, std::uint8_t stencil = 0) {
        Command& c = push(Command::Op::clear_depth);
        c.objects[0] = depth;
        c.flags = flags;
        c.values[0] = value;
        c.a = stencil;
    }
    // Clears rectangles of a target (D3D11.1 ClearView), at most two.
    void clear_rects(const Texture* target, const float rgba[4], const Rect* rects, unsigned count) {
        Command& c = push(Command::Op::clear_rects);
        c.objects[0] = target;
        for (int k = 0; k < 4; ++k) c.values[k] = rgba[k];
        c.count = static_cast<std::uint8_t>(count < 2 ? count : 2);
        c.c = store(rects, sizeof(Rect) * c.count);
    }
    void set_raster(const RasterState* state) { push(Command::Op::set_raster).objects[0] = state; }
    void set_depth(const DepthState* state) { push(Command::Op::set_depth).objects[0] = state; }
    // A null state is the backend's default (blending off, every channel written); factor null: (1, 1, 1, 1).
    void set_blend(const BlendState* state, const float* factor = nullptr) {
        Command& c = push(Command::Op::set_blend);
        c.objects[0] = state;
        if (factor) {
            c.flags = 1;
            for (int k = 0; k < 4; ++k) c.values[k] = factor[k];
        }
    }
    // The program's shaders, its vertex layout and triangle lists.
    void set_program(Program program) { push(Command::Op::set_program).program = program; }
    void set_vertex_buffer(const Buffer* buffer, std::uint32_t stride, std::uint32_t offset = 0) {
        Command& c = push(Command::Op::set_vertex_buffer);
        c.objects[0] = buffer;
        c.a = stride;
        c.b = offset;
    }
    // 32-bit indices.
    void set_index_buffer(const Buffer* buffer) { push(Command::Op::set_index_buffer).objects[0] = buffer; }
    void set_constants(std::uint8_t stages, std::uint32_t slot, unsigned count, const Buffer* const* buffers) {
        Command& c = push(Command::Op::set_constants);
        c.flags = stages;
        c.slot = slot;
        c.count = static_cast<std::uint8_t>(count);
        for (unsigned k = 0; k < count && k < 4; ++k) c.objects[k] = buffers[k];
    }
    void set_constants(std::uint8_t stages, std::uint32_t slot, const Buffer* buffer) { set_constants(stages, slot, 1, &buffer); }
    // Pixel-shader textures (their shader resource views; null unbinds), at most four.
    void set_textures(std::uint32_t slot, unsigned count, const Texture* const* textures) {
        Command& c = push(Command::Op::set_textures);
        c.slot = slot;
        c.count = static_cast<std::uint8_t>(count);
        for (unsigned k = 0; k < count && k < 4; ++k) c.objects[k] = textures[k];
    }
    void set_texture(std::uint32_t slot, const Texture* texture) { set_textures(slot, 1, &texture); }
    void set_sampler(std::uint32_t slot, const SamplerState* sampler) {
        Command& c = push(Command::Op::set_sampler);
        c.slot = slot;
        c.objects[0] = sampler;
    }
    // A Usage::gpu buffer's bytes [offset, offset + bytes) (whole: offset 0, bytes = its size).
    void update_buffer(const Buffer* buffer, const void* data, std::size_t bytes, std::size_t offset = 0, bool whole = true) {
        Command& c = push(Command::Op::update_buffer);
        c.objects[0] = buffer;
        c.a = store(data, bytes);
        c.b = static_cast<std::uint32_t>(bytes);
        c.c = static_cast<std::uint32_t>(offset);
        c.flags = whole ? 0 : 1;
    }
    // A Usage::gpu texture's box (mip 0), rows of row_pitch bytes.
    void update_texture(const Texture* texture, const Box& box, const void* data, unsigned row_pitch) {
        Command& c = push(Command::Op::update_texture);
        c.objects[0] = texture;
        c.d = store(&box, sizeof box);
        const std::size_t rows = box.bottom > box.top ? box.bottom - box.top : 0;
        c.a = store(data, rows * row_pitch);
        c.b = row_pitch;
    }
    void draw(std::uint32_t count, std::uint32_t first) {
        Command& c = push(Command::Op::draw);
        c.a = count;
        c.b = first;
    }
    void draw_indexed(std::uint32_t count, std::uint32_t first_index) {
        Command& c = push(Command::Op::draw_indexed);
        c.a = count;
        c.b = first_index;
    }
    // The whole texture (same size and format).
    void copy(const Texture* destination, const Texture* source) {
        Command& c = push(Command::Op::copy);
        c.objects[0] = destination;
        c.objects[1] = source;
    }
    // A region: source subresource's box (null: whole) to destination subresource at (x, y).
    void copy_region(const Texture* destination, unsigned destination_sub, unsigned x, unsigned y, const Texture* source,
                     unsigned source_sub, const Box* box) {
        Command& c = push(Command::Op::copy_region);
        c.objects[0] = destination;
        c.objects[1] = source;
        c.a = destination_sub;
        c.b = x;
        c.c = y;
        c.d = source_sub;
        if (box) {
            c.flags = 1;
            c.slot = store(box, sizeof *box);
        }
    }
    void flush() { push(Command::Op::flush); }
    // Every binding back to the backend's defaults (before a device is released).
    void reset_state() { push(Command::Op::reset_state); }

private:
    Command& push(Command::Op op) {
        commands_.emplace_back();
        commands_.back().op = op;
        return commands_.back();
    }
    std::uint32_t store(const void* data, std::size_t bytes) {
        const auto at = arena_.size();
        const auto* p = static_cast<const std::uint8_t*>(data);
        if (bytes) arena_.insert(arena_.end(), p, p + bytes);
        return static_cast<std::uint32_t>(at);
    }
    std::vector<Command> commands_;
    std::vector<std::uint8_t> arena_;
};

// Port-only (checkpoint 132md9, research/RENDERER.md "GPU skinning"): what a backend can do beyond the common set.
// gpu_skinning: Program::scene_skinned / shadow_skinned (VertexLayout::skinned, the bone palette rows in constant
// buffer b2) are implemented; without it the mod system skins every replacement part on the CPU. (Not named
// DeviceCapabilities: the Windows headers define that as a macro.)
// block_textures (132md11): immutable sampled textures of Format::bc1 / bc3 with their mip levels (a GXM backend:
// SCE_GXM_TEXTURE_FORMAT_UBC1_ABGR / UBC3_ABGR, the blocks tiled at upload; vitaGL: GL_COMPRESSED_RGBA_S3TC_DXT1 /
// DXT5); without it a mod's block-compressed texture is decoded to RGBA8 before the upload (gfx::upload_levels).
struct Capabilities {
    bool gpu_skinning = false;
    bool block_textures = false;
};

class Device {
public:
    virtual ~Device() = default;
    // The backend's description ("D3D11 hardware (adapter)", ...).
    virtual const std::string& name() const = 0;
    // Port-only (132md9): the optional features this device implements; a backend that does not override this has
    // none of them (its callers take their fallbacks).
    virtual Capabilities capabilities() const { return {}; }

    // Resources. `initial`: the buffer's bytes (immutable / gpu buffers; may be null for gpu ones); `levels`: the
    // texture's mip levels for layer 0 (null for none).
    virtual std::shared_ptr<Buffer> create_buffer(const BufferDesc& desc, const void* initial, const char* label) = 0;
    virtual std::shared_ptr<Texture> create_texture(const TextureDesc& desc, const Level* levels,
                                                    const TextureLabels& labels) = 0;
    virtual std::shared_ptr<BlendState> create_blend(const BlendDesc& desc, const char* label) = 0;
    virtual std::shared_ptr<DepthState> create_depth(const DepthDesc& desc, const char* label) = 0;
    virtual std::shared_ptr<RasterState> create_raster(const RasterDesc& desc, const char* label) = 0;
    virtual std::shared_ptr<SamplerState> create_sampler(const SamplerDesc& desc, const char* label) = 0;
    // Makes a program ready (compiles it on first use on this device). set_program needs it prepared.
    virtual void prepare(Program program) = 0;

    // Executes a list (in order, after everything submitted before) and clears it.
    virtual void submit(CommandList& list) = 0;
    // Immediate: a dynamic buffer's first `bytes` replaced (the rest undefined).
    virtual void write(const Buffer& buffer, const void* data, std::size_t bytes, const char* label) = 0;
    // Immediate: a staging texture's subresource, mapped for reading (rows of row_pitch bytes).
    virtual void read(const Texture& staging, unsigned subresource,
                      const std::function<void(const std::uint8_t* data, unsigned row_pitch)>& reader,
                      const char* label) = 0;
    // The device was removed or reset (draws are lost until the owner re-creates it).
    virtual bool removed() const = 0;
    // Port-only diagnostic (--present-probe): waits until the GPU has finished everything submitted; false when the
    // backend cannot tell (no wait made).
    virtual bool wait_idle() = 0;
};

// Port-only (132md9): the device exists and skins replacement parts (Capabilities::gpu_skinning).
inline bool gpu_skinning(const Device* device) { return device && device->capabilities().gpu_skinning; }
inline bool block_textures(const Device* device) { return device && device->capabilities().block_textures; }

// A window's presentable buffers.
class SwapChain {
public:
    virtual ~SwapChain() = default;
    // The buffers' size.
    virtual void size(unsigned& width, unsigned& height, const char* label) = 0;
    // The current back buffer: with_target adds its render target view (a new view on each call).
    virtual std::shared_ptr<Texture> buffer(bool with_target, const char* label, const char* target_label) = 0;
    // Resizes the buffers (every view of them must be released first).
    virtual void resize(unsigned width, unsigned height, const char* label) = 0;
    enum class Presented : std::uint8_t { ok, occluded, lost };
    virtual Presented present(unsigned sync_interval, const char* label) = 0;
};

// A device and the swap chain the scene draws into. owned: made for one renderer (its own window), which may not
// release or reset it; otherwise shared (the session's game window, HostWindow::scene_display).
struct Display {
    std::shared_ptr<Device> device;
    std::shared_ptr<SwapChain> swap_chain;
    std::string backend;
    bool owned = false;
};

// The backend's own device without a window (the GUI renderer's offscreen captures); the backend linked in
// defines it (gfx_d3d11.cpp: hardware D3D11, else WARP).
std::shared_ptr<Device> create_offscreen_device(const char* label);

// Convenience: one-command lists, submitted at once.
inline void clear_target(Device& device, const Texture* target, const float rgba[4]) {
    CommandList list;
    list.clear_target({target, 0}, rgba);
    device.submit(list);
}
inline void clear_depth(Device& device, const Texture* depth, float value) {
    CommandList list;
    list.clear_depth(depth, CommandList::clear_depth_bit, value);
    device.submit(list);
}

}  // namespace omega::gfx
