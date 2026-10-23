// Port-only (checkpoint 132md15, platform_movie.hpp): the Windows movie backend, Media Foundation's Source Reader (a
// system component: mfplat / mfreadwrite, nothing shipped). MP4 / M4V / MOV with the decoders Windows has (H.264 and
// AAC on every Windows 10 / 11 desktop; HEVC with its store extension). Pictures are asked for as NV12 (I420 when the
// decoder offers only that) and cropped to the minimum display aperture (an H.264 1080p stream decodes 1088 rows);
// sound as 16-bit PCM at the stream's own rate and channels.
#include "platform_movie.hpp"

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>

namespace omega::movie {

namespace {

using Microsoft::WRL::ComPtr;

std::string hresult_text(HRESULT hr) {
    char text[16];
    std::snprintf(text, sizeof text, "%08lX", static_cast<unsigned long>(hr));
    return text;
}

void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) throw std::runtime_error(std::string(what) + " failed (HRESULT " + hresult_text(hr) + ")");
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// An IStream over a MovieFile (MFCreateMFByteStreamOnStream wraps it for the source): read and seek only.
class FileStream final : public IStream {
public:
    explicit FileStream(MovieFile file) : file_(std::move(file)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IStream) || riid == __uuidof(ISequentialStream)) {
            *out = static_cast<IStream*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(++refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto left = --refs_;
        if (!left) delete this;
        return static_cast<ULONG>(left);
    }
    HRESULT STDMETHODCALLTYPE Read(void* to, ULONG bytes, ULONG* read) override {
        std::lock_guard lock(mutex_);
        const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(bytes, file_.size - std::min(position_, file_.size)));
        std::size_t got = 0;
        try {
            got = n ? file_.read(position_, std::span(static_cast<std::uint8_t*>(to), n)) : 0;
        } catch (...) {
            return E_FAIL;
        }
        position_ += got;
        if (read) *read = static_cast<ULONG>(got);
        return got == bytes ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG*) override { return STG_E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* now) override {
        std::lock_guard lock(mutex_);
        std::int64_t base = 0;
        switch (origin) {
        case STREAM_SEEK_SET: base = 0; break;
        case STREAM_SEEK_CUR: base = static_cast<std::int64_t>(position_); break;
        case STREAM_SEEK_END: base = static_cast<std::int64_t>(file_.size); break;
        default: return STG_E_INVALIDFUNCTION;
        }
        const std::int64_t to = base + move.QuadPart;
        if (to < 0) return STG_E_INVALIDFUNCTION;
        position_ = static_cast<std::uint64_t>(to);
        if (now) now->QuadPart = position_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override { return STG_E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*, ULARGE_INTEGER*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Revert() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* stat, DWORD) override {
        if (!stat) return STG_E_INVALIDPOINTER;
        std::memset(stat, 0, sizeof *stat);
        stat->type = STGTY_STREAM;
        stat->cbSize.QuadPart = file_.size;
        stat->grfMode = STGM_READ;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clone(IStream**) override { return E_NOTIMPL; }

private:
    ~FileStream() = default;
    std::atomic<long> refs_{1};
    std::mutex mutex_;
    MovieFile file_;
    std::uint64_t position_ = 0;
};

std::string codec_name(const GUID& subtype) {
    if (subtype == MFVideoFormat_H264) return "H.264";
    if (subtype == MFVideoFormat_HEVC) return "H.265";
    if (subtype == MFVideoFormat_MP4V) return "MPEG-4 Part 2";
    if (subtype == MFVideoFormat_MPEG2) return "MPEG-2";
    if (subtype == MFVideoFormat_MJPG) return "Motion JPEG";
    if (subtype == MFAudioFormat_AAC) return "AAC";
    if (subtype == MFAudioFormat_MP3) return "MP3";
    if (subtype == MFAudioFormat_PCM) return "PCM";
    if (subtype == MFAudioFormat_Dolby_AC3) return "AC-3";
    return "codec " + hresult_text(static_cast<HRESULT>(subtype.Data1));
}

constexpr DWORD video_stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
constexpr DWORD audio_stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);

class MediaFoundationMovie final : public PlatformMovieDecoder {
public:
    explicit MediaFoundationMovie(MovieFile file) {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        com_ = SUCCEEDED(com);   // RPC_E_CHANGED_MODE: an STA thread; Media Foundation works there too
        check(MFStartup(MF_VERSION, MFSTARTUP_LITE), "MFStartup");
        started_ = true;
        try {
            open(std::move(file));
        } catch (...) {
            close();
            throw;
        }
    }
    ~MediaFoundationMovie() override { close(); }

    const PlatformMovieInfo& info() const override { return info_; }

    bool next_picture(mpeg2::Frame& frame, double& seconds) override {
        while (!video_done_) {
            DWORD flags = 0;
            LONGLONG time = 0;
            ComPtr<IMFSample> sample;
            const HRESULT hr = reader_->ReadSample(video_stream, 0, nullptr, &flags, &time, &sample);
            if (FAILED(hr)) return fail_video("reading a picture failed (HRESULT " + hresult_text(hr) + ")");
            if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
                try {
                    read_video_type();
                } catch (const std::exception& e) {
                    return fail_video(e.what());
                }
            }
            if (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR)) {
                video_done_ = true;
                if (flags & MF_SOURCE_READERF_ERROR) error_ = "the picture stream reported an error";
                return false;
            }
            if (!sample) continue;   // a stream tick (a gap)
            try {
                to_frame(sample.Get(), frame);
            } catch (const std::exception& e) {
                return fail_video(e.what());
            }
            seconds = static_cast<double>(time) / 1e7;
            return true;
        }
        return false;
    }

    bool next_audio(std::vector<std::int16_t>& out, double& seconds) override {
        while (!audio_done_) {
            DWORD flags = 0;
            LONGLONG time = 0;
            ComPtr<IMFSample> sample;
            const HRESULT hr = reader_->ReadSample(audio_stream, 0, nullptr, &flags, &time, &sample);
            if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR)) {
                audio_done_ = true;
                if (error_.empty()) error_ = "reading the sound failed (HRESULT " + hresult_text(hr) + ")";
                return false;
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                audio_done_ = true;
                return false;
            }
            if (!sample) continue;
            ComPtr<IMFMediaBuffer> buffer;
            if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) continue;
            BYTE* data = nullptr;
            DWORD length = 0;
            if (FAILED(buffer->Lock(&data, nullptr, &length))) continue;
            const std::size_t at = out.size();
            out.resize(at + length / 2);
            std::memcpy(out.data() + at, data, (length / 2) * 2);
            buffer->Unlock();
            seconds = static_cast<double>(time) / 1e7;
            return true;
        }
        return false;
    }

    std::string error() const override { return error_; }

private:
    void open(MovieFile file) {
        const auto name = file.name;
        std::string extension = name.substr(std::min(name.size(), name.find_last_of('.')));
        for (auto& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const wchar_t* type = extension == ".mov" ? L"video/quicktime" : extension == ".m4v" ? L"video/x-m4v" : L"video/mp4";
        ComPtr<IStream> stream;
        stream.Attach(new FileStream(std::move(file)));
        ComPtr<IMFByteStream> bytes;
        check(MFCreateMFByteStreamOnStream(stream.Get(), &bytes), "MFCreateMFByteStreamOnStream");
        ComPtr<IMFAttributes> stream_attributes;
        if (SUCCEEDED(bytes.As(&stream_attributes))) {
            stream_attributes->SetString(MF_BYTESTREAM_CONTENT_TYPE, type);
            stream_attributes->SetString(MF_BYTESTREAM_ORIGIN_NAME, widen(name).c_str());
        }
        ComPtr<IMFAttributes> attributes;
        check(MFCreateAttributes(&attributes, 2), "MFCreateAttributes");
        attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        const HRESULT made = MFCreateSourceReaderFromByteStream(bytes.Get(), attributes.Get(), &reader_);
        if (FAILED(made)) throw std::runtime_error("not a movie Media Foundation reads (HRESULT " + hresult_text(made) + ")");
        reader_->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);

        // The picture.
        ComPtr<IMFMediaType> native;
        if (FAILED(reader_->GetNativeMediaType(video_stream, 0, &native))) throw std::runtime_error("no picture stream");
        GUID subtype{};
        native->GetGUID(MF_MT_SUBTYPE, &subtype);
        info_.video_codec = codec_name(subtype);
        check(reader_->SetStreamSelection(video_stream, TRUE), "selecting the picture stream");
        bool set = false;
        for (const GUID& want : {MFVideoFormat_NV12, MFVideoFormat_I420, MFVideoFormat_IYUV}) {
            ComPtr<IMFMediaType> type_out;
            check(MFCreateMediaType(&type_out), "MFCreateMediaType");
            type_out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            type_out->SetGUID(MF_MT_SUBTYPE, want);
            if (SUCCEEDED(reader_->SetCurrentMediaType(video_stream, nullptr, type_out.Get()))) {
                set = true;
                break;
            }
        }
        if (!set) throw std::runtime_error("no decoder for its " + info_.video_codec + " pictures on this Windows");
        read_video_type();

        // The sound (none is not an error).
        ComPtr<IMFMediaType> native_audio;
        if (SUCCEEDED(reader_->GetNativeMediaType(audio_stream, 0, &native_audio))) {
            GUID audio_subtype{};
            native_audio->GetGUID(MF_MT_SUBTYPE, &audio_subtype);
            ComPtr<IMFMediaType> type_out;
            check(MFCreateMediaType(&type_out), "MFCreateMediaType");
            type_out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            type_out->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
            type_out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            if (SUCCEEDED(reader_->SetStreamSelection(audio_stream, TRUE)) &&
                SUCCEEDED(reader_->SetCurrentMediaType(audio_stream, nullptr, type_out.Get()))) {
                ComPtr<IMFMediaType> current;
                check(reader_->GetCurrentMediaType(audio_stream, &current), "the sound's format");
                UINT32 rate = 0, channels = 0, bits = 0;
                current->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
                current->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
                current->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
                if (rate && channels && bits == 16) {
                    info_.audio_rate = static_cast<int>(rate);
                    info_.audio_channels = static_cast<int>(channels);
                    info_.audio_codec = codec_name(audio_subtype);
                }
            }
            if (!info_.audio_rate) {
                reader_->SetStreamSelection(audio_stream, FALSE);
                info_.audio_codec = codec_name(audio_subtype) + " (no decoder: not played)";
            }
        }
        if (!info_.audio_rate) audio_done_ = true;

        PROPVARIANT duration;
        PropVariantInit(&duration);
        if (SUCCEEDED(reader_->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &duration)) &&
            duration.vt == VT_UI8)
            info_.seconds = static_cast<double>(duration.uhVal.QuadPart) / 1e7;
        PropVariantClear(&duration);
    }

    void read_video_type() {
        ComPtr<IMFMediaType> type;
        check(reader_->GetCurrentMediaType(video_stream, &type), "the picture's format");
        GUID subtype{};
        type->GetGUID(MF_MT_SUBTYPE, &subtype);
        nv12_ = subtype == MFVideoFormat_NV12;
        UINT32 w = 0, h = 0;
        check(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h), "the picture's size");
        buffer_width_ = static_cast<int>(w);
        buffer_height_ = static_cast<int>(h);
        MFVideoArea area{};
        crop_x_ = crop_y_ = 0;
        info_.width = buffer_width_;
        info_.height = buffer_height_;
        for (const GUID& key : {MF_MT_MINIMUM_DISPLAY_APERTURE, MF_MT_GEOMETRIC_APERTURE, MF_MT_PAN_SCAN_APERTURE})
            if (SUCCEEDED(type->GetBlob(key, reinterpret_cast<UINT8*>(&area), sizeof area, nullptr)) && area.Area.cx > 0 &&
                area.Area.cy > 0) {
                crop_x_ = std::max<int>(0, area.OffsetX.value) & ~1;
                crop_y_ = std::max<int>(0, area.OffsetY.value) & ~1;
                info_.width = std::min<int>(area.Area.cx, buffer_width_ - crop_x_);
                info_.height = std::min<int>(area.Area.cy, buffer_height_ - crop_y_);
                break;
            }
        UINT32 stride = 0;
        stride_ = SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) ? static_cast<int>(static_cast<INT32>(stride)) : buffer_width_;
        if (stride_ <= 0) stride_ = buffer_width_;
        UINT32 par_n = 1, par_d = 1;
        if (FAILED(MFGetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, &par_n, &par_d)) || !par_n || !par_d) par_n = par_d = 1;
        info_.display_aspect = info_.height ? static_cast<float>(double(info_.width) * par_n / (double(info_.height) * par_d)) : 0.0F;
        UINT32 rate_n = 0, rate_d = 0;
        if (SUCCEEDED(MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &rate_n, &rate_d)) && rate_n && rate_d)
            info_.frame_rate = double(rate_n) / rate_d;
        if (info_.width <= 0 || info_.height <= 0) throw std::runtime_error("the pictures have no size");
    }

    void to_frame(IMFSample* sample, mpeg2::Frame& frame) {
        ComPtr<IMFMediaBuffer> buffer;
        check(sample->ConvertToContiguousBuffer(&buffer), "a picture's buffer");
        ComPtr<IMF2DBuffer> buffer2d;
        BYTE* base = nullptr;
        LONG pitch = stride_;
        DWORD length = 0;
        const bool locked2d = SUCCEEDED(buffer.As(&buffer2d)) && SUCCEEDED(buffer2d->Lock2D(&base, &pitch));
        if (!locked2d) {
            check(buffer->Lock(&base, nullptr, &length), "locking a picture");
            pitch = stride_;
        }
        struct Unlock {
            IMFMediaBuffer* b;
            IMF2DBuffer* b2;
            ~Unlock() { b2 ? b2->Unlock2D() : b->Unlock(); }
        } unlock{buffer.Get(), locked2d ? buffer2d.Get() : nullptr};
        if (pitch <= 0) throw std::runtime_error("a bottom-up picture");
        const std::size_t p = static_cast<std::size_t>(pitch);
        const std::size_t need = nv12_ ? p * buffer_height_ * 3 / 2 : p * buffer_height_ + 2 * (p / 2) * ((buffer_height_ + 1) / 2);
        if (!locked2d && length < need) throw std::runtime_error("a picture's buffer is short");
        const int w = info_.width, h = info_.height, cw = (w + 1) / 2, ch = (h + 1) / 2;
        frame.width = w;
        frame.height = h;
        frame.y.resize(std::size_t(w) * h);
        frame.u.resize(std::size_t(cw) * ch);
        frame.v.resize(frame.u.size());
        frame.type = 'I';
        frame.temporal_reference = 0;
        frame.decode_index = pictures_++;
        for (int r = 0; r < h; ++r)
            std::memcpy(frame.y.data() + std::size_t(r) * w, base + (std::size_t(crop_y_) + r) * p + crop_x_, std::size_t(w));
        const BYTE* chroma = base + p * buffer_height_;
        const int cx = crop_x_ / 2, cy = crop_y_ / 2;
        if (nv12_) {
            for (int r = 0; r < ch; ++r) {
                const BYTE* row = chroma + (std::size_t(cy) + r) * p + std::size_t(cx) * 2;
                std::uint8_t* u = frame.u.data() + std::size_t(r) * cw;
                std::uint8_t* v = frame.v.data() + std::size_t(r) * cw;
                for (int c = 0; c < cw; ++c) {
                    u[c] = row[2 * c];
                    v[c] = row[2 * c + 1];
                }
            }
        } else {
            const std::size_t cp = p / 2;
            const BYTE* u_plane = chroma;
            const BYTE* v_plane = chroma + cp * ((buffer_height_ + 1) / 2);
            for (int r = 0; r < ch; ++r) {
                std::memcpy(frame.u.data() + std::size_t(r) * cw, u_plane + (std::size_t(cy) + r) * cp + cx, std::size_t(cw));
                std::memcpy(frame.v.data() + std::size_t(r) * cw, v_plane + (std::size_t(cy) + r) * cp + cx, std::size_t(cw));
            }
        }
    }

    bool fail_video(const std::string& why) {
        video_done_ = true;
        if (error_.empty()) error_ = why;
        return false;
    }

    void close() {
        reader_.Reset();
        if (started_) MFShutdown();
        started_ = false;
        if (com_) CoUninitialize();
        com_ = false;
    }

    ComPtr<IMFSourceReader> reader_;
    PlatformMovieInfo info_;
    bool com_ = false, started_ = false;
    bool nv12_ = true;
    int buffer_width_ = 0, buffer_height_ = 0, stride_ = 0, crop_x_ = 0, crop_y_ = 0;
    bool video_done_ = false, audio_done_ = false;
    std::uint64_t pictures_ = 0;
    std::string error_;
};

}  // namespace

PlatformMovieBackend media_foundation_movie_backend() {
    PlatformMovieBackend backend;
    backend.name = "Media Foundation";
    backend.extensions = {".mp4", ".m4v", ".mov"};
    backend.open = [](MovieFile file) -> std::unique_ptr<PlatformMovieDecoder> { return std::make_unique<MediaFoundationMovie>(std::move(file)); };
    return backend;
}

}  // namespace omega::movie
