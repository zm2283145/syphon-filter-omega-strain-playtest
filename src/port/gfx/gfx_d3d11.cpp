// Port-only (checkpoint 132mt1): the Direct3D 11 backend of gfx_device.hpp. See gfx_d3d11.hpp.
#include "gfx_d3d11.hpp"
#include "gfx_d3d11_shaders.hpp"

#include "device_lost.hpp"

#include <d3d11_1.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace omega::gfx::d3d11 {
namespace {

template <class T> using Ptr = Microsoft::WRL::ComPtr<T>;

void check(HRESULT result, const char* operation) { check_device(result, operation); }

DXGI_FORMAT dxgi_format(Format f) {
    switch (f) {
    case Format::rgba8: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case Format::bgra8: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case Format::r8: return DXGI_FORMAT_R8_UNORM;
    case Format::r32f: return DXGI_FORMAT_R32_FLOAT;
    case Format::d24s8: return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case Format::bc1: return DXGI_FORMAT_BC1_UNORM;   // 132md11
    case Format::bc3: return DXGI_FORMAT_BC3_UNORM;
    case Format::unknown: break;
    }
    throw std::runtime_error("gfx d3d11: a texture of an unknown format");
}
Format gfx_format(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: return Format::rgba8;
    case DXGI_FORMAT_B8G8R8A8_UNORM: return Format::bgra8;
    case DXGI_FORMAT_R8_UNORM: return Format::r8;
    case DXGI_FORMAT_R32_FLOAT: return Format::r32f;
    case DXGI_FORMAT_D24_UNORM_S8_UINT: return Format::d24s8;
    default: return Format::unknown;
    }
}
D3D11_USAGE d3d_usage(Usage u) {
    switch (u) {
    case Usage::immutable: return D3D11_USAGE_IMMUTABLE;
    case Usage::gpu: return D3D11_USAGE_DEFAULT;
    case Usage::dynamic: return D3D11_USAGE_DYNAMIC;
    case Usage::staging: return D3D11_USAGE_STAGING;
    }
    return D3D11_USAGE_DEFAULT;
}
UINT cpu_access(Usage u) {
    return u == Usage::dynamic ? D3D11_CPU_ACCESS_WRITE : u == Usage::staging ? D3D11_CPU_ACCESS_READ : 0;
}
UINT d3d_bind(unsigned bind) {
    UINT out = 0;
    if (bind & bind_vertices) out |= D3D11_BIND_VERTEX_BUFFER;
    if (bind & bind_indices) out |= D3D11_BIND_INDEX_BUFFER;
    if (bind & bind_constants) out |= D3D11_BIND_CONSTANT_BUFFER;
    if (bind & bind_sampled) out |= D3D11_BIND_SHADER_RESOURCE;
    if (bind & bind_target) out |= D3D11_BIND_RENDER_TARGET;
    if (bind & bind_depth) out |= D3D11_BIND_DEPTH_STENCIL;
    return out;
}
D3D11_BLEND d3d_blend(BlendFactor f) {
    switch (f) {
    case BlendFactor::zero: return D3D11_BLEND_ZERO;
    case BlendFactor::one: return D3D11_BLEND_ONE;
    case BlendFactor::src_alpha: return D3D11_BLEND_SRC_ALPHA;
    case BlendFactor::inv_src_alpha: return D3D11_BLEND_INV_SRC_ALPHA;
    case BlendFactor::factor: return D3D11_BLEND_BLEND_FACTOR;
    case BlendFactor::inv_factor: return D3D11_BLEND_INV_BLEND_FACTOR;
    }
    return D3D11_BLEND_ONE;
}
D3D11_BLEND_OP d3d_op(BlendOp op) { return op == BlendOp::rev_subtract ? D3D11_BLEND_OP_REV_SUBTRACT : D3D11_BLEND_OP_ADD; }
D3D11_COMPARISON_FUNC d3d_compare(Compare c) {
    switch (c) {
    case Compare::never: return D3D11_COMPARISON_NEVER;
    case Compare::less: return D3D11_COMPARISON_LESS;
    case Compare::less_equal: return D3D11_COMPARISON_LESS_EQUAL;
    case Compare::always: return D3D11_COMPARISON_ALWAYS;
    }
    return D3D11_COMPARISON_ALWAYS;
}

class D3D11Buffer final : public Buffer {
public:
    D3D11Buffer(const BufferDesc& desc, Ptr<ID3D11Buffer> buffer) : buffer_(std::move(buffer)) { desc_ = desc; }
    Ptr<ID3D11Buffer> buffer_;
};

class D3D11Texture final : public Texture {
public:
    explicit D3D11Texture(const TextureDesc& desc) { desc_ = desc; }
    Ptr<ID3D11Texture2D> texture_;
    Ptr<ID3D11ShaderResourceView> sampled_;
    std::vector<Ptr<ID3D11RenderTargetView>> targets_;  // one per layer (layer 0 only for a plain texture)
    Ptr<ID3D11DepthStencilView> depth_;
    ID3D11RenderTargetView* target(unsigned layer) const {
        if (layer >= targets_.size() || !targets_[layer]) throw std::runtime_error("gfx d3d11: a texture without that render target");
        return targets_[layer].Get();
    }
};

TextureDesc texture_desc(ID3D11Texture2D* texture) {
    D3D11_TEXTURE2D_DESC d{};
    texture->GetDesc(&d);
    TextureDesc out;
    out.width = d.Width;
    out.height = d.Height;
    out.mips = d.MipLevels;
    out.layers = d.ArraySize;
    out.format = gfx_format(d.Format);
    out.usage = d.Usage == D3D11_USAGE_STAGING ? Usage::staging : d.Usage == D3D11_USAGE_DYNAMIC ? Usage::dynamic
              : d.Usage == D3D11_USAGE_IMMUTABLE ? Usage::immutable : Usage::gpu;
    out.bind = ((d.BindFlags & D3D11_BIND_SHADER_RESOURCE) ? bind_sampled : 0) | ((d.BindFlags & D3D11_BIND_RENDER_TARGET) ? bind_target : 0)
             | ((d.BindFlags & D3D11_BIND_DEPTH_STENCIL) ? bind_depth : 0);
    out.samples = d.SampleDesc.Count;
    return out;
}

class D3D11Blend final : public BlendState { public: Ptr<ID3D11BlendState> state_; };
class D3D11Depth final : public DepthState { public: Ptr<ID3D11DepthStencilState> state_; };
class D3D11Raster final : public RasterState { public: Ptr<ID3D11RasterizerState> state_; };
class D3D11Sampler final : public SamplerState { public: Ptr<ID3D11SamplerState> state_; };

// A command's object back to the backend type (through the gfx base it was recorded as).
template <class T> const T* as(const void* object) {
    if constexpr (std::is_base_of_v<RasterState, T>) return static_cast<const T*>(static_cast<const RasterState*>(object));
    else if constexpr (std::is_base_of_v<DepthState, T>) return static_cast<const T*>(static_cast<const DepthState*>(object));
    else if constexpr (std::is_base_of_v<BlendState, T>) return static_cast<const T*>(static_cast<const BlendState*>(object));
    else return static_cast<const T*>(static_cast<const SamplerState*>(object));
}
const D3D11Texture* tex(const void* object) { return static_cast<const D3D11Texture*>(static_cast<const Texture*>(object)); }
ID3D11Buffer* buf(const void* object) {
    return object ? static_cast<const D3D11Buffer*>(static_cast<const Buffer*>(object))->buffer_.Get() : nullptr;
}

class D3D11Device final : public Device {
public:
    D3D11Device(Ptr<ID3D11Device> device, Ptr<ID3D11DeviceContext> context, std::string name)
        : device_(std::move(device)), context_(std::move(context)), name_(std::move(name)) {
        if (!device_ || !context_) throw std::runtime_error("gfx d3d11: no device");
        context_.As(&context1_);  // ClearView (the 4:3 bar mask) needs D3D11.1; without it the mask is skipped
    }
    // Port-only (132ws8, research/FRONTEND.md "Shutdown (132ws8)"): the device is released with no GPU work in flight.
    // ClearState unbinds every view and state, Flush submits what is queued, then an event query is polled for at
    // most 2 s (a removed device ends the wait at once). The play-test exit hang was the NVIDIA driver's final device
    // release waiting in the user-mode driver (minutes, under GPU load); the members then release in reverse order
    // (the programs, layouts and query before the context, the context before the device).
    ~D3D11Device() override {
        try {
            drain_before_release();
        } catch (...) {
        }
    }
    const std::string& name() const override { return name_; }
    Capabilities capabilities() const override { return {.gpu_skinning = true, .block_textures = true}; }   // 132md9, 132md11

    std::shared_ptr<Buffer> create_buffer(const BufferDesc& desc, const void* initial, const char* label) override {
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = static_cast<UINT>(desc.bytes);
        d.Usage = d3d_usage(desc.usage);
        d.BindFlags = d3d_bind(desc.bind);
        d.CPUAccessFlags = cpu_access(desc.usage);
        const D3D11_SUBRESOURCE_DATA content{initial, 0, 0};
        Ptr<ID3D11Buffer> buffer;
        check(device_->CreateBuffer(&d, initial ? &content : nullptr, buffer.GetAddressOf()), label);
        return std::make_shared<D3D11Buffer>(desc, std::move(buffer));
    }

    std::shared_ptr<Texture> create_texture(const TextureDesc& desc, const Level* levels, const TextureLabels& labels) override {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = desc.width;
        d.Height = desc.height;
        d.MipLevels = desc.mips;
        d.ArraySize = desc.layers;
        d.Format = dxgi_format(desc.format);
        d.SampleDesc.Count = desc.samples;
        d.Usage = d3d_usage(desc.usage);
        d.BindFlags = d3d_bind(desc.bind);
        d.CPUAccessFlags = cpu_access(desc.usage);
        std::vector<D3D11_SUBRESOURCE_DATA> content;
        if (levels)
            for (unsigned k = 0; k < desc.mips; ++k) content.push_back({levels[k].data, levels[k].row_pitch, 0});
        auto out = std::make_shared<D3D11Texture>(desc);
        check(device_->CreateTexture2D(&d, levels ? content.data() : nullptr, out->texture_.GetAddressOf()), labels.texture);
        if (desc.bind & bind_target) {
            if (desc.layers == 1) {
                out->targets_.emplace_back();
                check(device_->CreateRenderTargetView(out->texture_.Get(), nullptr, out->targets_.back().GetAddressOf()),
                      labels.target);
            } else {
                for (unsigned layer = 0; layer < desc.layers; ++layer) {
                    D3D11_RENDER_TARGET_VIEW_DESC view{};
                    view.Format = d.Format;
                    view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                    view.Texture2DArray.FirstArraySlice = layer;
                    view.Texture2DArray.ArraySize = 1;
                    out->targets_.emplace_back();
                    check(device_->CreateRenderTargetView(out->texture_.Get(), &view, out->targets_.back().GetAddressOf()),
                          labels.target);
                }
            }
        }
        if (desc.bind & bind_sampled)
            check(device_->CreateShaderResourceView(out->texture_.Get(), nullptr, out->sampled_.GetAddressOf()), labels.sampled);
        if (desc.bind & bind_depth)
            check(device_->CreateDepthStencilView(out->texture_.Get(), nullptr, out->depth_.GetAddressOf()), labels.depth);
        return out;
    }

    std::shared_ptr<BlendState> create_blend(const BlendDesc& desc, const char* label) override {
        D3D11_BLEND_DESC d{};
        d.IndependentBlendEnable = desc.independent ? TRUE : FALSE;
        for (unsigned k = 0; k < (desc.independent ? 2u : 1u); ++k) {
            const TargetBlend& t = desc.targets[k];
            auto& rt = d.RenderTarget[k];
            rt.BlendEnable = t.enable ? TRUE : FALSE;
            rt.SrcBlend = d3d_blend(t.src);
            rt.DestBlend = d3d_blend(t.dst);
            rt.BlendOp = d3d_op(t.op);
            rt.SrcBlendAlpha = d3d_blend(t.src_alpha);
            rt.DestBlendAlpha = d3d_blend(t.dst_alpha);
            rt.BlendOpAlpha = d3d_op(t.op_alpha);
            rt.RenderTargetWriteMask = t.write_mask;
        }
        auto out = std::make_shared<D3D11Blend>();
        check(device_->CreateBlendState(&d, out->state_.GetAddressOf()), label);
        return out;
    }
    std::shared_ptr<DepthState> create_depth(const DepthDesc& desc, const char* label) override {
        D3D11_DEPTH_STENCIL_DESC d{};
        d.DepthEnable = desc.test ? TRUE : FALSE;
        d.DepthWriteMask = desc.write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        d.DepthFunc = d3d_compare(desc.compare);
        auto out = std::make_shared<D3D11Depth>();
        check(device_->CreateDepthStencilState(&d, out->state_.GetAddressOf()), label);
        return out;
    }
    std::shared_ptr<RasterState> create_raster(const RasterDesc& desc, const char* label) override {
        D3D11_RASTERIZER_DESC d{};
        d.FillMode = desc.wireframe ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
        d.CullMode = D3D11_CULL_NONE;
        d.DepthClipEnable = TRUE;
        auto out = std::make_shared<D3D11Raster>();
        check(device_->CreateRasterizerState(&d, out->state_.GetAddressOf()), label);
        return out;
    }
    std::shared_ptr<SamplerState> create_sampler(const SamplerDesc& desc, const char* label) override {
        D3D11_SAMPLER_DESC d{};
        d.Filter = desc.filter == Filter::point                  ? D3D11_FILTER_MIN_MAG_MIP_POINT
                 : desc.filter == Filter::min_point_mag_linear ? D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT
                                                                : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        d.AddressU = d.AddressV = d.AddressW = desc.address == Address::wrap ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_CLAMP;
        d.MaxLOD = desc.max_lod;
        if (desc.compare_always) d.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
        auto out = std::make_shared<D3D11Sampler>();
        check(device_->CreateSamplerState(&d, out->state_.GetAddressOf()), label);
        return out;
    }

    void prepare(Program program) override {
        Compiled& p = programs_[static_cast<std::size_t>(program)];
        if (p.ready) return;
        const HlslProgram& h = hlsl_program(program);
        Ptr<ID3D11PixelShader> shared_ps;
        if (const Program base = pixel_program(program); base != program) {
            prepare(base);
            shared_ps = programs_[static_cast<std::size_t>(base)].ps;
        }
        const auto vs = compile(h, h.vertex_entry, "vs_5_0");
        const auto gs = h.geometry_entry ? compile(h, h.geometry_entry, "gs_5_0") : Ptr<ID3DBlob>{};
        const auto ps = shared_ps ? Ptr<ID3DBlob>{} : compile(h, h.pixel_entry, "ps_5_0");
        Compiled made;
        check(device_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, made.vs.GetAddressOf()),
              h.vertex_label);
        if (gs)
            check(device_->CreateGeometryShader(gs->GetBufferPointer(), gs->GetBufferSize(), nullptr, made.gs.GetAddressOf()),
                  h.geometry_label);
        if (shared_ps)
            made.ps = shared_ps;
        else
            check(device_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, made.ps.GetAddressOf()),
                  h.pixel_label);
        const auto layout = program_layout(program);
        auto& input = layouts_[static_cast<std::size_t>(layout)];
        if (!input) {
            const HlslLayout& l = hlsl_layout(layout);
            check(device_->CreateInputLayout(l.elements, l.count, vs->GetBufferPointer(), vs->GetBufferSize(),
                                             input.GetAddressOf()),
                  l.label);
        }
        made.layout = input;
        made.ready = true;
        p = std::move(made);
    }

    void submit(CommandList& list) override {
        ID3D11DeviceContext* c = context_.Get();
        for (const Command& k : list.commands()) {
            switch (k.op) {
            case Command::Op::set_targets: {
                ID3D11RenderTargetView* views[2]{};
                for (unsigned n = 0; n < k.count && n < 2; ++n)
                    views[n] = k.objects[n] ? tex(k.objects[n])->target(k.layers[n]) : nullptr;
                ID3D11DepthStencilView* depth = k.objects[2] ? tex(k.objects[2])->depth_.Get() : nullptr;
                c->OMSetRenderTargets(k.count, k.count ? views : nullptr, depth);
                break;
            }
            case Command::Op::set_viewport: {
                const D3D11_VIEWPORT v{k.values[0], k.values[1], k.values[2], k.values[3], k.values[4], k.values[5]};
                c->RSSetViewports(1, &v);
                break;
            }
            case Command::Op::clear_target:
                c->ClearRenderTargetView(tex(k.objects[0])->target(k.layers[0]), k.values);
                break;
            case Command::Op::clear_depth: {
                UINT flags = 0;
                if (k.flags & CommandList::clear_depth_bit) flags |= D3D11_CLEAR_DEPTH;
                if (k.flags & CommandList::clear_stencil_bit) flags |= D3D11_CLEAR_STENCIL;
                c->ClearDepthStencilView(tex(k.objects[0])->depth_.Get(), flags, k.values[0], static_cast<UINT8>(k.a));
                break;
            }
            case Command::Op::clear_rects: {
                if (!context1_) break;
                D3D11_RECT rects[2]{};
                const auto* r = reinterpret_cast<const Rect*>(list.arena(k.c));
                for (unsigned n = 0; n < k.count; ++n) {
                    Rect copy;
                    std::memcpy(&copy, r + n, sizeof copy);
                    rects[n] = {LONG(copy.left), LONG(copy.top), LONG(copy.right), LONG(copy.bottom)};
                }
                context1_->ClearView(tex(k.objects[0])->target(0), k.values, rects, k.count);
                break;
            }
            case Command::Op::set_raster:
                c->RSSetState(k.objects[0] ? as<D3D11Raster>(k.objects[0])->state_.Get() : nullptr);
                break;
            case Command::Op::set_depth:
                c->OMSetDepthStencilState(k.objects[0] ? as<D3D11Depth>(k.objects[0])->state_.Get() : nullptr, 0);
                break;
            case Command::Op::set_blend:
                c->OMSetBlendState(k.objects[0] ? as<D3D11Blend>(k.objects[0])->state_.Get() : nullptr,
                                   (k.flags & 1) ? k.values : nullptr, 0xffffffff);
                break;
            case Command::Op::set_program: {
                const Compiled& p = programs_[static_cast<std::size_t>(k.program)];
                if (!p.ready) throw std::runtime_error("gfx d3d11: a program used before prepare()");
                c->VSSetShader(p.vs.Get(), nullptr, 0);
                c->GSSetShader(p.gs.Get(), nullptr, 0);
                c->PSSetShader(p.ps.Get(), nullptr, 0);
                c->IASetInputLayout(p.layout.Get());
                c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                break;
            }
            case Command::Op::set_vertex_buffer: {
                ID3D11Buffer* b = buf(k.objects[0]);
                const UINT stride = k.a, offset = k.b;
                c->IASetVertexBuffers(0, 1, &b, &stride, &offset);
                break;
            }
            case Command::Op::set_index_buffer:
                c->IASetIndexBuffer(buf(k.objects[0]), DXGI_FORMAT_R32_UINT, 0);
                break;
            case Command::Op::set_constants: {
                ID3D11Buffer* b[4]{};
                for (unsigned n = 0; n < k.count && n < 4; ++n) b[n] = buf(k.objects[n]);
                if (k.flags & stage_vertex) c->VSSetConstantBuffers(k.slot, k.count, b);
                if (k.flags & stage_geometry) c->GSSetConstantBuffers(k.slot, k.count, b);
                if (k.flags & stage_pixel) c->PSSetConstantBuffers(k.slot, k.count, b);
                break;
            }
            case Command::Op::set_textures: {
                ID3D11ShaderResourceView* views[4]{};
                for (unsigned n = 0; n < k.count && n < 4; ++n) views[n] = k.objects[n] ? tex(k.objects[n])->sampled_.Get() : nullptr;
                c->PSSetShaderResources(k.slot, k.count, views);
                break;
            }
            case Command::Op::set_sampler: {
                ID3D11SamplerState* s = k.objects[0] ? as<D3D11Sampler>(k.objects[0])->state_.Get() : nullptr;
                c->PSSetSamplers(k.slot, 1, &s);
                break;
            }
            case Command::Op::update_buffer:
                if (k.flags & 1) {
                    const D3D11_BOX box{k.c, 0, 0, k.c + k.b, 1, 1};
                    c->UpdateSubresource(buf(k.objects[0]), 0, &box, list.arena(k.a), 0, 0);
                } else {
                    c->UpdateSubresource(buf(k.objects[0]), 0, nullptr, list.arena(k.a), 0, 0);
                }
                break;
            case Command::Op::update_texture: {
                Box b;
                std::memcpy(&b, list.arena(k.d), sizeof b);
                const D3D11_BOX box{b.left, b.top, b.front, b.right, b.bottom, b.back};
                c->UpdateSubresource(tex(k.objects[0])->texture_.Get(), 0, &box, list.arena(k.a), k.b, 0);
                break;
            }
            case Command::Op::draw: c->Draw(k.a, k.b); break;
            case Command::Op::draw_indexed: c->DrawIndexed(k.a, k.b, 0); break;
            case Command::Op::copy:
                c->CopyResource(tex(k.objects[0])->texture_.Get(), tex(k.objects[1])->texture_.Get());
                break;
            case Command::Op::copy_region: {
                D3D11_BOX box{};
                if (k.flags & 1) {
                    Box b;
                    std::memcpy(&b, list.arena(k.slot), sizeof b);
                    box = {b.left, b.top, b.front, b.right, b.bottom, b.back};
                }
                c->CopySubresourceRegion(tex(k.objects[0])->texture_.Get(), k.a, k.b, k.c, 0, tex(k.objects[1])->texture_.Get(),
                                         k.d, (k.flags & 1) ? &box : nullptr);
                break;
            }
            case Command::Op::flush: c->Flush(); break;
            case Command::Op::reset_state: c->ClearState(); break;
            }
        }
        list.clear();
    }

    void write(const Buffer& buffer, const void* data, std::size_t bytes, const char* label) override {
        ID3D11Buffer* b = static_cast<const D3D11Buffer&>(buffer).buffer_.Get();
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT result = device_result(context_->Map(b, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), label);
        if (device_lost_code(result) && mapped.pData) context_->Unmap(b, 0);  // the test hook's loss after a real map
        throw_if_failed(result, label);
        std::memcpy(mapped.pData, data, bytes);
        context_->Unmap(b, 0);
    }

    void read(const Texture& staging, unsigned sub, const std::function<void(const std::uint8_t*, unsigned)>& reader,
              const char* label) override {
        ID3D11Texture2D* t = static_cast<const D3D11Texture&>(staging).texture_.Get();
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT result = device_result(context_->Map(t, sub, D3D11_MAP_READ, 0, &mapped), label);
        if (device_lost_code(result) && mapped.pData) context_->Unmap(t, sub);
        throw_if_failed(result, label);
        try {
            reader(static_cast<const std::uint8_t*>(mapped.pData), mapped.RowPitch);
        } catch (...) {
            context_->Unmap(t, sub);
            throw;
        }
        context_->Unmap(t, sub);
    }

    bool removed() const override { return device_->GetDeviceRemovedReason() != S_OK; }

    // Port-only (132ws8): ~D3D11Device's bounded drain; false when the GPU was still busy after `limit_ms`.
    bool drain_before_release(unsigned limit_ms = 2000) {
        if (!context_) return true;
        context_->ClearState();
        context_->Flush();
        Ptr<ID3D11Query> query;
        const D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
        if (FAILED(device_->CreateQuery(&qd, query.GetAddressOf()))) return true;
        context_->End(query.Get());
        const auto start = std::chrono::steady_clock::now();
        for (;;) {
            const HRESULT r = context_->GetData(query.Get(), nullptr, 0, 0);
            if (r != S_FALSE) return true;
            if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(limit_ms)) {
                std::cerr << "gfx d3d11 (port-only, 132ws8): " << name_ << ": the GPU was still busy " << limit_ms
                          << " ms after the release's flush; released anyway\n";
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    bool wait_idle() override {
        if (!query_) {
            const D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
            if (FAILED(device_->CreateQuery(&qd, query_.GetAddressOf()))) {
                query_.Reset();
                return false;
            }
        }
        context_->End(query_.Get());
        while (context_->GetData(query_.Get(), nullptr, 0, 0) == S_FALSE) std::this_thread::yield();
        return true;
    }

    Ptr<ID3D11Device> device_;
    Ptr<ID3D11DeviceContext> context_;

private:
    struct Compiled {
        Ptr<ID3D11VertexShader> vs;
        Ptr<ID3D11GeometryShader> gs;
        Ptr<ID3D11PixelShader> ps;
        Ptr<ID3D11InputLayout> layout;
        bool ready = false;
    };
    static Ptr<ID3DBlob> compile(const HlslProgram& h, const char* entry, const char* profile) {
        Ptr<ID3DBlob> code, errors;
        const auto result = D3DCompile(h.source, h.size, h.source_name, nullptr, nullptr, entry, profile,
                                       D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                       code.GetAddressOf(), errors.GetAddressOf());
        if (FAILED(result) && errors)
            throw std::runtime_error(std::string(h.error_prefix) + static_cast<const char*>(errors->GetBufferPointer()));
        check(result, h.compile_label);
        return code;
    }
    Ptr<ID3D11DeviceContext1> context1_;
    std::string name_;
    std::array<Compiled, program_count> programs_{};
    std::array<Ptr<ID3D11InputLayout>, static_cast<std::size_t>(VertexLayout::count)> layouts_{};
    Ptr<ID3D11Query> query_;
};

class D3D11SwapChain final : public SwapChain {
public:
    D3D11SwapChain(std::shared_ptr<Device> device, Ptr<IDXGISwapChain> swap) : device_(std::move(device)), swap_(std::move(swap)) {
        if (!swap_) throw std::runtime_error("gfx d3d11: no swap chain");
    }
    void size(unsigned& width, unsigned& height, const char* label) override {
        DXGI_SWAP_CHAIN_DESC d{};
        check(swap_->GetDesc(&d), label);
        width = d.BufferDesc.Width;
        height = d.BufferDesc.Height;
    }
    std::shared_ptr<Texture> buffer(bool with_target, const char* label, const char* target_label) override {
        Ptr<ID3D11Texture2D> back;
        check(swap_->GetBuffer(0, IID_PPV_ARGS(back.GetAddressOf())), label);
        auto out = std::make_shared<D3D11Texture>(texture_desc(back.Get()));
        out->texture_ = back;
        if (with_target) {
            auto& d = static_cast<D3D11Device&>(*device_);
            out->targets_.emplace_back();
            check(d.device_->CreateRenderTargetView(back.Get(), nullptr, out->targets_.back().GetAddressOf()), target_label);
        }
        return out;
    }
    void resize(unsigned width, unsigned height, const char* label) override {
        DXGI_SWAP_CHAIN_DESC kept{};   // 132win10: the shared swap chain's flags (the waitable diagnostic) are kept
        swap_->GetDesc(&kept);
        check(swap_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, kept.Flags), label);
    }
    Presented present(unsigned sync_interval, const char* label) override {
        const auto result = device_result(swap_->Present(std::min(sync_interval, 4u), 0), label);
        if (device_lost_code(result)) return Presented::lost;
        throw_if_failed(result, label);
        return result == DXGI_STATUS_OCCLUDED ? Presented::occluded : Presented::ok;
    }

private:
    std::shared_ptr<Device> device_;
    Ptr<IDXGISwapChain> swap_;
};

std::string adapter_name(ID3D11Device* device) {
    Ptr<IDXGIDevice> dxgi;
    Ptr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC description{};
    std::string name;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(dxgi.GetAddressOf()))) && SUCCEEDED(dxgi->GetAdapter(adapter.GetAddressOf()))
        && SUCCEEDED(adapter->GetDesc(&description)))
        for (const wchar_t* c = description.Description; *c; ++c) name += *c < 128 ? static_cast<char>(*c) : '?';
    return name;
}

constexpr unsigned max_viewport = 16384;  // D3D11's texture limit (a fullscreen 8K monitor fits)

}  // namespace

std::shared_ptr<Device> wrap_device(Ptr<ID3D11Device> device, Ptr<ID3D11DeviceContext> context, std::string name) {
    return std::make_shared<D3D11Device>(std::move(device), std::move(context), std::move(name));
}

std::shared_ptr<SwapChain> wrap_swap_chain(const std::shared_ptr<Device>& device, Ptr<IDXGISwapChain> swap_chain) {
    return std::make_shared<D3D11SwapChain>(device, std::move(swap_chain));
}

std::shared_ptr<Texture> wrap_target(const std::shared_ptr<Device>&, ID3D11RenderTargetView* view) {
    if (!view) return {};
    Ptr<ID3D11Resource> resource;
    view->GetResource(resource.GetAddressOf());
    Ptr<ID3D11Texture2D> texture;
    check(resource.As(&texture), "The window target is not a 2D texture");
    auto out = std::make_shared<D3D11Texture>(texture_desc(texture.Get()));
    out->texture_ = texture;
    out->targets_.emplace_back(view);
    return out;
}

Display create_window_display(HWND window) {
    RECT client{};
    if (!GetClientRect(window, &client)) throw std::runtime_error("Cannot read 3D viewport dimensions");
    const auto width = static_cast<unsigned>(client.right);
    const auto height = static_cast<unsigned>(client.bottom);
    if (!width || !height || width > max_viewport || height > max_viewport)
        throw std::runtime_error("Unsupported 3D viewport dimensions");
    DXGI_SWAP_CHAIN_DESC swap{};
    swap.BufferDesc.Width = width;
    swap.BufferDesc.Height = height;
    swap.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swap.SampleDesc.Count = 1;
    swap.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap.BufferCount = 2;
    swap.OutputWindow = window;
    swap.Windowed = TRUE;
    swap.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL requested[]{D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL level{};
    Ptr<IDXGISwapChain> swap_chain;
    Ptr<ID3D11Device> device;
    Ptr<ID3D11DeviceContext> context;
    auto create = [&](D3D_DRIVER_TYPE type) {
        return D3D11CreateDeviceAndSwapChain(nullptr, type, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, requested, 1,
                                             D3D11_SDK_VERSION, &swap, swap_chain.ReleaseAndGetAddressOf(),
                                             device.ReleaseAndGetAddressOf(), &level, context.ReleaseAndGetAddressOf());
    };
    auto result = create(D3D_DRIVER_TYPE_HARDWARE);
    std::string backend = "D3D11 hardware";
    if (FAILED(result)) {
        std::cerr << "Hardware D3D11 unavailable; trying the WARP software renderer.\n";
        result = create(D3D_DRIVER_TYPE_WARP);
        backend = "D3D11 WARP";
    }
    check(result, "Create D3D11 device and swap chain");
    const std::string adapter = adapter_name(device.Get());
    if (!adapter.empty()) backend += " (" + adapter + ")";
    Display out;
    out.device = wrap_device(device, context, backend);
    out.swap_chain = wrap_swap_chain(out.device, swap_chain);
    out.backend = backend;
    out.owned = true;
    return out;
}

std::shared_ptr<Device> create_offscreen_device(const char* label) {
    Ptr<ID3D11Device> device;
    Ptr<ID3D11DeviceContext> context;
    auto create = [&](D3D_DRIVER_TYPE type) {
        return D3D11CreateDevice(nullptr, type, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                                 device.ReleaseAndGetAddressOf(), nullptr, context.ReleaseAndGetAddressOf());
    };
    HRESULT result = create(D3D_DRIVER_TYPE_HARDWARE);
    std::string name = "D3D11 hardware";
    if (FAILED(result)) {
        std::cerr << "Hardware D3D11 unavailable; trying the WARP software renderer.\n";
        result = create(D3D_DRIVER_TYPE_WARP);
        name = "D3D11 WARP";
    }
    check(result, label);
    return wrap_device(device, context, name);
}

ID3D11Device* native_device(const Device& device) { return static_cast<const D3D11Device&>(device).device_.Get(); }
ID3D11DeviceContext* native_context(const Device& device) { return static_cast<const D3D11Device&>(device).context_.Get(); }
ID3D11Texture2D* native_texture(const Texture& texture) { return static_cast<const D3D11Texture&>(texture).texture_.Get(); }

}  // namespace omega::gfx::d3d11

// Port-only (132vk1): gfx::create_offscreen_device (the chosen PC backend's) is in gfx_pc_backend.cpp.
