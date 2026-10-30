#include "lan_net_core.hpp"

#include "dme_library.hpp"
#include "net_registrations.hpp"
#include "npc_generator.hpp"
#include "player_camera.hpp"
#include "team_setup.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <stdexcept>

namespace omega::lan {
namespace {
std::string hex8(std::uint32_t v) {
    char text[16];
    std::snprintf(text, sizeof text, "%08x", v);
    return text;
}

[[noreturn]] void unhosted(std::uint32_t address, const std::string& what) {
    throw std::runtime_error("unhosted(0x" + hex8(address) + "): " + what);
}

// TIM0 at BUSCLK / 256 (147.456 MHz / 256 = 576 kHz), compare 0x24a with the count cleared by 004292b0.
constexpr double kTimerEventMs = 586.0 / 576.0;
}  // namespace

LanNetCore::LanNetCore(LanTransport& transport, LanClock clock, bool host_role) : transport_(transport) {
    dme_ = std::make_unique<LanDme>(transport, std::move(clock),
                                    [this](LevelLoadMemory& m, std::uint32_t function, std::uint32_t arg) {
                                        if (function == dme::kSyncCallback) dme_sync_callback_00429c00(m, arg);
                                        else if (function == dme::kClientJoinedGame) dme_remote_client_joined_003faab0(m, *this, arg);
                                        else if (function == 0x429920) call(m, 0x429920, {arg});
                                        else if (function == 0x2d7620) receive_objective_message(arg);
                                        else unhosted(function, "a DME callback that is not ported");
                                    },
                                    [this](LevelLoadMemory& m, std::uint32_t address, std::initializer_list<std::uint32_t> args) {
                                        return call(m, address, args);
                                    });
    auto& m = memory_;
    // The executable's initial .data values that the DME.cc code reads.
    m.write(dme::kLocalSlot, 4, 0xffffffffU);
    m.write(0x494198, 4, 0xffffffffU);
    for (std::uint32_t k = 0; k < 4; ++k) m.write(0x493a10 + k, 1, 2);
    m.write(0x497540, 1, 1);
    m.copy(0x4bfa10, "Foo");
    m.write(0x48a100, 1, 1);  // the team setup latch (0016a970 / 0016aa80)
    m.write(0x48c450, 4, 4);  // the DME's current world: none until the barrier 002e15e0
    m.write(0x497578, 4, 0xffffffffU);  // the GenInfo object handle: none (00439560 sets it, 0043ace0 resets it)
    // 004d6aa0, the static constructors: the NetMap 0x0055c7a0 (003fcca0) is empty, its leftmost node its header.
    m.write(dme::kNetMap + 0xc, 4, dme::kNetMap + 4);
    // 004d7544 / 004d7574 (00431380 / 004313a0): the network object maps 0x00582700 / 0x00582720 are empty.
    m.write(0x58270c, 4, 0x582704);
    m.write(0x58272c, 4, 0x582724);
    // The three transfer maps that 0042e6d0 resets also start empty in the executable's static state.
    for (const auto map : {0x5826a0U, 0x5826c0U, 0x5826e0U}) m.write(map + 0xc, 4, map + 4);
    m.write(0x497568, 1, 1);            // the headset library's first-call latch (00438340)
    m.write(0x497548, 4, 0xffffffffU);  // the headset device: none
    // 004d8c10 / 00472380: cNetMsgThrottle's map 0x00588020 is empty, its leftmost node (+0xc) its header (+4).
    m.write(0x58802c, 4, 0x588024);
    // 004d6c40 / 004d6c60 / 004d6cc0, the static constructors of the message class keys: the action message's
    // 0x0055d480 = 0x004be540, cDisableMsg's 0x0055d498 = 0x004be570, cEnableMsg's 0x0055d4b8 = 0x004be5b0. 003fb020
    // drops a message whose key's word equals 0x0055d480's (the actions it keeps local); with the keys left 0 every
    // message matched, and a cEnableMsg with +24 = 1 or 2 (a joiner's own client index) read as action 1 / 2.
    m.write(0x55d480, 4, 0x4be540);
    m.write(0x55d498, 4, 0x4be570);
    m.write(0x55d4b8, 4, 0x4be5b0);
    // 132ln5h13: 004cd730's generator message keys: "Generator Message" 0x004a4a68 = 0x004a4ab0, "GeneratorBase
    // Message" 0x004a4a70 = 0x004a4ad0, "GeneratorData Message" 0x004a4a78 = 0x004a4af0 (00221610 compares the words).
    m.write(0x4a4a68, 4, 0x4a4ab0);
    m.write(0x4a4a70, 4, 0x4a4ad0);
    m.write(0x4a4a78, 4, 0x4a4af0);
    // 004d3600: MSGGAME_NET_OBJECTIVE_MESSAGE's class name is distinct from the action class.
    // Only pointer identity is used here; this port address stands in for its rodata string.
    m.write(0x51f0b0, 4, 0x4de4f0);
    m.write(0x4de4f0 + 0x10, 4, 0x2d7580);
    // SFOLib's ActivateMsg and DeactivateMsg class-name pointers and pack slots.
    m.write(0x572188, 4, 0x4e0e60);
    m.write(0x572198, 4, 0x4e0e80);
    m.write(0x4e0e60 + 0x10, 4, 0x428cd0);
    m.write(0x4e0e80 + 0x10, 4, 0x429150);
    // Both Objective vtables use cSystem_SendCreated at +0x1c. The matched
    // 0012ed00 simply returns 1, so 003fa940 creates no objective snapshot.
    m.write(0x4db790 + 0x1c, 4, 0x12ed00);
    m.write(0x4db7d0 + 0x1c, 4, 0x12ed00);
    m.write(0x4ffc04, 4, kGuiApp);  // PORT-ONLY: the GUI app pointer (the front end's), read only for nonzero
    // The session state LanSession's host / join leave in place of 0042a870 / 00429790 (see the header).
    m.write(dme::kNetworkGame, 1, 1);
    m.write(dme::kHostRole, 1, host_role ? 1U : 0U);
    m.write(dme::kWorld, 4, 0);
    if (!dme_timer_install_00429430(m, *this)) throw std::runtime_error("LanNetCore: the TIM0 handler was not installed");
    // 00165bc0 (the first network start): the DME init 0042af00 and the 72 message / object registrations on the
    // native DME library; the init leaves 0x005724b0 = 1, the state LanSession's host / join replace (6).
    net_registry_image(m);
    call(m, 0x165bc0, {});
    if (!m.read(dme::kMessagesReady, 1)) throw std::runtime_error("LanNetCore: 00165bc0 did not register the network messages");
    // StaticInit_hud registered cNetTimerMsg as generic cNetMsg inner ID 16;
    // 004c3700 later registered cNetAgentUpdateMsg as ID 32. The verified
    // boot image's next ID is 33; those static constructors are outside this
    // network core.
    m.write(0x4f7dc8, 4, 16);
    m.write(0x4f8588, 4, 19);
    m.write(0x4ea0a0, 4, 32);
    m.write(0x5061d8, 4, 33);
    m.write(dme::kState, 1, 6);
    // 00227ed0 ran during static World_Construct with multiplayer off. The
    // persistent objective manager has no NetMap key; 001350e0 resets category
    // counters before each level's objectives take keys starting at F60001.
    // 132ln5f: the .data the character send 00185a30 reads: the default weapon list 0x0048a2f8 (twelve 0xff bytes)
    // and the player vtable 0x004da690's +0x1c (00185a30 itself).
    for (std::uint32_t k = 0; k < 12; k += 4) m.write(0x48a2f8 + k, 4, 0xffffffffU);
    m.write(0x4da690 + 0x1c, 4, 0x185a30);
    // PORT-ONLY: the characters' stand-ins (the header). The character list 0x004ffd44 is a nonzero block that
    // 0040b6d0 / 0040b790 identify; the game's list is not mirrored.
    m.write(kHumanVtable + 0x20, 4, kFnNop);
    // 132ln5h14c: the human's virtual +0x18 (00184cc0: networked), as 0x004da690's: 002c6b70's 003cc990(.., 0)
    // sends the remote human's health message.
    m.write(kHumanVtable + 0x18, 4, 0x184cc0);
    m.write(kComponentVtable + 0xc, 4, kFnNop);
    m.write(kComponentVtable + 0x58, 4, kFnNop);
    m.write(kRemoteControllerVtable + 0x88, 4, kFnRemote);
    m.write(kPlayerControllerVtable + 0x54, 4, kFnHuman);
    m.write(kRemoteComponent, 4, kComponentVtable);
    m.write(kRemoteComponent + 0x50, 1, 0);
    m.write(0x4ffd44, 4, kCharacters);
    // 132ln5g: the leave (see the header). 0042a870 / 00429790 give the world 00429920 as its client-left
    // callback; LanSession creates the world, so it is set here. 004d6aa0's static constructors leave the client map
    // 0x0055c770 empty (003fb750 reads it; its inserts 003fb9c0 come from the stubbed 001ae450, so it stays empty).
    dme_->set_client_left_callback(0x429920);
    m.write(0x55c77c, 4, 0x55c774);
    // NPCInfo's static list/tree constructors: an empty list links both ends
    // to its sentinel. Native cleanup must not traverse a zero next pointer.
    for (const auto list : {0x582ab0U, 0x582ad0U, 0x582af0U, 0x582b10U, 0x582b30U}) {
        m.write(list + 4, 4, list + 4);
        m.write(list + 8, 4, list + 4);
    }
    m.write(0x582b5c, 4, 0x582b54);
    // The GenInfo queues (static construction's state: empty): the lists 0x005838b8 / 0x005838d8 (header +4: prev
    // +4, next +8) and the map 0x00583900 (root +4, leftmost +0xc = the header +4).
    m.write(0x5838bc, 4, 0x5838bc);
    m.write(0x5838c0, 4, 0x5838bc);
    m.write(0x5838dc, 4, 0x5838dc);
    m.write(0x5838e0, 4, 0x5838dc);
    m.write(0x58390c, 4, 0x583904);
    m.write(kRemoteControllerVtable + 0x54, 4, kFnHuman);  // the remote controller's +0x54 (0014a6b0): nonzero
    m.write(kHumanVtable + 0x60, 4, kFnLeave);
    m.copy(0x4bf9d0, "HOST_DISCONNECT");  // 00429920's text (.rodata)
    m.copy(0x4bfa00, "GAME_DISCONNECT");  // 00429d30's text (.rodata)
    m.copy(0x4bfa18, "F_TO_JOINGAME");    // 0042a1b0's failure text (.rodata)
    m.copy(0x49e8b0, "PLAYER_LEFTGAME");  // 001b2e90's text key (.rodata)
    // 132ln5h3: the locomotion replication (research/NET_INLEVEL.md 4.14). The locomotion message's vtable
    // 0x004e0100 (+8 the destructor, +0xc the clone, +0x10 the serialiser; .data) and the player vtable's entries
    // 001770f0's message reads: +0xf4 the placement (00132280), +0xf8 the floor plane (0014d3d0), +0x110 the floor
    // object (0013e020), +0x18 (00184cc0: 1, a networked actor).
    m.write(0x4e0100 + 0x8, 4, 0x14d270);
    m.write(0x4e0100 + 0xc, 4, 0x3e5d70);
    m.write(0x4e0100 + 0x10, 4, 0x3e57b0);
    m.write(0x4da690 + 0xf4, 4, 0x132280);
    m.write(0x4da690 + 0xf8, 4, 0x14d3d0);
    m.write(0x4da690 + 0x110, 4, 0x13e020);
    m.write(0x4da690 + 0x18, 4, 0x184cc0);
    // 132ln5h: the remote human's controller (the header). The vtable 0x004de530's entries 002d83c0 reads or
    // 002c6b70's callees call (.data), the shot latch 0x0048c0c0 (.data: 1; 002c4f20 clears it).
    m.write(0x4de530 + 0x40, 4, 0x2d83c0);
    m.write(0x4de530 + 0x54, 4, 0x2d7c70);
    m.write(0x4de530 + 0x60, 4, 0x22f5b0);
    m.write(0x4de530 + 0x88, 4, 0x2d7c60);
    m.write(0x48c0c0, 1, 1);
    // PORT-ONLY: the life state virtual +0x108 of the objects 0040c540 answers (the player's 001872f0, a remote
    // human's, the NPC proxy's) answers 0 (alive): the play checks the target it resolves.
    m.write(0x4da690 + 0x108, 4, kFnNop);
    m.write(kHumanVtable + 0x108, 4, kFnNop);
    m.write(kProxyVtable + 0x108, 4, kFnNop);
    // 132ln5h stage 2: the local player's network block +0x3584 is its cNetHuman's data (002d1ff0 from the player
    // update's network tail, linked by 002d1dc0). PORT-ONLY: the player controller's type (virtual +0x88) answers
    // 1, neither the remote human's 4 nor the 2 whose +0x1e0 / +0x1e4 00316980 / 002c4ea0 set (not decoded).
    m.write(kPlayerControllerVtable + 0x88, 4, kFnHuman);
    // 132ln5h4: the controllers' virtual +0x58 / +0x5c the network section 001b1710 asks (the player controller
    // 0x004d9cf0's and the remote controller 0x004de530's: 00140e10 / 001535d0, both answer 0; .data).
    for (const auto vtable : {0x4de530U, kRemoteControllerVtable, kPlayerControllerVtable}) {
        m.write(vtable + 0x58, 4, 0x140e10);
        m.write(vtable + 0x5c, 4, 0x1535d0);
    }
    // 132ln5h11: the generators' messages: cDisableMsg 0x004e0860 / cEnableMsg 0x004e0880 (+8 the destructor
    // 0016fc70 / 0016fd90, +0xc the clone 003ff3b0 / 003ff690, +0x10 the serialiser 003ff570 / 003ff850; .data);
    // PORT-ONLY: the generator blocks' vtable and the players' position virtual +0xc0 (003ce850, the header).
    m.write(0x4e0860 + 0x8, 4, 0x16fc70);
    m.write(0x4e0860 + 0xc, 4, 0x3ff3b0);
    m.write(0x4e0860 + 0x10, 4, 0x3ff570);
    m.write(0x4e0880 + 0x8, 4, 0x16fd90);
    m.write(0x4e0880 + 0xc, 4, 0x3ff690);
    m.write(0x4e0880 + 0x10, 4, 0x3ff850);
    m.write(kGeneratorVtable + 0x28, 4, kFnGenEnable);
    m.write(kGeneratorVtable + 0x2c, 4, kFnGenDisable);
    m.write(kGeneratorVtable + 0xc0, 4, kFnGenPosition);
    m.write(0x4da690 + 0xc0, 4, kFnHumanPosition);
    m.write(kHumanVtable + 0xc0, 4, kFnHumanPosition);
    // 132ln5h12: the spawn message 0x004de240 (+8 the destructor 0021df80, +0xc the clone 002d0ee0, +0x10 the
    // serialiser 002d1100; .data), the generator's virtual +0x18 (00222870: networked) and the NPC controllers'
    // entries (0x004d9380 the AI's, 0x004de2a0 the remote NPC's; .data) the network paths call.
    m.write(0x4de240 + 0x8, 4, 0x21df80);
    m.write(0x4de240 + 0xc, 4, 0x2d0ee0);
    m.write(0x4de240 + 0x10, 4, 0x2d1100);
    // 132ln5h13: the "Generator Message" vtable 0x004db600 (+8 the destructor 00157d20, +0xc the clone 00222410,
    // +0x10 the serialiser 0021d370; .data).
    m.write(0x4db600 + 0x8, 4, 0x157d20);
    m.write(0x4db600 + 0xc, 4, 0x222410);
    m.write(0x4db600 + 0x10, 4, 0x21d370);
    // 132ln5h14: the despawn message vtable 0x004de260 (+8 the destructor 00220350, +0xc the clone 002d15c0, +0x10
    // the serialiser 002d1790; .data).
    m.write(0x4de260 + 0x8, 4, 0x220350);
    m.write(0x4de260 + 0xc, 4, 0x2d15c0);
    m.write(0x4de260 + 0x10, 4, 0x2d1790);
    // 132ln5h14c: the health message vtable 0x004e0ee0 (+8 the destructor 003cc670, +0xc the clone 00431960, +0x10
    // the serialiser 00431bc0; .data).
    m.write(0x4e0ee0 + 0x8, 4, 0x3cc670);
    m.write(0x4e0ee0 + 0xc, 4, 0x431960);
    m.write(0x4e0ee0 + 0x10, 4, 0x431bc0);
    // 132ln5h14c: the damage message MSGLIB_DAMAGE_REQUEST vtable 0x004e08a0 (+8 the destructor 001880e0, +0xc the
    // clone 00407f40, +0x10 the serialiser 004083d0; .data). PORT-ONLY: the damage virtual +0x100 (003ccb00) of the
    // objects 004081e0 reaches (the players, the remote humans, the NPC blocks, the proxy) is kFnDamage (the play's).
    m.write(0x4e08a0 + 0x8, 4, 0x1880e0);
    m.write(0x4e08a0 + 0xc, 4, 0x407f40);
    m.write(0x4e08a0 + 0x10, 4, 0x4083d0);
    // 132ln5h14c: the death action vtable 0x004e01e0 (+8 the destructor 00184eb0, +0xc the clone 003e7370, +0x10 the
    // serialiser 003e6270; .data).
    m.write(0x4e01e0 + 0x8, 4, 0x184eb0);
    m.write(0x4e01e0 + 0xc, 4, 0x3e7370);
    m.write(0x4e01e0 + 0x10, 4, 0x3e6270);
    m.write(0x4da690 + 0x100, 4, kFnDamage);
    m.write(kHumanVtable + 0x100, 4, kFnDamage);
    m.write(kProxyVtable + 0x100, 4, kFnDamage);
    m.write(kGeneratorVtable + 0x18, 4, 0x222870);
    for (const auto& [vtable, entries] : {std::pair{0x4d9380U, std::array<std::uint32_t, 5>{0x12f370, 0x15b7e0, 0x1535d0, 0x1489c0, 0x157a40}},
                                          std::pair{0x4de2a0U, std::array<std::uint32_t, 5>{0x2d2a90, 0x2d2aa0, 0x2d2b20, 0x2d2a80, 0x157a40}}}) {
        m.write(vtable + 0x54, 4, entries[0]);
        m.write(vtable + 0x58, 4, entries[1]);
        m.write(vtable + 0x5c, 4, entries[2]);
        m.write(vtable + 0x88, 4, entries[3]);
        m.write(vtable + 0x44, 4, entries[4]);
    }
}

std::uint32_t LanNetCore::local_player_key() const {
    return (static_cast<std::uint32_t>(dme_->local_index()) + 1) << 24 | kLocalPlayerId;
}

void LanNetCore::bind_backpack(std::uint32_t id, std::uint16_t owner, std::uint32_t address) {
    const auto binding = std::pair{owner, address};
    const auto [it, inserted] = backpack_bindings_.emplace(id, binding);
    if (!inserted && it->second != binding)
        throw std::runtime_error("LanNetCore: conflicting NetMap backpack binding 0x" + hex8(id));
}

void LanNetCore::send_backpack(std::uint8_t type, std::uint8_t owner, std::uint32_t id, std::uint32_t target,
                               std::uint16_t recipient) {
    dme_->send_backpack(type, owner, id, target, recipient);
}

void LanNetCore::require_objective_main_thread() const {
    if (pump_.joinable())
        throw std::runtime_error("LAN objective access during background load pump; defer until stop_load_pump");
}

void LanNetCore::register_objective(std::uint32_t local_script_id) {
    require_objective_main_thread();
    if (!local_script_id) unhosted(0x229900, "an objective with script ID zero");
    if (objective_bindings_.contains(local_script_id))
        unhosted(0x229900, "an objective registered twice (" + std::to_string(local_script_id) + ")");
    // 0040c240: one category counter, incremented by each objective constructor.
    // The original wraps and searches visible GOBJs; this bounded port arena cannot
    // host that many, so fail explicitly before inventing a wrap search.
    if (objective_counter_ == 0xffffU) unhosted(0x40c240, "objective network key counter wrapped");
    ++objective_counter_;
    const auto key = 0x00f60000U | objective_counter_;
    const auto block = next_objective_block_;
    if (block + 0x40 > kNpcBlocks) unhosted(0x229900, "more objectives than the port's blocks hold");
    next_objective_block_ += 0x40;
    auto& m = memory_;
    for (std::uint32_t k = 0; k < 0x40; k += 4) m.write(block + k, 4, 0);
    m.write(block, 4, 0x4db7d0);
    m.write(block + 0xc, 4, key);
    m.write(block + 0x10, 4, key);
    // cSystem_ctor enables the objective; 00229900 clears it on a client.
    m.write(block + 0x14, 1, transport_.is_host() ? 1 : 0);
    // 00229900 calls Activate before assigning the key, then explicitly
    // clears active on a client. No constructor activation packet is sent.
    m.write(block + 0x25, 1, transport_.is_host() ? 1 : 0);
    // cNetObj_AddSystem (00229900) is represented by the existing NetMap object slot.
    // NetObjId: a client binds to the peer-to-peer host, not its local slot.
    call(m, 0x3fb650, {key, transport_.is_host() ? dme_->local_index() : session_host(), block});
    objective_bindings_.emplace(local_script_id, std::pair{key, block});
}

void LanNetCore::unregister_objective(std::uint32_t local_script_id) {
    require_objective_main_thread();
    const auto found = objective_bindings_.find(local_script_id);
    if (found == objective_bindings_.end()) return;
    if (const auto object = call(memory_, 0x3fbb10, {found->second.first}))
        memory_.write(object, 4, 0); // stale packets now fail the original's receiver lookup
    objective_bindings_.erase(found);
}

std::uint32_t LanNetCore::objective_network_key(std::uint32_t local_script_id) const {
    require_objective_main_thread();
    const auto found = objective_bindings_.find(local_script_id);
    return found == objective_bindings_.end() ? 0U : found->second.first;
}

void LanNetCore::send_objective_outcome(std::uint32_t local_script_id, bool success) {
    require_objective_main_thread();
    const auto found = objective_bindings_.find(local_script_id);
    if (found == objective_bindings_.end())
        unhosted(0x225fa0, "an outcome for an objective without a category-0xF6 key");
    if (!memory_.read(dme::kNetworkGame, 1)) return;
    auto& m = memory_;
    const auto caller = sp_;
    sp_ -= 0x100;
    const auto msg = sp_ + 0x40;
    try {
        // 00225fa0 / 002260f0: cNetObjectiveMsg, action 1 / 0, stage -1,
        // flags 4 (all peers), reliable ordered 0x40. The source path uses 003c9420
        // to clone and then reaches the same 003fb020 generic sender; the port has
        // no local objective message queue, so the script host applies locally.
        call(m, 0x3c9e30, {msg, 0x51f0b0});
        m.write(msg, 4, 0x4de4f0);
        m.write(msg + 0x1c, 4, 0x40);
        m.write(msg + 0x20, 1, 4);
        m.write(msg + 0x2c, 1, success ? 1 : 0);
        m.write(msg + 0x30, 4, 0xffffffffU);
        // Succeed/Fail leave +0x34/name uninitialised in the original stack
        // object. They are unused for actions 0/1; use deterministic padding.
        m.write(msg + 0x34, 4, 0);
        for (std::uint32_t i = 0; i < 16; ++i) m.write(msg + 0x38 + i, 1, 0);
        call(m, 0x3fb020, {found->second.first, msg, 0xffffffffU, 0xffffffffU});
        m.write(msg, 4, 0x4de4f0);
        call(m, 0x3c9d60, {msg, 0});
    } catch (...) {
        sp_ = caller;
        throw;
    }
    sp_ = caller;
    if (m.read(dme::kInLevel, 1)) ++objective_sent_;
}

std::uint32_t LanNetCore::receive_objective_message(std::uint32_t data) {
    auto& m = memory_;
    if (!m.read(dme::kBusy, 1)) m.write(dme::kBusy, 1, 1);
    try {
        if (m.read(dme::kInLevel, 1)) {
            // 002d7620 copies every field into cNetObjectiveMsg before looking up
            // d->id in cNetObj. The port queues the outcome for the script host.
            const auto key = m.read(data + 4);
            if (const auto object = call(m, 0x3fbb10, {key}); object && m.read(object)) {
                const auto found = std::find_if(objective_bindings_.begin(), objective_bindings_.end(),
                                                [key](const auto& b) { return b.second.first == key; });
                if (found == objective_bindings_.end())
                    unhosted(0x225cf0, "a known objective-manager packet action outside the implemented outcome bridge");
                const auto action = m.read(data + 8, 1);
                if (action != 0 && action != 1)
                    unhosted(0x228e10, "an objective message action outside Fail / Succeed");
                RemoteObjectiveOutcome outcome;
                outcome.local_script_id = found->first;
                outcome.success = action == 1;
                outcome.network_key = key;
                outcome.timestamp = m.read(data);
                outcome.stage = static_cast<std::int32_t>(m.read(data + 0xc));
                outcome.value = static_cast<std::int32_t>(m.read(data + 0x10));
                for (std::uint32_t i = 0; i < 16; ++i)
                    outcome.name[i] = static_cast<std::uint8_t>(m.read(data + 0x14 + i, 1));
                objective_outcomes_.push_back(outcome);
                ++objective_received_;
            }
        }
    } catch (...) {
        m.write(dme::kBusy, 1, 0);
        throw;
    }
    m.write(dme::kBusy, 1, 0);
    return 0x24;
}

std::vector<LanNetCore::RemoteObjectiveOutcome> LanNetCore::take_objective_outcomes() {
    require_objective_main_thread();
    std::vector<RemoteObjectiveOutcome> out;
    out.swap(objective_outcomes_);
    return out;
}

bool LanNetCore::objective_network_active(std::uint32_t local_script_id) const {
    require_objective_main_thread();
    const auto found = objective_bindings_.find(local_script_id);
    if (found == objective_bindings_.end()) return false;
    return memory_.read(found->second.second + 0x25, 1) != 0;
}

void LanNetCore::send_objective_active(std::uint32_t local_script_id, bool active) {
    require_objective_main_thread();
    const auto found = objective_bindings_.find(local_script_id);
    if (found == objective_bindings_.end())
        unhosted(active ? 0x229000 : 0x229100, "an activation for an objective without a category-0xF6 key");
    auto& m = memory_;
    m.write(found->second.second + 0x25, 1, active ? 1 : 0);
    if (!m.read(dme::kNetworkGame, 1) || !m.read(dme::kInLevel, 1)) return;
    const auto caller = sp_;
    sp_ -= 0x100;
    const auto msg = sp_ + 0x40;
    try {
        call(m, 0x3c9e30, {msg, active ? 0x572188U : 0x572198U});
        m.write(msg, 4, active ? 0x4e0e60U : 0x4e0e80U);
        if (active) {
            // 00229000: the value -1 makes 00428ef0 call only the object's
            // virtual +0x18; +0x28 is state 0, +0x1c reliable ordered.
            m.write(msg + 0x1c, 4, 0x40);
            m.write(msg + 0x24, 4, 0xffffffffU);
            m.write(msg + 0x28, 1, 0);
        }
        // 00428fd0 / 00429200 both swap the target key and message, then
        // call the generic sender with -1 for recipient and mode.
        call(m, 0x3fb020, {found->second.first, msg, 0xffffffffU, 0xffffffffU});
        m.write(msg, 4, active ? 0x4e0e60U : 0x4e0e80U);
        call(m, 0x3c9d60, {msg, 0});
    } catch (...) {
        sp_ = caller;
        throw;
    }
    sp_ = caller;
    ++objective_active_sent_;
}

std::uint32_t LanNetCore::receive_objective_active(std::uint32_t data, bool active) {
    auto& m = memory_;
    if (!m.read(dme::kBusy, 1)) m.write(dme::kBusy, 1, 1);
    try {
        if (m.read(dme::kInLevel, 1)) {
            const auto key = m.read(data + 4);
            if (const auto object = call(m, 0x3fbb10, {key}); object && m.read(object)) {
                const auto found = std::find_if(objective_bindings_.begin(), objective_bindings_.end(),
                                                [key](const auto& b) { return b.second.first == key; });
                if (found == objective_bindings_.end())
                    unhosted(active ? 0x428ef0 : 0x429190, "a known non-objective activation packet");
                if (active && static_cast<std::int32_t>(m.read(data + 8)) != -1)
                    unhosted(0x428ef0, "an objective activation packet with ownership transfer");
                // 00428ef0 / 00429190 call objective virtuals +0x18 / +0x1c.
                m.write(found->second.second + 0x25, 1, active ? 1 : 0);
                objective_actives_.push_back({found->first, active, key});
                ++objective_active_received_;
            }
        }
    } catch (...) {
        m.write(dme::kBusy, 1, 0);
        throw;
    }
    m.write(dme::kBusy, 1, 0);
    return active ? 0x10U : 8U;
}

std::vector<LanNetCore::RemoteObjectiveActive> LanNetCore::take_objective_actives() {
    require_objective_main_thread();
    std::vector<RemoteObjectiveActive> out;
    out.swap(objective_actives_);
    return out;
}

void LanNetCore::send_global_timer(float end, std::string_view label_key) {
    if (pump_.joinable())
        throw std::runtime_error("LAN global timer send during background load pump; defer until stop_load_pump");
    if (label_key.size() > 15)
        throw std::runtime_error("LAN global timer: localization key exceeds cNetTimerMsg's 16-byte buffer");
    auto& m = memory_;
    if (!m.read(dme::kNetworkGame, 1) || !m.read(dme::kInLevel, 1) || !transport_.is_host()) return;
    const auto type = m.read(0x5061c8);  // 002824d0's generic category-2 envelope
    const auto id = m.read(0x4f7dc8);    // StaticInit_hud: cNetTimerMsg
    if (!type || id != 16 || m.read(0x5061d8) != 33)
        unhosted(0x245560, "the cNetTimerMsg registrations are unavailable");
    const auto caller = sp_;
    sp_ -= 0x100;
    const auto packet = sp_ + 0x20;
    try {
        const auto text_bytes = static_cast<std::uint32_t>(label_key.size() + 1);
        const auto inner_size = 6U + text_bytes;
        // 002823e0's generic header, followed by 002467d0's exact payload:
        // raw float bits, u16 strlen+1, then the key including its NUL.
        m.write(packet, 4, call(m, 0x42b1a0, {0}) << 2);
        m.write(packet + 4, 4, local_player_key());
        m.write(packet + 8, 2, inner_size);
        m.write(packet + 0xa, 2, id);
        m.write(packet + 0xc, 4, std::bit_cast<std::uint32_t>(end));
        m.write(packet + 0x10, 2, text_bytes);
        for (std::size_t i = 0; i < label_key.size(); ++i)
            m.write(packet + 0x12 + static_cast<std::uint32_t>(i), 1,
                    static_cast<std::uint8_t>(label_key[i]));
        m.write(packet + 0x12 + static_cast<std::uint32_t>(label_key.size()), 1, 0);
        if (call(m, 0x3fad50, {type, inner_size + 0xc, packet, 0xffffffffU, 0x40U}))
            ++global_timers_sent_;
    } catch (...) {
        sp_ = caller;
        throw;
    }
    sp_ = caller;
}

std::vector<LanNetCore::RemoteGlobalTimer> LanNetCore::take_global_timers() {
    if (pump_.joinable())
        throw std::runtime_error("LAN global timer access during background load pump; defer until stop_load_pump");
    std::vector<RemoteGlobalTimer> out;
    out.swap(global_timers_);
    return out;
}

void LanNetCore::send_personal_timer(std::uint16_t recipient, std::uint32_t target_key, float seconds,
                                    std::int32_t localization_key) {
    if (pump_.joinable())
        throw std::runtime_error("LAN personal timer send during background load pump; defer until stop_load_pump");
    auto& m = memory_;
    if (!m.read(dme::kNetworkGame, 1) || !m.read(dme::kInLevel, 1)) return;
    const auto type = m.read(0x5061c8);
    const auto id = m.read(0x4f8588);
    if (!type || id != 19) unhosted(0x267860, "the cNetLocalTimerMsg registration is unavailable");
    const auto caller = sp_;
    sp_ -= 0x80;
    const auto packet = sp_ + 0x20;
    try {
        m.write(packet, 4, call(m, 0x42b1a0, {0}) << 2);
        m.write(packet + 4, 4, target_key);
        m.write(packet + 8, 2, 8);
        m.write(packet + 0xa, 2, id);
        m.write(packet + 0xc, 4, std::bit_cast<std::uint32_t>(seconds));
        m.write(packet + 0x10, 4, static_cast<std::uint32_t>(localization_key));
        if (call(m, 0x3fad50, {type, 20, packet, recipient, 0x40U})) ++personal_timers_sent_;
    } catch (...) {
        sp_ = caller;
        throw;
    }
    sp_ = caller;
}

std::vector<LanNetCore::RemotePersonalTimer> LanNetCore::take_personal_timers() {
    if (pump_.joinable())
        throw std::runtime_error("LAN personal timer access during background load pump; defer until stop_load_pump");
    std::vector<RemotePersonalTimer> out;
    out.swap(personal_timers_);
    return out;
}

void LanNetCore::send_player_sound(std::uint16_t recipient, bool stop, std::int32_t type, std::string_view name,
                                  std::int32_t priority, std::uint32_t owner_key, std::uint8_t flags) {
    if (pump_.joinable())
        throw std::runtime_error("LAN player sound send during background load pump; defer until stop_load_pump");
    if (name.size() > 15)
        throw std::runtime_error("LAN player sound: name exceeds the retail 16-byte field");
    auto& m = memory_;
    if (!m.read(dme::kNetworkGame, 1) || !m.read(dme::kInLevel, 1)) return;
    const auto id = m.read(stop ? 0x4f53e0 : 0x4f53d0);
    if (!id || id >= 0x100) unhosted(stop ? 0x20e950 : 0x20ee70, "the player sound registration is unavailable");
    const auto caller = sp_;
    sp_ -= 0x80;
    const auto data = sp_ + 0x20;
    try {
        m.write(data, 4, static_cast<std::uint32_t>(type));
        for (std::uint32_t k = 0; k < 16; ++k)
            m.write(data + 4 + k, 1, k < name.size() ? static_cast<std::uint8_t>(name[k]) : 0);
        m.write(data + 20, 4, static_cast<std::uint32_t>(priority));
        const auto size = stop ? 25U : 29U;
        if (stop) {
            m.write(data + 24, 1, flags);
        } else {
            m.write(data + 24, 4, owner_key);
            m.write(data + 28, 1, flags);
        }
        const auto result = dme_->send_message(m, 0x40, 0, recipient, 2, id, size, data);
        if (result) throw std::runtime_error("LAN player sound send failed with DME error " + std::to_string(result));
        ++player_sounds_sent_;
    } catch (...) {
        sp_ = caller;
        throw;
    }
    sp_ = caller;
}

std::vector<LanNetCore::RemotePlayerSound> LanNetCore::take_player_sounds() {
    if (pump_.joinable())
        throw std::runtime_error("LAN player sound access during background load pump; defer until stop_load_pump");
    std::vector<RemotePlayerSound> out;
    out.swap(player_sounds_);
    return out;
}

std::uint32_t LanNetCore::receive_player_sound(std::uint32_t origin, std::uint32_t data, bool stop) {
    auto& m = memory_;
    const auto size = stop ? 25U : 29U;
    const auto available = dme_->incoming_message_remaining();
    if (available != 0xffffffffU && available < size)
        unhosted(stop ? 0x20e8b0 : 0x20edd0, "a truncated player sound message");
    if (!m.read(dme::kInLevel, 1)) return size;
    RemotePlayerSound sound;
    sound.origin = static_cast<std::uint16_t>(origin);
    sound.stop = stop;
    sound.type = static_cast<std::int32_t>(m.read(data));
    for (std::uint32_t k = 0; k < 16; ++k) {
        const auto c = static_cast<char>(m.read(data + 4 + k, 1));
        if (!c) break;
        sound.name.push_back(c);
    }
    if (sound.name.size() == 16 ||
        m.read(data + 4 + static_cast<std::uint32_t>(sound.name.size()), 1) != 0)
        unhosted(stop ? 0x20e8b0 : 0x20edd0, "a player sound name without a terminator");
    sound.priority = static_cast<std::int32_t>(m.read(data + 20));
    if (stop) {
        sound.flags = static_cast<std::uint8_t>(m.read(data + 24, 1));
    } else {
        sound.owner_key = m.read(data + 24);
        sound.flags = static_cast<std::uint8_t>(m.read(data + 28, 1));
    }
    player_sounds_.push_back(std::move(sound));
    ++player_sounds_received_;
    return size;
}

void LanNetCore::send_agent_update(std::span<const std::uint8_t> inner) {
    if (pump_.joinable())
        throw std::runtime_error("LAN agent update during background load pump; defer until stop_load_pump");
    if (inner.size() < 3 || inner.size() > 0x100 || inner[0] != dme_->local_index())
        throw std::runtime_error("LAN agent update: invalid local AgentData stream");
    auto& m = memory_;
    const auto type = m.read(0x5061c8);  // 002824d0's DME registration, category 2
    const auto id = m.read(0x4ea0a0);    // 004c3700's generic inner registration
    if (!type || id != 32 || m.read(0x5061d8) != 33)
        unhosted(0x12eee0, "the AgentUpdate message registrations are unavailable");
    const auto caller = sp_;
    sp_ -= 0x200;
    const auto packet = sp_ + 0x20;
    try {
        // 0012eee0 chooses NetObj key 0, flags 4 (all peers). 002823e0
        // writes Event_PackHeader, a 16-bit body length and the registered
        // inner ID, then 00138820 writes the already encoded inner stream.
        // Synthetic bytes are fully defined; the original stack message has
        // uninitialised fields that are not serialized by this path.
        m.write(packet, 4, call(m, 0x42b1a0, {0}) << 2);  // transport +0x18 = 0
        m.write(packet + 4, 4, 0);
        m.write(packet + 8, 2, static_cast<std::uint32_t>(inner.size()));
        m.write(packet + 0xa, 2, id);
        for (std::size_t i = 0; i < inner.size(); ++i) m.write(packet + 0xc + static_cast<std::uint32_t>(i), 1, inner[i]);
        // Full updates set +0x1c=0x40 (ordered). A stats-only message leaves
        // that ctor field zero; 003fad50 still uses the native send path.
        if (call(m, 0x3fad50, {type, static_cast<std::uint32_t>(inner.size() + 0xc), packet,
                               0xffffffffU, inner[1] ? 0U : 0x40U})) ++agent_updates_sent_;
    } catch (...) {
        sp_ = caller;
        throw;
    }
    sp_ = caller;
}

std::uint32_t LanNetCore::receive_generic_message(std::uint32_t origin, std::uint32_t data) {
    auto& m = memory_;
    const auto available = dme_->incoming_message_remaining();
    if (available != 0xffffffffU && available < 0xc)
        unhosted(0x2822f0, "a truncated generic cNetMsg header");
    const auto len = m.read(data + 8, 2);
    if (len > 0x100) unhosted(0x2822f0, "an oversized generic cNetMsg body");
    const auto consumed = len + 0xc;
    if (available != 0xffffffffU && consumed > available)
        unhosted(0x2822f0, "a truncated generic cNetMsg body");
    // 002822f0's guard runs on every callback; before in-level it consumes
    // the packet but does not dispatch the registered inner receiver.
    m.write(dme::kBusy, 1, 1);
    try {
        if (m.read(dme::kInLevel, 1)) {
            const auto id = static_cast<std::int16_t>(m.read(data + 0xa, 2));
            if (id == static_cast<std::int32_t>(m.read(0x4ea0a0))) {
                if (len < 3 || origin >= 4) unhosted(0x12f0d0, "an invalid AgentUpdate stream or origin");
                RemoteAgentUpdate update;
                update.origin = static_cast<std::uint16_t>(origin);
                update.timestamp = m.read(data) >> 2;
                update.target_key = m.read(data + 4);
                update.inner.reserve(len);
                for (std::uint32_t i = 0; i < len; ++i)
                    update.inner.push_back(static_cast<std::uint8_t>(m.read(data + 0xc + i, 1)));
                update.client = update.inner[0];
                if (update.client >= 4) unhosted(0x12f0d0, "an AgentUpdate client outside four slots");
                // 0012f0d0 indexes G+0x1c8 with the declared client byte. It
                // may differ from the immediate origin when the host forwards.
                agent_updates_.push_back(std::move(update));
                ++agent_updates_received_;
            } else if (id == static_cast<std::int32_t>(m.read(0x4f7dc8))) {
                if (origin != 0) unhosted(0x245750, "a cNetTimerMsg from a non-authoritative client");
                if (len < 7 || len > 22) unhosted(0x245750, "an invalid cNetTimerMsg payload size");
                const auto text_bytes = m.read(data + 0x10, 2);
                if (!text_bytes || text_bytes > 16 || len != 6 + text_bytes ||
                    m.read(data + 0x11 + text_bytes, 1) != 0)
                    unhosted(0x245750, "an invalid cNetTimerMsg localization key");
                RemoteGlobalTimer timer;
                timer.origin = static_cast<std::uint16_t>(origin);
                timer.end = std::bit_cast<float>(m.read(data + 0xc));
                timer.timestamp = m.read(data) >> 2;
                timer.target_key = m.read(data + 4);
                timer.label_key.reserve(text_bytes - 1);
                for (std::uint32_t i = 0; i + 1 < text_bytes; ++i) {
                    const auto byte = static_cast<char>(m.read(data + 0x12 + i, 1));
                    if (!byte) unhosted(0x245750, "an embedded NUL in cNetTimerMsg's localization key");
                    timer.label_key.push_back(byte);
                }
                global_timers_.push_back(std::move(timer));
                ++global_timers_received_;
            } else if (id == static_cast<std::int32_t>(m.read(0x4f8588))) {
                if (len != 8) unhosted(0x2679f0, "an invalid cNetLocalTimerMsg payload size");
                RemotePersonalTimer timer;
                timer.origin = static_cast<std::uint16_t>(origin);
                timer.seconds = std::bit_cast<float>(m.read(data + 0xc));
                timer.localization_key = static_cast<std::int32_t>(m.read(data + 0x10));
                timer.timestamp = m.read(data) >> 2;
                timer.target_key = m.read(data + 4);
                if (timer.target_key != local_player_key())
                    unhosted(0x2679f0, "a cNetLocalTimerMsg for a non-local NetMap key");
                personal_timers_.push_back(timer);
                ++personal_timers_received_;
            } else {
                unhosted(0x2822f0, "an unsupported generic cNetMsg inner ID");
            }
        }
    } catch (...) {
        m.write(dme::kBusy, 1, 0);
        throw;
    }
    m.write(dme::kBusy, 1, 0);
    return consumed;
}

std::vector<LanNetCore::RemoteAgentUpdate> LanNetCore::take_agent_updates() {
    if (pump_.joinable())
        throw std::runtime_error("LAN agent update access during background load pump; defer until stop_load_pump");
    std::vector<RemoteAgentUpdate> out;
    out.swap(agent_updates_);
    return out;
}

void LanNetCore::send_end_level(std::uint8_t kind, bool quit) {
    if (pump_.joinable())
        throw std::runtime_error("LAN ENDLEVEL send during background load pump; defer until stop_load_pump");
    auto& m = memory_;
    // 0012fa60 gives clients flags 1 (local delivery only) and hosts the
    // constructor's flags 5 (local delivery plus broadcast). The gameplay
    // owner has already applied the local delivery before this transport call.
    if (!m.read(dme::kNetworkGame, 1) || !m.read(dme::kInLevel, 1) || !transport_.is_host()) return;
    const auto type = m.read(0x4f53a0);  // 002e9e50's category-2 registration
    if (!type) unhosted(0x12fa60, "the ENDLEVEL DME registration is unavailable");
    const auto caller = sp_;
    sp_ -= 0x100;
    const auto msg = sp_ + 0x20, type_out = sp_ + 0x60, packet = sp_ + 0x70;
    try {
        call(m, 0x3c9e30, {msg, 0x4f5398});
        m.write(msg, 4, 0x4dadb0);
        m.write(msg + 0x24, 1, kind);
        m.write(msg + 0x28, 4, quit ? 1U : 0U);
        // The native +9..+11 stack bytes are unspecified and ignored by
        // 0020e350. Give synthetic packets deterministic zero padding.
        m.write(packet + 9, 1, 0);
        m.write(packet + 0xa, 2, 0);
        if (call(m, 0x20e4c0, {msg, 0, type_out, 0, 0, 16, packet}) != 16 || m.read(type_out) != type)
            unhosted(0x20e4c0, "ENDLEVEL packet packing or registration failed");
        if (call(m, 0x3fad50, {type, 16, packet, 0xffffffffU, m.read(msg + 0x1c)})) ++end_levels_sent_;
        m.write(msg, 4, 0x4dadb0);
        call(m, 0x3c9d60, {msg, 0});
    } catch (...) {
        sp_ = caller;
        throw;
    }
    sp_ = caller;
}

std::uint32_t LanNetCore::receive_end_level(std::uint32_t origin, std::uint32_t data) {
    auto& m = memory_;
    const auto available = dme_->incoming_message_remaining();
    if (available != 0xffffffffU && available < 16)
        unhosted(0x20e350, "a truncated ENDLEVEL packet");
    m.write(dme::kBusy, 1, 1);
    try {
        // 0020e350 itself also checks GameState 0x0f/0x12 before calling
        // cGameSystem_Restart. The simulation owns that state and applies
        // the gate when draining; the transport never echoes on receive.
        if (m.read(dme::kInLevel, 1)) {
            end_levels_.push_back({static_cast<std::uint16_t>(origin),
                                   static_cast<std::int8_t>(m.read(data + 8, 1)),
                                   m.read(data + 0xc), m.read(data) >> 2, m.read(data + 4)});
            ++end_levels_received_;
        }
    } catch (...) {
        m.write(dme::kBusy, 1, 0);
        throw;
    }
    m.write(dme::kBusy, 1, 0);
    return 16;
}

std::vector<LanNetCore::RemoteEndLevel> LanNetCore::take_end_levels() {
    if (pump_.joinable())
        throw std::runtime_error("LAN ENDLEVEL access during background load pump; defer until stop_load_pump");
    std::vector<RemoteEndLevel> out;
    out.swap(end_levels_);
    return out;
}

// 132ln5h11 (the header).
void LanNetCore::register_generator(std::uint32_t id, const std::array<float, 3>& position) {
    auto& m = memory_;
    if (generator_blocks_.contains(id)) unhosted(0x3fb650, "a generator registered twice (" + std::to_string(id) + ")");
    const auto block = next_generator_;
    if (block + 0x40 > kLocalPlayer) unhosted(0x3fb650, "more generators than the port's blocks hold");
    next_generator_ += 0x40;
    for (std::uint32_t k = 0; k < 0x40; k += 4) m.write(block + k, 4, 0);
    m.write(block, 4, kGeneratorVtable);
    m.write(block + 0xc, 4, id);
    m.write(block + 0x10, 4, id);
    for (std::uint32_t c = 0; c < 3; ++c) m.write(block + 0x30 + c * 4, 4, std::bit_cast<std::uint32_t>(position[c]));
    m.write(block + 0x3c, 4, std::bit_cast<std::uint32_t>(1.0F));
    generator_blocks_.emplace(id, block);
}

std::uint32_t LanNetCore::generator_block(std::uint32_t id) const {
    const auto found = generator_blocks_.find(id);
    if (found == generator_blocks_.end()) unhosted(0x3fb650, "a generator the port did not register (" + std::to_string(id) + ")");
    return found->second;
}

// PORT-ONLY stand-in for 003fb8d0(client) (the client map 0x0055c770's player of a client; 003fb9c0 inserts them,
// not ported): the local player for the local client once set, a remote human whose key's top byte is client + 1.
std::uint32_t LanNetCore::client_player_003fb8d0(std::int32_t client) {
    if (client < 0) return 0;
    if (local_set_ && client == static_cast<std::int32_t>(dme_->local_index())) return kLocalPlayer;
    for (const auto& [key, human] : remote_keys_)
        if (static_cast<std::int32_t>((memory_.read(human + 0xc) & 0x7f000000U) >> 24) - 1 == client) return human;
    return 0;
}

// The cEnableMsg / cDisableMsg the generator's +64 / +68 builds on its stack (00221260.. / 00220de0..): 003c9e30
// (msg, 0055d4b8 / 0055d498), the vtable 004e0880 / 004e0860, +24 = a1, +1c = 0x40, byte +20 = 4; 003ff930(msg, +10)
// / 003ff650(msg, +10, -1); the vtable again and 003c9d60(msg, 0).
void LanNetCore::send_generator_message(std::uint32_t id, std::int32_t a1, bool enable) {
    auto& m = memory_;
    generator_block(id);
    const auto caller = sp_;
    sp_ -= 0x100;
    const auto msg = sp_ + 0x40;
    const auto vtable = enable ? 0x4e0880U : 0x4e0860U;
    call(m, 0x3c9e30, {msg, enable ? 0x55d4b8U : 0x55d498U});
    m.write(msg, 4, vtable);
    m.write(msg + 0x24, 4, static_cast<std::uint32_t>(a1));
    m.write(msg + 0x1c, 4, 0x40);
    m.write(msg + 0x20, 1, 4);
    if (enable) call(m, 0x3ff930, {msg, id});
    else call(m, 0x3ff650, {msg, id, 0xffffffffU});
    m.write(msg, 4, vtable);
    call(m, 0x3c9d60, {msg, 0});
    sp_ = caller;
    ++generator_sent_;
}

GeneratorNetHost LanNetCore::generator_net_host() {
    auto& m = memory_;
    GeneratorNetHost net;
    net.network = m.read(dme::kNetworkGame, 1) != 0;
    net.host_role = m.read(dme::kHostRole, 1) != 0;
    const auto keyp = [this, &m](std::uint32_t key) {
        m.write(kGeneratorKey, 4, key);
        return kGeneratorKey;
    };
    net.local_slot = [this, &m] {
        if (m.read(0x494178) == 0xffffffffU) call(m, 0x2ebaf8, {0x494178, m.read(dme::kWorld)});
        return static_cast<std::int32_t>(m.read(0x494178));
    };
    net.geninfo_43b190 = [this, &m] { call(m, 0x43b190, {0}); };
    net.owned_43aa40 = [this, &m, keyp](std::uint32_t key) { return call(m, 0x43aa40, {0, keyp(key)}) != 0; };
    net.owner_439c70 = [this, &m, keyp](std::uint32_t key) {
        return static_cast<std::int32_t>(call(m, 0x439c70, {0, keyp(key)}));
    };
    net.claimed_43ab20 = [this, &m, keyp](std::uint32_t key) { return call(m, 0x43ab20, {0, keyp(key)}) != 0; };
    net.interest_43aba0 = [this, &m, keyp](std::uint32_t key, std::int32_t client) {
        return call(m, 0x43aba0, {0, keyp(key), static_cast<std::uint32_t>(client)}) != 0;
    };
    net.add_4398e0 = [this, &m, keyp](std::uint32_t key, std::int32_t owner) {
        call(m, 0x4398e0, {0, keyp(key), static_cast<std::uint32_t>(owner)});
    };
    net.set_owner_439d00 = [this, &m, keyp](std::uint32_t key, std::int32_t owner) {
        call(m, 0x439d00, {0, keyp(key), static_cast<std::uint32_t>(owner)});
    };
    net.clear_interest_43a030 = [this, &m, keyp](std::uint32_t key) { call(m, 0x43a030, {0, keyp(key), 1}); };
    net.set_interest_43a420 = [this, &m, keyp](std::uint32_t key) { call(m, 0x43a420, {0, keyp(key), 1}); };
    net.session_host_2ebba0 = [this, &m]() -> std::optional<std::int32_t> {
        const auto out = kScratch + 0x84;
        constexpr std::uint32_t sentinel = 0x5eed5eedU;
        m.write(out, 4, sentinel);
        call(m, 0x2ebba0, {m.read(dme::kWorld), out});
        const auto v = m.read(out);
        if (v == sentinel) return std::nullopt;
        return static_cast<std::int32_t>(v);
    };
    net.netmap_insert_3fb650 = [this, &m](std::uint32_t key, std::int32_t client) {
        call(m, 0x3fb650, {key, static_cast<std::uint32_t>(client), generator_block(key)});
    };
    net.clock_42b1a0 = [this, &m] { return static_cast<std::int32_t>(call(m, 0x42b1a0, {1})); };
    net.nearest_player_3ce850 = [this, &m](std::uint32_t key) -> std::optional<std::uint32_t> {
        const auto player = call(m, 0x3ce850, {generator_block(key), 0});
        if (!player) return std::nullopt;
        if (player == kLocalPlayer) return local_player_key();  // PORT-ONLY: the port's local player's +c is its id
        return m.read(player + 0xc);
    };
    net.client_player_3fb8d0 = [this](std::int32_t client) { return client_player_003fb8d0(client) != 0; };
    net.send_enable_3ff930 = [this](std::uint32_t key, std::int32_t a1) { send_generator_message(key, a1, true); };
    net.send_disable_3ff650 = [this](std::uint32_t key, std::int32_t a1) { send_generator_message(key, a1, false); };
    return net;
}

GeneratorNetPrelogicHost LanNetCore::generator_prelogic_host(ActorKey key) {
    auto& m = memory_;
    GeneratorNetPrelogicHost mp;
    mp.npc_info_begin_436c00 = [this, &m] { call(m, 0x436c00, {kGeneratorNpcInfo}); };
    mp.npc_info_end_436ba0 = [this, &m] { call(m, 0x436ba0, {kGeneratorNpcInfo, 0xffffffffU}); };
    // 132ln5h12: a generator's spawned NPCs: their keys' NPC info entries on the pass's stack object and their
    // PORT-ONLY blocks (register_npc). The play answers the actor's own virtual calls it hosts (+20, +5c(0)).
    const auto keyp = [&m](std::uint32_t c) {
        m.write(kGeneratorKey, 4, c);
        return kGeneratorKey;
    };
    const auto block = [this, key](std::uint32_t actor) {
        if (!key) unhosted(0x21f0a0, "a generator's NPC without the play's actor keys");
        return npc_block(key(actor));
    };
    const auto none = [](std::uint32_t address, const char* what) {
        return [address, what](auto&&...) -> void { unhosted(address, std::string("a generator's NPC: ") + what); };
    };
    mp.npc_owner_434440 = [this, &m, keyp](std::uint32_t c) {
        return static_cast<std::int32_t>(call(m, 0x434440, {kGeneratorNpcInfo, keyp(c)}));
    };
    mp.npc_apply_4341f0 = [this, &m, keyp](std::uint32_t c) { call(m, 0x4341f0, {kGeneratorNpcInfo, keyp(c)}); };
    mp.npc_known_4340c0 = [this, &m, keyp](std::uint32_t c) { return call(m, 0x4340c0, {kGeneratorNpcInfo, keyp(c)}) != 0; };
    const auto component = [this, &m, block](std::uint32_t actor, std::uint32_t slot) -> std::optional<bool> {
        const auto controller = m.read(block(actor) + 0x58);
        if (!controller) return std::nullopt;
        return call(m, m.read(m.read(controller) + slot), {controller}) != 0;
    };
    mp.component_58 = [component](std::uint32_t actor) { return component(actor, 0x58); };
    mp.component_5c = [component](std::uint32_t actor) { return component(actor, 0x5c); };
    mp.actor_20 = none(0x1b4f40, "its virtual +20 (the play's)");
    mp.actor_24 = none(0x1b4970, "its virtual +24 (the play's)");
    mp.activate_5c0 = none(0x1b53d0, "its virtual +5c(0) (the play's)");
    mp.deactivate_600 = none(0x1b5190, "its virtual +60(0) (the play's)");
    mp.actor_3584 = [&m, block](std::uint32_t actor) { return m.read(block(actor) + 0x3584); };
    mp.actor_key_10 = [&m, block](std::uint32_t actor) { return m.read(block(actor) + 0x10); };
    mp.netmap_object_3fbb10 = [this, &m](std::uint32_t k) { return call(m, 0x3fbb10, {k}); };
    mp.attach_185970 = [this, &m, block](std::uint32_t actor, std::uint32_t object) {
        call(m, 0x185970, {block(actor), object});
    };
    mp.send_despawn_msg = [this, key](std::uint32_t actor) {
        if (!key) unhosted(0x21f6c8, "a generator's NPC despawn without the play's actor keys");
        send_npc_despawn(key(actor));
    };
    // 0021eaa0's message 0x00582730 (vtable 004d9300, byte +24 the kind) to the actor (003c9420(msg, actor, 1):
    // delivered at once); its receiver 001b3d98..001b4008 (in the human's handler 001a92a0) is ported here on the
    // block (132ln5h13): kind 2 keeps a controller whose +0x88 answers 2, else swaps in the remote NPC controller
    // 002d6030 (+0x14 = 0); kind 1 keeps a null controller or one whose +0x88 answers 1, else swaps the AI controller
    // back (001ae450(actor, 2, ..)). Kind 2 then releases the cNetHuman's ownership (002d2110(+0x3584)). PORT-ONLY:
    // the message object is not built, the controller objects are the block's vtable word, and the swap's AI hold /
    // resume and the new controller's activation +0x48 are the play's (take_npc_controls).
    mp.send_control_msg = [this, &m, block, key](std::uint32_t actor, std::uint8_t kind) {
        const auto b = block(actor);
        if (kind != 1 && kind != 2) unhosted(0x582730, "a control message of kind " + std::to_string(kind));
        const auto type = call(m, m.read(m.read(b + kNpcController) + 0x88), {b + kNpcController});
        if (kind == 2) {
            if (type != 2) {
                install_remote_npc_controller(b);
                npc_control({key(actor), 2});
            }
            if (const auto data = m.read(b + 0x3584)) call(m, 0x2d2110, {data});
            return;
        }
        if (type == 1) return;
        // 001ae450(actor, 2, ..): a fresh AI controller (00158800 keeps the word +0x150); the remote's link words go.
        m.write(b + kNpcController, 4, 0x4d9380U);
        m.write(b + kNpcController + 0x1e0, 4, 0);
        m.write(b + kNpcController + 0x1e4, 4, 0);
        npc_control({key(actor), 1});
    };
    mp.message_destroyed = [] {};  // 003c9d60 of the stack message: the port builds none (above)
    mp.set_actor_14 = [&m, block](std::uint32_t actor, std::uint8_t value) { m.write(block(actor) + 0x14, 1, value); };
    mp.netmap_client_3fb340 = [this, &m](std::uint32_t c) { return static_cast<std::int32_t>(call(m, 0x3fb340, {c})); };
    mp.netmap_set_client_3fb4b0 = [this, &m](std::uint32_t c, std::int32_t client) {
        call(m, 0x3fb4b0, {c, static_cast<std::uint32_t>(client)});
    };
    return mp;
}

std::uint32_t LanNetCore::npc_block(std::uint32_t c) const {
    const auto found = npc_blocks_.find(c);
    if (found == npc_blocks_.end()) unhosted(0x40c540, "an NPC the port did not register (0x" + hex8(c) + ")");
    return found->second;
}

// 002d6030's writes on the block (PORT-ONLY: the controller object is the block's controller word, the play's
// NpcAiController::set_remote builds the rest): the vtable 0x004de2a0, +1e0 = +1e4 = 0, then with +0x3584 its
// +1e0 = the cNetHuman and +1e4 = its +0x1c (the counters); the actor's +0x14 = 0.
void LanNetCore::install_remote_npc_controller(std::uint32_t b) {
    auto& m = memory_;
    const auto controller = b + kNpcController;
    m.write(controller, 4, 0x4de2a0U);
    m.write(controller + 0x1e0, 4, 0);
    m.write(controller + 0x1e4, 4, 0);
    if (const auto data = m.read(b + 0x3584)) {
        m.write(controller + 0x1e0, 4, data);
        m.write(controller + 0x1e4, 4, data + 0x1c);
    }
    m.write(b + 0x14, 1, 0);
}

// 132ln5h12 (the header).
void LanNetCore::register_npc(std::uint32_t c, std::uint32_t generator) {
    auto& m = memory_;
    if (npc_blocks_.contains(c)) unhosted(0x40b790, "an NPC registered twice (0x" + hex8(c) + ")");
    std::uint32_t block = 0;
    if (!free_npc_blocks_.empty()) {
        block = free_npc_blocks_.back();
        free_npc_blocks_.pop_back();
    } else {
        if (next_npc_block_ >= kNpcBlocks + kNpcBlockCount * kNpcBlockSize) unhosted(0x21e620, "more NPCs than the port's blocks hold");
        block = next_npc_block_;
        next_npc_block_ += kNpcBlockSize;
    }
    for (std::uint32_t k = 0; k < kNpcBlockSize; k += 4) m.write(block + k, 4, 0);
    m.write(block, 4, 0x4da690);
    m.write(block + 0xc, 4, c);
    m.write(block + 0x10, 4, c);
    m.write(block + 0x49, 1, 2);
    const auto controller = block + kNpcController;
    m.write(controller, 4, 0x4d9380);
    m.write(controller + 0x30, 4, block);
    m.write(block + 0x58, 4, controller);
    // 132ln5h14b: PORT-ONLY, the inventory +0x3524 001b1710's 001b2d20 reads (the six slots empty, +0x84 = 6).
    for (std::uint32_t k = 0; k < 6; ++k) m.write(block + kNpcInventory + k * 16 + 8, 4, 0xffffffffU);
    m.write(block + kNpcInventory + 0x84, 1, 6);
    m.write(block + 0x3524, 4, block + kNpcInventory);
    npc_blocks_.emplace(c, block);
    npc_generators_[c] = generator;
}

// 132ln5h13 (the header): 001b53d0's network tail on the NPC's block. Frame 0x40.
void LanNetCore::npc_activate_001b53d0(std::uint32_t c) {
    auto& m = memory_;
    const auto p = npc_block(c);
    const auto caller = sp_;
    sp_ -= 0x40;
    const auto sp = sp_;
    const auto slot = [&] {
        if (!m.read(dme::kNetworkGame, 1)) return 0U;
        if (m.read(0x494178) == 0xffffffffU) call(m, 0x2ebaf8, {0x494178, m.read(0x5721a8)});
        return m.read(0x494178);
    };
    if (m.read(dme::kNetworkGame, 1)) {
        if (!m.read(p + 0x3584) && m.read(0x5721d0, 1)) {
            call(m, 0x2d1ff0, {p + 0xc, m.read(p + 0x10)});
            call(m, 0x3fb650, {m.read(p + 0x10), slot(), p});
        }
        call(m, 0x3fb4b0, {m.read(p + 0x10), slot()});
        call(m, 0x436c00, {sp + 0x3c});
        call(m, 0x4341f0, {sp + 0x3c, p + 0xc});
        m.write(p + 0x14, 1, 1);
        if (const auto block = m.read(p + 0x3584)) call(m, 0x2d1d80, {block});
        call(m, 0x436ba0, {sp + 0x3c, 0xffffffffU});
    }
    m.write(p + 0x3268, 1, 1);
    sp_ = caller;
}

// 132ln5h14 (the header).
void LanNetCore::npc_deactivate_001b5190(std::uint32_t c, std::uint8_t life_108, float health_40) {
    auto& m = memory_;
    if (!m.read(dme::kNetworkGame, 1)) return;
    const auto p = npc_block(c);
    const auto caller = sp_;
    sp_ -= 0x50;
    const auto obj = sp_ + 0x4c;
    call(m, 0x436c00, {obj});
    call(m, 0x433ce0, {obj, p + 0xc});
    if (const auto data = m.read(p + 0x3584)) {
        // 001b5238..001b5258: cvt.w.s truncates, so +40 > 0 as a word is +40 >= 1.0.
        if (life_108 != 3 || health_40 >= 1.0F) call(m, 0x2d2110, {data});
    }
    call(m, 0x436ba0, {obj, 0xffffffffU});
    sp_ = caller;
}

void LanNetCore::npc_deactivate_001b4970(std::uint32_t c) {
    auto& m = memory_;
    const auto p = npc_block(c);
    const auto caller = sp_;
    sp_ -= 0x40;
    const auto obj = sp_ + 0x3c;
    call(m, 0x436c00, {obj});
    call(m, 0x433ce0, {obj, p + 0xc});
    call(m, 0x436ba0, {obj, 0xffffffffU});
    sp_ = caller;
}

// 132ln5h13: 0014c510..0014c5f8 on the NPC's block (frame 0x640 of 0014bff0: the NPC info object at +0x63c).
void LanNetCore::npc_contact_0014c510(std::uint32_t c, std::uint8_t contact_158, const std::array<float, 3>& position) {
    auto& m = memory_;
    if (!m.read(dme::kNetworkGame, 1)) return;
    const auto p = npc_block(c);
    const auto caller = sp_;
    sp_ -= 0x640;
    const auto obj = sp_ + 0x63c;
    call(m, 0x436c00, {obj});
    std::uint32_t slot = 0;
    if (m.read(dme::kNetworkGame, 1)) {
        if (m.read(0x494178) == 0xffffffffU) call(m, 0x2ebaf8, {0x494178, m.read(0x5721a8)});
        slot = m.read(0x494178);
    }
    if ((call(m, 0x433ec0, {obj, p + 0xc, slot}) & 0xffU) != contact_158) call(m, 0x433f20, {obj, p + 0xc, contact_158});
    if (m.read(p + 0x14, 1))
        if (const auto data = m.read(p + 0x3584))
            for (std::uint32_t k = 0; k < 3; ++k) m.write(data + 0x44 + k * 4, 4, std::bit_cast<std::uint32_t>(position[k]));
    call(m, 0x436ba0, {obj, 0xffffffffU});
    sp_ = caller;
}

void LanNetCore::npc_speed_0014c6e0(std::uint32_t c, float speed) {
    auto& m = memory_;
    if (!m.read(dme::kNetworkGame, 1)) return;
    const auto p = npc_block(c);
    if (m.read(p + 0x14, 1))
        if (const auto data = m.read(p + 0x3584)) m.write(data + 0x3c, 4, std::bit_cast<std::uint32_t>(speed));
}

void LanNetCore::npc_heading_001a99a4(std::uint32_t c, float yaw) {
    auto& m = memory_;
    if (!m.read(dme::kNetworkGame, 1)) return;
    const auto p = npc_block(c);
    if (m.read(p + 0x14, 1))
        if (const auto data = m.read(p + 0x3584)) m.write(data + 0x40, 4, std::bit_cast<std::uint32_t>(yaw));
}

std::optional<LanNetCore::NpcNetLink> LanNetCore::npc_net_link(std::uint32_t c) const {
    const auto& m = memory_;
    const auto controller = npc_block(c) + kNpcController;
    const auto data = m.read(controller + 0x1e0);
    if (!data || !m.read(controller + 0x1e4)) return std::nullopt;
    NpcNetLink l;
    l.point_valid_0 = m.read(data, 1) != 0;
    l.aiming_20 = m.read(data + 0x20, 1) != 0;
    l.firing_21 = m.read(data + 0x21, 1) != 0;
    l.target_8 = m.read(data + 8);
    l.subtarget_c = static_cast<std::int8_t>(m.read(data + 0xc, 1));
    for (std::uint32_t k = 0; k < 3; ++k) l.point_10[k] = std::bit_cast<float>(m.read(data + 0x10 + k * 4));
    return l;
}
bool LanNetCore::npc_fire_gate_002c4f20(std::uint32_t c) {
    auto& m=memory_;
    const auto counter=m.read(npc_block(c)+kNpcController+0x1e4);
    if(!counter)unhosted(0x2c4f20,"the remote NPC fire gate without +1e4");
    std::uint16_t current=static_cast<std::uint16_t>(m.read(counter+2,2));
    if(npc_fire_gate_reset_){current=0;npc_fire_gate_reset_=false;}
    const auto maximum=static_cast<std::uint16_t>(m.read(counter,2));
    if(current>=maximum)return false;
    m.write(counter+2,2,static_cast<std::uint16_t>(current+1));
    return true;
}
std::optional<LanNetCore::NpcNetMotion> LanNetCore::npc_net_motion(std::uint32_t c) const {
    const auto& m = memory_;
    const auto data = m.read(npc_block(c) + 0x3584);
    if (!data) return std::nullopt;
    NpcNetMotion n;
    n.speed_3c = std::bit_cast<float>(m.read(data + 0x3c));
    n.heading_40 = std::bit_cast<float>(m.read(data + 0x40));
    for (std::uint32_t k = 0; k < 3; ++k) n.position_44[k] = std::bit_cast<float>(m.read(data + 0x44 + k * 4));
    return n;
}

void LanNetCore::npc_net_write_position(std::uint32_t c, const std::array<float, 3>& position) {
    auto& m = memory_;
    const auto data = m.read(npc_block(c) + 0x3584);
    if (!data) unhosted(0x2d5160, "the node teleport's write of +0x44 without the NPC's cNetHuman");
    for (std::uint32_t k = 0; k < 3; ++k) m.write(data + 0x44 + k * 4, 4, std::bit_cast<std::uint32_t>(position[k]));
}

std::uint32_t LanNetCore::npc_controller_150(std::uint32_t c) const {
    return memory_.read(npc_block(c) + kNpcController + 0x150);
}

bool LanNetCore::npc_object_live(std::uint32_t word) {
    if (generator_blocks_.contains(word)) return true;
    auto& m = memory_;
    m.write(kGeneratorKey, 4, word);
    const auto object = call(m, 0x40c540, {kGeneratorKey});
    return object != 0 && m.read(object + 4) == 0xbebaafdeU;
}

bool LanNetCore::npc_remote_controller_001b4f40(std::uint32_t c) {
    auto& m = memory_;
    const auto b = npc_block(c);
    const auto controller = m.read(b + 0x58);
    if (controller) {
        if (call(m, m.read(m.read(controller) + 0x54), {controller}) != 0) return false;   // 001b4f98: the player's
        if ((call(m, m.read(m.read(controller) + 0x88), {controller}) & 0xffU) == 2) return false;   // 001b4fd4
    }
    install_remote_npc_controller(b);   // 001b5010..001b50a8
    return true;
}

void LanNetCore::forget_npc(std::uint32_t c) {
    const auto found = npc_blocks_.find(c);
    if (found == npc_blocks_.end()) return;
    free_npc_blocks_.push_back(found->second);
    npc_blocks_.erase(found);
    npc_generators_.erase(c);
}

void LanNetCore::npc_network_register(std::uint32_t c) {
    auto& m = memory_;
    const auto block = npc_block(c);
    call(m, 0x1856d0, {block});
    const auto local = generator_net_host().local_slot();
    m.write(kGeneratorKey, 4, m.read(block + 0xc));
    call(m, 0x436c00, {kGeneratorNpcInfo});
    call(m, 0x435f30, {kGeneratorNpcInfo, kGeneratorKey, static_cast<std::uint32_t>(local)});
    call(m, 0x436ba0, {kGeneratorNpcInfo, 0xffffffffU});
}

void LanNetCore::send_npc_spawn(const NpcSpawn& spawn) {
    auto& m = memory_;
    const auto gen = generator_block(spawn.generator), block = npc_block(spawn.c);
    const auto caller = sp_;
    sp_ -= 0x100;
    const auto msg = sp_ + 0x40, rows = sp_ + 0xc0;
    const auto f32 = [&m](std::uint32_t at, float v) { m.write(at, 4, std::bit_cast<std::uint32_t>(v)); };
    call(m, 0x3c9e30, {msg, 0x51f058});
    m.write(msg, 4, 0x4de240);
    m.write(msg + 0x50, 4, m.read(block + 0xc));
    m.write(msg + 0x54, 4, m.read(block + 0x10));
    m.write(msg + 0x58, 4, spawn.health);
    for (std::uint32_t r = 0; r < 3; ++r)
        for (std::uint32_t k = 0; k < 4; ++k) f32(rows + r * 16 + k * 4, spawn.rows[r][k]);
    call(m, 0x1bd590, {msg + 0x40, rows});
    for (std::uint32_t k = 0; k < 3; ++k) f32(msg + 0x30 + k * 4, spawn.position[k]);
    f32(msg + 0x3c, 1.0F);
    m.write(msg + 0x5c, 4, spawn.random);
    m.write(msg + 0x60, 4, static_cast<std::uint32_t>(spawn.node));
    m.write(msg + 0x1c, 4, 0x40);
    m.write(msg + 0x20, 1, 4);
    call(m, 0x3c9420, {msg, gen, 0});
    m.write(msg, 4, 0x4de240);
    call(m, 0x3c9d60, {msg, 0});
    sp_ = caller;
    ++spawns_sent_;
}

std::vector<LanNetCore::RemoteSpawn> LanNetCore::take_remote_spawns() {
    std::vector<RemoteSpawn> out;
    out.swap(remote_spawns_);
    return out;
}

// 0021f6c8..0021f7e0 (the header): the message is built on the prelogic's stack at sp+0x40 (PORT-ONLY: destroyed
// here at once, before the despawn's 0040b720 / 003d9f40 / 003cb230 rather than after them; they do not read it).
void LanNetCore::send_npc_despawn(std::uint32_t c) {
    auto& m = memory_;
    const auto generator = npc_generators_.find(c);
    if (generator == npc_generators_.end()) unhosted(0x21f6c8, "the despawn message of an NPC without a generator");
    const auto gen = generator_block(generator->second), block = npc_block(c);
    const auto caller = sp_;
    sp_ -= 0x100;
    const auto msg = sp_ + 0x40;
    call(m, 0x3c9e30, {msg, 0x51f068});
    m.write(msg, 4, 0x4de260);
    m.write(msg + 0x2c, 4, m.read(block + 0xc));
    m.write(msg + 0x30, 4, m.read(block + 0x10));
    m.write(msg + 0x1c, 4, 0x40);
    m.write(msg + 0x20, 1, 4);
    call(m, 0x3c9420, {msg, gen, 0});
    m.write(msg, 4, 0x4de260);
    call(m, 0x3c9d60, {msg, 0});
    sp_ = caller;
    ++despawns_sent_;
}

std::vector<LanNetCore::RemoteDespawn> LanNetCore::take_remote_despawns() {
    std::vector<RemoteDespawn> out;
    out.swap(remote_despawns_);
    return out;
}

// 132ln5h14c (the header).
void LanNetCore::set_health_003cc990(std::uint32_t c, std::int32_t hp) {
    const auto human = npc_block(c);
    const auto caller = sp_;
    sp_ -= 0x200;
    try {
        health_set_003cc990(memory_, *this, human, static_cast<std::uint32_t>(hp), 0, caller);
    } catch (...) {
        sp_ = caller;
        throw;
    }
    sp_ = caller;
    if (memory_.read(dme::kNetworkGame, 1)) ++healths_sent_;
}

std::vector<LanNetCore::RemoteHealth> LanNetCore::take_remote_healths() {
    std::vector<RemoteHealth> out;
    out.swap(remote_healths_);
    return out;
}

// 132ln5h14c (the header): 003ccb00's / 00187b58's 003c9420(msg, actor, 0) of a DamageMsg.
bool LanNetCore::send_damage_message(const DamageSend& d) {
    auto& m = memory_;
    std::uint32_t victim = 0;
    if (d.victim_key == local_player_key()) victim = kLocalPlayer;
    for (const auto& [key, human] : remote_keys_)
        if (key == d.victim_key) victim = human;
    if (!victim) {
        const auto npc = npc_blocks_.find(d.victim_key);
        if (npc == npc_blocks_.end()) unhosted(0x3ccb00, "a DamageMsg to an object of no LAN key (0x" + hex8(d.victim_key) + ")");
        victim = npc->second;
    }
    // PORT-ONLY: the message's +0x28 is the source object; the serialiser sends its +0xc (the LAN key here).
    m.write(kDamageSource + 0xc, 4, d.source_key);
    const auto caller = sp_;
    sp_ -= 0x200;
    const auto msg = sp_ + 0x40;
    damage_queued_ = false;
    sending_damage_ = true;
    try {
        call(m, 0x3c9e30, {msg, 0x5716f0});
        m.write(msg, 4, 0x4e08a0);
        m.write(msg + 0x1c, 4, d.word_1c);
        m.write(msg + 0x20, 1, d.flags_20);
        m.write(msg + 0x24, 4, static_cast<std::uint32_t>(d.value));
        m.write(msg + 0x28, 4, kDamageSource);  // PORT-ONLY: always an object (004083d0 reads its +c)
        m.write(msg + 0x2c, 1, d.type_2c);
        m.write(msg + 0x2d, 1, d.category_2d);
        m.write(msg + 0x2e, 1, d.byte_2e);
        call(m, 0x3c9420, {msg, victim, 0});
        m.write(msg, 4, 0x4e08a0);
        call(m, 0x3c9d60, {msg, 0});
    } catch (...) {
        sending_damage_ = false;
        sp_ = caller;
        throw;
    }
    sending_damage_ = false;
    sp_ = caller;
    if (!damage_queued_) ++damages_sent_;
    return damage_queued_;
}

// 132ln5h14c (the header): 00186e04..00186e58's death action, 003c9420(msg, actor, 1).
void LanNetCore::send_death_action(std::uint32_t key, std::int32_t code) {
    auto& m = memory_;
    std::uint32_t actor = 0;
    if (key == local_player_key()) actor = kLocalPlayer;
    for (const auto& [k, human] : remote_keys_)
        if (k == key) actor = human;
    if (!actor) {
        const auto npc = npc_blocks_.find(key);
        if (npc == npc_blocks_.end()) unhosted(0x186e58, "the death action of an object of no LAN key (0x" + hex8(key) + ")");
        actor = npc->second;
    }
    const auto caller = sp_;
    sp_ -= 0x100;
    const auto msg = sp_ + 0x40;
    try {
        call(m, 0x3c9e30, {msg, 0x55d480});
        m.write(msg, 4, 0x4e01e0);
        m.write(msg + 0x24, 4, 0x42);
        m.write(msg + 0x28, 1, 2);
        m.write(msg + 0x2c, 4, static_cast<std::uint32_t>(code));
        m.write(msg + 0x1c, 4, 0x40);
        m.write(msg + 0x20, 1, 4);  // PORT-ONLY: the constructor's 5 less bit 1 (delivered here by the play)
        call(m, 0x3c9420, {msg, actor, 1});
        m.write(msg, 4, 0x4e0840);
        call(m, 0x3c9d60, {msg, 0});
    } catch (...) {
        sp_ = caller;
        throw;
    }
    sp_ = caller;
    ++deaths_sent_;
}

std::vector<LanNetCore::RemoteDeath> LanNetCore::take_remote_deaths() {
    std::vector<RemoteDeath> out;
    out.swap(remote_deaths_);
    return out;
}

std::vector<LanNetCore::RemoteDamage> LanNetCore::take_remote_damages() {
    std::vector<RemoteDamage> out;
    out.swap(remote_damages_);
    return out;
}

// 132ln5h13 (the header): 00157c44..00157c9c / 002d4554..
void LanNetCore::send_generator_release(std::uint32_t c, std::uint32_t word_150) {
    auto& m = memory_;
    if (!generator_blocks_.contains(word_150)) {
        std::ostringstream o;
        o << "the Generator Message to the live object of +150 0x" << std::hex << word_150 << ", not a generator";
        unhosted(0x157c3c, o.str());
    }
    const auto gen = generator_block(word_150), block = npc_block(c);
    const auto caller = sp_;
    sp_ -= 0x100;
    const auto msg = sp_ + 0x40;
    call(m, 0x3c9e30, {msg, 0x4a4a68});
    m.write(msg, 4, 0x4db600);
    m.write(msg + 0x28, 1, 1);
    m.write(msg + 0x24, 4, block);
    call(m, 0x3c9420, {msg, gen, 0});
    m.write(msg, 4, 0x4db660);
    call(m, 0x3c9d60, {msg, 0});
    sp_ = caller;
    ++generator_releases_sent_;
}

std::vector<LanNetCore::GeneratorRelease> LanNetCore::take_generator_releases() {
    std::vector<GeneratorRelease> out;
    out.swap(generator_releases_);
    return out;
}

std::vector<LanNetCore::NpcControl> LanNetCore::take_npc_controls() {
    std::vector<NpcControl> out;
    out.swap(npc_controls_);
    return out;
}

GeneratorSpawnReceiveHost LanNetCore::generator_spawn_receive_host(ActorKey key) {
    auto& m = memory_;
    GeneratorSpawnReceiveHost host;
    const auto block = [this, key](std::uint32_t actor) { return npc_block(key(actor)); };
    host.set_key_185940 = [this, &m, block](std::uint32_t actor, std::uint32_t k) { call(m, 0x185940, {block(actor), k}); };
    // 0021e380..0021e408: new 0x220, 002d6030(it, 0, actor) (the remote NPC controller, vtable 0x004de2a0; it writes
    // the actor's +14 = 0 and copies the AI controller's +98 / +141 / +150 and its node), 001ae450(actor, 4, ..., it).
    // PORT-ONLY: the controller object is not built; the block's controller becomes 0x004de2a0 (its +54 / +58 / +5c
    // / +88 entries answer the generator paths, as after the control message 2), +14 = 0, and the play holds the
    // NPC's AI (take_npc_controls: kind 2, puppet).
    host.remote_controller = [this, block, key](std::uint32_t actor) {
        install_remote_npc_controller(block(actor));
        npc_control({key(actor), 2, true});
    };
    host.netmap_insert_3fb650 = [this, &m, block](std::uint32_t actor, std::int32_t client) {
        const auto b = block(actor);
        call(m, 0x3fb650, {m.read(b + 0x10), static_cast<std::uint32_t>(client), b});
    };
    host.npc_info_merge_434600 = [this, &m, block](std::uint32_t actor, std::int32_t owner) {
        const auto b = block(actor);
        m.write(kGeneratorKey, 4, m.read(b + 0xc));
        call(m, 0x436c00, {kGeneratorNpcInfo});
        call(m, 0x434600, {kGeneratorNpcInfo, kGeneratorKey, static_cast<std::uint32_t>(owner)});
        call(m, 0x436ba0, {kGeneratorNpcInfo, 0xffffffffU});
    };
    host.attach_185970 = [this, &m, block](std::uint32_t actor) {
        const auto b = block(actor);
        const auto object = call(m, 0x3fbb10, {m.read(b + 0x10)});
        if (object) call(m, 0x185970, {b, object});
    };
    // 0021e530..0021e548: +3598 = 1 and the controller's +0x150 = the generator's +c (register_npc's generator).
    host.mark_remote = [this, &m, key, block](std::uint32_t actor) {
        const auto b = block(actor);
        m.write(b + 0x3598, 4, 1);
        m.write(m.read(b + 0x58) + 0x150, 4, npc_generators_.at(key(actor)));
    };
    return host;
}

void LanNetCore::set_local_player(const std::array<float, 3>& position, const std::array<std::array<float, 3>, 3>& rows,
                                  float health, const std::optional<std::array<float, 4>>& floor_plane) {
    auto& m = memory_;
    const auto p = kLocalPlayer;
    const auto bits = [](float v) { return std::bit_cast<std::uint32_t>(v); };
    m.write(p, 4, 0x4da690);
    m.write(p + 0xc, 4, kLocalPlayerId);
    m.write(p + 0x10, 4, local_player_key());
    m.write(p + 0x2c, 1, 1);
    m.write(p + 0x2d, 1, 1);
    m.write(p + 0x2e, 1, 0);
    m.write(p + 0x40, 4, bits(health));
    m.write(p + 0x50, 4, kPlayerComponent);
    m.write(p + 0x58, 4, kPlayerController);
    m.write(p + 0x3524, 4, local_inventory_ ? kLocalInventory : 0U);  // none yet: the default weapon list
    m.write(kPlayerController, 4, kPlayerControllerVtable);
    m.write(kPlayerComponent, 4, kComponentVtable);
    m.write(kPlayerComponent + 0x50, 1, 0);
    for (std::uint32_t r = 0; r < 3; ++r) {
        for (std::uint32_t c = 0; c < 3; ++c) m.write(kPlayerComponent + 0x10 + r * 16 + c * 4, 4, bits(rows[r][c]));
        m.write(kPlayerComponent + 0x10 + r * 16 + 12, 4, 0);
    }
    for (std::uint32_t c = 0; c < 3; ++c) m.write(kPlayerComponent + 0x40 + c * 4, 4, bits(position[c]));
    m.write(kPlayerComponent + 0x4c, 4, bits(1.0F));
    m.write(0x571770, 4, p);
    m.write(0x571774, 4, kLocalPlayerId);
    m.write(kPlayerController + 0x30, 4, p);  // the controller's human (001770f0 reads it)
    local_floor_plane_ = floor_plane;
    local_set_ = true;
}

void LanNetCore::set_local_packing(const AgentPacking& packing) {
    for (std::uint32_t k = 0; k < packing.size(); ++k) memory_.write(kLocalPlayer + 0xa0 + k, 1, packing[k]);
}

void LanNetCore::set_local_inventory(const std::array<std::int32_t, 6>& items, std::uint8_t held_slot) {
    auto& m = memory_;
    for (std::uint32_t k = 0; k < 6; ++k) m.write(kLocalInventory + k * 16 + 8, 4, static_cast<std::uint32_t>(items[k]));
    m.write(kLocalInventory + 0x84, 1, held_slot);
    m.write(kLocalPlayer + 0x3524, 4, kLocalInventory);
    // PORT-ONLY: the first inventory also writes the held item +6 (002493c0): the remote controller 002d83c0 makes
    // the other console's human hold +6, and the level start's equip that would record it is not hosted (the block's
    // initial value in the original is not decoded).
    if (!local_inventory_) net_record_weapon_002493c0(m, kLocalInput, kLocalPlayer);
    local_inventory_ = true;
}

void LanNetCore::record_lock(std::uint32_t target, std::int32_t subtarget) {
    auto& m = memory_;
    m.write(kScratch, 4, target);
    // PORT-ONLY: the locked object's +0x10 (block +4, which no reader is known to use) is its LAN id too.
    net_record_lock_002494e0(m, kLocalInput, kLocalPlayer, target, kScratch, static_cast<std::uint32_t>(subtarget));
    net_record_aim_mode_00249420(m, kLocalInput, kLocalPlayer, 1);
}

void LanNetCore::record_lower() { net_record_aim_mode_00249420(memory_, kLocalInput, kLocalPlayer, 0); }

void LanNetCore::record_point(const std::array<float, 3>& point) {
    auto& m = memory_;
    for (std::uint32_t c = 0; c < 3; ++c) m.write(kScratch + c * 4, 4, std::bit_cast<std::uint32_t>(point[c]));
    m.write(kScratch + 12, 4, std::bit_cast<std::uint32_t>(1.0F));
    net_record_point_00249490(m, kLocalInput, kLocalPlayer, kScratch);
}

void LanNetCore::record_shot() { net_record_shot_00249530(memory_, kLocalInput, kLocalPlayer); }

void LanNetCore::record_weapon(std::int32_t item) {
    auto& m = memory_;
    if (!local_inventory_) set_local_inventory({-1, -1, -1, -1, -1, -1}, 6);
    // The switch has changed the play's inventory already: the held slot follows the item.
    std::uint32_t slot = 6;
    for (std::uint32_t k = 0; k < 6 && item != -1; ++k)
        if (static_cast<std::int32_t>(m.read(kLocalInventory + k * 16 + 8)) == item) slot = k;
    if (slot == 6 && item != -1) {
        for (std::uint32_t k = 0; k < 6 && slot == 6; ++k)
            if (m.read(kLocalInventory + k * 16 + 8) == 0xffffffffU) slot = k;
        if (slot == 6) slot = 0;
        m.write(kLocalInventory + slot * 16 + 8, 4, static_cast<std::uint32_t>(item));
    }
    m.write(kLocalInventory + 0x84, 1, slot);
    net_record_weapon_002493c0(m, kLocalInput, kLocalPlayer);
}

std::uint32_t LanNetCore::human_key(std::uint32_t human) const {
    for (const auto& [key, h] : remote_keys_)
        if (h == human) return key;
    return 0;
}

std::uint32_t LanNetCore::lan_id(LevelLoadMemory& m, std::uint32_t object) const {
    if (object == kLocalPlayer) return local_player_key();
    if (const auto key = human_key(object)) return key;
    return m.read(object + 0xc);
}

std::vector<LanNetCore::RemoteAction> LanNetCore::take_remote_actions() {
    std::vector<RemoteAction> out;
    out.swap(remote_actions_);
    return out;
}

std::vector<LanNetCore::RemoteAim> LanNetCore::take_remote_aims() {
    std::vector<RemoteAim> out;
    out.swap(remote_aims_);
    return out;
}

std::vector<LanNetCore::RemoteWeapon> LanNetCore::take_remote_weapons() {
    std::vector<RemoteWeapon> out;
    out.swap(remote_weapons_);
    return out;
}

std::vector<LanNetCore::RemoteHuman> LanNetCore::take_remote_humans() {
    std::vector<RemoteHuman> out;
    out.swap(remote_events_);
    return out;
}

// PORT-ONLY: 001b8130(human, matrix, -1, 0, health, sp+0x600, 3, loader; $sp+0 the key's address) from 002c6b70.
std::uint32_t LanNetCore::human_constructor_001b8130(LevelLoadMemory& m, const std::vector<std::uint32_t>& a,
                                                      std::uint32_t sp) {
    if (a.size() != 8) unhosted(0x1b8130, "a human constructor without its eight register arguments");
    const auto human = a[0], matrix = a[1];
    const auto key = m.read(m.read(sp));
    const auto controller = call(m, 0x36b180, {0, 0x200, 0, 0});
    m.write(human, 4, kHumanVtable);
    m.write(human + 0xc, 4, key);  // the base constructors' +0xc: the key (001b2e90 takes the client from it)
    m.write(human + 0x10, 4, key);  // 132ln5h14c: +0x10 too (003c9420's 003fb020 / 003fb340 key, as the NPC blocks')
    m.write(human + 0x50, 4, kRemoteComponent);
    m.write(human + 0x58, 4, controller);
    m.write(controller, 4, kRemoteControllerVtable);
    // 132ln5h: the inventory (0x84 = 6: nothing held; the six slots empty). The network block +0x3584 is the remote
    // human's cNetHuman data, linked by 00185970 below (002c6b70) or by 002d1f00 when the object arrives later.
    const auto inventory = call(m, 0x36b180, {0, 0x100, 0, 0});
    for (std::uint32_t k = 0; k < 6; ++k) m.write(inventory + k * 16 + 8, 4, 0xffffffffU);
    m.write(inventory + 0x84, 1, 6);
    m.write(human + 0x3524, 4, inventory);
    m.write(human + 0x3584, 4, 0);
    m.write(human + 0x2c, 1, 1);  // the root bytes 002d83c0 checks of a target: active, eligible, not dead
    m.write(human + 0x2d, 1, 1);
    m.write(human + 0x2e, 1, 0);
    remote_keys_.emplace_back(key, human);
    RemoteHuman event;
    event.client = remote_client_;
    event.key = key;
    event.human = human;
    for (std::uint32_t k = 0; k < 16; ++k) event.matrix[k] = std::bit_cast<float>(m.read(matrix + 4 * k));
    remote_positions_[human] = {event.matrix[12], event.matrix[13], event.matrix[14]};  // 132ln5h11, PORT-ONLY
    event.health = static_cast<std::int32_t>(a[4]);
    // 132ln5h6: the descriptor (+0x18 of the stack arguments: 002c6b70's sp + 0x160, which 00406ec0 filled).
    if (const auto descriptor = m.read(sp + 0x18); descriptor && part_cache_) {
        AgentPacking packing{};
        for (std::uint32_t k = 0; k < packing.size(); ++k) packing[k] = static_cast<std::uint8_t>(m.read(descriptor + k, 1));
        event.packing = packing;
    }
    remote_events_.push_back(event);
    ++remote_created_;
    char text[160];
    std::snprintf(text, sizeof text, "002c6b70: remote human key %08x from client %u at (%.2f %.2f %.2f) health %d", key,
                  static_cast<unsigned>(remote_client_), event.matrix[12], event.matrix[13], event.matrix[14], event.health);
    log_.push_back(text);
    return human;
}

void LanNetCore::run_thread() {
    if (!thread_ || thread_->exited()) unhosted(0x10e010, "the DME thread is not running");
    if (running_) unhosted(0x10e010, "the DME thread woken from itself");
    running_ = true;
    sleeping_ = false;
    ++thread_runs_;
    const auto main_sp = sp_;
    sp_ = kThreadStack;
    const bool sleeps = thread_->resume(memory_, *this);
    // ExitDeleteThread removes the kernel object. A later 0042a770 (World_Teardown) may create another one.
    if (!sleeps && thread_->exited()) thread_.reset();
    sp_ = main_sp;
    running_ = false;
    sleeping_ = sleeps;
}

std::uint32_t LanNetCore::kernel(std::uint32_t address, const std::vector<std::uint32_t>& a) {
    const auto arg = [&](std::size_t i) { return i < a.size() ? a[i] : 0U; };
    switch (address) {
    case 0x10cff0:  // ChangeThreadPriority (thread 0: the calling thread, the main one here)
        if (arg(0) == 0) main_priority_ = arg(1);
        else if (arg(0) == kDmeThreadId) dme_priority_ = arg(1);
        else unhosted(address, "ChangeThreadPriority of an unknown thread");
        return 0;
    case 0x10cf60:  // CreateThread
        if (thread_) unhosted(address, "a second DME thread");
        if (memory_.read(arg(0) + 4) != dme::kThreadEntry) unhosted(address, "a thread entry other than 004295e0");
        thread_ = std::make_unique<DmeThreadBody>();
        dme_priority_ = memory_.read(arg(0) + 0x14);
        return kDmeThreadId;
    case 0x10cf80:  // StartThread: the new thread outranks the main thread and runs at once
        if (arg(0) != kDmeThreadId || !thread_) unhosted(address, "StartThread of an unknown thread");
        if (dme_priority_ >= main_priority_) unhosted(address, "a DME thread that does not outrank the main thread");
        run_thread();
        return 0;
    case 0x10d060: {  // ReferThreadStatus: the code reads the current priority (+0x18)
        std::uint32_t priority = 0;
        if (arg(0) == 0) priority = main_priority_;
        else if (arg(0) == kDmeThreadId && thread_) priority = dme_priority_;
        else unhosted(address, "ReferThreadStatus of an unknown thread");
        memory_.write(arg(1) + 0x18, 4, priority);
        return 0;
    }
    case 0x10e010:  // iWakeupThread
        if (arg(0) != kDmeThreadId) unhosted(address, "iWakeupThread of an unknown thread");
        if (sleeping_) run_thread();
        return kDmeThreadId;
    case 0x10e1c0:  // DelayThread: the woken DME thread has run already
        return 0;
    case 0x10d080:  // SleepThread: the body yields after this call (DmeThreadBody)
    case 0x1161f8:  // EI
    case 0x1161a0:  // DI
        return 0;
    case 0x10cfa0:  // ExitDeleteThread
        return 0;
    case 0x10ce40:  // AddIntcHandler(9, 004292b0, 0)
        if (arg(0) != 9 || arg(1) != dme::kTimerInterrupt) unhosted(address, "an interrupt handler other than TIM0's");
        return 1;
    case 0x10dca8:  // EnableIntc(9)
    case 0x10dc40:  // DisableIntc(9) (0016a0a0's heap guard; the timer events come only from advance_timer)
        return 0;
    default:
        unhosted(address, "an EE kernel call");
    }
}

std::uint32_t LanNetCore::game_alloc(std::uint32_t size) {
    const std::uint32_t need = std::max<std::uint32_t>(16, (size + 15) & ~15U);
    for (auto it = game_free_.begin(); it != game_free_.end(); ++it) {
        if (it->second < need) continue;
        const auto block = it->first, left = it->second - need;
        game_free_.erase(it);
        if (left) game_free_.emplace(block + need, left);
        game_live_.emplace(block, need);
        return block;
    }
    const auto block = game_heap_;
    if (block + need > kGameHeapEnd) unhosted(0x36b180, "the port's game heap arena is full");
    game_heap_ = block + need;
    game_live_.emplace(block, need);
    return block;
}

void LanNetCore::game_free(std::uint32_t block) {
    if (block == 0) return;
    const auto live = game_live_.find(block);
    if (live == game_live_.end()) unhosted(0x36b130, "a free of a block the game heap did not hand out (" + hex8(block) + ")");
    auto at = block;
    auto size = live->second;
    game_live_.erase(live);
    // Merge with the free neighbours, then give a block at the top back to the unused end.
    auto next = game_free_.find(at + size);
    if (next != game_free_.end()) {
        size += next->second;
        game_free_.erase(next);
    }
    auto prev = game_free_.lower_bound(at);
    if (prev != game_free_.begin()) {
        --prev;
        if (prev->first + prev->second == at) {
            at = prev->first;
            size += prev->second;
            game_free_.erase(prev);
        }
    }
    if (at + size == game_heap_) game_heap_ = at;
    else game_free_.emplace(at, size);
}

std::uint32_t LanNetCore::call(LevelLoadMemory& m, std::uint32_t address, std::initializer_list<std::uint32_t> list) {
    const std::vector<std::uint32_t> a(list);
    const auto arg = [&](std::size_t i) { return i < a.size() ? a[i] : 0U; };
    struct Frame {  // the caller's $sp for a native function; its callees go lower
        std::uint32_t& sp;
        std::uint32_t caller;
        explicit Frame(std::uint32_t& s, std::uint32_t size = 0x200) : sp(s), caller(s) { sp -= size; }
        ~Frame() { sp = caller; }
    };
    switch (address) {
    case 0x165bc0: { Frame f(sp_); net_start_00165bc0(m, *this); return 0; }
    case 0x3fb250: { Frame f(sp_); net_register_all_003fb250(m, *this); return 0; }
    case 0x42af00: { Frame f(sp_); dme_init_0042af00(m, *this, f.caller); return 0; }
    // PORT-ONLY stand-ins for what the DME init reaches below the library (research/NET_INLEVEL.md section 4.3):
    case 0x3a6710: return 0;  // the IRX module loader (00165bc0's 003a6710(1)): a LAN game loads no IOP modules
    case 0x2caa70: {          // the game's allocator 002ea4f0 hands the DME: a bump arena, 16-byte aligned
        const auto p = heap_;
        heap_ = (p + arg(0) + 15) & ~15U;
        if (heap_ > kHeapEnd) unhosted(address, "the port's DME allocation arena is full");
        return p;
    }
    case 0x2ca9b0: return 0;  // its free (002dd328 from the object deletes 002dea68): the bump arena keeps the block
    case 0x3021d0: case 0x302260: case 0x3023d8: case 0x302218:  // the library lock (EE semaphores): one thread at a time here
    case 0x302088:  // the IOP-side RPC bind of the network stack
    case 0x2fb788:  // the network stack's start (the DME key, none in LAN)
    case 0x2fc088:  // the local address text (0x0058808c stays empty: LanTransport has the addresses)
        return 0;
    case 0x3060c0:  // the network stack's clock into *a0 (0: LanDme keeps the clocks)
        m.write(arg(0), 4, 0);
        return 0;
    case 0x429d30: { Frame f(sp_); return dme_update_00429d30(m, *this, f.caller); }
    case 0x42a770: { Frame f(sp_); return dme_thread_start_0042a770(m, *this, f.caller); }
    case 0x42a1b0: { Frame f(sp_); dme_stop_thread_and_sync_0042a1b0(m, *this, f.caller); return 0; }
    case 0x42a680: {  // 0042a680 stops the update thread without 0042a1b0's session barrier.
        Frame f(sp_);
        const auto status = f.caller - 0xe0;
        call(m, 0x10d060, {m.read(dme::kThreadId), status + 0x60});
        call(m, 0x10d060, {m.read(dme::kMainThread), status + 0x30});
        if (static_cast<std::int32_t>(m.read(status + 0x78)) <=
            static_cast<std::int32_t>(m.read(status + 0x48)))
            call(m, 0x10cff0, {m.read(dme::kThreadId), 0});
        m.write(dme::kThreadStop, 1, 1);
        const auto woken = call(m, 0x10e010, {m.read(dme::kThreadId)});
        call(m, 0x10e1c0, {10});
        if (woken == m.read(dme::kThreadId)) {
            while (!m.read(dme::kThreadDone, 1)) call(m, 0x10e1c0, {10});
        }
        m.write(dme::kThreadStop, 1, 0);
        m.write(dme::kThreadStarted, 1, 0);
        m.write(dme::kThreadWake, 4, 0);
        m.write(dme::kThreadDone, 1, 0);
        m.write(dme::kIntcInstalled, 1, 0);
        return 0;
    }
    case 0x42e6d0: {  // cNetObjectMgr_Reset: only the five empty transfer maps are hosted.
        for (const auto map : {0x5826a0U, 0x5826c0U, 0x5826e0U, 0x582700U, 0x582720U}) {
            if (m.read(map) || m.read(map + 4) || m.read(map + 0xc) != map + 4)
                unhosted(address, "a nonempty transfer-object, pending-message or reference map");
        }
        return 0;
    }
    case 0x3e56f0: {  // cCharActionMsg_DestinationParams_ClearPrevMsg
        const auto last = m.read(0x55a228);
        if (!last) return 0;
        if (m.read(last) != 0x4e0100 || !game_live_.contains(last))
            unhosted(address, "a previous locomotion message outside the game heap or of another class");
        call(m, m.read(m.read(last) + 8), {last, 0xffffffffU});
        call(m, 0x36b130, {0, last, 0x4bdd08, 0x37b});
        m.write(0x55a228, 4, 0);
        return 0;
    }
    case 0x42b2d0: {  // cDME_Restart: the library object's release is a separate, guarded service below.
        m.write(0x5721d0, 1, 0);
        m.write(0x5723f8, 1, 0);
        m.write(0x572408, 1, 1);
        m.write(0x572400, 1, 0);
        m.write(0x497540, 1, 0);
        const auto result = call(m, 0x2ec340, {});
        m.write(0x5721b8, 1, 1);
        return result;
    }
    case 0x2ec340: {  // NetFreeAllObjects / NetIFreeAllObjects (002df8e8).
        if (m.read(0x48c4d7, 1) != 1) return 0x11;
        if (m.read(0x48c4dc, 1) != 1) return 0x2b;
        // Preflight all live handles: 002dea68 invokes the registered remove callback before deleting an
        // object's buffers. Other registered classes (notably GameGOBJ's 00210780 / 00212040 / 002128a0)
        // remain unhosted, so none of the earlier handles may be freed when one of those is present.
        for (std::uint32_t i = 0; i < 0x1000; ++i) {
            if (m.read(0x58c178 + i, 1) != 2) continue;
            const auto obj = m.read(0x5880b0 + 4 * i);
            if (!obj || m.read(0x48c450) >= 4)
                unhosted(address, "a live handle without an object in the current DME world");
            const auto cls = m.read(obj, 1);
            if (cls >= 16) unhosted(address, "a live object with an unregistered class");
            const auto cb = m.read(0x58c138 + 4 * cls);
            if (cb && cb != 0x2d1d50 && cb != 0x16a450 && cb != 0x438c20 && cb != 0x4321d0 &&
                cb != 0x4375d0 && cb != 0x2c7b20)
                unhosted(address, "a live object's unhosted remove callback 0x" + hex8(cb));
        }
        if (call(m, 0x302260, {0x5253f0, 0})) return 0x2b;  // rt_mutex_lock
        for (std::uint32_t i = 0; i < 0x1000; ++i)
            if (m.read(0x58c178 + i, 1) == 2) call(m, 0x2dea68, {i});
        for (std::uint32_t i = 0; i < 0x1000; ++i) m.write(0x58c178 + i, 1, 0);
        if (call(m, 0x3023d8, {0x5253f0, 0})) return 0x2b;  // rt_mutex_unlock
        return 0;
    }
    case 0x44e340:
        if (arg(0) != kLobbySession) unhosted(address, "a lobby other than LanSession's");
        if (!lobby_exit_) unhosted(address, "SFOLobby_ExitGame has no caller lobby-exit service");
        lobby_exit_();
        return 0;
    case 0x2c7b20: return 0;  // tNetGenerator_NetGeneratorDeleteCallback is empty in the original.
    case 0x437bf0: dme_voice_end_00437bf0(m, *this); return 0;
    case 0x42b230: dme_session_reset_0042b230(m); return 0;
    case 0x42b1f0: dme_session_end_0042b1f0(m, *this); return 0;
    case 0x3fbcd0: { Frame f(sp_); net_objects_clear_003fbcd0(m, *this, f.caller); return 0; }
    case 0x42b0c0: { Frame f(sp_); dme_level_net_start_0042b0c0(m, *this); return 0; }
    case 0x42b1a0: { Frame f(sp_); return dme_clock_0042b1a0(m, *this, arg(0), f.caller); }
    case 0x3fac40: { Frame f(sp_); dme_send_network_data_003fac40(m, *this, arg(0)); return 0; }
    case 0x43a810: { Frame f(sp_); dme_geninfo_update_0043a810(m, *this, f.caller); return 0; }
    case 0x436c00: { Frame f(sp_); return npc_info_update_00436c00(m, *this, arg(0)); }
    case 0x436380: { Frame f(sp_); npc_info_clear_00436380(m, *this, arg(0)); return 0; }
    case 0x4355d0: { Frame f(sp_); npc_info_objects_004355d0(m, *this, arg(0)); return 0; }
    case 0x433a60: { Frame f(sp_); npc_info_objects_create_00433a60(m, *this, f.caller); return 0; }
    case 0x434740: { Frame f(sp_); npc_info_send_timers_00434740(m, *this); return 0; }
    case 0x4352c0: { Frame f(sp_); npc_info_send_changed_004352c0(m, *this, f.caller); return 0; }
    case 0x436ba0: { Frame f(sp_); return npc_info_release_00436ba0(m, *this, arg(0), arg(1)); }
    case 0x129a70: {  // strncpy
        bool ended = false;
        for (std::uint32_t k = 0; k < arg(2); ++k) {
            const auto b = ended ? 0U : m.read(arg(1) + k, 1);
            if (b == 0) ended = true;
            m.write(arg(0) + k, 1, b);
        }
        return arg(0);
    }
    case 0x127358:
    case 0x10f388: {
        std::string line = "printf " + hex8(arg(0));
        for (std::size_t k = 1; k < a.size(); ++k) line += " " + hex8(a[k]);
        log_.push_back(line);
        return 0;
    }
    case 0x16a970: { Frame f(sp_); run_team_setup(m, *this); return 0; }
    case 0x16aa80: { Frame f(sp_); run_team_teardown(m, *this); return 0; }
    case 0x16a0a0: { Frame f(sp_); team_data_object_0016a0a0(m, *this, f.caller); return 0; }
    case 0x36b180:  // PORT-ONLY: the game heap (0036b180(0, size, file, line)), 16-byte aligned blocks
        return game_alloc(arg(1));
    case 0x36b130:  // PORT-ONLY: the game heap's free (0036b130(0, block, file, line))
        ++game_frees_;
        game_free(arg(1));
        return 0;
    case 0x3fa780: return net_map_take_003fa780(m, arg(0), arg(1), arg(2));
    case 0x3fa8c0: { Frame f(sp_); net_map_set_003fa8c0(m, *this, arg(0), arg(1), arg(2), arg(3)); return 0; }
    case 0x3fb650: { Frame f(sp_); return net_map_set_object_003fb650(m, *this, arg(0), arg(1), arg(2)); }
    case 0x3fa940: { Frame f(sp_); net_client_loaded_003fa940(m, *this, arg(0)); return 0; }
    case 0x3fbec0: { Frame f(sp_); net_frame_003fbec0(m, *this, f.caller); return 0; }
    case 0x169390: { Frame f(sp_); net_update_00169390(m, *this); return 0; }
    case 0x472080: { Frame f(sp_); net_throttle_send_00472080(m, *this); return 0; }
    case 0x2ca580: { Frame f(sp_); lobby_keepalive_002ca580(m, *this, arg(0)); return 0; }
    case 0x4294c0:  // 004294c0: *0x00572480 while byte 0x00572488 is set (the sentinel session, see set_lobby)
        return m.read(0x572480) && m.read(0x572488, 1) ? m.read(0x572480) : 0;
    case 0x4536b0:  // PORT-ONLY: SFOLobby's update of the session: LanSession's discovery
        if (arg(0) != kLobbySession) unhosted(address, "a lobby session that is not LanSession's");
        if (lobby_) lobby_();
        return 0;
    case 0x3fbeb0: return 0;  // an empty function (jr $ra) that 00429d30 calls in the level (0x005724d0 set)
    case 0x452fd0: return 0;  // PORT-ONLY: the session's disconnect check (+0x2564 == 5): a LAN lobby has no server to lose
    case 0x43b190: { Frame f(sp_); return geninfo_create_0043b190(m, *this, arg(0)); }
    case 0x439670: { Frame f(sp_); geninfo_objects_00439670(m, *this, f.caller); return 0; }
    case 0x43aa30:  // j 0043a8b0
    case 0x43a8b0: { Frame f(sp_); geninfo_send_timer_0043a8b0(m, *this, f.caller); return 0; }
    case 0x42eb20: { Frame f(sp_); net_objects_update_0042eb20(m, *this); return 0; }
    case 0x42cf30: { Frame f(sp_); net_objects_send_0042cf30(m, *this, f.caller); return 0; }
    case 0x438770: { Frame f(sp_); voice_frame_00438770(m, *this); return 0; }
    case 0x438340: { Frame f(sp_); return voice_headset_00438340(m, *this, f.caller); }
    case 0x3241c8:  // PORT-ONLY: lgaud.c's init; LGAUD.IRX is not loaded (003a6710), the voice chat is out of scope
        return 0xffffffffU;
    case 0x40bf10: return local_player_0040bf10(m);  // the play's player once set_local_player ran (132ln5f)
    case 0x185840: { Frame f(sp_); net_create_characters_00185840(m, *this, f.caller); return 0; }
    case 0x3fa5d0: { Frame f(sp_); net_load_network_data_003fa5d0(m, *this, f.caller); return 0; }
    case 0x1350e0:  // the level load
        if (!level_) unhosted(address, "the level load of a network game (005721c8 set) is not hosted by the port's play");
        return level_->call(m, address, list);
    // 132ln5f: the characters (the header).
    case 0x185a30: { Frame f(sp_, 0x1000); return player_send_00185a30(m, *this, arg(0), arg(1), f.caller); }
    case 0x2c72a0: {
        Frame f(sp_, 0x1000);
        return send_character_002c72a0(m, *this, arg(0), arg(1), arg(2), arg(3), arg(4), arg(5), arg(6), f.caller);
    }
    case 0x3fad50: { Frame f(sp_); return net_send_003fad50(m, *this, arg(0), arg(1), arg(2), arg(3), arg(4)); }
    case 0x2c7630: { Frame f(sp_); return recv_named_character_002c7630(m, *this, arg(2), arg(3)); }
    case 0x2c7810: { Frame f(sp_); return recv_character_002c7810(m, *this, arg(2), arg(3)); }
    case 0x2c6b70: {  // NetCreateCharacterMsg.cc: a remote human (from 002c7810 in the level, 00185840 at the load)
        Frame f(sp_, 0x1000);
        const auto prior = remote_client_;
        remote_client_ = static_cast<std::uint16_t>(arg(1));
        create_remote_human_002c6b70(m, *this, arg(0), arg(1), f.caller);
        remote_client_ = prior;
        return 0;
    }
    case 0x2c6690:  // NetCreateNamedCharacterMsg.cc: a remote named character (an NPC); not ported
        unhosted(address, "a remote named character 002c6690 (NetCreateNamedCharacterMsg) is not ported");
    case 0x185940: human_set_key_00185940(m, arg(0), arg(1)); return 0;
    case 0x185970: { Frame f(sp_); human_set_net_entry_00185970(m, *this, arg(0), arg(1)); return 0; }
    case 0x2de398:  // the DME send under 002de880 (the native library's game messages)
        return dme_->send_message(m, arg(0), arg(1), arg(2), arg(3), arg(4), arg(5), arg(6));
    case 0x1bd590: {  // the rotation rows' Euler angles (camera_matrix_euler_1bd590) into a0
        std::array<RootVector, 3> rows{};
        for (std::uint32_t r = 0; r < 3; ++r)
            for (std::uint32_t c = 0; c < 4; ++c) rows[r][c] = std::bit_cast<float>(m.read(arg(1) + r * 16 + c * 4));
        const auto e = camera_matrix_euler_1bd590(rows);
        for (std::uint32_t c = 0; c < 3; ++c) m.write(arg(0) + c * 4, 4, std::bit_cast<std::uint32_t>(e[c]));
        return arg(0);
    }
    case 0x4073c0: {  // 132ln5h6: the compact appearance into the scratchpad (n at a1)
        if (!part_cache_) {  // PORT-ONLY: no level part cache: none
            m.write(arg(1), 4, 0);
            return arg(0);
        }
        AgentPacking packing{};
        for (std::uint32_t k = 0; k < packing.size(); ++k) packing[k] = static_cast<std::uint8_t>(m.read(arg(0) + k, 1));
        const auto compact = agent_packing_compact(*part_cache_, packing);
        constexpr std::uint32_t scratchpad = 0x70000000;
        for (std::uint32_t k = 0; k < compact.size(); ++k) m.write(scratchpad + k, 1, compact[k]);
        m.write(arg(1), 4, static_cast<std::uint32_t>(compact.size()));
        return scratchpad;
    }
    case 0x1260f0:  // memset
        for (std::uint32_t k = 0; k < arg(2); ++k) m.write(arg(0) + k, 1, arg(1) & 0xffU);
        return arg(0);
    case 0x1294f8: {  // strlen
        std::uint32_t n = 0;
        while (m.read(arg(0) + n, 1)) ++n;
        return n;
    }
    case 0x40b6d0: {  // PORT-ONLY: the character list's lookup by key: the remote humans made here
        if (arg(0) != kCharacters) unhosted(address, "a character list other than the port's");
        const auto key = m.read(arg(1));
        for (const auto& [k, human] : remote_keys_)
            if (k == key) return human;
        return 0;
    }
    // 132ln5h: the remote human's controller (the header).
    case 0x2d85d0: { Frame f(sp_); return remote_controller_construct_002d85d0(m, *this, arg(0), arg(1), arg(2)); }
    case 0x230240: { Frame f(sp_); return aim_component_construct_00230240(m, *this, arg(0)); }
    case 0x2d83c0: { Frame f(sp_); remote_controller_update_002d83c0(m, *this, arg(0), f.caller); return 0; }
    case 0x2d7c70: return 1;
    case 0x2d7c60: return 4;
    case 0x1ae450:  // PORT-ONLY: the human's controller hand-over (mode 3 from 002c6b70): +0x58 = the controller
        if (arg(1) != 3) unhosted(address, "a controller hand-over of a mode other than 3");
        m.write(arg(0) + 0x58, 4, arg(3));
        return 0;
    case 0x22f5b0: {  // PORT-ONLY: the aim accumulator (the play's aim update runs it): a RemoteAim
        RemoteAim aim;
        aim.human = m.read(arg(0) + 0x4c);
        aim.key = human_key(aim.human);
        aim.target = lan_id(m, arg(1));
        aim.item = static_cast<std::int32_t>(arg(2));
        aim.subtarget = static_cast<std::int32_t>(arg(3));
        remote_aims_.push_back(aim);
        return 0;
    }
    case 0x40c540: {  // PORT-ONLY: the object of an id (LAN ids, the header)
        const auto id = m.read(arg(0));
        if (id == 0 || id == 0xffffffffU) return 0;
        if (id == local_player_key()) return kLocalPlayer;
        for (const auto& [key, human] : remote_keys_)
            if (key == id) return human;
        if (id >> 24) {  // a client's key (the top byte the client + 1): an NPC block (132ln5h12), else none yet
            const auto npc = npc_blocks_.find(id);
            return npc == npc_blocks_.end() ? 0U : npc->second;
        }
        m.write(kNpcProxy, 4, kProxyVtable);
        m.write(kNpcProxy + 0xc, 4, id);
        m.write(kNpcProxy + 0x2c, 1, 1);
        m.write(kNpcProxy + 0x2d, 1, 1);
        m.write(kNpcProxy + 0x2e, 1, 0);
        return kNpcProxy;
    }
    // 132ln5h3: the actor messages and the locomotion replication (research/NET_INLEVEL.md 4.14).
    case 0x3c9e30: { Frame f(sp_); return message_construct_003c9e30(m, *this, arg(0), arg(1)); }
    case 0x3c9d60: { Frame f(sp_); return message_destroy_003c9d60(m, *this, arg(0), arg(1)); }
    case 0x14d270: { Frame f(sp_); return move_message_destroy_0014d270(m, *this, arg(0), arg(1)); }
    case 0x3e5d70: { Frame f(sp_); return move_message_clone_003e5d70(m, *this, arg(0)); }
    case 0x3e57b0: {
        Frame f(sp_);
        const auto r = move_message_serialise_003e57b0(m, *this, arg(0), arg(1), arg(2), arg(3), arg(4), arg(5), arg(6));
        if (r == 0x28) ++moves_sent_;
        return r;
    }
    case 0x428cd0: {  // cActivateMsg_Pack (00428cd0), 0x10-byte packet
        if (arg(5) < 8 || !arg(6)) return 0xffffffffU;
        const auto transport = static_cast<std::int32_t>(m.read(arg(0) + 0x18));
        m.write(arg(6), 4, call(m, 0x42b1a0, {0}) << 2 |
                                 (transport < 4 ? static_cast<std::uint32_t>(transport) & 3U : 3U));
        m.write(arg(6) + 4, 4, arg(1));
        if (arg(5) <= 0x10) return 0xffffffffU;
        m.write(arg(6) + 8, 4, m.read(arg(0) + 0x24));
        m.write(arg(6) + 0xc, 1, m.read(arg(0) + 0x28, 1));
        if (arg(2)) m.write(arg(2), 4, m.read(0x572190));
        return arg(2) ? 0x10U : 0xffffffffU;
    }
    case 0x429150: {  // cDeactivateMsg_Pack (00429150), only the header
        if (arg(5) < 8 || !arg(6) || !arg(2)) return 0xffffffffU;
        const auto transport = static_cast<std::int32_t>(m.read(arg(0) + 0x18));
        m.write(arg(6), 4, call(m, 0x42b1a0, {0}) << 2 |
                                 (transport < 4 ? static_cast<std::uint32_t>(transport) & 3U : 3U));
        m.write(arg(6) + 4, 4, arg(1));
        m.write(arg(2), 4, m.read(0x5721a0));
        return 8;
    }
    case 0x20e4c0: {  // cEndLevelMsg_Pack: direct 16-byte category-2 packet
        if (arg(5) < 16 || !arg(6)) return 0xffffffffU;
        const auto transport = static_cast<std::int32_t>(m.read(arg(0) + 0x18));
        m.write(arg(6), 4, call(m, 0x42b1a0, {0}) << 2 |
                                 (transport < 4 ? static_cast<std::uint32_t>(transport) & 3U : 3U));
        m.write(arg(6) + 4, 4, arg(1));
        m.write(arg(6) + 8, 1, m.read(arg(0) + 0x24, 1));
        m.write(arg(6) + 0xc, 4, m.read(arg(0) + 0x28));
        if (arg(2)) m.write(arg(2), 4, m.read(0x4f53a0));
        return arg(2) ? 16U : 0xffffffffU;
    }
    case 0x2d7580: {  // cNetObjectiveMsg_Pack: header, action, stage/value, and 16-byte name
        if (arg(5) < 0x24 || !arg(6)) return 0xffffffffU;
        // Event_PackHeader (003c9ca0): clock/transport word and target NetObj key.
        const auto transport = static_cast<std::int32_t>(m.read(arg(0) + 0x18));
        m.write(arg(6), 4, call(m, 0x42b1a0, {0}) << 2 |
                                 (transport < 4 ? static_cast<std::uint32_t>(transport) & 3U : 3U));
        m.write(arg(6) + 4, 4, arg(1));
        m.write(arg(6) + 8, 1, m.read(arg(0) + 0x2c, 1));
        m.write(arg(6) + 0xc, 4, m.read(arg(0) + 0x30));
        m.write(arg(6) + 0x10, 4, m.read(arg(0) + 0x34));
        bool ended = false;
        for (std::uint32_t i = 0; i < 16; ++i) {
            const auto byte = ended ? 0U : m.read(arg(0) + 0x38 + i, 1);
            m.write(arg(6) + 0x14 + i, 1, byte);
            if (byte == 0) ended = true;  // 002d7580 calls strncpy(..., 0x10)
        }
        if (arg(2)) m.write(arg(2), 4, m.read(0x51f0b8));
        return arg(2) ? 0x24U : 0xffffffffU;
    }
    case 0x3fb020: { Frame f(sp_, 0x600); return net_message_send_003fb020(m, *this, arg(0), arg(1), arg(2), arg(3), f.caller); }
    case 0x3c9350: { Frame f(sp_); return actor_message_deliver_003c9350(m, *this, arg(0), arg(1), arg(2), f.caller); }
    case 0x3e4430: {
        Frame f(sp_);
        ++moves_received_;
        return move_message_receive_003e4430(m, *this, arg(3), f.caller);
    }
    case 0x2d7620: return receive_objective_message(arg(3));
    case 0x2822f0: return receive_generic_message(arg(2), arg(3));
    case 0x20edd0: return receive_player_sound(arg(2), arg(3), false);
    case 0x20e8b0: return receive_player_sound(arg(2), arg(3), true);
    case 0x20e350: return receive_end_level(arg(2), arg(3));
    case 0x12ed00: return 1;  // matched cSystem_SendCreated: no state packet
    case 0x428ef0: return receive_objective_active(arg(3), true);
    case 0x429190: return receive_objective_active(arg(3), false);
    case 0x1770f0: { Frame f(sp_); player_net_update_001770f0(m, *this, arg(0), arg(1), f.caller); return 0; }
    case 0x184cc0: return 1;  // the actor's virtual +0x18: networked
    case 0x132280: {  // PORT-ONLY: the player's placement (virtual +0xf4) into a0: the component's +0x10 matrix
        if (arg(1) != kLocalPlayer) unhosted(address, "the placement of an actor other than the local player");
        for (std::uint32_t k = 0; k < 0x40; k += 4) m.write(arg(0) + k, 4, m.read(kPlayerComponent + 0x10 + k));
        return arg(0);
    }
    case 0x14d3d0: {  // PORT-ONLY: the player's floor plane (virtual +0xf8, physical +0xf0 while +0xe8): the play's
        if (arg(0) != kLocalPlayer) unhosted(address, "the floor plane of an actor other than the local player");
        if (!local_floor_plane_) return 0;
        for (std::uint32_t c = 0; c < 4; ++c) m.write(arg(1) + c * 4, 4, std::bit_cast<std::uint32_t>((*local_floor_plane_)[c]));
        return 1;
    }
    case 0x13e020:  // PORT-ONLY: the player's flagged floor object (virtual +0x110): none (the play's floor records)
        return 0;
    case 0x24b5d0: case 0x24aaf0: case 0x261540:  // PORT-ONLY: 001770f0's tail (the weapon / aim updates): the play's
        return 0;
    case 0x3ca0e0: return queue_message_003ca0e0(m, arg(3));
    case 0x3c9420: {  // an actor message: the locomotion message natively; PORT-ONLY: else an action as a RemoteAction
        if (m.read(arg(0)) == 0x4e0100 || m.read(arg(0)) == 0x4de240 || m.read(arg(0)) == 0x4db600 ||
            m.read(arg(0)) == 0x4de260 || m.read(arg(0)) == 0x4e0ee0 || m.read(arg(0)) == 0x4e08a0 ||
            m.read(arg(0)) == 0x4e01e0) {  // 132ln5h12 / h13 / h14 / h14c
            Frame f(sp_);
            return actor_message_send_003c9420(m, *this, arg(0), arg(1), arg(2), f.caller);
        }
        const auto msg = arg(0);
        // 132ln5h4: the network section's action 0x43 (kind 1, to the remote human at once; 001a92a0 -> 001ad5e0:
        // the physical's +0x12 / +0x13): the byte +0x2c for the play (RemoteAnimation::physical_12).
        if (m.read(msg + 0x24) == 0x43 && m.read(msg + 0x28, 1) == 1) {
            // 132ln5h14b: the section on an NPC's block (npc_section_001b1710): the byte for the play's NPC.
            if (section_npc_ && arg(1) == section_human_) {
                pending_physical_ = static_cast<std::uint8_t>(m.read(msg + 0x2c, 1));
                return 0;
            }
            // After 001b2e90 released the human (its client left: 003cb230 deletes it 0.1 s later) the section
            // still sends it the action; the play's figure is already removed, so it is dropped.
            if (arg(1) == section_human_ && !human_key(arg(1))) return 0;
            if (!human_key(arg(1))) unhosted(address, "the action 0x43 to an object other than a remote human");
            pending_physical_ = static_cast<std::uint8_t>(m.read(msg + 0x2c, 1));
            return 0;
        }
        RemoteAction act;
        act.human = arg(1);
        act.key = human_key(act.human);
        act.transport = static_cast<std::uint8_t>(arg(2));
        act.action = static_cast<std::int32_t>(m.read(msg + 0x24));
        act.kind = static_cast<std::uint8_t>(m.read(msg + 0x28, 1));
        act.word_2c = static_cast<std::int32_t>(m.read(msg + 0x2c));
        act.word_30 = static_cast<std::int32_t>(m.read(msg + 0x30));
        act.word_34 = static_cast<std::int32_t>(m.read(msg + 0x34));
        act.byte_38 = static_cast<std::uint8_t>(m.read(msg + 0x38, 1));
        const auto point = act.kind == 4 ? msg + 0x30 : msg + 0x40;
        for (std::uint32_t c = 0; c < 4; ++c) act.point[c] = std::bit_cast<float>(m.read(point + c * 4));
        act.power_50 = std::bit_cast<float>(m.read(msg + 0x50));
        // The local player's +c (kLocalPlayerId, 0040c540's object) as its LAN id.
        const auto local = static_cast<std::int32_t>(kLocalPlayerId);
        if (act.action == 0x3e && act.word_2c == local) act.word_2c = static_cast<std::int32_t>(local_player_key());
        if (act.kind == 0xb && act.word_30 == local) act.word_30 = static_cast<std::int32_t>(local_player_key());
        remote_actions_.push_back(act);
        return 0;
    }
    case 0x143df0: {  // PORT-ONLY: an item into the inventory (002c6b70's weapon list): the first empty slot
        const auto inventory = arg(0), item = arg(1);
        for (std::uint32_t k = 0; k < 6; ++k)
            if (m.read(inventory + k * 16 + 8) == item) return 0;
        for (std::uint32_t k = 0; k < 6; ++k)
            if (m.read(inventory + k * 16 + 8) == 0xffffffffU) {
                m.write(inventory + k * 16 + 8, 4, item);
                return 0;
            }
        return 0;
    }
    case 0x1439b0: {  // PORT-ONLY: hold an item (the slot with it; -1: nothing): a RemoteWeapon
        const auto inventory = arg(0), item = arg(1);
        std::uint32_t slot = 6;
        if (item != 0xffffffffU) {
            for (std::uint32_t k = 0; k < 6 && slot == 6; ++k)
                if (m.read(inventory + k * 16 + 8) == item) slot = k;
            for (std::uint32_t k = 0; k < 6 && slot == 6; ++k)
                if (m.read(inventory + k * 16 + 8) == 0xffffffffU) slot = k;
            if (slot == 6) slot = 0;
            m.write(inventory + slot * 16 + 8, 4, item);
        }
        m.write(inventory + 0x84, 1, slot);
        // 132ln5h14b: an NPC's (its section's 001b2d20): the item for the play's NPC.
        if (section_npc_ && m.read(section_human_ + 0x3524) == inventory) {
            pending_npc_weapon_ = static_cast<std::int32_t>(item);
            return 0;
        }
        RemoteWeapon w;
        for (const auto& [key, human] : remote_keys_)
            if (m.read(human + 0x3524) == inventory) {
                w.key = key;
                w.human = human;
            }
        w.item = static_cast<std::int32_t>(item);
        remote_weapons_.push_back(w);
        return 0;
    }
    case 0x1b8740: return 0;       // PORT-ONLY: the world's team (team 0)
    // 132ln5g: the leave (see the header).
    case 0x429920: { Frame f(sp_); dme_client_left_00429920(m, *this, arg(0), f.caller); return 0; }
    case 0x43acb0: geninfo_client_left_0043acb0(m, arg(1)); return 0;
    case 0x439390: {
        // $s2 (00438e20 reads it): the caller's; 1 from the timer path (see geninfo_send_timer_0043a8b0).
        Frame f(sp_);
        geninfo_send_00439390(m, *this, arg(1), arg(2), f.caller, a.size() > 3 ? a[3] : 1U);
        return 0;
    }
    // 132ln5h9: GenInfoObject.cc's entry API and its queues' drains.
    case 0x438e20: { Frame f(sp_); geninfo_apply_adds_00438e20(m, *this, arg(0), a.size() > 1 ? a[1] : 1U, f.caller); return 0; }
    case 0x439030: { Frame f(sp_); geninfo_apply_interest_00439030(m, *this, arg(0)); return 0; }
    case 0x438c40: { Frame f(sp_); geninfo_apply_owners_00438c40(m, *this, arg(0)); return 0; }
    case 0x439c70: return geninfo_owner_00439c70(m, arg(1));
    case 0x43aa40: { Frame f(sp_); return geninfo_owned_0043aa40(m, *this, arg(1)); }
    case 0x43ab20: return geninfo_claimed_0043ab20(m, arg(1));
    case 0x43aba0: { Frame f(sp_); return geninfo_interest_0043aba0(m, *this, arg(1), arg(2)); }
    case 0x4398e0: { Frame f(sp_); geninfo_add_004398e0(m, *this, arg(1), arg(2), f.caller); return 0; }
    case 0x439d00: { Frame f(sp_); geninfo_set_owner_00439d00(m, *this, arg(1), arg(2), f.caller); return 0; }
    case 0x43a030: { Frame f(sp_); geninfo_clear_interest_0043a030(m, *this, arg(1), f.caller, a.size() > 2 ? a[2] : 1U); return 0; }
    case 0x43a420: { Frame f(sp_); geninfo_set_interest_0043a420(m, *this, arg(1), f.caller, a.size() > 2 ? a[2] : 1U); return 0; }
    case 0x43ace0: { Frame f(sp_); geninfo_reset_0043ace0(m, *this); return 0; }
    case 0x3fb750: { Frame f(sp_); return client_map_remove_003fb750(m, *this, arg(0)); }
    // 132ln5h stage 2: the class callbacks 002e9d88 registered (0x0058c0b8 / 0x0058c0f8 / remove), their helpers.
    case 0x2d1ff0: { Frame f(sp_); net_human_create_002d1ff0(m, *this, arg(0), arg(1), f.caller); return 0; }
    case 0x2d1dc0: { Frame f(sp_); net_human_owner_002d1dc0(m, *this, arg(0), arg(1), arg(2), f.caller); return 0; }
    case 0x2d1f00: { Frame f(sp_); net_human_created_002d1f00(m, *this, arg(0), arg(1), f.caller); return 0; }
    case 0x2d1d50: net_human_removed_002d1d50(m); return 0;
    case 0x2d1d80: { Frame f(sp_); net_human_send_002d1d80(m, *this, arg(0)); return 0; }
    case 0x2d2110: { Frame f(sp_); net_human_release_002d2110(m, *this, arg(0), f.caller); return 0; }
    case 0x316980: { Frame f(sp_); net_human_link_block_00316980(m, *this, arg(0), arg(1)); return 0; }
    case 0x2c4ea0: { Frame f(sp_); net_human_link_counters_002c4ea0(m, *this, arg(0), arg(1)); return 0; }
    case 0x316a00: net_record_init_00316a00(m, arg(0)); return 0;
    case 0x316a30: { Frame f(sp_); return net_record_release_00316a30(m, *this, arg(0), arg(1)); }
    case 0x3fbb10: return net_map_object_003fbb10(m, arg(0));
    case 0x16a580: { Frame f(sp_); team_data_created_0016a580(m, *this, arg(1), f.caller); return 0; }
    case 0x16a550: team_data_owner_0016a550(m); return 0;
    case 0x16a450: { Frame f(sp_); team_data_removed_0016a450(m, *this); return 0; }
    case 0x439560: { Frame f(sp_); geninfo_created_00439560(m, *this, arg(1), f.caller); return 0; }
    case 0x438c20: geninfo_removed_00438c20(m); return 0;
    case 0x433160: { Frame f(sp_); npc_info_created_00433160(m, *this, arg(1), f.caller); return 0; }
    case 0x432230: { Frame f(sp_); npc_info_owner_00432230(m, *this, arg(1), arg(2), f.caller); return 0; }
    case 0x4321d0: npc_info_removed_004321d0(m); return 0;
    case 0x437830: { Frame f(sp_); headset_created_00437830(m, *this, arg(1), f.caller); return 0; }
    case 0x437640: { Frame f(sp_); headset_owner_00437640(m, *this, arg(1), arg(2), f.caller); return 0; }
    case 0x4375d0: headset_removed_004375d0(m); return 0;
    case 0x3fb4b0: { Frame f(sp_); net_map_set_client_003fb4b0(m, *this, arg(0), arg(1), f.caller); return 0; }
    case 0x4361c0: return npc_info_find_004361c0(m, arg(0), arg(1));
    case 0x4341f0: { Frame f(sp_); npc_info_apply_004341f0(m, *this, arg(0), arg(1), f.caller); return 0; }
    // 132ln5h12: NPCInfoObject.cc's owner drains and queues, the entry queries, the owner change, the spawn message
    // (vtable 0x004de240) and an actor's NetMap registration (research/NET_INLEVEL.md 4.22).
    case 0x432b10: { Frame f(sp_); npc_info_drain_adds_00432b10(m, *this, arg(1), f.caller); return 0; }
    case 0x432f50: { Frame f(sp_); npc_info_drain_interest_set_00432f50(m, *this, arg(1), arg(2)); return 0; }
    case 0x432910: { Frame f(sp_); npc_info_drain_interest_clear_00432910(m, *this, arg(1), arg(2)); return 0; }
    case 0x432750: { Frame f(sp_); npc_info_drain_owners_00432750(m, *this, arg(1), arg(2)); return 0; }
    case 0x432d00: { Frame f(sp_); npc_info_drain_alive_00432d00(m, *this, arg(1), arg(2)); return 0; }
    case 0x432510: { Frame f(sp_); npc_info_drain_owner_changes_00432510(m, *this, arg(1), arg(2)); return 0; }
    case 0x434aa0: { Frame f(sp_); npc_info_queue_add_00434aa0(m, *this, arg(1)); return 0; }
    case 0x435070: { Frame f(sp_); npc_info_queue_interest_00435070(m, *this, arg(1)); return 0; }
    // 132ln5h14: an NPC's deactivation (001b5190) clears the local interest (00433ce0 / 00434e20).
    case 0x433ce0: { Frame f(sp_); npc_info_clear_interest_00433ce0(m, *this, arg(0), arg(1), f.caller); return 0; }
    case 0x434e20: { Frame f(sp_); npc_info_queue_uninterest_00434e20(m, *this, arg(1)); return 0; }
    case 0x434d10: { Frame f(sp_); npc_info_queue_owner_00434d10(m, *this, arg(1)); return 0; }
    case 0x434920: { Frame f(sp_); npc_info_flush_00434920(m, *this, arg(1), f.caller); return 0; }
    case 0x435f30: { Frame f(sp_); npc_info_add_00435f30(m, *this, arg(0), arg(1), arg(2), f.caller); return 0; }
    case 0x434600: { Frame f(sp_); npc_info_set_owner_00434600(m, *this, arg(0), arg(1), arg(2), f.caller); return 0; }
    case 0x4344a0: { Frame f(sp_); npc_info_set_owner_key_004344a0(m, *this, arg(0), arg(1), arg(2), f.caller); return 0; }
    case 0x434440: { Frame f(sp_); return npc_info_owner_of_00434440(m, *this, arg(0), arg(1)); }
    case 0x434380: { Frame f(sp_); return npc_info_next_owner_00434380(m, *this, arg(0), arg(1)); }
    case 0x4340c0: { Frame f(sp_); return npc_info_others_interested_004340c0(m, *this, arg(1)); }
    case 0x434190: { Frame f(sp_); return npc_info_interested_00434190(m, *this, arg(1), arg(2)); }
    // 132ln5h13: the owner's per-NPC contact byte (0014bff0 at 0014c510) and its alive-map queue.
    case 0x433ec0: { Frame f(sp_); return npc_info_contact_00433ec0(m, *this, arg(1), arg(2)); }
    case 0x433f20: { Frame f(sp_); npc_info_set_contact_00433f20(m, *this, arg(0), arg(1), arg(2), f.caller); return 0; }
    case 0x434be0: { Frame f(sp_); npc_info_queue_alive_00434be0(m, *this, arg(1), arg(2), f.caller); return 0; }
    case 0x4343e0: { Frame f(sp_); return npc_info_owner_004343e0(m, *this, arg(0), arg(1), f.caller); }
    case 0x3fbc40: { Frame f(sp_); return npc_owned_locally_003fbc40(m, *this, arg(0)); }
    case 0x3fb340: { Frame f(sp_); return net_map_client_003fb340(m, *this, arg(0)); }
    case 0x1856d0: { Frame f(sp_); actor_net_register_001856d0(m, *this, arg(0)); return 0; }
    case 0x21df80: { Frame f(sp_); return spawn_message_destroy_0021df80(m, *this, arg(0), arg(1)); }
    case 0x2d0ee0: { Frame f(sp_); return spawn_message_clone_002d0ee0(m, *this, arg(0)); }
    case 0x2d1100: { Frame f(sp_); return spawn_message_serialise_002d1100(m, *this, arg(0), arg(1), arg(2), arg(5), arg(6)); }
    case 0x2d1290: { Frame f(sp_); return spawn_message_build_002d1290(m, *this, arg(0), arg(1), f.caller); }
    case 0x2d11d0: { Frame f(sp_); ++spawns_received_; return spawn_message_receive_002d11d0(m, *this, arg(3), f.caller); }
    // 132ln5h14: the despawn message (vtable 0x004de260) and its receive (002d16e0 registers 002d1810 for 0x0051f070).
    case 0x220350: { Frame f(sp_); return despawn_message_destroy_00220350(m, *this, arg(0), arg(1)); }
    case 0x2d15c0: { Frame f(sp_); return despawn_message_clone_002d15c0(m, *this, arg(0)); }
    case 0x2d1790: { Frame f(sp_); return despawn_message_serialise_002d1790(m, *this, arg(0), arg(1), arg(2), arg(5), arg(6)); }
    case 0x2d1810: { Frame f(sp_); ++despawns_received_; return despawn_message_receive_002d1810(m, *this, arg(3), f.caller); }
    // 132ln5h14c: the health message (vtable 0x004e0ee0) and its receive (00431b10 registers 00431a70 for 0x00582748).
    case 0x3cc670: { Frame f(sp_); return health_message_destroy_003cc670(m, *this, arg(0), arg(1)); }
    case 0x431960: { Frame f(sp_); return health_message_clone_00431960(m, *this, arg(0)); }
    case 0x431bc0: { Frame f(sp_); return health_message_serialise_00431bc0(m, *this, arg(0), arg(1), arg(2), arg(5), arg(6)); }
    case 0x431a70: { Frame f(sp_); ++healths_received_; return health_message_receive_00431a70(m, *this, arg(3), f.caller); }
    // 132ln5h14c: the damage message (vtable 0x004e08a0) and its receive (net_registrations: 004081e0 for 0x005716f8).
    case 0x1880e0: { Frame f(sp_); return damage_message_destroy_001880e0(m, *this, arg(0), arg(1)); }
    case 0x407f40: { Frame f(sp_); return damage_message_clone_00407f40(m, *this, arg(0)); }
    case 0x4083d0: { Frame f(sp_); return damage_message_serialise_004083d0(m, *this, arg(0), arg(1), arg(2), arg(5), arg(6)); }
    case 0x4081e0: { Frame f(sp_); ++damages_received_; return damage_message_receive_004081e0(m, *this, arg(3), f.caller); }
    // 132ln5h14c: the death action (vtable 0x004e01e0) and its receive (net_registrations: 003e4f70 for 0x0055a220).
    case 0x184eb0: { Frame f(sp_); return death_action_destroy_00184eb0(m, *this, arg(0), arg(1)); }
    case 0x3e7370: { Frame f(sp_); return death_action_clone_003e7370(m, *this, arg(0)); }
    case 0x3e6270: { Frame f(sp_); return death_action_serialise_003e6270(m, *this, arg(0), arg(1), arg(2), arg(5), arg(6)); }
    case 0x3e4f70: { Frame f(sp_); ++deaths_received_; return death_action_receive_003e4f70(m, *this, arg(3), f.caller); }
    // 132ln5h13: the "Generator Message" (vtable 0x004db600) and the "GeneratorBase Message" receive.
    case 0x157d20: { Frame f(sp_); return generator_release_message_destroy_00157d20(m, *this, arg(0), arg(1)); }
    case 0x222410: { Frame f(sp_); return generator_release_message_clone_00222410(m, *this, arg(0)); }
    case 0x21d370: { Frame f(sp_); return generator_release_message_serialise_0021d370(m, *this, arg(0), arg(1), arg(2), arg(5), arg(6)); }
    case 0x21d6c0: {
        // Kinds 0..2 (vtables 0x004db660 / 0x004db640 / 0x004db620: the SpawnedNPCMsg / DespawnedNPCMsg classes'
        // serialisers 0021d4d0..) are not sent by the port; their clones are not hosted.
        if (const auto kind = m.read(arg(3) + 8, 1); kind != 3)
            unhosted(address, "the GeneratorBase message of kind " + std::to_string(kind) + " (its vtable's clone)");
        Frame f(sp_, 0x200);
        ++generator_releases_received_;
        return generator_base_message_receive_0021d6c0(m, *this, arg(3), f.caller);
    }
    case 0x222870: return 1;  // the generator's virtual +0x18 (networked; jr ra, li v0 1)
    // The NPC controllers' virtual functions: the AI controller 0x004d9380 (+0x54 0012f370: 0, +0x58 0015b7e0: 1,
    // +0x5c 001535d0: 0, +0x88 001489c0: 1) and the remote NPC controller 0x004de2a0 (+0x54 002d2a90: 0, +0x58
    // 002d2aa0: 0, +0x5c 002d2b20: 1, +0x88 002d2a80: 2); +0x44 00157a40 (both).
    case 0x12f370: case 0x2d2a90: case 0x2d2aa0: return 0;
    case 0x15b7e0: case 0x1489c0: case 0x2d2b20: return 1;
    case 0x2d2a80: return 2;
    case 0x157a40: {  // the controller's actor (+0x30) alive (its +0x108 not 2 or 3), or dead and its +0x164 zero
        const auto ctl = arg(0), actor = m.read(ctl + 0x30);
        const auto life = call(m, m.read(m.read(actor) + 0x108), {actor}) & 0xffU;
        if (life - 2U >= 2U) return 1;
        return call(m, m.read(m.read(ctl) + 0x164), {ctl}) == 0 ? 1U : 0U;
    }
    case 0x1b2e90: { Frame f(sp_, 0x400); return human_left_check_001b2e90(m, *this, arg(0), f.caller); }
    case 0x1b2d20: { Frame f(sp_); human_weapon_sync_001b2d20(m, *this, arg(0)); return 0; }
    case 0x140e10: case 0x1535d0: return 0;  // the controllers' virtual +0x58 / +0x5c (jr ra; move v0, zero)
    case 0x3cc990: {  // 003cc990(human, hp, a2)
        // In 001b1710's section (a2 1: the owner's health on a non-owner) the play applies it (pending_health_).
        if (section_human_) {
            m.write(arg(0) + 0x40, 4, std::bit_cast<std::uint32_t>(static_cast<float>(static_cast<std::int32_t>(arg(1)))));
            if (arg(2) == 1) pending_health_ = static_cast<std::int32_t>(arg(1));
            return 0;
        }
        // 132ln5h14c: natively (a2 0 in a network game sends the health message); a2 1 outside a section is the
        // health message's receive 00431a70 (0040c540's object): kept for the play.
        Frame f(sp_);
        health_set_003cc990(m, *this, arg(0), arg(1), arg(2), f.caller);
        if (arg(2) == 1) {
            RemoteHealth h;
            h.local_player = arg(0) == kLocalPlayer;
            h.key = h.local_player ? local_player_key() : human_key(arg(0)) ? human_key(arg(0)) : m.read(arg(0) + 0xc);
            h.hp = static_cast<std::int32_t>(arg(1));
            remote_healths_.push_back(h);
        }
        return 0;
    }
    case 0x128aa8: {  // snprintf(a0, a1, a2): the texts it gets here have no conversions
        std::string text;
        for (std::uint32_t k = 0; auto b = m.read(arg(2) + k, 1); ++k) {
            if (b == '%') unhosted(address, "an snprintf format with a conversion");
            text.push_back(static_cast<char>(b));
        }
        if (arg(1)) {
            if (text.size() >= arg(1)) text.resize(arg(1) - 1);
            m.copy(arg(0), text.c_str());
        }
        log_.push_back("128aa8: " + text);
        return static_cast<std::uint32_t>(text.size());
    }
    case 0x3fdb50: return arg(0);  // PORT-ONLY: the localized text of a key: the key itself (no text tables here)
    case 0x12f5d0: return arg(0) + (arg(1) << 4) + 0xf0;  // a client's name slot (empty here: the names are not mirrored)
    case 0x128bd0: {  // sprintf(a0, a1, ...): %s only
        std::string text;
        std::size_t next = 2;
        for (std::uint32_t k = 0; auto b = m.read(arg(1) + k, 1); ++k) {
            if (b != '%') { text.push_back(static_cast<char>(b)); continue; }
            const auto c = m.read(arg(1) + ++k, 1);
            if (c != 's') unhosted(address, "a sprintf format other than %s");
            for (std::uint32_t j = 0; auto s = m.read(arg(next) + j, 1); ++j) text.push_back(static_cast<char>(s));
            ++next;
        }
        m.copy(arg(0), text.c_str());
        return static_cast<std::uint32_t>(text.size());
    }
    case 0x3fa3f0: {  // PORT-ONLY: the HUD's message line (a0 the HUD +0x100, a1 the text, a2 the colour): logged
        hud_line_.clear();
        for (std::uint32_t k = 0; auto b = m.read(arg(1) + k, 1); ++k) hud_line_.push_back(static_cast<char>(b));
        log_.push_back("003fa3f0: HUD message \"" + hud_line_ + "\"");
        return 0;
    }
    case 0x1314f0: return kPlayerBlocks;  // PORT-ONLY: the game's player block of a client (001b2e90 clears +0xb70)
    case 0x18c770:  // PORT-ONLY: the human's state 3 (the leaving character's engine state): not ported
    case 0x257040:  // PORT-ONLY: the client's per-player statistics reset: not ported
    case 0x3cb230:  // PORT-ONLY: the character's fade-out (f12 = 0.1): the play hides the figure (RemoteLeft)
        return 0;
    case 0x2ec2a8: {  // the DME object release (002dec60, native)
        Frame f(sp_);
        const auto r = dme_library_call(m, *this, address, list, f.caller);
        log_.push_back("002ec2a8: the DME object " + hex8(arg(0)) + " released (" + hex8(r) + ")");
        return r;
    }
    case 0x40b720: {  // PORT-ONLY: the character list's remove: the remote human's key is forgotten
        if (arg(0) != kCharacters) unhosted(address, "a character list other than the port's");
        std::erase_if(remote_keys_, [&](const auto& e) { return e.second == arg(1); });
        remote_positions_.erase(arg(1));
        return 0;
    }
    case kFnLeave: {  // PORT-ONLY: the remote human's release (virtual +0x60, a1 = 1)
        RemoteLeft left;
        left.key = m.read(arg(0) + 0x10);
        left.client = static_cast<std::uint16_t>(((m.read(arg(0) + 0xc) >> 24) & 0x7fU) - 1U);
        left.human = arg(0);
        left.message = hud_line_;
        remote_left_.push_back(left);
        std::erase_if(remote_moves_, [&](const RemoteMove& u) { return u.human == left.human; });  // its stale data
        log_.push_back("001b2e90: remote human key " + hex8(left.key) + " of client " + std::to_string(left.client) +
                       " removed (its client left)");
        return 0;
    }
    case 0x406ec0: {  // 132ln5h6: the message's compact appearance into 002c6b70's descriptor
        if (!part_cache_) return 0;  // PORT-ONLY: no level part cache: the figure keeps its own parts
        // The message is 0x430 bytes; its compact (from +0x30) is at most 0x400 of them.
        std::vector<std::uint8_t> compact(0x400);
        for (std::uint32_t k = 0; k < compact.size(); ++k) compact[k] = static_cast<std::uint8_t>(m.read(arg(1) + k, 1));
        AgentPacking packing{};
        for (std::uint32_t k = 0; k < packing.size(); ++k) packing[k] = static_cast<std::uint8_t>(m.read(arg(0) + k, 1));
        agent_packing_expand(*part_cache_, compact, packing);
        for (std::uint32_t k = 0; k < packing.size(); ++k) m.write(arg(0) + k, 1, packing[k]);
        return 0;
    }
    // 132ln5h11: the generators' messages (research/NET_INLEVEL.md 4.21).
    case 0x16fc70: { Frame f(sp_); return disable_message_destroy_0016fc70(m, *this, arg(0), arg(1)); }
    case 0x16fd90: { Frame f(sp_); return enable_message_destroy_0016fd90(m, *this, arg(0), arg(1)); }
    case 0x3ff3b0: { Frame f(sp_); return disable_message_clone_003ff3b0(m, *this, arg(0)); }
    case 0x3ff690: { Frame f(sp_); return enable_message_clone_003ff690(m, *this, arg(0)); }
    // 003fb020's serialiser call (msg, key, typeptr, clock, t0, cap, buf).
    case 0x3ff570: { Frame f(sp_); return disable_message_serialise_003ff570(m, *this, arg(0), arg(1), arg(2), arg(5), arg(6)); }
    case 0x3ff850: { Frame f(sp_); return enable_message_serialise_003ff850(m, *this, arg(0), arg(1), arg(2), arg(5), arg(6)); }
    case 0x3ff5d0: { Frame f(sp_); ++generator_received_; return disable_message_receive_003ff5d0(m, *this, arg(3)); }
    case 0x3ff8b0: { Frame f(sp_); ++generator_received_; return enable_message_receive_003ff8b0(m, *this, arg(3)); }
    case 0x3ff650: { Frame f(sp_); disable_message_send_003ff650(m, *this, arg(0), arg(1), arg(2)); return 0; }
    case 0x3ff930: { Frame f(sp_); enable_message_send_003ff930(m, *this, arg(0), arg(1)); return 0; }
    case 0x3ce850: { Frame f(sp_); return nearest_player_003ce850(m, *this, arg(0), arg(1), f.caller); }
    case 0x3fb8d0: return client_player_003fb8d0(static_cast<std::int32_t>(arg(0)));
    case kFnGenPosition: {  // PORT-ONLY: a generator block's position virtual +0xc0(out, generator, -1)
        for (std::uint32_t k = 0; k < 16; k += 4) m.write(arg(0) + k, 4, m.read(arg(1) + 0x30 + k));
        return arg(0);
    }
    case kFnHumanPosition: {  // PORT-ONLY: a player's position virtual +0xc0(out, human, -1) (the header)
        std::array<float, 3> p{};
        if (arg(1) == kLocalPlayer) {
            for (std::uint32_t c = 0; c < 3; ++c) p[c] = std::bit_cast<float>(m.read(kPlayerComponent + 0x40 + c * 4));
        } else {
            const auto found = remote_positions_.find(arg(1));
            if (found == remote_positions_.end()) unhosted(0x3ce850, "the position of a human that is not a player");
            p = found->second;
        }
        for (std::uint32_t c = 0; c < 3; ++c) m.write(arg(0) + c * 4, 4, std::bit_cast<std::uint32_t>(p[c]));
        m.write(arg(0) + 12, 4, std::bit_cast<std::uint32_t>(1.0F));
        return arg(0);
    }
    case kFnDamage: {  // PORT-ONLY: the damage virtual +0x100 (003ccb00) from 004081e0: the play's actor takes it
        if (arg(4) || arg(5) != 1 || arg(6))
            unhosted(0x3ccb00, "a damage virtual +0x100 other than 004081e0's (t0 0, quiet 1, t2 0)");
        RemoteDamage d;
        d.local_player = arg(0) == kLocalPlayer;
        d.victim_key = lan_id(m, arg(0));
        d.source_key = lan_id(m, arg(1));
        d.category = static_cast<std::uint8_t>(arg(2));
        d.type = static_cast<std::uint8_t>(arg(3));
        d.amount = std::bit_cast<float>(arg(7));
        remote_damages_.push_back(d);
        return 0;
    }
    case kFnGenEnable:
    case kFnGenDisable: {  // PORT-ONLY: the generator's virtual +0x28 (00220e60) / +0x2c (00220d00): the play's
        const bool enable = address == kFnGenEnable;
        const auto id = m.read(arg(0) + 0xc);
        if (!generator_receiver_)
            unhosted(enable ? 0x220e60 : 0x220d00, "a generator's remote " + std::string(enable ? "enable" : "disable") +
                                                       " without the play's generators");
        log_.push_back(std::string(enable ? "003ff8b0: cEnableMsg" : "003ff5d0: cDisableMsg") + " for generator " +
                       std::to_string(id) + " (client " + std::to_string(static_cast<std::int32_t>(arg(1))) + ")");
        generator_receiver_(id, static_cast<std::int32_t>(arg(1)), enable);
        return 0;
    }
    case 0x40b790: case 0x1b8750: case 0x2c9270:
    case 0x242fa0:
    case kFnNop:
        return 0;
    case kFnHuman: return 1;
    case kFnRemote: return 2;
    default:
        break;
    }
    if (net_registration_has(address)) {
        Frame f(sp_, 0x1000);  // the registration frames reach 0x440 below the entry $sp
        return net_registration_call(m, *this, address, f.caller);
    }
    if (dme_library_has(address)) {
        Frame f(sp_);
        return dme_library_call(m, *this, address, list, f.caller);
    }
    if (LanDme::is_dme_api(address)) return dme_->call(m, address, list);
    if ((address >= 0x10c000 && address < 0x10f400) || address == 0x1161f8 || address == 0x1161a0) return kernel(address, a);
    unhosted(address, "not ported (LAN phase 3: research/NET_INLEVEL.md)");
}

std::uint32_t LanNetCore::call_framed(LevelLoadMemory& m, std::uint32_t address, std::initializer_list<std::uint32_t> list,
                                      std::uint32_t sp) {
    if (address == 0x1b8130) return human_constructor_001b8130(m, std::vector<std::uint32_t>(list), sp);
    if (address == 0x2c7110) {
        if (list.size() != 8) unhosted(address, "a named character send without its eight register arguments");
        const auto* a = list.begin();
        const auto caller = sp_;
        sp_ = sp - 0x1000;  // the function's frame (0xf0) and its callees go below the caller's $sp
        const auto r = send_named_character_002c7110(m, *this, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], sp);
        sp_ = caller;
        return r;
    }
    if (address != 0x2eaf28) return LevelLoadHost::call_framed(m, address, list, sp);
    if (list.size() != 8) unhosted(address, "an object create without its eight register arguments");
    const auto* a = list.begin();
    const auto caller = sp_;
    sp_ = sp - 0x400;  // the library's own callees go below 002eaf28's frames
    const auto r = dme_library_call(m, *this, address, {a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], m.read(sp), m.read(sp + 8)}, sp);
    sp_ = caller;
    return r;
}

void LanNetCore::level_load_001693f0(LevelLoadHost* level) {
    level_load_begin(level);
    level_load_finish(level);
}

void LanNetCore::set_lobby(std::function<void()> pump) {
    lobby_ = std::move(pump);
    memory_.write(0x572480, 4, lobby_ ? kLobbySession : 0U);  // PORT-ONLY: the sentinel stands in for SFOLobby's session
    memory_.write(0x572488, 1, lobby_ ? 1U : 0U);
}

void LanNetCore::set_lobby_exit(std::function<void()> exit_game) {
    lobby_exit_ = std::move(exit_game);
}

void LanNetCore::finish_level_teardown(bool retain_session) {
    level_loaded_ = false;
    // World_Teardown has already freed the native network objects. These are
    // port-side level mirrors and delivery queues; none belongs to the lobby.
    // In particular, the callbacks and part cache may refer to play locals
    // that are destroyed before the front end resumes.
    level_ = nullptr;
    lobby_exit_ = {};
    generator_receiver_ = {};
    npc_control_handler_ = {};
    part_cache_ = nullptr;
    remote_keys_.clear();
    backpack_bindings_.clear();
    remote_events_.clear();
    remote_moves_.clear();
    remote_left_.clear();
    remote_actions_.clear();
    remote_aims_.clear();
    remote_weapons_.clear();
    remote_animations_.clear();
    remote_positions_.clear();
    generator_blocks_.clear();
    npc_blocks_.clear();
    npc_generators_.clear();
    free_npc_blocks_.clear();
    remote_spawns_.clear();
    npc_controls_.clear();
    generator_releases_.clear();
    remote_despawns_.clear();
    remote_healths_.clear();
    remote_damages_.clear();
    remote_deaths_.clear();
    objective_bindings_.clear();
    objective_outcomes_.clear();
    objective_actives_.clear();
    agent_updates_.clear();
    global_timers_.clear();
    personal_timers_.clear();
    player_sounds_.clear();
    end_levels_.clear();
    (void)dme_->take_backpacks();
    pending_health_.reset();
    pending_physical_.reset();
    pending_npc_weapon_.reset();
    local_floor_plane_.reset();
    hud_line_.clear();
    local_set_ = false;
    local_inventory_ = false;
    if (!retain_session) {
        lobby_ = {};
        memory_.write(0x572480, 4, 0);
        memory_.write(0x572488, 1, 0);
    }
}

void LanNetCore::level_load_begin(LevelLoadHost* level) {
    require_objective_main_thread();
    auto& m = memory_;
    // The play host flushes deferred objective constructors before its
    // 001350e0 adapter. Mirror that loader's 0040c2d0 reset here, before any
    // new registration. Reusing IDs/storage requires the preceding native
    // 003fbcd0 and finish_level_teardown, not merely removing C++ bindings.
    if (level_loaded_ || !objective_bindings_.empty() || !objective_outcomes_.empty() ||
        !objective_actives_.empty() || m.read(dme::kNetMap) || m.read(dme::kNetMap + 4))
        throw std::runtime_error("LAN level load requires previous world network cleanup before objective ID reuse");
    objective_counter_ = 0;
    next_objective_block_ = kObjectiveBlocks;
    level_ = level;
    if (m.read(dme::kNetworkGame, 1)) call(m, 0x42a770, {});
    call(m, 0x16a970, {});
}

void LanNetCore::level_load_finish(LevelLoadHost* level) {
    level_ = level;
    auto& m = memory_;
    const auto flag = [&] { return m.read(dme::kNetworkGame, 1); };
    call(m, 0x1350e0, {0x4ffb50});
    if (flag()) {
        call(m, 0x42a1b0, {});
        call(m, 0x42b0c0, {});
        const auto route = flag() ? m.read(dme::kHostRole, 1) : 1U;
        if (route) {
            call(m, 0x16a970, {});
        } else {
            call(m, 0x185840, {});
            call(m, 0x3fa5d0, {});
        }
    }
    if (flag()) call(m, 0x429d30, {});
    level_loaded_ = true;
    level_ = nullptr;  // the level host is the caller's (a local of the play's level setup)
}

bool LanNetCore::take_quit_request() {
    if (!memory_.read(dme::kQuitRequest, 1)) return false;
    memory_.write(dme::kQuitRequest, 1, 0);
    return true;
}

std::string LanNetCore::quit_text() {
    std::string text;
    for (std::uint32_t k = 0; k < 16; ++k) {
        const auto c = static_cast<char>(memory_.read(dme::kQuitText + k, 1));
        if (!c) break;
        text.push_back(c);
    }
    return text;
}

LanNetCore::QuitNotice LanNetCore::quit_notice() const {
    if (pump_.joinable())
        throw std::runtime_error("LAN quit notice access during background load pump");
    const auto bounded = [&](std::uint32_t base, std::uint32_t size) {
        std::string text;
        for (std::uint32_t i = 0; i < size; ++i) {
            const auto c = static_cast<char>(memory_.read(base + i, 1));
            if (!c) break;
            text.push_back(c);
        }
        return text;
    };
    QuitNotice out;
    out.kind = static_cast<std::uint8_t>(memory_.read(0x5723b0, 1));
    out.key = bounded(0x5721e0, 16);
    const auto message = bounded(0x5721f0, 128);
    if (!message.empty()) out.message = message;
    return out;
}

void LanNetCore::retained_frontend_tick(double milliseconds) {
    if (pump_.joinable() || level_loaded_ || memory_.read(dme::kNetworkGame, 1) ||
        memory_.read(dme::kInLevel, 1) || !lobby_)
        throw std::logic_error("LAN retained frontend tick requires a quiescent MP-off core with a lobby");
    if (!std::isfinite(milliseconds) || milliseconds < 0.0)
        throw std::invalid_argument("LAN retained frontend tick requires finite nonnegative elapsed time");
    if (!thread_alive()) call(memory_, 0x42a770, {});
    advance_timer(milliseconds);
}

void LanNetCore::net_frame() {
    if (dme_->refused_messages() != refused_logged_) {
        log_.push_back("00305a48: " + std::to_string(dme_->refused_messages() - refused_logged_) +
                       " game message(s) refused and dropped (error 7), the last by " +
                       hex8(dme_->last_refused() & 0xffffffU) + " (category " + std::to_string(dme_->last_refused() >> 24) +
                       ") from client " + std::to_string(dme_->last_refused_origin()));
        refused_logged_ = dme_->refused_messages();
    }
    call(memory_, 0x169390, {});
    if (memory_.read(dme::kNetworkGame, 1)) call(memory_, 0x3fbec0, {});
    if (!level_loaded_) return;
    // 132ln5h4 (PORT-ONLY caller: the game's per-frame character traversal is the play's): each remote human's
    // update 001b1710, whose network section (human_network_section_001b1710) applies its cNetHuman block (the
    // health, +0x33a5, the held weapon 001b2d20, the leave check 001b2e90 through the controller's +0x54, the
    // animation fields and the action 0x43); its results go to the play as a RemoteAnimation.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> humans(remote_keys_);
    for (const auto& [key, human] : humans) {
        pending_health_.reset();
        pending_physical_.reset();
        human_update_001b1710(human);
        if (!human_key(human)) continue;  // released by 001b2e90 (its client left)
        RemoteAnimation a;
        a.key = key;
        a.human = human;
        a.flags_3248 = memory_.read(human + 0x3248);
        a.turn_correction_31b3 = memory_.read(human + 0x31b3, 1) != 0;
        a.accumulate_31b4 = memory_.read(human + 0x31b4, 1) != 0;
        a.byte_31b5 = memory_.read(human + 0x31b5, 1) != 0;
        a.physical_12 = pending_physical_;
        a.health = pending_health_;
        a.killed_33a5 = memory_.read(human + 0x33a5, 1) != 0;
        remote_animations_.push_back(a);
    }
    // 132ln5h (PORT-ONLY caller): each remote human's controller update, virtual +0x40 (002d83c0).
    humans = remote_keys_;
    for (const auto& [key, human] : humans) {
        const auto controller = memory_.read(human + 0x58);
        if (controller && memory_.read(controller) == 0x4de530) {
            call(memory_, memory_.read(0x4de530 + 0x40), {controller});
            ++controller_runs_;
        }
    }
    if (local_set_) {
        player_network_001b53d0();
        // 132ln5h4: the local player's update 001b1710's network section (+0x14 set: the block written from the
        // player: +0x38 the health, +0x52 the flashlight, +0x35 the killed byte; 001b2d20 the held weapon).
        if (memory_.read(kLocalPlayer + 0x3524)) human_update_001b1710(kLocalPlayer);
        // 132ln5h3 (PORT-ONLY caller: the play's player update is the port's): the player controller's network
        // update 001770f0 (its virtual update's network part), dt 1/60.
        call(memory_, 0x1770f0, {kPlayerController, std::bit_cast<std::uint32_t>(1.0F / 60.0F)});
    }
}

// 001b1710 from its entry: +0x3269 set takes 001b2650 (not ported); else its network section (the rest, from
// 001b1bf0, is the play's human update). Frame 0x340.
void LanNetCore::human_update_001b1710(std::uint32_t human) {
    if (memory_.read(human + 0x3269, 1)) unhosted(0x1b2650, "a human whose +0x3269 is set (001b1710's other update)");
    const auto caller = sp_;
    sp_ -= 0x400;
    section_human_ = human;
    try {
        human_network_section_001b1710(memory_, *this, human, caller);
    } catch (...) {
        sp_ = caller;
        section_human_ = 0;
        throw;
    }
    sp_ = caller;
    section_human_ = 0;
    ++sections_run_;
}

// 132ln5h14b (the header).
LanNetCore::NpcSectionOut LanNetCore::npc_section_001b1710(std::uint32_t c, const NpcSectionIn& in) {
    auto& m = memory_;
    const auto p = npc_block(c);
    m.write(p + 0x40, 4, std::bit_cast<std::uint32_t>(in.health_40));
    m.write(p + 0x33a5, 1, in.killed_33a5 ? 1U : 0U);
    const auto inventory = m.read(p + 0x3524);
    std::uint32_t slot = 6;
    if (in.held_item != -1) {
        const auto item = static_cast<std::uint32_t>(in.held_item);
        for (std::uint32_t k = 0; k < 6 && slot == 6; ++k)
            if (m.read(inventory + k * 16 + 8) == item) slot = k;
        for (std::uint32_t k = 0; k < 6 && slot == 6; ++k)
            if (m.read(inventory + k * 16 + 8) == 0xffffffffU) slot = k;
        if (slot == 6) slot = 0;
        m.write(inventory + slot * 16 + 8, 4, item);
    }
    m.write(inventory + 0x84, 1, slot);
    const auto data_before = m.read(p + 0x3584);
    if (m.read(p + 0x14, 1) && data_before)
        mirror_animation_block(m, data_before, in.flags_3248, in.current_3204, in.turn_correction_31b3,
                               in.accumulate_31b4, in.byte_31b5, in.physical_12);
    const bool locked_before = data_before && m.read(data_before + 0x28, 1) != 0;
    pending_health_.reset();
    pending_physical_.reset();
    pending_npc_weapon_.reset();
    section_npc_ = true;
    try {
        human_update_001b1710(p);
    } catch (...) {
        section_npc_ = false;
        throw;
    }
    section_npc_ = false;
    ++npc_sections_run_;
    NpcSectionOut out;
    out.owner = m.read(p + 0x14, 1) != 0;
    const auto data = m.read(p + 0x3584);
    out.linked = data != 0;
    const bool locked_after = data && m.read(data + 0x28, 1) != 0;
    out.claimed = !locked_before && locked_after;
    out.released = locked_before && !locked_after && data == data_before;
    out.health = pending_health_;
    out.killed_33a5 = m.read(p + 0x33a5, 1) != 0;
    out.weapon = pending_npc_weapon_;
    out.physical_12 = pending_physical_;
    out.flags_3248 = m.read(p + 0x3248);
    out.turn_correction_31b3 = m.read(p + 0x31b3, 1) != 0;
    out.accumulate_31b4 = m.read(p + 0x31b4, 1) != 0;
    out.byte_31b5 = m.read(p + 0x31b5, 1) != 0;
    return out;
}

std::vector<LanNetCore::RemoteAnimation> LanNetCore::take_remote_animations() {
    std::vector<RemoteAnimation> out;
    out.swap(remote_animations_);
    return out;
}

void LanNetCore::set_remote_state(std::uint32_t key, float health_40, bool killed_33a5) {
    for (const auto& [k, human] : remote_keys_) {
        if (k != key) continue;
        memory_.write(human + 0x40, 4, std::bit_cast<std::uint32_t>(health_40));
        memory_.write(human + 0x33a5, 1, killed_33a5 ? 1U : 0U);
    }
}

// PORT-ONLY (the header): the local player's animation fields, and their mirror into its network block.
//   block +0x50: bit 1 = +0x3248 bit 0, bit 2 = +0x3204 bit 0x10 (0019a6b0's crouch), bit 4 = +0x31b3,
//                bit 8 = +0x31b4, bit 0x20 = +0x31b5, bit 0x40 = the physical's +0x12 is 0;
//   block +0x51: 0xc with +0x3248 bit 31, 0xb with bit 30, else 0xd (written only over 0xb / 0xc).
void LanNetCore::set_local_animation(std::uint32_t flags_3248, std::uint32_t current_3204, bool turn_correction_31b3,
                                     bool accumulate_31b4, bool byte_31b5, std::uint8_t physical_12, bool flashlight_on,
                                     bool killed_33a5) {
    auto& m = memory_;
    const auto p = kLocalPlayer;
    m.write(p + 0x3248, 4, flags_3248);
    m.write(p + 0x3204, 4, current_3204);
    m.write(p + 0x31b3, 1, turn_correction_31b3 ? 1U : 0U);
    m.write(p + 0x31b4, 1, accumulate_31b4 ? 1U : 0U);
    m.write(p + 0x31b5, 1, byte_31b5 ? 1U : 0U);
    m.write(p + 0x33a5, 1, killed_33a5 ? 1U : 0U);
    m.write(kScratch + 0x40, 1, flashlight_on ? 1U : 0U);  // PORT-ONLY: the flashlight object +0x33ec (its byte +0)
    m.write(p + 0x33ec, 4, kScratch + 0x40);
    const auto block = m.read(p + 0x3584);
    if (!block) return;
    mirror_animation_block(m, block, flags_3248, current_3204, turn_correction_31b3, accumulate_31b4, byte_31b5,
                           physical_12);
}

void LanNetCore::mirror_animation_block(LevelLoadMemory& m, std::uint32_t block, std::uint32_t flags_3248,
                                        std::uint32_t current_3204, bool turn_correction_31b3, bool accumulate_31b4,
                                        bool byte_31b5, std::uint8_t physical_12) {
    std::uint32_t bits = 0;
    if (flags_3248 & 1U) bits |= 1U;
    if (current_3204 & 0x10U) bits |= 2U;
    if (turn_correction_31b3) bits |= 4U;
    if (accumulate_31b4) bits |= 8U;
    if (byte_31b5) bits |= 0x20U;
    if (physical_12 == 0) bits |= 0x40U;
    m.write(block + 0x50, 1, (m.read(block + 0x50, 1) & 0x90U) | bits);
    const auto mode = m.read(block + 0x51, 1);
    if (flags_3248 & 0x80000000U) m.write(block + 0x51, 1, 0xc);
    else if (flags_3248 & 0x40000000U) m.write(block + 0x51, 1, 0xb);
    else if (mode == 0xb || mode == 0xc) m.write(block + 0x51, 1, 0xd);
}

// 001b53d0's network tail (the player's per-frame update, vtable +0x5c; PORT-ONLY caller: the play's player update
// is the port's): in a network game, the player's cNetHuman created (002d1ff0) and its NetMap entry (003fb650) while
// +0x3584 is 0 in the level, the entry's client (003fb4b0), the NPC info pass (00436c00, 004341f0, 00436ba0) with
// +0x14 = 1, the cNetHuman's ownership claimed (002d1d80); then +0x3268 = 1. Frame 0x40.
void LanNetCore::player_network_001b53d0() {
    auto& m = memory_;
    const auto p = kLocalPlayer;
    const auto caller = sp_;
    sp_ -= 0x40;
    const auto sp = sp_;
    const auto slot = [&] {
        if (!m.read(dme::kNetworkGame, 1)) return 0U;
        if (m.read(0x494178) == 0xffffffffU) call(m, 0x2ebaf8, {0x494178, m.read(0x5721a8)});
        return m.read(0x494178);
    };
    if (m.read(dme::kNetworkGame, 1)) {
        if (!m.read(p + 0x3584) && m.read(0x5721d0, 1)) {
            call(m, 0x2d1ff0, {p + 0xc, m.read(p + 0x10)});
            const auto key = m.read(p + 0x10);
            call(m, 0x3fb650, {key, slot(), p});
            if (m.read(p + 0x3584)) {
                ++local_links_;
                // PORT-ONLY: the held weapon recorded again into the new block (the play records it at the level
                // start, before the block exists; the original's equip after the link is not hosted).
                if (local_inventory_) net_record_weapon_002493c0(m, kLocalInput, kLocalPlayer);
            }
        }
        const auto key = m.read(p + 0x10);
        call(m, 0x3fb4b0, {key, slot()});
        call(m, 0x436c00, {sp + 0x3c});
        call(m, 0x4341f0, {sp + 0x3c, p + 0xc});
        m.write(p + 0x14, 1, 1);
        if (const auto block = m.read(p + 0x3584)) call(m, 0x2d1d80, {block});
        call(m, 0x436ba0, {sp + 0x3c, 0xffffffffU});
    }
    m.write(p + 0x3268, 1, 1);
    sp_ = caller;
}

// PORT-ONLY: the actor message queue 003ca0e0(out, 0x005435c0, at, entry {transport, clone, target}) for the
// locomotion message (the only message queued here, 132ln5h3). The queue's delivery runs the target's message
// handler 001a92a0 a frame later; that handler is the play's: the local player's own message (001770f0 sends
// flags 5: to the network and to itself) is dropped (the play moves its player), a remote human's is a RemoteMove
// (001a92a0's network branch: the stale check here, the rest in the play). The clone is released either way.
std::uint32_t LanNetCore::queue_message_003ca0e0(LevelLoadMemory& m, std::uint32_t entry) {
    const auto clone = m.read(entry + 4), target = m.read(entry + 8);
    if (m.read(clone) == 0x4de240) {  // 132ln5h12: a spawn message to a generator (0021e0c0, the play's)
        const auto found = std::find_if(generator_blocks_.begin(), generator_blocks_.end(),
                                        [target](const auto& g) { return g.second == target; });
        if (found == generator_blocks_.end()) unhosted(0x3ca0e0, "a spawn message to an object other than a generator");
        RemoteSpawn spawn;
        spawn.generator = found->first;
        const auto f = [&](std::uint32_t at) { return std::bit_cast<float>(m.read(clone + at)); };
        for (std::uint32_t c = 0; c < 4; ++c) spawn.message.position[c] = f(0x30 + c * 4);
        for (std::uint32_t c = 0; c < 3; ++c) spawn.message.angles[c] = f(0x40 + c * 4);
        spawn.message.key_50 = m.read(clone + 0x50);
        spawn.message.key_54 = m.read(clone + 0x54);
        spawn.message.health_58 = m.read(clone + 0x58);
        spawn.message.random_5c = m.read(clone + 0x5c);
        spawn.message.node_60 = static_cast<std::int32_t>(m.read(clone + 0x60));
        remote_spawns_.push_back(spawn);
        m.write(clone + 8, 4, 0);
        call(m, m.read(m.read(clone) + 8), {clone, 0xffffffffU});
        call(m, 0x36b130, {0, clone, 0x4bd118, 0x132});
        return 0;
    }
    if (m.read(clone) == 0x4de260) {  // 132ln5h14: a despawn message to a generator (00221610 at 002216c8, the play's)
        const auto found = std::find_if(generator_blocks_.begin(), generator_blocks_.end(),
                                        [target](const auto& g) { return g.second == target; });
        if (found == generator_blocks_.end()) unhosted(0x3ca0e0, "a despawn message to an object other than a generator");
        if (m.read(m.read(clone + 4)) != m.read(0x51f068))  // 003c9c20, lw (v0): 002216dc
            unhosted(0x221610, "a message of vtable 0x004de260 whose type is not 0x0051f068");
        remote_despawns_.push_back({found->first, m.read(clone + 0x2c), m.read(clone + 0x30)});
        m.write(clone + 8, 4, 0);
        call(m, m.read(m.read(clone) + 8), {clone, 0xffffffffU});
        call(m, 0x36b130, {0, clone, 0x4bd118, 0x132});
        return 0;
    }
    if (m.read(clone) == 0x4e01e0) {  // 132ln5h14c: a received death action (003e4f70's 003c9350, transport 0)
        RemoteDeath death;
        death.code = static_cast<std::int32_t>(m.read(clone + 0x2c));
        if (m.read(clone + 0x24) != 0x42) unhosted(0x3e4f70, "an action message of vtable 0x004e01e0 other than the death action 0x42");
        if (target == kLocalPlayer) {
            death.local_player = true;
            death.key = local_player_key();
        }
        for (const auto& [k, human] : remote_keys_)
            if (human == target) death.key = k;
        for (const auto& [k, block] : npc_blocks_)
            if (block == target) death.key = k;
        if (!death.key) unhosted(0x3e4f70, "a death action to an object of no LAN key");
        remote_deaths_.push_back(death);
        m.write(clone + 8, 4, 0);
        call(m, m.read(m.read(clone) + 8), {clone, 0xffffffffU});
        call(m, 0x36b130, {0, clone, 0x4bd118, 0x132});
        return 0;
    }
    if (m.read(clone) == 0x4e08a0) {  // 132ln5h14c: a DamageMsg queued here (the target's handler, the play's)
        // send_damage_message's own 003c9420 (003ccb00's flags 2 on the host, a replica to an object this console
        // owns): the play queues its ai_notification. Else 004081e0's 003c9350 of a notice (+0x2e 1) to the
        // sender's figure here: PORT-ONLY, the remote figure's handler is not the play's (counted, dropped).
        if (sending_damage_) damage_queued_ = true;
        else ++damage_notices_dropped_;
        m.write(clone + 8, 4, 0);
        call(m, m.read(m.read(clone) + 8), {clone, 0xffffffffU});
        call(m, 0x36b130, {0, clone, 0x4bd118, 0x132});
        return 0;
    }
    if (m.read(clone) == 0x4db600) {  // 132ln5h13: a Generator / GeneratorBase message to a generator (00221610)
        const auto found = std::find_if(generator_blocks_.begin(), generator_blocks_.end(),
                                        [target](const auto& g) { return g.second == target; });
        if (found == generator_blocks_.end()) unhosted(0x3ca0e0, "a Generator Message to an object other than a generator");
        const auto type = m.read(m.read(clone + 4));  // 003c9c20, lw (v0)
        if (type == m.read(0x4a4a68)) {
            // 00221668..00221694: +0x28 == 1, 0043b190 (the play's, at its next generator pass); return 1.
            if (m.read(clone + 0x28, 1) == 1) {
                GeneratorRelease release;
                release.generator = found->first;
                release.c = m.read(m.read(clone + 0x24) + 0xc);
                generator_releases_.push_back(release);
            }
        } else if (type != m.read(0x4a4a70)) {
            unhosted(0x221610, "a message of vtable 0x004db600 whose type is neither 0x004a4a68 nor 0x004a4a70");
        }  // the received GeneratorBase message: none of 00221610's keys (002219f0..00221b98), return 1
        m.write(clone + 8, 4, 0);
        call(m, m.read(m.read(clone) + 8), {clone, 0xffffffffU});
        call(m, 0x36b130, {0, clone, 0x4bd118, 0x132});
        return 0;
    }
    if (m.read(clone) != 0x4e0100) unhosted(0x3ca0e0, "a queued actor message other than the locomotion or spawn message");
    if (target != kLocalPlayer) {
        std::uint32_t key = 0;
        for (const auto& [k, human] : remote_keys_)
            if (human == target) key = k;
        if (!key) unhosted(0x3ca0e0, "a locomotion message to an object other than the player or a remote human");
        const auto stamp = m.read(clone + 0xc);
        if (stamp < m.read(target + 0x3594)) {  // 001a92a0: a message older than the last one (0049e4a0)
            call(m, 0x127358, {0x49e4a0, stamp, m.read(target + 0x3594)});
        } else {
            RemoteMove move;
            move.key = key;
            move.human = target;
            move.stamp = stamp;
            move.clock = call(m, 0x42b1a0, {0});
            const auto f = [&](std::uint32_t at) { return std::bit_cast<float>(m.read(clone + at)); };
            for (std::uint32_t c = 0; c < 4; ++c) {
                move.position[c] = f(0x30 + c * 4);
                move.floor_plane[c] = f(0x40 + c * 4);
            }
            move.heading = f(0x50);
            move.speed = f(0x54);
            remote_positions_[target] = {move.position[0], move.position[1], move.position[2]};  // 132ln5h11, PORT-ONLY
            remote_moves_.push_back(move);
        }
    }
    m.write(clone + 8, 4, 0);
    call(m, m.read(m.read(clone) + 8), {clone, 0xffffffffU});
    call(m, 0x36b130, {0, clone, 0x4bd118, 0x132});
    return 0;
}

std::vector<LanNetCore::RemoteMove> LanNetCore::take_remote_moves() {
    std::vector<RemoteMove> out;
    out.swap(remote_moves_);
    return out;
}
std::vector<LanNetCore::RemoteLeft> LanNetCore::take_remote_left() {
    std::vector<RemoteLeft> out;
    out.swap(remote_left_);
    return out;
}

void LanNetCore::mirror_empty_level_objects() {
    memory_.write(0x571740, 4, kLevelObjects);
    memory_.write(0x571748, 4, kLevelObjects + 0x1000);
}

LanNetCore::~LanNetCore() {
    if (pump_.joinable()) {
        pump_stop_ = true;
        pump_.join();
    }
}

void LanNetCore::start_load_pump() {
    if (pump_.joinable()) throw std::logic_error("LanNetCore: the load pump is already running");
    pump_stop_ = false;
    pump_error_ = nullptr;
    pumped_ms_ = 0;
    pump_ = std::thread([this] {
        using clock = std::chrono::steady_clock;
        auto last = clock::now();
        try {
            while (!pump_stop_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                const auto now = clock::now();
                const double ms = std::chrono::duration<double, std::milli>(now - last).count();
                last = now;
                pumped_ms_ += ms;
                advance_timer(ms);
            }
        } catch (...) {
            pump_error_ = std::current_exception();
        }
    });
}

double LanNetCore::stop_load_pump() {
    if (!pump_.joinable()) throw std::logic_error("LanNetCore: the load pump is not running");
    pump_stop_ = true;
    pump_.join();
    if (pump_error_) std::rethrow_exception(pump_error_);
    return pumped_ms_;
}

void LanNetCore::advance_timer(double milliseconds) {
    timer_ms_ += milliseconds;
    while (timer_ms_ >= kTimerEventMs) {
        timer_ms_ -= kTimerEventMs;
        memory_.write(dme::kT0Mode, 4, memory_.read(dme::kT0Mode) | 0x400U);  // the compare event's flag
        dme_timer_interrupt_004292b0(memory_, *this);
    }
}

}  // namespace omega::lan
