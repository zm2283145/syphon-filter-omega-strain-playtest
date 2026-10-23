#include "sound_iop_host.hpp"

#include "binary.hpp"
#include "bank_sounds.hpp"
#include "mod_set.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <sstream>
#include <stdexcept>

namespace omega {
namespace {

[[noreturn]] void unhosted(const char* address, const std::string& what) {
    throw std::runtime_error(std::string("Game sound host: ") + what + " (" + address + ") is not hosted");
}

std::string hex(std::uint32_t value) {
    std::ostringstream s;
    s << std::hex << value;
    return s.str();
}

std::vector<std::string> components(const std::string& path) {
    std::vector<std::string> out;
    std::string part;
    for (const char c : path) {
        if (c == '\\' || c == '/') {
            if (!part.empty()) out.push_back(part);
            part.clear();
        } else {
            part.push_back(c);
        }
    }
    if (!part.empty()) out.push_back(part);
    return out;
}

constexpr std::uint64_t sector_bytes = 2048;

// Checkpoint 132md13: "<key> <- <mod folder> (<file>[, audio_overrides <target>][, volume <n>%])".
std::string source_label(const std::string& key, const mods::ModSet::AudioSource& source) {
    std::string label = key + " <- " + source.mod->folder + " (" + source.path.filename().string();
    if (source.via.starts_with("audio_overrides ")) label += ", " + source.via;
    if (source.gain != 1.0F) label += ", volume " + std::to_string(static_cast<int>(source.gain * 100.0F + 0.5F)) + "%";
    return label + ")";
}

// The fitted samples the mods' streams may hold decoded at load (132md13): PC 64 MB, the Vita 8 MB (its 64 MB mod
// cap, research/MODDING.md "Mods on the Vita").
std::size_t preload_budget() { return mods::current_platform() == "vita" ? std::size_t(8) << 20 : std::size_t(64) << 20; }

}

SoundIopHost::~SoundIopHost() {
    preload_stop_->store(true);
    for (auto& worker : preload_workers_)
        if (worker.valid()) worker.wait();
}

SoundIopHost::SoundIopHost(DiscImage& disc, SoundIopHostConfig config, audio::Mixer& mixer)
    : disc_(disc), config_(std::move(config)), mixer_(mixer) {
    // The IRX's .rodata / .data at their vaddrs (snd_bank.hpp), the voices as
    // 182cc leaves them and the sound-RAM tree of ed20 (the module start).
    irx_ = disc_.read(disc_.file({"IRX", "989NOMID.IRX"}));
    const auto rodata = snd989::irx_section(irx_, ".rodata"), data = snd989::irx_section(irx_, ".data");
    spu_ = std::make_unique<audio::Spu2Voices>();
    engine_ = std::make_unique<snd989::BankEngine>(rodata.addr, rodata.bytes, data.addr, data.bytes, *spu_, volume_);
    engine_->init_voices();
    engine_->sram_init();
}

std::uint32_t SoundIopHost::owner_serial(std::uint32_t owner) {
    if (!owner_serial_hook) unhosted("00150c60", "an owned sound's object serial");
    return owner_serial_hook(owner);
}
GameSoundOwnerView SoundIopHost::owner_view(std::uint32_t owner) {
    if (!owner_view_hook) unhosted("001c75c0", "an owned sound's object view (+2c..+2e, +49, +50 root)");
    return owner_view_hook(owner);
}
std::uint8_t SoundIopHost::owner_state_108(std::uint32_t owner) {
    if (!owner_state_hook) unhosted("001c8e10", "an owned sound's object state (virtual +108)");
    return owner_state_hook(owner);
}
void SoundIopHost::owner_clear_2dc8(std::uint32_t owner) {
    if (!owner_clear_hook) unhosted("001c8e10", "an owned sound's object +2dc8");
    owner_clear_hook(owner);
}
void SoundIopHost::lip_sync_391810(std::uint32_t owner, const GameSoundResource& lpd) {
    if (!lip_sync_hook) unhosted("00391810", "the quip lip sync");
    // 0036dc60(slot +e4): +90 when set, else +88 (flags & 4 and loaded); the member's bytes (0036de50).
    const auto address = lpd.word_90 ? lpd.word_90 : lpd.word_88;
    if (!(lpd.flags_0 & 4) || !lpd.loaded_94 || !address) unhosted("0036dc60", "the data of an LPD that is not loaded");
    auto& block = lpd_block(address, "0036dc60");
    lip_sync_hook(owner, lpd.name_4, std::span<const std::uint8_t>(block.bytes.data(), block.member_size));
}
std::optional<std::array<float, 4>> SoundIopHost::listener_4ffc0c() {
    if (!listener_hook) unhosted("004ffc0c", "the listener of a positioned sound");
    return listener_hook();
}
std::optional<GameSoundCamera> SoundIopHost::camera_4ffc14() {
    if (!camera_hook) unhosted("004ffc14", "the camera of a positioned sound");
    return camera_hook();
}
// 001c8b9c: 003eae60(003eba50(), slot +e4, 001c9400, slot record, 0, 0, 1). The resource is the slot's own
// +e4 (the loader keeps its address); the record argument is passed as slot + 1 (a record address is never 0).
void SoundIopHost::load_lpd_3eae60(std::size_t slot, const GameSoundResource&) {
    if (!manager_) unhosted("003eae60", "the LPD load without the manager");
    auto& lpd = manager_->slots().at(slot).record.lpd_e4;
    lpd_loader_.load(lpd, 0x1c9400, static_cast<std::uint32_t>(slot + 1), 0, false, true);
    log_.push_back("003eae60 " + lpd.name_4 + " (slot " + std::to_string(slot) + ")");
}
// 001c76d0: 003eaa60(003eba50(), slot +e4, 1).
void SoundIopHost::release_lpd_3eaa60(std::size_t slot, const GameSoundResource&) {
    if (!manager_) unhosted("003eaa60", "the LPD release without the manager");
    auto& lpd = manager_->slots().at(slot).record.lpd_e4;
    const auto name = lpd.name_4;
    const auto r = lpd_loader_.release(lpd, 1);
    log_.push_back("003eaa60 " + name + " (slot " + std::to_string(slot) + ") = " + std::to_string(r));
}

std::int32_t SoundIopHost::lpd_service() {
    ++lpd_frame_;
    return lpd_loader_.service();
}

std::pair<const GameSoundHog*, std::size_t> SoundIopHost::lpd_member(const GameSoundResource& resource, const char* address) const {
    // 0036def0 / 0036de50 / 0036d950 / 0036d990: flags bit 2 is a HOG member (+98, the index +84 of 0036d2a0).
    if (!(resource.flags_0 & 2)) unhosted(address, "a loader resource that is not a HOG member (flags " + std::to_string(resource.flags_0) + ")");
    if (!manager_ || resource.hog_98 <= 0 || std::size_t(resource.hog_98) > manager_->hogs().size())
        unhosted(address, "a loader resource on an unknown HOG");
    const auto index = manager_->find(resource.hog_98, resource.name_4);
    if (index < 0) unhosted(address, "the member index -1 of '" + resource.name_4 + "'");
    return {&manager_->hogs()[std::size_t(resource.hog_98 - 1)], std::size_t(index)};
}

SoundIopHost::LpdBlock& SoundIopHost::lpd_block(std::uint32_t address, const char* site) {
    const auto it = lpd_blocks_.find(address);
    if (it == lpd_blocks_.end()) unhosted(site, "a loader block at " + hex(address) + " that this host did not allocate");
    return it->second;
}

void SoundIopHost::lpd_read(GameSoundResource& resource, std::uint32_t dest, std::uint32_t size, const char* address) {
    const auto [hog, index] = lpd_member(resource, address);
    auto& block = lpd_block(dest, address);
    if (size > block.bytes.size()) unhosted(address, "a read larger than its block");
    const auto& offsets = hog_offsets_.at(hog->path);
    const auto byte = std::uint64_t(hog->sector_84) * sector_bytes + std::uint32_t(hog->data_10) + offsets.at(index);
    const auto bytes = disc_.read({byte, size});
    std::copy(bytes.begin(), bytes.end(), block.bytes.begin());
    block.member_size = size;
    ++lpd_reads_;
    log_.push_back(std::string(address) + " " + resource.name_4 + ": " + std::to_string(size) + " bytes");
}

// A quip that pre-empts a queued lip-sync load (001c8a88..001c8b2c copy 0036e5d0's fresh resource over the
// slot's +e4) leaves the request on a flags-0 resource: 0036def0 returns 1 at once, 0036de50 returns size 0,
// 0036d950 reads nothing and 0036d8e0 reports done, so 003ebc90 completes it on a 0x7f-byte block.
void SoundIopHost::LpdHost::member_index_36def0(GameSoundResource& resource) {
    if (resource.flags_0 == 0) return;                                          // 0036df00: v0 = 1
    if (resource.flags_0 & 2) {
        owner_.lpd_member(resource, "0036def0");
        return;
    }
    if (resource.flags_0 & 1) unhosted("0036d680", "the open of a non-HOG loader resource (flags bit 1)");
}
std::uint32_t SoundIopHost::LpdHost::size_36de50(GameSoundResource& resource) {
    if (!(resource.flags_0 & 2)) {
        if (resource.flags_0 & 1) unhosted("0036de8c", "the size of a non-HOG loader resource (flags bit 1)");
        return 0;                                                               // 0036de88: v0 = 0
    }
    // 0036d380: *(+160)[i + 1] - *(+160)[i].
    const auto [hog, index] = owner_.lpd_member(resource, "0036de50");
    const auto& offsets = owner_.hog_offsets_.at(hog->path);
    if (index + 1 >= offsets.size()) unhosted("0036d380", "the offset after the last member");
    return offsets[index + 1] - offsets[index];
}
std::uint32_t SoundIopHost::LpdHost::alloc_36b180(std::uint32_t size) {
    // 0036b180(1, size, "fstream.cc", 0x37): host memory at 128-aligned addresses (+90 stays 0); it does not fail.
    const auto address = owner_.next_lpd_address_;
    owner_.next_lpd_address_ += ((size + 0x7fU) & ~0x7fU) + 0x80U;
    owner_.lpd_blocks_[address].bytes.assign(size, 0);
    return address;
}
void SoundIopHost::LpdHost::free_36b130(std::uint32_t address) {
    owner_.lpd_block(address, "0036b130");
    owner_.lpd_blocks_.erase(address);
}
void SoundIopHost::LpdHost::read_36d950(GameSoundResource& resource, std::uint32_t dest, std::uint32_t size) {
    if (!(resource.flags_0 & 2)) return;                                        // 0036d960: v0 = 0, nothing read
    owner_.lpd_read(resource, dest, size, "0036d950");
}
void SoundIopHost::LpdHost::read_36d990(GameSoundResource& resource, std::uint32_t dest, std::uint32_t size) {
    owner_.lpd_read(resource, dest, size, "0036d990");
}
std::int32_t SoundIopHost::LpdHost::poll_36d8e0(GameSoundResource& resource) {
    // 0036d8e0: without flags bit 4 the poll returns 1 at once (a resource
    // reset by a pre-empting quip, flags 0). Bit 4 and bit 2: 00369160(hog + 80),
    // the CD status; the host's read completed in 0036d950. Bit 4 and bit 1 only:
    // 00369160(+98). Bit 4 alone: 1.
    if (!(resource.flags_0 & 4)) return 1;
    if (resource.flags_0 & 2) {
        owner_.lpd_member(resource, "0036d8e0");
        return 1;
    }
    if (resource.flags_0 & 1) unhosted("0036d8e0", "the CD status of a non-HOG loader resource (flags bit 1)");
    return 1;
}
void SoundIopHost::LpdHost::callback(std::uint32_t function, GameSoundResource&, std::uint32_t arg, std::uint32_t) {
    if (function != 0x1c9400) unhosted("003eb2b0", "a loader callback other than 001c9400 (" + hex(function) + ")");
    if (!arg || !owner_.manager_) unhosted("001c9400", "the LPD callback without its slot record");
    owner_.manager_->lpd_loaded(arg - 1);
}

bool SoundIopHost::hud_4ffc2c() {
    if (!hud) unhosted("004ffc2c", "the HUD object query");
    return hud();
}

void SoundIopHost::subtitle_245e00(const std::array<std::uint8_t, 16>& name) {
    if (!subtitle) unhosted("00245e00", "the quip subtitle");
    std::string text;
    for (const auto c : name) {
        if (!c) break;
        text.push_back(static_cast<char>(c));
    }
    subtitle(text);
}

void SoundIopHost::schedule_3ea1d0(std::int32_t delay, std::function<void()> event) {
    if (!schedule) unhosted("003ea1d0", "the script callback list");
    schedule(delay, std::move(event));
}

GameSoundHog SoundIopHost::open_hog_3f96e0(const std::string& path) {
    // 003f96e0 opens the HOG through the file layer and keeps its header; the
    // port reads the same fields from the ISO (a missing file throws below).
    Extent container;
    try {
        container = disc_.file(components(path));
    } catch (const std::exception& e) {
        unhosted("003f96e0", "opening '" + path + "' (" + e.what() + ")");
    }
    if (container.offset % sector_bytes) throw std::runtime_error("Game sound host: HOG not sector aligned: " + path);
    const auto head = disc_.read({container.offset, 20});
    const auto index_size = read_u32(head, 16);
    const auto index = disc_.read({container.offset, index_size});
    GameSoundHog hog;
    hog.path = path;
    hog.loaded_14c = true;
    hog.sector_84 = static_cast<std::uint32_t>(container.offset / sector_bytes);
    hog.data_10 = static_cast<std::int32_t>(index_size);
    hog.names = hog_member_names(index, container.size);
    const auto table = read_u32(index, 8);
    std::vector<std::uint32_t> offsets_160;
    for (std::size_t i = 0; i < hog.names.size(); ++i) {
        hog.offsets.push_back(read_u32(index, table + i * 4));
        offsets_160.push_back(hog.offsets.back());
        const auto offset = std::uint64_t(index_size) + hog.offsets.back();
        if (offset % sector_bytes == 0) {
            const auto sector = hog.sector_84 + static_cast<std::uint32_t>(offset / sector_bytes);
            members_[sector] = hog.names[i];
            member_keys_[sector] = mods::member_key(mods::asset_key(components(path)), hog.names[i]);
        }
    }
    // 0036d380 reads the entry after a member: the table has count + 1 offsets.
    if (table + (hog.names.size() + 1) * 4 > index.size()) throw std::runtime_error("Game sound host: HOG offset table past its index: " + path);
    offsets_160.push_back(read_u32(index, table + hog.names.size() * 4));
    hog_offsets_[path] = std::move(offsets_160);
    log_.push_back("003f96e0 " + path + ": " + std::to_string(hog.names.size()) + " members at sector " + std::to_string(hog.sector_84));
    preload_mod_streams(path, hog);
    return hog;
}

void SoundIopHost::preload_mod_streams(const std::string& path, const GameSoundHog& hog) {
    const auto* set = mods::active();
    if (!set || !set->has_sounds()) return;
    struct Job {
        std::string label;
        std::filesystem::path file;
        std::uint32_t rate = 0, channels = 0;
        float gain = 1;
        std::promise<std::shared_ptr<audio::PcmClip>> done;
    };
    std::vector<Job> jobs;
    for (std::size_t i = 0; i < hog.names.size(); ++i) {
        std::string name = hog.names[i];
        for (auto& c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (!name.ends_with(".VPK") && !name.ends_with(".VAG")) continue;
        const auto offset = std::uint64_t(hog.data_10) + hog.offsets[i];
        if (offset % sector_bytes) continue;
        const auto sector = hog.sector_84 + static_cast<std::uint32_t>(offset / sector_bytes);
        const auto key = member_keys_.find(sector);
        if (key == member_keys_.end() || preloads_.contains(key->second) || replaced_.contains({sector, false}) ||
            replaced_.contains({sector, true}))
            continue;
        const auto source = set->audio(key->second);
        if (!source) continue;
        Job job;
        try {
            const auto head = disc_.read({std::uint64_t(sector) * sector_bytes, 0x30});
            if (audio::is_vpk(head)) {
                const auto vpk = audio::parse_vpk_header(head);
                job.rate = vpk.rate;
                job.channels = vpk.channels;
            } else if (audio::is_vag(head)) {
                job.rate = audio::parse_vag_header(head).rate;
                job.channels = 1;
            }
        } catch (const std::exception&) {
            continue;   // the stream start reads it again and reports
        }
        if (!job.rate || !job.channels) continue;
        job.label = source_label(key->second, *source);
        job.file = source->path;
        job.gain = source->gain;
        preloads_[key->second] = {job.rate, job.channels, job.done.get_future().share()};
        jobs.push_back(std::move(job));
    }
    if (jobs.empty()) return;
    mods::note(path + ": " + std::to_string(jobs.size()) + " mod stream(s) decoding at load");
    preload_workers_.push_back(std::async(std::launch::async, [jobs = std::move(jobs), stop = preload_stop_,
                                                               used = preload_used_, budget = preload_budget()]() mutable {
        for (auto& job : jobs) {
            std::shared_ptr<audio::PcmClip> clip;
            try {
                if (!stop->load()) {
                    mods::FittedAudio fitted(mods::read_file(job.file), job.rate, job.channels, std::nullopt, job.gain);
                    const auto bytes = fitted.clip()->samples.size() * sizeof(std::int16_t);
                    if (used->fetch_add(bytes) + bytes > budget) {
                        used->fetch_sub(bytes);
                        mods::note("sound " + job.label + ": not decoded at load (past the " +
                                   std::to_string(budget >> 20) + " MB budget); decoded as it plays");
                    } else {
                        while (!fitted.done() && !stop->load()) fitted.decode_to(fitted.decoded_frames() + 65536);
                        if (fitted.done()) clip = fitted.clip();
                        else used->fetch_sub(bytes);
                    }
                }
            } catch (const std::exception&) {
                // A file the stream start refuses too: it reports there.
            }
            job.done.set_value(std::move(clip));
        }
    }));
}

std::string SoundIopHost::member_at(std::uint32_t sector) const {
    const auto it = members_.find(sector);
    return it == members_.end() ? "sector " + std::to_string(sector) : it->second;
}

std::optional<std::int32_t> SoundIopHost::stream_volume_10(std::uint32_t handle) const {
    const auto it = streams_.find(handle);
    if (it == streams_.end()) return std::nullopt;
    return it->second.volume;
}

std::optional<bool> SoundIopHost::stream_paused(std::uint32_t handle) const {
    const auto it = streams_.find(handle);
    if (it == streams_.end()) return std::nullopt;
    return it->second.paused;
}

// Port-only (checkpoint 132md4, research/MODDING.md "Sounds and music"): a mod's sounds\<member>.wav / .ogg for the
// stream at `sector`, fitted to the original's rate and channels; a stream that does not loop is cut or padded to the
// original's length, so its end (the voice end the game's scripts, subtitles and lip sync wait for) falls on the same
// frame. Only the headers are read here: the samples are decoded a piece ahead of the mix (decode_ahead). nullptr:
// no replacement, or one refused (logged; the disc's stream is played).
std::shared_ptr<const audio::PcmClip> SoundIopHost::replacement_clip(std::uint32_t sector, bool loop) {
    const auto* set = mods::active();
    if (!set || !set->has_sounds()) return nullptr;
    const auto key = member_keys_.find(sector);
    if (key == member_keys_.end()) return nullptr;
    if (const auto it = replaced_.find({sector, loop}); it != replaced_.end()) return it->second;
    const auto source = set->audio(key->second);
    if (!source) return nullptr;
    const auto label = source_label(key->second, *source);
    std::shared_ptr<const audio::PcmClip> result;
    try {
        const std::uint64_t at = std::uint64_t(sector) * sector_bytes;
        const auto head = disc_.read({at, 0x30});
        std::uint32_t rate = 0, channels = 0;
        std::size_t frames = 0;
        bool repeat = false;
        if (audio::is_vpk(head)) {
            // The clip is sized from the header up front: no chunk is read.
            const audio::VpkProgressive shape(head, [this, at](std::uint64_t offset, std::size_t size) {
                return disc_.read({at + offset, size});
            });
            rate = shape.clip()->rate;
            channels = shape.clip()->channels;
            frames = shape.clip()->frames();
        } else {
            const auto original = audio::decode_stream(disc_.read({at, audio::stream_bytes(head)}));
            rate = original.rate;
            channels = original.channels;
            frames = original.frames();
            repeat = original.repeat;
        }
        // 132md13: decoded at load (preload_mod_streams): the clip whole, cut or padded as the progressive one is.
        if (const auto pre = preloads_.find(key->second);
            pre != preloads_.end() && pre->second.rate == rate && pre->second.channels == channels &&
            pre->second.clip.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            if (const auto& whole = pre->second.clip.get()) {
                auto clip = loop ? whole : std::make_shared<audio::PcmClip>(*whole);
                if (!loop) clip->samples.resize(frames * channels, 0);
                clip->repeat = repeat;
                std::ostringstream what;
                what.setf(std::ios::fixed);
                what.precision(2);
                what << "sound " << label << ": decoded at load -> " << rate << " Hz x" << channels << " "
                     << double(clip->frames()) / rate << " s ("
                     << (loop ? "a looping stream: its own length" : "the original's length") << ")";
                mods::note(what.str());
                replaced_[{sector, loop}] = clip;
                return clip;
            }
        }
        auto fitted = std::make_unique<mods::FittedAudio>(mods::read_file(source->path), rate, channels,
                                                          loop ? std::nullopt : std::optional<std::size_t>(frames), source->gain);
        fitted->clip()->repeat = repeat;
        fitted->decode_to(1);
        std::ostringstream what;
        what.setf(std::ios::fixed);
        what.precision(2);
        what << "sound " << label << ": " << fitted->source_rate() << " Hz x" << fitted->source_channels() << " "
             << double(fitted->source_frames()) / fitted->source_rate() << " s -> " << rate << " Hz x" << channels
             << " " << double(fitted->clip()->frames()) / rate << " s ("
             << (loop ? "a looping stream: its own length" : "the original's length") << ")";
        mods::note(what.str());
        result = fitted->clip();
        if (!fitted->done()) decoding_mods_.emplace_back(key->second, std::move(fitted));
    } catch (const std::exception& e) {
        mods::note("sound " + label + " refused (" + e.what() + "); the disc's stream is played");
    }
    replaced_[{sector, loop}] = result;
    return result;
}

// Port-only (checkpoint 132md5, research/MODDING.md "Sound effects in the banks"): each bank sample n with a mod's
// sounds\<bank member>\<n>.wav / .ogg is fitted to mono at the rate the bank plays it by default
// (mods::bank_sample_rate), a sample that does not loop cut or padded to the original's length (its voice ends on the
// same tick), encoded to PS-ADPCM and registered as an overlay at the sample's sound RAM address: voices the bank
// engine keys on there play it. The engine, its memory and the sound RAM are the disc's. With no sounds\ entry nothing
// is parsed.
void SoundIopHost::bank_sample_mods(std::uint32_t sector, std::span<const std::uint8_t> file, std::uint32_t spu) {
    const auto* set = mods::active();
    if (!set || !set->has_sounds()) return;
    const auto key = member_keys_.find(sector);
    if (key == member_keys_.end() || !mods::is_sound_bank(file)) return;
    std::vector<mods::BankSample> samples;
    std::string problem;
    try {
        samples = mods::bank_samples(file);
    } catch (const std::exception& e) {
        problem = std::string("the bank's sample table: ") + e.what();
    }
    // The entries below the bank that name no sample of it (once per bank: noted, never played).
    if (bank_checked_.insert(key->second).second)
        for (const auto& entry : set->entries()) {
            // 132md13: an audio_overrides= target below this bank (<bank>/<n or sound name>) naming none of its samples.
            if (entry.kind == "audio") {
                const auto slash = entry.key.rfind('/');
                if (slash == std::string::npos || mods::audio_target_match(entry.key.substr(0, slash), key->second) != 2) continue;
                const bool named = problem.empty() && std::any_of(samples.begin(), samples.end(), [&](const mods::BankSample& s) {
                    return mods::audio_target_match(entry.key, key->second + "/" + std::to_string(s.index), s.sounds) != 0;
                });
                if (!named)
                    mods::note("sound " + key->second + " <- " + entry.source.mod->folder + " (" +
                               entry.source.path.filename().string() + ", audio_overrides " + entry.key + "): " +
                               (problem.empty() ? "no sample of this bank is numbered or played by that name"
                                                : "the bank's samples are not read (" + problem + ")") +
                               "; nothing replaced in it");
                continue;
            }
            if (entry.kind != "sounds" || !entry.key.starts_with(key->second + "/")) continue;
            const auto number = entry.key.substr(key->second.size() + 1);
            const bool sample = !number.empty() && number.size() <= 6 &&
                                std::all_of(number.begin(), number.end(), [](char c) { return c >= '0' && c <= '9'; }) &&
                                std::stoul(number) < samples.size();
            if (problem.empty() && sample) continue;
            mods::note("sound " + entry.key + " <- " + entry.source.mod->folder + " (" +
                       entry.source.path.filename().string() + ") refused (" +
                       (problem.empty() ? "the bank has " + std::to_string(samples.size()) + " samples, numbered from 0" : problem) +
                       "); not played");
        }
    if (!problem.empty()) return;
    std::size_t replaced = 0;
    for (const auto& sample : samples) {
        const auto sample_key = key->second + "/" + std::to_string(sample.index);
        const auto source = set->audio(sample_key, sample.sounds);
        if (!source) continue;
        auto cached = bank_overlays_.find(sample_key);
        if (cached == bank_overlays_.end()) {
            const auto label = source_label(sample_key, *source);
            // 132md13: a sample a sound name selected may be played by other sounds too (they all change).
            std::string shared;
            if (source->via.starts_with("audio_overrides ")) {
                const auto target = source->via.substr(16);
                if (mods::audio_target_match(target, sample_key) != 2)
                    for (const auto& name : sample.sounds)
                        if (!mods::audio_target_match(target, sample_key, std::span<const std::string>(&name, 1)))
                            shared += (shared.empty() ? "" : ", ") + name;
            }
            std::shared_ptr<const std::vector<std::uint8_t>> blocks;
            try {
                const auto decoded = mods::decode_audio_file(mods::read_file(source->path));
                const auto fitted = mods::fit_audio(decoded, sample.rate, 1,
                                                    sample.loops ? std::nullopt : std::optional<std::size_t>(sample.frames()),
                                                    source->gain);
                blocks = std::make_shared<const std::vector<std::uint8_t>>(mods::encode_adpcm(fitted.samples, sample.loops));
                std::ostringstream what;
                what.setf(std::ios::fixed);
                what.precision(2);
                what << "sound " << label << ": " << decoded.rate << " Hz x" << decoded.channels << " "
                     << double(decoded.frames()) / decoded.rate << " s -> bank sample " << sample.rate << " Hz x1 "
                     << double(fitted.frames()) / sample.rate << " s, " << blocks->size() / 16 << " ADPCM blocks ("
                     << (sample.loops ? "a looping sample: its own length, looped whole" : "the original's length")
                     << ")";
                if (!shared.empty()) what << "; the sample is shared: " << shared << " play it too";
                mods::note(what.str());
            } catch (const std::exception& e) {
                mods::note("sound " + label + " refused (" + e.what() + "); the disc's sample is played");
            }
            cached = bank_overlays_.emplace(sample_key, std::move(blocks)).first;
        }
        if (!cached->second) continue;
        spu_->set_overlay(spu + sample.offset, cached->second);
        ++replaced;
    }
    if (replaced)
        log_.push_back("bank " + key->second + ": " + std::to_string(replaced) + " of " + std::to_string(samples.size()) +
                       " samples replaced by mods (port-only, sound RAM 0x" + hex(spu) + ")");
}

std::shared_ptr<const audio::PcmClip> SoundIopHost::clip(std::uint32_t sector, bool loop) {
    if (auto replaced = replacement_clip(sector, loop)) return replaced;
    if (const auto it = clips_.find(sector); it != clips_.end()) return it->second;
    const std::uint64_t at = std::uint64_t(sector) * sector_bytes;
    const auto head = disc_.read({at, 0x30});
    if (audio::is_vpk(head)) {
        // Port-only (checkpoint 132win5): music is read and decoded a chunk ahead of the mix (decode_ahead), not
        // all at the stream start; the samples mixed are decode_stream's.
        auto progressive = std::make_unique<audio::VpkProgressive>(
            head, [this, at](std::uint64_t offset, std::size_t size) { return disc_.read({at + offset, size}); });
        progressive->decode_to(1);
        std::shared_ptr<const audio::PcmClip> decoded = progressive->clip();
        if (!progressive->done()) decoding_.push_back(std::move(progressive));
        clips_[sector] = decoded;
        return decoded;
    }
    const auto bytes = disc_.read({at, audio::stream_bytes(head)});
    auto decoded = std::make_shared<const audio::PcmClip>(audio::decode_stream(bytes));
    clips_[sector] = decoded;
    return decoded;
}

#ifdef __vita__
void SoundIopHost::drop_unheard_clips() {
    std::size_t bytes = 0, count = 0;
    for (auto it = clips_.begin(); it != clips_.end();) {
        const auto decoder = std::find_if(decoding_.begin(), decoding_.end(),
                                          [&](const auto& d) { return d->clip() == it->second; });
        if (it->second.use_count() > (decoder == decoding_.end() ? 1 : 2)) {
            ++it;
            continue;
        }
        bytes += it->second->samples.capacity() * sizeof(std::int16_t);
        ++count;
        if (decoder != decoding_.end()) decoding_.erase(decoder);
        it = clips_.erase(it);
    }
    if (count)
        log_.push_back("PS Vita (port-only, 132vt2b2): " + std::to_string(count) + " stream clip(s) no longer played freed, " +
                       std::to_string(bytes >> 10) + " KB");
}
#endif

void SoundIopHost::decode_ahead(std::size_t frames) {
    for (auto it = decoding_.begin(); it != decoding_.end();) {
        auto& d = **it;
        d.decode_to(mixer_.frames_read_by(d.clip().get(), frames));
        it = d.done() ? decoding_.erase(it) : std::next(it);
    }
    for (auto it = decoding_mods_.begin(); it != decoding_mods_.end();) {
        auto& d = *it->second;
        try {
            d.decode_to(mixer_.frames_read_by(d.clip().get(), frames));
        } catch (const std::exception& e) {
            std::ostringstream what;
            what.setf(std::ios::fixed);
            what.precision(2);
            what << "sound " << it->first << ": decoding stopped at " << double(d.decoded_frames()) / d.clip()->rate
                 << " s (" << e.what() << "); the rest is silent";
            mods::note(what.str());
            it = decoding_mods_.erase(it);
            continue;
        }
        it = d.done() ? decoding_mods_.erase(it) : std::next(it);
    }
}

std::int32_t SoundIopHost::snd(std::uint32_t address, const std::vector<std::int32_t>& args) {
    const auto arg = [&](std::size_t i) {
        if (i >= args.size()) throw std::runtime_error("Game sound host: missing argument to " + hex(address));
        return args[i];
    };
    switch (address) {
    case 0x3a8120: {                                             // cmd 0x60, 17b0 -> 1b3b8: a ducker
        std::array<std::int32_t, 6> def{};
        for (std::size_t i = 0; i < 6; ++i) def[i] = arg(1 + i);
        snd989::set_ducker(volume_, static_cast<std::uint32_t>(arg(0)), def[0] == -1 ? nullptr : &def);
        return 0;
    }
    case 0x3a80f0:                                               // cmd 0xb (0554 -> 1a544): the output mode halfword 0x1e51c
        volume_.play_mode = static_cast<std::int16_t>(arg(0));
        return 0;
    case 0x3a81d0: {                                             // cmd 9 (04fc -> 1a408): a group master volume
        const auto group = arg(0);
        if (group == 15 || group == 16) unhosted("0001a43c", "the master volume of group " + std::to_string(group));
        // *0x1c614 (a volume callback) is 0 in the image; 1af50 refreshes the group's playing voices
        // and b3d4 its stream, as the duck tick's refresh below.
        const auto r = snd989::set_master_volume(volume_, group, arg(1));
        for (auto& [handle, s] : streams_)
            if (r.refresh_mask & snd989::voice_group_bit(s.group)) apply_volume(s);
        engine_->reapply_groups(r.refresh_mask);
        engine_errors();
        log_.push_back("003a81d0 group " + std::to_string(group) + " master " + std::to_string(volume_.master[std::size_t(group)]));
        return 0;
    }
    case 0x3a6cd0:                                               // init: voice / stream setup
    case 0x3a8240:                                               // the end of the bank loads
        return 0;
    case 0x3a7330:                                               // level load: 10324, the stream side's group range
        engine_->set_stream_group(arg(0), arg(1), arg(2));
        return 0;
    case 0x3a6d10:                                               // init: d0e4's reverb work area on core 0, type 4
        engine_->reverb_area(0, arg(1));
        engine_errors();
        return 0;
    case 0x3a7370: {                                             // level load: the stream buffers' sound RAM (ed70)
        // *0053dc00 == 1 (the last cmd 0x2a reply, b4c: fc68 != 0): 0 without an IOP call.
        if (streams_open_) {
            log_.push_back("003a7370: the stream buffers are open (0053dc00), no IOP call");
            return 0;
        }
        if (!bank_commands_.empty()) snd(0x3a85e0, {});          // the queue (00542878) drained first
        const auto count = static_cast<std::uint32_t>(arg(0)), size = static_cast<std::uint32_t>(arg(1));
        const auto pages = std::max<std::uint32_t>(2, size >> 12) << 12;
        const auto at = engine_->sram_alloc(pages * count);
        if (!at) {
            log_.push_back("003a7370: no sound RAM for the stream buffers (989snd error 0x2c)");
            return 0;
        }
        streams_open_ = true;
        stream_ram_ = at;
        stream_ram_size_ = pages * count;
        log_.push_back("003a7370 stream buffers: " + std::to_string(count) + " x 0x" + hex(pages) + " at SPU 0x" + hex(at));
        return 1;
    }
    case 0x3a7200: {                                             // snd_CloseStreams: cmd 0x35 (0e04 -> 173d4)
        if (!streams_open_) return 0;                            // *0053dc00 == 0
        if (!bank_commands_.empty()) snd(0x3a85e0, {});
        // 173d4: 11260 again (the streams are gone), then 17220 frees the buffers' sound RAM (ef68).
        for (auto& [handle, s] : streams_)
            if (s.voice && mixer_.playing(*s.voice)) mixer_.stop(*s.voice);
        streams_.clear();
#ifdef __vita__
        drop_unheard_clips();
#endif
        engine_->sram_free(stream_ram_, stream_ram_size_);
        engine_errors();
        log_.push_back("003a7200 stream buffers closed: SPU 0x" + hex(stream_ram_) + " + 0x" + hex(stream_ram_size_) + " freed");
        streams_open_ = false;
        stream_ram_ = stream_ram_size_ = 0;
        return 0;
    }
    case 0x3a7fe0: {                                             // cmd 0x18 (0814 -> e2e0): stop everything
        // e2e0 on the stream handlers (type 4: e3e4 -> 119f4) at once, as every host stream call; the bank
        // handlers and the voices at the flush.
        for (auto& [handle, s] : streams_) {
            if (s.ended || s.stopped) continue;
            stop_stream(s);
            log_.push_back("003a7fe0 stop " + member_at(s.sector) + " (handle " + std::to_string(handle) + ")");
        }
        bank_command(address, args);
        return 0;
    }
    case 0x3a7310: {                                             // cmd 0x34 (0de4 -> 11260): the stream channels released
        for (auto& [handle, s] : streams_)
            if (s.voice && mixer_.playing(*s.voice)) mixer_.stop(*s.voice);
        if (!streams_.empty()) log_.push_back("003a7310 streams released: " + std::to_string(streams_.size()));
        streams_.clear();
#ifdef __vita__
        drop_unheard_clips();
#endif
        bank_command(address, args);
        return 0;
    }
    case 0x3a8210:                                               // a bank unload (cmd 6, 048c -> 8e30)
        bank_command(address, args);
        return 0;
    case 0x3a81a0:                                               // a free stream channel
        return 1;
    case 0x3a8260:                                               // a bank load (IRX 93ac)
        return load_bank(static_cast<std::uint32_t>(arg(0)), static_cast<std::uint32_t>(arg(1)));
    case 0x3a8000: case 0x3a8070:                                // user data / play (cmds 0x56 / 0x5c)
        bank_command(address, args);
        return 0;
    case 0x3a7190: {                                             // stream start (IRX cmd 0x2c, handler 0c28 -> 10554)
        const auto sector = static_cast<std::uint32_t>(arg(0));
        const auto slot = static_cast<std::size_t>(arg(10));
        if (arg(9) & 1) unhosted("0001090c", "a stream started with flag 1 (stream +16 bit 2: no voice volume writes)");
        for (auto it = streams_.begin(); it != streams_.end();)
            it = (it->second.slot == slot && (it->second.ended || it->second.stopped)) ? streams_.erase(it) : std::next(it);
        Stream s;
        s.slot = slot;
        s.sector = sector;
        s.clip = clip(sector, (arg(9) & 4) != 0);
        // 10554's new stream (0x10870..): +10 = (u16 volume * 127) >> 10 (0x108c8), +12 = the pan halfword,
        // +17 = argument 6, the volume group the voices take as +1b (1 music, 2 quips, 0xe owned quips).
        // The voice plays at once: nothing in 10554 or the restart 09cc waits for a 003a7f00.
        s.group = static_cast<std::int8_t>(arg(6));
        s.volume = static_cast<std::int32_t>((static_cast<std::uint32_t>(arg(4)) & 0xffff) * 127U >> 10);
        s.pan = (static_cast<std::uint32_t>(arg(5)) + 2U < 2U) ? 0 : static_cast<std::int16_t>(arg(5));  // 0x108d8
        s.out.assign(s.clip->channels, {0, 0});
        s.voice = mixer_.start(s.clip, 0.0F, 0.0F, (arg(9) & 4) != 0);
        apply_volume(s);
        const auto handle = next_handle_++;
        streams_[handle] = std::move(s);
        replies_.push_back({static_cast<std::int32_t>(handle), slot});
        ++started_;
        log_.push_back("003a7190 stream " + member_at(sector) + " slot " + std::to_string(slot) + " volume "
                       + std::to_string(arg(4)) + " pan " + std::to_string(arg(5)) + " group " + std::to_string(arg(6))
                       + " loop " + std::to_string(arg(9)) + " -> handle " + std::to_string(handle));
        return 0;
    }
    case 0x3a7f00: {                                             // stream restart: cmd 0x21, 09cc -> c268
        if (static_cast<std::uint32_t>(arg(0)) & 0x80000000U) {  // a bank handle: c268's reply at the flush
            bank_command(address, args);
            return 0;
        }
        auto& s = stream(static_cast<std::uint32_t>(arg(0)), "003a7f00");
        if (s.ended || s.stopped) return 0;                      // de2c finds no stream: c268 returns 0
        auto flags = static_cast<std::uint32_t>(arg(1));
        if ((flags & 6) == 6) flags &= ~2U;
        const auto volume = (flags & 1) ? arg(2) : 0x7fffffff;
        const auto pan = (flags & 2) ? arg(3) : -2;
        if (flags & 3) {                                         // c1d4 -> 179e4 (17768 / 177ac)
            s.volume = snd989::stream_volume(s.volume, volume);
            s.pan = snd989::stream_pan(s.pan, pan);
            apply_volume(s);
        }
        if (flags & 4) {                                         // 2bbc(handle, pan, 0, 0x1e, 4)
            std::size_t used = 0;                                // e9fc: the 64 records of the pool 0x2386c
            for (const auto& [h, other] : streams_) used += other.fade.has_value() + other.glide.has_value();
            snd989::GlideStream view{static_cast<std::int16_t>(s.pan), s.glide};
            const auto call = snd989::glide_start(view, used < 64, arg(3), 0, 0x1e, 4);
            s.glide = view.glide;
            glide_result(s, call.set_pan);
        }
        if (flags & 0x18) unhosted((flags & 8) ? "0000c5e4" : "0000c580", "the 989snd restart flag " + hex(flags));
        return 0;
    }
    case 0x3a7ec0: {                                             // stream fade: cmd 0x22, 0a14 -> 3b1c(handle, s16 vol, time, mode)
        const auto handle = static_cast<std::uint32_t>(arg(0));
        auto& s = stream(handle, "003a7ec0");
        log_.push_back("003a7ec0 fade " + member_at(s.sector) + " (handle " + std::to_string(handle) + ") to "
                       + std::to_string(static_cast<std::int16_t>(arg(1))) + " over " + std::to_string(static_cast<std::int16_t>(arg(2)))
                       + " ticks mode " + std::to_string(static_cast<std::int16_t>(arg(3))));
        if (s.ended || s.stopped) return 0;                      // de2c finds no stream: 3b1c returns
        std::size_t used = 0;                                    // e9fc: the 64 records of the pool 0x2386c
        for (const auto& [h, other] : streams_) used += other.fade.has_value() + other.glide.has_value();
        auto view = fade_view(s);
        const auto call = snd989::fade_start(view, used < 64, static_cast<std::int16_t>(arg(1)), static_cast<std::int16_t>(arg(2)),
                                             static_cast<std::int16_t>(arg(3)));
        s.fade = view.fade;
        fade_result(handle, s, call.stop, call.set_volume);
        return 0;
    }
    case 0x3a7fb0:                                               // cmd 0x16, 07cc -> e0a8: pause the groups' streams
    case 0x3a7f80: {                                             // cmd 0x17, 07f0 -> e214: resume them
        const bool pause = address == 0x3a7fb0;
        const auto mask = static_cast<std::uint32_t>(arg(0));
        bank_command(address, args);                             // the bank handlers of the groups, in order at the flush
        for (auto& [handle, s] : streams_) {
            if (s.ended || s.stopped || !(mask & snd989::voice_group_bit(s.group)) || s.paused == pause) continue;
            // Host streams are VAG streams (type 4): 11468 sets +16 bit 2 and pauses the voices, 116bc undoes it.
            s.paused = pause;
            if (s.voice) mixer_.pause(*s.voice, pause);
            log_.push_back(std::string(pause ? "003a7fb0 pause " : "003a7f80 resume ") + member_at(s.sector) + " (handle "
                           + std::to_string(handle) + ", group " + std::to_string(s.group) + ")");
        }
        return 0;
    }
    case 0x3a8040: {                                             // stream stop (cmd 0x15, 07a8 -> c060)
        const auto handle = static_cast<std::uint32_t>(arg(0));
        if (handle & 0x80000000U) {                              // a bank handle: c060 at the flush
            bank_command(address, args);
            return 0;
        }
        auto& s = stream(handle, "003a8040");
        if (s.voice && mixer_.playing(*s.voice)) mixer_.stop(*s.voice);
        log_.push_back("003a8040 stop " + member_at(s.sector) + " (handle " + std::to_string(handle) + ")");
        streams_.erase(handle);
        return 0;
    }
    case 0x3a7f50: {                                             // cmd 0x61, 0834 -> e174: c060 on every stream of the groups
        const auto mask = static_cast<std::uint32_t>(arg(0));
        bank_command(address, args);
        for (auto& [handle, s] : streams_) {
            if (s.ended || s.stopped || !(mask & snd989::voice_group_bit(s.group))) continue;
            stop_stream(s);
            log_.push_back("003a7f50 stop " + member_at(s.sector) + " (handle " + std::to_string(handle) + ", group "
                           + std::to_string(s.group) + ")");
        }
        return 0;
    }
    case 0x3a85e0: {                                             // the IOP replies (001c9430)
        if (!manager_) throw std::runtime_error("Game sound host: 003a85e0 without an attached manager");
        run_bank_commands();
        auto data = std::move(user_data_);
        user_data_.clear();
        for (const auto& [slot, words] : data) manager_->bank_user_data(slot, words);
        auto replies = std::move(replies_);
        replies_.clear();
        for (const auto& [result, slot] : replies) manager_->stream_result(result, slot);
        return 0;
    }
    default:
        unhosted(("0x" + hex(address)).c_str(), "the 989snd call");
    }
}

SoundIopHost::Stream& SoundIopHost::stream(std::uint32_t handle, const char* address) {
    const auto it = streams_.find(handle);
    if (it == streams_.end()) throw std::runtime_error(std::string("Game sound host: ") + address + " on an unknown stream handle");
    return it->second;
}

void SoundIopHost::apply_volume(Stream& s) {
    if (!s.voice) return;
    // The channel pans of the VAG / VPK setup (000149xx): mono 0 (0x14a74), stereo 270 and 90 (0x14a18).
    for (std::size_t c = 0; c < s.out.size(); ++c) {
        const std::int32_t channel_pan = s.out.size() == 1 ? 0 : (c == 0 ? 270 : 90);
        snd989::make_volume4(volume_, 127, s.volume, s.pan, 127, channel_pan, 127, 0, s.out[c]);
        // 00011890 / 00017bf0: the SPU voice volume (u16) AdjustVolToGroup >> 1, a 15-bit signed level.
        const auto spu = [&](std::size_t side) {
            const auto adjusted = static_cast<std::uint16_t>(snd989::adjust_vol_to_group(volume_, s.out[c][side], s.group));
            const auto level = static_cast<std::int32_t>(adjusted >> 1);
            return static_cast<float>(level >= 0x4000 ? level - 0x8000 : level) / 16384.0F;
        };
        mixer_.set_channel_gains(*s.voice, c, spu(0), spu(1));
    }
}

snd989::FadeStream SoundIopHost::fade_view(const Stream& s) {
    snd989::FadeStream view;
    view.handle = 0x84000000U;  // an active (bit 31) VAG stream (type 4)
    view.scale = 127;           // +c (0x108e4)
    view.volume_10 = static_cast<std::int16_t>(s.volume);
    view.fade = s.fade;
    return view;
}

void SoundIopHost::stop_stream(Stream& s) {
    if (s.voice && mixer_.playing(*s.voice)) mixer_.stop(*s.voice);
    s.voice.reset();
    s.fade.reset();
    s.glide.reset();
    s.stopped = true;
}

void SoundIopHost::glide_result(Stream& s, std::optional<std::int32_t> pan) {
    if (!pan) return;  // c1d4(handle, 0x7fffffff, pan)
    s.volume = snd989::stream_volume(s.volume, 0x7fffffff);
    s.pan = snd989::stream_pan(s.pan, *pan);
    apply_volume(s);
}

void SoundIopHost::fade_result(std::uint32_t handle, Stream& s, bool stop, std::optional<std::int32_t> volume) {
    if (volume) {  // c1d4(handle, v, -2)
        s.volume = snd989::stream_volume(s.volume, *volume);
        s.pan = snd989::stream_pan(s.pan, -2);
        apply_volume(s);
    }
    if (stop) {  // c060 (3bf8) / e3e4(stream, 1, 0, 0) (e7d0)
        stop_stream(s);
        log_.push_back("fade stop " + member_at(s.sector) + " (handle " + std::to_string(handle) + ")");
    }
}

void SoundIopHost::tick() {
    // f884: the bank handlers (e7d0) and voices, then (the host's handle
    // order) e7d0 on the streams: a VAG stream's records ticked by e8b0
    // unless it is paused (+16 & 2). The fade (volume only) and the pan glide
    // (pan only) commute, and a fade stop stops the stream, so their chain
    // order is not observable: the fade first, then the glide.
    const bool duck = engine_->tick();
    for (auto& [handle, s] : streams_) {
        if (s.ended || s.stopped || s.paused) continue;
        if (s.fade) {
            auto view = fade_view(s);
            const auto step = snd989::fade_tick(view);
            s.fade = view.fade;
            if (step) fade_result(handle, s, step->stop, step->set_volume);
        }
        if (s.stopped || !s.glide) continue;
        snd989::GlideStream view{static_cast<std::int16_t>(s.pan), s.glide};
        const auto call = snd989::glide_tick(view);
        s.glide = view.glide;
        if (call) glide_result(s, call->set_pan);
    }
    engine_errors();
    if (!duck) return;
    // 1b148: the ducks follow the groups with a playing, unpaused voice.
    std::uint32_t active = engine_->voice_groups_playing();
    for (const auto& [handle, s] : streams_)
        if (s.voice && !s.paused && mixer_.playing(*s.voice)) active |= snd989::voice_group_bit(s.group);
    const auto changed = snd989::duck_tick(volume_, active);
    if (changed) {
        for (auto& [handle, s] : streams_)
            if (changed & snd989::voice_group_bit(s.group)) apply_volume(s);
        engine_->reapply_groups(changed);
    }
}

void SoundIopHost::bank_command(std::uint32_t address, const std::vector<std::int32_t>& args) {
    bank_commands_.push_back({address, args});
}

void SoundIopHost::run_bank_commands() {
    auto commands = std::move(bank_commands_);
    bank_commands_.clear();
    constexpr auto scratch = snd989::BankEngine::scratch;
    auto& e = *engine_;
    const auto zero = [&](std::uint32_t at, std::uint32_t n) {
        std::fill(e.memory().begin() + at, e.memory().begin() + at + n, std::uint8_t(0));
    };
    const auto name = [&](std::uint32_t at, const std::vector<std::int32_t>& a, std::size_t first) {
        for (std::size_t i = 0; i < 4; ++i) e.w32(at + std::uint32_t(i) * 4, static_cast<std::uint32_t>(a.at(first + i)));
    };
    const auto text = [&](std::uint32_t at) {
        std::string s;
        for (std::uint32_t i = 0; i < 16 && e.r8(at + i); ++i) s.push_back(static_cast<char>(e.r8(at + i)));
        return s;
    };
    for (const auto& [address, a] : commands) {
        switch (address) {
        case 0x3a8000: {                                         // cmd 0x56 -> bb84(bank, name8, index, name16, out)
            zero(scratch, 0x70);
            e.w32(scratch, static_cast<std::uint32_t>(a.at(0)));
            e.w32(scratch + 4, static_cast<std::uint32_t>(a.at(2)));
            name(scratch + 0x10, a, 3);
            if (e.user_data(static_cast<std::uint32_t>(a.at(0)), scratch + 8, a.at(2), scratch + 0x10, scratch + 0x60)) {
                std::array<std::int32_t, 4> words{};
                for (std::size_t i = 0; i < 4; ++i) words[i] = e.s32(scratch + 0x60 + std::uint32_t(i) * 4);
                user_data_.push_back({static_cast<std::size_t>(a.at(7)), words});
            }
            break;
        }
        case 0x3a8070: {                                         // cmd 0x5c -> be54 on the command struct
            zero(scratch, 0x30);
            const auto bank = static_cast<std::uint32_t>(a.at(0));
            e.w32(scratch, bank ? bank : static_cast<std::uint32_t>(a.at(1)));
            e.w32(scratch + 8, static_cast<std::uint32_t>(a.at(6)));
            e.w32(scratch + 0xc, static_cast<std::uint32_t>(a.at(7)));
            name(scratch + 0x10, a, 2);
            e.w16(scratch + 0x20, static_cast<std::uint32_t>(a.at(8)));
            e.w16(scratch + 0x22, static_cast<std::uint32_t>(a.at(9)));
            e.w32(scratch + 0x28, (bank ? 1U : 0U) | 0x3cU);
            const auto sound = text(scratch + 0x10);
            const auto handle = e.play(scratch);
            const auto slot = static_cast<std::size_t>(a.at(10));
            if (handle) ++bank_started_;
            const auto it = bank_names_.find(bank);
            log_.push_back("003a8070 play " + sound + " (" + (it == bank_names_.end() ? "bank 0x" + hex(bank) : it->second)
                           + ") volume " + std::to_string(a.at(6)) + " pan " + std::to_string(a.at(7)) + " slot "
                           + std::to_string(slot) + " -> handle 0x" + hex(handle));
            replies_.push_back({static_cast<std::int32_t>(handle), slot});
            break;
        }
        case 0x3a7f00:                                           // cmd 0x21 -> c268: the handle while it lives, else 0
            replies_.push_back({static_cast<std::int32_t>(e.restart(static_cast<std::uint32_t>(a.at(0)),
                                                                    static_cast<std::uint32_t>(a.at(1)), a.at(2), a.at(3),
                                                                    a.at(4), a.at(5))),
                                static_cast<std::size_t>(a.at(6))});
            break;
        case 0x3a8040:                                           // cmd 0x15 -> c060
            e.stop(static_cast<std::uint32_t>(a.at(0)));
            break;
        case 0x3a7f50:                                           // cmd 0x61 -> e174
            e.stop_groups(static_cast<std::uint32_t>(a.at(0)));
            break;
        case 0x3a7fb0: case 0x3a7f80:                            // cmds 0x16 / 0x17 -> e0a8 / e214
            e.pause_groups(static_cast<std::uint32_t>(a.at(0)), address == 0x3a7fb0);
            break;
        case 0x3a7fe0: {                                         // cmd 0x18 -> e2e0
            e.stop_all();
            // e3ac: 1b91c(0x80e8) while the active list is not empty; the IOP's 240 Hz ticks run meanwhile.
            std::size_t ticks = 0;
            for (; !e.active_handles().empty(); ++ticks) {
                if (ticks >= 240 * 60) unhosted("0000e3ac", "e2e0's wait for the active handlers (a minute of ticks)");
                tick();
            }
            log_.push_back("003a7fe0 all sounds stopped (" + std::to_string(ticks) + " IOP ticks of e2e0's wait)");
            break;
        }
        case 0x3a7310:                                           // cmd 0x34 -> 11260: the host streams went at the call
            break;
        case 0x3a8210: {                                         // cmd 6 -> 8e30
            const auto head = static_cast<std::uint32_t>(a.at(0));
            const auto it = bank_names_.find(head);
            const auto label = it == bank_names_.end() ? "head 0x" + hex(head) : it->second;
            if (e.unload_bank(head)) {
                heads_.erase(head);                              // *0x1d984 (FreeSysMemory) on the head
                if (const auto body = bank_bodies_.find(head); body != bank_bodies_.end()) {
                    spu_->clear_overlays(body->second.first, body->second.first + body->second.second);
                    bank_bodies_.erase(body);
                }
                if (it != bank_names_.end()) bank_names_.erase(it);
                log_.push_back("003a8210 bank " + label + " unloaded (sound RAM free 0x" + hex(e.sram_free_total()) + ")");
            } else {
                log_.push_back("003a8210 bank " + label + " not unloaded");
            }
            break;
        }
        default:
            throw std::runtime_error("Game sound host: no bank command 0x" + hex(address));
        }
    }
    engine_errors();
}

std::int32_t SoundIopHost::load_bank(std::uint32_t sector, std::uint32_t byte) {
    // 93ac reads the bank file at the HOG member's byte; its SBlk head goes to
    // the IOP heap (here the engine memory from BankEngine::heads), the body
    // to the sound RAM 93ac's allocator chose (head +24).
    const auto at = std::uint64_t(sector) * sector_bytes + byte;
    const auto name = member_at(sector + byte / static_cast<std::uint32_t>(sector_bytes));
    const auto head_bytes = disc_.read({at, 24});
    const auto head_size = read_u32(head_bytes, 12), body = read_u32(head_bytes, 16), body_size = read_u32(head_bytes, 20);
    const auto total = std::max<std::uint64_t>(0x18 + std::uint64_t(head_size), std::uint64_t(body) + body_size);
    const auto aligned = (head_size + 15U) & ~15U;
    // The IOP heap (AllocSysMemory) is the host's: the first free range from BankEngine::heads.
    std::uint64_t place = snd989::BankEngine::heads;
    for (const auto& [used, used_size] : heads_) {
        if (place + aligned <= used) break;
        place = std::max<std::uint64_t>(place, std::uint64_t(used) + used_size);
    }
    if (place + aligned > snd989::BankEngine::memory_size)
        unhosted("000093ac", "a bank head past the host's IOP memory (" + name + ")");
    const auto file = disc_.read({at, total});
    const auto head = engine_->load_bank(file, static_cast<std::uint32_t>(place));
    if (head) {
        const auto spu = engine_->r32(head + 0x24);
        spu_->write_ram(spu, std::span<const std::uint8_t>(file).subspan(body, body_size));
        spu_->clear_overlays(spu, spu + body_size);
        bank_bodies_[head] = {spu, body_size};
        bank_sample_mods(sector + byte / static_cast<std::uint32_t>(sector_bytes), file, spu);
        heads_[head] = aligned;
        bank_names_[head] = name;
        log_.push_back("003a8260 bank " + name + " -> head 0x" + hex(head) + ", sound RAM 0x" + hex(spu) + " + 0x" + hex(body_size));
    } else {
        log_.push_back("003a8260 bank " + name + " not loaded (sound RAM free 0x" + hex(engine_->sram_free_total()) + ")");
    }
    engine_errors();
    return static_cast<std::int32_t>(head);
}

void SoundIopHost::engine_errors() {
    const auto& errors = engine_->errors();
    for (; engine_errors_seen_ < errors.size(); ++engine_errors_seen_)
        log_.push_back("989snd error 0x" + hex(static_cast<std::uint32_t>(errors[engine_errors_seen_])));
}

void SoundIopHost::mix(std::size_t frames, std::vector<std::int16_t>& out) {
    // 989snd's sound tick runs at 240 Hz (000076fc): 200 output frames.
    constexpr std::size_t tick_frames = audio::Mixer::output_rate / 240;
    while (frames) {
        if (tick_phase_ == 0) {
            tick();
            tick_phase_ = tick_frames;
        }
        const auto n = std::min(frames, tick_phase_);
        decode_ahead(n);
        const auto ended = mixer_.mix(n, out);
        spu_->render_add(n, out);
        for (const auto voice : ended) {
            for (auto& [handle, s] : streams_) {
                if (!s.voice || *s.voice != voice) continue;
                s.ended = true;
                s.voice.reset();
                replies_.push_back({0, s.slot});
                log_.push_back("stream end " + member_at(s.sector) + " (handle " + std::to_string(handle) + ")");
            }
        }
        frames -= n;
        tick_phase_ -= n;
    }
}

std::vector<std::string> SoundIopHost::take_log() {
    auto out = std::move(log_);
    log_.clear();
    return out;
}

}
