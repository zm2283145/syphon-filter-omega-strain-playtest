#include "sound_gobj.hpp"

#include <stdexcept>

namespace omega {

SoundGobjWorld::SoundGobjWorld(const std::vector<PopulationSoundObject>& records, SoundGobjHost host)
    : records_(records), states_(records.size()), host_(std::move(host)) {
    if (!host_.play || !host_.stop || !host_.local_player)
        throw std::runtime_error("SoundGOBJ: the sound manager 004ffb50 is not hosted (00212ee0's virtual +60(1) sends to it)");
    for (std::size_t i = 0; i < records_.size(); ++i) {
        const auto& r = records_[i];
        // 0040c9d0 registers the object under +c; a second record of a key would shadow the first.
        if (!index_.emplace(r.id, i).second) throw std::runtime_error("SoundGOBJ: duplicate key " + std::to_string(r.id) + " (0040c9d0)");
        // 00213034..002130d8: every record gets the +18 script holder {0, 0, -1}; 003d9d30(holder, name) binds a
        // non-empty name (001294f8) to a script (003e15b0 / 003d9da0), which is not hosted.
        if (!r.script.empty())
            throw std::runtime_error("SoundGOBJ " + std::to_string(r.id) + ": the script '" + r.script + "' (003d9d30) is not hosted");
        auto& s = states_[i];
        s.autostart_1f1 = r.autostart() ? 1 : 0;   // 001c3490..001c3498: (flag ^ 1) != 0, stored at 00213468
        s.owned_1f2 = r.owned_1f2;
        deactivate(r.id);                           // 00213504..00213514: virtual +60(1)
        s.playing_1f0 = 0;                          // 00213588
    }
}

std::size_t SoundGobjWorld::at(std::uint32_t key, const char* what) const {
    const auto found = index_.find(key);
    if (found == index_.end()) throw std::runtime_error(std::string("SoundGOBJ: ") + what + " on " + std::to_string(key) + ", not an SND: record");
    return found->second;
}

const SoundGobjState& SoundGobjWorld::state(std::uint32_t key) const { return states_[at(key, "the state")]; }
const PopulationSoundObject& SoundGobjWorld::record(std::uint32_t key) const { return records_[at(key, "the record")]; }

void SoundGobjWorld::play(std::size_t i) {
    auto& s = states_[i];
    // 0021094c..00210960: the owner is the object when +1f2, else none; nothing while +1f0.
    if (s.playing_1f0) return;
    s.playing_1f0 = 1;
    // 0020f990(msg, +60, owner); the sb 1, msg+20 behind `bnez 001c6700` never runs (+20 stays 003c9e30's 5);
    // 003c9420(msg, 004ffb50, 1).
    host_.play(records_[i], s.owned_1f2 ? records_[i].id : 0U);
}

void SoundGobjWorld::stop(std::size_t i) {
    auto& s = states_[i];
    // 002108a0..002108d0: +1f2 -> the object; else *004ffc0c ? none : the object. 0020eba0, 003c9420(msg, 004ffb50, 1).
    const std::uint32_t owner = s.owned_1f2 ? records_[i].id : (host_.local_player() ? 0U : records_[i].id);
    host_.stop(records_[i], owner);
    s.playing_1f0 = 0;                              // 00210928
}

void SoundGobjWorld::activate(std::uint32_t key) {
    const auto i = at(key, "virtual +5c (00212bb0)");
    if (states_[i].autostart_1f1) play(i);
    // 003ce980: +2d = 1; the +58 controller is null (no virtual +48).
    states_[i].present_2d = 1;
}

void SoundGobjWorld::deactivate(std::uint32_t key) {
    const auto i = at(key, "virtual +60 (00212ae0)");
    if (states_[i].autostart_1f1) stop(i);
    // 003ce790: +2d = 0; the +58 controller is null (no virtual +4c).
    states_[i].present_2d = 0;
}

void SoundGobjWorld::trigger(std::uint32_t key) { play(at(key, "Trigger (00210940)")); }

void SoundGobjWorld::kill(std::uint32_t key) { stop(at(key, "KillSnd (00210890)")); }

void SoundGobjWorld::set_enabled(std::uint32_t key, bool enable) {
    states_[at(key, enable ? "Enable (0015bf80)" : "Disable (0015bf90)")].enabled_2c = enable ? 1 : 0;
}

bool SoundGobjWorld::message(std::uint32_t key, Message message) {
    const auto i = at(key, "the message handler 00212c80");
    auto& s = states_[i];
    switch (message) {
    case Message::trigger_4f5440:                   // 00212cb4..00212d44 (no +1f1 check)
        play(i);
        return true;
    case Message::enable_55d4b8:                    // 00212d64..00212dfc
        if (s.owned_1f2) break;
        play(i);                                    // owner none (+1f2 is 0)
        return true;
    case Message::disable_55d498:                   // 00212e1c..00212eb0
        if (s.owned_1f2) break;
        stop(i);
        return true;
    case Message::other:
        throw std::runtime_error("SoundGOBJ " + std::to_string(key) + ": the default receiver 003cea10 is not hosted");
    }
    // An owning object (+1f2) passes Enable / Disable to the default receiver 003cea10 (neither 55d4d8 nor
    // 55d4e0) -> 003cedd0, whose test is the presence +2d && +2c && !+2e (+2e: cGOBJ_ctor 003cf0e0's 0).
    const bool present = s.present_2d && s.enabled_2c;
    if (message == Message::enable_55d4b8) {
        if (!present) activate(key);                // 003cedec..003cee48: virtual +5c(1) = 00212bb0
        s.enabled_2c = 1;                           // 003cee58
    } else {
        if (present) deactivate(key);               // 003cee7c..003ceed8: virtual +60(1) = 00212ae0
        s.enabled_2c = 0;                           // 003ceee0
    }
    return true;
}

}  // namespace omega
