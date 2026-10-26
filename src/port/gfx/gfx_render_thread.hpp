#pragma once
// Port-only (checkpoint 132mt2, not the original): the render thread of research/THREADING.md step 2. The PS2 game
// runs its frame on the EE with the GS / VU1 alongside; the PC port records the frame's GPU work on the simulation
// thread (the scene frame: game logic, list A, and the CPU preparation, list B, unchanged and in the same order) and
// executes it (the submission and the present, list C) on this thread, one frame behind.
//
// How: RenderThread::wrap(display) gives a gfx::Display whose Device and SwapChain queue every context operation
// (Device::submit, Device::write, SwapChain::present) into the frame being recorded instead of executing it. A
// frame's queue is its FrameSnapshot: plain data (the recorded command lists with their arenas, the dynamic buffer
// bytes, the present), immutable once handed off by end_frame. The render thread executes the snapshots in order
// on the backend device; the simulation records the next frame meanwhile (at most one frame ahead: end_frame waits
// while an older frame is still queued). Nothing the render thread does is read by the game: the simulation never
// waits on a GPU result except where a host diagnostic reads one back (captures, the corona visibility log), and
// those drain the queue first (Device::read).
//
// Rules the wrapper keeps:
// - Order: every operation runs in the order the drawing code made it, so the pixels are the single-threaded ones.
// - Resource creation (create_*, prepare) runs at once on the calling thread (a D3D11 device is free-threaded; a
//   GXM backend makes its objects on the CPU too). A resource released by the drawing code is kept alive until
//   every frame recorded before its release has executed (the graveyard), since a queued list names it.
// - Immediate reads and swap-chain changes (Device::read, wait_idle, SwapChain::size / buffer / resize) drain the
//   queue first and then run on the calling thread (the render thread is idle then).
// - Device loss: the render thread catches DeviceLost (the backend already marked the loss) and skips the rest of
//   the queued work while the mark is pending; the owner sees device_lost_pending() at its next frame as before and
//   re-creates the device after drain(). While a test injection is armed (device_lost_inject, the
//   --test-device-lost-mid / -sweep diagnostics) every operation runs inline on the calling thread, so the injected
//   loss hits the same call as in a single-threaded run.
// - Any other exception on the render thread is rethrown on the simulation thread at the next end_frame / drain.
// - stop() (level teardown) drains and joins; the wrapped device and swap chain then run every call inline, so
//   renderers that outlive the thread keep working.
//
// Step 2b (interpolated uncapped frame rate) and the PS Vita: the wrapper is backend-neutral (std::thread, the
// gfx interface only), so the GXM backend gets the same thread; the snapshot of 2b (two sim-rate snapshots with
// camera / pose data and snap flags, research/THREADING.md) is drawn by re-recording the interpolated part on this
// thread, which the queue's per-frame batches leave room for (a batch may present more than once).
#include "gfx_device.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace omega::gfx {

class RenderThread {
public:
    // The render thread's own time for one simulation frame.
    struct FrameTime {
        std::uint64_t frame = 0;   // the simulation frame end_frame named
        float execute_ms = 0;      // the backend submission of the frame's lists and writes
        float present_ms = 0;      // the swap chain's present
        float wait_ms = 0;         // the simulation thread's wait in end_frame (the bound of one frame ahead)
    };

    // threaded false: the wrapped displays run every call inline (no thread is started).
    explicit RenderThread(bool threaded = true);
    ~RenderThread();
    RenderThread(const RenderThread&) = delete;
    RenderThread& operator=(const RenderThread&) = delete;

    // A display whose device and swap chain queue their context work to this thread. The same thread serves every
    // display wrapped here (the window's device after a re-creation: wrap the new display again).
    Display wrap(const Display& inner);
    // The frame's recorded work is handed to the render thread; waits while an older frame is still queued.
    void end_frame(std::uint64_t frame);
    // Everything recorded so far has executed and the released resources are freed (device re-creation, resize).
    void drain();
    // drain(), then the thread ends; later calls of the wrapped displays run inline.
    void stop();
    bool threaded() const;

    // Port-only (checkpoint 132md7, research/MODDING.md "Speed"): the copy a queued Device::write of at least
    // bulk_copy_bytes takes its bytes with (a mod's HD characters in the overlay: tens of MB a frame), e.g. split across
    // the worker pool; called on the writing thread. The bytes are the same. Unset (the default): one memcpy.
    static constexpr std::size_t bulk_copy_bytes = std::size_t{4} << 20;
    using BulkCopy = std::function<void(std::uint8_t* destination, const std::uint8_t* source, std::size_t bytes)>;
    void set_bulk_copy(BulkCopy copy);

    // The per-frame times recorded so far (frames handed off by end_frame) and a one-line summary of them.
    std::vector<FrameTime> frame_times() const;
    std::string summary() const;

    // Port-only (132vg1 / 132vg2, research/VITA.md "Vita render path (132vg2)"): every render thread's totals since
    // the start (microseconds; the frames executed, their execute and present times), read by the PS Vita's play log
    // per window. Nothing else reads them.
    struct Totals {
        std::uint64_t frames = 0, execute_us = 0, present_us = 0;
    };
    static Totals totals();

    struct Core;

private:
    std::shared_ptr<Core> core_;
};

}  // namespace omega::gfx
