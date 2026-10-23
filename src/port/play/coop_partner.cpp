#include "coop_partner.hpp"
#include "npc_perception.hpp"
#include "target_visibility.hpp"
#include "world_los.hpp"
#include <algorithm>

#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace omega {
namespace {
float horizontal_distance(const RootVector& a, const std::array<float, 3>& b) {
    const float dx = a[0] - b[0], dy = a[1] - b[1];
    return std::sqrt(dx * dx + dy * dy);
}
float horizontal_distance(const RootVector& a, const RootVector& b) {
    return horizontal_distance(a, std::array<float, 3>{b[0], b[1], b[2]});
}
float horizontal_distance(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    const float dx = a[0] - b[0], dy = a[1] - b[1];
    return std::sqrt(dx * dx + dy * dy);
}
}

void AgentSlots::assign(std::size_t slot, AgentDriver driver, std::size_t actor, std::uint32_t object_id) {
    if (slot >= count) throw std::runtime_error("Agent slots: slot " + std::to_string(slot) + " out of range");
    if (driver == AgentDriver::empty) throw std::runtime_error("Agent slots: assign needs a driver");
    if (slots_[slot].driver != AgentDriver::empty)
        throw std::runtime_error("Agent slots: slot " + std::to_string(slot) + " is occupied");
    if (slot_of_actor(actor)) throw std::runtime_error("Agent slots: actor " + std::to_string(actor) + " already has a slot");
    slots_[slot] = {driver, actor, object_id};
}

void AgentSlots::release(std::size_t slot) {
    if (slot >= count) throw std::runtime_error("Agent slots: slot " + std::to_string(slot) + " out of range");
    slots_[slot] = {};
}

const AgentSlot& AgentSlots::slot(std::size_t index) const {
    if (index >= count) throw std::runtime_error("Agent slots: slot " + std::to_string(index) + " out of range");
    return slots_[index];
}

std::optional<std::size_t> AgentSlots::slot_of_actor(std::size_t actor) const {
    for (std::size_t k = 0; k < count; ++k)
        if (slots_[k].driver != AgentDriver::empty && slots_[k].actor == actor) return k;
    return std::nullopt;
}

std::optional<std::size_t> AgentSlots::first_free() const {
    for (std::size_t k = 0; k < count; ++k)
        if (slots_[k].driver == AgentDriver::empty) return k;
    return std::nullopt;
}

std::size_t AgentSlots::occupied() const {
    std::size_t n = 0;
    for (const auto& s : slots_) n += s.driver != AgentDriver::empty;
    return n;
}

const char* agent_driver_name(AgentDriver driver) {
    switch (driver) {
    case AgentDriver::empty: return "empty";
    case AgentDriver::local_pad: return "local pad";
    case AgentDriver::cpu_partner: return "CPU partner";
    case AgentDriver::remote: return "remote";
    }
    return "?";
}

std::optional<std::int32_t> coop_partner_nearest_node(const NpcNodeGraph& graph, const RootVector& point, float max_dz,
                                                      float min_distance) {
    std::optional<std::int32_t> best;
    float best_distance = std::numeric_limits<float>::max();
    for (std::size_t k = 0; k < graph.nodes.size(); ++k) {
        const auto& n = graph.nodes[k];
        if (!(n.flags & npc_node_nav) || n.links.empty()) continue;
        if (std::fabs(n.position[2] - point[2]) > max_dz) continue;
        const float d = horizontal_distance(point, n.position);
        if (d < min_distance || !(d < best_distance)) continue;
        best_distance = d;
        best = static_cast<std::int32_t>(k);
    }
    return best;
}

bool coop_partner_crawl_interior(const NpcNodeGraph& graph, std::size_t node) {
    if (node >= graph.nodes.size()) return false;
    const auto crawl = [](const NpcNode::Link& l) { return l.type == 8; };
    const auto& links = graph.nodes[node].links;
    const bool overlay = node < graph.port_overlay.size() && !graph.port_overlay[node].empty();
    if (links.empty() && !overlay) return false;
    return std::all_of(links.begin(), links.end(), crawl) &&
           (!overlay || std::all_of(graph.port_overlay[node].begin(), graph.port_overlay[node].end(), crawl));
}

namespace {
std::optional<std::int32_t> follow_node_filtered(const NpcNodeGraph& graph, const RootVector& player,
                                                 const RootVector& partner, float follow_distance, float max_dz,
                                                 float far_distance, const std::function<bool(std::size_t)>& skip) {
    // Checkpoint 132ob11: a crawl's interior node (every link of type 8, NOD or overlay) is not a goal. The
    // NOD's crawls are crossed whole (136789 ... 136795 have only type-8 links; the chain is entered from
    // 135871 and left through 136796's type-10 link), so the partner crosses a crawl in one route, behind
    // the player, instead of parking in the passage.
    const auto interior = [&graph, &skip](std::size_t k) {
        return coop_partner_crawl_interior(graph, k) || (skip && skip(k));
    };
    std::optional<std::int32_t> behind;
    float best_distance = std::numeric_limits<float>::max();
    const float tx = partner[0] - player[0];
    const float ty = partner[1] - player[1];
    for (std::size_t k = 0; k < graph.nodes.size(); ++k) {
        const auto& n = graph.nodes[k];
        if (!(n.flags & npc_node_nav) || n.links.empty()) continue;
        if (std::fabs(n.position[2] - player[2]) > max_dz) continue;
        if ((n.position[0] - player[0]) * tx + (n.position[1] - player[1]) * ty < 0.0F) continue;
        const float d = horizontal_distance(player, n.position);
        if (d > follow_distance || !(d < best_distance)) continue;
        if (interior(k)) continue;
        best_distance = d;
        behind = static_cast<std::int32_t>(k);
    }
    if (behind) return behind;
    std::optional<std::int32_t> nearest;
    best_distance = std::numeric_limits<float>::max();
    for (std::size_t k = 0; k < graph.nodes.size(); ++k) {
        const auto& n = graph.nodes[k];
        if (!(n.flags & npc_node_nav) || n.links.empty() || interior(k)) continue;
        if (std::fabs(n.position[2] - player[2]) > max_dz) continue;
        const float d = horizontal_distance(player, n.position);
        if (!(d < best_distance)) continue;
        best_distance = d;
        nearest = static_cast<std::int32_t>(k);
    }
    // Checkpoint 132pw3 (port-only): a player standing where no Nav node lies within max_dz of its height
    // nearby (TORONTO1's bank steps and the plaza at 4.5 .. 5.1, over a street at 0.8) would otherwise send
    // the partner to the nearest node at that height anywhere in the level (the overlay ladder top
    // 2147483654 at 6.5, 149 m off), where it stuck. When that node lies farther than far_distance, the
    // node nearest the player in 3D within far_distance whose height lies in the band just below (player z -
    // 2 max_dz .. player z + max_dz) wins: the street node below the steps. Floors farther below (TORONTO2's
    // lower level 8.5 m down, TORONTO3's yard 20 m down, BEIJING1's street 7.1 m down) keep the old goal.
    if (!nearest || !(best_distance > far_distance)) return nearest;
    const auto distance3 = [&player](const auto& p) {
        const float dx = p[0] - player[0], dy = p[1] - player[1], dz = p[2] - player[2];
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    };
    std::optional<std::int32_t> below;
    float below_distance = far_distance;
    for (std::size_t k = 0; k < graph.nodes.size(); ++k) {
        const auto& n = graph.nodes[k];
        if (!(n.flags & npc_node_nav) || n.links.empty() || interior(k)) continue;
        if (n.position[2] > player[2] + max_dz || n.position[2] < player[2] - 2.0F * max_dz) continue;
        const float d = distance3(n.position);
        if (!(d <= below_distance)) continue;
        below_distance = d;
        below = static_cast<std::int32_t>(k);
    }
    return below ? below : nearest;
}
} // namespace

std::optional<std::int32_t> coop_partner_follow_node(const NpcNodeGraph& graph, const RootVector& player,
                                                     const RootVector& partner, float follow_distance, float max_dz,
                                                     float far_distance) {
    return follow_node_filtered(graph, player, partner, follow_distance, max_dz, far_distance, {});
}

std::optional<std::int32_t> coop_partner_follow_node_spaced(const NpcNodeGraph& graph, const RootVector& player,
                                                            const RootVector& partner, float follow_distance, float max_dz,
                                                            float far_distance, const std::vector<RootVector>& avoid,
                                                            float avoid_radius) {
    if (avoid.empty()) return coop_partner_follow_node(graph, player, partner, follow_distance, max_dz, far_distance);
    const auto taken = [&](std::size_t k) {
        return std::any_of(avoid.begin(), avoid.end(), [&](const RootVector& a) {
            return horizontal_distance(a, graph.nodes[k].position) <= avoid_radius;
        });
    };
    if (const auto node = follow_node_filtered(graph, player, partner, follow_distance, max_dz, far_distance, taken)) return node;
    return coop_partner_follow_node(graph, player, partner, follow_distance, max_dz, far_distance);
}

std::vector<std::int32_t> coop_partner_start_nodes(const NpcNodeGraph& graph, const RootVector& start, float max_dz,
                                                   float min_distance, std::size_t count, float spacing) {
    std::vector<std::int32_t> nodes;
    const auto first = coop_partner_nearest_node(graph, start, max_dz, min_distance);
    if (!first || count == 0) return nodes;
    nodes.push_back(*first);
    while (nodes.size() < count) {
        std::optional<std::int32_t> best;
        float best_distance = std::numeric_limits<float>::max();
        for (std::size_t k = 0; k < graph.nodes.size(); ++k) {
            const auto& n = graph.nodes[k];
            if (!(n.flags & npc_node_nav) || n.links.empty()) continue;
            // 132pw5: never a crawl's interior node (coop_partner_crawl_interior): TORONTO1's 136789 under the
            // wreck stood partner 3 on the wreck's roof (z 4.7, no route anywhere) at a test teleport.
            if (coop_partner_crawl_interior(graph, k)) continue;
            if (std::fabs(n.position[2] - start[2]) > max_dz) continue;
            const float d = horizontal_distance(start, n.position);
            if (d < min_distance || !(d < best_distance)) continue;
            if (std::any_of(nodes.begin(), nodes.end(), [&](std::int32_t m) {
                    return horizontal_distance(graph.nodes[static_cast<std::size_t>(m)].position, n.position) < spacing;
                }))
                continue;
            best_distance = d;
            best = static_cast<std::int32_t>(k);
        }
        nodes.push_back(best ? *best : *first);
    }
    return nodes;
}

std::vector<bool> coop_partner_reachable(const NpcNodeGraph& graph, std::int32_t start, bool overlay) {
    std::vector<bool> seen;
    if (start < 0 || static_cast<std::size_t>(start) >= graph.nodes.size()) return seen;
    seen.assign(graph.nodes.size(), false);
    std::vector<std::int32_t> open{start};
    seen[static_cast<std::size_t>(start)] = true;
    while (!open.empty()) {
        const auto k = static_cast<std::size_t>(open.back());
        open.pop_back();
        if (static_cast<std::int32_t>(k) != start && !(graph.nodes[k].flags & 0x88U)) continue;
        const auto visit = [&](const std::vector<NpcNode::Link>& links) {
            for (const auto& l : links) {
                if (l.type == 7 || l.target < 0 || static_cast<std::size_t>(l.target) >= graph.nodes.size()) continue;
                if (seen[static_cast<std::size_t>(l.target)]) continue;
                seen[static_cast<std::size_t>(l.target)] = true;
                open.push_back(l.target);
            }
        };
        visit(graph.nodes[k].links);
        if (overlay && k < graph.port_overlay.size()) visit(graph.port_overlay[k]);
    }
    return seen;
}

std::vector<std::int32_t> coop_partner_reachable_nodes(const NpcNodeGraph& graph, const std::vector<bool>& reachable,
                                                       std::size_t limit, const RootVector& point, float max_dz,
                                                       std::size_t count, float spacing) {
    std::vector<std::int32_t> nodes;
    const std::size_t n_max = std::min({graph.nodes.size(), reachable.size(), limit});
    while (nodes.size() < count) {
        std::optional<std::int32_t> best;
        float best_distance = std::numeric_limits<float>::max();
        for (std::size_t k = 0; k < n_max; ++k) {
            const auto& n = graph.nodes[k];
            if (!reachable[k] || !(n.flags & npc_node_nav) || n.links.empty() || coop_partner_crawl_interior(graph, k)) continue;
            if (std::fabs(n.position[2] - point[2]) > max_dz) continue;
            const float d = horizontal_distance(point, n.position);
            if (!(d < best_distance)) continue;
            if (std::any_of(nodes.begin(), nodes.end(), [&](std::int32_t m) {
                    return horizontal_distance(graph.nodes[static_cast<std::size_t>(m)].position, n.position) < spacing;
                }))
                continue;
            best_distance = d;
            best = static_cast<std::int32_t>(k);
        }
        if (!best) {
            if (nodes.empty()) return nodes;
            nodes.push_back(nodes.front());
        } else
            nodes.push_back(*best);
    }
    return nodes;
}

bool coop_partner_player_at_crawl(const NpcNodeGraph& graph, const RootVector& player, float max_dz) {
    const auto nearest = coop_partner_nearest_node(graph, player, max_dz);
    return nearest && coop_partner_crawl_interior(graph, static_cast<std::size_t>(*nearest)) &&
           horizontal_distance(player, graph.nodes[static_cast<std::size_t>(*nearest)].position) <= 3.0F;
}

std::optional<std::int32_t> coop_partner_follow(CoopFollowState& state, const CoopFollowConfig& config,
                                                const NpcNodeGraph& graph, const RootVector& partner,
                                                const RootVector& player, std::uint32_t frame) {
    return coop_partner_follow_step(state, config, graph, partner, player, frame, {}).node;
}

CoopFollowStep coop_partner_follow_step(CoopFollowState& state, const CoopFollowConfig& config,
                                        const NpcNodeGraph& graph, const RootVector& partner, const RootVector& player,
                                        std::uint32_t frame, const std::function<bool(const RootVector&)>& routable) {
    CoopFollowStep step;
    if (frame < state.next_frame) return step;
    state.next_frame = frame + config.interval_frames;
    const auto previous = state.last_partner;
    state.last_partner = partner;
    const float apart = horizontal_distance(partner, player);
    if (apart <= config.follow_distance && std::fabs(partner[2] - player[2]) <= config.max_dz) {
        state.goal = -1;
        state.point.reset();
        return step;
    }
    // Checkpoint 132ob6: a goal at a ladder foot (a type 3 link) is kept until the partner arrives (the
    // navigation's planar 1 m), so the next route starts on the ladder link at the foot. Re-routing short
    // of it starts the link up to 2 m away (the follower's hold radius), where the AI's one-shot ladder
    // grab (0x1a when the state 2 turn ends, 0014bff0) finds no edge and only the stuck
    // relocation (001bc4b0) takes it up.
    if (state.goal >= 0 && static_cast<std::size_t>(state.goal) < graph.nodes.size() && previous &&
        horizontal_distance(partner, *previous) > 0.25F) {
        const auto ladder = [](const std::vector<NpcNode::Link>& links) {
            return std::any_of(links.begin(), links.end(), [](const NpcNode::Link& l) { return l.type == 3; });
        };
        const auto g = static_cast<std::size_t>(state.goal);
        const bool foot = ladder(graph.nodes[g].links) || (g < graph.port_overlay.size() && ladder(graph.port_overlay[g]));
        if (foot && horizontal_distance(partner, graph.nodes[g].position) > 1.0F) return step;
    }
    // Checkpoint 132ob11 (port-only): while the player is at or in a crawl (its nearest Nav node is a crawl
    // interior node), the partner keeps its goal and does not close in. It crosses the crawl once the player
    // is out, so it never crawls into the player or crowds the crawl's mouth while the player gets down.
    // 132pw5 (partners 2 and 3): one farther than run_distance still closes in, to a goal beside the player (the
    // crawl's own nodes are not goals); TORONTO1's partner 3 otherwise kept the goal 23 m back that it had reached
    // when the player went down at the C4 room's crawl (frame 10260) until the catch-up (10871). Within 4 m of a
    // crawling player the crawl clearance holds it (play_main).
    if (coop_partner_player_at_crawl(graph, player, config.max_dz) && (config.avoid.empty() || apart <= config.run_distance))
        return step;
    auto node = coop_partner_follow_node_spaced(graph, player, partner, config.follow_distance, config.max_dz,
                                                config.far_node_distance, config.avoid, config.avoid_radius);
    if (node && !config.avoid.empty() &&
        !(horizontal_distance(player, graph.nodes[static_cast<std::size_t>(*node)].position) < apart)) {
        // 132pw5 (partners 2 and 3): the spaced node is no closer to the player than the partner (the nodes near the
        // player are the other partners' goals), so the rule's own node: TORONTO1's partner 3 otherwise stayed 20 m
        // back when partners 1 and 2 moved on (frame 15689) until the catch-up (16016).
        node = coop_partner_follow_node(graph, player, partner, config.follow_distance, config.max_dz, config.far_node_distance);
    }
    if (node && *node != state.goal) {
        const auto& position = graph.nodes[static_cast<std::size_t>(*node)].position;
        if (horizontal_distance(player, position) < apart && horizontal_distance(partner, position) > 1.0F) {
            state.goal = *node;
            state.point.reset();
            step.node = node;
            return step;
        }
    }
    if (!routable) return step;
    if (node && horizontal_distance(player, graph.nodes[static_cast<std::size_t>(*node)].position) <= config.follow_distance)
        return step;
    if (state.goal >= 0 && !state.point && (!previous || horizontal_distance(partner, *previous) > 0.5F)) return step;
    const float d = std::min(config.point_distance, apart);
    std::vector<RootVector> candidates;
    const auto along = [&](float distance) {
        RootVector p = player;
        p[0] += (partner[0] - player[0]) / apart * distance;
        p[1] += (partner[1] - player[1]) / apart * distance;
        return p;
    };
    candidates.push_back(along(d));
    candidates.push_back(player);
    for (float s = d + 1.0F; s <= apart - 2.0F; s += 1.0F) candidates.push_back(along(s));
    for (const auto& p : candidates) {
        if (!routable(p)) continue;
        if (state.point && horizontal_distance(*state.point, p) <= 2.0F) return step;
        state.point = p;
        step.point = p;
        return step;
    }
    return step;
}

std::uint8_t coop_partner_travel_speed(const CoopFollowConfig& config, float apart) {
    return apart > config.run_distance ? 3 : 2;
}

void coop_partner_record_trail(CoopCatchUpState& state, const CoopCatchUpConfig& config, const RootVector& player,
                               std::uint32_t frame, bool standing) {
    if (!standing || frame < state.next_trail) return;
    state.next_trail = frame + config.trail_interval;
    if (!state.trail.empty() && horizontal_distance(player, state.trail.back()) < 1.0F &&
        std::fabs(player[2] - state.trail.back()[2]) < 0.5F)
        return;
    state.trail.push_back(player);
    if (state.trail.size() > config.trail_size) state.trail.erase(state.trail.begin());
}

bool coop_partner_catch_up_due(CoopCatchUpState& state, const CoopCatchUpConfig& config, const RootVector& partner,
                               const RootVector& player, float max_dz, float dt, bool busy) {
    if (!(dt > 0.0F)) throw std::runtime_error("Coop partner catch-up: the frame time must be positive");
    const float dz = std::fabs(partner[2] - player[2]);
    if (busy || (horizontal_distance(partner, player) <= config.near_distance && dz <= max_dz)) {
        state.counting = false;
        state.stuck = 0.0F;
        return false;
    }
    const float dx = partner[0] - player[0], dy = partner[1] - player[1];
    const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
    const float mx = partner[0] - state.anchor[0], my = partner[1] - state.anchor[1], mz = partner[2] - state.anchor[2];
    const bool moved = std::sqrt(mx * mx + my * my + mz * mz) > config.progress;
    if (!state.counting || d < state.best - config.progress || moved) {
        state.counting = true;
        state.stuck = 0.0F;
        state.best = d;
        state.anchor = partner;
        return false;
    }
    state.best = std::min(state.best, d);
    state.stuck += dt;
    return state.stuck >= config.stuck_seconds;
}

std::vector<RootVector> coop_partner_catch_up_points(const CoopCatchUpState& state, const CoopCatchUpConfig& config,
                                                     const NpcNodeGraph& graph, const RootVector& player, float max_dz,
                                                     const std::function<bool(const RootVector&)>& same_region) {
    const auto in_band = [&](const RootVector& p) {
        const float d = horizontal_distance(player, p);
        return d >= config.target_min && d <= config.target_max && std::fabs(p[2] - player[2]) <= max_dz;
    };
    std::vector<std::pair<float, RootVector>> nodes;
    for (const auto& n : graph.nodes) {
        if (!(n.flags & npc_node_nav) || n.links.empty()) continue;
        const RootVector p{n.position[0], n.position[1], n.position[2], 1.0F};
        if (!in_band(p) || (same_region && !same_region(p))) continue;
        nodes.push_back({horizontal_distance(player, p), p});
    }
    std::stable_sort(nodes.begin(), nodes.end(), [](const auto& l, const auto& r) { return l.first < r.first; });
    std::vector<RootVector> out;
    out.reserve(nodes.size() + state.trail.size());
    for (const auto& [d, p] : nodes) out.push_back(p);
    for (auto it = state.trail.rbegin(); it != state.trail.rend(); ++it)
        if (in_band(*it)) out.push_back(*it);
    return out;
}

void coop_partner_caught_up(CoopCatchUpState& state) {
    state.counting = false;
    state.stuck = 0.0F;
    ++state.count;
}

namespace {
RootVector hotbox_centre(const Hotbox& box) {
    const auto& lo = box.record.minimum;
    const auto& hi = box.record.maximum;
    const std::array<float, 3> c{(lo[0] + hi[0]) * 0.5F, (lo[1] + hi[1]) * 0.5F, (lo[2] + hi[2]) * 0.5F};
    if (!box.world) return {c[0], c[1], c[2], 1.0F};
    const auto& m = *box.world;
    RootVector out{0.0F, 0.0F, 0.0F, 1.0F};
    for (std::size_t r = 0; r < 3; ++r) out[r] = c[0] * m[r] + c[1] * m[4 + r] + c[2] * m[8 + r] + m[12 + r];
    return out;
}
}

std::vector<RootVector> hotbox_entry_points(const Hotbox& box, const RootVector& from, float inset, float spacing) {
    std::array<float, 3> f{from[0], from[1], from[2]};
    if (box.world) {
        const auto inv = hotbox_rigid_inverse(*box.world);
        for (std::size_t r = 0; r < 3; ++r)
            f[r] = from[0] * inv[r] + from[1] * inv[4 + r] + from[2] * inv[8 + r] + inv[12 + r];
    }
    const auto& lo = box.record.minimum;
    const auto& hi = box.record.maximum;
    std::array<std::vector<float>, 2> axis;
    for (std::size_t c = 0; c < 2; ++c) {
        const float a = lo[c] + std::min(inset, (hi[c] - lo[c]) * 0.5F);
        const float b = hi[c] - std::min(inset, (hi[c] - lo[c]) * 0.5F);
        for (float v = a; v < b; v += spacing) axis[c].push_back(v);
        axis[c].push_back(b);
    }
    std::vector<std::pair<float, std::array<float, 2>>> local;
    for (const float x : axis[0])
        for (const float y : axis[1]) local.push_back({(x - f[0]) * (x - f[0]) + (y - f[1]) * (y - f[1]), {x, y}});
    std::stable_sort(local.begin(), local.end(), [](const auto& l, const auto& r) { return l.first < r.first; });
    std::vector<RootVector> out;
    out.reserve(local.size());
    for (const auto& [d, p] : local) {
        if (!box.world) {
            out.push_back({p[0], p[1], lo[2], 1.0F});
            continue;
        }
        const auto& m = *box.world;
        RootVector w{0.0F, 0.0F, 0.0F, 1.0F};
        for (std::size_t r = 0; r < 3; ++r) w[r] = p[0] * m[r] + p[1] * m[4 + r] + lo[2] * m[8 + r] + m[12 + r];
        out.push_back(w);
    }
    return out;
}

std::optional<RootVector> coop_box_place_point(const Hotbox& box, const RootVector& partner, float inset,
                                               const std::function<bool(const RootVector&, const RootVector&)>& blocked) {
    const auto up = [](const RootVector& p, float h) { return RootVector{p[0], p[1], p[2] + h, 1.0F}; };
    const float height = box.record.maximum[2] - box.record.minimum[2];
    for (auto p : hotbox_entry_points(box, partner, inset)) {
        p[2] = std::max(p[2], std::min(partner[2], p[2] + height));
        if (blocked && (blocked(up(partner, 0.3F), up(p, 0.3F)) || blocked(up(partner, 1.0F), up(p, 1.0F)) ||
                        !blocked(up(p, 1.0F), up(p, -0.5F))))
            continue;
        return p;
    }
    return std::nullopt;
}

void coop_partner_route_answer(CoopBoostState& state, std::uint8_t event) {
    if (!state.route_pending) return;
    state.route_pending = false;
    if (event == 2)
        state.routed = true;
    else
        ++state.entry;
}

void coop_partner_boost_done(CoopBoostState& state) {
    if (!state.box || !state.triggered) return;
    const auto box = state.box;
    state = {};
    state.done_box = box;
}

void coop_partner_respawn_reset(CoopFollowState& follow, CoopBoostState& boost, CoopCatchUpState& catch_up,
                                CoopCombatState& combat) {
    follow = {};
    boost = {};
    const auto catch_ups = catch_up.count;
    catch_up = {};
    catch_up.count = catch_ups;
    const auto engagements = combat.engagements;
    combat = {};
    combat.engagements = engagements;
}

CoopRespawnPoint coop_partner_respawn_point(const CoopRespawnConfig& config, const NpcNodeGraph& graph,
                                            const RootVector& anchor, const RootVector& right,
                                            const RootVector& forward, float max_dz,
                                            const std::function<bool(const RootVector&, const RootVector&)>& blocked,
                                            const std::vector<RootVector>& taken) {
    const auto up = [](const RootVector& p, float h) { return RootVector{p[0], p[1], p[2] + h, 1.0F}; };
    // 132pw5: partners 2 and 3 skip the places the lower-numbered partners were just stood on.
    const auto free = [&](const RootVector& p) {
        return std::none_of(taken.begin(), taken.end(),
                            [&](const RootVector& t) { return horizontal_distance(p, t) < config.min_distance; });
    };
    const auto clear = [&](const RootVector& p) {
        return free(p) && (!blocked || (!blocked(up(anchor, 0.3F), up(p, 0.3F)) && !blocked(up(anchor, 1.0F), up(p, 1.0F))));
    };
    std::optional<std::int32_t> best;
    float best_distance = std::numeric_limits<float>::max();
    for (std::size_t k = 0; k < graph.nodes.size(); ++k) {
        const auto& n = graph.nodes[k];
        if (!(n.flags & npc_node_nav) || n.links.empty()) continue;
        if (std::fabs(n.position[2] - anchor[2]) > max_dz) continue;
        const float d = horizontal_distance(anchor, n.position);
        if (d < config.min_distance || d > config.node_radius || !(d < best_distance)) continue;
        if (!clear(RootVector{n.position[0], n.position[1], n.position[2], 1.0F})) continue;
        best_distance = d;
        best = static_cast<std::int32_t>(k);
    }
    if (best) {
        const auto& p = graph.nodes[static_cast<std::size_t>(*best)].position;
        return {RootVector{p[0], p[1], p[2], 1.0F}, CoopRespawnPlace::node, best};
    }
    const float rl = std::hypot(right[0], right[1]), fl = std::hypot(forward[0], forward[1]);
    if (rl > 0.0F && fl > 0.0F) {
        const float rx = right[0] / rl, ry = right[1] / rl, fx = forward[0] / fl, fy = forward[1] / fl;
        const std::array<std::array<float, 2>, 4> directions{{{rx, ry}, {-rx, -ry}, {-fx, -fy}, {fx, fy}}};
        for (const auto& d : directions) {
            const RootVector p{anchor[0] + d[0] * config.offset, anchor[1] + d[1] * config.offset, anchor[2], 1.0F};
            if (!clear(p)) continue;
            if (blocked && !blocked(up(p, 1.0F), up(p, -0.5F))) continue;
            return {p, CoopRespawnPlace::beside, std::nullopt};
        }
    }
    return {RootVector{anchor[0], anchor[1], anchor[2], 1.0F}, CoopRespawnPlace::shared, std::nullopt};
}

namespace {
// The route into an engaged box: MoveToNode to the node nearest the box's horizontal centre at height goal_z,
// then, there, a route to each routable floor point in turn until one is found.
void coop_route_into_box(CoopBoostState& state, const Hotbox& box, const NpcNodeGraph& graph, const RootVector& partner,
                         float goal_z, float max_dz, const std::function<bool(const RootVector&)>& routable,
                         CoopBoostStep& step, float inset = 0.3F,
                         float node_reach = std::numeric_limits<float>::max()) {
    if (state.goal == -1) {
        auto goal = hotbox_centre(box);
        goal[2] = goal_z;
        const auto node = coop_partner_nearest_node(graph, goal, max_dz);
        if (node && horizontal_distance(goal, graph.nodes.at(static_cast<std::size_t>(*node)).position) <= node_reach) {
            state.goal = *node;
            step.move_to = node;
            return;
        }
        // Checkpoint 132ms5: no Nav node near the box (BELARUS2's bridge: the nearest is 241 m off): the route points
        // are asked for from where the partner stands (the route search needs only a navigation region, 0015d790).
        state.goal = coop_route_goal_direct;
    }
    // Checkpoint 132ms5: once the points are under way the partner is no longer held to the goal node (a failed
    // route may leave it a step off, which stalled it at BELARUS2's C-4 cache).
    if (!state.routed && !state.route_pending && !state.unreachable &&
        (state.goal == coop_route_goal_direct || !state.entries.empty() ||
         horizontal_distance(partner, graph.nodes.at(static_cast<std::size_t>(state.goal)).position) <= 1.0F)) {
        if (state.entries.empty() && state.entry == 0) {
            // A point within 2.0 of the partner gives no route segment (00160720, set_npc_path_segment_to's
            // c.ole.s 2.0), so the partner would never step into the box: such points are skipped.
            for (const auto& p : hotbox_entry_points(box, partner, inset))
                if (state.entries.size() < coop_route_tries && horizontal_distance(partner, p) > coop_route_min_segment &&
                    routable(p))
                    state.entries.push_back(p);
        }
        if (state.entry < state.entries.size()) {
            state.route_pending = true;
            step.route_to = state.entries[state.entry];
        } else {
            state.unreachable = true;
            step.unreachable = state.box;
        }
    }
}
} // namespace

CoopBoostStep coop_partner_boost(CoopBoostState& state, const HotboxWorld& world, const NpcNodeGraph& graph,
                                 const RootVector& partner, const RootVector& player,
                                 const std::vector<std::size_t>& player_inside, float max_dz,
                                 const std::function<bool(const RootVector&)>& routable) {
    const auto player_in = [&player_inside](std::size_t k) {
        return std::find(player_inside.begin(), player_inside.end(), k) != player_inside.end();
    };
    if (state.done_box && !player_in(*state.done_box)) state.done_box.reset();
    CoopBoostStep step;
    if (state.box) {
        const auto& box = world.boxes.at(*state.box);
        if (state.triggered) {
            if (!box.eligible() || !player_in(*state.box)) {
                step.released = state.box;
                state = {};
            }
            return step;
        }
        if (!box.eligible() || !player_in(*state.box)) {
            state = {};
            return step;
        }
        if (hotbox_contains(box, partner)) {
            state.triggered = true;
            step.trigger_box = state.box;
            return step;
        }
        // The box's horizontal centre at the player's height (the box runs up the wall).
        coop_route_into_box(state, box, graph, partner, player[2], max_dz, routable, step);
        return step;
    }
    for (std::size_t k = 0; k < world.boxes.size(); ++k) {
        const auto& box = world.boxes[k];
        if (box.record.type != 12 || box.link_keys.empty() || !box.eligible() || !player_in(k)) continue;
        if (state.done_box == k) continue;
        state.box = k;
        return coop_partner_boost(state, world, graph, partner, player, player_inside, max_dz, routable);
    }
    return step;
}

const std::vector<CoopCommandRule>& coop_command_rules(const std::string& mission) {
    // TORONTO1 (STREETS handlers 838 / 839): in front of the bank's entrance, outside fire zone 144361, the player
    // sends the partner to the water main (box 139949), then runs into the laptop box 139948 once the sprinklers
    // are on (LaptopCheck's 9.0 s). The point is measured in research/TORONTO1_COOP_AUDIT.md "Checkpoint 132pc1".
    static const std::vector<CoopCommandRule> toronto1{
        {RootVector{45.0F, 36.75F, 3.2F, 1.0F}, 3.0F, 2.0F, {139949U}, 1U,
         "Press TRIANGLE: send partner to the water main", "Partner: on the way to the water main",
         "Partner: the water main is turned", "Partner: the water main order is cancelled",
         "the water main (TURN_WATERMAIN) for the laptop"}};
    static const std::vector<CoopCommandRule> none;
    // BELARUS2 (132ms5, research/COOP.md "BELARUS2 bridge"): the second agent's half of Obj_Bridge. The C-4 cache
    // (box 533164, handler OnGOBJTriggerEventMsg word 6027: PlayerAddItem(Who, 'C-4', 1), one charge per carrier)
    // and the two plant spots (boxes 533187 / 533185, handler word 9552: PlayerRemoveItem(Who, 'C-4', 1); both
    // planted -> BlowBridgeStart). Partner slot 1 takes 533187 first, slot 2 533185 first; with one partner the
    // player plants the other. The plant waits until every C-4 carrier is in a plant box (together). The route
    // points lie 1 m inside the boxes (at the 0.3 default a partner stopped 0.1 m short of the cache's side).
    static const std::vector<CoopCommandRule> belarus2{
        {RootVector{-650.6F, -601.3F, 0.0F, 1.0F}, 30.0F, 8.0F, {533164U}, 1U, nullptr, "Partner: taking C-4",
         "Partner: C-4 taken", nullptr, "the C-4 cache (02_C4Taken)", true, nullptr, "C-4", false, 1.0F, {533187U, 533185U}},
        {RootVector{-650.6F, -601.3F, 0.0F, 1.0F}, 30.0F, 8.0F, {533164U}, 2U, nullptr, "Partner 2: taking C-4",
         "Partner 2: C-4 taken", nullptr, "the C-4 cache (02_C4Taken)", true, nullptr, "C-4", false, 1.0F, {533187U, 533185U}},
        {RootVector{0.7F, 23.8F, 3.0F, 1.0F}, 60.0F, 12.0F, {533187U, 533185U}, 1U, nullptr, "Partner: to the bridge charge",
         "Partner: C-4 planted", nullptr, "a bridge plant spot (02_C4Plant)", true, "C-4", nullptr, true, 1.0F},
        {RootVector{0.7F, 23.8F, 3.0F, 1.0F}, 60.0F, 12.0F, {533185U, 533187U}, 2U, nullptr, "Partner 2: to the bridge charge",
         "Partner 2: C-4 planted", nullptr, "a bridge plant spot (02_C4Plant)", true, "C-4", nullptr, true, 1.0F}};
    if (mission == "BELARUS2") return belarus2;
    // UKRAINE (132uk6c, research/UKRAINE_COOP.md "Checkpoint 132uk6c"): BASE's launch override. The two switch boxes
    // (type 8: A 1123035 -> 1123562, B 1123036 -> 1123563; OnGOBJTriggerEventMsg: both on within MissleOverrideTimer's
    // 20 s -> ObjManager(PASS, Obj_DestroyMissile)) stand on platforms only the type-12 buddy boxes reach (A 1123037 ->
    // InteractGOBJ 1123565, B 1123038 -> 1123564; 00216230's pairing), so three agents are needed ("Agents needed"):
    // partner 2 boosts partner 1 up at B, drops and walks to A (325 m by the NOD) to boost partner 3 up there; partner 1
    // waits in switch box B and presses when switch A's box closes; partner 3 presses A once partner 1 stands in box B.
    // The rules start once Obj_SaveGabe (id 5) has been active (StartFinalFight: the four lieutenants dead)
    // and apply wherever the player is (the area is the whole level); each partner then holds on its place. The switch
    // rules route directly from the climb's landing (`direct`), and every rule asks its route again when stalled.
    // 132uk6e: every rule needs the three level tasks partners (min_tasks): with fewer, no partner is sent.
    static const std::vector<CoopCommandRule> ukraine = [] {
        const auto rule = [](std::uint32_t slot, std::uint32_t box, const char* sent, const char* done, const char* what) {
            CoopCommandRule r;
            r.point = RootVector{0.0F, 0.0F, 0.0F, 1.0F};
            r.radius = 100000.0F;
            r.max_dz = 100000.0F;
            r.targets = {box};
            r.partner_slot = slot;
            r.sent = sent;
            r.done = done;
            r.what = what;
            r.automatic = true;
            r.once = true;
            r.retry_stalled = true;
            r.min_tasks = 3U;
            return r;
        };
        std::vector<CoopCommandRule> rules;
        auto climb_b = rule(1U, 1123038U, "Partner: climbing to switch B", nullptr, "buddy box B (climb, 1123564)");
        climb_b.buddy = CoopBuddyPart::climb;
        climb_b.after_objective = 5U;
        rules.push_back(climb_b);
        auto switch_b = rule(1U, 1123036U, "Partner: at switch B", "Partner: switch B pressed",
                             "launch override switch B (1123563, on switch A's cue)");
        switch_b.after_rule = 0U;
        switch_b.cue_closed = {1123035U};
        switch_b.hold_after = true;
        switch_b.direct = true;
        rules.push_back(switch_b);
        auto boost_b = rule(2U, 1123038U, "Partner 2: boosting at B", nullptr, "buddy box B (boost, 1123564)");
        boost_b.buddy = CoopBuddyPart::boost;
        boost_b.after_objective = 5U;
        rules.push_back(boost_b);
        auto boost_a = rule(2U, 1123037U, "Partner 2: to buddy box A", nullptr, "buddy box A (boost, 1123565)");
        boost_a.buddy = CoopBuddyPart::boost;
        boost_a.after_rule = 2U;
        boost_a.hold_after = true;
        rules.push_back(boost_a);
        auto climb_a = rule(3U, 1123037U, "Partner 3: to buddy box A", nullptr, "buddy box A (climb, 1123565)");
        climb_a.buddy = CoopBuddyPart::climb;
        climb_a.after_objective = 5U;
        rules.push_back(climb_a);
        auto switch_a = rule(3U, 1123035U, "Partner 3: at switch A", "Partner 3: switch A pressed",
                             "launch override switch A (1123562, once switch B's partner is in place)");
        switch_a.after_rule = 4U;
        switch_a.cue_occupied = {1123036U};
        switch_a.hold_after = true;
        switch_a.direct = true;
        rules.push_back(switch_a);
        return rules;
    }();
    if (mission == "UKRAINE") return ukraine;
    return mission == "TORONTO1" ? toronto1 : none;
}

bool coop_command_area_contains(const CoopCommandRule& rule, const RootVector& player) {
    const float dx = player[0] - rule.point[0];
    const float dy = player[1] - rule.point[1];
    return dx * dx + dy * dy <= rule.radius * rule.radius && std::fabs(player[2] - rule.point[2]) <= rule.max_dz;
}

std::optional<std::size_t> coop_command_open_target(const CoopCommandRule& rule, const HotboxWorld& world) {
    if (rule.buddy == CoopBuddyPart::none) return coop_command_open_box(rule.targets, world);
    // 132uk6c: a buddy rule's box is a type-12 co-op box with its InteractGOBJ link (the pairing object).
    for (const auto id : rule.targets) {
        const auto k = world.find(id);
        if (k && world.boxes[*k].eligible() && world.boxes[*k].record.type == 12 && !world.boxes[*k].link_keys.empty())
            return k;
    }
    return std::nullopt;
}

std::optional<std::size_t> coop_command_open_box(const std::vector<std::uint32_t>& ids, const HotboxWorld& world) {
    for (const auto id : ids) {
        const auto k = world.find(id);
        if (!k || !world.boxes[*k].eligible()) continue;
        const auto type = world.boxes[*k].record.type;
        if (type == 8 || type == 0x12) return k;
    }
    return std::nullopt;
}

bool coop_command_rule_gates(const CoopCommandRule& rule, const CoopCommandInput& input) {
    if (rule.partner_slot != input.slot) return false;
    if (rule.min_tasks != 0 && input.tasks < rule.min_tasks) return false;
    if (rule.requires_item && (!input.held || input.held(rule.requires_item) <= 0)) return false;
    if (rule.unless_item && input.held && input.held(rule.unless_item) > 0) return false;
    return true;
}

bool coop_command_rule_sequence(const CoopCommandState& state, const std::vector<CoopCommandRule>& rules, std::size_t r) {
    const auto done = [&state](std::size_t i) { return std::find(state.done.begin(), state.done.end(), i) != state.done.end(); };
    const auto& rule = rules.at(r);
    if (rule.once && done(r)) return false;
    if (rule.after_rule && !done(*rule.after_rule)) return false;
    if (rule.after_objective &&
        std::find(state.seen_active.begin(), state.seen_active.end(), rule.after_objective) == state.seen_active.end())
        return false;
    return true;
}

bool coop_command_cue_ready(const CoopCommandRule& rule, std::size_t box, const CoopCommandInput& input,
                            const HotboxWorld& world) {
    if (rule.buddy == CoopBuddyPart::climb && (!input.buddy_waits || !input.buddy_waits(box))) return false;
    const auto find = [&world](std::uint32_t id) {
        const auto k = world.find(id);
        if (!k) throw std::runtime_error("Coop command rule (port-only, 132uk6c): cue box " + std::to_string(id) +
                                         " is not in the level's box world");
        return *k;
    };
    if (!rule.cue_closed.empty() && std::none_of(rule.cue_closed.begin(), rule.cue_closed.end(), [&](std::uint32_t id) {
            return !world.boxes[find(id)].eligible();
        }))
        return false;
    if (!rule.cue_occupied.empty() && std::none_of(rule.cue_occupied.begin(), rule.cue_occupied.end(), [&](std::uint32_t id) {
            return input.occupied && input.occupied(find(id));
        }))
        return false;
    return true;
}

CoopCommandStep coop_partner_command(CoopCommandState& state, const std::vector<CoopCommandRule>& rules,
                                     const CoopCommandInput& input, const HotboxWorld& world, const NpcNodeGraph& graph,
                                     const RootVector& partner, float max_dz,
                                     const std::function<bool(const RootVector&)>& routable) {
    CoopCommandStep step;
    const bool pressed = input.triangle && !state.triangle_held;
    state.triangle_held = input.triangle;
    for (const auto& rule : rules)
        if (rule.after_objective &&
            std::find(state.seen_active.begin(), state.seen_active.end(), rule.after_objective) == state.seen_active.end() &&
            input.objective_active && input.objective_active(rule.after_objective))
            state.seen_active.push_back(rule.after_objective);
    std::optional<std::size_t> prompt, order;
    if (!state.rule && input.partner_free)
        for (std::size_t r = 0; r < rules.size(); ++r)
            if (coop_command_rule_gates(rules[r], input) && coop_command_rule_sequence(state, rules, r) &&
                coop_command_area_contains(rules[r], input.player) &&
                coop_command_open_target(rules[r], world) &&
                (rules[r].while_open.empty() || coop_command_open_box(rules[r].while_open, world)) &&
                (!rules[r].automatic || coop_command_area_contains(rules[r], partner))) {
                if (rules[r].automatic) order = r;
                else prompt = r;
                break;
            }
    if (prompt && pressed) {
        order = prompt;
        prompt.reset();
    } else if (!state.rule && input.seek && state.seek_wait) {
        --state.seek_wait;
    } else if (!state.rule && input.seek && input.partner_free) {
        for (std::size_t r = 0; r < rules.size() && !state.rule; ++r) {
            if (!coop_command_rule_gates(rules[r], input) || !coop_command_rule_sequence(state, rules, r) ||
                (!rules[r].while_open.empty() && !coop_command_open_box(rules[r].while_open, world)))
                continue;
            if (const auto target = coop_command_open_target(rules[r], world)) {
                state.rule = r;
                state.route = {};
                state.waiting = false;
                state.route.box = target;
                state.stall_point = partner;
                state.stall_frame = input.frame;
                step.ordered = r;
                prompt.reset();
            }
        }
        step.nothing = !state.rule;
    }
    if (order) {
        state.rule = order;
        state.route = {};
        state.waiting = false;
        state.route.box = coop_command_open_target(rules[*order], world);
        state.stall_point = partner;
        state.stall_frame = input.frame;
        step.ordered = order;
    }
    if (prompt != state.prompt) {
        if (prompt) step.show = prompt;
        else step.hide = true;
        state.prompt = prompt;
    }
    if (!state.rule) return step;
    const auto& box = world.boxes.at(*state.route.box);
    if (!box.eligible()) {
        step.route.released = state.route.box;
        step.cancelled = state.rule;
        state.rule.reset();
        state.route = {};
        state.waiting = false;
        return step;
    }
    if (hotbox_contains(box, partner)) {
        if (rules[*state.rule].together && !input.team_ready) {
            if (!state.waiting) step.waiting = state.rule;
            state.waiting = true;
            return step;
        }
        if (!coop_command_cue_ready(rules[*state.rule], *state.route.box, input, world)) {
            if (!state.waiting) step.cued = state.rule;
            state.waiting = true;
            return step;
        }
        step.route.trigger_box = state.route.box;
        step.arrived = state.rule;
        if (std::find(state.done.begin(), state.done.end(), *state.rule) == state.done.end())
            state.done.push_back(*state.rule);
        state.buddy = rules[*state.rule].buddy;
        state.buddy_box = state.buddy == CoopBuddyPart::none ? std::nullopt : state.route.box;
        state.seek_wait = coop_command_seek_wait;
        state.rule.reset();
        state.route = {};
        state.waiting = false;
        return step;
    }
    const auto& rule = rules[*state.rule];
    if (rule.retry_stalled) {
        if (horizontal_distance(partner, state.stall_point) > coop_command_stall_reach) {
            state.stall_point = partner;
            state.stall_frame = input.frame;
        } else if (input.frame - state.stall_frame >= coop_command_stall_frames) {
            const auto target = state.route.box;
            state.route = {};
            state.route.box = target;
            state.stall_point = partner;
            state.stall_frame = input.frame;
            step.rerouted = state.rule;
        }
    }
    coop_route_into_box(state.route, box, graph, partner, box.record.minimum[2], max_dz, routable, step.route,
                        rule.inset, rule.direct ? -1.0F : coop_command_node_reach);
    return step;
}

const CoopLinkOverlay* coop_link_overlay(const std::string& mission) {
    // TORONTO1 (checkpoint 132ob5): the player's route from the street (y 117.7) round the low wall west of
    // the ladder, the Triangle climb up it (HangClimbUp, frame 1079: (-49.4, 110.1, 0.88) -> (-47.5, 110.9,
    // 5.98)), the ladder (mode 16 from (-44.3, 109.6, 6.56), off at the top (-42.5, 109.7, 18.06)) and the
    // roof walk to the NOD's roof nodes (135889 at (-58.4, 91.5, 18.0)). Regions: 15 on the street (135974 /
    // 135975), 6 on the roof (135889 / 135891).
    static const CoopLinkOverlay toronto1{
        {
            {0x80000001U, {-32.2F, 117.7F, 0.85F}, 15},  // the street, north of 135975
            {0x80000002U, {-56.9F, 117.9F, 0.88F}, 15},  // the street's west end
            {0x80000003U, {-57.2F, 110.5F, 0.90F}, 15},  // west of the low wall
            {0x80000004U, {-50.6F, 110.2F, 0.88F}, 15},  // before the low wall (facing +x)
            {0x80000005U, {-47.5F, 110.9F, 5.98F}, 15},  // on top of the low wall
            // Checkpoint 132ob6: at the ladder's foot, 0.6 short of the player's mount point, as an NOD author places
            // a type-3 link's start node. 0014bff0 sends the grab 0x1a once, when its state-2 turn ends; from the
            // 132ob5 spot 1.7 back (-45.7) the turn could end out of 001dfb80's reach (-44.78 at frame 1547 when the
            // partner arrived 40 frames earlier), so the grab missed and only the AI's stuck relocation lifted it.
            {0x80000006U, {-44.9F, 109.6F, 6.50F}, 15},  // the ladder's foot (facing +x)
            {0x80000007U, {-40.3F, 109.9F, 18.01F}, 6},  // the roof at the ladder's top
            {0x80000008U, {-39.5F, 118.8F, 18.01F}, 6},
            {0x80000009U, {-54.9F, 120.0F, 18.03F}, 6},
            {0x8000000aU, {-57.0F, 104.3F, 18.03F}, 6},
            // Checkpoint 132ob11: the A2 pocket, the crawl and the C4 room (the player's route, frames 11390-12420).
            // The crawl nodes follow the player's own crawl line closely: a crawling actor turns slowly while it
            // keeps moving (about 2 degrees a frame), so a sharp corner between two crawl links runs it into the wall.
            {0x8000000bU, {-401.5F, -61.5F, 2.21F}, 33},   // the pocket south of A2, beside the test teleport's place
            {0x8000000cU, {-401.0F, -79.5F, 2.21F}, 33},
            {0x8000000dU, {-400.3F, -83.4F, 2.21F}, 33},   // the crawl's west mouth (standing, on the crawl's axis)
            {0x8000000eU, {-398.0F, -83.6F, 2.21F}, 33},   // in the crawl's entrance (where the player starts crawling)
            {0x8000000fU, {-393.1F, -83.5F, 2.59F}, 33},
            {0x80000010U, {-391.0F, -84.6F, 2.30F}, 33},
            {0x80000011U, {-390.8F, -88.7F, 1.91F}, 33},
            {0x80000012U, {-390.9F, -93.3F, 1.52F}, 33},
            {0x80000013U, {-392.3F, -96.8F, 1.45F}, 33},
            {0x80000014U, {-394.6F, -97.0F, 1.41F}, 33},
            {0x80000015U, {-397.0F, -98.0F, 1.00F}, 33},
            {0x80000016U, {-398.0F, -101.3F, 0.92F}, 33},
            {0x80000017U, {-398.2F, -107.5F, 0.82F}, 33},  // out of the crawl zone (+cf4 0 from frame 12245)
            {0x80000018U, {-410.7F, -99.6F, 0.83F}, 33},   // the C4 room
        },
        {
            {135974U, 0x80000001U, 2}, {135975U, 0x80000001U, 2},
            {0x80000001U, 135974U, 2}, {0x80000001U, 135975U, 2},
            {0x80000001U, 0x80000002U, 2}, {0x80000002U, 0x80000001U, 2},
            {0x80000002U, 0x80000003U, 2}, {0x80000003U, 0x80000002U, 2},
            {0x80000003U, 0x80000004U, 2}, {0x80000004U, 0x80000003U, 2},
            {0x80000004U, 0x80000005U, 4},                                  // Triangle up the low wall
            {0x80000005U, 0x80000006U, 2},
            {0x80000006U, 0x80000007U, 3},                                  // the ladder link (0x1a, 0014bff0)
            {0x80000007U, 0x80000008U, 2}, {0x80000008U, 0x80000007U, 2},
            {0x80000008U, 0x80000009U, 2}, {0x80000009U, 0x80000008U, 2},
            {0x80000009U, 0x8000000aU, 2}, {0x8000000aU, 0x80000009U, 2},
            {0x8000000aU, 135889U, 2}, {135889U, 0x8000000aU, 2},
            // Checkpoint 132ob11: the pocket south of A2 (no NOD node within 50 m at its height) and the crawl
            // passage into the C4 room (hotbox 140177's room), which the player crosses crouched in a crawl zone
            // (camera +cf4 0x800, toronto1_objectives_coop.txt frames 11690-12245). TORONTO1's NOD draws its own
            // crawls as chains of type-8 links (135871 -> 136789 ... 136796, CROUCH_INPUT.md "Checkpoint 132x");
            // the crawl here gets the same: 00152168 crouches the AI (0014d900(4): action 9) and sets Run on each
            // type-8 link, and stands it (0014d900(0xa)) when the route leaves them. Then a walk across the C4 room
            // to 136131 / 136134 (the room's patrol 136313 -> 136311 already ends at 136131). Region 33, as 136131.
            {0x8000000bU, 0x8000000cU, 2}, {0x8000000cU, 0x8000000bU, 2},
            {0x8000000cU, 0x8000000dU, 2}, {0x8000000dU, 0x8000000cU, 2},
            {0x8000000dU, 0x8000000eU, 8}, {0x8000000eU, 0x8000000dU, 8},   // the crawl, both ways
            {0x8000000eU, 0x8000000fU, 8}, {0x8000000fU, 0x8000000eU, 8},
            {0x8000000fU, 0x80000010U, 8}, {0x80000010U, 0x8000000fU, 8},
            {0x80000010U, 0x80000011U, 8}, {0x80000011U, 0x80000010U, 8},
            {0x80000011U, 0x80000012U, 8}, {0x80000012U, 0x80000011U, 8},
            {0x80000012U, 0x80000013U, 8}, {0x80000013U, 0x80000012U, 8},
            {0x80000013U, 0x80000014U, 8}, {0x80000014U, 0x80000013U, 8},
            {0x80000014U, 0x80000015U, 8}, {0x80000015U, 0x80000014U, 8},
            {0x80000015U, 0x80000016U, 8}, {0x80000016U, 0x80000015U, 8},
            {0x80000016U, 0x80000017U, 8}, {0x80000017U, 0x80000016U, 8},
            {0x80000017U, 0x80000018U, 2}, {0x80000018U, 0x80000017U, 2},
            {0x80000018U, 136131U, 2}, {136131U, 0x80000018U, 2},
            {0x80000018U, 136134U, 2}, {136134U, 0x80000018U, 2},
        }};
    // TORONTO2 (checkpoint 132ob10): the blown street's co-op box 205486 (StreetsBlow's Enable) lies on a raised
    // slab whose NOD nodes 202170 (-130.1, -249.1, 5.42) and 202169 (-142.2, -249.1, 5.42) have no links (flags
    // 0x81): no NPC of the original walks up there (the second player is human). The slope from 202171 (-125.7,
    // -249.1, 2.80; linked to the street's 202157) up to 202170 is walkable: a player walking -x along y -249.1
    // from (-118, -249.1, 0.3) is on the slab (z 5.42) at x -130.9 by frame 60 (local\ob10 t2probe). One overlay
    // node on the slab joins them; all of region 158 (node +7c), as 202171 / 202170 / 202169.
    static const CoopLinkOverlay toronto2{
        {
            {0x80000001U, {-136.1F, -249.1F, 5.42F}, 158},  // on the slab between 202170 and 202169
        },
        {
            {202171U, 0x80000001U, 2}, {0x80000001U, 202171U, 2},       // up / down the slope
            {0x80000001U, 202170U, 2}, {202170U, 0x80000001U, 2},
            {0x80000001U, 202169U, 2}, {202169U, 0x80000001U, 2},
        }};
    if (mission == "TORONTO1") return &toronto1;
    if (mission == "TORONTO2") return &toronto2;
    return nullptr;
}

std::int32_t install_coop_link_overlay(NpcNodeGraph& graph, const CoopLinkOverlay& overlay) {
    const auto first = static_cast<std::int32_t>(graph.nodes.size());
    for (const auto& n : graph.nodes)
        if (n.id >= coop_overlay_first_id)
            throw std::runtime_error("Coop overlay: NOD node " + std::to_string(n.id) + " uses an overlay ID");
    const auto index_of = [&](std::uint32_t id) {
        for (std::size_t k = 0; k < graph.nodes.size(); ++k)
            if (graph.nodes[k].id == id) return static_cast<std::int32_t>(k);
        throw std::runtime_error("Coop overlay: no node " + std::to_string(id));
    };
    for (const auto& o : overlay.nodes) {
        if (o.id < coop_overlay_first_id) throw std::runtime_error("Coop overlay: node ID " + std::to_string(o.id) + " is not an overlay ID");
        for (std::size_t k = static_cast<std::size_t>(first); k < graph.nodes.size(); ++k)
            if (graph.nodes[k].id == o.id) throw std::runtime_error("Coop overlay: node " + std::to_string(o.id) + " twice");
        NpcNode n;
        n.position = o.position;
        n.flags = npc_node_nav;
        n.parameter_7c = o.region;
        n.id = o.id;
        graph.nodes.push_back(n);
    }
    graph.port_overlay.resize(static_cast<std::size_t>(first));
    for (const auto& l : overlay.links) {
        if (l.type != 2 && l.type != 3 && l.type != 4 && l.type != 8)
            throw std::runtime_error("Coop overlay: link type " + std::to_string(l.type));
        const auto from = index_of(l.from), to = index_of(l.to);
        if (from < first && to < first) throw std::runtime_error("Coop overlay: a link between two NOD nodes");
        const auto& a = graph.nodes[static_cast<std::size_t>(from)].position;
        const auto& b = graph.nodes[static_cast<std::size_t>(to)].position;
        const float dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
        if (!(std::sqrt(dx * dx + dy * dy + dz * dz) > 2.0F))
            throw std::runtime_error("Coop overlay: link " + std::to_string(l.from) + " -> " + std::to_string(l.to) + " is not longer than 2.0");
        if (graph.nodes[static_cast<std::size_t>(to)].parameter_7c < 0)
            throw std::runtime_error("Coop overlay: link to node " + std::to_string(l.to) + " of no region");
        const NpcNode::Link link{to, l.type};
        if (from < first) graph.port_overlay[static_cast<std::size_t>(from)].push_back(link);
        else graph.nodes[static_cast<std::size_t>(from)].links.push_back(link);
    }
    return first;
}

bool coop_partner_wall_hides(const SoloCollisionWorld& world, const RootVector& viewer, const RootVector& point) {
    const RootVector eye{viewer[0], viewer[1], viewer[2], 1.0F}, target{point[0], point[1], point[2], 1.0F};
    const float dx = target[0] - eye[0], dy = target[1] - eye[1], dz = target[2] - eye[2];
    if (dx * dx + dy * dy + dz * dz == 0.0F) return false;  // the viewer at the point: seen
    const auto result = answer_los_request(world, pair_los_request(0, eye, 0, target));
    return pair_los_blocks(result, eye, target);
}

std::array<RootVector, 3> coop_partner_body_points(const RootVector& feet) {
    return {RootVector{feet[0], feet[1], feet[2] + 0.3F, 1.0F}, RootVector{feet[0], feet[1], feet[2] + 1.0F, 1.0F},
            RootVector{feet[0], feet[1], feet[2] + 1.7F, 1.0F}};
}

bool coop_partner_walls_hide_body(const SoloCollisionWorld& world, const RootVector& viewer, const RootVector& feet) {
    for (const auto& p : coop_partner_body_points(feet))
        if (!coop_partner_wall_hides(world, viewer, p)) return false;
    return true;
}

bool coop_partner_combat_update(CoopCombatState& state, const CoopCombatConfig& config, const CoopCombatSignals& signals,
                                float dt) {
    if (!(dt >= 0.0F)) throw std::runtime_error("Coop partner combat: negative frame time");
    if (signals.danger > 0.0F || signals.player_fired || signals.player_targeted) {
        state.in_combat = true;
        state.quiet = 0.0F;
    } else if (state.in_combat) {
        state.quiet += dt;
        if (state.quiet >= config.hold_seconds) state.in_combat = false;
    }
    if (signals.danger > 0.0F || signals.player_targeted || signals.partner_targeted) {
        state.alerted = true;
        state.calm = 0.0F;
    } else if (state.alerted) {
        state.calm += dt;
        if (state.calm >= config.hold_seconds) state.alerted = false;
    }
    return state.in_combat;
}

bool coop_partner_may_fire(const CoopCombatState& state) { return state.alerted; }

bool coop_partner_combat_engaged(const CoopCombatState& state, const CoopCombatConfig& config, const RootVector& partner,
                                 const RootVector& player) {
    if (!state.in_combat) return false;
    const float dx = partner[0] - player[0], dy = partner[1] - player[1], dz = partner[2] - player[2];
    const float limit = state.engaged ? config.release : config.leash;
    return dx * dx + dy * dy + dz * dz <= limit * limit;
}

std::size_t coop_partner_forget_npcs(NpcPerception& perception) {
    const auto freed = perception.records.size();
    perception_clear(perception);   // 0016dc60
    perception.candidate_48 = 0;    // 0016e0a0: +48 0, +4c -2
    perception.candidate_id_4c = -2;
    return freed;
}

}
