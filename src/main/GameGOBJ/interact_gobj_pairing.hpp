#pragma once
// The two-agent pairing of an InteractGOBJ (receiver 00216580, pairing
// 00216230): the buddy-climb objects of TORONTO1 (140480..140486, no script
// handlers). The first agent to trigger the object waits at it (0x31 idle,
// or 0x35 when it already stands at the node's height); the second makes the
// pair, and the one below the node is boosted (leader 0x34 / 0x38) by the
// other (follower 0x33 / 0x37). Each send is 003e67c0(actor, action, self, 0,
// -1.0): an object action of kind 7 with the object = this InteractGOBJ.
//
// Object fields: +a8 the node (its position z), +b0 the pair is done, +b4 the
// first agent, +b8 the second, +94 the first already stands at the node's
// height, +bc a cloned trigger (the generic 0x25 path: a lift call button).
//
// The constructor 00216ae0 keeps the record's references minus the last two
// (+a8 the node, +ac the camera) in the vector +98 (count +9c, data +a0),
// calls each one's vt+110 (00216c50: +6c = 0) and sets its own +68 = 7 when
// one is a lift (+68 == 3). 0xe forwards the clone to each of them.
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace omega {
// The trigger 00216580 cloned (vt+c) into +bc: action +24, word +28, sender
// +2c and the actor +30.
struct InteractTriggerClone {
    std::int32_t action{};
    std::uint32_t word_28{};
    std::uint64_t sender_2c{};
    std::uint64_t actor{};
    bool operator==(const InteractTriggerClone&) const = default;
};

struct InteractPairingState {
    bool done_b0{};
    std::uint64_t first_b4{};
    std::uint64_t second_b8{};
    bool first_up_94{};
    std::optional<InteractTriggerClone> clone_bc;
    bool operator==(const InteractPairingState&) const = default;
};

struct InteractPairingSend {
    std::uint64_t actor{};
    std::int32_t action{};
    bool operator==(const InteractPairingSend&) const = default;
};

// 00216230: `actor` triggered the object (trigger action 7). node_z is the
// height of +a8's position (nullopt: +a8 is null); z_of gives an actor's
// world position z. Returns the object actions in send order.
std::vector<InteractPairingSend> interact_pairing_trigger(InteractPairingState& state, std::uint64_t actor,
                                                          std::optional<float> node_z,
                                                          const std::function<float(std::uint64_t)>& z_of);

struct InteractPairingCodeResult {
    // 003e6690(actor, 1) for each of these (0xd: the other agents of an
    // interrupted pair): the action-0x28 request (+28 kind 2, +2c = 1, the
    // game::StopPerformingAction message) to each actor, transport 0.
    std::vector<std::uint64_t> interrupted;
    // The trigger the object sends itself in solo or as the host (+30 = the
    // sender): 3 after 0xd, 2 after 0xf; nullopt for 0xe.
    std::optional<std::int32_t> self_trigger;
    // 0xe with +bc set (002166a8): the clone, sent with 003c9420(clone,
    // +a0[i], 0) (transport 0) to each kept reference i < +9c, then freed
    // (36b130 line 0x4ed) and +bc = 0.
    std::optional<InteractTriggerClone> forward;
};

// 00216580's CAS branch (key 0055a1d8): `code` (+24) from `sender` (+2c).
// 0xe forwards +bc (nothing when it is null); 0xd (line 0x4f7) and 0xf (line
// 0x526) free it first. Any other code does nothing (the switch falls out).
InteractPairingCodeResult interact_pairing_code(InteractPairingState& state, std::int32_t code, std::uint64_t sender);

// 00216580's trigger branch (key 004f5440): ignored while +bc holds a clone
// (002165c4) or when the actor +30 does not resolve (40c540, `actor` 0); 2
// and 3 (the object's own completion triggers) are ignored, 7 is the
// pairing. Any other action sends 0x25 to the actor (003e67c0(actor, 0x25,
// self, 0, -1.0)) and clones the trigger into +bc (00216628..00216654).
std::vector<InteractPairingSend> interact_pairing_receive_trigger(InteractPairingState& state, std::int32_t action,
                                                                  std::uint64_t actor, std::optional<float> node_z,
                                                                  const std::function<float(std::uint64_t)>& z_of,
                                                                  std::uint32_t word_28 = 0, std::uint64_t sender_2c = 0);
}
