#pragma once
#include "population.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace omega {
// The lift (ElevatorGOBJ, GOB kind 3, vtable 004db040, constructor 00215700;
// research/ELEVATORS.md). A solo, unparented lift: the update 002135c0 with the
// path update 00216c60 in mode 4 (and its stop target 002170a8), the receiver
// 00214640, the script natives, the links (+70) and per-floor door extras
// (+e8/+f0) with the door steps 00213dc0 / 00213ee0, and the link pass of
// 00218200 with its rider pass (+84), bit-exact against tools/ee/gen_lift.py,
// gen_lift_links.py and gen_lift_riders.py (tests/lift_goldens.hpp,
// tests/lift_link_goldens.hpp, tests/lift_rider_goldens.hpp).

// The four sound descriptors by their object offset (00215700 / 001ca010).
enum class LiftSound : std::uint16_t { Ding = 0x150, Loop = 0x2e0, Start = 0x470, Stop = 0x600 };

struct LiftEvent {
    enum class Kind : std::uint8_t {
        PathedNotice,       // 004dae90 sent to the lift, transport 0 (+24 = a, +28 = b)
        ElevatorNotice,     // 004daed0 sent to the lift, transport 0 (+24 = a, +28 = b)
        PlaySound,          // 0020f990 message 004dae30 to 004ffb50, transport 1, +20 = 1; a = LiftSound
        StopSound,          // 0020eba0 message 004dadf0 to 004ffb50, transport 1, +20 = 5; a = LiftSound
        Pose,               // a = 0: vt+94 (interpolated), 1: vt+ac (mode-4 endpoint); matrix rows
        Forward,            // the message goes to the PathedGOBJ receiver 00219070
        EnableRecipient,    // recipient a: vt+64(-1) = 0016fd00 (hotbox +2c = 1)
        DisableRecipient,   // recipient a: vt+68(-1) = 0016fbe0 (hotbox +2c = 0)
        ChooseFloor,        // 00214bd0: the HUD's floor chooser (+6a4) for this lift; a = the state index (0045bec0)
        Trigger,            // 00214aa0: 004daf10 (*004f5440) to the lift, transport 0: +24 = 1, +28 = a, +2c = 0,
                            // +30 = the reply's agent +c (b = 1) or 0 (b = 0)
        DoorTrigger,        // 00214470 (a = 0, a link) / 00214310 (a = 1, a door extra) b = its index, object = its ID:
                            // 004daf10 (*004f5440), transport 0, +24 = +28 = +2c = +30 = 0, byte +20 = 1
        Carry,              // 00218200's link pass: a = the link index, object = its ID, matrix = its root x M;
                            // b = 0: the link's vt+94; b = 1: a 004d9360 message (*0055d4e0, +24 = 2) with transport 1
        NetworkTrigger,     // 00214920 for an agent that is not the player: the trigger (+24 = 1, +28 = -1,
                            // +30 = a, the agent's +c) handed to 003fb020, which does nothing solo
        RiderCarry,         // 00218200's rider pass on a live handle: object = the rider, a = its id (the handle's
                            // second word); b = 0: the rider's vt+a8(M + 0x30), matrix[12..15] = M's row 3;
                            // b = 1: a 004d9360 message (*0055d4e0 through 003c9e30, +24 = 3, byte +20 = 1) with
                            // transport 1 (003c9420(msg, rider, 1)), matrix = its +30: rows 0..2 of 0014b210's
                            // identity 0055c9f0, row 3 = (M row 3 x, y, z, 0) (0015a400 / 0014b160)
    };
    Kind kind{};
    std::int32_t a{}, b{};
    std::uint32_t object{};
    std::array<float, 16> matrix{};
};

// A link (+7c[i]) or door extra (+f0[t]) as the door predicates and 00218200
// read it.
struct LiftDoor {
    bool game_gobj{true};        // +4c == *004f5468 (the GameGOBJ base 0021add0 -> 003cf0e0)
    std::int32_t kind{};         // +68 (2: a PathedGOBJ)
    bool stopped{true};          // +dc
    std::int32_t index{};        // *(+d0)
    std::int32_t points{};       // +98
    std::array<float, 16> matrix{}; // its +50 component's +10..+4f (the root placement)
};
using LiftDoorView = std::function<LiftDoor(std::uint32_t)>;
// A rider handle of the vector +84 (8 bytes: the object and the id it had at
// +c, 00256630).
struct LiftRider {
    std::uint32_t object{}, id{};
    bool operator==(const LiftRider&) const = default;
};
// Whether a non-null rider object's +c still equals the handle's id (the
// handle test of 00218240.. / 00256578..): false once the object's base
// destructor 003cad70 has written 0xf0fffff0 there.
using LiftRiderView = std::function<bool(const LiftRider&)>;
// 00214920: 0040c540(msg +30) finds no agent (None), or the agent's +14 byte,
// +58 controller and its vt+54 answer the player (Player), or not (Other).
enum class LiftAgent : std::uint8_t { None, Player, Other };

// The state block (+d0 = +128): index +0, direction +4, destination +20,
// busy +28; then +dc, +e0, +cc and the floor queue (+12c[5], count +140).
struct LiftState {
    std::int32_t index{0}, direction{1}, destination{0}, busy{0};
    bool stopped{true};   // +dc
    float progress{1.F};  // +e0
    std::int32_t stop_target{-1}; // +cc
    std::int32_t queue_count{0};
    std::array<std::int32_t, 5> queue{};
};

class ElevatorGOBJ {
public:
    // 00215700 for a population kind-3 record: the record's references become
    // the links (the GameGOBJ base 0021b388..0021b440, 0040c480 IDs) and its
    // extra references the door extras (00215b88); throws for mismatched path
    // arrays. A record's script name (0021affc -> 003d9d30) is kept for the
    // script host's binding (MissionScriptHost::bind_entity_script). `recipients` is the number of
    // hotboxes registered in +118 (001732a4).
    ElevatorGOBJ(const PopulationObject& object, std::size_t recipients);
    // A lift from raw path data (tests): speed is the runtime +d4.
    ElevatorGOBJ(std::vector<Vec3> points, std::vector<std::array<float, 4>> rotations, float speed,
                 std::size_t recipients);

    // The link and door extra IDs (tests; the population constructor sets them).
    void set_doors(std::vector<std::uint32_t> links, std::vector<std::uint32_t> extras);
    // How the lift reads its links and extras (their objects); required when
    // there are any.
    void set_door_view(LiftDoorView view) { view_ = std::move(view); }
    // The root placement before the first pose write (tests; 00215700 keeps
    // the record's placement).
    void set_placement(const std::array<float, 16>& matrix) { matrix_ = matrix; }
    // +cc (a stop target the PathedGOBJ natives can set on the lift; tests).
    void set_stop_target(std::int32_t target) { state_.stop_target = target; }
    const std::vector<std::uint32_t>& links() const { return links_; }
    const std::vector<std::uint32_t>& extras() const { return extras_; }

    // 002135c0: the path update 00216c60 (mode 4) then the floor state machine.
    void update(float timestep);
    // 00214640 with a trigger message (type *004f5440): action byte +24, floor
    // +28, the agent 0040c540(+30) for action 10 (00214920) with its key +c.
    // Returns false when the message was forwarded to 00219070 (a Forward
    // event; the caller runs the PathedGOBJ receiver).
    bool receive_trigger(std::int32_t action, std::int32_t floor, LiftAgent agent = LiftAgent::None,
                         std::uint32_t agent_key = 0);
    // 00214640 with any other message type: forwarded to 00219070 (a Forward
    // event).
    void receive_other();
    // 00219070's trigger branch (0021909c..00219230) for a trigger 00214640
    // forwarded (receive_trigger returned false): +dc = 0, a PathedNotice
    // (004dae90, +24 = 1, +28 = -1) to the lift (transport 0), +e0 = 0. The
    // caller plays +e4 (0 for a lift) and sends the class-0xb references a
    // GOBJTriggerEventMsg (decomp src/main/GameGOBJ/GameGOBJ_219070.c).
    void receive_forwarded_trigger();
    // 00214640 with a MSGGAME_MENU_CHOICE (*004f7e48) from the floor chooser:
    // 00214aa0. A floor of -0xff (closed) does nothing; any other posts a
    // Trigger event (action 1 to that floor, the agent when `agent`). Solo:
    // the +790 network branch (002ec460) is not taken.
    void receive_menu_choice(std::int32_t floor, bool agent);
    // Natives (object:ElevatorGOBJ).
    void seek_floor(std::int32_t floor, bool ui);          // SeekFloor 002110e0 -> 00214c90(f, ui, 0)
    bool open_doors();                                     // OpenDoors 002110c0 -> 00213dc0
    bool close_doors();                                    // CloseDoors 002110a0 -> 00213ee0
    std::int32_t get_state() const;                        // GetState 00211080 -> 00214fa0
    void lock_floor(std::int32_t floor);                   // LockFloor 00210d00 -> 002155f0
    void unlock_floor(std::int32_t floor);                 // UnlockFloor 00210cd0 -> 00215530
    void set_floor_name(std::int32_t floor, std::string name); // SetFloorName 00210e30 -> 00215010
    void set_sound(LiftSound sound, std::string name);     // Set{Ding,Loop,Start,Stop}Sound 00210df0/d30/db0/d70
    std::int32_t current_floor() const { return state_.index; }      // GetCurrentFloor 00211020
    std::int32_t destination_floor() const { return state_.destination; } // GetDestFloor 00211040
    // Passengers 00211060: the rider count +88 > 0 (dead handles count until
    // the next rider pass removes them). The rider vector +84 has a single
    // writer, the backpack placement 002561d0 (00256400..00256444): actors
    // ride on the moving collision, not on +84.
    bool passengers() const { return !riders_.empty(); }
    // 002561d0's attach (00256630 the handle, 00256500 the find, 002564c0 /
    // 002564e0 the end test, 002564a0 -> 00258e10 the append): the first
    // entry whose resolved object (the object when live, else null) equals
    // the new handle's is a match; with none the handle is appended.
    void attach_rider(const LiftRider& rider);
    // How the lift tells a live handle (required once there are riders).
    void set_rider_view(LiftRiderView view) { rider_view_ = std::move(view); }
    // The rider vector (tests set it directly).
    const std::vector<LiftRider>& riders() const { return riders_; }
    void set_riders(std::vector<LiftRider> riders) { riders_ = std::move(riders); }
    // 00218200(lift, M, s3) itself (the pose writes call it with M = the rigid
    // inverse of the old root x the new root): the rider pass then the link
    // pass. s3 = 1 (teleport) after vt+ac, 0 after vt+94.
    void carry_pass(const std::array<float, 16>& m, bool teleport);

    const LiftState& state() const { return state_; }
    // The events of every operation since the last clear_events, in order.
    const std::vector<LiftEvent>& events() const { return events_; }
    void clear_events() { events_.clear(); }
    const std::vector<float>& lengths() const { return lengths_; }
    const std::vector<std::uint8_t>& locks() const { return locks_; }
    const std::vector<std::string>& floor_names() const { return floor_names_; }
    const std::string& sound(LiftSound sound) const;
    // The last pose written (rows: basis x, y, z with w = 0, then the
    // translation with w = 1); the stored placement until the first write.
    const std::array<float, 16>& matrix() const { return matrix_; }
    std::size_t recipients() const { return recipients_; }
    // The record's script name 0021add0 hands to 003d9d30 (empty: none).
    const std::string& script() const { return script_; }

private:
    std::vector<Vec3> points_;
    std::vector<std::array<float, 4>> rotations_;
    std::vector<float> lengths_;
    std::vector<std::uint8_t> locks_;
    std::vector<std::string> floor_names_;
    std::array<std::string, 4> sounds_;
    float speed_{};
    std::size_t recipients_{};
    LiftState state_;
    std::array<float, 16> matrix_{};
    std::vector<LiftEvent> events_;
    // +84 (count +88, data +8c): 8-byte handles (object, id at object +c).
    std::vector<LiftRider> riders_;
    std::string script_;
    LiftRiderView rider_view_;
    std::vector<std::uint32_t> links_;  // +70 (count) / +74 (+7c data)
    std::vector<std::uint32_t> extras_; // +e8 (count +ec, data +f0)
    LiftDoorView view_;

    void path_update(float timestep);
    void seek(std::int32_t floor, bool ui);
    void arrive();
    bool open();
    bool close();
    LiftDoor door(std::uint32_t id) const;
    bool rider_live(const LiftRider& rider) const;
    bool links_at(bool open) const;
    bool extras_at(bool open) const;
    bool doors_open() const;
    bool doors_closed() const;
    bool trigger_links();
    bool trigger_extras();
    void door_trigger(std::int32_t which, std::size_t index, std::uint32_t id);
    void carry(const std::array<float, 16>& old, bool teleport);
    void enable_recipients();
    void disable_recipients();
    void notice(LiftEvent::Kind kind, std::int32_t a, std::int32_t b);
    void sound_event(LiftEvent::Kind kind, LiftSound sound);
    void pose(int which, const std::array<float, 9>& basis, const Vec3& position);
    Vec3 point(std::int32_t index) const;
    void set_lock(std::int32_t floor, std::uint8_t value);
};

// The level's lifts by population ID in registration order (the object loop
// 00133420 runs each one's +48 update 002135c0 every frame).
class ElevatorWorld {
public:
    ElevatorGOBJ& add(std::uint32_t id, ElevatorGOBJ lift);
    ElevatorGOBJ* find(std::uint32_t id);
    ElevatorGOBJ& at(std::uint32_t id); // throws when the ID is not a lift
    std::vector<std::pair<std::uint32_t, ElevatorGOBJ>>& lifts() { return lifts_; }

private:
    std::vector<std::pair<std::uint32_t, ElevatorGOBJ>> lifts_;
};
}
