#include "character_model.hpp"
#include "mesh_simplify.hpp"

#include "skeleton_pose.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <stdexcept>

namespace omega::gfx {
namespace {

using gltf::Json;
using gltf::Matrix;
using Vec = std::array<float, 3>;

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error(message); }

Vec transform_point(const Matrix& m, const Vec& p) {
    return {m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12], m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13],
            m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14]};
}
Vec transform_vector(const Matrix& m, const Vec& v) {
    return {m[0] * v[0] + m[4] * v[1] + m[8] * v[2], m[1] * v[0] + m[5] * v[1] + m[9] * v[2], m[2] * v[0] + m[6] * v[1] + m[10] * v[2]};
}
Vec normalized(Vec v, Vec fallback) {
    const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!(length > 1e-12f) || !std::isfinite(length)) return fallback;
    return {v[0] / length, v[1] / length, v[2] / length};
}
Vec translation(const Matrix& m) { return {m[12], m[13], m[14]}; }

std::string bone_name(std::size_t bone) {
    std::string name = "bone_";
    if (bone < 10) name.push_back('0');
    return name + std::to_string(bone);
}
// bone_NN or bone_NN_<suffix>: NN, else -1.
int parse_bone_name(const std::string& name) {
    if (!name.starts_with("bone_")) return -1;
    std::size_t at = 5;
    int value = 0;
    while (at < name.size() && name[at] >= '0' && name[at] <= '9' && at < 9) value = value * 10 + (name[at++] - '0');
    if (at == 5 || (at < name.size() && name[at] != '_')) return -1;
    return value;
}

const Json& array_member(const Json& object, const char* key) {
    static const Json empty = Json::make_array();
    const auto* v = object.find(key);
    return v && v->type == Json::Type::array ? *v : empty;
}

std::string key_text(const std::array<std::uint8_t, 16>& key) {
    std::size_t n = 0;
    while (n < key.size() && key[n]) ++n;
    return std::string(key.begin(), key.begin() + static_cast<std::ptrdiff_t>(n));
}

constexpr std::size_t morph_name_limit = 1024;

}  // namespace

void skin_replacement_vertex(const ReplacementModel& model, std::span<const SkinPaletteBone> palette,
                             const std::vector<std::array<float, 3>>* offsets, std::size_t v, std::array<float, 3>& p,
                             std::array<float, 3>& q) {
    const auto n = model.vertices.size();
    const auto& in = model.vertices[v];
    Vec position = in.position, normal = in.normal;
    if (offsets) {
        const auto& dp = (*offsets)[v];
        const auto& dn = (*offsets)[n + v];
        for (std::size_t c = 0; c < 3; ++c) {
            position[c] = dp[c] + position[c];
            normal[c] = dn[c] + normal[c];
        }
    }
    p = {0, 0, 0};
    q = {0, 0, 0};
    for (std::size_t k = 0; k < in.count; ++k) {
        if (in.bones[k] >= palette.size()) fail("model replacement: bone " + std::to_string(in.bones[k]) + " outside the palette");
        const auto& bone = palette[in.bones[k]];
        const float w = in.weights[k];
        const auto tp = transform_point(bone.position, position);
        const auto tn = transform_vector(bone.normal, normal);
        for (std::size_t c = 0; c < 3; ++c) {
            p[c] += w * tp[c];
            q[c] += w * tn[c];
        }
    }
}

namespace {

// Skins `model` with optional per-vertex bind-space offsets (offsets[v] position, offsets[n + v] normal).
void skin_range(const ReplacementModel& model, std::span<const SkinPaletteBone> palette,
                const std::vector<std::array<float, 3>>* offsets, SkinnedMeshGeometry& output, std::size_t begin, std::size_t end) {
    for (std::size_t v = begin; v < end; ++v) {
        auto& out = output.vertices[v];
        Vec p, q;
        skin_replacement_vertex(model, palette, offsets, v, p, q);
        out.position = {p[0], p[1], p[2], 1};
        out.normal = {q[0], q[1], q[2], 0};
        out.uv = model.vertices[v].uv;
    }
}

void skin_with_offsets(const ReplacementModel& model, std::span<const SkinPaletteBone> palette,
                       const std::vector<std::array<float, 3>>* offsets, SkinnedMeshGeometry& output) {
    const auto n = model.vertices.size();
    output.vertices.resize(n);
    output.triangles.assign(model.triangles.begin(), model.triangles.end());
    output.triangle_chunks.assign(model.triangles.size(), 0);
    skin_range(model, palette, offsets, output, 0, n);
}

}  // namespace

std::array<float, 3> to_gltf(std::array<float, 3> v) { return {-v[0], v[2], v[1]}; }

float bind_tolerance(const SkeletonDefinition& skeleton) {
    const auto bind = skeleton_bind_pose(skeleton);
    float extent = 0;
    for (const auto& bone : bind) {
        const auto t = translation(bone.model);
        extent = std::max(extent, std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]));
    }
    return std::max(1e-3f, extent * 0.005f);
}

std::vector<std::size_t> validate_skeleton(const gltf::Document& doc, const SkeletonDefinition& skeleton) {
    const auto& skins = array_member(doc.json, "skins");
    if (skins.array.empty()) fail("no skin (the model must be rigged to the character's skeleton)");
    if (skins.array.size() > 1) fail(std::to_string(skins.array.size()) + " skins (one is expected)");
    const auto& joints = array_member(skins.array[0], "joints");
    const auto& nodes = array_member(doc.json, "nodes");
    const auto count = skeleton.bones.size();
    if (joints.array.size() != count)
        fail("bone count " + std::to_string(joints.array.size()) + ", the skeleton has " + std::to_string(count));
    std::vector<std::size_t> bone_node(count, SIZE_MAX);
    std::vector<int> joint_bone(nodes.array.size(), -1);
    for (std::size_t j = 0; j < joints.array.size(); ++j) {
        const auto& v = joints.array[j];
        if (v.type != Json::Type::number || v.number < 0 || v.number >= static_cast<double>(nodes.array.size()))
            fail("joint " + std::to_string(j) + " is not a node");
        const auto node = static_cast<std::size_t>(v.number);
        const auto* name = nodes.array[node].find("name");
        const std::string text = name && name->type == Json::Type::string ? name->string : std::string();
        const int bone = parse_bone_name(text);
        if (bone < 0 || static_cast<std::size_t>(bone) >= count)
            fail("joint " + std::to_string(j) + " is named \"" + text + "\" (bone_00 .. " + bone_name(count - 1) + " expected)");
        if (bone_node[static_cast<std::size_t>(bone)] != SIZE_MAX || joint_bone[node] >= 0)
            fail("two joints are " + bone_name(static_cast<std::size_t>(bone)));
        bone_node[static_cast<std::size_t>(bone)] = node;
        joint_bone[node] = bone;
    }
    const auto parents = gltf::node_parents(doc.json);
    for (std::size_t b = 0; b < count; ++b) {
        const int parent_node = parents[bone_node[b]];
        const int parent_bone = parent_node >= 0 ? joint_bone[static_cast<std::size_t>(parent_node)] : -1;
        const int expected = skeleton.bones[b].parent;
        if (parent_bone != expected)
            fail("hierarchy: " + bone_name(b) + "'s parent is " + (parent_bone < 0 ? std::string("not a bone") : bone_name(static_cast<std::size_t>(parent_bone))) +
                 ", the skeleton's is " + (expected < 0 ? std::string("none (root)") : bone_name(static_cast<std::size_t>(expected))));
    }
    const auto worlds = gltf::node_worlds(doc.json);
    const auto bind = skeleton_bind_pose(skeleton);
    const float tolerance = bind_tolerance(skeleton);
    // Parents before children, so a moved bone is named rather than the descendants it carries along.
    std::vector<std::size_t> depth(count, 0), order(count);
    for (std::size_t b = 0; b < count; ++b)
        for (int p = skeleton.bones[b].parent; p >= 0 && depth[b] <= count; p = skeleton.bones[static_cast<std::size_t>(p)].parent) ++depth[b];
    for (std::size_t b = 0; b < count; ++b) order[b] = b;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t x, std::size_t y) { return depth[x] < depth[y]; });
    for (const auto b : order) {
        const auto rest = from_gltf(translation(worlds[bone_node[b]]));
        const auto expected = translation(bind[b].model);
        const float d = std::sqrt((rest[0] - expected[0]) * (rest[0] - expected[0]) + (rest[1] - expected[1]) * (rest[1] - expected[1]) +
                                  (rest[2] - expected[2]) * (rest[2] - expected[2]));
        if (!(d <= tolerance)) {
            char text[160];
            std::snprintf(text, sizeof text, "bind pose: %s rests %.4g from the skeleton's (tolerance %.3g)", bone_name(b).c_str(),
                          static_cast<double>(d), static_cast<double>(tolerance));
            fail(text);
        }
    }
    return bone_node;
}

ReplacementModel load_replacement(const gltf::Document& doc, const SkeletonDefinition& skeleton) {
    ReplacementModel model;
    model.bone_count = skeleton.bones.size();
    const auto& nodes = array_member(doc.json, "nodes");
    const auto& meshes = array_member(doc.json, "meshes");
    bool any_mesh = false;
    for (const auto& node : nodes.array) any_mesh |= node.find("mesh") != nullptr;
    if (!any_mesh) return model;   // a hidden part

    const auto bone_node = validate_skeleton(doc, skeleton);
    const auto& skin = array_member(doc.json, "skins").array[0];
    const auto& joints = array_member(skin, "joints");
    std::vector<std::uint8_t> joint_to_bone(joints.array.size());
    for (std::size_t b = 0; b < bone_node.size(); ++b)
        for (std::size_t j = 0; j < joints.array.size(); ++j)
            if (static_cast<std::size_t>(joints.array[j].number) == bone_node[b]) joint_to_bone[j] = static_cast<std::uint8_t>(b);
    std::vector<Matrix> inverse_bind(joints.array.size(), gltf::identity());
    if (skin.find("inverseBindMatrices")) {
        std::size_t components = 0;
        const auto values = gltf::read_floats(doc, gltf::index_of(skin, "inverseBindMatrices", "skin"), components);
        if (components != 16 || values.size() != 16 * joints.array.size()) fail("inverseBindMatrices do not match the joints");
        for (std::size_t j = 0; j < joints.array.size(); ++j) std::copy_n(values.begin() + static_cast<std::ptrdiff_t>(16 * j), 16, inverse_bind[j].begin());
    }
    const auto worlds = gltf::node_worlds(doc.json);
    std::vector<Matrix> rest(joints.array.size());
    for (std::size_t j = 0; j < joints.array.size(); ++j)
        rest[j] = gltf::multiply(worlds[static_cast<std::size_t>(joints.array[j].number)], inverse_bind[j]);

    std::vector<bool> needs_normal;
    for (std::size_t n = 0; n < nodes.array.size(); ++n) {
        const auto& node = nodes.array[n];
        if (!node.find("mesh")) continue;
        const auto mesh_index = gltf::index_of(node, "mesh", "node");
        if (mesh_index >= meshes.array.size()) fail("node " + std::to_string(n) + "'s mesh is missing");
        if (!node.find("skin")) fail("mesh node " + std::to_string(n) + " is not skinned to the skeleton");
        if (gltf::index_of(node, "skin", "node") != 0) fail("mesh node " + std::to_string(n) + " uses another skin");
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
            if (model.vertices.size() + count > replacement_vertex_limit)
                fail("more than " + std::to_string(replacement_vertex_limit) + " vertices");
            std::vector<float> normals, uvs;
            if (attributes->find("NORMAL")) {
                normals = gltf::read_floats(doc, gltf::index_of(*attributes, "NORMAL", what), c);
                if (c != 3 || normals.size() != positions.size()) fail(what + " NORMAL does not match POSITION");
            }
            if (attributes->find("TEXCOORD_0")) {
                uvs = gltf::read_floats(doc, gltf::index_of(*attributes, "TEXCOORD_0", what), c);
                if (c != 2 || uvs.size() != count * 2) fail(what + " TEXCOORD_0 does not match POSITION");
            }
            std::vector<std::pair<std::vector<std::uint32_t>, std::vector<float>>> sets;
            for (int s = 0;; ++s) {
                const auto jn = "JOINTS_" + std::to_string(s), wn = "WEIGHTS_" + std::to_string(s);
                if (!attributes->find(jn) || !attributes->find(wn)) break;
                auto j = gltf::read_uints(doc, gltf::index_of(*attributes, jn, what), c);
                if (c != 4 || j.size() != count * 4) fail(what + " " + jn + " does not match POSITION");
                auto w = gltf::read_floats(doc, gltf::index_of(*attributes, wn, what), c);
                if (c != 4 || w.size() != count * 4) fail(what + " " + wn + " does not match POSITION");
                sets.emplace_back(std::move(j), std::move(w));
            }
            if (sets.empty()) fail(what + " has no JOINTS_0 / WEIGHTS_0");
            // 132md3: morph targets (POSITION / NORMAL offsets), named by the mesh's extras.targetNames.
            struct TargetData { std::size_t morph; std::vector<float> position, normal; };
            std::vector<TargetData> target_data;
            if (const auto& targets = array_member(primitive, "targets"); !targets.array.empty()) {
                const auto* extras = meshes.array[mesh_index].find("extras");
                const auto* names = extras ? extras->find("targetNames") : nullptr;
                if (!names || names->type != Json::Type::array || names->array.size() != targets.array.size())
                    fail(what + " has " + std::to_string(targets.array.size()) + " morph targets without as many names (mesh.extras.targetNames)");
                std::vector<std::string> seen;
                for (std::size_t k = 0; k < targets.array.size(); ++k) {
                    const auto& name = names->array[k];
                    if (name.type != Json::Type::string || name.string.empty()) fail(what + " morph target " + std::to_string(k) + " has no name");
                    if (std::find(seen.begin(), seen.end(), name.string) != seen.end())
                        fail(what + " has two morph targets named \"" + name.string + "\"");
                    seen.push_back(name.string);
                    TargetData data{SIZE_MAX, {}, {}};
                    for (std::size_t m = 0; m < model.morphs.size(); ++m)
                        if (model.morphs[m].name == name.string) data.morph = m;
                    if (data.morph == SIZE_MAX) {
                        if (model.morphs.size() == morph_name_limit) fail("more than " + std::to_string(morph_name_limit) + " morph targets");
                        data.morph = model.morphs.size();
                        model.morphs.push_back({name.string, {}, {}, {}});
                    }
                    const auto& target = targets.array[k];
                    for (const auto* attribute : {"POSITION", "NORMAL"}) {
                        if (!target.find(attribute)) continue;
                        auto values = gltf::read_floats(doc, gltf::index_of(target, attribute, what), c);
                        if (c != 3 || values.size() != positions.size())
                            fail(what + " morph target \"" + name.string + "\" " + attribute + " does not match POSITION");
                        (std::string_view(attribute) == "POSITION" ? data.position : data.normal) = std::move(values);
                    }
                    target_data.push_back(std::move(data));
                }
            }
            const auto base = static_cast<std::uint32_t>(model.vertices.size());
            for (std::size_t v = 0; v < count; ++v) {
                std::vector<std::pair<float, std::uint32_t>> influences;
                for (const auto& [js, ws] : sets)
                    for (std::size_t k = 0; k < 4; ++k) {
                        const float w = ws[v * 4 + k];
                        if (!(w > 0)) continue;
                        const auto joint = js[v * 4 + k];
                        if (joint >= joints.array.size()) fail(what + " vertex " + std::to_string(v) + " uses joint " + std::to_string(joint));
                        influences.emplace_back(w, joint);
                    }
                std::stable_sort(influences.begin(), influences.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
                if (influences.size() > replacement_influence_limit) influences.resize(replacement_influence_limit);
                float total = 0;
                for (const auto& i : influences) total += i.first;
                if (!(total > 0)) fail(what + " vertex " + std::to_string(v) + " has no skin weight");
                ReplacementVertex out;
                out.count = static_cast<std::uint8_t>(influences.size());
                Matrix blend{};
                for (std::size_t k = 0; k < influences.size(); ++k) {
                    const float w = influences[k].first / total;
                    out.weights[k] = w;
                    out.bones[k] = joint_to_bone[influences[k].second];
                    const auto& m = rest[influences[k].second];
                    for (std::size_t e = 0; e < 16; ++e) blend[e] += w * m[e];
                }
                const Vec p{positions[v * 3], positions[v * 3 + 1], positions[v * 3 + 2]};
                out.position = from_gltf(transform_point(blend, p));
                if (!normals.empty())
                    out.normal = from_gltf(normalized(transform_vector(blend, {normals[v * 3], normals[v * 3 + 1], normals[v * 3 + 2]}), {0, 1, 0}));
                if (!uvs.empty()) out.uv = {uvs[v * 2], uvs[v * 2 + 1]};
                for (const auto& data : target_data) {
                    const auto offset = [&](const std::vector<float>& values) {
                        if (values.empty()) return Vec{0, 0, 0};
                        return from_gltf(transform_vector(blend, {values[v * 3], values[v * 3 + 1], values[v * 3 + 2]}));
                    };
                    const auto dp = offset(data.position), dn = offset(data.normal);
                    if (dp == Vec{0, 0, 0} && dn == Vec{0, 0, 0}) continue;
                    auto& morph = model.morphs[data.morph];
                    morph.vertices.push_back(static_cast<std::uint32_t>(model.vertices.size()));
                    morph.position.push_back(dp);
                    morph.normal.push_back(dn);
                }
                model.vertices.push_back(out);
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
            const auto add = [&](std::uint32_t a, std::uint32_t b, std::uint32_t d) {
                if (a != b && b != d && a != d) model.triangles.push_back({base + a, base + b, base + d});
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
            if (!model.image && primitive.find("material")) {
                {
                    const auto& materials = array_member(doc.json, "materials");
                    const auto m = gltf::index_of(primitive, "material", what);
                    if (m >= materials.array.size()) fail(what + "'s material is missing");
                    const auto* pbr = materials.array[m].find("pbrMetallicRoughness");
                    const auto* texture_info = pbr ? pbr->find("baseColorTexture") : nullptr;
                    if (texture_info) {
                        const auto& textures = array_member(doc.json, "textures");
                        const auto t = gltf::index_of(*texture_info, "index", "baseColorTexture");
                        if (t >= textures.array.size()) fail("texture " + std::to_string(t) + " is missing");
                        const auto image = mods::decode_image(gltf::image_bytes(doc, gltf::index_of(textures.array[t], "source", "texture")));
                        if (image.width > mods::image_size_limit || image.height > mods::image_size_limit)
                            fail("the texture is larger than " + std::to_string(mods::image_size_limit) + " pixels a side");
                        model.image = image;
                        if (const auto* factor = pbr->find("baseColorFactor"); factor && factor->array.size() == 4) {
                            float f[4];
                            bool tint = false;
                            for (int k = 0; k < 4; ++k) {
                                f[k] = std::clamp(static_cast<float>(factor->array[static_cast<std::size_t>(k)].number), 0.0f, 1.0f);
                                tint |= f[k] != 1.0f;
                            }
                            if (tint)
                                for (std::size_t p = 0; p < model.image->rgba.size(); ++p)
                                    model.image->rgba[p] = static_cast<std::uint8_t>(std::lround(model.image->rgba[p] * f[p % 4]));
                        }
                    }
                }
            }
        }
    }
    if (std::find(needs_normal.begin(), needs_normal.end(), true) != needs_normal.end()) {
        std::vector<Vec> sum(model.vertices.size(), Vec{0, 0, 0});
        for (const auto& t : model.triangles) {
            const auto& a = model.vertices[t[0]].position;
            const auto& b = model.vertices[t[1]].position;
            const auto& d = model.vertices[t[2]].position;
            const Vec u{b[0] - a[0], b[1] - a[1], b[2] - a[2]}, w{d[0] - a[0], d[1] - a[1], d[2] - a[2]};
            const Vec n{u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0]};
            for (const auto i : t)
                for (int k = 0; k < 3; ++k) sum[i][static_cast<std::size_t>(k)] += n[static_cast<std::size_t>(k)];
        }
        for (std::size_t v = 0; v < model.vertices.size(); ++v)
            if (needs_normal[v]) model.vertices[v].normal = normalized(sum[v], {0, 0, 1});
    }
    model.bone_boxes = replacement_bone_boxes(model);
    return model;
}

ReplacementModel load_replacement(const std::filesystem::path& file, const SkeletonDefinition& skeleton) {
    return load_replacement(gltf::read_document(file), skeleton);
}

void skin_replacement_into(const ReplacementModel& model, std::span<const SkinPaletteBone> palette, SkinnedMeshGeometry& output) {
    skin_with_offsets(model, palette, nullptr, output);
}

void skin_replacement_draw_into(const ReplacementDraw& draw, SkinnedMeshGeometry& output) {
    skin_with_offsets(*draw.model, draw.palette, draw.offsets, output);
}

std::vector<GpuSkinVertex> gpu_skin_vertices(const ReplacementModel& model) {
    std::vector<GpuSkinVertex> out(model.vertices.size());
    for (std::size_t v = 0; v < out.size(); ++v) {
        const auto& in = model.vertices[v];
        auto& o = out[v];
        o.position = in.position;
        o.normal = in.normal;
        o.uv = in.uv;
        for (std::size_t k = 0; k < in.count && k < replacement_influence_limit; ++k) {
            o.bones[k] = in.bones[k];
            o.weights[k] = in.weights[k];
        }
    }
    return out;
}

void gpu_palette_rows(std::span<const SkinPaletteBone> palette, std::vector<std::array<float, 4>>& rows) {
    if (palette.size() > gpu_palette_limit) throw std::runtime_error("GPU skin palette past the limit");
    rows.assign(gpu_palette_rows_per_bone * gpu_palette_limit, {0, 0, 0, 0});
    for (std::size_t b = 0; b < palette.size(); ++b) {
        const auto& m = palette[b].position;
        const auto& n = palette[b].normal;
        auto* p = rows.data() + 3 * b;
        auto* q = rows.data() + 3 * (gpu_palette_limit + b);
        for (std::size_t c = 0; c < 3; ++c) {
            p[c] = {m[c], m[4 + c], m[8 + c], m[12 + c]};
            q[c] = {n[c], n[4 + c], n[8 + c], 0};
        }
    }
}

std::size_t replacement_bones_used(const ReplacementModel& model) {
    if (!model.bone_boxes.empty()) return model.bone_boxes.size();
    std::size_t used = 0;
    for (const auto& v : model.vertices)
        for (std::size_t k = 0; k < v.count; ++k) used = std::max<std::size_t>(used, std::size_t{v.bones[k]} + 1);
    return used;
}

std::vector<std::array<float, 6>> replacement_bone_boxes(const ReplacementModel& model) {
    std::size_t used = 0;
    for (const auto& v : model.vertices)
        for (std::size_t k = 0; k < v.count; ++k) used = std::max<std::size_t>(used, std::size_t{v.bones[k]} + 1);
    constexpr float big = std::numeric_limits<float>::max();
    std::vector<std::array<float, 6>> boxes(used, {big, big, big, -big, -big, -big});
    for (const auto& v : model.vertices)
        for (std::size_t k = 0; k < v.count; ++k) {
            auto& b = boxes[v.bones[k]];
            for (std::size_t c = 0; c < 3; ++c) {
                b[c] = std::min(b[c], v.position[c]);
                b[3 + c] = std::max(b[3 + c], v.position[c]);
            }
        }
    return boxes;
}

SkinMatrix ghost_camera_matrix(const SkinMatrix& a, const std::array<std::array<float, 4>, 4>& m, float t) {
    SkinMatrix out{};
    const auto camera = [&](float x, float y, float z, float* o) {
        o[0] = (x * m[0][0] + y * m[0][1] + z * m[0][2]) / t;
        o[1] = x * m[1][0] + y * m[1][1] + z * m[1][2];
        o[2] = (x * m[2][0] + y * m[2][1] + z * m[2][2]) / t;
    };
    for (std::size_t c = 0; c < 3; ++c) camera(a[4 * c], a[4 * c + 1], a[4 * c + 2], &out[4 * c]);
    camera(a[12] - m[3][0], a[13] - m[3][1], a[14] - m[3][2], &out[12]);
    out[15] = 1;
    return out;
}

SkinMatrix ghost_world_matrix(const SkinMatrix& a, const std::array<std::array<float, 4>, 4>& m, float t) {
    SkinMatrix out{};
    const auto world = [&](float x, float y, float z, bool point, float* o) {
        const float cx = x * t, cz = z * t;
        for (std::size_t j = 0; j < 3; ++j) o[j] = (point ? m[3][j] : 0.0F) + cx * m[0][j] + y * m[1][j] + cz * m[2][j];
    };
    for (std::size_t c = 0; c < 3; ++c) world(a[4 * c], a[4 * c + 1], a[4 * c + 2], false, &out[4 * c]);
    world(a[12], a[13], a[14], true, &out[12]);
    out[15] = 1;
    return out;
}

std::size_t build_replacement_lods(ReplacementModel& model) {
    model.lods.clear();
    if (!model.morphs.empty() || model.triangles.size() < replacement_lod_triangles) return 0;
    std::vector<std::array<float, 3>> positions(model.vertices.size());
    for (std::size_t v = 0; v < positions.size(); ++v) positions[v] = model.vertices[v].position;
    for (auto& level : simplify_levels(positions, model.triangles)) model.lods.push_back({std::move(level.triangles), level.error});
    return model.lods.size();
}

std::size_t replacement_lod(const ReplacementModel& model, float pixels_per_unit, float max_pixels) {
    std::size_t chosen = 0;
    for (std::size_t k = 0; k < model.lods.size(); ++k) {
        if (!(model.lods[k].error * pixels_per_unit <= max_pixels)) break;
        chosen = k + 1;
    }
    return chosen;
}

bool gpu_skinnable(const ReplacementModel& model, std::size_t palette_size) {
    if (model.triangles.empty() || !model.morphs.empty() || palette_size > gpu_palette_limit) return false;
    return replacement_bones_used(model) <= palette_size;
}

bool replacement_bounds(const ReplacementModel& model, std::span<const SkinPaletteBone> palette, std::array<float, 3>& minimum,
                        std::array<float, 3>& maximum) {
    const auto computed = model.bone_boxes.empty() ? replacement_bone_boxes(model) : std::vector<std::array<float, 6>>{};
    const auto& boxes = model.bone_boxes.empty() ? computed : model.bone_boxes;
    bool any = false;
    for (std::size_t b = 0; b < boxes.size(); ++b) {
        const auto& box = boxes[b];
        if (box[0] > box[3]) continue;
        if (b >= palette.size()) fail("model replacement: bone " + std::to_string(b) + " outside the palette");
        for (std::size_t corner = 0; corner < 8; ++corner) {
            const Vec x{box[(corner & 1) ? 3 : 0], box[(corner & 2) ? 4 : 1], box[(corner & 4) ? 5 : 2]};
            const auto p = transform_point(palette[b].position, x);
            for (std::size_t c = 0; c < 3; ++c) {
                minimum[c] = any ? std::min(minimum[c], p[c]) : p[c];
                maximum[c] = any ? std::max(maximum[c], p[c]) : p[c];
            }
            any = true;
        }
    }
    return any;
}

std::string shape_target_name(const ShapeNames& names, std::size_t index) {
    if (index >= names.size()) fail("shape " + std::to_string(index) + " outside its table of " + std::to_string(names.size()));
    const auto text = key_text(names[index]);
    for (std::size_t i = 0; i < index; ++i)
        if (key_text(names[i]) == text) return text + "@" + std::to_string(index);
    return text;
}

std::vector<std::int16_t> bind_morphs(const ReplacementModel& model, const ShapeNames& names) {
    std::vector<std::int16_t> shapes(names.size(), -1);
    for (std::size_t m = 0; m < model.morphs.size(); ++m) {
        const auto& name = model.morphs[m].name;
        std::size_t index = SIZE_MAX;
        const auto at = name.rfind('@');
        if (at != std::string::npos && at + 1 < name.size() &&
            std::all_of(name.begin() + static_cast<std::ptrdiff_t>(at + 1), name.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            const auto n = name.size() - at - 1 > 6 ? SIZE_MAX : std::stoul(name.substr(at + 1));
            if (n < names.size() && key_text(names[n]) == name.substr(0, at)) index = n;
        } else {
            for (std::size_t i = 0; i < names.size() && index == SIZE_MAX; ++i)
                if (key_text(names[i]) == name) index = i;
        }
        if (index == SIZE_MAX)
            fail("morph target \"" + name + "\" is no shape of the part's table (" + std::to_string(names.size()) + " shapes)");
        if (shapes[index] >= 0)
            fail("morph targets \"" + model.morphs[static_cast<std::size_t>(shapes[index])].name + "\" and \"" + name + "\" are one shape");
        shapes[index] = static_cast<std::int16_t>(m);
    }
    return shapes;
}

void skin_morphed_replacement_into(const ReplacementModel& model, std::span<const SkinPaletteBone> palette,
                                   std::span<const std::int16_t> shapes, std::span<const SkinMorphTarget> targets,
                                   std::vector<std::array<float, 3>>& scratch, SkinnedMeshGeometry& output) {
    skin_with_offsets(model, palette, replacement_morph_offsets(model, shapes, targets, scratch), output);
}

const std::vector<std::array<float, 3>>* replacement_morph_offsets(const ReplacementModel& model, std::span<const std::int16_t> shapes,
                                                                   std::span<const SkinMorphTarget> targets,
                                                                   std::vector<std::array<float, 3>>& scratch) {
    const auto bound = [&](const SkinMorphTarget& t) { return t.shape_index < shapes.size() && shapes[t.shape_index] >= 0; };
    if (std::none_of(targets.begin(), targets.end(), [&](const SkinMorphTarget& t) { return t.weight != 0 && bound(t); }))
        return nullptr;
    const auto n = model.vertices.size();
    scratch.assign(2 * n, Vec{0, 0, 0});
    // Slot order, as the original accumulates (sum of weighted offsets, then added to the vertex).
    for (const auto& t : targets) {
        if (!bound(t)) continue;
        const auto& morph = model.morphs[static_cast<std::size_t>(shapes[t.shape_index])];
        for (std::size_t k = 0; k < morph.vertices.size(); ++k) {
            auto& dp = scratch[morph.vertices[k]];
            auto& dn = scratch[n + morph.vertices[k]];
            for (std::size_t c = 0; c < 3; ++c) {
                dp[c] = dp[c] + morph.position[k][c] * t.weight;
                dn[c] = dn[c] + morph.normal[k][c] * t.weight;
            }
        }
    }
    return &scratch;
}

ExportedModel export_model(const SkeletonDefinition& skeleton, std::span<const ExportPart> parts, const std::string& stem) {
    const auto bind = skeleton_bind_pose(skeleton);
    const auto count = skeleton.bones.size();
    if (count > 255) fail("the skeleton has more than 255 bones");
    gltf::Builder b;
    b.json["asset"]["generator"] = "omega_mod_tool export-model (Syphon Filter: The Omega Strain port, checkpoint 132md2)";
    auto nodes = Json::make_array();   // b.json gains members below (a reference into it would dangle)
    {
        auto armature = Json::make_object();
        armature["name"] = "Armature";
        auto children = Json::make_array();
        for (std::size_t i = 0; i < count; ++i)
            if (skeleton.bones[i].parent < 0) children.push(Json(i + 1));
        armature["children"] = std::move(children);
        nodes.push(std::move(armature));
    }
    std::vector<float> ibm;
    for (std::size_t i = 0; i < count; ++i) {
        auto node = Json::make_object();
        node["name"] = bone_name(i);
        auto local = translation(bind[i].model);
        if (skeleton.bones[i].parent >= 0) {
            const auto parent = translation(bind[static_cast<std::size_t>(skeleton.bones[i].parent)].model);
            local = {local[0] - parent[0], local[1] - parent[1], local[2] - parent[2]};
        }
        const auto t = to_gltf(local);
        auto tj = Json::make_array();
        for (const float f : t) tj.push(Json(static_cast<double>(f)));
        node["translation"] = std::move(tj);
        auto children = Json::make_array();
        for (std::size_t k = 0; k < count; ++k)
            if (skeleton.bones[k].parent == static_cast<int>(i)) children.push(Json(k + 1));
        if (!children.array.empty()) node["children"] = std::move(children);
        nodes.push(std::move(node));
        auto m = gltf::identity();
        const auto world = to_gltf(translation(bind[i].model));
        m[12] = -world[0];
        m[13] = -world[1];
        m[14] = -world[2];
        ibm.insert(ibm.end(), m.begin(), m.end());
    }
    const auto ibm_accessor = b.accessor({reinterpret_cast<const std::uint8_t*>(ibm.data()), ibm.size() * 4}, 5126, count, "MAT4");
    {
        auto skins = Json::make_array();
        auto skin = Json::make_object();
        skin["name"] = "skeleton";
        skin["inverseBindMatrices"] = ibm_accessor;
        skin["skeleton"] = 0;
        auto joints = Json::make_array();
        for (std::size_t i = 0; i < count; ++i) joints.push(Json(i + 1));
        skin["joints"] = std::move(joints);
        skins.push(std::move(skin));
        b.json["skins"] = std::move(skins);
    }
    auto scene_nodes = Json::make_array();
    scene_nodes.push(Json(0));
    auto meshes = Json::make_array(), materials = Json::make_array(), textures = Json::make_array(), images = Json::make_array();
    ExportedModel result;
    std::vector<SkinPaletteBone> identity_palette(count);
    for (auto& bone : identity_palette) bone.position = bone.normal = gltf::identity();
    for (std::size_t p = 0; p < parts.size(); ++p) {
        const auto& part = parts[p];
        if (!part.mesh) continue;
        const auto& mesh = *part.mesh;
        const auto geometry = skin_skinned_mesh(mesh, identity_palette, SkinMorphContext{0});
        struct Source { Vec position, normal; std::array<float, 2> uv; std::vector<std::pair<std::uint32_t, float>> influences; std::size_t chunk = 0, index = 0; };
        std::vector<Source> sources;
        sources.reserve(geometry.vertices.size());
        for (const auto& chunk : mesh.chunks) {
            const bool flare = chunk.microprogram_entry == 12 || chunk.microprogram_entry == 14;
            const auto first = sources.size();
            Vec offset{0, 0, 0};
            if (part.bone_local && !chunk.vertices.empty()) {
                if (!flare && chunk.groups.size() != 1) fail(part.name + ": a bone-local part's chunk has " + std::to_string(chunk.groups.size()) + " bone groups");
                const auto bone = chunk.groups.front().bone;
                if (bone >= count) fail(part.name + ": bone " + std::to_string(bone) + " outside the skeleton");
                offset = translation(bind[bone].model);
            }
            for (const auto& v : chunk.vertices) {
                Source s;
                s.position = {v.position.x + offset[0], v.position.y + offset[1], v.position.z + offset[2]};
                s.normal = flare ? Vec{0, 0, 1} : normalized({v.normal.x, v.normal.y, v.normal.z}, {0, 0, 1});
                s.uv = v.uv;
                s.chunk = static_cast<std::size_t>(&chunk - mesh.chunks.data());
                s.index = sources.size() - first;
                sources.push_back(std::move(s));
            }
            if (flare || chunk.groups.size() == 1) {
                for (std::size_t k = first; k < sources.size(); ++k) sources[k].influences.emplace_back(chunk.groups.front().bone, 1.0f);
            } else {
                for (const auto& g : chunk.groups)
                    for (std::size_t k = 0; k < g.weights.size(); ++k)
                        if (g.weights[k] > 0 && first + g.first_vertex + k < sources.size())
                            sources[first + g.first_vertex + k].influences.emplace_back(g.bone, g.weights[k]);
            }
        }
        if (sources.size() != geometry.vertices.size()) fail(part.name + ": vertex count differs from its skinning");
        // Only the vertices the triangles use, in first-use order.
        std::vector<std::uint32_t> remap(sources.size(), UINT32_MAX), order;
        std::vector<std::uint32_t> indices;
        for (const auto& t : geometry.triangles)
            for (const auto i : t) {
                if (remap[i] == UINT32_MAX) {
                    remap[i] = static_cast<std::uint32_t>(order.size());
                    order.push_back(i);
                }
                indices.push_back(remap[i]);
            }
        std::size_t most = 1;
        for (const auto i : order) most = std::max(most, sources[i].influences.size());
        if (most > 8) fail(part.name + ": a vertex has more than 8 influences");
        const std::size_t sets = most > 4 ? 2 : 1;
        std::vector<float> positions, normals, uvs, weights[2];
        std::vector<std::uint8_t> joint_bytes[2];
        std::vector<float> minimum{1e30f, 1e30f, 1e30f}, maximum{-1e30f, -1e30f, -1e30f};
        for (const auto i : order) {
            auto& s = sources[i];
            const auto gp = to_gltf(s.position), gn = to_gltf(s.normal);
            positions.insert(positions.end(), gp.begin(), gp.end());
            normals.insert(normals.end(), gn.begin(), gn.end());
            uvs.insert(uvs.end(), s.uv.begin(), s.uv.end());
            for (int k = 0; k < 3; ++k) {
                minimum[static_cast<std::size_t>(k)] = std::min(minimum[static_cast<std::size_t>(k)], gp[static_cast<std::size_t>(k)]);
                maximum[static_cast<std::size_t>(k)] = std::max(maximum[static_cast<std::size_t>(k)], gp[static_cast<std::size_t>(k)]);
            }
            // Repeated bones (overlapping groups) merge; weights normalised to sum 1.
            std::map<std::uint32_t, float> merged;
            for (const auto& [bone, w] : s.influences) merged[bone] += w;
            std::vector<std::pair<std::uint32_t, float>> list(merged.begin(), merged.end());
            std::stable_sort(list.begin(), list.end(), [](const auto& a, const auto& c) { return a.second > c.second; });
            float total = 0;
            for (const auto& l : list) total += l.second;
            if (!(total > 0)) list = {{s.influences.empty() ? 0u : s.influences.front().first, 1.0f}}, total = 1;
            for (const auto& l : list)
                if (l.first >= count) fail(part.name + ": an influence names bone " + std::to_string(l.first));
            for (std::size_t set = 0; set < sets; ++set)
                for (std::size_t k = 0; k < 4; ++k) {
                    const auto at = set * 4 + k;
                    joint_bytes[set].push_back(at < list.size() ? static_cast<std::uint8_t>(list[at].first) : 0);
                    weights[set].push_back(at < list.size() ? list[at].second / total : 0.0f);
                }
        }
        const auto bytes = [](const auto& v) {
            return std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(v.data()), v.size() * sizeof(v[0]));
        };
        auto attributes = Json::make_object();
        const auto n = order.size();
        attributes["POSITION"] = b.accessor(bytes(positions), 5126, n, "VEC3", 34962, &minimum, &maximum);
        attributes["NORMAL"] = b.accessor(bytes(normals), 5126, n, "VEC3", 34962);
        attributes["TEXCOORD_0"] = b.accessor(bytes(uvs), 5126, n, "VEC2", 34962);
        for (std::size_t set = 0; set < sets; ++set) {
            attributes["JOINTS_" + std::to_string(set)] = b.accessor(bytes(joint_bytes[set]), 5121, n, "VEC4", 34962);
            attributes["WEIGHTS_" + std::to_string(set)] = b.accessor(bytes(weights[set]), 5126, n, "VEC4", 34962);
        }
        auto primitive = Json::make_object();
        primitive["attributes"] = std::move(attributes);
        primitive["indices"] = b.accessor(bytes(indices), 5125, indices.size(), "SCALAR", 34963);
        primitive["material"] = materials.array.size();
        primitive["mode"] = 4;
        // 132md3: the morph part's shapes as morph targets (deform_skinned_mesh_into's offsets at weight 1: the raw
        // bytes' (b - 128) / 256 on X, Y, Z, and a face's W on the normal's Z; an absent sparse block moves nothing).
        auto target_names = Json::make_array(), default_weights = Json::make_array();
        if (part.morph && part.shape_names) {
            const auto& morph = *part.morph;
            const bool face = morph.kind == MorphKind::face;
            if (morph.shapes.size() != part.shape_names->size())
                fail(part.name + ": " + std::to_string(morph.shapes.size()) + " shapes, its table names " + std::to_string(part.shape_names->size()));
            if (morph.chunk_count != mesh.chunks.size()) fail(part.name + ": the shapes' chunk count differs from the mesh's");
            auto targets = Json::make_array();
            for (std::size_t s = 0; s < morph.shapes.size(); ++s) {
                const auto& shape = morph.shapes[s];
                if (shape.chunks.size() != mesh.chunks.size()) fail(part.name + ": shape " + std::to_string(s) + "'s chunk count");
                std::vector<float> dp, dn, low{1e30f, 1e30f, 1e30f}, high{-1e30f, -1e30f, -1e30f};
                for (const auto i : order) {
                    const auto& source = sources[i];
                    const auto& block = shape.chunks[source.chunk];
                    const auto stride = face ? 4u : 3u;
                    Vec d{0, 0, 0}, normal_offset{0, 0, 0};
                    if (block) {
                        if ((source.index + 1) * stride > block->size()) fail(part.name + ": shape " + std::to_string(s) + "'s block is short");
                        const auto at = [&](unsigned component) {
                            return static_cast<float>(int((*block)[source.index * stride + component]) - 128) / 256.0f;
                        };
                        d = {at(0), at(1), at(2)};
                        if (face) {
                            const auto& raw = mesh.chunks[source.chunk].vertices[source.index].normal;
                            const Vec before{raw.x, raw.y, raw.z}, after{raw.x, raw.y, at(3) + raw.z};
                            const auto n0 = normalized(before, {0, 0, 1}), n1 = normalized(after, {0, 0, 1});
                            normal_offset = {n1[0] - n0[0], n1[1] - n0[1], n1[2] - n0[2]};
                        }
                    }
                    const auto gd = to_gltf(d), gn = to_gltf(normal_offset);
                    dp.insert(dp.end(), gd.begin(), gd.end());
                    dn.insert(dn.end(), gn.begin(), gn.end());
                    for (std::size_t k = 0; k < 3; ++k) {
                        low[k] = std::min(low[k], gd[k]);
                        high[k] = std::max(high[k], gd[k]);
                    }
                }
                auto target = Json::make_object();
                target["POSITION"] = b.accessor(bytes(dp), 5126, n, "VEC3", 34962, &low, &high);
                if (face) target["NORMAL"] = b.accessor(bytes(dn), 5126, n, "VEC3", 34962);
                targets.push(std::move(target));
                target_names.push(Json(shape_target_name(*part.shape_names, s)));
                default_weights.push(Json(0));
            }
            primitive["targets"] = std::move(targets);
        }
        auto material = Json::make_object();
        material["name"] = part.name;
        auto pbr = Json::make_object();
        if (part.image) {
            const auto image_name = stem + (parts.size() == 1 ? std::string() : "." + std::to_string(p)) + ".png";
            auto image = Json::make_object();
            image["uri"] = image_name;
            images.push(std::move(image));
            auto texture = Json::make_object();
            texture["sampler"] = 0;
            texture["source"] = images.array.size() - 1;
            textures.push(std::move(texture));
            auto info = Json::make_object();
            info["index"] = textures.array.size() - 1;
            pbr["baseColorTexture"] = std::move(info);
            result.images.emplace_back(image_name, mods::encode_png(*part.image));
        }
        pbr["metallicFactor"] = 0;
        pbr["roughnessFactor"] = 1;
        material["pbrMetallicRoughness"] = std::move(pbr);
        material["doubleSided"] = true;
        materials.push(std::move(material));
        auto primitives = Json::make_array();
        primitives.push(std::move(primitive));
        auto mesh_json = Json::make_object();
        mesh_json["name"] = part.name;
        mesh_json["primitives"] = std::move(primitives);
        if (!target_names.array.empty()) {
            mesh_json["weights"] = std::move(default_weights);
            auto extras = Json::make_object();
            extras["targetNames"] = std::move(target_names);
            mesh_json["extras"] = std::move(extras);
        }
        meshes.push(std::move(mesh_json));
        auto node = Json::make_object();
        node["name"] = part.name;
        node["mesh"] = meshes.array.size() - 1;
        node["skin"] = 0;
        nodes.push(std::move(node));
        scene_nodes.push(Json(nodes.array.size() - 1));
    }
    if (!meshes.array.empty()) b.json["meshes"] = std::move(meshes);
    if (!materials.array.empty()) b.json["materials"] = std::move(materials);
    if (!images.array.empty()) {
        auto samplers = Json::make_array();
        auto sampler = Json::make_object();
        sampler["magFilter"] = 9729;
        sampler["minFilter"] = 9987;
        samplers.push(std::move(sampler));
        b.json["samplers"] = std::move(samplers);
        b.json["textures"] = std::move(textures);
        b.json["images"] = std::move(images);
    }
    b.json["nodes"] = std::move(nodes);
    auto scenes = Json::make_array();
    auto scene = Json::make_object();
    scene["nodes"] = std::move(scene_nodes);
    scenes.push(std::move(scene));
    b.json["scenes"] = std::move(scenes);
    b.json["scene"] = 0;
    result.gltf = b.finish(stem + ".bin");
    result.bin = std::move(b.bin);
    return result;
}

void write_exported(const std::filesystem::path& folder, const std::string& stem, const ExportedModel& model) {
    std::filesystem::create_directories(folder);
    const auto write = [](const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out) fail("cannot write " + path.string());
    };
    write(folder / (stem + ".gltf"), {reinterpret_cast<const std::uint8_t*>(model.gltf.data()), model.gltf.size()});
    if (!model.bin.empty()) write(folder / (stem + ".bin"), model.bin);
    for (const auto& [name, bytes] : model.images) write(folder / name, bytes);
}

}  // namespace omega::gfx
