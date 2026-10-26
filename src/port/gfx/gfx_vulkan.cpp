// Port-only (checkpoint 132vk1): the PC's Vulkan backend of gfx_device.hpp. See gfx_vulkan.hpp and research/VULKAN.md.
//
// How the D3D11 interface maps to Vulkan (research/VULKAN.md "Design"):
// - Execution: one queue; every submitted list is recorded into the current batch's command buffer in order, and a
//   batch is submitted at a present, a read-back, wait_idle or a flush. Resource creation with initial data records
//   its upload into the batch's "init" command buffer, submitted ahead of the batch's main one.
// - Images live in VK_IMAGE_LAYOUT_GENERAL from creation (the swap chain's own images excepted); a full memory
//   barrier separates every render pass and transfer from the work after it, so D3D11's implicit hazard tracking is
//   kept without per-resource state.
// - Render passes are dynamic rendering, begun lazily at the first draw after the targets change (load and store
//   always), ended by a target change, a transfer or a clear of a texture that is not attached.
// - Buffers: whole updates (Device::write, update_buffer of the whole buffer) rename the buffer to a fresh host-visible
//   slot (D3D11's map-discard renaming); partial updates of a large vertex / index buffer are GPU copies from the
//   upload ring into its device-local memory, in order with the draws.
// - Descriptors: one push-descriptor set: HLSL b<n> -> binding n, t<n> -> 16 + n, s<n> -> 32 + n (the SPIR-V's
//   register shifts, tools/build_vulkan_spirv.py). Unbound slots read a zero buffer / a transparent-black texture,
//   as an unbound D3D11 slot reads zero.
// - The D3D11 conventions: the viewport is flipped (negative height) so clip space stays D3D's; the default states
//   (null blend / depth / raster) are D3D11's (no blend; depth test LESS with writes; cull back, clockwise front);
//   a texture bound as a target is unbound from the shader slots, as D3D11 does.
#include "gfx_vulkan.hpp"
#include "gfx_vulkan_spirv.hpp"

#include "device_lost.hpp"
#include "geometry.hpp"

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_win32.h>

#include <dxgi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace omega::gfx::vulkan {
namespace {

#define OMEGA_VK_GLOBAL(X) X(vkCreateInstance) X(vkEnumerateInstanceExtensionProperties) X(vkEnumerateInstanceLayerProperties) \
    X(vkEnumerateInstanceVersion)
#define OMEGA_VK_INSTANCE(X) X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceFeatures) X(vkGetPhysicalDeviceFeatures2) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFormatProperties) \
    X(vkCreateDevice) X(vkGetDeviceProcAddr) X(vkEnumerateDeviceExtensionProperties) X(vkCreateWin32SurfaceKHR) \
    X(vkDestroySurfaceKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR)
#define OMEGA_VK_DEVICE(X) X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkQueueSubmit) X(vkQueueWaitIdle) \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkResetCommandPool) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) \
    X(vkGetFenceStatus) X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkCreateImage) \
    X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) \
    X(vkCreateSampler) X(vkDestroySampler) X(vkCreateShaderModule) X(vkDestroyShaderModule) \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
    X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) X(vkCmdBeginRendering) X(vkCmdEndRendering) \
    X(vkCmdPipelineBarrier) X(vkCmdBindPipeline) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdSetBlendConstants) \
    X(vkCmdBindVertexBuffers2) X(vkCmdBindIndexBuffer) X(vkCmdDraw) X(vkCmdDrawIndexed) X(vkCmdCopyBuffer) \
    X(vkCmdCopyBufferToImage) X(vkCmdCopyImage) X(vkCmdCopyImageToBuffer) X(vkCmdClearColorImage) \
    X(vkCmdClearDepthStencilImage) X(vkCmdClearAttachments) X(vkCmdBlitImage) X(vkCmdFillBuffer) \
    X(vkCmdPushDescriptorSetKHR) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) \
    X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) X(vkCmdSetDepthTestEnable) X(vkCmdSetDepthWriteEnable) \
    X(vkCmdSetDepthCompareOp) X(vkCmdSetCullMode) X(vkCmdSetFrontFace) X(vkCmdSetColorBlendEnableEXT) X(vkCmdSetColorBlendEquationEXT) \
    X(vkCmdSetColorWriteMaskEXT)
#define OMEGA_VK_DECLARE(name) PFN_##name name = nullptr;

std::string result_text(VkResult r) {
    switch (r) {
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    default: return "VkResult " + std::to_string(static_cast<int>(r));
    }
}

// The process's Vulkan instance (shared by the window's device and the offscreen ones), over the driver's loader.
struct Instance {
    HMODULE library = nullptr;
    PFN_vkGetInstanceProcAddr get = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    bool win32_surface = false;
    OMEGA_VK_GLOBAL(OMEGA_VK_DECLARE)
    OMEGA_VK_INSTANCE(OMEGA_VK_DECLARE)
    ~Instance() {
        if (instance && vkDestroyInstance) vkDestroyInstance(instance, nullptr);
        if (library) FreeLibrary(library);
    }
};

std::shared_ptr<Instance> shared_instance() {
    static std::mutex m;
    static std::weak_ptr<Instance> cache;
    std::lock_guard lock(m);
    if (auto p = cache.lock()) return p;
    auto in = std::make_shared<Instance>();
    in->library = LoadLibraryW(L"vulkan-1.dll");
    if (!in->library) throw std::runtime_error("no Vulkan loader (vulkan-1.dll) on this machine");
    in->get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(reinterpret_cast<void*>(GetProcAddress(in->library, "vkGetInstanceProcAddr")));
    if (!in->get) throw std::runtime_error("vulkan-1.dll has no vkGetInstanceProcAddr");
#define OMEGA_VK_LOAD_GLOBAL(name) in->name = reinterpret_cast<PFN_##name>(in->get(VK_NULL_HANDLE, #name));
    OMEGA_VK_GLOBAL(OMEGA_VK_LOAD_GLOBAL)
#undef OMEGA_VK_LOAD_GLOBAL
    std::uint32_t version = VK_API_VERSION_1_0;
    if (in->vkEnumerateInstanceVersion) in->vkEnumerateInstanceVersion(&version);
    if (version < VK_API_VERSION_1_3)
        throw std::runtime_error("the Vulkan loader is " + std::to_string(VK_API_VERSION_MAJOR(version)) + "." +
                                 std::to_string(VK_API_VERSION_MINOR(version)) + " (1.3 needed)");
    std::uint32_t count = 0;
    in->vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    in->vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data());
    const auto has = [&](const char* name) {
        return std::any_of(available.begin(), available.end(), [&](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
    };
    std::vector<const char*> extensions;
    if (has(VK_KHR_SURFACE_EXTENSION_NAME) && has(VK_KHR_WIN32_SURFACE_EXTENSION_NAME)) {
        extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
        extensions.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
        in->win32_surface = true;
    }
    // Port-only diagnostic: OMEGA_VK_VALIDATION=1 enables the Khronos validation layer when it is installed.
    std::vector<const char*> layers;
    if (const char* v = std::getenv("OMEGA_VK_VALIDATION"); v && *v == '1') {
        in->vkEnumerateInstanceLayerProperties(&count, nullptr);
        std::vector<VkLayerProperties> found(count);
        in->vkEnumerateInstanceLayerProperties(&count, found.data());
        for (const auto& l : found)
            if (std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) layers.push_back("VK_LAYER_KHRONOS_validation");
        std::cout << "Vulkan (port-only): validation layer " << (layers.empty() ? "not installed" : "on") << "\n";
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Syphon Filter: The Omega Strain (native port)";
    app.pEngineName = "omega";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    info.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    info.ppEnabledLayerNames = layers.data();
    const VkResult r = in->vkCreateInstance(&info, nullptr, &in->instance);
    if (r != VK_SUCCESS) throw std::runtime_error("vkCreateInstance failed (" + result_text(r) + ")");
#define OMEGA_VK_LOAD_INSTANCE(name) in->name = reinterpret_cast<PFN_##name>(in->get(in->instance, #name));
    OMEGA_VK_INSTANCE(OMEGA_VK_LOAD_INSTANCE)
#undef OMEGA_VK_LOAD_INSTANCE
    cache = in;
    return in;
}

// The adapter D3D11CreateDevice(nullptr, ...) uses: DXGI's first (the system's choice for this program), so both
// backends draw on the same GPU of a hybrid laptop.
bool default_adapter_luid(LUID& out) {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) return false;
    IDXGIAdapter1* adapter = nullptr;
    bool ok = false;
    if (SUCCEEDED(factory->EnumAdapters1(0, &adapter))) {
        DXGI_ADAPTER_DESC1 d{};
        if (SUCCEEDED(adapter->GetDesc1(&d))) {
            out = d.AdapterLuid;
            ok = true;
        }
        adapter->Release();
    }
    factory->Release();
    return ok;
}

struct FormatInfo {
    VkFormat format;
    unsigned bytes;  // per texel, or per 4 x 4 block when block
    bool block;
};
FormatInfo format_info(Format f, VkFormat depth_format) {
    switch (f) {
    case Format::rgba8: return {VK_FORMAT_R8G8B8A8_UNORM, 4, false};
    case Format::bgra8: return {VK_FORMAT_B8G8R8A8_UNORM, 4, false};
    case Format::r8: return {VK_FORMAT_R8_UNORM, 1, false};
    case Format::r32f: return {VK_FORMAT_R32_SFLOAT, 4, false};
    case Format::d24s8: return {depth_format, 4, false};
    case Format::bc1: return {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 8, true};
    case Format::bc3: return {VK_FORMAT_BC3_UNORM_BLOCK, 16, true};
    case Format::unknown: break;
    }
    throw std::runtime_error("gfx vulkan: a texture of an unknown format");
}

// ---- Device memory: per (memory type, linear / optimal) 64 MiB blocks, each with a list of its free ranges (first fit,
// coalesced on release, so the renderers' per-frame resources reuse the space of the ones they released); a block is
// freed when empty unless it is its pool's last. Large requests get a block of their own.
struct MemBlock {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    unsigned live = 0;
    std::uint8_t* mapped = nullptr;
    std::uint32_t pool = 0;
    bool dedicated = false;
    std::map<VkDeviceSize, VkDeviceSize> free;  // offset -> bytes
};
struct Allocation {
    MemBlock* block = nullptr;
    VkDeviceSize offset = 0, size = 0;
    std::uint8_t* ptr() const { return block && block->mapped ? block->mapped + offset : nullptr; }
};
// ---- The backend's objects.
struct Core;

class VkBuf final : public Buffer {
public:
    VkBuf(std::shared_ptr<Core> core, const BufferDesc& desc) : owner_(core.get()), core_(std::move(core)) { desc_ = desc; }
    ~VkBuf() override;
    struct Slot {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        std::uint8_t* ptr = nullptr;
        std::uint64_t retire = 0;
    };
    Core* owner_;                  // outlives every object it made (the objects' handles are freed through it)
    std::shared_ptr<Core> core_;   // keeps it alive while this object exists
    bool home = false;  // device-local, updated by GPU copies
    VkBuffer home_buffer = VK_NULL_HANDLE;
    Allocation home_memory;
    VkDeviceSize slot_size = 0;
    std::vector<std::pair<VkBuffer, Allocation>> blocks;
    std::deque<Slot> free;
    unsigned slots = 0;
    Slot current;
    std::vector<std::uint8_t> shadow;  // the current bytes for partial updates of slot buffers
    bool shadow_stale = false;         // a whole update since: the slot holds the current bytes
    const void* cpu_mirror = nullptr;
    VkBuffer buffer() const { return home ? home_buffer : current.buffer; }
    VkDeviceSize offset() const { return home ? 0 : current.offset; }
};

class VkTex final : public Texture {
public:
    VkTex(std::shared_ptr<Core> core, const TextureDesc& desc) : owner_(core.get()), core_(std::move(core)) { desc_ = desc; }
    ~VkTex() override;
    Core* owner_;
    std::shared_ptr<Core> core_;  // null for the device's own dummies (no cycle)
    VkImage image = VK_NULL_HANDLE;
    Allocation memory;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    unsigned bytes = 4;
    bool block = false;
    VkImageView sampled = VK_NULL_HANDLE;
    std::vector<VkImageView> targets;  // per layer, mip 0
    VkImageView depth = VK_NULL_HANDLE;
    // Usage::staging: a host-visible buffer of every subresource (tight rows).
    bool staging = false;
    VkBuffer staging_buffer = VK_NULL_HANDLE;
    Allocation staging_memory;
    std::vector<VkDeviceSize> sub_offsets;
    unsigned width(unsigned mip) const { return std::max(1u, desc_.width >> mip); }
    unsigned height(unsigned mip) const { return std::max(1u, desc_.height >> mip); }
    unsigned row_pitch(unsigned mip) const { return width(mip) * bytes; }
};

// A released object leaves the recording state (D3D11's context holds a reference to what it has bound; here a later
// list's draw must not read a released object through a slot it did not set): forget() clears it from every slot.
void forget(Core* core, const void* object);
template <class Base, class Desc> class VkState final : public Base {
public:
    VkState(std::shared_ptr<Core> core, const Desc& d, std::uint64_t state_id) : desc(d), id(state_id), core_(std::move(core)) {}
    ~VkState() override { forget(core_.get(), this); }
    Desc desc;
    std::uint64_t id;  // the same for equal descriptions (D3D11 hands back one object for them): the pipeline key
    std::shared_ptr<Core> core_;
};
using VkBlend = VkState<BlendState, BlendDesc>;
using VkDepth = VkState<DepthState, DepthDesc>;
using VkRaster = VkState<RasterState, RasterDesc>;
class VkSamp final : public SamplerState {
public:
    explicit VkSamp(std::shared_ptr<Core> core) : owner_(core.get()), core_(std::move(core)) {}
    ~VkSamp() override;
    Core* owner_;
    std::shared_ptr<Core> core_;
    VkSampler sampler = VK_NULL_HANDLE;
};

template <class T> const T* as(const void* object) {
    if constexpr (std::is_base_of_v<RasterState, T>) return static_cast<const T*>(static_cast<const RasterState*>(object));
    else if constexpr (std::is_base_of_v<DepthState, T>) return static_cast<const T*>(static_cast<const DepthState*>(object));
    else if constexpr (std::is_base_of_v<BlendState, T>) return static_cast<const T*>(static_cast<const BlendState*>(object));
    else return static_cast<const T*>(static_cast<const SamplerState*>(object));
}
const VkTex* tex(const void* object) { return object ? static_cast<const VkTex*>(static_cast<const Texture*>(object)) : nullptr; }
VkBuf* buf(const void* object) {
    return object ? const_cast<VkBuf*>(static_cast<const VkBuf*>(static_cast<const Buffer*>(object))) : nullptr;
}

// The renderers' vertex structs by D3D semantic (gfx_d3d11_shaders.cpp's input layouts).
struct Element {
    const char* semantic;
    unsigned index;
    VkFormat format;
    std::uint32_t offset;
};
struct Layout {
    const Element* elements;
    unsigned count;
    std::uint32_t stride;
};
static_assert(sizeof(MeshVertex) == 60);
const Element mesh_elements[]{
    {"POSITION", 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, position)},
    {"NORMAL", 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, normal)},
    {"TEXCOORD", 0, VK_FORMAT_R32G32_SFLOAT, offsetof(MeshVertex, uv)},
    {"COLOR", 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(MeshVertex, color)},
    {"TEXCOORD", 1, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, gore)},
};
const Element gui_elements[]{
    {"POSITION", 0, VK_FORMAT_R32G32_SFLOAT, 0},
    {"TEXCOORD", 0, VK_FORMAT_R32G32_SFLOAT, 8},
    {"COLOR", 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16},
    {"TEXCOORD", 1, VK_FORMAT_R32G32_SFLOAT, 32},
};
const Element particle_elements[]{
    {"POSITION", 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
    {"TEXCOORD", 0, VK_FORMAT_R32G32_SFLOAT, 12},
    {"COLOR", 0, VK_FORMAT_R32G32B32A32_SFLOAT, 20},
};
const Element clip_elements[]{
    {"POSITION", 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
    {"TEXCOORD", 0, VK_FORMAT_R32G32_SFLOAT, 16},
    {"COLOR", 0, VK_FORMAT_R32G32B32A32_SFLOAT, 24},
};
const Element haze_elements[]{
    {"POSITION", 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
    {"TEXCOORD", 0, VK_FORMAT_R32G32_SFLOAT, 12},
    {"TEXCOORD", 1, VK_FORMAT_R32G32_SFLOAT, 20},
    {"COLOR", 0, VK_FORMAT_R32G32B32A32_SFLOAT, 28},
};
const Element corona_elements[]{
    {"POSITION", 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
    {"TEXCOORD", 0, VK_FORMAT_R32G32_SFLOAT, 16},
    {"COLOR", 0, VK_FORMAT_R32G32B32A32_SFLOAT, 24},
    {"TEXCOORD", 1, VK_FORMAT_R32G32B32A32_SFLOAT, 40},
};
const Element skinned_elements[]{
    {"POSITION", 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
    {"NORMAL", 0, VK_FORMAT_R32G32B32_SFLOAT, 12},
    {"TEXCOORD", 0, VK_FORMAT_R32G32_SFLOAT, 24},
    {"BLENDINDICES", 0, VK_FORMAT_R8G8B8A8_UINT, 32},
    {"BLENDINDICES", 1, VK_FORMAT_R8G8B8A8_UINT, 36},
    {"BLENDWEIGHT", 0, VK_FORMAT_R32G32B32A32_SFLOAT, 40},
    {"BLENDWEIGHT", 1, VK_FORMAT_R32G32B32A32_SFLOAT, 56},
};
#define OMEGA_LAYOUT(e, stride) Layout{e, static_cast<unsigned>(std::size(e)), stride}
const Layout layouts[]{
    OMEGA_LAYOUT(mesh_elements, 60), OMEGA_LAYOUT(gui_elements, 40), OMEGA_LAYOUT(particle_elements, 36),
    OMEGA_LAYOUT(clip_elements, 40), OMEGA_LAYOUT(haze_elements, 44), OMEGA_LAYOUT(corona_elements, 56),
    OMEGA_LAYOUT(skinned_elements, 72),
};
#undef OMEGA_LAYOUT
static_assert(std::size(layouts) == static_cast<std::size_t>(VertexLayout::count));

VkBlendFactor vk_blend(BlendFactor f) {
    switch (f) {
    case BlendFactor::zero: return VK_BLEND_FACTOR_ZERO;
    case BlendFactor::one: return VK_BLEND_FACTOR_ONE;
    case BlendFactor::src_alpha: return VK_BLEND_FACTOR_SRC_ALPHA;
    case BlendFactor::inv_src_alpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case BlendFactor::factor: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case BlendFactor::inv_factor: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    }
    return VK_BLEND_FACTOR_ONE;
}
VkBlendOp vk_op(BlendOp op) { return op == BlendOp::rev_subtract ? VK_BLEND_OP_REVERSE_SUBTRACT : VK_BLEND_OP_ADD; }
VkCompareOp vk_compare(Compare c) {
    switch (c) {
    case Compare::never: return VK_COMPARE_OP_NEVER;
    case Compare::less: return VK_COMPARE_OP_LESS;
    case Compare::less_equal: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case Compare::always: return VK_COMPARE_OP_ALWAYS;
    }
    return VK_COMPARE_OP_ALWAYS;
}

constexpr unsigned batch_count = 3;
constexpr VkDeviceSize block_bytes = 64ull << 20, chunk_bytes = 8ull << 20, home_threshold = 256u << 10;
constexpr VkDeviceSize zero_buffer_bytes = 64u << 10;

// The device: every Vulkan object and the recording state. One recursive mutex guards it (resources are made on the
// main thread while the render thread submits, as D3D11's device is free-threaded beside its immediate context).
struct Core : std::enable_shared_from_this<Core> {
    std::shared_ptr<Instance> in;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t family = 0;
    OMEGA_VK_DEVICE(OMEGA_VK_DECLARE)
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memory_properties{};
    VkFormat depth_format = VK_FORMAT_D24_UNORM_S8_UINT;
    bool bc = false;
    bool eds3 = false;   // VK_EXT_extended_dynamic_state3's blend enable / equation / write mask
    std::string name;
    std::recursive_mutex m;
    bool lost = false;

    // Memory.
    std::vector<std::unique_ptr<MemBlock>> blocks;
    std::uint32_t host_type = 0, device_type = 0;

    // Batches.
    struct Chunk {
        VkBuffer buffer = VK_NULL_HANDLE;
        Allocation memory;
        VkDeviceSize size = 0, used = 0;
    };
    struct Batch {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer init = VK_NULL_HANDLE, main = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore acquire = VK_NULL_HANDLE;
        std::uint64_t serial = 0;
        bool submitted = false, init_used = false, main_used = false;
        std::vector<Chunk> chunks;
    };
    std::array<Batch, batch_count> batches{};
    unsigned batch_index = 0;
    std::uint64_t serial = 0, completed = 0;
    // The current batch's size: a long stretch without a present or a read-back (hidden runs do not present) is submitted
    // in parts, as D3D11's driver flushes its command buffer when it fills.
    std::size_t batch_commands = 0;
    VkDeviceSize batch_upload = 0;
    std::vector<Chunk> spare_chunks;
    std::vector<std::pair<std::uint64_t, std::function<void()>>> garbage;

    // Fixed objects.
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkBuffer zero_buffer = VK_NULL_HANDLE;
    Allocation zero_memory;
    std::shared_ptr<VkTex> dummy_2d, dummy_array;
    VkSampler default_sampler = VK_NULL_HANDLE;
    std::map<const std::uint32_t*, VkShaderModule> modules;
    std::array<bool, program_count> prepared{};
    struct PipelineKey {
        std::uint8_t program, colour_count;
        std::uint64_t blend, depth, raster;
        VkFormat colour[2], depth_format;
        bool operator<(const PipelineKey& o) const {
            return std::tie(program, colour_count, blend, depth, raster, colour[0], colour[1], depth_format) <
                   std::tie(o.program, o.colour_count, o.blend, o.depth, o.raster, o.colour[0], o.colour[1], o.depth_format);
        }
    };
    std::map<PipelineKey, VkPipeline> pipelines;
    // State descriptions to ids: equal descriptions share an id, so a renderer that makes its states again each frame
    // reuses the cached pipelines.
    std::map<std::vector<std::uint8_t>, std::uint64_t> state_ids;
    std::uint64_t intern(std::vector<std::uint8_t> key) { return state_ids.try_emplace(std::move(key), state_ids.size() + 1).first->second; }

    // Recording state (the D3D11 immediate context's).
    struct Target {
        const VkTex* texture = nullptr;
        unsigned layer = 0;
        bool operator==(const Target& o) const { return texture == o.texture && layer == o.layer; }
    };
    std::array<Target, 2> colour{};
    unsigned colour_count = 0;
    const VkTex* depth_target = nullptr;
    bool in_pass = false, need_barrier = false;
    std::array<Target, 2> pass_colour{};
    unsigned pass_colour_count = 0;
    const VkTex* pass_depth = nullptr;
    VkExtent2D pass_extent{};
    std::optional<Program> program;
    const VkBlend* blend = nullptr;
    std::array<float, 4> blend_factor{1, 1, 1, 1};
    const VkDepth* depth_state = nullptr;
    const VkRaster* raster = nullptr;
    Viewport viewport{};
    bool viewport_set = false;
    VkBuf* vertex_buffer = nullptr;
    std::uint32_t vertex_stride = 0, vertex_offset = 0;
    VkBuf* index_buffer = nullptr;
    std::array<VkBuf*, 4> constants{};
    std::array<const VkTex*, 4> textures{};
    std::array<const VkSamp*, 2> samplers{};
    // What the current command buffer has bound.
    VkPipeline bound_pipeline = VK_NULL_HANDLE;
    bool pipeline_dirty = true;
    // What changed since the last draw (port-only performance: a draw re-checks only these).
    bool state_dirty = true, bindings_dirty = true, vertex_dirty = true, targets_dirty = true;
    void touched(const VkBuf* b) {   // a buffer renamed or rewritten: its bindings point elsewhere now
        for (const VkBuf* c : constants)
            if (c == b) bindings_dirty = true;
        if (b == vertex_buffer || b == index_buffer) vertex_dirty = true;
    }
    bool viewport_dirty = true, factor_dirty = true;
    // The dynamic state the current command buffer has set.
    struct Dynamic {
        bool valid = false, blend_set = false;
        unsigned depth_set = 0, cull_set = 0, blend_count = 0;
        VkBool32 test = VK_FALSE, write = VK_FALSE;
        VkCompareOp op = VK_COMPARE_OP_NEVER;
        VkCullModeFlags cull = 0;
        std::array<VkBool32, 2> enable{};
        std::array<VkColorBlendEquationEXT, 2> eq{};
        std::array<VkColorComponentFlags, 2> mask{};
    } dyn;
    VkBuffer bound_vertex = VK_NULL_HANDLE;
    VkDeviceSize bound_vertex_offset = 0;
    std::uint32_t bound_stride = 0;
    VkBuffer bound_index = VK_NULL_HANDLE;
    VkDeviceSize bound_index_offset = 0;
    struct Pushed {
        std::array<VkBuffer, 4> buffers{};
        std::array<VkDeviceSize, 4> offsets{}, ranges{};
        std::array<VkImageView, 4> views{};
        std::array<VkSampler, 2> samplers{};
        bool valid = false;
        bool operator==(const Pushed& o) const {
            return valid == o.valid && buffers == o.buffers && offsets == o.offsets && ranges == o.ranges && views == o.views &&
                   samplers == o.samplers;
        }
    } pushed;

    ~Core();

    // Port-only diagnostic: OMEGA_VK_STATS=<n> logs the device's memory and caches every n presents / part submits.
    void log_stats(const char* where) {
        static const unsigned every = [] {
            const char* v = std::getenv("OMEGA_VK_STATS");
            return v ? static_cast<unsigned>(std::strtoul(v, nullptr, 10)) : 0u;
        }();
        if (!every || ++stats_events % every) return;
        VkDeviceSize bytes = 0, free_bytes = 0;
        for (const auto& b : blocks) {
            bytes += b->size;
            for (const auto& [at, n] : b->free) free_bytes += n;
        }
        std::size_t chunks = spare_chunks.size();
        for (const auto& b : batches) chunks += b.chunks.size();
        std::cout << "Vulkan (port-only diagnostic): " << where << " " << stats_events << ": " << blocks.size() << " memory blocks, "
                  << (bytes >> 20) << " MiB (" << (free_bytes >> 20) << " MiB free), " << pipelines.size() << " pipelines, "
                  << state_ids.size() << " state descriptions, " << garbage.size() << " objects to release, " << chunks
                  << " upload chunks, serial " << serial << " completed " << completed << "; since the last line: " << counters.draws
                  << " draws, " << counters.passes << " passes, " << counters.barriers << " barriers, " << counters.buffer_copies
                  << " buffer copies, " << counters.image_transfers << " image transfers, " << counters.clears << " clears, "
                  << counters.reads << " read-backs, " << counters.submits << " submits, " << counters.wait_ms << " ms waiting for the GPU; " << counters.pushes << " pushes " << counters.push_ms << " ms, "
                  << counters.updates << " updates " << counters.update_ms << " ms, draw calls " << counters.draw_ms << " ms, execute "
                  << counters.execute_ms << " ms (prepare " << counters.prepare_ms << ", pass " << counters.pass_ms << ", pipeline "
                  << counters.pipeline_ms << ", binds " << counters.bind_ms << " ms); " << counters.lookups << " pipeline lookups, " << counters.pipeline_binds
                  << " pipeline binds, " << counters.viewports << " viewports, " << counters.factors << " blend factors; dynamic " << counters.dynamic_ms
                  << " ms, want " << counters.want_ms << " ms; per op (count ms):";
        for (std::size_t k = 0; k < counters.op_ms.size(); ++k)
            if (counters.op_count[k]) std::cout << " " << k << ":" << counters.op_count[k] << "/" << std::lround(counters.op_ms[k]);
        std::cout << "\n";
        counters = {};
    }
    std::uint64_t stats_events = 0;
    // The diagnostic's timers run only when OMEGA_VK_STATS is set (else now() is a constant: no clock reads per draw).
    const bool timing = std::getenv("OMEGA_VK_STATS") != nullptr;
    std::chrono::steady_clock::time_point now() const {
        return timing ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    }
    struct Counters {
        std::uint64_t draws = 0, passes = 0, barriers = 0, buffer_copies = 0, image_transfers = 0, clears = 0, reads = 0,
                      submits = 0;
        double wait_ms = 0;
        std::array<double, 32> op_ms{};
        std::array<std::uint64_t, 32> op_count{};
        double dynamic_ms = 0, want_ms = 0;
        double push_ms = 0, update_ms = 0, draw_ms = 0, execute_ms = 0, prepare_ms = 0, pass_ms = 0, pipeline_ms = 0, bind_ms = 0;
        std::uint64_t pushes = 0, updates = 0, pipeline_binds = 0, lookups = 0, viewports = 0, factors = 0;
    } counters;
    // ---- Errors.
    void check(VkResult r, const char* label) {
        if (r == VK_ERROR_DEVICE_LOST) {
            lost = true;
            check_device(DXGI_ERROR_DEVICE_REMOVED, label);
        }
        if (r < 0) throw std::runtime_error(std::string(label) + " failed (" + result_text(r) + ")");
    }
    // The device-loss test hook at the entry points D3D11 checks (an injected loss throws DeviceLost).
    void hook(const char* label) {
        check_device(S_OK, label);
        if (lost) check_device(DXGI_ERROR_DEVICE_REMOVED, label);
    }

    // ---- Memory.
    std::uint32_t memory_type(std::uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags prefer) const {
        for (int pass = 0; pass < 2; ++pass)
            for (std::uint32_t k = 0; k < memory_properties.memoryTypeCount; ++k) {
                const auto flags = memory_properties.memoryTypes[k].propertyFlags;
                if (!(bits & (1u << k)) || (flags & want) != want) continue;
                if (pass == 0 && (flags & prefer) != prefer) continue;
                return k;
            }
        throw std::runtime_error("gfx vulkan: no suitable memory type");
    }
    Allocation allocate(const VkMemoryRequirements& req, VkMemoryPropertyFlags want, VkMemoryPropertyFlags prefer, bool linear,
                        const char* label) {
        const std::uint32_t type = memory_type(req.memoryTypeBits, want, prefer);
        const std::uint32_t pool = type * 2 + (linear ? 1 : 0);
        const bool host = (memory_properties.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
        auto make_block = [&](VkDeviceSize size) {
            auto b = std::make_unique<MemBlock>();
            VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            info.allocationSize = size;
            info.memoryTypeIndex = type;
            check(vkAllocateMemory(device, &info, nullptr, &b->memory), label);
            b->size = size;
            b->pool = pool;
            if (host) {
                void* p = nullptr;
                check(vkMapMemory(device, b->memory, 0, VK_WHOLE_SIZE, 0, &p), label);
                b->mapped = static_cast<std::uint8_t*>(p);
            }
            blocks.push_back(std::move(b));
            return blocks.back().get();
        };
        if (req.size > block_bytes / 2) {
            MemBlock* b = make_block(req.size);
            b->dedicated = true;
            b->live = 1;
            return {b, 0, req.size};
        }
        const auto carve = [&](MemBlock* b) -> std::optional<Allocation> {
            for (auto it = b->free.begin(); it != b->free.end(); ++it) {
                const VkDeviceSize start = it->first, end = it->first + it->second;
                const VkDeviceSize at = (start + req.alignment - 1) / req.alignment * req.alignment;
                if (at + req.size > end) continue;
                b->free.erase(it);
                if (at > start) b->free[start] = at - start;
                if (at + req.size < end) b->free[at + req.size] = end - (at + req.size);
                ++b->live;
                return Allocation{b, at, req.size};
            }
            return std::nullopt;
        };
        for (const auto& b : blocks)
            if (b->pool == pool && !b->dedicated)
                if (auto a = carve(b.get())) return *a;
        MemBlock* b = make_block(block_bytes);
        b->free[0] = block_bytes;
        return *carve(b);
    }
    void release(Allocation a) {
        if (!a.block) return;
        MemBlock* b = a.block;
        if (!b->dedicated) {
            // The range back to the free list, joined to its neighbours.
            VkDeviceSize start = a.offset, end = a.offset + a.size;
            auto next = b->free.lower_bound(start);
            if (next != b->free.begin()) {
                auto prev = std::prev(next);
                if (prev->first + prev->second == start) {
                    start = prev->first;
                    b->free.erase(prev);
                }
            }
            next = b->free.lower_bound(end);
            if (next != b->free.end() && next->first == end) {
                end += next->second;
                b->free.erase(next);
            }
            b->free[start] = end - start;
        }
        if (--b->live) return;
        if (!b->dedicated) {
            const auto others = std::count_if(blocks.begin(), blocks.end(), [&](const auto& p) { return p->pool == b->pool && !p->dedicated; });
            if (others <= 1) return;  // the pool's last block is kept
        }
        vkFreeMemory(device, b->memory, nullptr);
        blocks.erase(std::remove_if(blocks.begin(), blocks.end(), [&](const auto& p) { return p.get() == b; }), blocks.end());
    }    VkBuffer make_buffer(VkDeviceSize bytes, VkBufferUsageFlags usage, bool host, Allocation& memory, const char* label) {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = bytes;
        info.usage = usage;
        VkBuffer b = VK_NULL_HANDLE;
        check(vkCreateBuffer(device, &info, nullptr, &b), label);
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device, b, &req);
        try {
            memory = host ? allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0, true, label)
                          : allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, true, label);
            check(vkBindBufferMemory(device, b, memory.block->memory, memory.offset), label);
        } catch (...) {
            vkDestroyBuffer(device, b, nullptr);
            throw;
        }
        return b;
    }
    // Destroys `f`'s objects once the GPU has finished every batch recorded so far.
    void retire(std::function<void()> f) {
        std::lock_guard lock(m);
        garbage.emplace_back(serial, std::move(f));
    }
    void collect() {
        std::vector<std::function<void()>> run;
        for (auto it = garbage.begin(); it != garbage.end();) {
            if (it->first <= completed) {
                run.push_back(std::move(it->second));
                it = garbage.erase(it);
            } else {
                ++it;
            }
        }
        for (auto& f : run) f();
    }

    // ---- Batches.
    Batch& batch() { return batches[batch_index]; }
    VkCommandBuffer cb() {
        batch().main_used = true;
        return batch().main;
    }
    VkCommandBuffer init_cb() {
        batch().init_used = true;
        return batch().init;
    }
    void wait_batch(Batch& b) {
        if (!b.submitted) return;
        const auto wait_begin = now();
        const VkResult r = vkWaitForFences(device, 1, &b.fence, VK_TRUE, UINT64_MAX);
        counters.wait_ms += std::chrono::duration<double, std::milli>(now() - wait_begin).count();
        if (r == VK_ERROR_DEVICE_LOST) lost = true;
        if (r != VK_SUCCESS && r != VK_ERROR_DEVICE_LOST) check(r, "Wait for the GPU");
        b.submitted = false;
        completed = std::max(completed, b.serial);
        for (auto& c : b.chunks) {
            if (c.size == chunk_bytes) {
                c.used = 0;
                spare_chunks.push_back(c);
            } else {
                const VkBuffer buffer = c.buffer;
                vkDestroyBuffer(device, buffer, nullptr);
                release(c.memory);
            }
        }
        b.chunks.clear();
    }
    void begin_batch() {
        Batch& b = batch();
        wait_batch(b);
        collect();
        check(vkResetFences(device, 1, &b.fence), "Reset a fence");
        check(vkResetCommandPool(device, b.pool, 0), "Reset a command pool");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(b.init, &begin), "Begin a command buffer");
        check(vkBeginCommandBuffer(b.main, &begin), "Begin a command buffer");
        b.serial = ++serial;
        b.init_used = b.main_used = false;
        batch_commands = 0;
        batch_upload = 0;
        // A new command buffer: nothing bound.
        bound_pipeline = VK_NULL_HANDLE;
        pipeline_dirty = state_dirty = bindings_dirty = vertex_dirty = targets_dirty = true;
        viewport_dirty = factor_dirty = true;
        dyn = {};
        bound_vertex = bound_index = VK_NULL_HANDLE;
        bound_stride = 0;
        pushed.valid = false;
        need_barrier = true;
    }
    // Submits the current batch (waiting `wait` at `stage`, signalling `signal`) and begins the next one; answers
    // the submitted batch's serial.
    std::uint64_t submit_batch(VkSemaphore wait = VK_NULL_HANDLE, VkPipelineStageFlags stage = 0, VkSemaphore signal = VK_NULL_HANDLE) {
        end_pass();
        Batch& b = batch();
        check(vkEndCommandBuffer(b.init), "End a command buffer");
        check(vkEndCommandBuffer(b.main), "End a command buffer");
        VkCommandBuffer list[2];
        std::uint32_t n = 0;
        if (b.init_used) list[n++] = b.init;
        list[n++] = b.main;
        VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        info.commandBufferCount = n;
        info.pCommandBuffers = list;
        if (wait) {
            info.waitSemaphoreCount = 1;
            info.pWaitSemaphores = &wait;
            info.pWaitDstStageMask = &stage;
        }
        if (signal) {
            info.signalSemaphoreCount = 1;
            info.pSignalSemaphores = &signal;
        }
        ++counters.submits;
        const VkResult r = vkQueueSubmit(queue, 1, &info, b.fence);
        b.submitted = r == VK_SUCCESS;
        const std::uint64_t done = b.serial;
        batch_index = (batch_index + 1) % batch_count;
        begin_batch();
        check(r, "Submit to the GPU");
        return done;
    }
    void wait_serial(std::uint64_t s) {
        for (auto& b : batches)
            if (b.submitted && b.serial <= s) wait_batch(b);
        completed = std::max(completed, std::min(s, serial - 1));
        collect();
        if (lost) check(VK_ERROR_DEVICE_LOST, "Wait for the GPU");
    }
    // The submitted batches' progress, without waiting.
    void poll() {
        for (auto& b : batches)
            if (b.submitted && vkGetFenceStatus(device, b.fence) == VK_SUCCESS) wait_batch(b);
    }

    // Upload ring: host-visible bytes valid until the current batch completes.
    struct Slice {
        VkBuffer buffer;
        VkDeviceSize offset;
        std::uint8_t* ptr;
    };
    Slice upload(VkDeviceSize bytes, VkDeviceSize align, const char* label) {
        batch_upload += bytes;
        Batch& b = batch();
        if (!b.chunks.empty()) {
            Chunk& c = b.chunks.back();
            const VkDeviceSize at = (c.used + align - 1) / align * align;
            if (at + bytes <= c.size) {
                c.used = at + bytes;
                return {c.buffer, at, c.memory.ptr() + at};
            }
        }
        Chunk c;
        if (bytes <= chunk_bytes && !spare_chunks.empty()) {
            c = spare_chunks.back();
            spare_chunks.pop_back();
        } else {
            c.size = std::max(chunk_bytes, bytes);
            c.buffer = make_buffer(c.size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, c.memory, label);
        }
        c.used = bytes;
        b.chunks.push_back(c);
        return {c.buffer, 0, c.memory.ptr()};
    }

    // ---- Barriers and passes.
    void barrier(VkCommandBuffer c) {
        ++counters.barriers;
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0,
                             nullptr);
    }
    void sync() {
        if (!need_barrier) return;
        barrier(cb());
        need_barrier = false;
    }
    void end_pass() {
        if (!in_pass) return;
        vkCmdEndRendering(cb());
        in_pass = false;
        need_barrier = true;
    }
    // Begins rendering into (count colour targets, depth); false when there is nothing to draw into.
    bool begin_rendering(const Target* targets, unsigned count, const VkTex* depth) {
        std::array<VkRenderingAttachmentInfo, 2> colours{};
        VkRenderingAttachmentInfo depth_info{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        VkExtent2D extent{~0u, ~0u};
        bool any = false;
        for (unsigned k = 0; k < count; ++k) {
            auto& a = colours[k];
            a.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            a.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            if (const VkTex* t = targets[k].texture) {
                if (targets[k].layer >= t->targets.size()) throw std::runtime_error("gfx vulkan: a texture without that render target");
                a.imageView = t->targets[targets[k].layer];
                extent.width = std::min(extent.width, t->desc().width);
                extent.height = std::min(extent.height, t->desc().height);
                any = true;
            }
        }
        if (depth) {
            depth_info.imageView = depth->depth;
            depth_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            depth_info.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            depth_info.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            extent.width = std::min(extent.width, depth->desc().width);
            extent.height = std::min(extent.height, depth->desc().height);
            any = true;
        }
        if (!any) return false;
        sync();
        VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
        info.renderArea = {{0, 0}, extent};
        info.layerCount = 1;
        info.colorAttachmentCount = count;
        info.pColorAttachments = colours.data();
        if (depth) {
            info.pDepthAttachment = &depth_info;
            if (depth->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) info.pStencilAttachment = &depth_info;
        }
        ++counters.passes;
        vkCmdBeginRendering(cb(), &info);
        const VkRect2D scissor{{0, 0}, extent};
        vkCmdSetScissor(cb(), 0, 1, &scissor);
        pass_extent = extent;
        return true;
    }
    bool ensure_pass() {
        if (in_pass && !targets_dirty) return true;
        targets_dirty = false;
        state_dirty = true;   // the blend attachments follow the pass
        if (in_pass && pass_colour_count == colour_count && pass_colour == colour && pass_depth == depth_target) return true;
        end_pass();
        if (!begin_rendering(colour.data(), colour_count, depth_target)) return false;
        pipeline_dirty = true;   // the pipeline's attachment formats
        in_pass = true;
        pass_colour = colour;
        pass_colour_count = colour_count;
        pass_depth = depth_target;
        return true;
    }
    bool attached(const VkTex* t) const {
        if (!t) return false;
        for (unsigned k = 0; k < colour_count; ++k)
            if (colour[k].texture == t) return true;
        return depth_target == t;
    }

    // ---- Pipelines.
    VkShaderModule module(const SpirvStage& s) {
        auto& m = modules[s.code];
        if (!m) {
            VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            info.codeSize = s.words * 4;
            info.pCode = s.code;
            check(vkCreateShaderModule(device, &info, nullptr, &m), "Create a shader module");
        }
        return m;
    }
    VkPipeline pipeline(Program p) {
        PipelineKey key{};
        key.program = static_cast<std::uint8_t>(p);
        key.colour_count = static_cast<std::uint8_t>(pass_colour_count);
        key.blend = eds3 || !blend ? 0 : blend->id;   // dynamic with VK_EXT_extended_dynamic_state3
        key.depth = 0;                                // dynamic (core 1.3)
        key.raster = raster && raster->desc.wireframe ? 1 : 0;   // the polygon mode (culling is dynamic)
        for (unsigned k = 0; k < 2; ++k)
            key.colour[k] = k < pass_colour_count && pass_colour[k].texture ? pass_colour[k].texture->format : VK_FORMAT_UNDEFINED;
        key.depth_format = pass_depth ? pass_depth->format : VK_FORMAT_UNDEFINED;
        auto& slot = pipelines[key];
        if (slot) return slot;
        const SpirvProgram& sp = spirv_program(p);
        std::vector<VkPipelineShaderStageCreateInfo> stages;
        const auto add = [&](const SpirvStage& s, VkShaderStageFlagBits bit) {
            if (!s.code) return;
            VkPipelineShaderStageCreateInfo st{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            st.stage = bit;
            st.module = module(s);
            st.pName = s.entry;
            stages.push_back(st);
        };
        add(sp.vertex, VK_SHADER_STAGE_VERTEX_BIT);
        add(sp.geometry, VK_SHADER_STAGE_GEOMETRY_BIT);
        add(sp.pixel, VK_SHADER_STAGE_FRAGMENT_BIT);
        const Layout& layout = layouts[static_cast<std::size_t>(program_layout(p))];
        VkVertexInputBindingDescription binding{0, layout.stride, VK_VERTEX_INPUT_RATE_VERTEX};
        std::vector<VkVertexInputAttributeDescription> attributes;
        for (unsigned k = 0; k < sp.input_count; ++k) {
            const SpirvInput& in = sp.inputs[k];
            const Element* e = std::find_if(layout.elements, layout.elements + layout.count, [&](const Element& x) {
                return std::strcmp(x.semantic, in.semantic) == 0 && x.index == in.index;
            });
            if (e == layout.elements + layout.count)
                throw std::runtime_error(std::string("gfx vulkan: the vertex layout has no ") + in.semantic + std::to_string(in.index));
            attributes.push_back({in.location, 0, e->format, e->offset});
        }
        VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vertex.vertexBindingDescriptionCount = 1;
        vertex.pVertexBindingDescriptions = &binding;
        vertex.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
        vertex.pVertexAttributeDescriptions = attributes.data();
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport_state{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport_state.viewportCount = viewport_state.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = raster && raster->desc.wireframe ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
        // D3D11's default rasterizer state (no state set): back faces culled, clockwise front; a state: no culling.
        rs.cullMode = raster ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
        rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
        rs.lineWidth = 1.0F;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        // D3D11's default depth state: test LESS with writes.
        const DepthDesc d = depth_state ? depth_state->desc : DepthDesc{true, true, Compare::less};
        ds.depthTestEnable = d.test ? VK_TRUE : VK_FALSE;
        ds.depthWriteEnable = d.test && d.write ? VK_TRUE : VK_FALSE;
        ds.depthCompareOp = vk_compare(d.compare);
        std::array<VkPipelineColorBlendAttachmentState, 2> attachments{};
        const BlendDesc b = blend ? blend->desc : BlendDesc{};
        for (unsigned k = 0; k < 2; ++k) {
            const TargetBlend& t = b.targets[b.independent ? k : 0];
            auto& a = attachments[k];
            a.blendEnable = t.enable ? VK_TRUE : VK_FALSE;
            a.srcColorBlendFactor = vk_blend(t.src);
            a.dstColorBlendFactor = vk_blend(t.dst);
            a.colorBlendOp = vk_op(t.op);
            a.srcAlphaBlendFactor = vk_blend(t.src_alpha);
            a.dstAlphaBlendFactor = vk_blend(t.dst_alpha);
            a.alphaBlendOp = vk_op(t.op_alpha);
            a.colorWriteMask = t.write_mask & 15u;
        }
        VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cbs.attachmentCount = pass_colour_count;
        cbs.pAttachments = attachments.data();
        std::vector<VkDynamicState> dynamic_states{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                                                   VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
                                                   VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
                                                   VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE};
        if (eds3)
            dynamic_states.insert(dynamic_states.end(), {VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT, VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT,
                                                         VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT});
        VkPipelineDynamicStateCreateInfo dyn_info{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dyn_info.dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size());
        dyn_info.pDynamicStates = dynamic_states.data();
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = pass_colour_count;
        rendering.pColorAttachmentFormats = key.colour;
        rendering.depthAttachmentFormat = key.depth_format;
        if (pass_depth && (pass_depth->aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) rendering.stencilAttachmentFormat = key.depth_format;
        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        info.pNext = &rendering;
        info.stageCount = static_cast<std::uint32_t>(stages.size());
        info.pStages = stages.data();
        info.pVertexInputState = &vertex;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport_state;
        info.pRasterizationState = &rs;
        info.pMultisampleState = &ms;
        info.pDepthStencilState = &ds;
        info.pColorBlendState = &cbs;
        info.pDynamicState = &dyn_info;
        info.layout = pipeline_layout;
        check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &slot), "Create a graphics pipeline");
        return slot;
    }

    // ---- Buffers.
    void rename(VkBuf& b) {
        if (b.free.empty() || b.free.front().retire > completed) {
            // A new block: as many slots as the buffer has (doubling), at least two.
            const unsigned count = std::max(2u, b.slots);
            Allocation memory;
            const VkBuffer block = make_buffer(b.slot_size * count,
                                               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                                   VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                               true, memory, "Create a buffer");
            b.blocks.emplace_back(block, memory);
            for (unsigned k = 0; k < count; ++k)
                b.free.push_front({block, b.slot_size * k, memory.ptr() + b.slot_size * k, 0});
            b.slots += count;
        }
        if (b.current.buffer) {
            b.current.retire = serial;
            b.free.push_back(b.current);
        }
        b.current = b.free.front();
        b.free.pop_front();
    }
    void copy_to_home(VkBuf& b, const void* data, VkDeviceSize bytes, VkDeviceSize offset) {
        if (!bytes) return;
        end_pass();
        sync();
        const Slice s = upload(bytes, 16, "Upload buffer data");
        std::memcpy(s.ptr, data, bytes);
        const VkBufferCopy region{s.offset, offset, bytes};
        ++counters.buffer_copies;
        vkCmdCopyBuffer(cb(), s.buffer, b.home_buffer, 1, &region);
        need_barrier = true;
    }
    void update(VkBuf& b, const void* data, std::size_t bytes, std::size_t offset, bool partial) {
        const std::size_t size = b.desc().bytes;
        if (!partial) offset = 0;
        if (offset >= size) return;
        bytes = std::min(bytes, size - offset);
        if (b.home) {
            copy_to_home(b, data, bytes, offset);
            return;
        }
        if (partial) {
            // The rest of the buffer keeps its bytes: the shadow (refreshed from the slot after whole updates) patched.
            if (b.shadow.size() != size) {
                b.shadow.assign(size, 0);
                b.shadow_stale = b.current.ptr != nullptr;
            }
            if (b.shadow_stale) std::memcpy(b.shadow.data(), b.current.ptr, size);
            b.shadow_stale = false;
            std::memcpy(b.shadow.data() + offset, data, bytes);
            rename(b);
            std::memcpy(b.current.ptr, b.shadow.data(), size);
            return;
        }
        rename(b);
        std::memcpy(b.current.ptr, data, bytes);
        b.shadow_stale = true;
    }

    // ---- Draws.
    bool prepare_draw(bool indexed) {
        if (!program || !viewport_set || viewport.width <= 0 || viewport.height <= 0) return false;
        if (!vertex_buffer) return false;
        if (indexed && !index_buffer) return false;
        auto tp = now();
        if (!ensure_pass()) return false;
        auto tq = now();
        counters.pass_ms += std::chrono::duration<double, std::milli>(tq - tp).count();
        VkCommandBuffer c = cb();
        if (pipeline_dirty) {   // the program, a state object or the targets changed since the last draw
            ++counters.lookups;
            const VkPipeline p = pipeline(*program);
            if (p != bound_pipeline) {
                ++counters.pipeline_binds;
                vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
                bound_pipeline = p;
            }
            pipeline_dirty = false;
        }
        tp = now();
        counters.pipeline_ms += std::chrono::duration<double, std::milli>(tp - tq).count();
        if (viewport_dirty) {
            // D3D11's viewport, flipped so clip space keeps D3D's +Y up (VK_KHR_maintenance1, core).
            const VkViewport v{viewport.x, viewport.y + viewport.height, viewport.width, -viewport.height, viewport.min_depth,
                               viewport.max_depth};
            ++counters.viewports;
            vkCmdSetViewport(c, 0, 1, &v);
            viewport_dirty = false;
        }
        // Dynamic state (port-only performance, research/VULKAN.md "Design"): depth test / write / compare and culling
        // (core 1.3), and blending (VK_EXT_extended_dynamic_state3 when present) are set per draw only when they change,
        // so the pipelines are per program and targets and a state change costs no pipeline switch.
        if (state_dirty) {
            state_dirty = false;
            const DepthDesc d = depth_state ? depth_state->desc : DepthDesc{true, true, Compare::less};
            const VkBool32 test = d.test ? VK_TRUE : VK_FALSE, write = d.test && d.write ? VK_TRUE : VK_FALSE;
            const VkCompareOp op = vk_compare(d.compare);
            const VkCullModeFlags cull = raster ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;   // D3D11's default: back faces
            if (!dyn.valid) {
                vkCmdSetFrontFace(c, VK_FRONT_FACE_CLOCKWISE);
                dyn.valid = true;
                dyn.depth_set = dyn.cull_set = dyn.blend_count = 0;
                dyn.blend_set = false;
            }
            if (!dyn.depth_set || dyn.test != test) vkCmdSetDepthTestEnable(c, dyn.test = test);
            if (!dyn.depth_set || dyn.write != write) vkCmdSetDepthWriteEnable(c, dyn.write = write);
            if (!dyn.depth_set || dyn.op != op) vkCmdSetDepthCompareOp(c, dyn.op = op);
            dyn.depth_set = 1;
            if (!dyn.cull_set || dyn.cull != cull) vkCmdSetCullMode(c, dyn.cull = cull);
            dyn.cull_set = 1;
            const BlendDesc b = blend ? blend->desc : BlendDesc{};
            bool uses_factor = false;
            std::array<VkBool32, 2> enable{};
            std::array<VkColorBlendEquationEXT, 2> eq{};
            std::array<VkColorComponentFlags, 2> mask{};
            for (unsigned k = 0; k < pass_colour_count; ++k) {
                const TargetBlend& tb = b.targets[b.independent ? k : 0];
                enable[k] = tb.enable ? VK_TRUE : VK_FALSE;
                eq[k] = {vk_blend(tb.src), vk_blend(tb.dst), vk_op(tb.op), vk_blend(tb.src_alpha), vk_blend(tb.dst_alpha), vk_op(tb.op_alpha)};
                mask[k] = tb.write_mask & 15u;
                const auto factor_of = [](BlendFactor x) { return x == BlendFactor::factor || x == BlendFactor::inv_factor; };
                if (tb.enable && (factor_of(tb.src) || factor_of(tb.dst) || factor_of(tb.src_alpha) || factor_of(tb.dst_alpha)))
                    uses_factor = true;
            }
            if (eds3 && pass_colour_count) {
                const auto same_eq = [](const VkColorBlendEquationEXT& x, const VkColorBlendEquationEXT& y) {
                    return std::memcmp(&x, &y, sizeof x) == 0;
                };
                bool changed = !dyn.blend_set || dyn.blend_count != pass_colour_count;
                for (unsigned k = 0; k < pass_colour_count && !changed; ++k)
                    changed = dyn.enable[k] != enable[k] || !same_eq(dyn.eq[k], eq[k]) || dyn.mask[k] != mask[k];
                if (changed) {
                    vkCmdSetColorBlendEnableEXT(c, 0, pass_colour_count, enable.data());
                    vkCmdSetColorBlendEquationEXT(c, 0, pass_colour_count, eq.data());
                    vkCmdSetColorWriteMaskEXT(c, 0, pass_colour_count, mask.data());
                    dyn.enable = enable;
                    dyn.eq = eq;
                    dyn.mask = mask;
                    dyn.blend_count = pass_colour_count;
                    dyn.blend_set = true;
                }
            }
            // The blend factor only matters to a blend that reads it (D3D11 keeps it with the state; here it is set when used).
            if (uses_factor && factor_dirty) {
                ++counters.factors;
                vkCmdSetBlendConstants(c, blend_factor.data());
                factor_dirty = false;
            }
        }
        const auto t_dynamic = now();
        counters.dynamic_ms += std::chrono::duration<double, std::milli>(t_dynamic - tp).count();
        // Descriptors.
        if (bindings_dirty) {
        bindings_dirty = false;
        Pushed want;
        want.valid = true;
        for (unsigned k = 0; k < 4; ++k) {
            if (VkBuf* b = constants[k]) {
                want.buffers[k] = b->buffer();
                want.offsets[k] = b->offset();
                want.ranges[k] = std::min<VkDeviceSize>(b->desc().bytes, properties.limits.maxUniformBufferRange);
            } else {
                want.buffers[k] = zero_buffer;
                want.offsets[k] = 0;
                want.ranges[k] = zero_buffer_bytes;
            }
            const VkTex* t = textures[k];
            const bool array = *program == Program::shadow_receiver && k == 1;
            want.views[k] = t && t->sampled ? t->sampled : (array ? dummy_array->sampled : dummy_2d->sampled);
        }
        for (unsigned k = 0; k < 2; ++k) want.samplers[k] = samplers[k] ? samplers[k]->sampler : default_sampler;
        counters.want_ms += std::chrono::duration<double, std::milli>(now() - t_dynamic).count();
        if (!(want == pushed)) {
            // Only the bindings that changed (push descriptors keep the rest; the whole set after a new command buffer).
            std::array<VkDescriptorBufferInfo, 4> bi{};
            std::array<VkDescriptorImageInfo, 4> ii{};
            std::array<VkDescriptorImageInfo, 2> si{};
            std::array<VkWriteDescriptorSet, 10> writes{};
            std::uint32_t n = 0;
            const bool all = !pushed.valid;
            for (unsigned k = 0; k < 4; ++k) {
                if (all || want.buffers[k] != pushed.buffers[k] || want.offsets[k] != pushed.offsets[k] || want.ranges[k] != pushed.ranges[k]) {
                    bi[k] = {want.buffers[k], want.offsets[k], want.ranges[k]};
                    auto& w = writes[n++];
                    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    w.dstBinding = binding_constants + k;
                    w.descriptorCount = 1;
                    w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                    w.pBufferInfo = &bi[k];
                }
                if (all || want.views[k] != pushed.views[k]) {
                    ii[k] = {VK_NULL_HANDLE, want.views[k], VK_IMAGE_LAYOUT_GENERAL};
                    auto& w = writes[n++];
                    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    w.dstBinding = binding_textures + k;
                    w.descriptorCount = 1;
                    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                    w.pImageInfo = &ii[k];
                }
            }
            for (unsigned k = 0; k < 2; ++k)
                if (all || want.samplers[k] != pushed.samplers[k]) {
                    si[k] = {want.samplers[k], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
                    auto& w = writes[n++];
                    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    w.dstBinding = binding_samplers + k;
                    w.descriptorCount = 1;
                    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
                    w.pImageInfo = &si[k];
                }
            const auto push_begin = now();
            if (n) vkCmdPushDescriptorSetKHR(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, n, writes.data());
            ++counters.pushes;
            counters.push_ms += std::chrono::duration<double, std::milli>(now() - push_begin).count();
            pushed = want;
        }
        }
        tq = now();
        if (vertex_dirty || (indexed && bound_index == VK_NULL_HANDLE)) {
        vertex_dirty = false;
        const VkBuffer vb = vertex_buffer->buffer();
        const VkDeviceSize vo = vertex_buffer->offset() + vertex_offset;
        if (vb != bound_vertex || vo != bound_vertex_offset || vertex_stride != bound_stride) {
            const VkDeviceSize stride = vertex_stride;
            vkCmdBindVertexBuffers2(c, 0, 1, &vb, &vo, nullptr, &stride);
            bound_vertex = vb;
            bound_vertex_offset = vo;
            bound_stride = vertex_stride;
        }
        if (indexed) {
            const VkBuffer ib = index_buffer->buffer();
            const VkDeviceSize io = index_buffer->offset();
            if (ib != bound_index || io != bound_index_offset) {
                vkCmdBindIndexBuffer(c, ib, io, VK_INDEX_TYPE_UINT32);
                bound_index = ib;
                bound_index_offset = io;
            }
        }
        }
        counters.bind_ms += std::chrono::duration<double, std::milli>(now() - tq).count();
        return true;
    }

    // ---- Transfers.
    void clear_colour(const VkTex* t, unsigned layer, const float rgba[4]) {
        if (in_pass && pass_colour_count == colour_count && pass_colour == colour) {
            for (unsigned k = 0; k < pass_colour_count; ++k)
                if (pass_colour[k].texture == t && pass_colour[k].layer == layer) {
                    VkClearAttachment a{VK_IMAGE_ASPECT_COLOR_BIT, k, {}};
                    std::memcpy(a.clearValue.color.float32, rgba, sizeof(float) * 4);
                    const VkClearRect r{{{0, 0}, pass_extent}, 0, 1};
                    vkCmdClearAttachments(cb(), 1, &a, 1, &r);
                    return;
                }
        }
        end_pass();
        sync();
        VkClearColorValue v{};
        std::memcpy(v.float32, rgba, sizeof(float) * 4);
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layer, 1};
        ++counters.clears;
        vkCmdClearColorImage(cb(), t->image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &range);
        need_barrier = true;
    }
    void clear_depth(const VkTex* t, std::uint8_t flags, float value, std::uint8_t stencil) {
        VkImageAspectFlags aspect = 0;
        if (flags & CommandList::clear_depth_bit) aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
        if ((flags & CommandList::clear_stencil_bit) && (t->aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
        if (!aspect) return;
        if (in_pass && pass_depth == t && pass_colour_count == colour_count && pass_colour == colour && depth_target == t) {
            VkClearAttachment a{aspect, 0, {}};
            a.clearValue.depthStencil = {value, stencil};
            const VkClearRect r{{{0, 0}, pass_extent}, 0, 1};
            vkCmdClearAttachments(cb(), 1, &a, 1, &r);
            return;
        }
        end_pass();
        sync();
        const VkClearDepthStencilValue v{value, stencil};
        const VkImageSubresourceRange range{aspect, 0, 1, 0, 1};
        vkCmdClearDepthStencilImage(cb(), t->image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &range);
        need_barrier = true;
    }
    // D3D11.1 ClearView: rectangles of a target (no rectangles: the whole target).
    void clear_rects(const VkTex* t, const float rgba[4], const Rect* rects, unsigned count) {
        end_pass();
        const Target target{t, 0};
        if (!begin_rendering(&target, 1, nullptr)) return;
        VkClearAttachment a{VK_IMAGE_ASPECT_COLOR_BIT, 0, {}};
        std::memcpy(a.clearValue.color.float32, rgba, sizeof(float) * 4);
        std::vector<VkClearRect> out;
        if (count == 0) out.push_back({{{0, 0}, pass_extent}, 0, 1});
        for (unsigned k = 0; k < count; ++k) {
            const int l = std::max(0, rects[k].left), tp = std::max(0, rects[k].top);
            const int r = std::min<int>(rects[k].right, static_cast<int>(pass_extent.width));
            const int b = std::min<int>(rects[k].bottom, static_cast<int>(pass_extent.height));
            if (r > l && b > tp)
                out.push_back({{{l, tp}, {static_cast<std::uint32_t>(r - l), static_cast<std::uint32_t>(b - tp)}}, 0, 1});
        }
        if (!out.empty()) vkCmdClearAttachments(cb(), 1, &a, static_cast<std::uint32_t>(out.size()), out.data());
        vkCmdEndRendering(cb());
        need_barrier = true;
    }
    void copy_image(const VkTex* dst, unsigned dst_sub, unsigned x, unsigned y, const VkTex* src, unsigned src_sub, const Box* box) {
        ++counters.image_transfers;
        end_pass();
        sync();
        const unsigned smip = src_sub % src->desc().mips, slayer = src_sub / src->desc().mips;
        const unsigned dmip = dst_sub % dst->desc().mips, dlayer = dst_sub / dst->desc().mips;
        Box b = box ? *box : Box{0, 0, 0, src->width(smip), src->height(smip), 1};
        if (b.right <= b.left || b.bottom <= b.top) return;
        const VkExtent3D extent{b.right - b.left, b.bottom - b.top, 1};
        if (dst->staging) {
            VkBufferImageCopy r{};
            r.bufferOffset = dst->sub_offsets[dst_sub] + (VkDeviceSize(y) * dst->width(dmip) + x) * dst->bytes;
            r.bufferRowLength = dst->width(dmip);
            r.imageSubresource = {static_cast<VkImageAspectFlags>(src->aspect & VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT), smip,
                                  slayer, 1};
            r.imageOffset = {static_cast<std::int32_t>(b.left), static_cast<std::int32_t>(b.top), 0};
            r.imageExtent = extent;
            vkCmdCopyImageToBuffer(cb(), src->image, VK_IMAGE_LAYOUT_GENERAL, dst->staging_buffer, 1, &r);
        } else {
            VkImageCopy r{};
            r.srcSubresource = {src->aspect, smip, slayer, 1};
            r.srcOffset = {static_cast<std::int32_t>(b.left), static_cast<std::int32_t>(b.top), 0};
            r.dstSubresource = {dst->aspect, dmip, dlayer, 1};
            r.dstOffset = {static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), 0};
            r.extent = extent;
            vkCmdCopyImage(cb(), src->image, VK_IMAGE_LAYOUT_GENERAL, dst->image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
        }
        need_barrier = true;
    }
    void copy_whole(const VkTex* dst, const VkTex* src) {
        const unsigned mips = std::min(dst->desc().mips, src->desc().mips), layers = std::min(dst->desc().layers, src->desc().layers);
        for (unsigned layer = 0; layer < layers; ++layer)
            for (unsigned mip = 0; mip < mips; ++mip)
                copy_image(dst, subresource(mip, layer, dst->desc().mips), 0, 0, src, subresource(mip, layer, src->desc().mips), nullptr);
    }
    void update_texture(const VkTex* t, const Box& b, const std::uint8_t* data, unsigned row_pitch) {
        ++counters.image_transfers;
        if (b.right <= b.left || b.bottom <= b.top) return;
        end_pass();
        sync();
        const unsigned rows = b.bottom - b.top;
        const VkDeviceSize bytes = VkDeviceSize(rows) * row_pitch;
        const Slice s = upload(bytes, 16, "Upload a texture box");
        std::memcpy(s.ptr, data, bytes);
        VkBufferImageCopy r{};
        r.bufferOffset = s.offset;
        r.bufferRowLength = row_pitch / t->bytes;
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.imageOffset = {static_cast<std::int32_t>(b.left), static_cast<std::int32_t>(b.top), 0};
        r.imageExtent = {b.right - b.left, rows, 1};
        vkCmdCopyBufferToImage(cb(), s.buffer, t->image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
        need_barrier = true;
    }

    // ---- Lists.
    void execute(CommandList& list);
    void reset_bindings() {
        end_pass();
        colour = {};
        colour_count = 0;
        depth_target = nullptr;
        program.reset();
        blend = nullptr;
        blend_factor = {1, 1, 1, 1};
        factor_dirty = true;
        depth_state = nullptr;
        raster = nullptr;
        pipeline_dirty = state_dirty = bindings_dirty = vertex_dirty = targets_dirty = true;
        viewport_set = false;
        vertex_buffer = index_buffer = nullptr;
        constants = {};
        textures = {};
        samplers = {};
    }

    // ---- Resources.
    std::shared_ptr<VkTex> make_texture(const TextureDesc& desc, const Level* levels, const TextureLabels& labels);
};

void Core::execute(CommandList& list) {
    for (const Command& k : list.commands()) {
        const auto op_begin = now();
        struct OpTimer {
            Core& c;
            std::size_t op;
            std::chrono::steady_clock::time_point begin;
            ~OpTimer() {
                c.counters.op_ms[op] += std::chrono::duration<double, std::milli>(c.now() - begin).count();
                ++c.counters.op_count[op];
            }
        } op_timer{*this, static_cast<std::size_t>(k.op), op_begin};
        switch (k.op) {
        case Command::Op::set_targets: {
            targets_dirty = bindings_dirty = true;
            colour = {};
            colour_count = std::min<unsigned>(k.count, 2);
            for (unsigned n = 0; n < colour_count; ++n) colour[n] = {tex(k.objects[n]), k.layers[n]};
            depth_target = tex(k.objects[2]);
            // D3D11 unbinds a shader resource that is now a target.
            for (auto& t : textures)
                if (attached(t)) t = nullptr;
            break;
        }
        case Command::Op::set_viewport:
            viewport = {k.values[0], k.values[1], k.values[2], k.values[3], k.values[4], k.values[5]};
            viewport_set = true;
            viewport_dirty = true;
            break;
        case Command::Op::clear_target: clear_colour(tex(k.objects[0]), k.layers[0], k.values); break;
        case Command::Op::clear_depth: clear_depth(tex(k.objects[0]), k.flags, k.values[0], static_cast<std::uint8_t>(k.a)); break;
        case Command::Op::clear_rects: {
            Rect rects[2]{};
            std::memcpy(rects, list.arena(k.c), sizeof(Rect) * std::min<unsigned>(k.count, 2));
            clear_rects(tex(k.objects[0]), k.values, rects, k.count);
            break;
        }
        case Command::Op::set_raster: {
            const VkRaster* r = k.objects[0] ? as<VkRaster>(k.objects[0]) : nullptr;
            if ((r && r->desc.wireframe) != (raster && raster->desc.wireframe)) pipeline_dirty = true;   // the polygon mode
            raster = r;
            state_dirty = true;
            break;
        }
        case Command::Op::set_depth:
            depth_state = k.objects[0] ? as<VkDepth>(k.objects[0]) : nullptr;   // dynamic state: no pipeline change
            state_dirty = true;
            break;
        case Command::Op::set_blend: {
            {
                const VkBlend* bl = k.objects[0] ? as<VkBlend>(k.objects[0]) : nullptr;
                if (!eds3 && bl != blend) pipeline_dirty = true;   // with VK_EXT_extended_dynamic_state3 blending is dynamic
                blend = bl;
                state_dirty = true;
            }
            std::array<float, 4> f{1, 1, 1, 1};
            if (k.flags & 1) std::copy(k.values, k.values + 4, f.begin());
            if (f != blend_factor) {
                blend_factor = f;
                factor_dirty = true;
            }
            break;
        }
        case Command::Op::set_program:
            if (!prepared[static_cast<std::size_t>(k.program)]) throw std::runtime_error("gfx vulkan: a program used before prepare()");
            if (program != k.program) pipeline_dirty = true;
            program = k.program;
            break;
        case Command::Op::set_vertex_buffer:
            vertex_dirty = true;
            vertex_buffer = buf(k.objects[0]);
            vertex_stride = k.a;
            vertex_offset = k.b;
            break;
        case Command::Op::set_index_buffer:
            index_buffer = buf(k.objects[0]);
            vertex_dirty = true;
            break;
        case Command::Op::set_constants:
            bindings_dirty = true;
            for (unsigned n = 0; n < k.count && k.slot + n < 4; ++n) constants[k.slot + n] = buf(k.objects[n]);
            break;
        case Command::Op::set_textures:
            bindings_dirty = true;
            for (unsigned n = 0; n < k.count && k.slot + n < 4; ++n) {
                const VkTex* t = tex(k.objects[n]);
                textures[k.slot + n] = attached(t) ? nullptr : t;   // D3D11: a bound target is not a shader resource
            }
            break;
        case Command::Op::set_sampler:
            bindings_dirty = true;
            if (k.slot < 2) samplers[k.slot] = k.objects[0] ? as<VkSamp>(k.objects[0]) : nullptr;
            break;
        case Command::Op::update_buffer: {
            const auto update_begin = now();
            update(*buf(k.objects[0]), list.arena(k.a), k.b, k.c, (k.flags & 1) != 0);
            touched(buf(k.objects[0]));
            ++counters.updates;
            counters.update_ms += std::chrono::duration<double, std::milli>(now() - update_begin).count();
            break;
        }
        case Command::Op::update_texture: {
            Box b;
            std::memcpy(&b, list.arena(k.d), sizeof b);
            update_texture(tex(k.objects[0]), b, list.arena(k.a), k.b);
            break;
        }
        case Command::Op::draw:
            if (k.a && prepare_draw(false)) {
                ++counters.draws;
                vkCmdDraw(cb(), k.a, 1, k.b, 0);
            }
            break;
        case Command::Op::draw_indexed: {
            const auto prepare_begin = now();
            const bool ready = k.a && prepare_draw(true);
            counters.prepare_ms += std::chrono::duration<double, std::milli>(now() - prepare_begin).count();
            if (ready) {
                ++counters.draws;
                const auto draw_begin = now();
                vkCmdDrawIndexed(cb(), k.a, 1, k.b, 0, 0);
                counters.draw_ms += std::chrono::duration<double, std::milli>(now() - draw_begin).count();
            }
            break;
        }
        case Command::Op::copy: copy_whole(tex(k.objects[0]), tex(k.objects[1])); break;
        case Command::Op::copy_region: {
            Box b{};
            if (k.flags & 1) std::memcpy(&b, list.arena(k.slot), sizeof b);
            copy_image(tex(k.objects[0]), k.a, k.b, k.c, tex(k.objects[1]), k.d, (k.flags & 1) ? &b : nullptr);
            break;
        }
        case Command::Op::flush:
            if (batch().main_used || batch().init_used) submit_batch();
            break;
        case Command::Op::reset_state: reset_bindings(); break;
        }
    }
    list.clear();
}

std::shared_ptr<VkTex> Core::make_texture(const TextureDesc& desc, const Level* levels, const TextureLabels& labels) {
    auto out = std::make_shared<VkTex>(shared_from_this(), desc);
    const FormatInfo f = format_info(desc.format, depth_format);
    out->format = f.format;
    out->bytes = f.bytes;
    out->block = f.block;
    const bool is_depth = desc.format == Format::d24s8;
    out->aspect = is_depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_COLOR_BIT;
    if (desc.samples != 1) throw std::runtime_error("gfx vulkan: multisampled textures are not supported");
    if (desc.usage == Usage::staging) {
        // A read-back texture: a host-visible buffer of every subresource, tight rows.
        out->staging = true;
        VkDeviceSize at = 0;
        for (unsigned layer = 0; layer < desc.layers; ++layer)
            for (unsigned mip = 0; mip < desc.mips; ++mip) {
                out->sub_offsets.push_back(at);
                at += (VkDeviceSize(out->row_pitch(mip)) * out->height(mip) + 15) / 16 * 16;
            }
        out->staging_buffer = make_buffer(std::max<VkDeviceSize>(at, 16), VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, out->staging_memory,
                                          labels.texture);
        return out;
    }
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = f.format;
    info.extent = {desc.width, desc.height, 1};
    info.mipLevels = desc.mips;
    info.arrayLayers = desc.layers;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (is_depth) {
        info.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    } else {
        info.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
        if ((desc.bind & bind_target) && !f.block) info.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    check(vkCreateImage(device, &info, nullptr, &out->image), labels.texture);
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device, out->image, &req);
    out->memory = allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, false, labels.texture);
    check(vkBindImageMemory(device, out->image, out->memory.block->memory, out->memory.offset), labels.texture);
    const auto view = [&](VkImageViewType type, VkImageAspectFlags aspect, unsigned base_mip, unsigned mips, unsigned base_layer,
                          unsigned layers, const char* label) {
        VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        v.image = out->image;
        v.viewType = type;
        v.format = f.format;
        v.subresourceRange = {aspect, base_mip, mips, base_layer, layers};
        VkImageView made = VK_NULL_HANDLE;
        check(vkCreateImageView(device, &v, nullptr, &made), label);
        return made;
    };
    if (!is_depth)
        out->sampled = view(desc.layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, 0, desc.mips,
                            0, desc.layers, labels.sampled);
    if ((desc.bind & bind_target) && !is_depth && !f.block)
        for (unsigned layer = 0; layer < desc.layers; ++layer)
            out->targets.push_back(view(VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layer, 1, labels.target));
    if (is_depth) out->depth = view(VK_IMAGE_VIEW_TYPE_2D, out->aspect, 0, 1, 0, 1, labels.depth);
    // To GENERAL (and the initial levels) in the batch's init command buffer.
    VkCommandBuffer c = init_cb();
    VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    ib.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = out->image;
    ib.subresourceRange = {out->aspect, 0, desc.mips, 0, desc.layers};
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib);
    // A texture made without its levels starts zeroed, as D3D11's do (WDDM hands out zeroed memory): the sub-allocated
    // memory here may hold an earlier resource's bytes. Every subresource, then the given levels over it.
    if (is_depth) {
        const VkClearDepthStencilValue zero{};
        vkCmdClearDepthStencilImage(c, out->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &ib.subresourceRange);
    } else if ((!levels || desc.layers > 1) && !f.block) {
        const VkClearColorValue zero{};
        vkCmdClearColorImage(c, out->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &ib.subresourceRange);
        if (levels) barrier(c);
    }
    if (levels) {
        for (unsigned mip = 0; mip < desc.mips; ++mip) {
            const unsigned w = out->width(mip), h = out->height(mip);
            const unsigned rows = f.block ? (h + 3) / 4 : h;
            const unsigned pitch = levels[mip].row_pitch;
            const VkDeviceSize bytes = VkDeviceSize(rows) * pitch;
            if (!levels[mip].data || !bytes) continue;
            const Slice s = upload(bytes, 16, labels.texture);
            std::memcpy(s.ptr, levels[mip].data, bytes);
            VkBufferImageCopy r{};
            r.bufferOffset = s.offset;
            r.bufferRowLength = f.block ? pitch / f.bytes * 4 : pitch / f.bytes;
            r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
            r.imageExtent = {w, h, 1};
            vkCmdCopyBufferToImage(c, s.buffer, out->image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
        }
    }
    barrier(c);
    return out;
}

Core::~Core() {
    if (!device) return;
    vkDeviceWaitIdle(device);
    for (auto& b : batches) {
        b.submitted = false;
        for (auto& c : b.chunks) spare_chunks.push_back(c);
        b.chunks.clear();
    }
    completed = serial;
    collect();
    dummy_2d.reset();
    dummy_array.reset();
    collect();
    for (auto& c : spare_chunks) vkDestroyBuffer(device, c.buffer, nullptr);
    for (auto& [key, p] : pipelines) vkDestroyPipeline(device, p, nullptr);
    for (auto& [key, mod] : modules) vkDestroyShaderModule(device, mod, nullptr);
    if (default_sampler) vkDestroySampler(device, default_sampler, nullptr);
    if (zero_buffer) vkDestroyBuffer(device, zero_buffer, nullptr);
    if (pipeline_layout) vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
    if (set_layout) vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
    for (auto& b : batches) {
        if (b.fence) vkDestroyFence(device, b.fence, nullptr);
        if (b.acquire) vkDestroySemaphore(device, b.acquire, nullptr);
        if (b.pool) vkDestroyCommandPool(device, b.pool, nullptr);
    }
    for (auto& b : blocks) vkFreeMemory(device, b->memory, nullptr);
    vkDestroyDevice(device, nullptr);
}

void forget(Core* core, const void* object) {
    if (!core || !object) return;
    std::lock_guard lock(core->m);
    Core& c = *core;
    for (unsigned k = 0; k < 2; ++k) {
        if (c.colour[k].texture == object) c.colour[k] = {};
        if (c.in_pass && c.pass_colour[k].texture == object) c.end_pass();
    }
    if (c.depth_target == object) c.depth_target = nullptr;
    if (c.in_pass && c.pass_depth == object) c.end_pass();
    for (auto& t : c.textures)
        if (t == object) t = nullptr;
    for (auto& s : c.samplers)
        if (s == object) s = nullptr;
    for (auto& b : c.constants)
        if (b == object) b = nullptr;
    if (c.vertex_buffer == object) c.vertex_buffer = nullptr;
    if (c.index_buffer == object) c.index_buffer = nullptr;
    if (c.blend == object) c.blend = nullptr;
    if (c.depth_state == object) c.depth_state = nullptr;
    if (c.raster == object) c.raster = nullptr;
    c.pipeline_dirty = c.state_dirty = c.bindings_dirty = c.vertex_dirty = c.targets_dirty = true;
}

VkBuf::~VkBuf() {
    Core* core = owner_;
    forget(core, this);
    std::vector<VkBuffer> buffers;
    std::vector<Allocation> memory;
    if (home_buffer) {
        buffers.push_back(home_buffer);
        memory.push_back(home_memory);
    }
    for (auto& [b, a] : blocks) {
        buffers.push_back(b);
        memory.push_back(a);
    }
    core->retire([core, buffers, memory] {
        for (auto b : buffers) core->vkDestroyBuffer(core->device, b, nullptr);
        for (auto a : memory) core->release(a);
    });
}

VkTex::~VkTex() {
    Core* core = owner_;
    forget(core, this);
    const VkImage i = image;
    const Allocation a = memory, sa = staging_memory;
    const VkBuffer sb = staging_buffer;
    std::vector<VkImageView> views = targets;
    if (sampled) views.push_back(sampled);
    if (depth) views.push_back(depth);
    core->retire([core, i, a, sa, sb, views] {
        for (auto v : views) core->vkDestroyImageView(core->device, v, nullptr);
        if (i) core->vkDestroyImage(core->device, i, nullptr);
        if (sb) core->vkDestroyBuffer(core->device, sb, nullptr);
        core->release(a);
        core->release(sa);
    });
}

VkSamp::~VkSamp() {
    Core* core = owner_;
    forget(core, this);
    const VkSampler s = sampler;
    core->retire([core, s] { core->vkDestroySampler(core->device, s, nullptr); });
}

// ---- Device creation.
struct Picked {
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    std::uint32_t family = 0;
    VkPhysicalDeviceProperties properties{};
    bool bc = false;
};

bool has_extension(Instance& in, VkPhysicalDevice p, const char* name) {
    std::uint32_t count = 0;
    in.vkEnumerateDeviceExtensionProperties(p, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> e(count);
    in.vkEnumerateDeviceExtensionProperties(p, nullptr, &count, e.data());
    return std::any_of(e.begin(), e.end(), [&](const VkExtensionProperties& x) { return std::strcmp(x.extensionName, name) == 0; });
}

Picked pick_device(Instance& in, VkSurfaceKHR surface) {
    std::uint32_t count = 0;
    in.vkEnumeratePhysicalDevices(in.instance, &count, nullptr);
    std::vector<VkPhysicalDevice> all(count);
    in.vkEnumeratePhysicalDevices(in.instance, &count, all.data());
    LUID luid{};
    const bool have_luid = default_adapter_luid(luid);
    Picked best;
    int best_score = -1;
    std::string why = "no Vulkan device";
    for (VkPhysicalDevice p : all) {
        VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        props.pNext = &id;
        in.vkGetPhysicalDeviceProperties2(p, &props);
        const std::string name = props.properties.deviceName;
        if (props.properties.apiVersion < VK_API_VERSION_1_3) {
            why = name + " supports Vulkan " + std::to_string(VK_API_VERSION_MAJOR(props.properties.apiVersion)) + "." +
                  std::to_string(VK_API_VERSION_MINOR(props.properties.apiVersion)) + " (1.3 needed)";
            continue;
        }
        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        f.pNext = &f13;
        in.vkGetPhysicalDeviceFeatures2(p, &f);
        if (!f13.dynamicRendering || !f.features.geometryShader || !f.features.independentBlend || !f.features.fillModeNonSolid) {
            why = name + " lacks dynamic rendering, geometry shaders, independent blend or wireframe";
            continue;
        }
        if (!has_extension(in, p, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME) || (surface && !has_extension(in, p, VK_KHR_SWAPCHAIN_EXTENSION_NAME))) {
            why = name + " lacks VK_KHR_push_descriptor or VK_KHR_swapchain";
            continue;
        }
        std::uint32_t families = 0;
        in.vkGetPhysicalDeviceQueueFamilyProperties(p, &families, nullptr);
        std::vector<VkQueueFamilyProperties> fp(families);
        in.vkGetPhysicalDeviceQueueFamilyProperties(p, &families, fp.data());
        std::optional<std::uint32_t> family;
        for (std::uint32_t k = 0; k < families && !family; ++k) {
            if (!(fp[k].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            VkBool32 present = VK_TRUE;
            if (surface) in.vkGetPhysicalDeviceSurfaceSupportKHR(p, k, surface, &present);
            if (present) family = k;
        }
        if (!family) {
            why = name + " has no graphics queue that presents to the window";
            continue;
        }
        int score = 1;
        if (props.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) score += 1;
        if (have_luid && id.deviceLUIDValid && std::memcmp(id.deviceLUID, &luid, sizeof luid) == 0) score += 10;
        if (score > best_score) {
            best_score = score;
            best.physical = p;
            best.family = *family;
            best.properties = props.properties;
            best.bc = f.features.textureCompressionBC == VK_TRUE;
        }
    }
    if (!best.physical) throw std::runtime_error(why);
    return best;
}

std::shared_ptr<Core> make_core(std::shared_ptr<Instance> in, VkSurfaceKHR surface) {
    const Picked picked = pick_device(*in, surface);
    auto core = std::make_shared<Core>();
    core->in = in;
    core->physical = picked.physical;
    core->family = picked.family;
    core->properties = picked.properties;
    core->bc = picked.bc;
    core->name = std::string("Vulkan (") + picked.properties.deviceName + ")";
    in->vkGetPhysicalDeviceMemoryProperties(picked.physical, &core->memory_properties);
    const float priority = 1.0F;
    VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    q.queueFamilyIndex = picked.family;
    q.queueCount = 1;
    q.pQueuePriorities = &priority;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f.pNext = &f13;
    f.features.geometryShader = VK_TRUE;
    f.features.independentBlend = VK_TRUE;
    f.features.fillModeNonSolid = VK_TRUE;
    f.features.textureCompressionBC = picked.bc ? VK_TRUE : VK_FALSE;
    // VK_EXT_extended_dynamic_state3's blend state (port-only performance; without it blending stays in the pipeline).
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT e3{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    if (has_extension(*in, picked.physical, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME)) {
        VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        query.pNext = &e3;
        in->vkGetPhysicalDeviceFeatures2(picked.physical, &query);
        core->eds3 = e3.extendedDynamicState3ColorBlendEnable && e3.extendedDynamicState3ColorBlendEquation &&
                     e3.extendedDynamicState3ColorWriteMask;
    }
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT e3_on{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    e3_on.extendedDynamicState3ColorBlendEnable = e3_on.extendedDynamicState3ColorBlendEquation =
        e3_on.extendedDynamicState3ColorWriteMask = VK_TRUE;
    if (core->eds3) f13.pNext = &e3_on;
    std::vector<const char*> extensions{VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
    if (core->eds3) extensions.push_back(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
    if (has_extension(*in, picked.physical, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.pNext = &f;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &q;
    info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    const VkResult r = in->vkCreateDevice(picked.physical, &info, nullptr, &core->device);
    if (r != VK_SUCCESS) throw std::runtime_error("vkCreateDevice failed (" + result_text(r) + ")");
#define OMEGA_VK_LOAD_DEVICE(name) core->name = reinterpret_cast<PFN_##name>(in->vkGetDeviceProcAddr(core->device, #name));
    OMEGA_VK_DEVICE(OMEGA_VK_LOAD_DEVICE)
#undef OMEGA_VK_LOAD_DEVICE
    core->vkGetDeviceQueue(core->device, picked.family, 0, &core->queue);
    // The depth format: D3D11's D24_UNORM_S8_UINT, else D32_SFLOAT_S8_UINT (research/VULKAN.md "Pixel parity").
    VkFormatProperties fp{};
    in->vkGetPhysicalDeviceFormatProperties(picked.physical, VK_FORMAT_D24_UNORM_S8_UINT, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) core->depth_format = VK_FORMAT_D32_SFLOAT_S8_UINT;
    // Batches.
    for (auto& b : core->batches) {
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pool.queueFamilyIndex = picked.family;
        core->check(core->vkCreateCommandPool(core->device, &pool, nullptr, &b.pool), "Create a command pool");
        VkCommandBufferAllocateInfo a{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        a.commandPool = b.pool;
        a.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        a.commandBufferCount = 2;
        VkCommandBuffer two[2];
        core->check(core->vkAllocateCommandBuffers(core->device, &a, two), "Allocate command buffers");
        b.init = two[0];
        b.main = two[1];
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        core->check(core->vkCreateFence(core->device, &fence, nullptr, &b.fence), "Create a fence");
        VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        core->check(core->vkCreateSemaphore(core->device, &sem, nullptr, &b.acquire), "Create a semaphore");
    }
    // The one descriptor set layout (push descriptors) and pipeline layout.
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    for (unsigned k = 0; k < 4; ++k)
        bindings.push_back({binding_constants + k, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
    for (unsigned k = 0; k < 4; ++k)
        bindings.push_back({binding_textures + k, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
    for (unsigned k = 0; k < 2; ++k)
        bindings.push_back({binding_samplers + k, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
    VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    sl.bindingCount = static_cast<std::uint32_t>(bindings.size());
    sl.pBindings = bindings.data();
    core->check(core->vkCreateDescriptorSetLayout(core->device, &sl, nullptr, &core->set_layout), "Create the descriptor set layout");
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &core->set_layout;
    core->check(core->vkCreatePipelineLayout(core->device, &pl, nullptr, &core->pipeline_layout), "Create the pipeline layout");
    // D3D11's default sampler (a null sampler slot): linear, clamp, every mip.
    VkSamplerCreateInfo s{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    s.magFilter = s.minFilter = VK_FILTER_LINEAR;
    s.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    s.addressModeU = s.addressModeV = s.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    s.maxLod = VK_LOD_CLAMP_NONE;
    core->check(core->vkCreateSampler(core->device, &s, nullptr, &core->default_sampler), "Create the default sampler");
    core->begin_batch();
    // The zero constant buffer and the transparent-black textures for unbound slots.
    core->zero_buffer = core->make_buffer(zero_buffer_bytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false,
                                          core->zero_memory, "Create the zero buffer");
    core->vkCmdFillBuffer(core->init_cb(), core->zero_buffer, 0, zero_buffer_bytes, 0);
    const std::uint32_t black = 0;
    const Level level{&black, 4};
    TextureDesc d;
    d.width = d.height = 1;
    d.bind = bind_sampled;
    d.usage = Usage::immutable;
    core->dummy_2d = core->make_texture(d, &level, {});
    d.layers = 2;
    core->dummy_array = core->make_texture(d, nullptr, {});
    core->dummy_2d->core_.reset();   // the device's own: no reference cycle
    core->dummy_array->core_.reset();
    const VkClearColorValue v{};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2};
    core->vkCmdClearColorImage(core->init_cb(), core->dummy_array->image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &range);
    core->barrier(core->init_cb());
    return core;
}

class VulkanDevice final : public Device {
public:
    explicit VulkanDevice(std::shared_ptr<Core> core) : core_(std::move(core)) {}
    ~VulkanDevice() override {
        try {
            std::lock_guard lock(core_->m);
            core_->reset_bindings();
            if (!core_->lost) {
                const auto s = core_->submit_batch();
                core_->wait_serial(s);
            }
        } catch (...) {
        }
    }
    const std::string& name() const override { return core_->name; }
    Capabilities capabilities() const override { return {.gpu_skinning = true, .block_textures = core_->bc}; }

    std::shared_ptr<Buffer> create_buffer(const BufferDesc& desc, const void* initial, const char* label) override {
        std::lock_guard lock(core_->m);
        core_->hook(label);
        auto out = std::make_shared<VkBuf>(core_, desc);
        const VkDeviceSize bytes = std::max<VkDeviceSize>(desc.bytes, 4);
        const bool geometry = (desc.bind & (bind_vertices | bind_indices)) != 0;
        out->home = desc.usage == Usage::immutable || (desc.usage == Usage::gpu && geometry && bytes > home_threshold);
        if (out->home) {
            out->home_buffer = core_->make_buffer((bytes + 3) / 4 * 4,
                                                  VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                                      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                  false, out->home_memory, label);
            VkCommandBuffer c = core_->init_cb();
            if (initial && desc.bytes) {
                const auto s = core_->upload(desc.bytes, 16, label);
                std::memcpy(s.ptr, initial, desc.bytes);
                const VkBufferCopy region{s.offset, 0, desc.bytes};
                core_->vkCmdCopyBuffer(c, s.buffer, out->home_buffer, 1, &region);
            } else {
                core_->vkCmdFillBuffer(c, out->home_buffer, 0, VK_WHOLE_SIZE, 0);
            }
            core_->barrier(c);
            return out;
        }
        const VkDeviceSize align = std::max<VkDeviceSize>(256, core_->properties.limits.minUniformBufferOffsetAlignment);
        out->slot_size = (bytes + align - 1) / align * align;
        core_->rename(*out);
        std::memset(out->current.ptr, 0, out->slot_size);
        if (initial) std::memcpy(out->current.ptr, initial, desc.bytes);
        return out;
    }
    std::shared_ptr<Texture> create_texture(const TextureDesc& desc, const Level* levels, const TextureLabels& labels) override {
        std::lock_guard lock(core_->m);
        core_->hook(labels.texture);
        return core_->make_texture(desc, levels, labels);
    }
    std::shared_ptr<BlendState> create_blend(const BlendDesc& desc, const char* label) override {
        std::lock_guard lock(core_->m);
        core_->hook(label);
        std::vector<std::uint8_t> key{'B', desc.independent};
        for (const auto& b : desc.targets)
            key.insert(key.end(), {std::uint8_t(b.enable), std::uint8_t(b.src), std::uint8_t(b.dst), std::uint8_t(b.op), std::uint8_t(b.src_alpha),
                                   std::uint8_t(b.dst_alpha), std::uint8_t(b.op_alpha), b.write_mask});
        auto out = std::make_shared<VkBlend>(core_, desc, core_->intern(std::move(key)));
        return out;
    }
    std::shared_ptr<DepthState> create_depth(const DepthDesc& desc, const char* label) override {
        std::lock_guard lock(core_->m);
        core_->hook(label);
        auto out = std::make_shared<VkDepth>(core_, desc, core_->intern({'D', desc.test, desc.write, std::uint8_t(desc.compare)}));
        return out;
    }
    std::shared_ptr<RasterState> create_raster(const RasterDesc& desc, const char* label) override {
        std::lock_guard lock(core_->m);
        core_->hook(label);
        auto out = std::make_shared<VkRaster>(core_, desc, core_->intern({'R', desc.wireframe}));
        return out;
    }
    std::shared_ptr<SamplerState> create_sampler(const SamplerDesc& desc, const char* label) override {
        std::lock_guard lock(core_->m);
        core_->hook(label);
        auto out = std::make_shared<VkSamp>(core_);
        VkSamplerCreateInfo s{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        s.magFilter = desc.filter == Filter::point ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        s.minFilter = desc.filter == Filter::linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        s.mipmapMode = desc.filter == Filter::linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        s.addressModeU = s.addressModeV = s.addressModeW =
            desc.address == Address::wrap ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        s.minLod = 0.0F;
        s.maxLod = desc.max_lod;
        core_->check(core_->vkCreateSampler(core_->device, &s, nullptr, &out->sampler), label);
        return out;
    }
    void prepare(Program program) override {
        std::lock_guard lock(core_->m);
        const SpirvProgram& p = spirv_program(program);
        core_->module(p.vertex);
        if (p.geometry.code) core_->module(p.geometry);
        core_->module(p.pixel);
        core_->prepared[static_cast<std::size_t>(program)] = true;
    }
    void submit(CommandList& list) override {
        std::lock_guard lock(core_->m);
        if (core_->lost) {
            list.clear();
            return;
        }
        core_->batch_commands += list.commands().size();
        const auto execute_begin = core_->now();
        core_->execute(list);
        core_->counters.execute_ms += std::chrono::duration<double, std::milli>(core_->now() - execute_begin).count();
        if (core_->batch_commands > 16384 || core_->batch_upload > (64u << 20)) {
            core_->submit_batch();
            core_->poll();
            core_->log_stats("part submit");
        }
    }
    void write(const Buffer& buffer, const void* data, std::size_t bytes, const char* label) override {
        std::lock_guard lock(core_->m);
        core_->hook(label);
        core_->update(*buf(&buffer), data, bytes, 0, false);
        core_->touched(buf(&buffer));
    }
    void read(const Texture& staging, unsigned sub, const std::function<void(const std::uint8_t*, unsigned)>& reader,
              const char* label) override {
        std::lock_guard lock(core_->m);
        core_->hook(label);
        const VkTex& t = static_cast<const VkTex&>(staging);
        ++core_->counters.reads;
        if (!t.staging) throw std::runtime_error("gfx vulkan: a read of a texture that is not a staging texture");
        const auto s = core_->submit_batch();
        core_->wait_serial(s);
        const unsigned mip = sub % t.desc().mips;
        reader(t.staging_memory.ptr() + t.sub_offsets.at(sub), t.row_pitch(mip));
    }
    bool removed() const override { return core_->lost; }
    bool wait_idle() override {
        std::lock_guard lock(core_->m);
        const auto s = core_->submit_batch();
        core_->wait_serial(s);
        return true;
    }
    void cpu_mirror(const Buffer& buffer, const void* data) override { buf(&buffer)->cpu_mirror = data; }

    std::shared_ptr<Core> core_;
};

class VulkanSwapChain final : public SwapChain {
public:
    VulkanSwapChain(std::shared_ptr<Core> core, HWND window, VkSurfaceKHR surface, unsigned latency)
        : core_(std::move(core)), window_(window), surface_(surface), latency_(std::max(1u, latency)) {}
    ~VulkanSwapChain() override {
        try {
            std::lock_guard lock(core_->m);
            core_->vkDeviceWaitIdle(core_->device);
            destroy_swapchain();
            back_.reset();
            core_->in->vkDestroySurfaceKHR(core_->in->instance, surface_, nullptr);
        } catch (...) {
        }
    }
    void size(unsigned& width, unsigned& height, const char*) override {
        width = back_ ? back_->desc().width : 0;
        height = back_ ? back_->desc().height : 0;
    }
    std::shared_ptr<Texture> buffer(bool, const char*, const char*) override { return back_; }
    void resize(unsigned width, unsigned height, const char* label) override {
        std::lock_guard lock(core_->m);
        make_back(width, height, label);
    }
    Presented present(unsigned sync_interval, const char* label) override {
        std::lock_guard lock(core_->m);
        Core& c = *core_;
        try {
            c.hook(label);
            c.end_pass();
            std::uint64_t serial = 0;
            bool occluded = false;
            if (IsIconic(window_) || !IsWindowVisible(window_)) {
                // A minimised window shows nothing (D3D11: DXGI_STATUS_OCCLUDED); an invisible one (hidden runs) is not
                // presented at all: its frames are read from the back buffer before the present.
                serial = c.submit_batch();
                occluded = IsIconic(window_) != 0;
            } else {
                const VkPresentModeKHR want = mode_for(sync_interval);
                VkSurfaceCapabilitiesKHR caps{};
                c.check(c.in->vkGetPhysicalDeviceSurfaceCapabilitiesKHR(c.physical, surface_, &caps), label);
                if (caps.currentExtent.width == 0 || caps.currentExtent.height == 0) {
                    serial = c.submit_batch();
                    occluded = true;
                } else {
                    if (!swapchain_ || stale_ || caps.currentExtent.width != extent_.width || caps.currentExtent.height != extent_.height ||
                        want != mode_)
                        make_swapchain(caps, want, label);
                    const unsigned repeats = std::clamp(sync_interval, 1u, 4u);
                    for (unsigned n = 0; n < repeats && swapchain_; ++n) serial = present_once(label);
                    if (!serial) serial = c.submit_batch();
                }
            }
            // Frame latency (D3D11 SetMaximumFrameLatency): at most `latency` frames queued ahead of the GPU.
            in_flight_.push_back(serial);
            while (in_flight_.size() > latency_) {
                c.wait_serial(in_flight_.front());
                in_flight_.pop_front();
            }
            c.poll();
            c.log_stats("present");
            return occluded ? Presented::occluded : Presented::ok;
        } catch (const DeviceLost&) {
            return Presented::lost;
        }
    }

    void make_back(unsigned width, unsigned height, const char* label) {
        TextureDesc d;
        d.width = std::max(1u, width);
        d.height = std::max(1u, height);
        d.format = Format::bgra8;
        d.usage = Usage::gpu;
        d.bind = bind_target;
        TextureLabels labels;
        labels.texture = label;
        back_ = core_->make_texture(d, nullptr, labels);
        // A new back buffer starts black.
        const float black[4]{0, 0, 0, 0};
        VkClearColorValue v{};
        std::memcpy(v.float32, black, sizeof black);
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        core_->vkCmdClearColorImage(core_->init_cb(), back_->image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &range);
        core_->barrier(core_->init_cb());
    }
    std::string description() const {
        return std::string("Vulkan swap chain (") + (mode_ == VK_PRESENT_MODE_FIFO_KHR ? "FIFO" : mode_ == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "immediate") +
               ", " + std::to_string(images_.size()) + " images, frame latency " + std::to_string(latency_) + ")";
    }

private:
    VkPresentModeKHR mode_for(unsigned interval) {
        if (interval) return VK_PRESENT_MODE_FIFO_KHR;
        Core& c = *core_;
        std::uint32_t count = 0;
        c.in->vkGetPhysicalDeviceSurfacePresentModesKHR(c.physical, surface_, &count, nullptr);
        std::vector<VkPresentModeKHR> modes(count);
        c.in->vkGetPhysicalDeviceSurfacePresentModesKHR(c.physical, surface_, &count, modes.data());
        for (auto m : {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR})
            if (std::find(modes.begin(), modes.end(), m) != modes.end()) return m;
        return VK_PRESENT_MODE_FIFO_KHR;
    }
    void destroy_swapchain() {
        Core& c = *core_;
        for (auto s : done_) c.vkDestroySemaphore(c.device, s, nullptr);
        done_.clear();
        if (swapchain_) c.vkDestroySwapchainKHR(c.device, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
        images_.clear();
    }
    void make_swapchain(const VkSurfaceCapabilitiesKHR& caps, VkPresentModeKHR mode, const char* label) {
        Core& c = *core_;
        c.submit_batch();
        c.check(c.vkDeviceWaitIdle(c.device), label);
        std::uint32_t count = 0;
        c.in->vkGetPhysicalDeviceSurfaceFormatsKHR(c.physical, surface_, &count, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(count);
        c.in->vkGetPhysicalDeviceSurfaceFormatsKHR(c.physical, surface_, &count, formats.data());
        VkSurfaceFormatKHR chosen = formats.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR} : formats[0];
        for (VkFormat want : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM}) {
            const auto it = std::find_if(formats.begin(), formats.end(), [&](const VkSurfaceFormatKHR& f) {
                return f.format == want && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
            });
            if (it != formats.end()) {
                chosen = *it;
                break;
            }
        }
        VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        info.surface = surface_;
        info.minImageCount = std::max(caps.minImageCount, 2u);
        if (caps.maxImageCount) info.minImageCount = std::min(info.minImageCount, caps.maxImageCount);
        info.imageFormat = chosen.format;
        info.imageColorSpace = chosen.colorSpace;
        info.imageExtent = caps.currentExtent;
        info.imageArrayLayers = 1;
        info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.preTransform = caps.currentTransform;
        info.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                                  ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                                  : static_cast<VkCompositeAlphaFlagBitsKHR>(caps.supportedCompositeAlpha & (~caps.supportedCompositeAlpha + 1));
        info.presentMode = mode;
        info.clipped = VK_TRUE;
        info.oldSwapchain = swapchain_;
        VkSwapchainKHR made = VK_NULL_HANDLE;
        const VkResult r = c.vkCreateSwapchainKHR(c.device, &info, nullptr, &made);
        destroy_swapchain();
        c.check(r, label);
        swapchain_ = made;
        format_ = chosen.format;
        extent_ = caps.currentExtent;
        mode_ = mode;
        stale_ = false;
        c.vkGetSwapchainImagesKHR(c.device, swapchain_, &count, nullptr);
        images_.resize(count);
        c.vkGetSwapchainImagesKHR(c.device, swapchain_, &count, images_.data());
        for (std::uint32_t k = 0; k < count; ++k) {
            VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            VkSemaphore s = VK_NULL_HANDLE;
            c.check(c.vkCreateSemaphore(c.device, &sem, nullptr, &s), label);
            done_.push_back(s);
        }
    }
    // Acquires an image, copies (or scales) the back buffer into it and presents; the submitted batch's serial (0 when
    // nothing was presented).
    std::uint64_t present_once(const char* label) {
        Core& c = *core_;
        const VkSemaphore acquire = c.batch().acquire;
        std::uint32_t index = 0;
        VkResult r = c.vkAcquireNextImageKHR(c.device, swapchain_, UINT64_MAX, acquire, VK_NULL_HANDLE, &index);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) {
            stale_ = true;
            return 0;
        }
        c.check(r, label);
        if (r == VK_SUBOPTIMAL_KHR) stale_ = true;
        VkCommandBuffer cb = c.cb();
        c.sync();
        VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        ib.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib.image = images_[index];
        ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        c.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib);
        const unsigned bw = back_->desc().width, bh = back_->desc().height;
        if (bw == extent_.width && bh == extent_.height && format_ == back_->format) {
            VkImageCopy region{};
            region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.extent = {bw, bh, 1};
            c.vkCmdCopyImage(cb, back_->image, VK_IMAGE_LAYOUT_GENERAL, images_[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        } else {
            // The render scale (or a swap chain of another format): the back buffer scaled over the client area.
            VkImageBlit blit{};
            blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.srcOffsets[1] = {static_cast<std::int32_t>(bw), static_cast<std::int32_t>(bh), 1};
            blit.dstOffsets[1] = {static_cast<std::int32_t>(extent_.width), static_cast<std::int32_t>(extent_.height), 1};
            c.vkCmdBlitImage(cb, back_->image, VK_IMAGE_LAYOUT_GENERAL, images_[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                             VK_FILTER_LINEAR);
        }
        ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        ib.dstAccessMask = 0;
        ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        ib.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        c.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib);
        c.need_barrier = true;
        const std::uint64_t serial = c.submit_batch(acquire, VK_PIPELINE_STAGE_TRANSFER_BIT, done_[index]);
        VkPresentInfoKHR p{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        p.waitSemaphoreCount = 1;
        p.pWaitSemaphores = &done_[index];
        p.swapchainCount = 1;
        p.pSwapchains = &swapchain_;
        p.pImageIndices = &index;
        r = c.vkQueuePresentKHR(c.queue, &p);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) stale_ = true;
        else c.check(r, label);
        return serial;
    }

    std::shared_ptr<Core> core_;
    HWND window_;
    VkSurfaceKHR surface_;
    unsigned latency_;
    std::shared_ptr<VkTex> back_;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    std::vector<VkImage> images_;
    std::vector<VkSemaphore> done_;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    VkPresentModeKHR mode_ = VK_PRESENT_MODE_FIFO_KHR;
    bool stale_ = false;
    std::deque<std::uint64_t> in_flight_;
};

}  // namespace

WindowDisplay create_window_display(HWND window, unsigned width, unsigned height, unsigned frame_latency) {
    auto in = shared_instance();
    if (!in->win32_surface) throw std::runtime_error("the Vulkan loader has no Win32 surface support");
    VkWin32SurfaceCreateInfoKHR si{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    si.hinstance = GetModuleHandleW(nullptr);
    si.hwnd = window;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    const VkResult r = in->vkCreateWin32SurfaceKHR(in->instance, &si, nullptr, &surface);
    if (r != VK_SUCCESS) throw std::runtime_error("vkCreateWin32SurfaceKHR failed (" + result_text(r) + ")");
    std::shared_ptr<Core> core;
    try {
        core = make_core(in, surface);
    } catch (...) {
        in->vkDestroySurfaceKHR(in->instance, surface, nullptr);
        throw;
    }
    auto swap = std::make_shared<VulkanSwapChain>(core, window, surface, frame_latency);
    {
        std::lock_guard lock(core->m);
        swap->make_back(width, height, "Create the Vulkan back buffer");
    }
    WindowDisplay out;
    out.display.device = std::make_shared<VulkanDevice>(core);
    out.display.swap_chain = swap;
    out.display.backend = core->name;
    out.display.owned = false;
    out.swap_description = "Vulkan swap chain (made at the first present), frame latency " + std::to_string(std::max(1u, frame_latency));
    return out;
}

std::shared_ptr<Device> create_offscreen_device(const char*) {
    return std::make_shared<VulkanDevice>(make_core(shared_instance(), VK_NULL_HANDLE));
}

}  // namespace omega::gfx::vulkan
