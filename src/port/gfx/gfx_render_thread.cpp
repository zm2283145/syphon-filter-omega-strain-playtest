// Port-only (checkpoint 132mt2): the render thread. See gfx_render_thread.hpp and research/THREADING.md step 2.
#include "gfx_render_thread.hpp"

#include "gfx_device_lost.hpp"
#include "thread_roles.hpp"
#include "worker_pool.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace omega::gfx {

namespace {

using Clock = std::chrono::steady_clock;
float ms_since(Clock::time_point t) { return std::chrono::duration<float, std::milli>(Clock::now() - t).count(); }

// One queued context operation.
struct Op {
    enum class Kind : std::uint8_t { list, write, present, slot };  // slot: 132vc2 (RenderThread::reserve)
    Kind kind = Kind::list;
    Device* device = nullptr;     // list / write: the backend device
    SwapChain* swap = nullptr;    // present
    CommandList list;             // list
    const Buffer* buffer = nullptr;
    std::vector<std::uint8_t> bytes;  // write
    const char* label = nullptr;
    unsigned interval = 0;        // present
    std::vector<Op> sub;          // slot: the operations recorded by fill()
    bool filled = false;          // slot
};

// A run of operations handed to the thread: a frame's snapshot (frame_end), or the part recorded before a drain.
struct Batch {
    std::vector<Op> ops;
    std::uint64_t seq = 0;
    bool frame_end = false;
    std::uint64_t frame = 0;
};

constexpr unsigned max_frames_ahead = 1;  // the simulation records frame N+1 while frame N executes
constexpr std::size_t max_pooled_lists = 64;
constexpr std::size_t max_pooled_bytes = 16;

// 132vg1: RenderThread::totals() (every render thread's, since the start).
std::atomic<std::uint64_t> g_total_frames{0}, g_total_execute_us{0}, g_total_present_us{0};

}  // namespace

struct RenderThread::Core {
    std::mutex m;
    std::condition_variable work_cv, done_cv;
    std::deque<Batch> queue;
    bool busy = false, stopping = false;
    bool threaded = false;               // written by the simulation thread only (start / stop)
    unsigned frames_pending = 0;         // frame_end batches queued or executing
    std::uint64_t next_seq = 1;          // the open batch's sequence number
    std::vector<std::pair<std::uint64_t, std::shared_ptr<void>>> graveyard;  // released resources, by batch
    std::vector<CommandList> pool;       // cleared lists for the recorders (keeps their capacity)
    // 132md7: the executed writes' byte arrays (keeping their capacity) for the next writes; a large write (a mod's
    // HD characters in the overlay: tens of MB a frame) then copies into pages already mapped instead of a fresh
    // allocation's (research/MODDING.md "Speed"). The bytes written are the same.
    std::vector<std::vector<std::uint8_t>> byte_pool;
    RenderThread::BulkCopy bulk_copy;    // set_bulk_copy (simulation thread only)
    std::exception_ptr error;
    std::vector<FrameTime> times;
    bool occluded = false;               // the last executed present was occluded
    std::shared_ptr<HostThread::Completion> worker;  // 132mt3: run() on the session's render host thread

    std::vector<Op> open;                // the batch being recorded (simulation thread only)
    std::vector<Op>* target = nullptr;   // 132vc2: fill()'s slot while it records (simulation thread only)
    std::size_t unfilled = 0;            // 132vc2: reserved slots not filled yet
    std::function<void()> before_hand_off;
    bool handing_off = false;
    std::vector<Op>& sink() { return target ? *target : open; }
    // 132vc2: the owner fills the open slots before the batch leaves (end_frame, drain).
    void fill_open_slots() {
        if (!unfilled || handing_off) return;
        if (!before_hand_off) throw std::logic_error("RenderThread: a reserved slot was not filled before the hand-off");
        handing_off = true;
        struct Done {
            bool& flag;
            ~Done() { flag = false; }
        } done{handing_off};
        before_hand_off();
        if (unfilled) throw std::logic_error("RenderThread: the hand-off callback left a reserved slot unfilled");
    }
    std::vector<std::pair<std::uint64_t, float>> waits;  // end_frame waits (simulation thread only)

    // Operations run inline: no thread, or a device-loss test injection is armed (so it hits the same call as in a
    // single-threaded run).
    bool inline_now() const { return !threaded || device_lost_injection.load() >= 0; }

    void run() {
        FrameTime current;
        for (;;) {
            Batch b;
            {
                std::unique_lock lock(m);
                work_cv.wait(lock, [&] { return !queue.empty() || stopping; });
                if (queue.empty()) return;
                b = std::move(queue.front());
                queue.pop_front();
                busy = true;
            }
            bool failed;
            {
                std::lock_guard lock(m);
                failed = error != nullptr;
            }
            const auto begin = Clock::now();
            float present_ms = 0;
            bool was_occluded = false;
            std::exception_ptr raised;
            const auto execute = [&](auto&& self, Op& op) -> void {
                if (op.kind == Op::Kind::slot) {  // 132vc2: the slot's operations in their order
                    for (Op& s : op.sub) self(self, s);
                    return;
                }
                ThreadBusyScope busy_time;  // Vita: the render core's busy-time report (empty elsewhere)
                if (failed || raised || device_lost_pending()) return;  // the owner recovers the device
                try {
                    switch (op.kind) {
                    case Op::Kind::list: op.device->submit(op.list); break;
                    case Op::Kind::write: op.device->write(*op.buffer, op.bytes.data(), op.bytes.size(), op.label); break;
                    case Op::Kind::present: {
                        const auto t = Clock::now();
                        was_occluded = op.swap->present(op.interval, op.label) == SwapChain::Presented::occluded;
                        present_ms += ms_since(t);
                        break;
                    }
                    case Op::Kind::slot: break;
                    }
                } catch (const DeviceLost&) {
                    // marked by the backend; the rest of the queued work is skipped while the mark is pending
                } catch (...) {
                    raised = std::current_exception();
                }
            };
            for (Op& op : b.ops) execute(execute, op);
            const float total_ms = ms_since(begin);
            current.execute_ms += total_ms - present_ms;
            current.present_ms += present_ms;
            g_total_execute_us += static_cast<std::uint64_t>(std::max(total_ms - present_ms, 0.0F) * 1000.0F);
            g_total_present_us += static_cast<std::uint64_t>(present_ms * 1000.0F);
            if (b.frame_end) ++g_total_frames;
            std::vector<std::shared_ptr<void>> freed;
            {
                std::lock_guard lock(m);
                if (raised && !error) error = raised;
                const auto recycle = [&](auto&& self, Op& op) -> void {
                    if (op.kind == Op::Kind::slot) {
                        for (Op& sub : op.sub) self(self, sub);
                        return;
                    }
                    if (op.kind == Op::Kind::list && pool.size() < max_pooled_lists) {
                        op.list.clear();
                        pool.push_back(std::move(op.list));
                    }
                    if (op.kind == Op::Kind::write && op.bytes.capacity()) {
                        // kept at its size: a bulk write resizes it, zero-filling only past the old size
                        if (byte_pool.size() >= max_pooled_bytes) {
                            // the smallest goes
                            auto smallest = std::min_element(byte_pool.begin(), byte_pool.end(), [](const auto& a, const auto& b) {
                                return a.capacity() < b.capacity();
                            });
                            if (smallest->capacity() < op.bytes.capacity()) *smallest = std::move(op.bytes);
                        } else {
                            byte_pool.push_back(std::move(op.bytes));
                        }
                    }
                };
                for (Op& op : b.ops) recycle(recycle, op);
                auto keep = graveyard.begin();
                for (auto& entry : graveyard) {
                    if (entry.first <= b.seq) freed.push_back(std::move(entry.second));
                    else *keep++ = std::move(entry);
                }
                graveyard.erase(keep, graveyard.end());
                if (b.frame_end) {
                    current.frame = b.frame;
                    occluded = was_occluded;
                    times.push_back(current);
                    current = FrameTime{};
                    --frames_pending;
                }
            }
            freed.clear();  // the backend objects are released outside the lock, before a drain() waiter wakes
            {
                std::lock_guard lock(m);
                busy = false;
            }
            done_cv.notify_all();
        }
    }

    // Simulation thread: the open batch to the queue.
    void push(bool frame_end, std::uint64_t frame) {
        fill_open_slots();  // 132vc2
        {
            std::lock_guard lock(m);
            Batch b;
            b.ops = std::move(open);
            b.seq = next_seq++;
            b.frame_end = frame_end;
            b.frame = frame;
            if (frame_end) ++frames_pending;
            queue.push_back(std::move(b));
        }
        open.clear();
        work_cv.notify_one();
    }
    void rethrow_locked(std::unique_lock<std::mutex>&) {
        if (error) {
            auto e = error;
            error = nullptr;
            std::rethrow_exception(e);
        }
    }
    void drain() {
        if (!threaded) return;
        if (!open.empty()) push(false, 0);
        std::vector<std::pair<std::uint64_t, std::shared_ptr<void>>> freed;
        {
            std::unique_lock lock(m);
            done_cv.wait(lock, [&] { return queue.empty() && !busy; });
            freed.swap(graveyard);
            rethrow_locked(lock);
        }
    }
    void drain_quiet() noexcept {
        try {
            drain();
        } catch (...) {
        }
    }
    void end_frame(std::uint64_t frame) {
        if (!threaded) return;
        const auto begin = Clock::now();
        push(true, frame);
        std::unique_lock lock(m);
        done_cv.wait(lock, [&] { return frames_pending <= max_frames_ahead || error; });
        waits.emplace_back(frame, ms_since(begin));
        rethrow_locked(lock);
    }
    // Simulation thread (the drawing code releases its resources there).
    void bury(std::shared_ptr<void> object) {
        if (!threaded) return;  // nothing queued: released here
        std::lock_guard lock(m);
        graveyard.emplace_back(next_seq, std::move(object));
    }
    // The pooled array that fits `bytes` most tightly (none: a new one).
    std::vector<std::uint8_t> take_bytes(std::size_t bytes) {
        std::lock_guard lock(m);
        auto best = byte_pool.end();
        for (auto it = byte_pool.begin(); it != byte_pool.end(); ++it)
            if (it->capacity() >= bytes && (best == byte_pool.end() || it->capacity() < best->capacity())) best = it;
        if (best == byte_pool.end()) return {};
        std::vector<std::uint8_t> out = std::move(*best);
        byte_pool.erase(best);
        return out;
    }
    void take_list(CommandList& list, Op& op) {
        {
            std::lock_guard lock(m);
            if (!pool.empty()) {
                op.list = std::move(pool.back());
                pool.pop_back();
            }
        }
        std::swap(op.list, list);  // the recorder keeps a cleared list (with capacity)
        list.clear();
    }
};

namespace {

using Core = RenderThread::Core;

// The released resource's backend object goes to the graveyard (freed once the frames that name it have run).
template <class T> std::shared_ptr<T> wrap_resource(const std::shared_ptr<Core>& core, std::shared_ptr<T> inner) {
    if (!inner) return inner;
    T* raw = inner.get();
    return std::shared_ptr<T>(raw, [core, inner](T*) mutable { core->bury(std::move(inner)); });
}

class QueuedDevice final : public Device {
public:
    QueuedDevice(std::shared_ptr<Core> core, std::shared_ptr<Device> inner) : core_(std::move(core)), inner_(std::move(inner)) {}
    ~QueuedDevice() override { core_->drain_quiet(); }
    const std::string& name() const override { return inner_->name(); }
    Capabilities capabilities() const override { return inner_->capabilities(); }

    std::shared_ptr<Buffer> create_buffer(const BufferDesc& desc, const void* initial, const char* label) override {
        return wrap_resource(core_, inner_->create_buffer(desc, initial, label));
    }
    std::shared_ptr<Texture> create_texture(const TextureDesc& desc, const Level* levels, const TextureLabels& labels) override {
        return wrap_resource(core_, inner_->create_texture(desc, levels, labels));
    }
    std::shared_ptr<BlendState> create_blend(const BlendDesc& desc, const char* label) override {
        return wrap_resource(core_, inner_->create_blend(desc, label));
    }
    std::shared_ptr<DepthState> create_depth(const DepthDesc& desc, const char* label) override {
        return wrap_resource(core_, inner_->create_depth(desc, label));
    }
    std::shared_ptr<RasterState> create_raster(const RasterDesc& desc, const char* label) override {
        return wrap_resource(core_, inner_->create_raster(desc, label));
    }
    std::shared_ptr<SamplerState> create_sampler(const SamplerDesc& desc, const char* label) override {
        return wrap_resource(core_, inner_->create_sampler(desc, label));
    }
    // A program is made ready before any list that sets it is recorded, so the thread only reads prepared slots.
    void prepare(Program program) override { inner_->prepare(program); }

    void submit(CommandList& list) override {
        if (core_->inline_now()) {
            core_->drain();
            inner_->submit(list);
            return;
        }
        if (list.empty()) return;
        Op op;
        op.kind = Op::Kind::list;
        op.device = inner_.get();
        core_->take_list(list, op);
        core_->sink().push_back(std::move(op));
    }
    void write(const Buffer& buffer, const void* data, std::size_t bytes, const char* label) override {
        if (core_->inline_now()) {
            core_->drain();
            inner_->write(buffer, data, bytes, label);
            return;
        }
        Op op;
        op.kind = Op::Kind::write;
        op.device = inner_.get();
        op.buffer = &buffer;
        const auto* p = static_cast<const std::uint8_t*>(data);
        op.bytes = core_->take_bytes(bytes);
        if (bytes >= RenderThread::bulk_copy_bytes && core_->bulk_copy) {
            op.bytes.resize(bytes);
            core_->bulk_copy(op.bytes.data(), p, bytes);
        } else {
            op.bytes.assign(p, p + bytes);
        }
        op.label = label;
        core_->sink().push_back(std::move(op));
    }
    void read(const Texture& staging, unsigned subresource, const std::function<void(const std::uint8_t*, unsigned)>& reader,
              const char* label) override {
        core_->drain();
        inner_->read(staging, subresource, reader, label);
    }
    bool removed() const override { return inner_->removed(); }
    bool wait_idle() override {
        core_->drain();
        return inner_->wait_idle();
    }

private:
    std::shared_ptr<Core> core_;
    std::shared_ptr<Device> inner_;
};

class QueuedSwapChain final : public SwapChain {
public:
    QueuedSwapChain(std::shared_ptr<Core> core, std::shared_ptr<SwapChain> inner) : core_(std::move(core)), inner_(std::move(inner)) {}
    ~QueuedSwapChain() override { core_->drain_quiet(); }
    void size(unsigned& width, unsigned& height, const char* label) override {
        core_->drain();
        inner_->size(width, height, label);
    }
    std::shared_ptr<Texture> buffer(bool with_target, const char* label, const char* target_label) override {
        core_->drain();
        return wrap_resource(core_, inner_->buffer(with_target, label, target_label));
    }
    void resize(unsigned width, unsigned height, const char* label) override {
        core_->drain();  // every queued use of the old buffers has run and their views are released
        inner_->resize(width, height, label);
    }
    // Queued: reports the previous executed present's occlusion (a lost device is reported through the loss mark).
    Presented present(unsigned sync_interval, const char* label) override {
        if (core_->inline_now()) {
            core_->drain();
            return inner_->present(sync_interval, label);
        }
        Op op;
        op.kind = Op::Kind::present;
        op.swap = inner_.get();
        op.interval = sync_interval;
        op.label = label;
        core_->open.push_back(std::move(op));
        std::lock_guard lock(core_->m);
        return core_->occluded ? Presented::occluded : Presented::ok;
    }

private:
    std::shared_ptr<Core> core_;
    std::shared_ptr<SwapChain> inner_;
};

}  // namespace

RenderThread::RenderThread(bool threaded) : core_(std::make_shared<Core>()) {
    if (!threaded) return;
    core_->threaded = true;
    // Checkpoint 132mt3 (port-only): run() goes to the session's render host thread (worker_pool.hpp HostThread),
    // made once, so a level start and end start and end no thread (research/THREADING.md "The loading-screen hang").
    core_->worker = HostThread::render().run([core = core_] { core->run(); });
}

RenderThread::~RenderThread() {
    try {
        stop();
    } catch (...) {
        // a render-thread error during unwinding is dropped (the first exception is already in flight)
    }
}

Display RenderThread::wrap(const Display& inner) {
    Display out = inner;
    if (inner.device) out.device = std::make_shared<QueuedDevice>(core_, inner.device);
    if (inner.swap_chain) out.swap_chain = std::make_shared<QueuedSwapChain>(core_, inner.swap_chain);
    return out;
}

void RenderThread::end_frame(std::uint64_t frame) { core_->end_frame(frame); }
void RenderThread::drain() { core_->drain(); }
bool RenderThread::threaded() const { return core_->threaded; }

void RenderThread::set_bulk_copy(BulkCopy copy) { core_->bulk_copy = std::move(copy); }

std::size_t RenderThread::reserve() {
    if (core_->inline_now() || core_->target) return 0;
    Op op;
    op.kind = Op::Kind::slot;
    core_->open.push_back(std::move(op));
    ++core_->unfilled;
    return core_->open.size();  // 1 + its index in the open batch
}

void RenderThread::fill(std::size_t slot, const std::function<void()>& record) {
    if (slot == 0 || slot > core_->open.size() || core_->open[slot - 1].kind != Op::Kind::slot || core_->open[slot - 1].filled)
        throw std::logic_error("RenderThread: fill of a slot that is not open");
    Op& op = core_->open[slot - 1];
    op.filled = true;
    --core_->unfilled;
    if (core_->inline_now()) {  // a device-loss injection armed since: inline, as every call now
        record();
        return;
    }
    std::vector<Op> sub;
    core_->target = &sub;
    struct Restore {
        Core& core;
        ~Restore() { core.target = nullptr; }
    } restore{*core_};
    record();
    core_->open[slot - 1].sub = std::move(sub);  // (open does not grow while target is set)
}

std::size_t RenderThread::unfilled() const { return core_->unfilled; }

void RenderThread::set_before_hand_off(std::function<void()> callback) { core_->before_hand_off = std::move(callback); }

void RenderThread::stop() {
    if (!core_->threaded) return;
    std::exception_ptr pending;
    try {
        core_->drain();
    } catch (...) {
        pending = std::current_exception();
    }
    {
        std::lock_guard lock(core_->m);
        core_->stopping = true;
    }
    core_->work_cv.notify_all();
    if (core_->worker) core_->worker->wait();
    core_->threaded = false;  // from here every wrapped call runs inline
    {
        std::lock_guard lock(core_->m);
        core_->graveyard.clear();
    }
    if (pending) std::rethrow_exception(pending);
}

RenderThread::Totals RenderThread::totals() {
    return {g_total_frames.load(), g_total_execute_us.load(), g_total_present_us.load()};
}

std::vector<RenderThread::FrameTime> RenderThread::frame_times() const {
    std::vector<FrameTime> out;
    {
        std::lock_guard lock(core_->m);
        out = core_->times;
    }
    // The simulation's waits by frame (both lists are in frame order).
    std::size_t w = 0;
    for (auto& t : out) {
        while (w < core_->waits.size() && core_->waits[w].first < t.frame) ++w;
        if (w < core_->waits.size() && core_->waits[w].first == t.frame) t.wait_ms = core_->waits[w].second;
    }
    return out;
}

std::string RenderThread::summary() const {
    const auto times = frame_times();
    std::ostringstream s;
    s << std::fixed << std::setprecision(3) << times.size() << " frames on the render thread";
    if (times.empty()) return s.str();
    const auto stat = [&](float FrameTime::*field, const char* name) {
        std::vector<float> v;
        for (const auto& t : times) v.push_back(t.*field);
        std::sort(v.begin(), v.end());
        double sum = 0;
        for (float x : v) sum += x;
        s << "; " << name << " mean " << sum / double(v.size()) << " p50 " << v[(v.size() - 1) / 2] << " p95 "
          << v[std::size_t(0.95 * double(v.size() - 1))] << " max " << v.back() << " ms";
    };
    stat(&FrameTime::execute_ms, "execute");
    stat(&FrameTime::present_ms, "present");
    stat(&FrameTime::wait_ms, "simulation wait");
    return s.str();
}

}  // namespace omega::gfx
