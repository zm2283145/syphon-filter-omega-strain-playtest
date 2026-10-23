// The SND: sound objects (SoundGOBJ, class 0xb, vtable 004daf30): the
// constructor 00212ee0's deactivation, activate 00212bb0 (vt+5c), deactivate
// 00212ae0 (vt+60), the natives Trigger 00210940 / KillSnd 00210890, the
// message handler 00212c80 and Enable / Disable 0015bf80 / 0015bf90 (vt+64 /
// +68: +2c only). The sounds are the descriptor +60 (001ca010(+60, type, name,
// obj, 0)) sent to the sound manager 004ffb50 through the host.
#pragma once

#include "population.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace omega {

struct SoundGobjHost {
    // 0020f990(msg, +60, owner) / 0020eba0(msg, +60, owner), then 003c9420(msg, 004ffb50, 1). `owner` is the
    // object's key (+c) when it owns the sound, else 0; the descriptor's own owner is always the object.
    std::function<void(const PopulationSoundObject& record, std::uint32_t owner)> play;
    std::function<void(const PopulationSoundObject& record, std::uint32_t owner)> stop;
    // *004ffc0c != 0 (the local player), read by the stop paths of the objects that do not own their sounds.
    std::function<bool()> local_player;
};

struct SoundGobjState {
    std::uint8_t playing_1f0{};
    std::uint8_t autostart_1f1{};
    std::uint8_t owned_1f2{};
    std::uint8_t enabled_2c{1};      // cGOBJ_ctor's 1; Enable / Disable (0015bf80 / 0015bf90)
    std::uint8_t present_2d{};       // vt+5c / vt+60 (003ce980 / 003ce790)
};

class SoundGobjWorld {
public:
    // The messages 00212c80 tells apart (003c9c20 against the type words).
    enum class Message { trigger_4f5440, enable_55d4b8, disable_55d498, other };

    // 00212ee0 per record in population order: a script name throws (003d9d30 is not hosted); virtual +60(1)
    // (the deactivation below: an autostart object sends its stop, +2d = 0); +1f0 = 0.
    SoundGobjWorld(const std::vector<PopulationSoundObject>& records, SoundGobjHost host);

    bool contains(std::uint32_t key) const { return index_.count(key) != 0; }
    const SoundGobjState& state(std::uint32_t key) const;
    const PopulationSoundObject& record(std::uint32_t key) const;
    const std::vector<PopulationSoundObject>& records() const { return records_; }

    void activate(std::uint32_t key);    // 00212bb0: +1f1 -> the play sequence; 003ce980: +2d = 1
    void deactivate(std::uint32_t key);  // 00212ae0: +1f1 -> the stop sequence; 003ce790: +2d = 0
    void trigger(std::uint32_t key);     // 00210940
    void kill(std::uint32_t key);        // 00210890
    void set_enabled(std::uint32_t key, bool enable); // 0015bf80 / 0015bf90
    // 00212c80; returns 1. An owning object's Enable / Disable go to 003cea10 -> 003cedd0 (+2c, vt+5c / +60).
    bool message(std::uint32_t key, Message message);

private:
    void play(std::size_t i);            // the shared sequence: !+1f0 -> +1f0 = 1, 0020f990, 003c9420
    void stop(std::size_t i);            // 0020eba0, 003c9420, +1f0 = 0 (no +1f0 check)
    std::size_t at(std::uint32_t key, const char* what) const;

    std::vector<PopulationSoundObject> records_;
    std::vector<SoundGobjState> states_;
    std::map<std::uint32_t, std::size_t> index_;
    SoundGobjHost host_;
};

}  // namespace omega
