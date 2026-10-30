#include "game_sound.hpp"

#include "game_libm.hpp"
#include "npc_alert.hpp"
#include "vu_float.hpp"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace omega {
namespace {

constexpr std::uint32_t snd_stream_start = 0x3a7190, snd_stream_restart = 0x3a7f00, snd_stream_fade = 0x3a7ec0,
    snd_stream_stop = 0x3a8040, snd_flush = 0x3a85e0, snd_stream_flags = 0x3a7f50, snd_stream_ready = 0x3a81a0,
    snd_bank_load = 0x3a8260, snd_bank_end = 0x3a8240, snd_pause = 0x3a7fb0, snd_resume = 0x3a7f80,
    snd_bank_data = 0x3a8000, snd_bank_play = 0x3a8070;

[[noreturn]] void unhosted(const char* address, const std::string& what) {
    throw std::runtime_error(std::string("Game sound: ") + what + " (" + address + ") is not hosted");
}

int lower(unsigned char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

// 00128e80 (strcasecmp).
bool same_name(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (lower(static_cast<unsigned char>(a[i])) != lower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

std::string_view record_name(const std::array<std::uint8_t, 16>& name) {
    const auto* p = reinterpret_cast<const char*>(name.data());
    return {p, static_cast<std::size_t>(std::find(name.begin(), name.end(), std::uint8_t{0}) - name.begin())};
}

// 0036e5d0: 003ebef0(+88) zeroes +88..+90 and the +94 byte; flags 0.
void construct_resource(GameSoundResource& r) {
    r.flags_0 = 0;
    r.word_88 = r.word_8c = r.word_90 = 0;
    r.loaded_94 = 0;
}

// 0036e1b0(rec, name, hog, flag).
void name_resource(GameSoundResource& r, const std::string& name, std::int32_t hog, bool flag) {
    r.flags_0 = 0;
    r.name_4 = name.substr(0, std::min<std::size_t>(name.size(), 0x80));
    r.hog_98 = hog;
    r.flags_0 |= 2;
    if (flag) r.flags_0 |= 4;
}

// 001c9f80 for one slot.
void construct_slot(GameSoundSlot& s) {
    auto& r = s.record;
    r.key_24 = -2;                                   // H+44
    r.owner_20 = 0;                                  // H+40
    construct_resource(r.vag_48);
    construct_resource(r.lpd_e4);
    r.type_0 = -1;
    r.name_10.fill(0xff);                            // sq of pcpyld(-1, -1)
    r.owner_20 = 0;
    r.key_24 = -1;
    r.position_30 = {};
    r.slot_184 = -1;
    r.priority_180 = 0;
    r.subtitle_186 = 0;
}

}

std::string GameSoundRecord::name() const { return std::string(record_name(name_10)); }

std::string game_sound_language_suffix(std::uint8_t language) {
    static constexpr const char* table[] = {"", "E", "F", "I", "G", "S", "J"}; // 00493a40
    if (language >= 7) unhosted("003fe090", "language " + std::to_string(language) + " past the 00493a40 table");
    return table[language];
}

GameSoundManager::GameSoundManager(GameSoundHost& host) : host_(host) {
    // 001c9ea0: 003cb4b0(mgr, 004f2cc0), vtable 004dacb0, 50 x 001c9f80,
    // the HOGs 0, paused 0.
    for (auto& s : slots_) construct_slot(s);
    vag_hog_54a8 = zmedia_hog_54ac = music_hog_54b0 = 0;
    paused_20 = 0;
}

std::uint32_t GameSoundManager::valid_owner(const GameSoundRecord& r) {
    if (!r.owner_20) return 0;
    return host_.owner_serial(r.owner_20) == static_cast<std::uint32_t>(r.key_24) ? r.owner_20 : 0;
}

GameSoundRecord GameSoundManager::request(std::int32_t type, std::string_view name, std::uint32_t owner,
                                          std::int32_t priority) {
    GameSoundRecord r;
    r.key_24 = -2;
    r.owner_20 = 0;
    construct_resource(r.vag_48);
    construct_resource(r.lpd_e4);
    r.type_0 = type;
    r.owner_20 = owner;
    r.key_24 = owner ? static_cast<std::int32_t>(host_.owner_serial(owner)) : -1;
    r.slot_184 = -1;
    r.priority_180 = priority;
    r.position_30[0] = r.position_30[1] = r.position_30[2] = 0.0F;
    r.name_10.fill(0);
    // 00129360 is an unbounded strcpy into the 16 bytes at +10.
    if (name.size() >= r.name_10.size()) unhosted("001ca210", "a sound name of 16 or more characters ('" + std::string(name) + "')");
    std::memcpy(r.name_10.data(), name.data(), name.size());
    r.subtitle_186 = 0;
    return r;
}

GameSoundRecord GameSoundManager::message_copy(const GameSoundRecord& source, bool stop) {
    // 0020f2a0 / 0020faa0 (and 0020ecb0 for stop): a fresh record (003c9e30, 2 x 0036e5d0) takes the
    // type and name; a valid owner (00150c60) keeps its serial and zero position, otherwise the
    // owner is cleared, the key is -1 and the position is copied. 0020ecb0 keeps neither the
    // position nor the priority / subtitle flag.
    GameSoundRecord r;
    construct_resource(r.vag_48);
    construct_resource(r.lpd_e4);
    r.type_0 = source.type_0;
    r.name_10 = source.name_10;
    r.slot_184 = -1;
    const auto owner = valid_owner(source);
    r.owner_20 = owner;
    r.key_24 = owner ? static_cast<std::int32_t>(host_.owner_serial(owner)) : -1;
    if (stop) {
        r.position_30[0] = r.position_30[1] = r.position_30[2] = 0.0F;
        return r;
    }
    if (owner) r.position_30[0] = r.position_30[1] = r.position_30[2] = 0.0F;
    else r.position_30 = source.position_30;
    r.priority_180 = source.priority_180;
    r.subtitle_186 = source.subtitle_186;
    return r;
}

void GameSoundManager::play_quip(std::string_view name, std::uint32_t owner, std::int32_t priority) {
    auto r = request(4, name, owner, priority);
    post({GameSoundMessage::Kind::quip_20f2a0, 5, message_copy(r, false)});
}

void GameSoundManager::play_quip(std::string_view name, std::int32_t priority) {
    if (name.empty()) return;
    auto r = request(4, name, 0, priority);
    r.subtitle_186 = 1;
    if (host_.multiplayer_5721c8() && !host_.authority_5721c0()) return;
    post({GameSoundMessage::Kind::quip_20f2a0, 5, message_copy(r, false)});
}

void GameSoundHost::send_player_music(std::uint32_t, std::string_view, bool) {
    unhosted("001c6890 / 001c6a00", "the multiplayer player-targeted music route");
}

void GameSoundManager::stop_music(std::string_view name, std::uint32_t player) {
    auto r = request(5, name, 0, 0);
    if (host_.multiplayer_5721c8()) {
        if (!player) unhosted("001c6890", "multiplayer StopMusic without a player");
        if (!host_.local_player_14(player)) {
            host_.send_player_music(player, name, true);
            return;
        }
        post({GameSoundMessage::Kind::stop_20ecb0, 1, message_copy(r, true)});
    } else {
        post({GameSoundMessage::Kind::stop_20ecb0, 5, message_copy(r, true)});
    }
}

void GameSoundManager::play_music(std::string_view name, std::uint32_t player) {
    auto r = request(5, name, 0, 0);
    if (host_.multiplayer_5721c8()) {
        if (!player) unhosted("001c6a00", "multiplayer PlayMusic without a player");
        if (!host_.local_player_14(player)) {
            host_.send_player_music(player, name, false);
            return;
        }
        post({GameSoundMessage::Kind::play_20faa0, 1, message_copy(r, false)});
    } else {
        post({GameSoundMessage::Kind::play_20faa0, 5, message_copy(r, false)});
    }
}

void GameSoundManager::receive_player_music(std::string_view name, bool stop) {
    auto r = request(5, name, 0, 0);
    post({stop ? GameSoundMessage::Kind::stop_20ecb0 : GameSoundMessage::Kind::play_20faa0, 1,
          message_copy(r, stop)});
}

void GameSoundManager::play_sound(std::int8_t type, std::string_view name, std::uint32_t owner) {
    // 001c6d00: 001ca210(rec, type, name, owner, 0) wrapped by 0020f2a0 (type 4) or 0020faa0; +20 = 1 when
    // *005721c8 or *005721c0 is clear (always, solo).
    auto r = request(type, name, owner, 0);
    if (host_.multiplayer_5721c8()) unhosted("001c6d60", "the multiplayer PlaySnd flag (*005721c0)");
    post({type == 4 ? GameSoundMessage::Kind::quip_20f2a0 : GameSoundMessage::Kind::play_20faa0, 1,
          message_copy(r, false)});
}

void GameSoundManager::play_sound(std::int8_t type, std::string_view name) {
    // 001c6ea0: the +20 = 1 copy (0xbb0 / 0x9f0) is never posted; the posted one (0x510 / 0x30) keeps +20 = 5.
    auto r = request(type, name, 0, 0);
    post({type == 4 ? GameSoundMessage::Kind::quip_20f2a0 : GameSoundMessage::Kind::play_20faa0, 5,
          message_copy(r, false)});
}

void GameSoundManager::stop_sound(std::int8_t type, std::string_view name, std::uint32_t owner) {
    // 001c6c50 (StopSnd(type, name) 001c72e0) / 001c6b70 (StopSnd(type, name, GOBJ) 001c7240):
    // 001ca210(rec, type, name, owner or 0, 0) wrapped by 0020ecb0(msg, rec, 0); +20 = 1 (001c6b70 only
    // when *005721c8 or *005721c0 is clear: always, solo), posted with 003c9420 transport 0.
    auto r = request(type, name, owner, 0);
    if (owner && host_.multiplayer_5721c8()) unhosted("001c6bc0", "the multiplayer StopSnd flag (*005721c0)");
    post({GameSoundMessage::Kind::stop_20ecb0, 1, message_copy(r, true)});
}

void GameSoundManager::stop_all_quips() {
    auto r = request(4, "", 0, 0);
    post({GameSoundMessage::Kind::stop_20ecb0, 5, message_copy(r, true)});
}

namespace {
GameSoundRecord owned_record(const GameSoundRecord& d, std::uint32_t owner, std::uint32_t serial) {
    // 0020f990 / 0020eba0 after 003c9e30: the fresh record (key -2, two 0036e5d0), then type +0 and the
    // 16 name bytes (lq/sq) from the descriptor, owner +20 unvalidated, key = *(owner + c) or -1, the
    // position zero and slot -1.
    GameSoundRecord r;
    construct_resource(r.vag_48);
    construct_resource(r.lpd_e4);
    r.type_0 = d.type_0;
    r.name_10 = d.name_10;
    r.owner_20 = owner;
    r.key_24 = owner ? static_cast<std::int32_t>(serial) : -1;
    r.slot_184 = -1;
    return r;
}
} // namespace

GameSoundMessage GameSoundManager::owned_play(const GameSoundRecord& descriptor, std::uint32_t owner) {
    auto r = owned_record(descriptor, owner, owner ? host_.owner_serial(owner) : 0);
    r.priority_180 = descriptor.priority_180;   // 0020fa28 / 0020fa78
    r.subtitle_186 = descriptor.subtitle_186;   // 0020fa7c / 0020fa80
    return {GameSoundMessage::Kind::play_20faa0, 1, r};
}

GameSoundMessage GameSoundManager::positional_play(const GameSoundRecord& descriptor, const std::array<float, 4>& position) {
    auto r = owned_record(descriptor, 0, 0);    // 0020f920: +50 = 0; 0020f944: key -1
    r.position_30 = position;                   // 0020f948: lq / sq to +60
    r.priority_180 = descriptor.priority_180;   // 0020f90c / 0020f958
    r.subtitle_186 = descriptor.subtitle_186;   // 0020f95c / 0020f968
    return {GameSoundMessage::Kind::play_20faa0, 1, r};
}

GameSoundMessage GameSoundManager::owned_stop(const GameSoundRecord& descriptor, std::uint32_t owner) {
    // 0020ec30 / 0020ec88: priority 0; 0020ec34: +186 0; 0020ec38: msg +1c0 = 0.
    return {GameSoundMessage::Kind::stop_20ecb0, 1, owned_record(descriptor, owner, owner ? host_.owner_serial(owner) : 0)};
}

void GameSoundManager::pump() {
    auto snapshot = std::move(queue_);
    queue_.clear();
    for (const auto& m : snapshot) deliver(m);
}

void GameSoundManager::deliver(const GameSoundMessage& message) {
    switch (message.kind) {                                   // 001c9d00
    case GameSoundMessage::Kind::play_20faa0:
    case GameSoundMessage::Kind::quip_20f2a0: play(message.request); break;
    case GameSoundMessage::Kind::stop_20ecb0: stop(message.request); break;
    case GameSoundMessage::Kind::pause_20e860: pause(message.pause_24 != 0); break; // 001c9e5c: 001c7e00(mgr, msg +24)
    }
}

void GameSoundManager::pause_groups(bool on) {  // 001c96d0
    host_.snd(on ? snd_pause : snd_resume, {-1});
}

void GameSoundManager::set_volume(std::uint8_t channel, std::int32_t volume) {  // 001c9540
    static constexpr std::uint32_t master = 0x3a81d0;
    if (channel == 0) {
        host_.snd(master, {0, volume});
        host_.snd(master, {3, volume});
    } else if (channel == 1) {
        host_.snd(master, {1, volume});
    } else if (channel == 2) {
        host_.snd(master, {2, volume});
        host_.snd(master, {0xe, volume});
    }
}

void GameSoundManager::set_mode(std::int32_t mode) { host_.snd(0x3a80f0, {mode}); }  // 001c9670

bool GameSoundManager::quip_playing(std::string_view name) const {
    for (const auto& s : slots_)
        if (s.record.type_0 == 4 && same_name(record_name(s.record.name_10), name)) return s.record.state_44 != 0;
    return false;
}

void GameSoundManager::pause(bool on) {
    // The first music slot (+50 type 5) whose state is not 0, 1 or 3.
    GameSoundRecord* music = nullptr;
    for (auto& s : slots_) {
        const auto state = s.record.state_44;
        if (s.record.type_0 == 5 && state != 0 && state != 1 && state != 3) {
            music = &s.record;
            break;
        }
    }
    const auto event = [this](bool pause_group) {
        // 0036b180(0, 0x10, 0049f4a0, 0x591 / 0x5a2): +0 4000, +4 / +5 0,
        // vtable 004dac30, +c the flag; 003ea1d0 runs 001c80b0 when it fires.
        ++pause_events_;
        host_.schedule_3ea1d0(4000, [this, pause_group] { host_.snd(pause_group ? snd_pause : snd_resume, {2}); });
    };
    if (on && !paused_20) {
        if (music) {
            host_.snd(snd_stream_fade, {static_cast<std::int32_t>(music->handle_40), 0, 0x1f4, 2});
            event(true);
        } else {
            host_.snd(snd_pause, {2});
        }
        paused_20 = 1;
    }
    if (!on && paused_20) {
        if (music) {
            host_.snd(snd_stream_fade, {static_cast<std::int32_t>(music->handle_40), 0x400, 0x1f4, 2});
            event(false);
        } else {
            host_.snd(snd_resume, {2});
        }
        paused_20 = 0;
    }
}

void GameSoundManager::init() {
    banks_5490.fill(0);
    for (auto& s : slots_) {
        s.record.handle_40 = 0;
        s.record.state_44 = 0;
    }
    host_.snd(0x3a80f0, {static_cast<std::int32_t>(host_.output_mode_4ffd2c_14())});
    host_.snd(0x3a6d10, {1, 4});
    host_.snd(0x3a6cd0, {1, 0xb21, 0xb21});
    host_.snd(0x3a8120, {0, 2, -13, 0x8ccc, 0x8000, 0x10000, 0});
}

std::int32_t GameSoundManager::add_hog(GameSoundHog hog) {
    for (std::size_t i = 0; i < hogs_.size(); ++i)
        if (same_name(hogs_[i].path, hog.path)) return static_cast<std::int32_t>(i + 1);
    if (hog.offsets.size() != hog.names.size()) throw std::runtime_error("Game sound: HOG offsets do not match its names");
    hogs_.push_back(std::move(hog));
    return static_cast<std::int32_t>(hogs_.size());
}

bool GameSoundManager::level_load() {
    host_.snd(0x3a7370, {4, 0xe000, 1, 1});
    transition_reset = 1;
    host_.snd(0x3a7330, {1, 0, 1});
    host_.snd(0x3a7330, {2, 2, 2});
    host_.snd(0x3a7330, {0xe, 2, 2});
    const std::string root = "GAMEDATA", level = host_.level_name_4ffc90();
    const auto lang = game_sound_language_suffix(host_.language_55d470());
    const auto snd_path = root + "\\" + level + "\\SND.HOG";
    const auto vag_path = root + "\\" + level + "\\SNDVAG.HOG" + game_sound_language_suffix(host_.language_55d470());
    const auto zmedia_path = "ZMEDIA\\SNDVAG" + lang + ".HOG";
    const std::string music_path = "ZMEDIA\\MUSIC.HOG";
    const auto open = [&](const std::string& path) {
        auto h = host_.open_hog_3f96e0(path);
        h.path = path;
        return add_hog(std::move(h));
    };
    const auto snd = open(snd_path);
    if (hogs_[std::size_t(snd - 1)].loaded_14c) {
        const auto bank = [&](const char* name, std::size_t index) {
            const auto found = find(snd, name);
            if (found == -1) return;
            const auto& h = hogs_[std::size_t(snd - 1)];
            banks_5490[index] = host_.snd(snd_bank_load, {static_cast<std::int32_t>(h.sector_84),
                static_cast<std::int32_t>(static_cast<std::uint32_t>(h.data_10) + h.offsets.at(std::size_t(found)))});
        };
        bank("WEPS.BNK", 1);
        bank("ENVIRON.BNK", 0);
        bank("VOX.BNK", 2);
        bank("SHELL.BNK", 3);
        hogs_[std::size_t(snd - 1)].loaded_14c = false;            // 003f9530 closes it
        hogs_[std::size_t(snd - 1)].names.clear();
        hogs_[std::size_t(snd - 1)].offsets.clear();
        hogs_[std::size_t(snd - 1)].path.clear();
        vag_hog_54a8 = open(vag_path);
        zmedia_hog_54ac = open(zmedia_path);
        music_hog_54b0 = open(music_path);
    }
    host_.snd(snd_bank_end, {});
    while (host_.snd(snd_flush, {}) != 0) {}
    return true;
}

void GameSoundManager::level_unload() {
    host_.snd(0x3a7fe0, {});
    host_.snd(0x3a7310, {});
    host_.snd(0x3a7200, {});
    for (auto& s : slots_) {
        s.record.handle_40 = 0;
        s.record.state_44 = 0;
    }
    for (const std::size_t index : {1U, 0U, 2U, 3U})               // 5494 WEPS, 5490 ENVIRON, 5498 VOX, 549c SHELL
        if (banks_5490[index]) host_.snd(0x3a8210, {banks_5490[index]});
    while (host_.snd(snd_flush, {}) != 0) {}
    // 003f9530(004ffb8c, path): every open HOG of that name (00128e80) closed (0036cd80). The paths carry
    // no language suffix, unlike level_load's.
    const std::array<std::string, 3> paths{"GAMEDATA\\" + host_.level_name_4ffc90() + "\\SNDVAG.HOG", "ZMEDIA\\SNDVAG.HOG",
                                           "ZMEDIA\\MUSIC.HOG"};
    for (const auto& path : paths)
        for (auto& h : hogs_) {
            if (h.path.empty() || !same_name(h.path, path)) continue;
            h.loaded_14c = false;
            h.names.clear();
            h.offsets.clear();
            h.path.clear();
        }
    vag_hog_54a8 = zmedia_hog_54ac = music_hog_54b0 = 0;
}
std::int32_t GameSoundManager::find(std::int32_t hog, std::string_view name) const {
    if (hog <= 0 || std::size_t(hog) > hogs_.size()) throw std::runtime_error("Game sound: 0036d2a0 on an unknown HOG");
    const auto& h = hogs_[std::size_t(hog - 1)];
    if (!h.loaded_14c) return 0;
    for (std::size_t i = 0; i < h.names.size(); ++i)
        if (same_name(h.names[i], name)) return static_cast<std::int32_t>(i);
    return -1;
}

std::int32_t GameSoundManager::member_sector(std::int32_t hog, std::int32_t index, std::uint32_t site) const {
    const auto& h = hogs_.at(std::size_t(hog - 1));
    if (!h.loaded_14c || index < 0 || std::size_t(index) >= h.offsets.size()) {
        char where[16];
        std::snprintf(where, sizeof where, "%08x", site);
        unhosted(where, "0036d520 on an unloaded HOG or outside its offset table");
    }
    const auto byte = static_cast<std::int32_t>(static_cast<std::uint32_t>(h.data_10) + h.offsets[std::size_t(index)]);
    return static_cast<std::int32_t>(h.sector_84) + byte / 2048;
}

int GameSoundManager::allocate(const GameSoundRecord& request, std::uint8_t state) {
    int chosen = -1;
    for (int i = 0; i < int(slot_count); ++i) {
        auto& r = slots_[std::size_t(i)].record;
        if (r.type_0 == 4 && r.state_44 == 0 && r.handle_40 == 0 && r.lpd_e4.loaded_94) {
            const auto owner = valid_owner(r);
            if (owner && host_.owner_view(owner).class_49 == 2) host_.owner_clear_2dc8(owner);
            host_.release_lpd_3eaa60(std::size_t(i), r.lpd_e4);
        }
        if (r.handle_40 == 0 && r.state_44 == 0) {
            chosen = i;
            break;
        }
    }
    if (chosen < 0) return -1;
    auto& s = slots_[std::size_t(chosen)];
    auto& r = s.record;
    if (const auto owner = valid_owner(request)) {
        r.type_0 = request.type_0;
        r.name_10 = request.name_10;
        r.owner_20 = owner;                                       // 00150c60(rec + 20)
        r.key_24 = static_cast<std::int32_t>(host_.owner_serial(owner));
        r.position_30[0] = r.position_30[1] = r.position_30[2] = 0.0F;
    } else {
        r.type_0 = request.type_0;
        r.name_10 = request.name_10;
        r.owner_20 = 0;
        r.key_24 = -1;
        r.position_30 = request.position_30;
    }
    r.state_44 = state;
    r.slot_184 = -1;
    r.priority_180 = request.priority_180;
    r.subtitle_186 = request.subtitle_186;
    r.slot_184 = static_cast<std::int16_t>(chosen);
    s.near_0 = 0;
    s.far_4 = 0;
    s.loop_14 = 1;
    return chosen;
}

std::pair<std::int32_t, std::int32_t> GameSoundManager::volume_pan(std::size_t slot) {
    const auto& r = slots_.at(slot).record;
    const auto owner = valid_owner(r);
    auto p = r.position_30;
    if (!owner) {
        if (p[0] == 0.0F && p[1] == 0.0F && p[2] == 0.0F) return {0x400, -1};
    } else {
        const auto view = host_.owner_view(owner);
        if (!(view.byte_2d && view.byte_2c && !view.byte_2e)) return {0, 0};
    }
    const auto listener = host_.listener_4ffc0c();
    if (owner) p = host_.owner_view(owner).root_40;
    std::array<float, 4> d{};
    if (listener) {
        for (int i = 0; i < 4; ++i) d[std::size_t(i)] = (*listener)[std::size_t(i)] - p[std::size_t(i)];
    } else {
        for (int i = 0; i < 4; ++i) d[std::size_t(i)] = 0.0F - p[std::size_t(i)] * 1.0F; // vmulax / vmsubw from vf0
        d[3] = 0.0F * 0.0F + 1.0F * 0.0F - p[3] * 1.0F;
    }
    std::int32_t volume = 0x400;
    if (listener) {
        const float xx = d[0] * d[0], yy = d[1] * d[1], zz = d[2] * d[2];
        const float sum = (xx + yy) + zz;
        const auto distance = npc_cvt_w_s(vu::sqrt(sum));
        const auto index = r.slot_184;
        if (index < 0 || std::size_t(index) >= slot_count) unhosted("001c7b98", "a slot index outside the manager");
        const auto& h = slots_[std::size_t(index)];
        const std::int32_t near = h.near_0 ? h.near_0 : 5, far = h.far_4 ? h.far_4 : 200;
        if (near < distance) {
            volume = static_cast<std::int32_t>(static_cast<std::uint32_t>(far - distance) << 10) / far;
            if (volume < 0) volume = 0;
        }
    }
    const auto camera = host_.camera_4ffc14();
    if (camera)
        for (int i = 0; i < 4; ++i) d[std::size_t(i)] = p[std::size_t(i)] - camera->root_40[std::size_t(i)];
    float angle = game_atan2f(d[0], d[1]);
    angle = angle - (camera ? camera->heading_64 : 0.0F);
    const float scale = std::bit_cast<float>(0x42652ee0U), full = std::bit_cast<float>(0x43b40000U);
    const float degrees = angle < 0.0F ? full + scale * angle : scale * angle;
    return {volume, npc_cvt_w_s(degrees)};
}

void GameSoundManager::schedule_transition_event() {
    // 0036b180(0, 0x10): +0 2000, +4 / +5 0, vtable 004dac90, +c = 004f2cd0;
    // 004f2cd8 = 1, then 003ea1d0.
    transition_active = 1;
    ++transition_events_;
    host_.schedule_3ea1d0(2000, [this] { fire_transition(); });
}

void GameSoundManager::transition(std::int32_t type, int new_slot, bool immediate) {
    if (transition_reset) {
        transition_active = 0;
        transition_stop = -1;
        transition_pending = -1;
        transition_reset = 0;
    }
    if (transition_active && new_slot >= 0) {
        transition_pending = new_slot;
        return;
    }
    const auto byte_type = static_cast<std::int32_t>(static_cast<std::int8_t>(type));
    int pending = new_slot;
    bool fading = false;
    for (int i = 0; i < int(slot_count); ++i) {
        auto& r = slots_[std::size_t(i)].record;
        if (r.type_0 != byte_type) continue;
        if (new_slot >= 0 && same_name(record_name(r.name_10), record_name(slots_[std::size_t(new_slot)].record.name_10))) continue;
        if (r.state_44 == 0) continue;
        if (immediate) {
            if (r.type_0 == 5 && r.handle_40) {
                host_.snd(snd_stream_fade, {static_cast<std::int32_t>(r.handle_40), 0, 0x1e0, 2});
                transition_stop = i;
                transition_pending = -1;
                schedule_transition_event();
            } else {
                r.state_44 = 0;
            }
        } else if (r.handle_40) {
            host_.snd(snd_stream_fade, {static_cast<std::int32_t>(r.handle_40), 0, 0x1e0, 2});
            transition_stop = i;
            transition_pending = pending;
            schedule_transition_event();
            fading = true;
            pending = -1;
        } else {
            r.state_44 = 0;
        }
    }
    if (!fading && pending >= 0) slots_[std::size_t(pending)].record.state_44 = 2;
}

void GameSoundManager::fire_transition() {
    if (transition_stop >= 0) {
        auto& r = slots_[std::size_t(transition_stop)].record;
        if (r.state_44) r.state_44 = 0;
    }
    if (transition_pending >= 0) {
        auto& r = slots_[std::size_t(transition_pending)].record;
        if (r.state_44 && (r.type_0 == 5 || r.type_0 == 4)) r.state_44 = 2;
    }
    transition_stop = -1;
    transition_pending = -1;
    transition_active = 0;
}

void GameSoundManager::play(const GameSoundRecord& request) {
    const auto owner = valid_owner(request);
    const auto type = request.type_0;
    if (type < 0 || type >= 6) return;
    if (type == 5) {
        const auto slot = allocate(request, 1);
        if (slot >= 0) transition(5, slot, false);
        return;
    }
    if (type == 4) {
        if (vag_hog_54a8 == 0 && zmedia_hog_54ac == 0) return;
        int decision = 1;
        for (const auto& s : slots_) {
            const auto& r = s.record;
            if (r.type_0 != 4 || r.state_44 == 0) continue;
            if (request.priority_180 < r.priority_180) decision = 0;
            else if (r.priority_180 == request.priority_180) decision = -1;
            break;
        }
        if (decision == 0) return;
        if (decision < 0) {
            // 001c88e8..001c891c: 003c9c10() (*005435c0, the pending count) >= 4
            // && 003c9b50(004f53c8) (the pending messages of the quip key) >= 4
            // -> 003c99b0(004f53c8): the first pending quip message is
            // destroyed (virtual +8) and unlinked (003c9fe0). The pump 003c8de0
            // moves its snapshot to the delivery list 005435e0 first, so only
            // messages posted since count; a quip count of 4 implies the
            // pending count (checkpoint 132zf7).
            const auto quip = [](const GameSoundMessage& m) { return m.kind == GameSoundMessage::Kind::quip_20f2a0; };
            if (std::count_if(queue_.begin(), queue_.end(), quip) >= 4) queue_.erase(std::find_if(queue_.begin(), queue_.end(), quip));
            post({GameSoundMessage::Kind::quip_20f2a0, 1, message_copy(request, false)});
            return;
        }
        const auto name = request.name();
        const auto vag_name = name + ".VAG", lpd_name = name + ".LPD";
        GameSoundResource vag, lpd;
        construct_resource(lpd);
        construct_resource(vag);
        name_resource(vag, vag_name, vag_hog_54a8, true);
        bool found = false;
        if (vag_hog_54a8 && find(vag.hog_98, vag_name) != -1) found = true;
        else if (zmedia_hog_54ac) {
            name_resource(vag, vag_name, zmedia_hog_54ac, true);
            found = find(vag.hog_98, vag_name) != -1;
        }
        if (!found) return;
        transition(4, -1, false);
        const auto slot = allocate(request, 1);
        if (slot < 0) return;
        auto& r = slots_[std::size_t(slot)].record;
        r.vag_48 = vag;
        r.lpd_e4 = lpd;                                           // 0036e5d0 only: flags 0, name / hog not set
        bool lip = false;
        if (owner && host_.owner_view(owner).class_49 == 2 && find(vag.hog_98, lpd_name) != -1) {
            name_resource(lpd, lpd_name, vag.hog_98, true);
            r.lpd_e4 = lpd;
            lip = true;
        }
        if (lip) {
            host_.load_lpd_3eae60(std::size_t(slot), r.lpd_e4);
            r.state_44 = 3;
        } else {
            r.state_44 = 2;
        }
        return;
    }
    const auto mode = host_.game_mode_4ffba0();
    if (mode == 0xf || type == 3 || mode == 2) {
        // 001c8c98: the bank branch. The name is the request's +10 quad (lq, paddub into a3 / a2); the
        // out address is the allocated slot's H+0 (by its +184), the record is the callback data.
        const auto slot = allocate(request, 1);
        if (slot < 0) return;
        const auto index = slots_[std::size_t(slot)].record.slot_184;
        std::array<std::int32_t, 4> name{};
        std::memcpy(name.data(), request.name_10.data(), 16);
        const auto bank = banks_5490[std::size_t(type)];
        host_.snd(snd_bank_data, {bank, 0, -1, name[0], name[1], name[2], name[3], index});
        const auto [volume, pan] = volume_pan(std::size_t(slot));
        host_.snd(snd_bank_play, {bank, 0, name[0], name[1], name[2], name[3], volume, pan, 0, 0, slot});
        return;
    }
    post({GameSoundMessage::Kind::play_20faa0, 1, message_copy(request, false)});
}

void GameSoundManager::bank_user_data(std::size_t slot, const std::array<std::int32_t, 4>& words) {
    auto& s = slots_.at(slot);
    s.near_0 = words[0];
    s.far_4 = words[1];
    s.word_8 = words[2];
    s.word_c = words[3];
}

void GameSoundManager::stop(const GameSoundRecord& request) {
    if (request.type_0 == 5) {
        transition(request.type_0, -1, false);
        return;
    }
    if (request.type_0 == 4) {
        transition(request.type_0, -1, true);
        return;
    }
    const auto request_owner = valid_owner(request);
    int mode = request_owner ? 1 : 0;
    bool matched = false;
    for (;;) {
        int hit = -1;
        for (int i = 0; i < int(slot_count); ++i) {
            auto& r = slots_[std::size_t(i)].record;
            if (r.state_44 == 0) continue;
            if (r.type_0 != request.type_0 || !same_name(record_name(request.name_10), record_name(r.name_10))) continue;
            const auto slot_owner = valid_owner(r);
            matched = true;
            if (mode != 0 && slot_owner && slot_owner == request_owner) { hit = i; break; }
            if (mode == 0 && !slot_owner) { hit = i; break; }
            if (mode == -1) { hit = i; break; }
        }
        if (!matched) return;
        if (hit >= 0) {
            slots_[std::size_t(hit)].record.state_44 = 0;
            return;
        }
        if (mode == -1) unhosted("001c8320", "StopSound's endless loop after 'Unable to find match in StopSound for " + request.name() + "'");
        mode = mode == 0 ? -1 : 0;
    }
}

void GameSoundManager::frame() {
    for (std::size_t i = 0; i < slot_count; ++i) {
        auto& s = slots_[i];
        auto& r = s.record;
        const auto slot_arg = static_cast<std::int32_t>(i);
        if (r.type_0 == 5) {
            if (!(r.state_44 == 2 || r.state_44 == 4) || music_hog_54b0 == 0 || paused_20) continue;
            std::string file;
            if (same_name(record_name(r.name_10), "INTRO")) {
                const auto level = host_.level_name_4ffc90();
                file = level + "_INTRO.VPK";
                if (find(music_hog_54b0, file) == -1) {
                    if (level.size() >= 16) unhosted("001c8ee4", "a level name of 16 or more characters in the INTRO retry");
                    auto cut = level;
                    if (!cut.empty()) cut.pop_back();
                    file = cut + "_INTRO.VPK";
                }
            } else {
                file = r.name() + ".VPK";
            }
            if (find(music_hog_54b0, file) == -1) continue;
            host_.snd(snd_stream_flags, {2});
            const auto sector = member_sector(music_hog_54b0, find(music_hog_54b0, file), 0x1c8f6c);
            host_.snd(snd_stream_start, {sector, 0, 0, 0, 0x400, -1, 1, 0, 0, s.loop_14 ? 4 : 0, slot_arg});
            r.state_44 = 1;
            continue;
        }
        if (r.state_44 == 0 || r.state_44 == 1 || r.state_44 == 3) continue;
        const auto owner = valid_owner(r);
        const auto [volume, pan] = volume_pan(i);
        bool dead = false;
        if (r.type_0 == 4 && (r.state_44 == 2 || r.state_44 == 4)) {
            if (owner && host_.owner_state_108(owner) == 3) dead = true;
            if (!dead) {
                host_.snd(snd_stream_flags, {4});
                host_.snd(snd_stream_flags, {0x4000});
                const auto hog = r.vag_48.hog_98;
                // A slot whose VAG was never loaded has res +98 = 0: 0036d2a0 would read the HOG fields at EE 0x14c.
                if (hog == 0) unhosted("001c9118", "0036d2a0 / 0036d520 on the null HOG of an unloaded VAG resource");
                const auto sector = member_sector(hog, find(hog, r.vag_48.name_4), 0x1c9118);
                if (host_.snd(snd_stream_ready, {2}) != 0)
                    host_.snd(snd_stream_start, {sector, 0, 0, 0, volume, pan, owner ? 0xe : 2, 0, 0, 0, slot_arg});
                if (r.subtitle_186 && host_.hud_4ffc2c()) host_.subtitle_245e00(r.name_10);
                if (r.state_44 == 4 && owner && r.lpd_e4.loaded_94 && host_.owner_view(owner).class_49 == 2)
                    host_.lip_sync_391810(owner, r.lpd_e4);
                r.state_44 = 1;
            }
        }
        if (r.type_0 == 4 && r.state_44 == 6 && owner && host_.owner_state_108(owner) == 3) r.state_44 = 0;
        if (r.state_44 == 6 && r.handle_40) {
            if (r.type_0 == 4 && !r.lpd_e4.loaded_94) {
                const auto o = valid_owner(r);
                if (o && host_.owner_view(o).class_49 == 2) host_.owner_clear_2dc8(o);
            }
            host_.snd(snd_stream_restart, {static_cast<std::int32_t>(r.handle_40), 5, volume, pan, 0, 0, slot_arg});
            r.state_44 = 1;
        }
    }
    for (auto& s : slots_) {
        auto& r = s.record;
        if (r.state_44 == 0 && r.handle_40) {
            host_.snd(snd_stream_stop, {static_cast<std::int32_t>(r.handle_40)});
            r.handle_40 = 0;
        }
    }
    host_.snd(snd_flush, {});
}

void GameSoundManager::stop_music_streams() {  // 001c7d80
    for (auto& s : slots_) {
        auto& r = s.record;
        if (r.type_0 != 5) continue;  // 001c7da0 lw +50, bne 5
        if (r.handle_40) {
            host_.snd(snd_stream_stop, {static_cast<std::int32_t>(r.handle_40)});
            r.handle_40 = 0;
        }
        r.state_44 = 0;  // 001c7dc8 sb +94
    }
    host_.snd(snd_flush, {});  // 001c7de0
}
void GameSoundManager::stream_result(std::int32_t result, std::size_t slot) {
    auto& r = slots_.at(slot).record;
    if (result == 0) {
        r.state_44 = (r.type_0 == 5 && (r.state_44 == 1 || r.state_44 == 3)) ? 2 : 0;
        return;
    }
    if (r.state_44 == 1 || r.state_44 == 3) r.state_44 = 6;
    r.handle_40 = static_cast<std::uint32_t>(result);
}

void GameSoundManager::lpd_loaded(std::size_t slot) {
    auto& r = slots_.at(slot).record;
    if (r.state_44 == 3 && r.lpd_e4.loaded_94) r.state_44 = 4;
}

}
