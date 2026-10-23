// Port-only (checkpoint 132md11): see mesh_simplify.hpp.
#include "mesh_simplify.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <exception>
#include <mutex>
#include <thread>
#include <tuple>

namespace omega::gfx {
namespace {

constexpr std::uint32_t removed = std::numeric_limits<std::uint32_t>::max();
constexpr double border_weight = 10.0;

struct Quadric {
    double a00 = 0, a01 = 0, a02 = 0, a11 = 0, a12 = 0, a22 = 0, b0 = 0, b1 = 0, b2 = 0, c = 0;
    double area = 0;   // the face planes' total weight (the border planes' is not in it)
    void plane(double x, double y, double z, double d, double weight) {
        a00 += weight * x * x; a01 += weight * x * y; a02 += weight * x * z;
        a11 += weight * y * y; a12 += weight * y * z; a22 += weight * z * z;
        b0 += weight * x * d; b1 += weight * y * d; b2 += weight * z * d; c += weight * d * d;
    }
    void add(const Quadric& q) {
        a00 += q.a00; a01 += q.a01; a02 += q.a02; a11 += q.a11; a12 += q.a12; a22 += q.a22;
        b0 += q.b0; b1 += q.b1; b2 += q.b2; c += q.c; area += q.area;
    }
    double eval(const std::array<float, 3>& p) const {
        const double x = p[0], y = p[1], z = p[2];
        return a00 * x * x + a11 * y * y + a22 * z * z + 2 * (a01 * x * y + a02 * x * z + a12 * y * z) +
               2 * (b0 * x + b1 * y + b2 * z) + c;
    }
};

using V3 = std::array<double, 3>;
V3 sub(const std::array<float, 3>& a, const std::array<float, 3>& b) { return {double(a[0]) - b[0], double(a[1]) - b[1], double(a[2]) - b[2]}; }
V3 cross(const V3& a, const V3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
double dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

struct PositionKey {
    std::uint32_t x, y, z;
    bool operator==(const PositionKey&) const = default;
};
// Runs fn(first, last, part) over [0, n) in `parts` contiguous ranges, on threads when the mesh is large. Each part's
// results are kept apart by the caller, so the outcome does not depend on the thread count.
template <class Fn> void for_ranges(std::size_t n, std::size_t parts, Fn fn) {
    if (parts <= 1) return fn(std::size_t{0}, n, std::size_t{0});
    std::vector<std::thread> threads;
    threads.reserve(parts - 1);
    std::exception_ptr failure;
    std::mutex failure_mutex;
    for (std::size_t p = 0; p < parts; ++p) {
        const auto first = n * p / parts, last = n * (p + 1) / parts;
        const auto run = [&, first, last, p] {
            try {
                fn(first, last, p);
            } catch (...) {
                std::lock_guard lock(failure_mutex);
                if (!failure) failure = std::current_exception();
            }
        };
        if (p + 1 == parts) run();
        else threads.emplace_back(run);
    }
    for (auto& t : threads) t.join();
    if (failure) std::rethrow_exception(failure);
}

enum class Kind : std::uint8_t { locked, interior, border };

struct Candidate {
    double error;
    std::uint32_t from, to;
};

class Simplifier {
public:
    Simplifier(std::span<const std::array<float, 3>> positions, std::span<const std::array<std::uint32_t, 3>> triangles)
        : positions_(positions), root_(positions.size()), wedges_(positions.size(), 0), quadrics_(positions.size()) {
        const std::size_t n = positions.size();
        if (n >= removed) throw std::runtime_error("simplify: too many vertices");
        // Weld: each vertex's root is the lowest index at its exact position.
        std::vector<std::pair<PositionKey, std::uint32_t>> keys(n);
        for (std::size_t i = 0; i < n; ++i) {
            std::memcpy(&keys[i].first, positions[i].data(), sizeof(PositionKey));
            keys[i].second = static_cast<std::uint32_t>(i);
        }
        std::sort(keys.begin(), keys.end(), [](const auto& x, const auto& y) {
            return std::tie(x.first.x, x.first.y, x.first.z, x.second) < std::tie(y.first.x, y.first.y, y.first.z, y.second);
        });
        for (std::size_t i = 0; i < n; ++i)
            root_[keys[i].second] = i && keys[i].first == keys[i - 1].first ? root_[keys[i - 1].second] : keys[i].second;
        std::vector<std::uint8_t> used(n, 0);
        tris_.reserve(triangles.size());
        for (const auto& t : triangles) {
            for (const auto i : t)
                if (i >= n) throw std::runtime_error("simplify: a triangle names a vertex outside the mesh");
            const auto a = root_[t[0]], b = root_[t[1]], c = root_[t[2]];
            if (a == b || b == c || a == c) continue;   // degenerate: not in any level
            tris_.push_back(t);
            for (const auto i : t) used[i] = 1;
        }
        for (std::size_t i = 0; i < n; ++i)
            if (used[i]) ++wedges_[root_[i]];
        // Face planes (area weighted) on every corner's position.
        for (const auto& t : tris_) {
            const auto& p0 = positions_[t[0]];
            const V3 normal = cross(sub(positions_[t[1]], p0), sub(positions_[t[2]], p0));
            const double length = std::sqrt(dot(normal, normal));
            if (length <= 0) continue;
            const double area = 0.5 * length;
            const V3 u{normal[0] / length, normal[1] / length, normal[2] / length};
            const double d = -(u[0] * p0[0] + u[1] * p0[1] + u[2] * p0[2]);
            for (const auto i : t) {
                auto& q = quadrics_[root_[i]];
                q.plane(u[0], u[1], u[2], d, area);
                q.area += area;
            }
        }
        // Large meshes classify and pick candidates on up to 8 threads (each vertex alone, so the levels are the same).
        if (n >= 32768) parts_ = std::clamp<std::size_t>(std::thread::hardware_concurrency(), 1, 8);
        scratch_.resize(parts_);
        adjacency();
        // Border planes: through each border edge, perpendicular to its face, holding the outline in place.
        for (std::size_t t = 0; t < tris_.size(); ++t)
            for (std::size_t k = 0; k < 3; ++k) {
                const auto a = root_[tris_[t][k]], b = root_[tris_[t][(k + 1) % 3]];
                if (has_edge(b, a)) continue;
                const auto& pa = positions_[a];
                const V3 e = sub(positions_[b], pa);
                const V3 normal = cross(sub(positions_[root_[tris_[t][1]]], positions_[root_[tris_[t][0]]]),
                                        sub(positions_[root_[tris_[t][2]]], positions_[root_[tris_[t][0]]]));
                V3 m = cross(e, normal);
                const double length = std::sqrt(dot(m, m));
                if (length <= 0) continue;
                m = {m[0] / length, m[1] / length, m[2] / length};
                const double d = -(m[0] * pa[0] + m[1] * pa[1] + m[2] * pa[2]);
                const double weight = border_weight * dot(e, e);
                quadrics_[a].plane(m[0], m[1], m[2], d, weight);
                quadrics_[b].plane(m[0], m[1], m[2], d, weight);
            }
    }

    std::size_t count() const { return live_; }
    float error() const { return static_cast<float>(max_error_); }
    std::vector<std::array<std::uint32_t, 3>> triangles() const { return tris_; }

    // One pass of independent collapses, cheapest first, down to `target` triangles. False: none was possible.
    bool pass(std::size_t target) {
        adjacency();
        classify();
        candidates_.clear();
        for_ranges(kind_.size(), parts_, [&](std::size_t first, std::size_t last, std::size_t part) {
            auto& s = scratch_[part];
            s.candidates.clear();
            for (auto a = static_cast<std::uint32_t>(first); a < last; ++a) candidate(a, s);
        });
        for (const auto& s : scratch_) candidates_.insert(candidates_.end(), s.candidates.begin(), s.candidates.end());
        if (candidates_.empty()) return false;
        std::sort(candidates_.begin(), candidates_.end(), [](const Candidate& x, const Candidate& y) {
            return x.error != y.error ? x.error < y.error : x.from < y.from;
        });
        // Cheapest first across passes: this pass takes only candidates up to the cost of the one that would reach the
        // target if every collapse (two triangles each) went through.
        const std::size_t needed = std::min(candidates_.size(), (live_ - target) / 2 + 1);
        const double limit = candidates_[needed - 1].error;
        touched_.assign(kind_.size(), 0);
        bool any = false;

        for (const auto& c : candidates_) {
            if (live_ <= target || c.error > limit) break;
            if (touched_[c.from] || touched_[c.to]) continue;
            if (!collapse(c)) continue;
            any = true;
            max_error_ = std::max(max_error_, c.error);
        }

        if (any) tris_.erase(std::remove_if(tris_.begin(), tris_.end(), [](const auto& t) { return t[0] == removed; }), tris_.end());
        return any;
    }

private:
    double cost(std::uint32_t a, std::uint32_t b) const {
        Quadric q = quadrics_[a];
        q.add(quadrics_[b]);
        const double value = std::max(0.0, q.eval(positions_[b]));
        return std::sqrt(value / std::max(q.area, 1e-30));
    }

    // Per position (a welded root), its triangles (CSR over tris_).
    void adjacency() {
        const std::size_t n = root_.size();
        offsets_.assign(n + 1, 0);
        for (const auto& t : tris_)
            if (t[0] != removed)
                for (const auto i : t) ++offsets_[root_[i] + 1];
        for (std::size_t i = 0; i < n; ++i) offsets_[i + 1] += offsets_[i];
        incident_.resize(offsets_[n]);
        fill_.assign(offsets_.begin(), offsets_.end() - 1);
        for (std::uint32_t t = 0; t < tris_.size(); ++t)
            if (tris_[t][0] != removed)
                for (const auto i : tris_[t]) incident_[fill_[root_[i]]++] = t;
        live_ = 0;
        for (const auto& t : tris_)
            if (t[0] != removed) ++live_;
    }

    // A triangle around position a with the directed edge a -> b.
    bool has_edge(std::uint32_t a, std::uint32_t b) const { return edge_count(a, b) > 0; }
    unsigned edge_count(std::uint32_t a, std::uint32_t b) const {
        unsigned found = 0;
        for (std::uint32_t k = offsets_[a]; k < offsets_[a + 1]; ++k) {
            const auto& t = tris_[incident_[k]];
            for (std::size_t j = 0; j < 3; ++j)
                if (root_[t[j]] == a && root_[t[(j + 1) % 3]] == b) ++found;
        }
        return found;
    }

    // Each position's kind from its directed edges: a border edge a -> b has no b -> a; a repeated directed edge (in or
    // out) makes it non-manifold. A border position moves only to its border neighbours (border_[a]: next, previous).
    void classify() {
        const std::size_t n = root_.size();
        kind_.assign(n, Kind::locked);
        border_.resize(n);
        for_ranges(n, parts_, [&](std::size_t first, std::size_t last, std::size_t part) {
            for (auto a = static_cast<std::uint32_t>(first); a < last; ++a) classify(a, scratch_[part]);
        });
    }
    struct Scratch {
        std::vector<std::uint32_t> out, in;
        std::vector<Candidate> options, candidates;
    };
    void classify(std::uint32_t a, Scratch& s) {
        if (root_[a] != a || wedges_[a] != 1 || offsets_[a] == offsets_[a + 1]) return;
        auto& out = s.out;
        auto& in = s.in;
        out.clear();
        in.clear();
        for (std::uint32_t k = offsets_[a]; k < offsets_[a + 1]; ++k) {
            const auto& t = tris_[incident_[k]];
            for (std::size_t j = 0; j < 3; ++j)
                if (root_[t[j]] == a) {
                    out.push_back(root_[t[(j + 1) % 3]]);
                    in.push_back(root_[t[(j + 2) % 3]]);
                }
        }
        std::sort(out.begin(), out.end());
        std::sort(in.begin(), in.end());
        if (std::adjacent_find(out.begin(), out.end()) != out.end() || std::adjacent_find(in.begin(), in.end()) != in.end()) return;
        std::uint32_t next = removed, previous = removed;
        unsigned outs = 0, ins = 0;
        for (const auto b : out)
            if (!std::binary_search(in.begin(), in.end(), b)) ++outs, next = b;
        for (const auto c : in)
            if (!std::binary_search(out.begin(), out.end(), c)) ++ins, previous = c;
        if (!outs && !ins) {
            kind_[a] = Kind::interior;
        } else if (outs == 1 && ins == 1) {
            kind_[a] = Kind::border;
            border_[a] = {next, previous};
        }
    }
    // Per vertex its cheapest move that flips nothing (ties: the shorter edge, then the lower index).
    void candidate(std::uint32_t a, Scratch& s) const {
        if (kind_[a] == Kind::locked) return;
        auto& options = s.options;
        options.clear();
        if (kind_[a] == Kind::border) {
            for (const auto b : border_[a]) options.push_back({cost(a, b), a, b});
        } else {
            for (std::uint32_t k = offsets_[a]; k < offsets_[a + 1]; ++k) {
                const auto& t = tris_[incident_[k]];
                for (std::size_t j = 0; j < 3; ++j)
                    if (root_[t[j]] == a) options.push_back({cost(a, root_[t[(j + 1) % 3]]), a, root_[t[(j + 1) % 3]]});
            }
        }
        const auto& pa = positions_[a];
        const auto length = [&](std::uint32_t b) { const V3 e = sub(positions_[b], pa); return dot(e, e); };
        std::sort(options.begin(), options.end(), [&](const Candidate& x, const Candidate& y) {
            if (x.error != y.error) return x.error < y.error;
            if (x.to == y.to) return false;
            const double lx = length(x.to), ly = length(y.to);
            return lx != ly ? lx < ly : x.to < y.to;
        });
        options.erase(std::unique(options.begin(), options.end(), [](const Candidate& x, const Candidate& y) { return x.to == y.to; }),
                       options.end());
        for (const auto& option : options)
            if (!flips(a, option.to)) {
                s.candidates.push_back(option);
                return;
            }
    }

    // Whether moving position a onto b flips a triangle left around a or collapses it to a sliver (its normal turns by
    // more than about 75 degrees, or its area vanishes).
    bool flips(std::uint32_t a, std::uint32_t b) const {
        const auto& pb = positions_[b];
        for (std::uint32_t k = offsets_[a]; k < offsets_[a + 1]; ++k) {
            const auto& t = tris_[incident_[k]];
            if (root_[t[0]] == b || root_[t[1]] == b || root_[t[2]] == b) continue;
            std::array<std::array<float, 3>, 3> p{positions_[root_[t[0]]], positions_[root_[t[1]]], positions_[root_[t[2]]]};
            const V3 before = cross(sub(p[1], p[0]), sub(p[2], p[0]));
            for (std::size_t j = 0; j < 3; ++j)
                if (root_[t[j]] == a) p[j] = pb;
            const V3 after = cross(sub(p[1], p[0]), sub(p[2], p[0]));
            const double lb = std::sqrt(dot(before, before)), la = std::sqrt(dot(after, after));
            if (la <= 1e-12 * std::max(lb, 1e-30) || dot(before, after) < 0.25 * lb * la) return true;
        }
        return false;
    }

    bool collapse(const Candidate& c) {
        const auto a = c.from, b = c.to;
        // The vertex index b's corners have in the triangles on the edge (one: a has no seam), and the link condition:
        // the positions next to both are exactly those triangles' third corners.
        std::uint32_t b_index = removed;
        neighbours_a_.clear();
        opposite_.clear();
        for (std::uint32_t k = offsets_[a]; k < offsets_[a + 1]; ++k) {
            const auto& t = tris_[incident_[k]];
            bool with_b = false;
            for (const auto i : t)
                if (root_[i] == b) {
                    if (b_index != removed && b_index != i) return false;
                    b_index = i;
                    with_b = true;
                }
            for (const auto i : t) {
                const auto r = root_[i];
                if (r == a) continue;
                neighbours_a_.push_back(r);
                if (with_b && r != b) opposite_.push_back(r);
            }
        }
        if (b_index == removed) return false;
        std::sort(neighbours_a_.begin(), neighbours_a_.end());
        neighbours_a_.erase(std::unique(neighbours_a_.begin(), neighbours_a_.end()), neighbours_a_.end());
        std::sort(opposite_.begin(), opposite_.end());
        opposite_.erase(std::unique(opposite_.begin(), opposite_.end()), opposite_.end());
        common_.clear();
        for (std::uint32_t k = offsets_[b]; k < offsets_[b + 1]; ++k)
            for (const auto i : tris_[incident_[k]]) {
                const auto r = root_[i];
                if (r != b && r != a && std::binary_search(neighbours_a_.begin(), neighbours_a_.end(), r)) common_.push_back(r);
            }
        std::sort(common_.begin(), common_.end());
        common_.erase(std::unique(common_.begin(), common_.end()), common_.end());
        if (common_ != opposite_) return false;
        if (flips(a, b)) return false;
        for (std::uint32_t k = offsets_[a]; k < offsets_[a + 1]; ++k) {
            auto& t = tris_[incident_[k]];
            if (root_[t[0]] == b || root_[t[1]] == b || root_[t[2]] == b) {
                t = {removed, removed, removed};
                --live_;
                continue;
            }
            for (auto& i : t)
                if (root_[i] == a) i = b_index;
        }
        quadrics_[b].add(quadrics_[a]);
        touched_[a] = touched_[b] = 1;
        for (const auto r : neighbours_a_) touched_[r] = 1;
        return true;
    }

    std::span<const std::array<float, 3>> positions_;
    std::vector<std::uint32_t> root_;
    std::vector<std::uint32_t> wedges_;
    std::vector<Quadric> quadrics_;
    std::vector<std::array<std::uint32_t, 3>> tris_;
    std::vector<std::uint32_t> offsets_, incident_, fill_;
    std::vector<Kind> kind_;
    std::vector<std::array<std::uint32_t, 2>> border_;

    std::vector<std::uint8_t> touched_;
    std::vector<Candidate> candidates_;
    std::size_t parts_ = 1;
    std::vector<Scratch> scratch_;
    std::vector<std::uint32_t> neighbours_a_, opposite_, common_;
    std::size_t live_ = 0;
    double max_error_ = 0;
};

}  // namespace

std::vector<SimplifiedLevel> simplify_levels(std::span<const std::array<float, 3>> positions,
                                             std::span<const std::array<std::uint32_t, 3>> triangles,
                                             const SimplifyOptions& options) {
    std::vector<SimplifiedLevel> levels;
    if (triangles.size() <= options.min_triangles || options.ratio <= 0 || options.ratio >= 1) return levels;
    Simplifier s(positions, triangles);
    std::size_t previous = triangles.size();
    const auto keep = [&] {
        const std::size_t now = s.count();
        if (double(now) > double(previous) * (1.0 - options.min_reduction)) return false;
        levels.push_back({s.triangles(), s.error()});
        previous = now;
        return true;
    };
    while (levels.size() < options.max_levels && previous > options.min_triangles) {
        const std::size_t target = std::max<std::size_t>(options.min_triangles, static_cast<std::size_t>(double(previous) * options.ratio));
        bool progress = true;
        for (unsigned passes = 0; s.count() > target && passes < 64 && progress; ++passes) progress = s.pass(target);
        if (!keep() || !progress) break;
    }
    return levels;
}

}  // namespace omega::gfx
