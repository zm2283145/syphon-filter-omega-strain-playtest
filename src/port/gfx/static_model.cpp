#include "static_model.hpp"

#include "material_anim.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <stdexcept>

namespace omega::gfx {
namespace {

using gltf::Json;
using gltf::Matrix;
using Vec = std::array<float, 3>;

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error(message); }

const Json& array_member(const Json& object, const char* key) {
    static const Json empty = Json::make_array();
    const auto* v = object.find(key);
    return v && v->type == Json::Type::array ? *v : empty;
}

Vec normalized(Vec v, Vec fallback) {
    const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!(length > 1e-12f) || !std::isfinite(length)) return fallback;
    return {v[0] / length, v[1] / length, v[2] / length};
}
Vec transform_point(const Matrix& m, const Vec& p) {
    return {m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12], m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13],
            m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14]};
}
// The cofactor of the upper 3x3 (det * inverse transpose): normals stay perpendicular under non-uniform scale.
Matrix normal_matrix(const Matrix& m, float& det) {
    const auto a = [&](int r, int c) { return m[static_cast<std::size_t>(c * 4 + r)]; };
    Matrix n = gltf::identity();
    const auto set = [&](int r, int c, float v) { n[static_cast<std::size_t>(c * 4 + r)] = v; };
    set(0, 0, a(1, 1) * a(2, 2) - a(1, 2) * a(2, 1));
    set(0, 1, a(1, 2) * a(2, 0) - a(1, 0) * a(2, 2));
    set(0, 2, a(1, 0) * a(2, 1) - a(1, 1) * a(2, 0));
    set(1, 0, a(0, 2) * a(2, 1) - a(0, 1) * a(2, 2));
    set(1, 1, a(0, 0) * a(2, 2) - a(0, 2) * a(2, 0));
    set(1, 2, a(0, 1) * a(2, 0) - a(0, 0) * a(2, 1));
    set(2, 0, a(0, 1) * a(1, 2) - a(0, 2) * a(1, 1));
    set(2, 1, a(0, 2) * a(1, 0) - a(0, 0) * a(1, 2));
    set(2, 2, a(0, 0) * a(1, 1) - a(0, 1) * a(1, 0));
    det = a(0, 0) * n[0] + a(0, 1) * n[1] + a(0, 2) * n[2];
    return n;
}
Vec transform_vector(const Matrix& m, const Vec& v) {
    return {m[0] * v[0] + m[4] * v[1] + m[8] * v[2], m[1] * v[0] + m[5] * v[1] + m[9] * v[2], m[2] * v[0] + m[6] * v[1] + m[10] * v[2]};
}

std::string material_name(std::size_t index) {
    std::string name = "material_";
    if (index < 10) name.push_back('0');
    return name + std::to_string(index);
}
// material_NN, material_NN_<suffix> or material_NN.<suffix> (an editor's duplicate): NN, else -1.
long parse_material_name(const std::string& name) {
    if (!name.starts_with("material_")) return -1;
    std::size_t at = 9;
    long value = 0;
    while (at < name.size() && at < 15 && name[at] >= '0' && name[at] <= '9') value = value * 10 + (name[at++] - '0');
    if (at == 9 || (at < name.size() && name[at] != '_' && name[at] != '.')) return -1;
    return value;
}

bool animated_material(const Mesh& mesh, std::uint32_t m) {
    return m < mesh.materials.size() && material_animation_kind(mesh.materials[m].flags) != MaterialAnimationKind::none;
}

// The original's first drawn material that is not animated, else its first drawn (the default for unnamed glTF
// materials).
std::uint32_t first_drawn_material(const Mesh& original) {
    for (const auto& s : original.surfaces)
        if (s.index_count && !animated_material(original, s.material)) return s.material;
    for (const auto& s : original.surfaces)
        if (s.index_count) return s.material;
    return 0;
}

// The colour bytes the game's vertex colours are (byte / 128; play's terrain_base_colours).
std::array<std::uint8_t, 3> colour_bytes(const MeshVertex& v) {
    std::array<std::uint8_t, 3> c{};
    for (std::size_t k = 0; k < 3; ++k) c[k] = static_cast<std::uint8_t>(std::clamp<long>(std::lround(v.color[k] * 128.0f), 0, 255));
    return c;
}

// The nearest original vertex to a point: a uniform grid of the original's vertices searched in growing shells. Equal
// distances prefer the vertex whose colour bytes and UV match the query's, then the lower index.
class NearestVertex {
public:
    explicit NearestVertex(const std::vector<MeshVertex>& points) : points_(points) {
        if (points.empty()) return;
        Vec hi{-1e30f, -1e30f, -1e30f};
        lo_ = {1e30f, 1e30f, 1e30f};
        for (const auto& v : points) {
            const float q[3]{v.position.x, v.position.y, v.position.z};
            for (std::size_t k = 0; k < 3; ++k) {
                lo_[k] = std::min(lo_[k], q[k]);
                hi[k] = std::max(hi[k], q[k]);
            }
        }
        const float largest = std::max({hi[0] - lo_[0], hi[1] - lo_[1], hi[2] - lo_[2], 1e-6f});
        double volume = 1;
        for (std::size_t k = 0; k < 3; ++k) volume *= std::max(static_cast<double>(hi[k] - lo_[k]), largest * 1e-3);
        cell_ = static_cast<float>(std::cbrt(volume / static_cast<double>(points.size())));
        for (;;) {
            std::size_t total = 1;
            for (std::size_t k = 0; k < 3; ++k) {
                dims_[k] = std::clamp(static_cast<int>(std::ceil((hi[k] - lo_[k]) / cell_)), 1, 1024);
                total *= static_cast<std::size_t>(dims_[k]);
            }
            if (total <= points.size() * 8 + 64) break;
            cell_ *= 1.26f;
        }
        const auto cells = static_cast<std::size_t>(dims_[0]) * static_cast<std::size_t>(dims_[1]) * static_cast<std::size_t>(dims_[2]);
        start_.assign(cells + 1, 0);
        std::vector<std::size_t> cell_of(points.size());
        for (std::size_t i = 0; i < points.size(); ++i) {
            const auto c = cell(points[i].position);
            cell_of[i] = index(c[0], c[1], c[2]);
            ++start_[cell_of[i] + 1];
        }
        for (std::size_t c = 0; c < cells; ++c) start_[c + 1] += start_[c];
        items_.resize(points.size());
        auto fill = start_;
        for (std::size_t i = 0; i < points.size(); ++i) items_[fill[cell_of[i]]++] = static_cast<std::uint32_t>(i);
    }

    std::uint32_t nearest(const MeshVertex& query) const {
        const auto c = cell(query.position);
        const auto bytes = colour_bytes(query);
        std::uint32_t best = UINT32_MAX;
        double best_distance = 0;
        int best_mismatch = 0;
        const auto visit = [&](int x, int y, int z) {
            const auto at = index(x, y, z);
            for (std::size_t i = start_[at]; i < start_[at + 1]; ++i) {
                const auto o = items_[i];
                const auto& p = points_[o].position;
                const double dx = p.x - query.position.x, dy = p.y - query.position.y, dz = p.z - query.position.z;
                const double distance = dx * dx + dy * dy + dz * dz;
                const int mismatch = (colour_bytes(points_[o]) != bytes) + (points_[o].uv != query.uv);
                if (best == UINT32_MAX || distance < best_distance ||
                    (distance == best_distance && (mismatch < best_mismatch || (mismatch == best_mismatch && o < best)))) {
                    best = o;
                    best_distance = distance;
                    best_mismatch = mismatch;
                }
            }
        };
        const int reach = std::max({dims_[0], dims_[1], dims_[2]});
        for (int r = 0; r <= reach; ++r) {
            for (int x = std::max(0, c[0] - r); x <= std::min(dims_[0] - 1, c[0] + r); ++x)
                for (int y = std::max(0, c[1] - r); y <= std::min(dims_[1] - 1, c[1] + r); ++y) {
                    if (std::abs(x - c[0]) == r || std::abs(y - c[1]) == r) {
                        for (int z = std::max(0, c[2] - r); z <= std::min(dims_[2] - 1, c[2] + r); ++z) visit(x, y, z);
                    } else {
                        if (c[2] - r >= 0) visit(x, y, c[2] - r);
                        if (r && c[2] + r < dims_[2]) visit(x, y, c[2] + r);
                    }
                }
            // Every vertex outside shell r is at least r cells away (a query outside the grid is farther still).
            const double reached = static_cast<double>(r) * cell_;
            if (best != UINT32_MAX && best_distance < reached * reached) break;
        }
        return best;
    }

private:
    std::array<int, 3> cell(const Vec3& p) const {
        const float q[3]{p.x, p.y, p.z};
        std::array<int, 3> c{};
        for (std::size_t k = 0; k < 3; ++k) {
            const float f = std::floor((q[k] - lo_[k]) / cell_);
            c[k] = std::isfinite(f) ? static_cast<int>(std::clamp(f, 0.0f, static_cast<float>(dims_[k] - 1))) : 0;
        }
        return c;
    }
    std::size_t index(int x, int y, int z) const {
        return (static_cast<std::size_t>(z) * static_cast<std::size_t>(dims_[1]) + static_cast<std::size_t>(y)) * static_cast<std::size_t>(dims_[0]) +
               static_cast<std::size_t>(x);
    }

    const std::vector<MeshVertex>& points_;
    Vec lo_{};
    float cell_ = 1;
    std::array<int, 3> dims_{1, 1, 1};
    std::vector<std::size_t> start_;
    std::vector<std::uint32_t> items_;
};

}  // namespace

std::string static_model_blocker(const Mesh& original) {
    if (original.materials.empty()) return "the disc's model has no material";
    return {};
}

std::string static_model_remaps(const Mesh& original) {
    std::string out;
    if (!original.color_patches.empty())
        out = std::to_string(original.color_patches.size()) +
              " colour patch sets (its lights switch its vertex colours), remapped to the replacement's nearest vertices";
    std::set<std::uint32_t> animated;
    for (const auto& s : original.surfaces)
        if (animated_material(original, s.material)) animated.insert(s.material);
    if (!animated.empty()) {
        if (!out.empty()) out += "; ";
        std::string names;
        for (const auto m : animated) names += (names.empty() ? "" : ", ") + material_name(m);
        out += "animated " + names + " (UV scroll / texture frames), kept with the disc's textures";
    }
    return out;
}

std::vector<ColorPatchSet> remap_color_patches(const Mesh& original, const Mesh& replacement, std::size_t* changed) {
    std::vector<ColorPatchSet> out;
    if (changed) *changed = 0;
    if (original.color_patches.empty()) return out;
    const auto n = original.vertices.size();
    std::vector<std::array<std::uint8_t, 3>> original_bytes(n), replacement_bytes(replacement.vertices.size());
    for (std::size_t i = 0; i < n; ++i) original_bytes[i] = colour_bytes(original.vertices[i]);
    for (std::size_t i = 0; i < replacement.vertices.size(); ++i) replacement_bytes[i] = colour_bytes(replacement.vertices[i]);
    std::vector<std::uint32_t> nearest(replacement.vertices.size(), UINT32_MAX);
    if (n) {
        const NearestVertex grid(original.vertices);
        for (std::size_t v = 0; v < replacement.vertices.size(); ++v) nearest[v] = grid.nearest(replacement.vertices[v]);
    }
    std::vector<bool> any(replacement.vertices.size(), false);
    std::vector<std::array<std::uint8_t, 3>> delta(n);
    for (const auto& set : original.color_patches) {
        // The set's change per original vertex: its spans' payloads, wrapping (003767e0 applies them in turn).
        std::fill(delta.begin(), delta.end(), std::array<std::uint8_t, 3>{});
        for (const auto& span : set.spans) {
            if (span.first_vertex > n || span.colors.size() > n - span.first_vertex) fail("a colour patch span is outside the vertices");
            for (std::size_t j = 0; j < span.colors.size(); ++j)
                for (std::size_t c = 0; c < 3; ++c)
                    delta[span.first_vertex + j][c] = static_cast<std::uint8_t>(delta[span.first_vertex + j][c] + (span.colors[j] >> (8 * c)));
        }
        ColorPatchSet remapped{set.key, {}};
        ColorPatchSpan* open = nullptr;
        for (std::size_t v = 0; v < replacement.vertices.size(); ++v) {
            const auto o = nearest[v];
            std::uint32_t word = 0;
            if (o != UINT32_MAX && delta[o] != std::array<std::uint8_t, 3>{}) {
                for (std::size_t c = 0; c < 3; ++c) {
                    const int base = original_bytes[o][c], mine = replacement_bytes[v][c];
                    const int off = static_cast<std::uint8_t>(base - delta[o][c]);
                    const int d = base - off;   // the signed change the set makes (off state in 0..255)
                    long scaled = base ? std::lround(static_cast<double>(d) * mine / base) : d;
                    scaled = std::clamp<long>(scaled, mine - 255L, mine);
                    word |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(scaled)) << (8 * c);
                }
            }
            if (!word) {
                open = nullptr;
                continue;
            }
            any[v] = true;
            if (!open || open->first_vertex + open->colors.size() != v) {
                remapped.spans.push_back({0, static_cast<std::uint32_t>(v), {}});
                open = &remapped.spans.back();
            }
            open->colors.push_back(word);
        }
        out.push_back(std::move(remapped));
    }
    if (changed) *changed = static_cast<std::size_t>(std::count(any.begin(), any.end(), true));
    return out;
}

StaticReplacement load_static_replacement(const gltf::Document& doc, const Mesh& original) {
    if (const auto blocker = static_model_blocker(original); !blocker.empty()) fail(blocker);
    StaticReplacement result;
    auto& mesh = result.mesh;
    mesh.textures = original.textures;
    mesh.header_flags = original.header_flags;
    mesh.declared_bounds = original.declared_bounds;
    mesh.declared_bounds_w = original.declared_bounds_w;
    mesh.bounds = original.bounds;

    const auto& nodes = array_member(doc.json, "nodes");
    const auto& meshes = array_member(doc.json, "meshes");
    const auto& materials = array_member(doc.json, "materials");
    if (!array_member(doc.json, "skins").array.empty())
        fail("the model is skinned (a level model is static: remove the armature)");
    for (std::size_t n = 0; n < nodes.array.size(); ++n)
        if (nodes.array[n].find("skin")) fail("node " + std::to_string(n) + " is skinned (a level model is static)");

    // The default scene's nodes (else every root).
    const auto parents = gltf::node_parents(doc.json);
    std::set<std::size_t> roots;
    const auto& scenes = array_member(doc.json, "scenes");
    if (!scenes.array.empty()) {
        const std::size_t scene = doc.json.find("scene") ? gltf::index_of(doc.json, "scene", "document") : 0;
        if (scene >= scenes.array.size()) fail("scene " + std::to_string(scene) + " is missing");
        for (const auto& v : array_member(scenes.array[scene], "nodes").array)
            if (v.type == Json::Type::number && v.number >= 0) roots.insert(static_cast<std::size_t>(v.number));
    } else {
        for (std::size_t n = 0; n < nodes.array.size(); ++n)
            if (parents[n] < 0) roots.insert(n);
    }
    const auto worlds = gltf::node_worlds(doc.json);

    // The original's surfaces with animated materials, in order: each makes an animator (00376ad0).
    std::vector<std::uint32_t> animated_sequence;
    for (const auto& s : original.surfaces)
        if (animated_material(original, s.material)) animated_sequence.push_back(s.material);
    std::map<long, std::uint32_t> mesh_material_of;   // glTF material (-1: none) -> mesh material
    std::map<std::uint32_t, std::uint32_t> animated_of;   // animated original material -> its one mesh material
    std::set<std::uint32_t> images_ignored;
    const auto mesh_material = [&](const Json& primitive, const std::string& what) -> std::uint32_t {
        long gm = -1;
        if (primitive.find("material")) {
            const auto m = gltf::index_of(primitive, "material", what);
            if (m >= materials.array.size()) fail(what + "'s material is missing");
            gm = static_cast<long>(m);
        }
        if (const auto found = mesh_material_of.find(gm); found != mesh_material_of.end()) return found->second;
        std::uint32_t inherit = first_drawn_material(original);
        if (inherit >= original.materials.size()) inherit = 0;
        std::optional<mods::RgbaImage> image;
        const Json* material = gm >= 0 ? &materials.array[static_cast<std::size_t>(gm)] : nullptr;
        if (material) {
            const auto* name = material->find("name");
            const long nn = name && name->type == Json::Type::string ? parse_material_name(name->string) : -1;
            if (nn >= 0 && static_cast<std::size_t>(nn) < original.materials.size()) {
                const auto m = static_cast<std::uint32_t>(nn);
                if (animated_material(original, m) &&
                    std::find(animated_sequence.begin(), animated_sequence.end(), m) == animated_sequence.end())
                    fail(name->string + " names an animated disc material the disc's model does not draw");
                inherit = m;
            }
        }
        if (animated_material(original, inherit)) {
            // One mesh material per animated original material, without a glTF image: its animator moves the disc's
            // texture (UV scroll) or steps its frames.
            const auto* pbr = material ? material->find("pbrMetallicRoughness") : nullptr;
            if (pbr && pbr->find("baseColorTexture")) images_ignored.insert(inherit);
            auto found = animated_of.find(inherit);
            if (found == animated_of.end()) {
                found = animated_of.emplace(inherit, static_cast<std::uint32_t>(mesh.materials.size())).first;
                mesh.materials.push_back(original.materials[inherit]);
                result.images.emplace_back();
                result.inherited.push_back(inherit);
            }
            mesh_material_of.emplace(gm, found->second);
            return found->second;
        }
        if (material) {
            const auto* pbr = material->find("pbrMetallicRoughness");
            if (const auto* texture_info = pbr ? pbr->find("baseColorTexture") : nullptr) {
                const auto& textures = array_member(doc.json, "textures");
                const auto t = gltf::index_of(*texture_info, "index", "baseColorTexture");
                if (t >= textures.array.size()) fail("texture " + std::to_string(t) + " is missing");
                image = mods::decode_image(gltf::image_bytes(doc, gltf::index_of(textures.array[t], "source", "texture")));
                if (image->width > mods::image_size_limit || image->height > mods::image_size_limit)
                    fail("a texture is larger than " + std::to_string(mods::image_size_limit) + " pixels a side");
                if (const auto* factor = pbr->find("baseColorFactor"); factor && factor->array.size() == 4) {
                    float f[4];
                    bool tint = false;
                    for (int k = 0; k < 4; ++k) {
                        f[k] = std::clamp(static_cast<float>(factor->array[static_cast<std::size_t>(k)].number), 0.0f, 1.0f);
                        tint |= f[k] != 1.0f;
                    }
                    if (tint)
                        for (std::size_t p = 0; p < image->rgba.size(); ++p)
                            image->rgba[p] = static_cast<std::uint8_t>(std::lround(image->rgba[p] * f[p % 4]));
                }
            }
        }
        const auto index = static_cast<std::uint32_t>(mesh.materials.size());
        mesh.materials.push_back(original.materials[inherit]);
        result.images.push_back(std::move(image));
        result.inherited.push_back(inherit);
        mesh_material_of.emplace(gm, index);
        return index;
    };

    std::vector<bool> needs_normal;
    std::size_t triangles = 0;
    for (std::size_t n = 0; n < nodes.array.size(); ++n) {
        const auto& node = nodes.array[n];
        if (!node.find("mesh")) continue;
        std::size_t root = n;
        for (std::size_t guard = 0; parents[root] >= 0 && guard <= nodes.array.size(); ++guard) root = static_cast<std::size_t>(parents[root]);
        if (!roots.count(root)) continue;
        const auto mesh_index = gltf::index_of(node, "mesh", "node");
        if (mesh_index >= meshes.array.size()) fail("node " + std::to_string(n) + "'s mesh is missing");
        const auto& world = worlds[n];
        float det = 1;
        const auto normal_world = normal_matrix(world, det);
        const bool mirrored = det < 0;
        for (const auto& primitive : array_member(meshes.array[mesh_index], "primitives").array) {
            const auto what = "mesh " + std::to_string(mesh_index);
            const int mode = primitive.find("mode") ? static_cast<int>(gltf::index_of(primitive, "mode", what)) : 4;
            if (mode != 4 && mode != 5 && mode != 6) fail(what + " has primitive mode " + std::to_string(mode) + " (triangles expected)");
            const auto* attributes = primitive.find("attributes");
            if (!attributes || !attributes->find("POSITION")) fail(what + " has no POSITION");
            std::size_t c = 0;
            const auto positions = gltf::read_floats(doc, gltf::index_of(*attributes, "POSITION", what), c);
            if (c != 3) fail(what + " POSITION is not VEC3");
            const std::size_t count = positions.size() / 3;
            if (mesh.vertices.size() + count > static_vertex_limit) fail("more than " + std::to_string(static_vertex_limit) + " vertices");
            std::vector<float> normals, uvs, colours;
            std::size_t colour_components = 0;
            if (attributes->find("NORMAL")) {
                normals = gltf::read_floats(doc, gltf::index_of(*attributes, "NORMAL", what), c);
                if (c != 3 || normals.size() != positions.size()) fail(what + " NORMAL does not match POSITION");
            }
            if (attributes->find("TEXCOORD_0")) {
                uvs = gltf::read_floats(doc, gltf::index_of(*attributes, "TEXCOORD_0", what), c);
                if (c != 2 || uvs.size() != count * 2) fail(what + " TEXCOORD_0 does not match POSITION");
            }
            if (attributes->find("COLOR_0")) {
                colours = gltf::read_floats(doc, gltf::index_of(*attributes, "COLOR_0", what), colour_components);
                if ((colour_components != 3 && colour_components != 4) || colours.size() != count * colour_components)
                    fail(what + " COLOR_0 does not match POSITION");
            }
            const auto material = mesh_material(primitive, what);
            const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
            for (std::size_t v = 0; v < count; ++v) {
                MeshVertex out;
                const auto p = from_gltf(transform_point(world, {positions[v * 3], positions[v * 3 + 1], positions[v * 3 + 2]}));
                for (const float f : p)
                    if (!std::isfinite(f)) fail(what + " vertex " + std::to_string(v) + " is not finite");
                out.position = {p[0], p[1], p[2]};
                if (!normals.empty()) {
                    auto nv = transform_vector(normal_world, {normals[v * 3], normals[v * 3 + 1], normals[v * 3 + 2]});
                    if (mirrored) nv = {-nv[0], -nv[1], -nv[2]};
                    const auto g = from_gltf(normalized(nv, {0, 1, 0}));
                    out.normal = {g[0], g[1], g[2]};
                }
                if (!uvs.empty()) out.uv = {uvs[v * 2], uvs[v * 2 + 1]};
                // COLOR_0 0.5 is the game's 0x80 (1.0, the colour unchanged); absent: 0x80; at most byte 0xff.
                for (std::size_t k = 0; k < 3; ++k)
                    out.color[k] = colours.empty() ? 1.0f : std::min(2.0f * std::clamp(colours[v * colour_components + k], 0.0f, 1.0f), 255.0f / 128.0f);
                out.color[3] = 1.0f;
                mesh.vertices.push_back(out);
                needs_normal.push_back(normals.empty());
            }
            std::vector<std::uint32_t> indices;
            if (primitive.find("indices")) {
                indices = gltf::read_uints(doc, gltf::index_of(primitive, "indices", what), c);
                if (c != 1) fail(what + " indices are not SCALAR");
            } else {
                indices.resize(count);
                for (std::size_t i = 0; i < count; ++i) indices[i] = static_cast<std::uint32_t>(i);
            }
            for (const auto i : indices)
                if (i >= count) fail(what + " index " + std::to_string(i) + " outside its " + std::to_string(count) + " vertices");
            const auto first_index = static_cast<std::uint32_t>(mesh.indices.size());
            const auto add = [&](std::uint32_t a, std::uint32_t b, std::uint32_t d) {
                if (a == b || b == d || a == d) return;
                if (mirrored) std::swap(b, d);
                mesh.indices.insert(mesh.indices.end(), {base + a, base + b, base + d});
            };
            if (mode == 4) {
                if (indices.size() % 3) fail(what + " index count is not a multiple of 3");
                for (std::size_t i = 0; i + 2 < indices.size(); i += 3) add(indices[i], indices[i + 1], indices[i + 2]);
            } else if (mode == 5) {
                for (std::size_t i = 2; i < indices.size(); ++i)
                    if (i % 2) add(indices[i - 1], indices[i - 2], indices[i]);
                    else add(indices[i - 2], indices[i - 1], indices[i]);
            } else {
                for (std::size_t i = 2; i < indices.size(); ++i) add(indices[0], indices[i - 1], indices[i]);
            }
            const auto index_count = static_cast<std::uint32_t>(mesh.indices.size()) - first_index;
            triangles += index_count / 3;
            if (triangles > static_triangle_limit) fail("more than " + std::to_string(static_triangle_limit) + " triangles");
            if (index_count) mesh.surfaces.push_back({material, first_index, index_count});
        }
    }
    // The animated surfaces laid out as the original's (one per original animated surface, in order, the first of each
    // material holding its triangles), then the others in glTF order.
    if (!animated_sequence.empty()) {
        for (const auto m : animated_sequence)
            if (!animated_of.count(m)) {
                animated_of.emplace(m, static_cast<std::uint32_t>(mesh.materials.size()));
                mesh.materials.push_back(original.materials[m]);
                result.images.emplace_back();
                result.inherited.push_back(m);
            }
        std::set<std::uint32_t> animated_mesh_materials;
        for (const auto& [m, mm] : animated_of) animated_mesh_materials.insert(mm);
        std::vector<std::uint32_t> indices;
        std::vector<MeshSurface> surfaces;
        const auto append = [&](const MeshSurface& s) {
            indices.insert(indices.end(), mesh.indices.begin() + s.first_index, mesh.indices.begin() + s.first_index + s.index_count);
        };
        std::set<std::uint32_t> placed;
        for (const auto m : animated_sequence) {
            const auto mm = animated_of.at(m);
            const auto first = static_cast<std::uint32_t>(indices.size());
            if (placed.insert(mm).second)
                for (const auto& s : mesh.surfaces)
                    if (s.material == mm) append(s);
            surfaces.push_back({mm, first, static_cast<std::uint32_t>(indices.size()) - first});
        }
        for (const auto& s : mesh.surfaces)
            if (!animated_mesh_materials.count(s.material)) {
                const auto first = static_cast<std::uint32_t>(indices.size());
                append(s);
                surfaces.push_back({s.material, first, s.index_count});
            }
        mesh.indices = std::move(indices);
        mesh.surfaces = std::move(surfaces);
        result.animated_surfaces = animated_sequence.size();
    }
    for (const auto m : images_ignored)
        result.notes.push_back(material_name(m) + " is animated: its glTF image is ignored, the disc's texture drawn (retexture it under textures\\)");
    if (std::find(needs_normal.begin(), needs_normal.end(), true) != needs_normal.end()) {
        std::vector<Vec> sum(mesh.vertices.size(), Vec{0, 0, 0});
        for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
            const auto& a = mesh.vertices[mesh.indices[t]].position;
            const auto& b = mesh.vertices[mesh.indices[t + 1]].position;
            const auto& d = mesh.vertices[mesh.indices[t + 2]].position;
            const Vec u{b.x - a.x, b.y - a.y, b.z - a.z}, w{d.x - a.x, d.y - a.y, d.z - a.z};
            const Vec n{u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0]};
            for (std::size_t k = 0; k < 3; ++k)
                for (int e = 0; e < 3; ++e) sum[mesh.indices[t + k]][static_cast<std::size_t>(e)] += n[static_cast<std::size_t>(e)];
        }
        for (std::size_t v = 0; v < mesh.vertices.size(); ++v)
            if (needs_normal[v]) {
                const auto n = normalized(sum[v], {0, 0, 1});
                mesh.vertices[v].normal = {n[0], n[1], n[2]};
            }
    }
    // Batches of up to 64 vertices with their bounds (the terrain point-light pass reaches a batch by its sphere).
    for (std::uint32_t first = 0; first < mesh.vertices.size(); first += static_batch_vertices) {
        MeshBatch batch;
        batch.first_vertex = first;
        batch.vertex_count = std::min<std::uint32_t>(static_batch_vertices, static_cast<std::uint32_t>(mesh.vertices.size()) - first);
        batch.header_minimum = {1e30f, 1e30f, 1e30f, 1};
        batch.header_maximum = {-1e30f, -1e30f, -1e30f, 1};
        for (std::uint32_t v = first; v < first + batch.vertex_count; ++v) {
            const auto& p = mesh.vertices[v].position;
            const float q[3]{p.x, p.y, p.z};
            for (std::size_t k = 0; k < 3; ++k) {
                batch.header_minimum[k] = std::min(batch.header_minimum[k], q[k]);
                batch.header_maximum[k] = std::max(batch.header_maximum[k], q[k]);
            }
        }
        mesh.batch_spans.push_back(batch);
    }
    mesh.batches = static_cast<std::uint32_t>(mesh.batch_spans.size());
    const auto& lo = original.declared_bounds.minimum;
    const auto& hi = original.declared_bounds.maximum;
    for (const auto& v : mesh.vertices) {
        const auto& p = v.position;
        result.overshoot = std::max({result.overshoot, lo.x - p.x, p.x - hi.x, lo.y - p.y, p.y - hi.y, lo.z - p.z, p.z - hi.z});
    }
    mesh.color_patches = remap_color_patches(original, mesh, &result.patched_vertices);
    return result;
}

StaticReplacement load_static_replacement(const std::filesystem::path& file, const Mesh& original) {
    return load_static_replacement(gltf::read_document(file), original);
}

ExportedModel export_static_model(const Mesh& mesh, std::span<const mods::RgbaImage* const> images, const std::string& stem) {
    gltf::Builder b;
    b.json["asset"]["generator"] = "omega_mod_tool export-model (Syphon Filter: The Omega Strain port, checkpoint 132md2)";
    const auto bytes = [](const auto& v) {
        return std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(v.data()), v.size() * sizeof(v[0]));
    };
    ExportedModel result;
    auto primitives = Json::make_array(), materials = Json::make_array(), textures = Json::make_array(), image_list = Json::make_array();
    std::map<std::uint32_t, std::size_t> texture_of_slot;   // mesh texture slot -> glTF texture
    for (std::uint32_t m = 0; m < mesh.materials.size(); ++m) {
        std::vector<std::uint32_t> remap(mesh.vertices.size(), UINT32_MAX), order, indices;
        for (const auto& s : mesh.surfaces) {
            if (s.material != m) continue;
            if (static_cast<std::size_t>(s.first_index) + s.index_count > mesh.indices.size()) fail("a surface reaches past the indices");
            for (std::uint32_t t = 0; t + 2 < s.index_count; t += 3) {
                const std::uint32_t tri[3]{mesh.indices[s.first_index + t], mesh.indices[s.first_index + t + 1], mesh.indices[s.first_index + t + 2]};
                if (tri[0] == tri[1] || tri[1] == tri[2] || tri[0] == tri[2]) continue;
                for (const auto i : tri) {
                    if (i >= mesh.vertices.size()) fail("an index is outside the vertices");
                    if (remap[i] == UINT32_MAX) {
                        remap[i] = static_cast<std::uint32_t>(order.size());
                        order.push_back(i);
                    }
                    indices.push_back(remap[i]);
                }
            }
        }
        if (indices.empty()) continue;
        std::vector<float> positions, normals, uvs, colours;
        std::vector<float> minimum{1e30f, 1e30f, 1e30f}, maximum{-1e30f, -1e30f, -1e30f};
        for (const auto i : order) {
            const auto& v = mesh.vertices[i];
            const auto gp = to_gltf({v.position.x, v.position.y, v.position.z});
            const auto gn = to_gltf(normalized({v.normal.x, v.normal.y, v.normal.z}, {0, 0, 1}));
            positions.insert(positions.end(), gp.begin(), gp.end());
            normals.insert(normals.end(), gn.begin(), gn.end());
            uvs.insert(uvs.end(), v.uv.begin(), v.uv.end());
            for (std::size_t k = 0; k < 3; ++k) colours.push_back(std::clamp(v.color[k] * 0.5f, 0.0f, 1.0f));
            for (std::size_t k = 0; k < 3; ++k) {
                minimum[k] = std::min(minimum[k], gp[k]);
                maximum[k] = std::max(maximum[k], gp[k]);
            }
        }
        const auto n = order.size();
        auto attributes = Json::make_object();
        attributes["POSITION"] = b.accessor(bytes(positions), 5126, n, "VEC3", 34962, &minimum, &maximum);
        attributes["NORMAL"] = b.accessor(bytes(normals), 5126, n, "VEC3", 34962);
        attributes["TEXCOORD_0"] = b.accessor(bytes(uvs), 5126, n, "VEC2", 34962);
        attributes["COLOR_0"] = b.accessor(bytes(colours), 5126, n, "VEC3", 34962);
        auto primitive = Json::make_object();
        primitive["attributes"] = std::move(attributes);
        primitive["indices"] = b.accessor(bytes(indices), 5125, indices.size(), "SCALAR", 34963);
        primitive["material"] = materials.array.size();
        primitive["mode"] = 4;
        primitives.push(std::move(primitive));

        const auto& source = mesh.materials[m];
        auto material = Json::make_object();
        material["name"] = material_name(m);
        auto pbr = Json::make_object();
        const auto slot = source.texture;
        const mods::RgbaImage* image = slot < images.size() ? images[slot] : nullptr;
        if (image) {
            auto found = texture_of_slot.find(slot);
            if (found == texture_of_slot.end()) {
                const auto image_name = stem + "." + std::to_string(slot) + ".png";
                auto entry = Json::make_object();
                entry["uri"] = image_name;
                image_list.push(std::move(entry));
                auto texture = Json::make_object();
                texture["sampler"] = 0;
                texture["source"] = image_list.array.size() - 1;
                textures.push(std::move(texture));
                result.images.emplace_back(image_name, mods::encode_png(*image));
                found = texture_of_slot.emplace(slot, textures.array.size() - 1).first;
            }
            auto info = Json::make_object();
            info["index"] = found->second;
            pbr["baseColorTexture"] = std::move(info);
        }
        pbr["metallicFactor"] = 0;
        pbr["roughnessFactor"] = 1;
        material["pbrMetallicRoughness"] = std::move(pbr);
        bool cutout = false;
        if (image)
            for (std::size_t p = 3; p < image->rgba.size(); p += 4)
                if (image->rgba[p] < 255) {
                    cutout = true;
                    break;
                }
        if (cutout) material["alphaMode"] = "MASK";
        material["doubleSided"] = true;
        auto extras = Json::make_object();
        if (slot < mesh.textures.size()) extras["texture"] = mesh.textures[slot];
        extras["shader"] = static_cast<std::size_t>(source.shader);
        extras["flags"] = static_cast<std::size_t>(source.flags);
        material["extras"] = std::move(extras);
        materials.push(std::move(material));
    }
    auto nodes = Json::make_array();
    auto node = Json::make_object();
    node["name"] = stem;
    if (!primitives.array.empty()) {
        auto meshes = Json::make_array();
        auto mesh_json = Json::make_object();
        mesh_json["name"] = stem;
        mesh_json["primitives"] = std::move(primitives);
        meshes.push(std::move(mesh_json));
        b.json["meshes"] = std::move(meshes);
        b.json["materials"] = std::move(materials);
        node["mesh"] = 0;
    }
    if (!image_list.array.empty()) {
        auto samplers = Json::make_array();
        auto sampler = Json::make_object();
        sampler["magFilter"] = 9729;
        sampler["minFilter"] = 9987;
        samplers.push(std::move(sampler));
        b.json["samplers"] = std::move(samplers);
        b.json["textures"] = std::move(textures);
        b.json["images"] = std::move(image_list);
    }
    nodes.push(std::move(node));
    b.json["nodes"] = std::move(nodes);
    auto scenes = Json::make_array();
    auto scene = Json::make_object();
    auto scene_nodes = Json::make_array();
    scene_nodes.push(Json(0));
    scene["nodes"] = std::move(scene_nodes);
    scenes.push(std::move(scene));
    b.json["scenes"] = std::move(scenes);
    b.json["scene"] = 0;
    result.gltf = b.finish(stem + ".bin");
    result.bin = std::move(b.bin);
    return result;
}

}  // namespace omega::gfx
