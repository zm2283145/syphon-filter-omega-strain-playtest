#include "audio_device.hpp"

#include "audio_output.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <xaudio2.h>

namespace omega::audio {
namespace {

constexpr std::size_t chunk_frames = Mixer::output_rate / 100;  // 10 ms per XAudio2 buffer
constexpr std::size_t chunks_in_flight = 4;
constexpr double stall_seconds = 0.25;  // no buffer finished for this long with the ring full: lost

std::string hex(HRESULT hr) {
    char code[16];
    std::snprintf(code, sizeof code, "%08lx", static_cast<unsigned long>(hr));
    return code;
}

std::string hresult(const char* what, HRESULT hr) {
    return std::string("Audio device: ") + what + " failed (HRESULT " + hex(hr) + ")";
}

void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) throw std::runtime_error(hresult(what, hr));
}

double seconds_now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

enum Loss : int { no_loss, critical_error, voice_error, default_changed, stalled, simulated_loss, simulated_switch };

// The default render device's changes (eConsole, the role XAudio2's default mastering voice follows).
class DefaultDeviceWatch final : public IMMNotificationClient {
public:
    DefaultDeviceWatch(std::atomic<int>& pending, std::atomic<bool>& arrived) : pending_(pending), arrived_(arrived) {}
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --refs_;
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            AddRef();
            *out = static_cast<IMMNotificationClient*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole) {
            int none = no_loss;
            pending_.compare_exchange_strong(none, default_changed);
            arrived_ = true;
        }
        return S_OK;
    }
    // Port-only (132win7): a device enabled or added; a lost output (none at startup) retries at once.
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD state) override {
        if (state == DEVICE_STATE_ACTIVE) arrived_ = true;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override {
        arrived_ = true;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    std::atomic<ULONG> refs_{1};
    std::atomic<int>& pending_;
    std::atomic<bool>& arrived_;
};

}

struct Device::Impl final : IXAudio2VoiceCallback, IXAudio2EngineCallback {
    Impl(std::size_t prime, bool mute) : ring(prime * 2, prime), muted(mute) {
        for (auto& chunk : chunks) chunk.resize(chunk_frames * 2);
    }

    ~Impl() {
        close();
        if (enumerator) {
            if (watch) enumerator->UnregisterEndpointNotificationCallback(watch);
            enumerator->Release();
        }
        if (watch) watch->Release();
        if (com) CoUninitialize();
    }

    void watch_default_device() {
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                    reinterpret_cast<void**>(&enumerator))))
            return;
        watch = new DefaultDeviceWatch(pending, arrived);
        if (FAILED(enumerator->RegisterEndpointNotificationCallback(watch))) {
            watch->Release();
            watch = nullptr;
        }
    }

    // The engine, the mastering voice on the default device and the source voice, primed with silence.
    void open() {
        check(XAudio2Create(&engine, 0, XAUDIO2_DEFAULT_PROCESSOR), "XAudio2Create");
        check(engine->RegisterForCallbacks(this), "IXAudio2::RegisterForCallbacks");
        check(engine->CreateMasteringVoice(&master), "CreateMasteringVoice");
        if (muted) check(master->SetVolume(0.0F), "IXAudio2MasteringVoice::SetVolume");
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 2;
        format.nSamplesPerSec = Mixer::output_rate;
        format.wBitsPerSample = 16;
        format.nBlockAlign = 4;
        format.nAvgBytesPerSec = Mixer::output_rate * 4;
        check(engine->CreateSourceVoice(&source, &format, 0, XAUDIO2_DEFAULT_FREQ_RATIO, this), "CreateSourceVoice");
        if (background_muted) check(source->SetVolume(0.0F), "IXAudio2SourceVoice::SetVolume");
        failure = S_OK;
        // Silence until the ring is primed; each finished chunk queues the next.
        for (std::size_t i = 0; i < chunks_in_flight; ++i) refill(i);
        check(failure, "SubmitSourceBuffer");
        check(source->Start(), "IXAudio2SourceVoice::Start");
    }

    void close() {
        closing = true;
        if (source) {
            source->Stop();
            source->DestroyVoice();  // returns after any running callback
            source = nullptr;
        }
        if (master) {
            master->DestroyVoice();
            master = nullptr;
        }
        if (engine) {
            engine->UnregisterForCallbacks(this);
            engine->StopEngine();
            engine->Release();
            engine = nullptr;
        }
        closing = false;
    }

    void lose(Loss why, HRESULT hr = S_OK) {
        int none = no_loss;
        if (pending.compare_exchange_strong(none, why)) pending_hr = hr;
    }

    // Pulls the next 10 ms from the ring into chunk `index` and queues it.
    void refill(std::size_t index) {
        {
            std::lock_guard lock(mutex);
            ring.read(chunks[index].data(), chunk_frames);
        }
        last_pull = seconds_now();
        drained.notify_all();
        XAUDIO2_BUFFER buffer{};
        buffer.AudioBytes = static_cast<UINT32>(chunk_frames * 4);
        buffer.pAudioData = reinterpret_cast<const BYTE*>(chunks[index].data());
        buffer.pContext = reinterpret_cast<void*>(index);
        const auto hr = source->SubmitSourceBuffer(&buffer);
        if (FAILED(hr)) {
            failure = hr;
            lose(voice_error, hr);
        }
    }

    void STDMETHODCALLTYPE OnBufferEnd(void* context) noexcept override {
        if (!closing) refill(reinterpret_cast<std::size_t>(context));
    }
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) noexcept override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() noexcept override {}
    void STDMETHODCALLTYPE OnStreamEnd() noexcept override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) noexcept override {}
    void STDMETHODCALLTYPE OnLoopEnd(void*) noexcept override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT error) noexcept override {
        failure = error;
        lose(voice_error, error);
    }
    void STDMETHODCALLTYPE OnProcessingPassStart() noexcept override {}
    void STDMETHODCALLTYPE OnProcessingPassEnd() noexcept override {}
    void STDMETHODCALLTYPE OnCriticalError(HRESULT error) noexcept override { lose(critical_error, error); }

    // A reported loss: the output closed and the ring cleared (the submit thread).
    void take_loss(double now) {
        if (arrived.exchange(false)) recovery.retry_soon(now);
        const int why = pending.exchange(no_loss);
        if (why == no_loss || recovery.lost()) return;
        const HRESULT hr = pending_hr;
        std::string reason;
        switch (why) {
        case critical_error:
            reason = "XAudio2 critical error (HRESULT " + hex(hr) + (hr == XAUDIO2_E_DEVICE_INVALIDATED ? ": the device was invalidated, unplugged or disabled" : "") + ")";
            break;
        case voice_error: reason = "the source voice failed (HRESULT " + hex(hr) + ")"; break;
        case default_changed: reason = "the default output device changed"; break;
        case stalled: reason = "the device stopped pulling audio for 250 ms"; break;
        case simulated_loss: reason = std::string("simulated device loss (port-only test") + (simulate_gone ? ", no device until it is back)" : ")"); break;
        default: reason = "simulated default device switch (port-only test)"; break;
        }
        close();
        {
            std::lock_guard lock(mutex);
            ring.clear();
        }
        recovery.lose(now);
        lost_frames_at_loss = lost_frames;
        log.push_back("Audio device: output lost (" + reason + "); the simulation does not wait for it, re-opening on the default device");
    }

    void try_reopen(double now) {
        if (!recovery.try_now(now)) return;
        try {
            if (simulate_gone) throw std::runtime_error("Audio device: no output device (simulated, port-only test)");
            open();
            recovery.reopened();
            char ms[32];
            std::snprintf(ms, sizeof ms, "%.0f", (now - recovery.lost_since()) * 1000.0);
            const bool first = !opened_once;
            opened_once = true;
            log.push_back(std::string(first ? "Audio device: opened on the default output (none at startup) after "
                                            : "Audio device: re-opened on the default output after ") +
                          ms + " ms (" + std::to_string(lost_frames - lost_frames_at_loss) + " frames dropped while " +
                          (first ? "silent, " : "lost, ") + std::to_string(recovery.failed_reopens()) +
                          " failed re-opens so far)");
        } catch (const std::exception& e) {
            close();
            recovery.reopen_failed(now);
            if (recovery.failed_reopens() <= 3)
                log.push_back(std::string(e.what()) + "; retrying once a second" +
                              (recovery.failed_reopens() == 3 ? " (further failures are counted)" : ""));
        }
    }

    bool com = false;
    bool muted = false;
    bool background_muted = false;   // 132win12 (port-only)
    IXAudio2* engine = nullptr;
    IXAudio2MasteringVoice* master = nullptr;
    IXAudio2SourceVoice* source = nullptr;
    IMMDeviceEnumerator* enumerator = nullptr;
    DefaultDeviceWatch* watch = nullptr;
    std::array<std::vector<std::int16_t>, chunks_in_flight> chunks;
    mutable std::mutex mutex;
    std::condition_variable drained;
    PcmRing ring;
    std::uint64_t submitted = 0, dropped = 0, lost_frames = 0, lost_frames_at_loss = 0;
    std::atomic<bool> closing{false};
    std::atomic<HRESULT> failure{S_OK};
    std::atomic<int> pending{no_loss};
    std::atomic<HRESULT> pending_hr{S_OK};
    std::atomic<bool> arrived{false};  // 132win7: a device arrival (watch) or simulate_device_back
    bool opened_once = false;
    std::atomic<double> last_pull{0.0};
    OutputRecovery recovery;
    bool simulate_gone = false;
    std::vector<std::string> log;
};

Device::Device(double latency_seconds, bool muted, bool simulate_absent) : latency_(latency_seconds) {
    const auto prime = static_cast<std::size_t>(latency_seconds * Mixer::output_rate);
    if (prime < chunk_frames) throw std::runtime_error("Audio device: latency must be at least 10 ms");
    impl_ = std::make_unique<Impl>(prime, muted);
    auto& d = *impl_;
    d.com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    d.simulate_gone = simulate_absent;
    try {
        if (d.simulate_gone) throw std::runtime_error("Audio device: no output device (simulated, port-only test)");
        d.open();
        d.opened_once = true;
    } catch (const std::exception& e) {
        // Port-only (132win7): no device at startup; silent until the same re-open path finds one.
        d.close();
        d.recovery.start_absent(seconds_now());
        d.log.push_back(std::string(e.what()) + "; starting without audio output (silent), opening the default " +
                        "output when one appears (retried once a second)");
    }
    d.watch_default_device();
}

Device::~Device() = default;

void Device::submit(const std::vector<std::int16_t>& samples) {
    std::lock_guard control(public_mutex_);
    if (samples.size() % 2) throw std::runtime_error("Audio device: odd sample count");
    const auto frames = samples.size() / 2;
    auto& d = *impl_;
    const double now = seconds_now();
    d.take_loss(now);
    if (d.recovery.lost()) d.try_reopen(now);
    if (d.recovery.lost()) {
        d.dropped += frames;
        d.lost_frames += frames;
        return;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    std::unique_lock lock(d.mutex);
    std::size_t done = 0;
    while (done < frames) {
        done += d.ring.write(samples.data() + done * 2, frames - done);
        if (done == frames) break;
        if (d.drained.wait_for(lock, std::chrono::milliseconds(50)) == std::cv_status::timeout && !d.ring.space()) {
            if (seconds_now() - d.last_pull > stall_seconds) {
                d.lose(stalled);
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline) break;
        }
    }
    d.submitted += done;
    d.dropped += frames - done;
}

Device::Stats Device::stats() const {
    std::lock_guard control(public_mutex_);
    std::lock_guard lock(impl_->mutex);
    const auto& d = *impl_;
    Stats s{d.ring.underruns(), d.ring.silent_frames(), d.submitted, d.dropped, d.ring.queued()};
    s.losses = d.recovery.losses();
    s.reopens = d.recovery.reopens();
    s.failed_reopens = d.recovery.failed_reopens();
    s.lost_frames = d.lost_frames;
    s.lost = d.recovery.lost() || d.pending != no_loss;
    s.started_absent = d.recovery.started_absent();
    s.background_muted = d.background_muted;
    return s;
}

void Device::set_background_muted(bool on) {
    std::lock_guard control(public_mutex_);
    auto& d = *impl_;
    if (d.background_muted == on) return;
    d.background_muted = on;
    if (d.source) d.source->SetVolume(on ? 0.0F : 1.0F);
    d.log.push_back(on ? "Audio device (port-only): output silenced while the game window is unfocused (the mixing goes on)"
                       : "Audio device (port-only): output back with the window's focus");
}

bool Device::background_muted() const {
    std::lock_guard control(public_mutex_);
    return impl_->background_muted;
}

std::vector<std::string> Device::take_log() {
    std::lock_guard control(public_mutex_);
    return std::exchange(impl_->log, {});
}

void Device::simulate_loss(bool gone) {
    std::lock_guard control(public_mutex_);
    if (gone) impl_->simulate_gone = true;
    impl_->lose(simulated_loss);
}

void Device::simulate_default_change() {
    std::lock_guard control(public_mutex_);
    impl_->lose(simulated_switch);
}

void Device::simulate_device_back() {
    std::lock_guard control(public_mutex_);
    impl_->simulate_gone = false;
    impl_->arrived = true;
}

void Device::set_retry_seconds(double seconds) {
    std::lock_guard control(public_mutex_);
    impl_->recovery.set_retry(seconds);
}

}
