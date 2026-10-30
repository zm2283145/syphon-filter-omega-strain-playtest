#include "npc_generator.hpp"

#include "game_libm.hpp"
#include "vu_float.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>
#include <string>

namespace omega {
namespace {

// vsub.xyzw, vmul.xyz, then vmulax.x / vmadday.x / vmaddz.x with vf3.x =
// vf0.x + vf0.w = 1: (x*x + y*y) + z*z.
float vu_distance_squared(const RootVector& a, const RootVector& b) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    const float x2 = dx * dx, y2 = dy * dy, z2 = dz * dz;
    const float xy = x2 + y2;
    return xy + z2;
}

bool binary32_matches_ee(float v) {
    return std::isfinite(v) && (v == 0.0F || std::fpclassify(v) == FP_NORMAL);
}

// The EE interpreter's binary32 arithmetic: round to nearest, denormal
// results flushed to zero; the VU / FPU clamp of an overflow is not hosted.
float view_fin(float x) {
    if (!std::isfinite(x)) throw std::runtime_error("Generator spawn test 002204c0: non-finite arithmetic (the EE clamp is not hosted)");
    if (x != 0.0F && std::fabs(x) < std::bit_cast<float>(0x00800000U)) return std::copysign(0.0F, x);
    return x;
}
float view_add(float a, float b) { return view_fin(a + b); }
float view_sub(float a, float b) { return view_fin(a - b); }
float view_mul(float a, float b) { return view_fin(a * b); }
template <class F>
const F& view_need(const F& f, const char* what) {
    if (!f) throw std::runtime_error(std::string("Generator spawn test 002204c0: host function not supplied: ") + what);
    return f;
}

void net_geninfo_43b190(const GeneratorNetHost& net) {
    if (!net.geninfo_43b190) throw std::runtime_error("Generator (multiplayer): host function not supplied: geninfo_43b190");
    net.geninfo_43b190();
}

}  // namespace

GeneratorState make_generator_state(const PopulationGenerator& record, std::span<const PopulationNode> nodes) {
    GeneratorState s;
    s.id = record.id;
    s.enabled_2c = record.enabled;
    for (const auto id : record.spawn_nodes) {
        std::int32_t index = -1;
        for (std::size_t i = 0; i < nodes.size(); ++i)
            if (nodes[i].id == id) {
                index = static_cast<std::int32_t>(i);
                break;
            }
        s.spawn_nodes.push_back(index);
    }
    s.interval_84 = record.interval;
    s.alive_cap_88 = static_cast<std::int32_t>(record.alive_cap);
    s.total_cap_8c = static_cast<std::int32_t>(record.total);
    s.no_refund_d2 = record.flag_d2;
    s.spawned_c0 = 0;                          // 00222320
    s.cursor_c4 = 0;
    s.last_c8 = s.cursor_c4;                   // +c8 = +c4
    s.forced_d0 = 0;
    s.timer_bc = s.interval_84;                // +bc = +84
    s.paused_d1 = 0;
    s.min_distance_sq_b8 = 256.0F;             // 00222348: 0x43800000
    return s;
}

void generator_enable(GeneratorState& s) { s.enabled_2c = 1; }       // 00220ff0
void generator_disable(GeneratorState& s) { s.enabled_2c = 0; }      // 00220e40
bool generator_receive_link_message(GeneratorState& s, bool enable) {
    if (enable) generator_enable(s);    // 00221ad8: +64 = 00220fd0
    else generator_disable(s);          // 00221b60: +68 = 00220d70
    return true;                        // 00221b98
}
void generator_pause(GeneratorState& s) { s.paused_d1 = 1; }         // 0021cbac
void generator_unpause(GeneratorState& s) { s.paused_d1 = 0; }       // 0021cb08
void generator_reset_pulse(GeneratorState& s) { s.spawned_c0 = 0; }  // 0021c9b4

void generator_set_min_spawn_distance(GeneratorState& s, float d) {
    if (!binary32_matches_ee(d))
        throw std::runtime_error("SetMinSpawnDistance: non-finite or denormal distance (EE FPU operand semantics not hosted)");
    const float squared = d * d;                                     // 0021ca54 mul.s
    if (!binary32_matches_ee(squared))
        throw std::runtime_error("SetMinSpawnDistance: d * d overflows or is denormal (EE FPU clamp / flush not hosted)");
    s.min_distance_sq_b8 = squared;                                  // 0021ca58
}

std::int32_t generator_alive_count(std::span<const GeneratorSpawnedEntry> spawned) {
    std::int32_t alive = 0;
    for (const auto& entry : spawned) {
        if (!entry.valid) continue;                                  // 0021ee80 movz -> 0021ee84 skips the call
        if (entry.life_108 == 3) continue;                           // 0021eea4
        ++alive;                                                     // 0021eeac
    }
    return alive;
}

std::int32_t generator_thinking_count(std::span<const std::optional<std::uint8_t>> think_life_states) {
    std::int32_t thinking = 0;
    for (const auto& life : think_life_states) {
        if (!life) throw std::runtime_error("0014b220: a think list entry that does not resolve (the original calls +108 on null)");
        if (static_cast<std::uint8_t>(*life - 2U) < 2U) continue;    // 0014b2c8 addiu -2, sltiu 2
        ++thinking;                                                  // 0014b2e8
    }
    return thinking;
}

void generator_spawn_bookkeeping(GeneratorState& s) {
    s.spawned_c0 = static_cast<std::int32_t>(static_cast<std::uint32_t>(s.spawned_c0) + 1U);  // 0021e958
    s.timer_bc = 0.0F;                                                                        // 0021e95c
    s.last_c8 = s.cursor_c4;                                                                  // 0021e964
}

void generator_spawn_actor(GeneratorState& s, std::int32_t node, std::int32_t cursor, const GeneratorActorHost& host) {
    const std::int32_t node_3548 = cursor < 0 ? -1 : node;           // 0021e648 bgez, 0021e654 / 0021e658
    const std::uint32_t draw = host.random();                        // 0021e660
    const std::uint32_t actor = host.create({node_3548, cursor < 0, s.id, s.spawned_c0, draw});  // 0021e7e8..0021e888
    if (actor == 0) throw std::runtime_error("Generator " + std::to_string(s.id) + ": 0021e620 with a null actor (0021e838) is not modelled");
    if (host.network_register) host.network_register(actor);         // 0021e740..0021e7bc (005721c8)
    host.register_actor(actor);                                      // 0021e890..0021e8a8
    host.activate(actor);                                            // 0021e8ac..0021e8bc
    s.spawned_90.push_back({actor, host.actor_id(actor)});           // 0021e8c0..0021e8fc
    s.pending_9c.push_back({actor, host.actor_id(actor)});           // 0021e904..0021e944
}

void generator_spawn_check(GeneratorState& s, float dt, const GeneratorSpawnHost& host,
                           std::optional<std::int32_t>& spawn_stamp_4f56b8, const GeneratorNetHost* net) {
    s.timer_bc = s.timer_bc + dt;                                    // 0021ee28 add.s, 0021ee30
    const std::int32_t alive = host.alive_count();                   // 0021ee50..0021eecc
    const bool network = net && net->network;
    if (net) net_geninfo_43b190(*net);                               // 0021eed8 (nothing in single player)
    if (s.paused_d1 != 0) return;                                    // 0021eee4
    if (network) {                                                   // 0021eeec
        if (!net->owned_43aa40(s.id)) return;                        // 0021ef00
        if (!spawn_stamp_4f56b8)
            throw std::runtime_error("Generator " + std::to_string(s.id) + ": the multiplayer spawn gate 0021ef10 with the stamp 004f56b8 unknown");
        const std::int32_t stamp = *spawn_stamp_4f56b8;              // 0021ef14
        if (!(stamp < net->clock_42b1a0())) return;                  // 0021ef18 slt
    }
    const std::int32_t max_active = host.max_active_npc_count();     // 0021ef38
    const std::int32_t thinking = host.thinking_count();             // 0021ef34
    if (!(thinking < max_active)) return;                            // 0021ef3c slt
    if (s.enabled_2c == 0) return;                                   // 0021ef48: +6c = 0015bfa0 (lbu +2c)
    if (!(s.spawned_c0 < s.total_cap_8c)) return;                    // 0021ef68 slt
    if (s.timer_bc <= s.interval_84) return;                         // 0021ef7c c.ole.s
    if (!(alive < s.alive_cap_88)) return;                           // 0021ef8c slt
    const auto m = static_cast<std::int32_t>(s.spawn_nodes.size());
    if (m == 0) throw std::runtime_error("Generator " + std::to_string(s.id) + " without spawn nodes: the spawn test of the generator itself (0021efa4) is not hosted");
    if (s.forced_d0 == 0) {                                          // 0021efb0 lbu
        const auto next = static_cast<std::int32_t>(static_cast<std::uint32_t>(s.cursor_c4) + 1U);
        s.cursor_c4 = next % m;                                      // 0021efc4 div, mfhi
    }
    if (s.cursor_c4 < 0 || s.cursor_c4 >= m)
        throw std::runtime_error("Generator " + std::to_string(s.id) + ": node cursor outside +7c");
    const std::int32_t node = s.spawn_nodes[static_cast<std::size_t>(s.cursor_c4)];  // 0021efe8
    if (node < 0) throw std::runtime_error("Generator " + std::to_string(s.id) + ": spawn node slot not resolved (0040c480 not hosted)");
    const std::uint8_t result = host.spawn_test(node);               // 0021eff4, 0021effc andi 0xff
    if (result == 1) {
        host.spawn(node, s.cursor_c4);                               // 0021f028 (a2 = +c4 with nodes)
        generator_spawn_bookkeeping(s);
        if (network) {                                               // 0021e968 lbu 005721c8
            if (!host.send_spawn_msg)
                throw std::runtime_error("Generator " + std::to_string(s.id) + ": the spawn message 0021e968 (004de240) is not hosted");
            host.send_spawn_msg();
        }
        const auto clock = host.clock_42b1a0();                      // 0021f030
        spawn_stamp_4f56b8.reset();
        if (clock) spawn_stamp_4f56b8 = static_cast<std::int32_t>(static_cast<std::uint32_t>(*clock) + 0xfaU);  // 0021f038..0021f044
        return;
    }
    if (result == 0 && s.last_c8 == s.cursor_c4) s.timer_bc = s.interval_84 - 1.0F;  // 0021f058, 0021f070 sub.s
}

std::optional<std::uint8_t> generator_spawn_test_prefix(GeneratorState& s, const RootVector& spawn_position,
                                                         std::span<const GeneratorSpawnedEntry> spawned,
                                                         const RootVector& player_translation) {
    const std::uint8_t forced = s.forced_d0;                         // 002204e8 lb
    if (forced != 0) {                                               // 002204f0
        if (forced == 1) s.forced_d0 = 0;                            // 00220b68..
        return forced;
    }
    bool occupied = false;                                           // s3
    for (const auto& entry : spawned) {                              // 00220528..00220654
        if (!entry.valid)
            throw std::runtime_error("002204c0: a spawned entry that does not resolve (the original calls +108 on null)");
        if (entry.life_108 == 3) continue;                           // 00220598
        if (vu_distance_squared(spawn_position, entry.translation) < 9.0F) occupied = true;  // 0022060c c.olt.s 0x41100000
    }
    if (occupied) return std::uint8_t{0};                                          // 0022065c
    // 00220738: the local player (*(*004ffc0c + 30) + 50) transform +40.
    if (vu_distance_squared(spawn_position, player_translation) < s.min_distance_sq_b8) return std::uint8_t{0};  // 002207bc c.olt.s
    return std::nullopt;                                             // 002207d0: s4 = 1, the view cone
}

std::uint8_t generator_spawn_test_view(GeneratorState& s, const RootVector& spawn_position, const GeneratorSpawnViewHost& host) {
    const auto& camera = view_need(host.camera_transform, "camera_transform");
    // 002207e0..00220870: the horizontal direction from the camera to the spawn position.
    RootVector d{};
    {
        const RootVector t = camera(0x40);                           // 002207f8..00220828
        for (std::size_t k = 0; k < 3; ++k) d[k] = view_sub(spawn_position[k], t[k]);   // 0022083c vsub
    }
    d[2] = 0.0F;                                                     // 0022084c swc1 +208
    const float radicand = view_add(view_add(view_mul(d[0], d[0]), view_mul(d[1], d[1])), view_mul(d[2], d[2]));  // 00220854..00220860
    if (radicand == 0.0F)
        throw std::runtime_error("Generator spawn test 002204c0: the spawn position is straight above or below the camera "
                                 "(VRSQRT of 0 at 00220864: the VU's divide-by-zero result is not hosted)");
    const float q = view_fin(vu::rsqrt(radicand));                   // 00220864 vrsqrt
    for (std::size_t k = 0; k < 3; ++k) d[k] = view_mul(d[k], q);   // 0022086c vmulq.xyz
    // 00220874..002208f4: the camera's row 1 with z = 0, dot = (x + y) + z in the w lane.
    RootVector row = camera(0x20);
    row[2] = 0.0F;                                                   // 002208cc swc1 +1f8
    const float dot = view_add(view_add(view_mul(row[0], d[0]), view_mul(row[1], d[1])), view_mul(row[2], d[2]));
    // 002208f8..00220918: cosf(0x3f5f66f3), 1 - c (sub.s), dot + that (add.s), c.olt.s against 1.0.
    const float cone = view_add(dot, view_sub(1.0F, game_cosf(std::bit_cast<float>(0x3f5f66f3U))));
    std::uint8_t result = 1;                                         // 0022091c: outside the view
    if (!(cone < 1.0F)) {
        // 00220928..00220b64: the line of sight from the raised spawn position to the camera's front.
        LosServiceRequest request;
        request.object_50 = view_need(host.local_player, "local_player")();   // 00220934
        request.object_54 = 0;                                       // 00220940 (003ce850 is multiplayer only)
        RootVector origin = spawn_position;                          // 00220974 0015c2c0
        origin[2] = view_add(origin[2], 6.0F);                       // 00220990 add.s 0x40c00000
        const RootVector t = camera(0x40);                           // 00220994..002209d8
        const RootVector f = camera(0x20);                           // 002209d4..00220a14
        RootVector to{};
        for (std::size_t k = 0; k < 3; ++k)
            to[k] = view_sub(view_add(t[k], view_mul(f[k], 1.5F)), origin[k]);  // 00220a20 vmulx 0x3fc00000, 00220a48 vadd, 00220a64 vsub
        origin[3] = 1.0F;                                            // 00220b1c +3c
        to[3] = 0.0F;                                                // 00220b20 +4c
        request.origin_30 = origin;
        request.displacement_40 = to;
        request.mask_58 = 0;                                         // 00220b14
        request.mode_5c = 0;                                         // 00220b18
        request.value_3c = 1.0F;
        request.value_4c = 0.0F;
        result = view_need(host.los, "los")(request).hit ? 1 : 0;    // 00220b2c, 00220b4c lbu result +0
    }
    if (result == 1) s.forced_d0 = 0;                                // 00220b78
    return result;
}

GeneratorWorld::GeneratorWorld(const Population& population) {
    for (const auto& record : population.generators) {
        if (!generators_.emplace(record.id, make_generator_state(record, population.nodes)).second)
            throw std::runtime_error("GeneratorWorld: duplicate generator ID " + std::to_string(record.id));
        order_.push_back(record.id);
    }
}

bool generator_active(const GeneratorState& s) {
    return s.present_2d != 0 && s.enabled_2c != 0 && s.suspended_2e == 0;
}

void generator_prelogic(GeneratorState& s, const GeneratorPrelogicHost& host) {
    auto resolves = [&host](const GeneratorListEntry& e) {           // 0021ff18..0021ff48, 00220410
        return e.actor != 0 && host.actor_id(e.actor) == e.id;
    };
    auto& spawned = s.spawned_90;
    auto& pending = s.pending_9c;
    for (auto it = spawned.begin(); it != spawned.end();) {          // 0021fee0, end re-read at 00220270
        const auto current = it++;                                   // 0021ff4c s2 = node, 0021ff54 s3 = next
        if (!resolves(*current)) continue;                           // 0021ff50
        const std::uint32_t actor = current->actor;                  // 0021ff6c: the handle {actor, actor +c}
        auto match = pending.begin();                                // 0021ff88..0021fffc
        while (match != pending.end() && !(resolves(*match) && match->actor == actor)) ++match;
        if (match != pending.end()) {                                // 00220030
            host.send_spawned_npc_msg(actor);                        // 00220040..00220068
            pending.erase(match);                                    // 0022007c
        }
        if (!generator_active(s)) {                                  // 002200a0..002200cc
            if (host.actor_active(actor)) host.deactivate_60(actor); // 002200d4, 00220114
        } else if (!host.actor_active(actor)) {                      // 00220128
            const auto alive = host.component_alive_44(actor);       // 0022015c, 00220170
            if (alive && *alive) host.activate_5c(actor);            // 0022018c
        }
        if (host.actor_active(actor)) continue;                      // 00220198..002201c4
        const std::uint8_t life = host.life_108(actor);              // 002201d4, andi 0xff
        if (!(static_cast<std::uint8_t>(life - 2U) < 2U) && s.no_refund_d2 == 0)  // 002201e0, 00220200
            s.spawned_c0 = static_cast<std::int32_t>(static_cast<std::uint32_t>(s.spawned_c0) - 1U);  // 00220214
        spawned.erase(current);                                      // 00220224
        host.unregister_40b720(actor);                               // 00220234
        host.script_drop_3d9f40(actor);                              // 00220244
        if (host.object_magic(actor)) host.release_3cb230(actor);    // 0022024c..00220268
    }
    if (s.no_refund_d2 != 0 && !generator_active(s) && spawned.empty()) s.spawned_c0 = 0;  // 00220290..002202dc
    if (spawned.empty() && !pending.empty()) pending.clear();        // 002202e0..00220314
}

std::int32_t deferred_release_delay_ms(float seconds) {
    const float ms = 1000.0F * seconds;                              // 003cb2d0..003cb2e0: lui 0x447a, mul.s
    if (!(std::fabs(ms) < 2147483648.0F)) throw std::runtime_error("003cb230: the delay does not fit cvt.w.s");
    return static_cast<std::int32_t>(ms);                            // 003cb2ec: cvt.w.s truncates
}

bool deferred_release_deletes(bool magic_4, bool removal_pending, bool in_tree) {
    if (!magic_4) return false;                                      // 003cb504..003cb508
    return removal_pending || !in_tree;                              // 0040bfa0..0040bfbc
}

void generator_zone_activate(GeneratorState& s) {
    s.suspended_2e = 0;                                              // 00224330
    generator_activate_5c(s);                                        // +5c(0), then 00224330's end
}

void generator_activate_5c(GeneratorState& s) {
    s.present_2d = 1;                                                // 003ce98c
}

void generator_zone_deactivate(GeneratorState& s, bool forced,
                               const std::function<void(const GeneratorListEntry&)>& deactivate_immediate) {
    if (forced && !s.spawned_90.empty()) {                           // 00220bb0
        if (!deactivate_immediate)
            throw std::runtime_error("Generator " + std::to_string(s.id) + ": a forced deactivation with spawned NPCs (00220bb0's DespawnedNPCMsg / +60(1)) is not hosted");
        const auto nodes = s.spawned_90;                             // 00220bf4..00220cd8: +90 in order
        for (const auto& n : nodes) deactivate_immediate(n);
    }
    s.present_2d = 0;                                                // 003ce790 (00220bb0, +60(0) = 00221460), then 00224180's end
}

void GeneratorWorld::tick(float dt, const GeneratorPrelogicHost& prelogic,
                          const std::function<GeneratorSpawnHost(GeneratorState&)>& spawn_host,
                          std::optional<std::int32_t>& spawn_stamp_4f56b8, const GeneratorNetPrelogicHost* mp,
                          const GeneratorNetHost* net) {
    const bool network = net && net->network;
    if (network && !mp) throw std::runtime_error("GeneratorWorld::tick: a network game without the prelogic's multiplayer host (0021f0dc)");
    for (const auto id : order_) {                                   // 00133420, registration order
        auto& s = generators_.at(id);
        if (network) generator_prelogic_network(s, prelogic, *mp, *net);  // 0021f0d4: 005721c8 set
        else generator_prelogic(s, prelogic);                        // 001336c4: +4c = 0021f0a0
        if (!generator_active(s)) continue;                          // the gate read after +4c
        generator_spawn_check(s, dt, spawn_host(s), spawn_stamp_4f56b8, net);  // 0013370c: +48 = 0021edf0
    }
}

namespace {

template <class F>
const F& net_need(const F& f, const char* what) {
    if (!f) throw std::runtime_error(std::string("Generator (multiplayer): host function not supplied: ") + what);
    return f;
}
std::uint32_t client_bit(std::int32_t client) { return 1U << (static_cast<std::uint32_t>(client) & 31U); }  // sllv
// The idiom 005721c8 ? (*00494178, 002ebaf8 when -1) : 0.
std::int32_t net_local(const GeneratorNetHost& net) { return net.network ? net_need(net.local_slot, "local_slot")() : 0; }
// 003ce850's player +c word -> ((w & 0x7f000000) >> 24) - 1 (0022104c..00221068).
std::int32_t player_client(std::uint32_t word) { return static_cast<std::int32_t>((word & 0x7f000000U) >> 24) - 1; }

}  // namespace

void generator_network_construct(GeneratorState& s, const GeneratorNetHost& net,
                                 std::optional<std::int32_t>& spawn_stamp_4f56b8) {
    if (!net.network) return;                                        // 00222358
    s.handoff_d3 = 0;                                                // 00222360
    s.interest_cc = s.enabled_2c != 0 ? 0xffU : 0U;                  // 00222364 movz on the record byte
    const auto host_client = net_need(net.session_host_2ebba0, "session_host_2ebba0")();  // 00222374
    if (!host_client)
        throw std::runtime_error("Generator " + std::to_string(s.id) + ": 002ebba0 did not write its out word (0022237c reads the stack)");
    net_need(net.netmap_insert_3fb650, "netmap_insert_3fb650")(s.id, *host_client);  // 00222384
    if (net.host_role) {                                             // 002223a0
        net_need(net.geninfo_43b190, "geninfo_43b190")();            // 002223ac
        net_need(net.add_4398e0, "add_4398e0")(s.id, -1);            // 002223bc
    }
    spawn_stamp_4f56b8 = static_cast<std::int32_t>(static_cast<std::uint32_t>(net_need(net.clock_42b1a0, "clock_42b1a0")()) + 0xfaU);  // 002223c8..002223d8
}

void generator_enable_network(GeneratorState& s, std::int32_t a1, const GeneratorNetHost& net) {
    s.enabled_2c = 1;                                                // 00220ff0
    if (!net.network) return;                                        // 00220ff8
    if (generator_active(s)) {                                       // 00221000..0022102c
        net_need(net.geninfo_43b190, "geninfo_43b190")();            // 00221034
        if (a1 < 0) {                                                // 0022103c
            const auto p = net_need(net.nearest_player_3ce850, "nearest_player_3ce850")(s.id);  // 00221044
            const std::int32_t owner = p ? player_client(*p) : net_local(net);
            net_need(net.set_owner_439d00, "set_owner_439d00")(s.id, owner);  // 002210bc
        } else if (a1 == net_local(net) && s.interest_cc == 0) {     // 00221118, 00221124
            net_need(net.set_owner_439d00, "set_owner_439d00")(s.id, net_local(net));  // 0022117c
        }
        net_need(net.set_interest_43a420, "set_interest_43a420")(s.id);  // 0022118c
    } else if (a1 < 0) {                                             // 002211a0
        const auto p = net_need(net.nearest_player_3ce850, "nearest_player_3ce850")(s.id);  // 002211ac
        const std::int32_t client = p ? player_client(*p) : -1;
        if (client != net_local(net)) {                              // 00221218
            net_need(net.geninfo_43b190, "geninfo_43b190")();        // 00221220
            net_need(net.set_owner_439d00, "set_owner_439d00")(s.id, client);  // 00221230
        }
    }
    if (a1 >= 0) s.interest_cc |= client_bit(a1);                    // 00221248
    else s.interest_cc = 0xffU;                                      // 00221258
    net_need(net.send_enable_3ff930, "send_enable_3ff930")(s.id, a1);      // 00221260..: cEnableMsg, 003ff930, 003c9d60
}

void generator_disable_network(GeneratorState& s, std::int32_t a1, const GeneratorNetHost& net) {
    if (!net.network) {                                              // 00220d8c
        s.enabled_2c = 0;                                            // 00220e40
        return;
    }
    if (a1 >= 0) s.interest_cc &= ~client_bit(a1);                   // 00220da0..00220db0
    if (s.interest_cc == 0) {                                        // 00220dbc
        net_need(net.geninfo_43b190, "geninfo_43b190")();            // 00220dc4
        net_need(net.clear_interest_43a030, "clear_interest_43a030")(s.id);  // 00220dd0
        s.enabled_2c = 0;                                            // 00220dd8
    }
    net_need(net.send_disable_3ff650, "send_disable_3ff650")(s.id, a1);    // 00220de0..: cDisableMsg, 003ff650(.., -1), 003c9d60
}

void generator_remote_enable(GeneratorState& s, std::int32_t client, const GeneratorNetHost& net) {
    s.enabled_2c = 1;                                                // 00220e7c
    if (generator_active(s)) {                                       // 00220e80..00220eac
        net_need(net.geninfo_43b190, "geninfo_43b190")();            // 00220eb4
        if (client == net_local(net) && s.interest_cc == 0)          // 00220f08, 00220f14
            net_need(net.set_owner_439d00, "set_owner_439d00")(s.id, net_local(net));
        net_need(net.set_interest_43a420, "set_interest_43a420")(s.id);
    }
    if (client >= 0) s.interest_cc |= client_bit(client);
    else s.interest_cc = 0xffU;
}

void generator_remote_disable(GeneratorState& s, std::int32_t client, const GeneratorNetHost& net) {
    if (client >= 0) s.interest_cc &= ~client_bit(client);           // 00220d14..00220d2c
    else s.interest_cc = 0;                                          // 00220d30
    if (s.interest_cc != 0) return;                                  // 00220d3c
    net_need(net.geninfo_43b190, "geninfo_43b190")();                // 00220d44
    s.enabled_2c = 0;                                                // 00220d48 (the delay slot)
    net_need(net.clear_interest_43a030, "clear_interest_43a030")(s.id);  // 00220d50
}

void generator_receive_enable_disable(GeneratorState& s, bool enable, std::int32_t client,
                                      const GeneratorNetHost& net) {
    if (!net.network) {
        generator_receive_link_message(s, enable);
    } else if (net.host_role) {
        if (enable) generator_remote_enable(s, client, net);
        else generator_remote_disable(s, client, net);
    } else {
        if (enable) generator_enable_network(s, client, net);
        else generator_disable_network(s, client, net);
    }
}

void generator_activate_5c_network(GeneratorState& s, const GeneratorNetHost& net) {
    s.present_2d = 1;                                                // 002214c4: 003ce980
    if (!net.network || !generator_active(s)) return;                // 002214d4, 002214dc..00221508
    net_need(net.geninfo_43b190, "geninfo_43b190")();                // 00221510
    if ((s.interest_cc & client_bit(net_local(net))) != 0 &&         // 00221560..00221570
        !net_need(net.claimed_43ab20, "claimed_43ab20")(s.id))       // 00221578
        net_need(net.set_owner_439d00, "set_owner_439d00")(s.id, net_local(net));  // 002215d4
    net_need(net.set_interest_43a420, "set_interest_43a420")(s.id);  // 002215e4
}

void generator_deactivate_60_network(GeneratorState& s, const GeneratorNetHost& net) {
    s.present_2d = 0;                                                // 00221470: 003ce790
    if (!net.network) return;                                        // 00221480
    net_need(net.geninfo_43b190, "geninfo_43b190")();                // 00221488
    net_need(net.clear_interest_43a030, "clear_interest_43a030")(s.id);  // 00221494
}

void generator_zone_activate_network(GeneratorState& s, const GeneratorNetHost& net) {
    s.suspended_2e = 0;                                              // 00224330
    generator_activate_5c_network(s, net);                           // +5c(0) = 002214b0
}

void generator_zone_deactivate_network(GeneratorState& s, bool forced, const GeneratorNetHost& net) {
    generator_zone_deactivate(s, forced);                            // 00220bb0 when forced
    generator_deactivate_60_network(s, net);                         // +60(0) = 00221460
    s.present_2d = 0;                                                // 00224180's end
}

void generator_owner_sync(GeneratorState& s, const GeneratorPrelogicHost& host, const GeneratorNetPrelogicHost& mp,
                          const GeneratorNetHost& net) {
    net_need(net.geninfo_43b190, "geninfo_43b190")();                // 0021eac0
    if (!net_need(net.claimed_43ab20, "claimed_43ab20")(s.id)) return;  // 0021eacc
    const std::int32_t local = net_local(net);                       // 0021eadc..0021eb20
    const bool owner = net_need(net.owner_439c70, "owner_439c70")(s.id) == local;  // 0021eb34
    for (const auto& e : s.spawned_90) {                             // 0021eb44 / 0021ec88
        if (e.actor == 0 || host.actor_id(e.actor) != e.id) continue;  // 0021eb78..0021ebac
        const auto answer = owner ? mp.component_5c(e.actor) : mp.component_58(e.actor);  // 0021ebb4 / 0021ecf4
        if (!answer || !*answer) continue;
        mp.send_control_msg(e.actor, owner ? 1 : 2);                 // 0021ebe8..0021ec10 / 0021ed28..0021ed54
        mp.message_destroyed();                                      // 0021ec28 / 0021ed6c
        mp.set_actor_14(e.actor, owner ? 1 : 0);                     // 0021ec38 / 0021ed74
        const auto id = host.actor_id(e.actor);                      // 0021ec3c / 0021ed7c
        mp.netmap_set_client_3fb4b0(id, net.owner_439c70(s.id));     // 0021ec40..0021ec4c
    }
}

void generator_prelogic_network(GeneratorState& s, const GeneratorPrelogicHost& host,
                                const GeneratorNetPrelogicHost& mp, const GeneratorNetHost& net) {
    auto resolves = [&host](const GeneratorListEntry& e) { return e.actor != 0 && host.actor_id(e.actor) == e.id; };
    auto& spawned = s.spawned_90;
    auto& pending = s.pending_9c;
    auto find_pending = [&](std::uint32_t actor) {                   // 0021f17c / 0021fad0: only with +9c's count > 0
        auto m = pending.begin();
        while (m != pending.end() && !(resolves(*m) && m->actor == actor)) ++m;
        return m;
    };
    // The despawn of the owner (0021f6c8..0021f7e0, 0021fbd8..0021fce4).
    auto despawn = [&](std::list<GeneratorListEntry>::iterator node) {
        const std::uint32_t actor = node->actor;
        mp.send_despawn_msg(actor);
        const std::uint8_t life = host.life_108(actor);
        if (!(static_cast<std::uint8_t>(life - 2U) < 2U) && s.no_refund_d2 == 0)
            s.spawned_c0 = static_cast<std::int32_t>(static_cast<std::uint32_t>(s.spawned_c0) - 1U);
        spawned.erase(node);
        host.unregister_40b720(actor);
        host.script_drop_3d9f40(actor);
        if (host.object_magic(actor)) host.release_3cb230(actor);
        mp.message_destroyed();
    };
    auto deactivate = [&](std::uint32_t actor) {                     // 0021f3b4..0021f414 / 0021f4d0..0021f528
        const auto c = mp.component_58(actor);
        if (c && *c) mp.deactivate_600(actor);
        else mp.actor_24(actor);
    };
    net_need(net.geninfo_43b190, "geninfo_43b190")();                // 0021f0dc
    mp.npc_info_begin_436c00();                                      // 0021f0e4
    const std::int32_t owner = net_need(net.owner_439c70, "owner_439c70")(s.id);  // 0021f0f0: s4
    for (auto it = spawned.begin(); it != spawned.end();) {          // 0021f0fc, the end re-read at 0021f800
        const auto current = it++;                                   // 0021f164 s7, 0021f16c s1 = next
        if (!resolves(*current)) {                                   // 0021f168
            spawned.erase(current);                                  // 0021f7e8
            continue;
        }
        const std::uint32_t actor = current->actor;
        const auto match = find_pending(actor);
        const bool not_pending = match == pending.end();             // 0021f258: s2
        bool drop = false;                                           // s6
        if (owner >= 0 && !not_pending) {                            // 0021f260, 0021f268
            if (mp.npc_owner_434440(host.actor_id(actor)) == owner) {  // 0021f274
                host.send_spawned_npc_msg(actor);                    // 0021f284..0021f2bc
                pending.erase(match);                                // 0021f2d0
                if (host.actor_active(actor)) mp.npc_apply_4341f0(host.actor_id(actor));  // 0021f2d8..0021f310
                mp.message_destroyed();                              // 0021f328
            } else {
                const auto alive = host.component_alive_44(actor);   // 0021f338..0021f34c
                if (alive && !*alive) pending.erase(match);          // 0021f368
            }
        }
        if (owner < 0) {                                             // 0021f370
            if (not_pending) {                                       // 0021f378
                if (host.actor_active(actor)) deactivate(actor);     // 0021f380..0021f414
                const auto c = mp.component_58(actor);               // 0021f418..0021f42c
                if (c && *c && !mp.npc_known_4340c0(host.actor_id(actor))) drop = true;  // 0021f44c
            }
        } else if (!generator_active(s)) {                           // 0021f468..0021f494
            if (host.actor_active(actor)) deactivate(actor);         // 0021f49c..0021f528
            else if (not_pending) {                                  // 0021f538
                const auto alive = host.component_alive_44(actor);   // 0021f540..0021f554
                if (alive && !*alive) drop = true;
            }
        } else if (!host.actor_active(actor)) {                      // 0021f570..0021f59c
            const auto alive = host.component_alive_44(actor);       // 0021f5a4..0021f5b8
            if (alive && *alive) {
                const auto c = mp.component_58(actor);               // 0021f5c8..0021f5dc
                if (c && *c) mp.activate_5c0(actor);                 // 0021f600
                else {
                    mp.actor_20(actor);                              // 0021f61c
                    if (mp.actor_3584(actor) == 0) {                 // 0021f628
                        const auto object = mp.netmap_object_3fbb10(mp.actor_key_10(actor));  // 0021f634
                        if (object) mp.attach_185970(actor, object); // 0021f648
                    }
                }
            } else if (not_pending) drop = true;                     // 0021f658
        }
        if (drop && owner == net_local(net)) despawn(current);       // 0021f668..0021f6c0
    }
    generator_owner_sync(s, host, mp, net);                          // 0021f820
    if (owner == net_local(net)) {                                   // 0021f828..0021f878
        bool hand_off = false;                                       // s2
        auto first_interested = [&](std::int32_t from) {             // 0021f8d8..0021f928 / 0021f9c0..0021fa10
            for (std::int32_t c = from; c < 4; ++c)
                if ((s.interest_cc & client_bit(c)) != 0 && net.interest_43aba0(s.id, c) &&
                    net_need(net.client_player_3fb8d0, "client_player_3fb8d0")(c))
                    return c;
            return -1;
        };
        auto give = [&](std::int32_t from) {
            const std::int32_t c = first_interested(from);
            if (c >= 0) net.set_owner_439d00(s.id, c);               // 0021f940 / 0021fa28
            else {
                net.set_owner_439d00(s.id, -1);                      // 0021f958 / 0021fa40
                hand_off = true;
            }
        };
        if (generator_active(s)) s.handoff_d3 = 1;                   // 0021f8b8
        else if (s.handoff_d3 != 0) give(0);                         // 0021f8c0
        else if (net.network ? net.host_role : true) {               // 0021f968..0021f988
            if (!(net_need(net.interest_43aba0, "interest_43aba0")(s.id, -1) && (s.interest_cc & 1U) != 0))  // 0021f998..0021f9b0
                give(1);
        }
        if (hand_off) {                                              // 0021fa50
            for (auto it = spawned.begin(); it != spawned.end();) {  // 0021fa58
                const auto current = it++;                           // 0021fac4 s0, 0021facc s1 = next
                if (!resolves(*current)) continue;                   // 0021fac8
                const auto match = find_pending(current->actor);     // 0021fad0..0021fbb8
                if (match != pending.end()) pending.erase(match);    // 0021fbcc
                despawn(current);                                    // 0021fbd8..0021fce4
            }
        }
    } else {
        s.handoff_d3 = 0;                                            // 0021fd10
    }
    if ((net.network ? net.host_role : true) && net.claimed_43ab20(s.id)) {  // 0021fd18..0021fd4c
        for (const auto& e : spawned) {                              // 0021fd54..0021fe44
            if (!resolves(e))
                throw std::runtime_error("Generator " + std::to_string(s.id) + ": 0021fdb4 reads +c of a spawned node that does not resolve (null)");
            const auto client = mp.netmap_client_3fb340(host.actor_id(e.actor));  // 0021fdb4
            if (client != net.owner_439c70(s.id))                    // 0021fdc4
                mp.netmap_set_client_3fb4b0(host.actor_id(e.actor), net.owner_439c70(s.id));  // 0021fe0c..0021fe1c
        }
    }
    if (s.no_refund_d2 != 0 && !net.claimed_43ab20(s.id) && spawned.empty()) s.spawned_c0 = 0;  // 0021fe50..0021fe7c
    for (std::int32_t c = 0; c < 4; ++c)                             // 0021fe80..0021fec0
        if (!net_need(net.client_player_3fb8d0, "client_player_3fb8d0")(c) && (s.interest_cc & client_bit(c)) != 0)
            s.interest_cc ^= client_bit(c);
    mp.npc_info_end_436ba0();                                        // 0021fecc
    if (spawned.empty() && !pending.empty()) pending.clear();        // 002202e0..00220314
}

GeneratorState* GeneratorWorld::find(std::uint32_t id) {
    const auto it = generators_.find(id);
    return it == generators_.end() ? nullptr : &it->second;
}

const GeneratorState* GeneratorWorld::find(std::uint32_t id) const {
    const auto it = generators_.find(id);
    return it == generators_.end() ? nullptr : &it->second;
}

std::size_t GeneratorWorld::unresolved_nodes() const {
    std::size_t n = 0;
    for (const auto& [id, s] : generators_)
        for (const auto node : s.spawn_nodes)
            if (node < 0) ++n;
    return n;
}

void generator_spawn_receive(GeneratorState& s, const GeneratorSpawnMessage& msg, const GeneratorSpawnReceiveHost& host,
                             const GeneratorNetHost& net) {
    const auto need = [&s](const auto& f, const char* what) -> const auto& {
        if (!f) throw std::runtime_error("Generator " + std::to_string(s.id) + ": 0021e0c0 host function not supplied: " + what);
        return f;
    };
    const std::uint32_t actor = need(host.create, "create")(msg);    // 0021e0f0..0021e2a0
    if (actor == 0) throw std::runtime_error("Generator " + std::to_string(s.id) + ": 0021e0c0 with a null actor is not modelled");
    need(host.register_actor, "register_actor")(actor);              // 0021e2a8..0021e2bc
    need(host.set_key_185940, "set_key_185940")(actor, msg.key_54);  // 0021e2c0..0021e2c4
    if (net.geninfo_43b190) net.geninfo_43b190();                    // 0021e2cc
    if (generator_active(s)) {                                       // 0021e2d4..0021e318
        need(host.activate_20, "activate_20")(actor);                // 0021e320 virtual +20
        s.spawned_c0 = static_cast<std::int32_t>(static_cast<std::uint32_t>(s.spawned_c0) + 1U);  // 0021e330
    } else {
        if (s.no_refund_d2 != 0)                                     // 0021e340
            s.spawned_c0 = static_cast<std::int32_t>(static_cast<std::uint32_t>(s.spawned_c0) + 1U);
        need(host.remote_controller, "remote_controller")(actor);    // 0021e358..0021e408
        const std::int32_t owner = net_need(net.owner_439c70, "owner_439c70")(s.id);  // 0021e414
        need(host.netmap_insert_3fb650, "netmap_insert_3fb650")(actor, owner);        // 0021e424
    }
    if (!net.network || net.host_role) {                             // 0021e430..0021e450
        if (net.geninfo_43b190) net.geninfo_43b190();                // 0021e458
        const std::int32_t owner = net_need(net.owner_439c70, "owner_439c70")(s.id);  // 0021e470
        need(host.npc_info_merge_434600, "npc_info_merge_434600")(actor, owner);      // 0021e460..0021e490
    }
    need(host.place_msg, "place_msg")(actor);                        // 0021e498..0021e510
    need(host.attach_185970, "attach_185970")(actor);                // 0021e514..0021e524
    need(host.mark_remote, "mark_remote")(actor);                    // 0021e530..0021e548
    const auto& id = need(host.actor_id, "actor_id");
    s.spawned_90.push_back({actor, id(actor)});                      // 0021e550..0021e58c
    s.pending_9c.push_back({actor, id(actor)});                      // 0021e594..0021e5d4
    s.timer_bc = 0.0F;                                               // 0021e5dc
    s.last_c8 = s.cursor_c4;                                         // 0021e5e4
}

bool generator_despawn_receive(GeneratorState& s, std::uint32_t actor, const GeneratorPrelogicHost& host,
                               const GeneratorNetPrelogicHost& mp) {
    const auto need = [&s](const auto& f, const char* what) -> const auto& {
        if (!f) throw std::runtime_error("Generator " + std::to_string(s.id) + ": 002216c8 host function not supplied: " + what);
        return f;
    };
    const auto c = need(mp.component_58, "component_58")(actor);    // 00221700..00221724
    if (c && *c) need(mp.deactivate_600, "deactivate_600")(actor);   // 00221730..0022173c
    else need(mp.actor_24, "actor_24")(actor);                       // 00221750..00221758
    const auto& id = need(host.actor_id, "actor_id");
    const auto same = [&](const GeneratorListEntry& e) { return e.actor == actor && id(e.actor) == e.id; };  // 00220410
    auto& spawned = s.spawned_90;
    const auto node = std::find_if(spawned.begin(), spawned.end(), same);  // 00221760..00221840
    const bool found = node != spawned.end();
    if (found) {
        if (!s.pending_9c.empty()) {                                 // 00221848
            const auto match = std::find_if(s.pending_9c.begin(), s.pending_9c.end(), same);  // 00221854..00221930
            if (match != s.pending_9c.end()) s.pending_9c.erase(match);  // 00221940
        }
        const std::uint8_t life = need(host.life_108, "life_108")(actor);  // 00221948..00221958
        if (!(static_cast<std::uint8_t>(life - 2U) < 2U) && s.no_refund_d2 == 0)  // 0022195c..0022197c
            s.spawned_c0 = static_cast<std::int32_t>(static_cast<std::uint32_t>(s.spawned_c0) - 1U);
        spawned.erase(node);                                         // 0022199c
    }
    need(host.unregister_40b720, "unregister_40b720")(actor);        // 002219b0
    need(host.script_drop_3d9f40, "script_drop_3d9f40")(actor);      // 002219c0
    if (need(host.object_magic, "object_magic")(actor))              // 002219c8..002219d4
        need(host.release_3cb230, "release_3cb230")(actor);          // 002219e0
    return found;
}

}  // namespace omega
