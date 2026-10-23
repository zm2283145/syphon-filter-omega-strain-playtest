#include "level_teardown_callees.hpp"

#include <initializer_list>
#include <stdexcept>

namespace omega {
namespace {
constexpr std::uint32_t lock_flag = 0x533880;
constexpr std::uint32_t lock_count = 0x533888;
constexpr std::uint32_t memory_cc = 0x4bbe50;   // the source-file name 0036b250 gives 0036bc10
constexpr std::uint32_t video_cc = 0x4bc200;    // the one 0037e320 gives the free 0036b130
constexpr std::uint32_t heap_table = 0x533cb0, heap_size = 0x818;
constexpr std::uint32_t slots = 0x5344b4;
constexpr std::uint32_t none = 0xffffffffU;

struct Ops {
    LevelLoadMemory& m;
    LevelLoadHost& host;
    std::uint32_t r8(std::uint32_t a) const { return m.read(a, 1); }
    std::uint32_t r32(std::uint32_t a) const { return m.read(a); }
    void w32(std::uint32_t a, std::uint32_t v) const { m.write(a, 4, v); }
    std::uint32_t call(std::uint32_t a, std::initializer_list<std::uint32_t> args = {}) const {
        return host.call(m, a, args);
    }
    void lock() const {
        if (r8(lock_flag)) call(0x10dc40, {9});
        w32(lock_count, r32(lock_count) + 1);
    }
    void unlock() const {
        if (r8(lock_flag)) call(0x10dca8, {9});
        w32(lock_count, r32(lock_count) - 1);
    }
    // 0036bc10's heap: the low nibble of the block header byte -0xc.
    void heap_free(std::uint32_t block, std::uint32_t line) const {
        const std::uint32_t heap = heap_table + (r8(block - 0xc) & 0xfU) * heap_size;
        call(0x36bc10, {heap, block, memory_cc, line});
    }
};
}  // namespace

void archive_release_003f9470(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t manager, std::uint32_t sp) {
    const Ops o{m, host};
    const std::uint32_t frame = sp - 0x50;
    // 003f9520 is the identity (jr ra; v0 = a0): the list sentinel is manager+4.
    const auto end = manager + 4;
    auto node = o.r32(end + 4);
    while (node != end) {
        const auto archive = node + 8;
        if (o.r8(node + 0x170) && o.r8(archive + 0x14c)) {
            node = o.r32(node + 4);
            continue;
        }
        if (o.r8(archive + 0x14c)) o.call(0x36cd80, {archive});
        o.w32(frame + 0x48, node);
        o.call(0x3f9a30, {frame + 0x4c, manager, frame + 0x48});
        node = o.r32(frame + 0x4c);
    }
}

void renderer_teardown_0037e320(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t video) {
    const Ops o{m, host};
    if (o.r32(video) == none) return;
    o.call(0x375cd0, {0x537f80});
    if (!(o.r32(video) & 8U)) {
        for (std::uint32_t i = 0; i < 6; ++i) {
            const auto entry = video + 0x1068 + 0x14 * i;
            o.call(0x3813a0, {o.r32(video + 0x1058), o.r32(entry), 0});
            if (o.r8(entry + 0x10)) o.call(0x3813a0, {o.r32(video + 0x1058), o.r32(entry) + 1, 0});
        }
    }
    const auto release = [&](std::uint32_t object, std::uint32_t line) {
        o.lock();
        o.call(0x36b130, {0, object, video_cc, line});
        o.unlock();
    };
    for (std::uint32_t j = 0; j < 2; ++j) {
        const auto block = video + 0xfb0 + 0x38 * j;
        release(o.r32(block), 0x23e);
        release(o.r32(block + 8), 0x242);
        release(o.r32(block + 0xc), 0x243);
        release(o.r32(block + 0x10), 0x244);
        for (std::uint32_t k = 0; k < 0x14; k += 4) o.w32(block + k, 0);
    }
    release(o.r32(video + 0x103c), 0x24c);
    o.w32(video + 0x103c, 0);

    auto object = o.r32(video + 0x1058);
    o.lock();
    o.call(0x383020, {object, none});
    o.call(0x36b130, {0, object, video_cc, 0x24f});
    o.unlock();
    o.w32(video + 0x1058, 0);

    object = o.r32(video + 0x105c);
    if (!object) throw std::runtime_error("0037e320: the +105c object is null");
    o.lock();
    const auto method = o.r32(o.r32(object + 0x10) + 8);
    if (!method) throw std::runtime_error("0037e320: the +105c object's destructor *(*(+10)+8) is null");
    o.call(method, {object, none});
    o.call(0x36b130, {0, object, video_cc, 0x251});
    o.unlock();
    o.w32(video + 0x105c, 0);
    o.call(0x37b0c0, {video, 1});
    o.w32(video, none);
}

void heap_slot_release_0036b250(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t slot) {
    const Ops o{m, host};
    const auto base = slots + heap_size * slot;
    if (o.r32(base + 0x10) == none) return;
    const auto block = o.r32(base);
    o.lock();
    if (block) o.heap_free(block, 0x244);
    o.unlock();
    o.w32(base + 4, 0);
    o.w32(base, 0);
    o.w32(base + 8, 0);
    o.w32(base + 0xc, 0);
    o.w32(base + 0x10, none);
}

}  // namespace omega
