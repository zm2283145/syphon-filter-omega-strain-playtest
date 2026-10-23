#pragma once

// The play host below gameSound.cc's request layer (checkpoint 119i): the
// 989snd EE library calls (003a6cd0..003a85e0) and the IOP behind them are
// not ported; this host answers them from the disc. The HOG objects of
// 003f96e0 come from the ISO, a stream start reads the member at its sector
// and decodes it (audio_formats.hpp), the stream replies 001c9430 are
// delivered at the next 003a85e0 and the decoded PCM is mixed by the host
// mixer (audio_output.hpp). Checkpoint 119j: the stream volume, pan, fade
// (cmd 0x22, ticked by e7d0 -> e8b0), pause / resume (cmds 0x16 / 0x17),
// group stop (IRX cmd 0x61) and the ducker (cmd 0x60, 1b148 on every 4th
// 240 Hz tick) follow the 989NOMID.IRX disassembly (snd_volume.hpp). Every
// remaining interpretation of a 989snd call is a host assumption, listed in
// research/GENERATOR_RUNTIME.md (checkpoints 119i and 119j).
// Checkpoint 119l: bank sounds run on the ported 989snd bank player
// (snd989::BankEngine over the host SPU2 model audio::Spu2Voices): the
// init's reverb work area (3a6d10 -> d0e4), the stream buffers' sound RAM
// (3a7370 -> ed70), the bank loads (3a8260 -> 93ac with the allocator, the
// body written to sound RAM), the user data (3a8000 -> bb84), play (3a8070
// -> be54), restart / stop / group stop on bank handles (c268 / c060 /
// e174) and the sound tick (f884) with the duck over bank and stream
// voices. As the EE library queues its commands until 003a85e0, the bank
// commands run there, in order, before the replies.
// Checkpoint 126c: the level unload 001c9710's calls: stop everything (3a7fe0,
// cmd 0x18 -> e2e0 and its wait), the stream channels (3a7310, cmd 0x34),
// snd_CloseStreams (3a7200, cmd 0x35 -> 173d4 / 17220: the buffers' sound RAM
// freed) and the bank unloads (3a8210, cmd 6 -> 8e30); 3a7370 keeps the EE's
// *0053dc00 guard. The bank heads take the first free host range, so an
// unloaded head's memory is reused.
// The owner, listener and camera services are hooks the play world sets;
// unset, they throw naming their address.
// Checkpoint 119o: a quip's LPD runs on fstream.cc's loader (the ported
// 003eae60 / 003eaa60 / 003eb340, fstream_loader.hpp) with this host below
// it: the member's size from the HOG table (0036d380), the block in host
// memory, the read from the disc (0036d950, complete at the first poll
// 0036d8e0) and the callback 001c9400 (GameSoundManager::lpd_loaded). The
// lip sync 00391810, the owner's +2dc8 and its life state (virtual +108)
// are hooks; unset, they throw naming their address.

#include "audio_files.hpp"
#include "audio_output.hpp"
#include "disc.hpp"
#include "fstream_loader.hpp"
#include "game_sound.hpp"
#include "snd_bank.hpp"
#include "snd_volume.hpp"
#include "spu2_voices.hpp"

#include <atomic>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace omega {

struct SoundIopHostConfig {
    std::string level;              // 004ffc90
    std::uint8_t language = 0;      // 0055d470
    std::uint8_t game_mode = 0xf;   // *004ffba0 (0xf: play)
    std::uint32_t output_mode = 0;  // *(*004ffd2c + 14): 0045a7a0's default 0
};

class SoundIopHost final : public GameSoundHost {
public:
    SoundIopHost(DiscImage& disc, SoundIopHostConfig config, audio::Mixer& mixer);
    ~SoundIopHost() override;
    void attach(GameSoundManager& manager) { manager_ = &manager; }
    // 004ffc90, which the front end swaps to "FRONTEND" around its sound calls (002cb3d0 / 002c9510 / 002c9860).
    void set_level(std::string level) { config_.level = std::move(level); }
    bool streams_open() const { return streams_open_; }
    // The host's bank heads in the IOP memory: head -> its 16-byte aligned size.
    const std::map<std::uint32_t, std::uint32_t>& bank_heads() const { return heads_; }

    // 003ea1d0 (the script callback list), *004ffc2c and 00245e00.
    std::function<void(std::int32_t, std::function<void()>)> schedule;
    std::function<bool()> hud;
    std::function<void(const std::string&)> subtitle;
    // 00150c60 (an owner's serial *(owner + c), 0 when it is gone), the
    // owner view 001c75c0 reads (+2c..+2e, +49, the +50 root's +40) and the
    // listener 004ffc0c / camera 004ffc14 of volume_pan 001c78e0.
    std::function<std::uint32_t(std::uint32_t)> owner_serial_hook;
    std::function<GameSoundOwnerView(std::uint32_t)> owner_view_hook;
    std::function<std::optional<std::array<float, 4>>()> listener_hook;
    std::function<std::optional<GameSoundCamera>()> camera_hook;
    // The owner's life state (virtual +108: 001872f0 for actors), 00391810(owner + 60, the LPD bytes, 0)
    // and the clear of owner + 60 + 2d68 (+2dc8).
    std::function<std::uint8_t(std::uint32_t)> owner_state_hook;
    std::function<void(std::uint32_t, const std::string&, std::span<const std::uint8_t>)> lip_sync_hook;
    std::function<void(std::uint32_t)> owner_clear_hook;

    // 003eb340(003eba50()) once per game frame (00167dd0), after 00165610's frame count 0055a818.
    std::int32_t lpd_service();
    const FstreamLoader& lpd_loader() const { return lpd_loader_; }
    std::size_t lpd_reads() const { return lpd_reads_; }

    // Mixes `frames` output frames into `out`; the streams that end queue
    // their 001c9430(0, slot) reply for the next 003a85e0.
    void mix(std::size_t frames, std::vector<std::int16_t>& out);
    // The calls and stream events since the last take, for the run log.
    std::vector<std::string> take_log();
    std::size_t streams_started() const { return started_; }
    // A stream's IRX +10 volume and +16 bit 2 (nullopt for an unknown handle).
    std::optional<std::int32_t> stream_volume_10(std::uint32_t handle) const;
    std::optional<bool> stream_paused(std::uint32_t handle) const;
    // The 989snd volume state (masters 0x1e41c, ducks 0x1e49c, duckers 0x25ddc).
    const snd989::VolumeState& volume_state() const { return volume_; }
    // The disc member a stream start's sector names (or "sector N").
    std::string member_at(std::uint32_t sector) const;
    // The bank player and the SPU2 voices it drives.
    const snd989::BankEngine& bank_engine() const { return *engine_; }
    const audio::Spu2Voices& spu() const { return *spu_; }
    std::size_t bank_sounds_started() const { return bank_started_; }

    std::uint32_t owner_serial(std::uint32_t owner) override;
    GameSoundOwnerView owner_view(std::uint32_t owner) override;
    std::uint8_t owner_state_108(std::uint32_t owner) override;
    void owner_clear_2dc8(std::uint32_t owner) override;
    void lip_sync_391810(std::uint32_t owner, const GameSoundResource& lpd) override;
    std::optional<std::array<float, 4>> listener_4ffc0c() override;
    std::optional<GameSoundCamera> camera_4ffc14() override;
    bool hud_4ffc2c() override;
    void subtitle_245e00(const std::array<std::uint8_t, 16>& name) override;
    std::string level_name_4ffc90() override { return config_.level; }
    std::uint8_t language_55d470() override { return config_.language; }
    std::uint8_t game_mode_4ffba0() override { return config_.game_mode; }
    bool multiplayer_5721c8() override { return false; }
    std::uint32_t output_mode_4ffd2c_14() override { return config_.output_mode; }
    GameSoundHog open_hog_3f96e0(const std::string& path) override;
    void load_lpd_3eae60(std::size_t slot, const GameSoundResource& lpd) override;
    void release_lpd_3eaa60(std::size_t slot, const GameSoundResource& lpd) override;
    void schedule_3ea1d0(std::int32_t delay, std::function<void()> event) override;
    std::int32_t snd(std::uint32_t address, const std::vector<std::int32_t>& args) override;

private:
    struct Stream {
        std::size_t slot{};
        std::uint32_t sector{};
        std::shared_ptr<const audio::PcmClip> clip;
        std::optional<audio::Mixer::Voice> voice;
        bool ended{};
        bool stopped{};                                // c060 by group (003a7f50): the handle stays until 003a8040
        bool paused{};                                 // +16 bit 2 (11468 / 116bc)
        std::int8_t group{};                           // IRX stream +17 (003a7190 argument 6), the voices' +1b
        std::int32_t volume{};                         // +10
        std::int32_t pan{};                            // +12 (0 for a start pan of -1 / -2: 0x108d8)
        std::optional<snd989::FadeRecord> fade;        // the +8 list's volume fade record (3b1c)
        std::optional<snd989::FadeRecord> glide;       // the +8 list's pan glide record (2bbc, flags & 4)
        std::vector<std::array<std::int16_t, 2>> out;  // the voices' previous {left, right} (+20)
    };
    // 179e4's per-channel volume: 1a884 -> 1ae88 -> >> 1 into the SPU voice volume.
    void apply_volume(Stream& s);
    // A fade's c1d4(handle, v, -2) (179e4: 17768 / 177ac) or its c060.
    void fade_result(std::uint32_t handle, Stream& s, bool stop, std::optional<std::int32_t> volume);
    // A pan glide's c1d4(handle, 0x7fffffff, pan) (2bbc / 2ef0).
    void glide_result(Stream& s, std::optional<std::int32_t> pan);
    // c060 on a stream (e3e4(stream, 1, 0, 0)): the voices stop, no reply.
    void stop_stream(Stream& s);
    // The fade record view of a host stream: a VAG stream (type 4, +c = 127).
    static snd989::FadeStream fade_view(const Stream& s);
    // One 240 Hz sound tick (000076fc -> fb20): e7d0 (the fade ticks) and,
    // on every 4th tick, the duck 1b148.
    void tick();
    // The stream handles of 003a7190 / 003a7f00 / 003a8040 (IRX handlers 0c28 / 09cc / 07a8).
    Stream& stream(std::uint32_t handle, const char* address);
    // A bank command of 003a8000 / 003a8070 / 003a7f00 / 003a8040 run at the next 003a85e0.
    void bank_command(std::uint32_t address, const std::vector<std::int32_t>& args);
    void run_bank_commands();
    // 3a8260 -> IRX 93ac: the bank file at the byte, its head at the first free host range from BankEngine::heads.
    std::int32_t load_bank(std::uint32_t sector, std::uint32_t byte);
    void engine_errors();
    snd989::VolumeState volume_;
    std::size_t tick_phase_ = 0;     // output frames until the next 240 Hz tick
    std::vector<std::uint8_t> irx_;
    std::unique_ptr<audio::Spu2Voices> spu_;
    std::unique_ptr<snd989::BankEngine> engine_;
    std::size_t engine_errors_seen_ = 0;
    std::map<std::uint32_t, std::uint32_t> heads_;             // the IOP heap's bank heads: address -> aligned size
    bool streams_open_ = false;      // *0053dc00: 3a7370's buffers (0x1e294) allocated
    std::uint32_t stream_ram_ = 0, stream_ram_size_ = 0;       // their sound RAM (ed70), freed by 17220
    std::map<std::uint32_t, std::string> bank_names_;          // head -> HOG member
    std::vector<std::pair<std::uint32_t, std::vector<std::int32_t>>> bank_commands_;
    std::vector<std::pair<std::size_t, std::array<std::int32_t, 4>>> user_data_;
    std::size_t bank_started_ = 0;
    DiscImage& disc_;
    SoundIopHostConfig config_;
    audio::Mixer& mixer_;
    GameSoundManager* manager_{};
    std::map<std::uint32_t, Stream> streams_;                 // by 989snd handle
    std::map<std::uint32_t, std::shared_ptr<const audio::PcmClip>> clips_; // by sector
    // Port-only (checkpoint 132win5): the VPK clips still decoding, a chunk ahead of the voices that read them.
    std::vector<std::unique_ptr<audio::VpkProgressive>> decoding_;
    void decode_ahead(std::size_t frames);
    // Port-only (132vt2b2, PS Vita, research/VITA.md "Memory"): the clips no stream or voice plays any more leave the
    // cache (a VPK track's whole decoded PCM, ~30 MB for the front end's THEME_MAIN.VPK, held into the level before).
    void drop_unheard_clips();
    std::map<std::uint32_t, std::string> members_;            // sector -> HOG member
    // Port-only (checkpoint 132md4, research/MODDING.md "Sounds and music"): sector -> the member's asset key, and the
    // mods' replacement clips by (sector, looping) (nullptr: refused, the disc's is played), and those still decoding
    // (a piece ahead of the voices that read them, as decoding_; the key for the log of a failure part way).
    std::map<std::uint32_t, std::string> member_keys_;
    std::map<std::pair<std::uint32_t, bool>, std::shared_ptr<const audio::PcmClip>> replaced_;
    std::vector<std::pair<std::string, std::unique_ptr<mods::FittedAudio>>> decoding_mods_;
    std::shared_ptr<const audio::PcmClip> replacement_clip(std::uint32_t sector, bool loop);
    // Port-only (checkpoint 132md13, research/MODDING.md "Audio overrides"): the mods' stream replacements of a HOG
    // decoded whole when it opens, on a worker per HOG (in order, within preload_budget bytes of fitted samples): by
    // member key, the rate and channels fitted to and the clip (nullptr: refused or past the budget; the clip is then
    // decoded as it plays, as before). replacement_clip takes a clip that is ready; its samples are the progressive
    // decode's. The workers stop at the next piece when the host goes.
    struct Preload {
        std::uint32_t rate = 0, channels = 0;
        std::shared_future<std::shared_ptr<audio::PcmClip>> clip;
    };
    std::map<std::string, Preload> preloads_;
    std::vector<std::future<void>> preload_workers_;
    std::shared_ptr<std::atomic<bool>> preload_stop_ = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<std::size_t>> preload_used_ = std::make_shared<std::atomic<std::size_t>>(0);
    void preload_mod_streams(const std::string& path, const GameSoundHog& hog);
    // Port-only (checkpoint 132md5, research/MODDING.md "Sound effects in the banks"): a mod's sounds\<bank>\<n>.wav /
    // .ogg for the bank samples of the bank loaded from the member at `sector` with its body at sound RAM `spu`,
    // registered as SPU overlays (Spu2Voices::set_overlay); the encoded replacements by sample key (nullptr: refused,
    // noted once), and each loaded bank's body {sound RAM address, size} by head, whose overlays go when it unloads.
    void bank_sample_mods(std::uint32_t sector, std::span<const std::uint8_t> file, std::uint32_t spu);
    std::map<std::string, std::shared_ptr<const std::vector<std::uint8_t>>> bank_overlays_;
    std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> bank_bodies_;
    std::set<std::string> bank_checked_;   // banks whose sounds\ entries were checked against their samples
    std::vector<std::pair<std::int32_t, std::size_t>> replies_;
    std::vector<std::string> log_;
    std::uint32_t next_handle_ = 1;
    std::size_t started_ = 0;
    std::shared_ptr<const audio::PcmClip> clip(std::uint32_t sector, bool loop);

    // The loader's services (FstreamHost) over this host's HOGs and the disc.
    class LpdHost final : public FstreamHost {
    public:
        explicit LpdHost(SoundIopHost& owner) : owner_(owner) {}
        void member_index_36def0(GameSoundResource& resource) override;
        std::uint32_t size_36de50(GameSoundResource& resource) override;
        std::uint32_t alloc_36b180(std::uint32_t size) override;
        void free_36b130(std::uint32_t address) override;
        void free_request_36b130(std::uint32_t) override {}
        void read_36d950(GameSoundResource& resource, std::uint32_t dest, std::uint32_t size) override;
        void read_36d990(GameSoundResource& resource, std::uint32_t dest, std::uint32_t size) override;
        std::int32_t poll_36d8e0(GameSoundResource& resource) override;
        void callback(std::uint32_t function, GameSoundResource& resource, std::uint32_t arg, std::uint32_t loaded) override;
        std::uint32_t frame_55a818() override { return owner_.lpd_frame_; }
        std::int32_t heap_534cdc() override { return 1; }
    private:
        SoundIopHost& owner_;
    };
    struct LpdBlock {
        std::vector<std::uint8_t> bytes;  // size + 0x7f (003ebdd0)
        std::uint32_t member_size{};      // 0036de50 when read
    };
    // The HOG member of a resource (+98, the name +4): the HOG and the index (0036d2a0).
    std::pair<const GameSoundHog*, std::size_t> lpd_member(const GameSoundResource& resource, const char* address) const;
    LpdBlock& lpd_block(std::uint32_t address, const char* site);
    void lpd_read(GameSoundResource& resource, std::uint32_t dest, std::uint32_t size, const char* address);
    std::map<std::string, std::vector<std::uint32_t>> hog_offsets_;  // by path: the count + 1 offsets of +160
    std::map<std::uint32_t, LpdBlock> lpd_blocks_;                     // by host address
    std::uint32_t next_lpd_address_ = 0x10000000;
    std::uint32_t lpd_frame_ = 0;
    std::size_t lpd_reads_ = 0;
    LpdHost lpd_host_{*this};
    FstreamLoader lpd_loader_{lpd_host_};
};

}
