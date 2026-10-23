#include "coop_breadcrumbs.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <queue>
#include <limits>
#include <stdexcept>
#include <utility>

namespace omega {
namespace {
float flat(const RootVector& a, const RootVector& b) {
    const float dx = a[0] - b[0], dy = a[1] - b[1];
    return std::sqrt(dx * dx + dy * dy);
}
float full(const RootVector& a, const RootVector& b) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
RootVector at(const std::array<float, 3>& p) { return {p[0], p[1], p[2], 1.0F}; }
// Every link must be longer than 2.0: 00160720 (set_npc_path_segment_to) makes no segment for a shorter one.
constexpr float min_link = 2.05F;
}

const char* coop_traversal_kind_name(CoopTraversalKind kind) {
    switch (kind) {
    case CoopTraversalKind::none: return "none";
    case CoopTraversalKind::climb: return "climb";
    case CoopTraversalKind::climb_down: return "climb down";
    case CoopTraversalKind::crawl: return "crawl";
    case CoopTraversalKind::ladder: return "ladder";
    case CoopTraversalKind::drop: return "drop";
    case CoopTraversalKind::buddy: return "buddy climb";
    case CoopTraversalKind::jump: return "jump";
    case CoopTraversalKind::walk: return "walk bridge";
    }
    return "?";
}

std::int32_t coop_traversal_link_type(CoopTraversalKind kind) {
    switch (kind) {
    case CoopTraversalKind::climb: return 4;
    case CoopTraversalKind::climb_down: return 0xb;
    case CoopTraversalKind::crawl: return 8;
    case CoopTraversalKind::ladder: return 3;
    case CoopTraversalKind::drop: return 2;
    case CoopTraversalKind::walk: return 2;
    default: return -1;
    }
}

bool coop_player_traversing(const CoopPlayerSample& s) {
    return (s.actions & (coop_traversal_actions | coop_fall_actions)) != 0 || s.airborne || (s.crouched && s.crawl_zone);
}

CoopTraversalKind coop_breadcrumb_classify(bool ladder, bool crawl, bool climb, bool air, bool buddy, float rise,
                                           float min_rise) {
    if (buddy) return CoopTraversalKind::buddy;
    if (ladder) return CoopTraversalKind::ladder;
    if (crawl) return CoopTraversalKind::crawl;
    // A climb that ends level (over a low wall onto the far side) is still the Triangle's climb (type 4).
    if (climb) return rise <= -min_rise ? CoopTraversalKind::climb_down : CoopTraversalKind::climb;
    if (air) return rise <= -min_rise ? CoopTraversalKind::drop : CoopTraversalKind::jump;
    return CoopTraversalKind::none;
}

std::vector<CoopBreadcrumb> coop_breadcrumb_record(CoopBreadcrumbRecorder& r, const CoopBreadcrumbConfig& c,
                                                   const CoopPlayerSample& s) {
    std::vector<CoopBreadcrumb> out;
    const bool jumped = r.last && full(*r.last, s.floor) > c.jump;
    r.last = s.floor;
    if (jumped) {
        // A teleport (the host's test teleport, a revive): the trail and any traversal restart.
        if (r.finishing) {
            out.push_back(*r.finishing);
            r.finishing.reset();
        }
        r.trail.clear();
        r.episode = false;
        r.last_standing.reset();
        ++r.restarts;
    }
    const bool traversing = coop_player_traversing(s);
    if (r.finishing) {
        if (traversing) {
            out.push_back(*r.finishing);
            r.finishing.reset();
        } else if (full(s.floor, r.finishing->exit) >= c.spacing) {
            r.finishing->after = s.floor;
            out.push_back(*r.finishing);
            r.finishing.reset();
        } else if (s.frame - r.finishing->end_frame >= c.after_wait_frames) {
            out.push_back(*r.finishing);   // the player stays where it stood again
            r.finishing.reset();
        }
    }
    const auto add_trail = [&](const RootVector& p) {
        if (r.trail.empty() || full(r.trail.back(), p) >= c.spacing) r.trail.push_back(p);
        if (r.trail.size() > c.trail_size) r.trail.erase(r.trail.begin());
    };
    if (!r.episode) {
        if (!traversing) {
            r.last_standing = s.floor;
            add_trail(s.floor);
            return out;
        }
        r.episode = true;
        r.start = r.last_standing ? *r.last_standing : s.floor;
        r.start_frame = s.frame;
        r.before = r.trail;
        r.settle.reset();
        r.quiet = 0;
        r.ladder = r.crawl = r.climb = r.air = r.buddy = false;
        r.line.clear();
    }
    r.ladder = r.ladder || (s.actions & (1U << 16)) != 0;
    r.buddy = r.buddy || (s.actions & coop_buddy_actions) != 0;
    r.climb = r.climb || (s.actions & coop_traversal_actions & ~((1U << 16) | coop_buddy_actions)) != 0;
    r.air = r.air || s.airborne || (s.actions & coop_fall_actions) != 0;
    if (s.crouched && s.crawl_zone) {
        r.crawl = true;
        const auto& from = r.line.empty() ? r.start : r.line.back();
        if (full(from, s.floor) >= c.spacing) r.line.push_back(s.floor);
    }
    if (traversing) {
        r.settle.reset();
        r.quiet = 0;
        return out;
    }
    if (!r.settle) {
        r.settle = s.floor;
        r.settle_frame = s.frame;
    }
    if (++r.quiet < c.settle_frames) return out;
    CoopBreadcrumb crumb;
    crumb.before = r.before;
    crumb.entry = r.start;
    crumb.exit = s.floor;   // where it stands once settled (a climb's root still rises in the first quiet frames)
    crumb.line = r.line;
    crumb.start_frame = r.start_frame;
    crumb.end_frame = s.frame;
    crumb.rise = crumb.exit[2] - crumb.entry[2];
    crumb.kind = coop_breadcrumb_classify(r.ladder, r.crawl, r.climb, r.air, r.buddy, crumb.rise, c.min_rise);
    r.episode = false;
    r.last_standing = s.floor;
    if (crumb.kind == CoopTraversalKind::none || crumb.kind == CoopTraversalKind::buddy ||
        crumb.kind == CoopTraversalKind::jump) {
        // No crumb: the standing trail goes on through it (a hop over a kerb does not cut the next approach short).
        add_trail(crumb.exit);
        add_trail(s.floor);
        out.push_back(std::move(crumb));
        return out;
    }
    // A crumb: the next approach starts at its exit (its goal joins the next crumb, coop_breadcrumb_push's goals).
    r.trail.clear();
    add_trail(crumb.exit);
    add_trail(s.floor);
    if (full(s.floor, crumb.exit) >= c.spacing) {
        crumb.after = s.floor;
        out.push_back(std::move(crumb));
    } else {
        r.finishing = std::move(crumb);
    }
    return out;
}

bool coop_breadcrumb_covered(const NpcNodeGraph& graph, const CoopBreadcrumb& crumb, const CoopBreadcrumbConfig& c) {
    const auto type = coop_traversal_link_type(crumb.kind);
    if (type < 0) return false;
    const auto near = [&](const RootVector& p, const RootVector& q) {
        return flat(p, q) <= c.covered_radius && std::fabs(p[2] - q[2]) <= c.join_max_dz;
    };
    const auto check = [&](std::size_t from, const NpcNode::Link& l) {
        if (l.type != type || l.target < 0) return false;
        const auto a = at(graph.nodes[from].position), b = graph.position(l.target);
        return near(a, crumb.entry) && (near(b, crumb.exit) || (crumb.after && near(b, *crumb.after)));
    };
    if (crumb.kind == CoopTraversalKind::crawl) {
        // A crawl is a chain: covered when a type-8 link starts by the entry and one ends by the exit.
        bool in = false, out = false;
        for (std::size_t k = 0; k < graph.nodes.size(); ++k) {
            const auto visit = [&](const NpcNode::Link& l) {
                if (l.type != 8 || l.target < 0) return;
                if (near(at(graph.nodes[k].position), crumb.entry)) in = true;
                if (near(graph.position(l.target), crumb.exit)) out = true;
            };
            for (const auto& l : graph.nodes[k].links) visit(l);
            if (k < graph.port_overlay.size())
                for (const auto& l : graph.port_overlay[k]) visit(l);
        }
        return in && out;
    }
    for (std::size_t k = 0; k < graph.nodes.size(); ++k) {
        for (const auto& l : graph.nodes[k].links)
            if (check(k, l)) return true;
        if (k < graph.port_overlay.size())
            for (const auto& l : graph.port_overlay[k])
                if (check(k, l)) return true;
    }
    return false;
}

namespace {
// Join candidates of a place: Nav nodes with links and a region, within join_radius horizontally and join_max_dz in
// height, a link's length away or nearer (any distance here), on a clear line; nearest first.
std::vector<std::pair<float, std::int32_t>> place_joins(const NpcNodeGraph& graph, const RootVector& p, const CoopBreadcrumbConfig& c,
                                                        const CoopBreadcrumbHost& host) {
    std::vector<std::pair<float, std::int32_t>> found;
    for (std::size_t k = 0; k < graph.nodes.size(); ++k) {
        const auto& n = graph.nodes[k];
        const bool linked = !n.links.empty() || (k < graph.port_overlay.size() && !graph.port_overlay[k].empty());
        if (!(n.flags & npc_node_nav) || n.parameter_7c < 0 || !linked) continue;
        const auto q = at(n.position);
        if (std::fabs(q[2] - p[2]) > c.join_max_dz) continue;
        const float d = flat(p, q);
        if (d <= c.join_radius) found.emplace_back(d, static_cast<std::int32_t>(k));
    }
    std::sort(found.begin(), found.end());
    std::vector<std::pair<float, std::int32_t>> out;
    for (const auto& f : found) {
        if (out.size() >= 4) break;
        if (host.clear(p, graph.position(f.second))) out.push_back(f);
    }
    return out;
}
}

bool coop_breadcrumb_routed(const NpcNodeGraph& graph, const CoopBreadcrumb& crumb, const CoopBreadcrumbConfig& c,
                            const CoopBreadcrumbHost& host) {
    if (!host.clear) throw std::runtime_error("Coop breadcrumbs: the host's clear line test is not linked");
    const RootVector to = crumb.after ? *crumb.after : crumb.exit;
    const auto starts = place_joins(graph, crumb.entry, c, host);
    if (starts.empty()) return false;
    std::vector<float> goal_cost(graph.nodes.size(), -1.0F);
    for (const auto& g : place_joins(graph, to, c, host)) goal_cost[static_cast<std::size_t>(g.second)] = g.first;
    const float budget = c.routed_factor * full(crumb.entry, to) + c.routed_extra;
    std::vector<float> best(graph.nodes.size(), std::numeric_limits<float>::infinity());
    using Item = std::pair<float, std::int32_t>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
    for (const auto& s0 : starts) {
        best[static_cast<std::size_t>(s0.second)] = s0.first;
        open.emplace(s0.first, s0.second);
    }
    while (!open.empty()) {
        const auto [d, k] = open.top();
        open.pop();
        const auto ku = static_cast<std::size_t>(k);
        if (d > best[ku] || d > budget) continue;
        if (goal_cost[ku] >= 0.0F && d + goal_cost[ku] <= budget) return true;
        const auto& n = graph.nodes[ku];
        if (!(n.flags & 0x88U)) continue;
        const auto from = at(n.position);
        const auto visit = [&](const NpcNode::Link& l) {
            if (l.type == 7 || l.target < 0 || static_cast<std::size_t>(l.target) >= graph.nodes.size()) return;
            const float e = d + full(from, graph.position(l.target));
            if (e < best[static_cast<std::size_t>(l.target)]) {
                best[static_cast<std::size_t>(l.target)] = e;
                open.emplace(e, l.target);
            }
        };
        for (const auto& l : n.links) visit(l);
        if (ku < graph.port_overlay.size())
            for (const auto& l : graph.port_overlay[ku]) visit(l);
    }
    return false;
}

std::optional<CoopBreadcrumbNodes> install_coop_breadcrumb(NpcNodeGraph& graph, std::size_t nod_count,
                                                           std::size_t first_crumb, const CoopBreadcrumb& crumb,
                                                           const CoopBreadcrumbConfig& c, const CoopBreadcrumbHost& host,
                                                           std::uint32_t& next_id, const std::vector<std::int32_t>& goals,
                                                           std::string& why) {
    const auto type = coop_traversal_link_type(crumb.kind);
    if (type < 0) throw std::runtime_error("Coop breadcrumbs: no link type for a " + std::string(coop_traversal_kind_name(crumb.kind)));
    if (!host.clear || !host.region) throw std::runtime_error("Coop breadcrumbs: the host's clear line and region tests are not linked");
    if (nod_count > first_crumb || first_crumb > graph.nodes.size())
        throw std::runtime_error("Coop breadcrumbs: the node counts are out of order");
    // 1. The typed chain: a crawl from its mouth (the last standing place a link's length before the entry; the AI
    //    crouches where its first type-8 link begins, as at the static overlay's 0x8000000d) along the crouched line
    //    to the exit; every other kind from the entry straight to the exit (or the after place when the exit is too
    //    close for a link).
    std::vector<RootVector> chain;
    std::size_t typed_from = 0, typed_to = 0;   // chain[typed_from .. typed_to] are joined by typed links
    std::ptrdiff_t trail_index = static_cast<std::ptrdiff_t>(crumb.before.size()) - 1;
    bool after_used = false;
    if (crumb.kind == CoopTraversalKind::crawl) {
        while (trail_index >= 0 && full(crumb.before[static_cast<std::size_t>(trail_index)], crumb.entry) < min_link)
            --trail_index;
        if (trail_index < 0) {
            why = "no standing place before the crawl's entry";
            return std::nullopt;
        }
        chain.push_back(crumb.before[static_cast<std::size_t>(trail_index--)]);
        chain.push_back(crumb.entry);
        for (const auto& p : crumb.line)
            if (full(chain.back(), p) >= min_link) chain.push_back(p);
        while (chain.size() > 2 && full(chain.back(), crumb.exit) < min_link) chain.pop_back();
        if (full(chain.back(), crumb.exit) < min_link) {
            why = "the crawl is shorter than a link";
            return std::nullopt;
        }
        chain.push_back(crumb.exit);
        typed_from = 0;
        typed_to = chain.size() - 1;
    } else {
        chain.push_back(crumb.entry);
        if (full(crumb.entry, crumb.exit) >= min_link) {
            chain.push_back(crumb.exit);
        } else if (crumb.after && full(crumb.entry, *crumb.after) >= min_link) {
            chain.push_back(*crumb.after);
            after_used = true;
        } else {
            why = "the traversal is shorter than a link";
            return std::nullopt;
        }
        typed_from = 0;
        typed_to = 1;
    }
    if (crumb.after && !after_used && full(chain.back(), *crumb.after) >= min_link) chain.push_back(*crumb.after);
    // 2. Joins: nodes of the level (and the static overlay) with the Nav bit, links and a region, or earlier crumbs'
    //    goals, within join_radius horizontally and join_max_dz in height, a link's length away, on a clear line.
    const auto linked = [&](std::size_t k) {
        return !graph.nodes[k].links.empty() || (k < graph.port_overlay.size() && !graph.port_overlay[k].empty());
    };
    const auto joins_of = [&](const RootVector& p) {
        std::vector<std::pair<float, std::int32_t>> found;
        const auto consider = [&](std::size_t k) {
            const auto& n = graph.nodes[k];
            if (!(n.flags & npc_node_nav) || n.parameter_7c < 0 || !linked(k)) return;
            const auto q = at(n.position);
            if (std::fabs(q[2] - p[2]) > c.join_max_dz) return;
            const float d = flat(p, q);
            if (d > c.join_radius || full(p, q) < min_link) return;
            for (const auto& f : found)
                if (f.second == static_cast<std::int32_t>(k)) return;
            found.emplace_back(d, static_cast<std::int32_t>(k));
        };
        for (std::size_t k = 0; k < first_crumb; ++k) consider(k);
        for (const auto g : goals)
            if (g >= 0 && static_cast<std::size_t>(g) < graph.nodes.size()) consider(static_cast<std::size_t>(g));
        std::sort(found.begin(), found.end());
        std::vector<std::int32_t> out;
        for (const auto& f : found) {
            if (out.size() >= c.max_joins) break;
            if (host.clear(p, graph.position(f.second))) out.push_back(f.second);
        }
        return out;
    };
    // 2b. Reuse: a joinable node where the chain begins, nearer than a link (a ladder from where the player's climb
    //     ended): the chain starts on that node, with no approach.
    std::int32_t reuse = -1;
    std::int32_t bridge = -1;   // 2b': a near node joined by a walking link to an entry moved out to a link's length
    {
        float best = std::numeric_limits<float>::max();
        const auto near_one = [&](std::size_t k) {
            const auto& n = graph.nodes[k];
            if (!(n.flags & npc_node_nav) || n.parameter_7c < 0 || !linked(k)) return;
            const auto q = at(n.position);
            const float d = full(chain.front(), q);
            if (d >= min_link || d >= best || std::fabs(q[2] - chain.front()[2]) > c.join_max_dz) return;
            if (chain.size() < 2 || full(q, chain[1]) < min_link) return;
            best = d;
            reuse = static_cast<std::int32_t>(k);
        };
        for (std::size_t k = 0; k < first_crumb; ++k) near_one(k);
        for (const auto g : goals)
            if (g >= 0 && static_cast<std::size_t>(g) < graph.nodes.size()) near_one(static_cast<std::size_t>(g));
        // The typed link of a climb or a ladder must start where the player started it: the AI sends the grab or the
        // climb when its turn there ends (0014bff0), and the edge detection 001dfb80 reaches only so far (132ob6: the
        // ladder foot 0.6 short of the mount point). A reused node farther than reuse_reach is not the player's place:
        // the entry is put on the line from that node through the player's place, a link's length from the node (at
        // most bridge_reach past the player's place), and joined to it by a walking link.
        const bool precise = crumb.kind == CoopTraversalKind::ladder || crumb.kind == CoopTraversalKind::climb ||
                             crumb.kind == CoopTraversalKind::climb_down;
        if (reuse >= 0 && precise && best > c.reuse_reach && min_link - best <= c.bridge_reach) {
            const auto q = graph.position(reuse);
            RootVector e = chain.front();
            for (int i = 0; i < 3; ++i) e[i] = q[i] + (e[i] - q[i]) / best * (min_link + 0.01F);
            if (full(e, chain[1]) >= min_link) {
                chain.front() = e;
                bridge = reuse;
                reuse = -1;
            }
        }
    }
    // 2c. The same at the chain's last place (a climb that ends where the static overlay's wall-top node stands): the
    //     chain ends on that node, which joins it to the graph.
    std::int32_t end_reuse = -1;
    if (chain.size() >= 2) {
        float best = std::numeric_limits<float>::max();
        const auto& last = chain.back();
        const auto& before_last = chain[chain.size() - 2];
        const auto near_end = [&](std::size_t k) {
            if (static_cast<std::int32_t>(k) == reuse) return;
            const auto& n = graph.nodes[k];
            if (!(n.flags & npc_node_nav) || n.parameter_7c < 0 || !linked(k)) return;
            const auto q = at(n.position);
            const float d = full(last, q);
            if (d >= min_link || d >= best || std::fabs(q[2] - last[2]) > c.join_max_dz) return;
            if (full(q, before_last) < min_link) return;
            if (chain.size() == 2 && reuse >= 0 && full(q, graph.position(reuse)) < min_link) return;
            best = d;
            end_reuse = static_cast<std::int32_t>(k);
        };
        for (std::size_t k = 0; k < first_crumb; ++k) near_end(k);
        for (const auto g : goals)
            if (g >= 0 && static_cast<std::size_t>(g) < graph.nodes.size()) near_end(static_cast<std::size_t>(g));
    }
    // 3. The approach: back along the player's trail from the chain's first place until a place joins.
    std::vector<RootVector> approach;   // nearest the chain first
    RootVector p = chain.front();
    // With a reused first node its joins are linked to it too (the walk the player made to it, e.g. from a ladder's
    // foot to the wall edge it then drops from, where the static overlay has a one-way link only).
    auto in_joins = joins_of(p);
    if (reuse >= 0) in_joins.erase(std::remove(in_joins.begin(), in_joins.end(), reuse), in_joins.end());
    if (bridge >= 0 && std::find(in_joins.begin(), in_joins.end(), bridge) == in_joins.end()) in_joins.insert(in_joins.begin(), bridge);
    while (reuse < 0 && in_joins.empty()) {
        if (approach.size() >= c.max_approach) {
            why = "no node within " + std::to_string(c.join_radius) + " on a clear line of the player's last " +
                  std::to_string(c.max_approach) + " standing places";
            return std::nullopt;
        }
        while (trail_index >= 0 && full(crumb.before[static_cast<std::size_t>(trail_index)], p) < min_link) --trail_index;
        if (trail_index < 0) {
            why = "no node within " + std::to_string(c.join_radius) + " on a clear line of the player's trail";
            return std::nullopt;
        }
        const auto q = crumb.before[static_cast<std::size_t>(trail_index--)];
        if (full(q, p) > c.approach_step) {
            why = "the player's trail breaks before a join";
            return std::nullopt;
        }
        approach.push_back(q);
        p = q;
        in_joins = joins_of(p);
    }
    auto out_joins = end_reuse >= 0 ? std::vector<std::int32_t>{} : joins_of(chain.back());
    out_joins.erase(std::remove(out_joins.begin(), out_joins.end(), reuse), out_joins.end());
    // 4. The nodes: approach (farthest first), then the chain.
    std::vector<RootVector> places(approach.rbegin(), approach.rend());
    const std::size_t chain_first = places.size();
    places.insert(places.end(), chain.begin(), chain.end());
    if (graph.nodes.size() + places.size() > graph.nodes.capacity())
        throw std::runtime_error("Coop breadcrumbs: the node graph is full (" + std::to_string(graph.nodes.size()) +
                                 " nodes; reserve node_capacity before the route manager links the graph)");
    if (graph.port_overlay.size() < nod_count) graph.port_overlay.resize(nod_count);
    std::int32_t region = reuse >= 0 ? graph.nodes[static_cast<std::size_t>(reuse)].parameter_7c : -1;
    for (const auto j : in_joins)
        if (region < 0) region = graph.nodes[static_cast<std::size_t>(j)].parameter_7c;
    CoopBreadcrumbNodes result;
    if (reuse >= 0) {
        result.nodes.push_back(reuse);   // places[0] (the chain's first place, no approach) is this node
        result.joins.push_back(reuse);
    }
    const std::size_t new_end = end_reuse >= 0 ? places.size() - 1 : places.size();
    for (std::size_t i = reuse >= 0 ? 1 : 0; i < new_end; ++i) {
        const auto& q = places[i];
        const auto rq = host.region(q);
        if (rq >= 0) region = rq;
        NpcNode n;
        n.position = {q[0], q[1], q[2]};
        n.flags = npc_node_nav;
        n.parameter_7c = region;     // the navigation region there, else the one before it on the chain
        n.id = next_id++;
        result.nodes.push_back(static_cast<std::int32_t>(graph.nodes.size()));
        graph.nodes.push_back(std::move(n));
    }
    if (end_reuse >= 0) {
        result.nodes.push_back(end_reuse);   // places.back() is this node
        result.joins.push_back(end_reuse);
    }
    const auto add = [&](std::int32_t from, std::int32_t to, std::int32_t t) {
        if (full(graph.position(from), graph.position(to)) < 2.0F)
            throw std::runtime_error("Coop breadcrumbs: a link not longer than 2.0");
        const NpcNode::Link link{to, t};
        auto& links = static_cast<std::size_t>(from) < nod_count ? graph.port_overlay[static_cast<std::size_t>(from)]
                                                                 : graph.nodes[static_cast<std::size_t>(from)].links;
        for (const auto& l : links)
            if (l.target == to && l.type == t) return;   // already there (a reused node's own link)
        links.push_back(link);
        ++result.links;
    };
    for (std::size_t k = 0; k + 1 < places.size(); ++k) {
        const auto a = result.nodes[k], b = result.nodes[k + 1];
        const bool typed = k >= chain_first + typed_from && k < chain_first + typed_to;
        if (!typed) {
            add(a, b, 2);
            add(b, a, 2);
        } else if (crumb.kind == CoopTraversalKind::crawl) {
            add(a, b, 8);
            add(b, a, 8);   // the NOD's crawls are crossed both ways
        } else {
            add(a, b, type);  // one way: the partner follows the player's direction
        }
    }
    for (const auto j : in_joins) {
        add(j, result.nodes.front(), 2);
        add(result.nodes.front(), j, 2);
        result.joins.push_back(j);
    }
    result.joined_out = !out_joins.empty() || end_reuse >= 0;
    for (const auto j : out_joins) {
        if (std::find(in_joins.begin(), in_joins.end(), j) != in_joins.end()) continue;
        add(result.nodes.back(), j, 2);
        add(j, result.nodes.back(), 2);
        result.joins.push_back(j);
    }
    result.entry = result.nodes[chain_first + typed_from];
    result.goal = result.nodes.back();
    return result;
}

std::size_t disable_coop_breadcrumb(NpcNodeGraph& graph, const CoopBreadcrumbNodes& nodes, CoopTraversalKind kind) {
    const auto type = coop_traversal_link_type(kind);
    std::size_t n = 0;
    const auto off = [&](std::vector<NpcNode::Link>& links) {
        for (auto& l : links) {
            if (l.type != type || l.target < 0 || type == 2) continue;
            if (std::find(nodes.nodes.begin(), nodes.nodes.end(), l.target) == nodes.nodes.end()) continue;
            l.target = -1;
            ++n;
        }
    };
    for (const auto k : nodes.nodes) {
        if (k < 0 || static_cast<std::size_t>(k) >= graph.nodes.size()) continue;
        off(graph.nodes[static_cast<std::size_t>(k)].links);
        if (static_cast<std::size_t>(k) < graph.port_overlay.size()) off(graph.port_overlay[static_cast<std::size_t>(k)]);
    }
    return n;
}

void coop_breadcrumb_push(CoopBreadcrumbQueue& q, const CoopBreadcrumbConfig& c, CoopBreadcrumbTrack track) {
    // Join candidates for later crumbs: every standing node of this one (a crawl's two ends only, not its crouched line).
    const auto keep = [&](std::int32_t k) {
        if (k >= 0 && std::find(q.goals.begin(), q.goals.end(), k) == q.goals.end()) q.goals.push_back(k);
    };
    if (track.kind == CoopTraversalKind::walk) {
        // A walk bridge (132pw5fix2) is this partner's own way to the player, not a join candidate for crumbs.
    } else if (track.kind == CoopTraversalKind::crawl) {
        if (!track.nodes.nodes.empty()) keep(track.nodes.nodes.front());
    } else {
        for (const auto k : track.nodes.nodes) keep(k);
    }
    if (track.kind != CoopTraversalKind::walk) keep(track.nodes.goal);
    ++q.installed;
    q.pending.push_back(std::move(track));
    while (q.pending.size() > c.max_pending) {
        q.pending.pop_front();
        ++q.dropped;
    }
}

std::size_t coop_breadcrumb_clear(CoopBreadcrumbQueue& q) {
    const auto n = q.pending.size();
    q.pending.clear();
    q.cleared += static_cast<std::uint32_t>(n);
    return n;
}

std::optional<CoopBreadcrumbTrack> coop_breadcrumb_route_failed(CoopBreadcrumbQueue& q) {
    if (q.pending.empty()) return std::nullopt;
    auto t = std::move(q.pending.front());
    q.pending.pop_front();
    ++q.dropped;
    return t;
}

CoopBreadcrumbStep coop_breadcrumb_follow(CoopBreadcrumbQueue& q, const CoopBreadcrumbConfig& c, const NpcNodeGraph& graph,
                                          const RootVector& partner, std::uint32_t frame, bool with_player, bool busy,
                                          bool traversing) {
    CoopBreadcrumbStep step;
    if (q.pending.empty()) return step;
    if (with_player && !busy) {
        step.cleared = coop_breadcrumb_clear(q);
        return step;
    }
    while (!q.pending.empty()) {
        auto& head = q.pending.front();
        const auto goal = graph.position(head.nodes.goal);
        if (!traversing && flat(partner, goal) <= c.arrive_distance && std::fabs(partner[2] - goal[2]) <= c.arrive_dz) {
            step.consumed.push_back(std::move(head));
            q.pending.pop_front();
            ++q.consumed;
            continue;
        }
        if (head.sent && head.waited > c.patience_frames) {
            step.dropped.push_back(std::move(head));
            q.pending.pop_front();
            ++q.dropped;
            continue;
        }
        break;
    }
    if (q.pending.empty()) return step;
    step.active = true;
    auto& head = q.pending.front();
    if (busy) {
        head.stall_anchor = partner;   // a traversal or a fight is not a stall
        head.stall_frame = frame;
        return step;
    }
    if (!head.sent) {
        head.sent = frame;
        head.waited = 0;
        head.stall_anchor = partner;
        head.stall_frame = frame;
        step.move_to = head.nodes.goal;
        return step;
    }
    ++head.waited;
    if (full(partner, head.stall_anchor) >= c.stall_move) {
        head.stall_anchor = partner;
        head.stall_frame = frame;
    } else if (frame - head.stall_frame >= c.stall_frames) {
        if (head.resends >= c.max_resends) {
            step.dropped.push_back(std::move(head));
            q.pending.pop_front();
            ++q.dropped;
            step.active = !q.pending.empty();
            return step;
        }
        ++head.resends;
        head.stall_frame = frame;
        step.move_to = head.nodes.goal;
    }
    return step;
}

void coop_breadcrumb_open_tail(CoopBreadcrumbTail& tail, const CoopBreadcrumbNodes& nodes) {
    tail = {};
    if (nodes.joined_out || nodes.goal < 0) return;
    tail.open = true;
    tail.last = nodes.goal;
    tail.own = nodes.nodes;
}

CoopBreadcrumbTailStep coop_breadcrumb_extend_tail(CoopBreadcrumbTail& tail, NpcNodeGraph& graph, std::size_t nod_count,
                                                   std::size_t first_crumb, const RootVector& player,
                                                   const CoopBreadcrumbConfig& c, const CoopBreadcrumbHost& host,
                                                   std::uint32_t& next_id, const std::vector<std::int32_t>& goals) {
    CoopBreadcrumbTailStep step;
    if (!tail.open) return step;
    if (!host.clear || !host.region) throw std::runtime_error("Coop breadcrumbs: the host's clear line and region tests are not linked");
    const auto last = graph.position(tail.last);
    const float d = full(last, player);
    if (d < std::max(c.spacing, min_link)) return step;
    if (d > c.approach_step || tail.count >= c.max_tail) {
        tail.open = false;
        step.closed = true;
        return step;
    }
    if (graph.nodes.size() + 1 > graph.nodes.capacity())
        throw std::runtime_error("Coop breadcrumbs: the node graph is full (" + std::to_string(graph.nodes.size()) + " nodes)");
    if (graph.port_overlay.size() < nod_count) graph.port_overlay.resize(nod_count);
    NpcNode n;
    n.position = {player[0], player[1], player[2]};
    n.flags = npc_node_nav;
    const auto region = host.region(player);
    n.parameter_7c = region >= 0 ? region : graph.nodes[static_cast<std::size_t>(tail.last)].parameter_7c;
    n.id = next_id++;
    const auto k = static_cast<std::int32_t>(graph.nodes.size());
    graph.nodes.push_back(std::move(n));
    const auto add = [&](std::int32_t from, std::int32_t to) {
        const NpcNode::Link link{to, 2};
        if (static_cast<std::size_t>(from) < nod_count) graph.port_overlay[static_cast<std::size_t>(from)].push_back(link);
        else graph.nodes[static_cast<std::size_t>(from)].links.push_back(link);
    };
    add(tail.last, k);
    add(k, tail.last);
    tail.own.push_back(k);
    tail.last = k;
    ++tail.count;
    step.node = k;
    // Joins (as install_coop_breadcrumb's), not the crumb's own nodes.
    std::vector<std::pair<float, std::int32_t>> found;
    const auto consider = [&](std::size_t j) {
        if (std::find(tail.own.begin(), tail.own.end(), static_cast<std::int32_t>(j)) != tail.own.end()) return;
        const auto& m = graph.nodes[j];
        const bool linked = !m.links.empty() || (j < graph.port_overlay.size() && !graph.port_overlay[j].empty());
        if (!(m.flags & npc_node_nav) || m.parameter_7c < 0 || !linked) return;
        const auto q = at(m.position);
        if (std::fabs(q[2] - player[2]) > c.join_max_dz) return;
        const float e = flat(player, q);
        if (e > c.join_radius || full(player, q) < min_link) return;
        for (const auto& f : found)
            if (f.second == static_cast<std::int32_t>(j)) return;
        found.emplace_back(e, static_cast<std::int32_t>(j));
    };
    for (std::size_t j = 0; j < first_crumb && j < graph.nodes.size(); ++j) consider(j);
    for (const auto g : goals)
        if (g >= 0 && static_cast<std::size_t>(g) < graph.nodes.size()) consider(static_cast<std::size_t>(g));
    std::sort(found.begin(), found.end());
    for (const auto& f : found) {
        if (step.joins.size() >= c.max_joins) break;
        if (!host.clear(player, graph.position(f.second))) continue;
        add(k, f.second);
        add(f.second, k);
        step.joins.push_back(f.second);
    }
    if (!step.joins.empty() || tail.count >= c.max_tail) {
        tail.open = false;
        step.closed = true;
    }
    return step;
}

bool coop_breadcrumb_retarget(CoopBreadcrumbQueue& q, std::int32_t from, std::int32_t to) {
    bool any = false;
    for (auto& t : q.pending)
        if (t.nodes.goal == from) {
            t.nodes.goal = to;
            t.nodes.nodes.push_back(to);
            if (t.sent) t.sent.reset();
            any = true;
        }
    if (std::find(q.goals.begin(), q.goals.end(), to) == q.goals.end()) q.goals.push_back(to);
    return any;
}

std::optional<CoopBreadcrumbNodes> install_coop_walk_bridge(NpcNodeGraph& graph, std::size_t nod_count,
                                                            std::size_t first_crumb, const std::vector<RootVector>& trail,
                                                            const RootVector& partner, const RootVector& player,
                                                            std::int32_t start_node, const CoopBreadcrumbConfig& c,
                                                            const CoopBreadcrumbHost& host, std::uint32_t& next_id,
                                                            const std::vector<std::int32_t>& goals, std::string& why) {
    if (!host.clear || !host.region) throw std::runtime_error("Coop breadcrumbs: the host's clear line and region tests are not linked");
    if (nod_count > first_crumb || first_crumb > graph.nodes.size())
        throw std::runtime_error("Coop breadcrumbs: the node counts are out of order");
    // 1. The player's places: its trail, then where it stands.
    std::vector<RootVector> places = trail;
    if (places.empty() || full(places.back(), player) >= min_link) places.push_back(player);
    // 2. The newest place the partner reaches: within join_radius / join_max_dz on a clear line.
    std::optional<std::size_t> first;
    for (std::size_t i = places.size(); i-- > 0;) {
        const auto& p = places[i];
        if (flat(partner, p) > c.join_radius || std::fabs(partner[2] - p[2]) > c.join_max_dz) continue;
        if (!host.clear(partner, p)) continue;
        first = i;
        break;
    }
    if (!first) {
        why = "no place of the player's trail (" + std::to_string(trail.size()) + " places) within " +
              std::to_string(c.join_radius) + " of the partner on a clear line";
        return std::nullopt;
    }
    // 3. The chain: from that place along the trail, each link to the farthest place within bridge_step (and
    //    bridge_step_dz) on a clear line (else the next place); a gap over approach_step (a teleport between) ends it.
    std::vector<RootVector> chain{places[*first]};
    std::size_t cur = *first;
    while (cur + 1 < places.size() && chain.size() < c.bridge_max_nodes) {
        if (full(places[cur], places[cur + 1]) > c.approach_step) break;
        std::size_t next = cur + 1;
        for (std::size_t j = cur + 2; j < places.size(); ++j) {
            if (full(places[j - 1], places[j]) > c.approach_step) break;
            if (full(places[cur], places[j]) > c.bridge_step || std::fabs(places[cur][2] - places[j][2]) > c.bridge_step_dz) break;
            if (host.clear(places[cur], places[j])) next = j;
        }
        if (full(chain.back(), places[next]) >= min_link) chain.push_back(places[next]);
        cur = next;
    }
    // 4. Its start: the partner's route start node joins the chain's first place (a node nearer than a link takes
    //    its place), else the level's nodes there.
    std::vector<std::int32_t> in_joins;
    if (start_node >= 0 && static_cast<std::size_t>(start_node) < graph.nodes.size()) {
        const auto s = graph.position(start_node);
        if (full(s, chain.front()) < min_link) {
            if (chain.size() < 2 || full(s, chain[1]) < min_link) {
                why = "the player is at the partner's route start node";
                return std::nullopt;
            }
            chain.erase(chain.begin());
        }
        if (flat(s, chain.front()) <= 2.0F * c.join_radius && host.clear(s, chain.front())) in_joins.push_back(start_node);
    }
    const auto linked = [&](std::size_t k) {
        return !graph.nodes[k].links.empty() || (k < graph.port_overlay.size() && !graph.port_overlay[k].empty());
    };
    const auto joins_of = [&](const RootVector& p, std::vector<std::int32_t>& out) {
        std::vector<std::pair<float, std::int32_t>> found;
        const auto consider = [&](std::size_t k) {
            const auto& n = graph.nodes[k];
            if (!(n.flags & npc_node_nav) || n.parameter_7c < 0 || !linked(k)) return;
            const auto q = at(n.position);
            if (std::fabs(q[2] - p[2]) > c.join_max_dz) return;
            const float d = flat(p, q);
            if (d > c.join_radius || full(p, q) < min_link) return;
            if (std::find(out.begin(), out.end(), static_cast<std::int32_t>(k)) != out.end()) return;
            for (const auto& f : found)
                if (f.second == static_cast<std::int32_t>(k)) return;
            found.emplace_back(d, static_cast<std::int32_t>(k));
        };
        for (std::size_t k = 0; k < first_crumb; ++k) consider(k);
        for (const auto g : goals)
            if (g >= 0 && static_cast<std::size_t>(g) < graph.nodes.size()) consider(static_cast<std::size_t>(g));
        std::sort(found.begin(), found.end());
        std::size_t added = 0;
        for (const auto& f : found) {
            if (added >= c.max_joins) break;
            if (host.clear(p, graph.position(f.second))) {
                out.push_back(f.second);
                ++added;
            }
        }
    };
    joins_of(chain.front(), in_joins);
    if (in_joins.empty()) {
        why = "nothing joins the bridge's start (the partner's route start node " +
              (start_node >= 0 ? std::to_string(graph.nodes.at(static_cast<std::size_t>(start_node)).id) : std::string("none")) +
              " is not on a clear line within " + std::to_string(2.0F * c.join_radius) + ")";
        return std::nullopt;
    }
    std::vector<std::int32_t> out_joins;
    joins_of(chain.back(), out_joins);
    out_joins.erase(std::remove_if(out_joins.begin(), out_joins.end(),
                                   [&](std::int32_t j) { return std::find(in_joins.begin(), in_joins.end(), j) != in_joins.end(); }),
                    out_joins.end());
    if (graph.nodes.size() + chain.size() + c.bridge_reserve > graph.nodes.capacity()) {
        why = "the node graph has no room (" + std::to_string(graph.nodes.size()) + " of " +
              std::to_string(graph.nodes.capacity()) + " nodes)";
        return std::nullopt;
    }
    if (graph.port_overlay.size() < nod_count) graph.port_overlay.resize(nod_count);
    CoopBreadcrumbNodes result;
    std::int32_t region = -1;
    for (const auto j : in_joins)
        if (region < 0) region = graph.nodes[static_cast<std::size_t>(j)].parameter_7c;
    for (const auto& q : chain) {
        const auto rq = host.region(q);
        if (rq >= 0) region = rq;
        NpcNode n;
        n.position = {q[0], q[1], q[2]};
        n.flags = npc_node_nav;
        n.parameter_7c = region;
        n.id = next_id++;
        result.nodes.push_back(static_cast<std::int32_t>(graph.nodes.size()));
        graph.nodes.push_back(std::move(n));
    }
    const auto add = [&](std::int32_t from, std::int32_t to) {
        if (full(graph.position(from), graph.position(to)) < 2.0F) throw std::runtime_error("Coop breadcrumbs: a link not longer than 2.0");
        auto& links = static_cast<std::size_t>(from) < nod_count ? graph.port_overlay[static_cast<std::size_t>(from)]
                                                                 : graph.nodes[static_cast<std::size_t>(from)].links;
        for (const auto& l : links)
            if (l.target == to && l.type == 2) return;
        links.push_back(NpcNode::Link{to, 2});
        ++result.links;
    };
    for (std::size_t k = 0; k + 1 < result.nodes.size(); ++k) {
        add(result.nodes[k], result.nodes[k + 1]);
        add(result.nodes[k + 1], result.nodes[k]);
    }
    for (const auto j : in_joins) {
        add(j, result.nodes.front());
        add(result.nodes.front(), j);
        result.joins.push_back(j);
    }
    for (const auto j : out_joins) {
        add(result.nodes.back(), j);
        add(j, result.nodes.back());
        result.joins.push_back(j);
    }
    result.joined_out = !out_joins.empty();
    result.entry = result.nodes.front();
    result.goal = result.nodes.back();
    return result;
}
}
