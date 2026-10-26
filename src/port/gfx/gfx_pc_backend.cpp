// Port-only (checkpoint 132vk1): the PC's backend choice (gfx_pc_backend.hpp) and the offscreen device of the chosen
// backend (gfx_device.hpp create_offscreen_device).
#include "gfx_pc_backend.hpp"

#include "gfx_d3d11.hpp"
#include "gfx_device_lost.hpp"
#ifdef OMEGA_PC_VULKAN
#include "gfx_vulkan.hpp"
#endif

#include <atomic>
#include <iostream>
#include <mutex>
#include <stdexcept>

namespace omega::gfx {
namespace {
std::atomic<PcBackend> chosen{PcBackend::d3d11};
std::mutex failure_mutex;
std::string failure;
}  // namespace

PcBackend pc_backend() { return chosen.load(); }
void set_pc_backend(PcBackend backend) { chosen.store(backend); }
const char* pc_backend_name(PcBackend backend) { return backend == PcBackend::vulkan ? "vulkan" : "d3d11"; }
bool pc_vulkan_built() {
#ifdef OMEGA_PC_VULKAN
    return true;
#else
    return false;
#endif
}

std::string pc_vulkan_failure() {
    std::lock_guard lock(failure_mutex);
    return failure;
}
void note_pc_vulkan_failure(const std::string& why) {
    std::lock_guard lock(failure_mutex);
    failure = why;
}

std::shared_ptr<Device> create_offscreen_device(const char* label) {
#ifdef OMEGA_PC_VULKAN
    if (pc_backend() == PcBackend::vulkan) {
        try {
            auto device = vulkan::create_offscreen_device(label);
            static std::once_flag logged;   // the evidence line of the first offscreen device (the GUI captures)
            std::call_once(logged, [&] { std::cout << "Renderer (port-only): offscreen devices: " << device->name() << "\n"; });
            return device;
        } catch (const DeviceLost&) {
            throw;
        } catch (const std::exception& e) {
            note_pc_vulkan_failure(e.what());
            std::cerr << "Vulkan (port-only): " << e.what() << "; the offscreen device is Direct3D 11\n";
        }
    }
#endif
    return d3d11::create_offscreen_device(label);
}

}  // namespace omega::gfx
