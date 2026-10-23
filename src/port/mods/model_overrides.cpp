// Port-only (checkpoint 132md2): see model_overrides.hpp.
#include "model_overrides.hpp"

#include "mod_set.hpp"
#include "skeleton_pose.hpp"
#include "texture_replacement.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <map>
#include <mutex>
#include <system_error>
#include <thread>
#include <tuple>

namespace omega::mods {
namespace {

struct Cache {
    std::mutex mutex;
    std::uint64_t generation = ~0ull;
    // (key, skeleton name) -> the loaded replacement (null: rejected, the original drawn).
    std::map<std::pair<std::string, std::string>, std::shared_ptr<const gfx::ReplacementModel>> models;
    // (model, uses_alpha, raw alpha) -> its GPU image.
    std::map<std::tuple<const gfx::ReplacementModel*, bool, bool>, std::shared_ptr<const Texture>> images;
    // Level model key -> its loaded replacement (null: rejected).
    std::map<std::string, std::shared_ptr<const gfx::StaticReplacement>> statics;
};
Cache& cache() {
    static Cache state;
    return state;
}
std::atomic<bool> build_lods{true};
void sync(Cache& c) {
    const auto now = generation();
    if (c.generation == now) return;
    c.generation = now;
    c.models.clear();
    c.images.clear();
    c.statics.clear();
}

// A newly loaded model waiting for its levels of detail (built after the batch's lookups, all at once).
struct Fresh {
    std::shared_ptr<gfx::ReplacementModel> model;
    std::string key;
};

// Builds the batch's levels of detail on one thread per model (the cache lock is held: nothing reads them yet).
void build_lods_for(std::vector<Fresh>& fresh) {
    struct Built {
        bool made = false;
        double ms = 0;
        std::exception_ptr error;
    };
    std::vector<Built> built(fresh.size());
    const auto build = [&](std::size_t i) {
        try {
            const auto start = std::chrono::steady_clock::now();
            built[i].made = gfx::build_replacement_lods(*fresh[i].model);
            built[i].ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        } catch (...) {
            built[i].error = std::current_exception();
        }
    };
    std::vector<std::thread> threads;
    try {
        for (std::size_t i = 1; i < fresh.size(); ++i) threads.emplace_back(build, i);
    } catch (const std::system_error&) {
    }
    if (!fresh.empty()) build(0);
    for (std::size_t i = threads.size() + 1; i < fresh.size(); ++i) build(i);  // threads that could not start
    for (auto& thread : threads) thread.join();
    for (std::size_t i = 0; i < fresh.size(); ++i) {
        auto& model = *fresh[i].model;
        if (built[i].error) {
            model.lods.clear();
            try {
                std::rethrow_exception(built[i].error);
            } catch (const std::exception& error) {
                note("models/" + fresh[i].key + ": no levels of detail (" + error.what() + ")");
            }
            continue;
        }
        if (!built[i].made) continue;
        std::string levels;
        for (const auto& lod : model.lods) levels += (levels.empty() ? "" : "/") + std::to_string(lod.triangles.size());
        char tail[96];
        std::snprintf(tail, sizeof tail, " triangles, largest error %.3g, %.0f ms)", model.lods.back().error, built[i].ms);
        note("models/" + fresh[i].key + ": " + std::to_string(model.lods.size()) + " levels of detail (" + levels + tail);
    }
    fresh.clear();
}

std::shared_ptr<const gfx::ReplacementModel> lookup(const ModSet& set, Cache& c, const SkeletonDefinition& skeleton,
                                                    const std::string& skeleton_name, const std::string& key,
                                                    std::vector<Fresh>& fresh, const std::string& tag = {}) {
    if (key.empty()) return nullptr;
    const auto source = set.model(key);
    if (!source) return nullptr;
    const auto slot = std::make_pair(key, skeleton_name + tag);
    if (const auto found = c.models.find(slot); found != c.models.end()) return found->second;
    std::shared_ptr<const gfx::ReplacementModel> model;
    try {
        auto loaded = std::make_shared<gfx::ReplacementModel>(gfx::load_replacement(source->path, skeleton));
        std::string what = "models/" + key + " <- " + source->mod->folder + " (" + skeleton_name + ", " +
                           std::to_string(loaded->vertices.size()) + " vertices, " +
                           std::to_string(loaded->triangles.size()) + " triangles";
        if (loaded->image)
            what += ", image " + std::to_string(loaded->image->width) + "x" + std::to_string(loaded->image->height);
        note(what + (loaded->vertices.empty() ? ", hidden)" : ")"));
        if (build_lods.load() && loaded->morphs.empty() && loaded->triangles.size() >= gfx::replacement_lod_triangles)
            fresh.push_back({loaded, key});
        model = std::move(loaded);
    } catch (const std::exception& error) {
        note("models/" + key + " from " + source->mod->folder + " rejected for " + skeleton_name + " (" + error.what() +
             "); the disc's model is drawn");
    }
    c.models.emplace(slot, model);
    return model;
}

}  // namespace

void set_model_lods(bool enabled) { build_lods.store(enabled); }
bool model_lods() { return build_lods.load(); }

std::string part_key(const std::vector<std::string>& archive, std::string_view member) {
    return member_key(asset_key(archive), member);
}

std::shared_ptr<const CharacterModelOverrides> character_model_overrides(const SkeletonDefinition& skeleton,
    const std::string& skeleton_name, std::span<const std::string> lod0_keys, std::span<const std::string> lod1_keys,
    std::span<const ShapeNames* const> shape_tables) {
    const auto* set = active();
    if (!set || !set->has_models()) return nullptr;
    auto& c = cache();
    std::lock_guard lock(c.mutex);
    sync(c);
    auto result = std::make_shared<CharacterModelOverrides>();
    bool any = false;
    std::vector<Fresh> fresh;
    for (std::size_t i = 0; i < lod0_keys.size(); ++i) {
        auto lod0 = lookup(*set, c, skeleton, skeleton_name, lod0_keys[i], fresh);
        std::vector<std::int16_t> shapes;
        const auto* table = i < shape_tables.size() ? shape_tables[i] : nullptr;
        if (lod0 && lod0->morphs.empty() && table && !lod0->vertices.empty())
            note("models/" + lod0_keys[i] + ": no morph targets; the part's " + std::to_string(table->size()) +
                 " shapes (speech, expressions, hand poses) do not move it (" + skeleton_name + ")");
        if (lod0 && !lod0->morphs.empty()) {
            const auto source = set->model(lod0_keys[i]);
            const std::string from = "models/" + lod0_keys[i] + " from " + (source ? source->mod->folder : std::string("?"));
            if (!table) {
                note(from + ": " + std::to_string(lod0->morphs.size()) + " morph targets not drawn (" + skeleton_name +
                     "'s part has no face / hand shapes)");
            } else {
                try {
                    shapes = gfx::bind_morphs(*lod0, *table);
                    note(from + ": " + std::to_string(lod0->morphs.size()) + " morph targets on " + std::to_string(table->size()) +
                         " shapes (" + skeleton_name + ")");
                } catch (const std::exception& error) {
                    note(from + " rejected for " + skeleton_name + " (" + error.what() + "); the disc's model is drawn");
                    lod0 = nullptr;
                }
            }
        }
        std::shared_ptr<const gfx::ReplacementModel> lod1;
        const std::string lod1_key = i < lod1_keys.size() ? lod1_keys[i] : std::string{};
        if (!lod1_key.empty()) {
            lod1 = lod1_key == lod0_keys[i] ? lod0 : lookup(*set, c, skeleton, skeleton_name, lod1_key, fresh);
            if (!lod1) lod1 = lod0;
        }
        any = any || lod0 || lod1;
        result->lod0.push_back(std::move(lod0));
        result->lod1.push_back(std::move(lod1));
        result->lod0_shapes.push_back(std::move(shapes));
        result->lod0_keys.push_back(lod0_keys[i]);
    }
    build_lods_for(fresh);
    if (!any) return nullptr;
    // Value-initialised (false) flags; libstdc++'s make_shared<T[]> copy-constructs its elements, which atomics forbid.
    result->shapes_logged = std::shared_ptr<std::atomic<bool>[]>(new std::atomic<bool>[lod0_keys.size()]());
    return result;
}

std::vector<const ShapeNames*> part_shape_tables(std::span<const AnimatedCharacterPart> parts, std::uint8_t table,
    const std::array<ShapeNames, 2>& faces, const std::array<std::array<ShapeNames, 2>, 2>& hands) {
    std::vector<const ShapeNames*> result;
    for (const auto& part : parts) {
        const ShapeNames* names = nullptr;
        if (table < 2 && part.morph) {
            if (part.binding == CharacterMorphBinding::face) names = &faces[table];
            else if (part.binding == CharacterMorphBinding::hand0) names = &hands[table][0];
            else if (part.binding == CharacterMorphBinding::hand1) names = &hands[table][1];
        }
        result.push_back(names);
    }
    return result;
}

void attach_model_images(const CharacterModelOverrides& models, std::vector<Texture>& images, std::vector<bool>& textured,
                         std::vector<std::optional<Texture>>& lod1_images) {
    auto& c = cache();
    std::lock_guard lock(c.mutex);
    const auto attach = [&](const std::shared_ptr<const gfx::ReplacementModel>& model, Texture& texture) {
        if (!model || !model->image) return false;
        const auto slot = std::make_tuple(model.get(), texture.uses_alpha, !texture.raw_alpha.empty());
        auto found = c.images.find(slot);
        if (found == c.images.end())
            found = c.images.emplace(slot, capped_replacement_texture(*model->image, texture)).first;
        texture.replacement = found->second;
        return true;
    };
    for (std::size_t i = 0; i < models.lod0.size() && i < images.size(); ++i)
        if (attach(models.lod0[i], images[i]) && i < textured.size()) textured[i] = true;
    for (std::size_t i = 0; i < models.lod1.size() && i < lod1_images.size(); ++i)
        if (lod1_images[i]) attach(models.lod1[i], *lod1_images[i]);
}

void substitute_models(const std::vector<std::shared_ptr<const gfx::ReplacementModel>>& models,
                       std::span<const SkinPaletteBone> palette, std::vector<AnimatedCharacterGeometry>& geometry) {
    for (std::size_t i = 0; i < models.size() && i < geometry.size(); ++i)
        if (models[i]) gfx::skin_replacement_into(*models[i], palette, geometry[i].mesh);
}

void replacement_draws(const CharacterModelOverrides& models, bool distant, const AnimatedCharacter& character,
                       std::vector<gfx::ReplacementDraw>& draws, std::vector<std::vector<std::array<float, 3>>>& scratch) {
    const auto& replaced = distant ? models.lod1 : models.lod0;
    const auto palette = character.committed_palette();
    draws.assign(replaced.size(), {});
    if (scratch.size() < replaced.size()) scratch.resize(replaced.size());
    const auto first_weights = [&](std::size_t i, std::span<const SkinMorphTarget> targets) {
        if (!models.shapes_logged || i >= models.lod0_keys.size() || models.shapes_logged[i].load(std::memory_order_relaxed))
            return;
        const auto& shapes = models.lod0_shapes[i];
        std::string list;
        for (const auto& t : targets) {
            if (t.weight == 0.0F || t.shape_index >= shapes.size() || shapes[t.shape_index] < 0) continue;
            char weight[32];
            std::snprintf(weight, sizeof weight, " %.3f", static_cast<double>(t.weight));
            list += (list.empty() ? "" : ", ") + replaced[i]->morphs[static_cast<std::size_t>(shapes[t.shape_index])].name + weight;
        }
        if (list.empty() || models.shapes_logged[i].exchange(true)) return;
        note("models/" + models.lod0_keys[i] + ": shapes drawn (first weighted: " + list + ")");
    };
    for (std::size_t i = 0; i < replaced.size(); ++i) {
        if (!replaced[i]) continue;
        auto& d = draws[i];
        d.model = replaced[i].get();
        d.palette = palette;
        const auto binding = i < character.parts().size() ? character.parts()[i].binding : CharacterMorphBinding::none;
        if (distant || i >= models.lod0_shapes.size() || models.lod0_shapes[i].empty() || binding == CharacterMorphBinding::none) {
        } else if (binding == CharacterMorphBinding::face) {
            const auto targets = morph_face_targets(character.morph());
            first_weights(i, targets);
            d.offsets = gfx::replacement_morph_offsets(*replaced[i], models.lod0_shapes[i], targets, scratch[i]);
        } else {
            const auto targets = morph_hand_targets(character.morph(), binding == CharacterMorphBinding::hand0 ? 0 : 1);
            first_weights(i, targets);
            d.offsets = gfx::replacement_morph_offsets(*replaced[i], models.lod0_shapes[i], targets, scratch[i]);
        }
    }
}

void substitute_models(const CharacterModelOverrides& models, bool distant, const AnimatedCharacter& character,
                       std::vector<AnimatedCharacterGeometry>& geometry) {
    thread_local std::vector<gfx::ReplacementDraw> draws;
    thread_local std::vector<std::vector<std::array<float, 3>>> scratch;
    replacement_draws(models, distant, character, draws, scratch);
    for (std::size_t i = 0; i < draws.size() && i < geometry.size(); ++i)
        if (draws[i].model) gfx::skin_replacement_draw_into(draws[i], geometry[i].mesh);
}

std::shared_ptr<const gfx::StaticReplacement> static_model_override(const std::string& key, const Mesh& original) {
    const auto* set = active();
    if (!set || !set->has_models() || key.empty()) return nullptr;
    const auto source = set->model(key);
    if (!source) return nullptr;
    auto& c = cache();
    std::lock_guard lock(c.mutex);
    sync(c);
    if (const auto found = c.statics.find(key); found != c.statics.end()) return found->second;
    std::shared_ptr<const gfx::StaticReplacement> model;
    try {
        auto loaded = std::make_shared<gfx::StaticReplacement>(gfx::load_static_replacement(source->path, original));
        std::size_t images = 0;
        for (const auto& image : loaded->images) images += image.has_value();
        std::string what = "models/" + key + " <- " + source->mod->folder + " (static, " +
                           std::to_string(loaded->mesh.vertices.size()) + " vertices, " +
                           std::to_string(loaded->mesh.indices.size() / 3) + " triangles, " +
                           std::to_string(loaded->mesh.materials.size()) + " materials";
        if (images) what += ", " + std::to_string(images) + " images";
        if (loaded->mesh.indices.empty()) what += ", hidden";
        if (!loaded->mesh.color_patches.empty())
            what += ", " + std::to_string(loaded->mesh.color_patches.size()) + " colour patch sets on " +
                    std::to_string(loaded->patched_vertices) + " vertices";
        if (loaded->animated_surfaces) what += ", " + std::to_string(loaded->animated_surfaces) + " animated surfaces kept";
        if (loaded->overshoot > 0) {
            char text[96];
            std::snprintf(text, sizeof text, ", extends %.4g outside the declared bounds", static_cast<double>(loaded->overshoot));
            what += text;
        }
        note(what + "); the collision stays the disc's");
        for (const auto& line : loaded->notes) note("models/" + key + ": " + line);
        model = std::move(loaded);
    } catch (const std::exception& error) {
        note("models/" + key + " from " + source->mod->folder + " rejected (" + error.what() + "); the disc's model is drawn");
    }
    c.statics.emplace(key, model);
    return model;
}

Texture model_image_texture(const RgbaImage& image, const Texture& base) {
    if (base.width == 0 || base.height == 0) {
        if (!texture_memory_cap()) return replacement_texture(image, base);
        return *capped_replacement_texture(image, base);
    }
    Texture texture = base;
    texture.animation.clear();
    texture.frames = 1;
    texture.replacement = capped_replacement_texture(image, base);
    return texture;
}

// ---- 132md3: gun models ----

std::string gun_model_name(const GunModel& model) {
    std::string name;
    for (std::size_t k = 0; k < 0x20 && model.defined[k] && model.bytes[k] != 0 && model.bytes[k] != '.'; ++k)
        name.push_back(static_cast<char>(model.bytes[k]));
    return upper(name);
}

SkeletonDefinition gun_skeleton(const GunModel& model) {
    SkeletonDefinition skeleton;
    skeleton.bones.resize(std::tuple_size_v<GunSlotMatrices>);
    for (std::size_t k = 1; k < skeleton.bones.size(); ++k) {
        const auto at = 0x150 + 32 * k;
        const auto parent = static_cast<std::int32_t>(model.word(at + 12));
        if (parent < 0 || static_cast<std::size_t>(parent) >= k)
            throw std::runtime_error("gun bone " + std::to_string(k) + " parent " + std::to_string(parent) + " not below it");
        skeleton.bones[k].parent = parent;
        skeleton.bones[k].local_offset = {model.real(at), model.real(at + 4), model.real(at + 8)};
    }
    return skeleton;
}

std::string gun_part_member(const GunModel& model, std::span<const std::uint8_t> hog, std::size_t part, std::int32_t lod) {
    if (part >= 8 || (lod != 0 && lod != 1)) return {};
    const auto rec = (lod == 0 ? 0x2c0u : 0x340u) + 16 * part;
    if (!model.all_defined(rec, 12) || model.word(rec) == 0 || model.bytes[rec + 7] == 0) return {};
    const std::size_t data = model.word(rec + 8);
    std::vector<HogMember> members;
    try {
        members = hog_members(hog);
    } catch (const std::exception&) {
        return {};
    }
    for (const auto& m : members)
        if (data >= m.offset && data < m.offset + m.size) return upper(m.name);
    return {};
}

std::string gun_part_key(std::string_view archive_key, std::string_view name, std::string_view member) {
    return member_key(member_key(archive_key, std::string(name) + ".HOG"), member);
}

std::int32_t gun_part_texture_id(const GunModel& model, std::size_t part, std::int32_t lod) {
    if (part >= 8 || (lod != 0 && lod != 1)) return -1;
    const auto at = 0x3c0 + 0x10 * static_cast<std::size_t>(lod) + 2 * part;
    if (!model.defined[at] || !model.defined[at + 1]) return -1;
    return static_cast<std::int16_t>(model.bytes[at] | (model.bytes[at + 1] << 8));
}

std::shared_ptr<const GunModelOverrides> gun_model_overrides(const GunModel& model, std::span<const std::uint8_t> hog,
                                                             std::string_view mission) {
    const auto* set = active();
    if (!set || !set->has_models()) return nullptr;
    const auto name = gun_model_name(model);
    if (name.empty() || !model.defined[0x40c]) return nullptr;
    const std::size_t parts = std::min<std::size_t>(model.bytes[0x40c], 8);
    std::vector<std::string> archives;
    if (!mission.empty()) archives.push_back("GAMEDATA/" + upper(std::string(mission)) + "/WPN.HOG");
    archives.push_back("GAMEDATA/COMMON/WPNHOG.HOG");
    auto result = std::make_shared<GunModelOverrides>();
    result->name = name;
    bool wanted = false;
    for (std::int32_t lod = 0; lod < 2; ++lod)
        for (std::size_t p = 0; p < parts; ++p) {
            const auto member = gun_part_member(model, hog, p, lod);
            if (member.empty()) continue;
            for (const auto& archive : archives)
                if (const auto key = gun_part_key(archive, name, member); set->model(key)) {
                    result->keys[static_cast<std::size_t>(lod)][p] = key;
                    wanted = true;
                    break;
                }
        }
    if (!wanted) return nullptr;
    SkeletonDefinition skeleton;
    std::vector<SkeletonBindBone> bind;
    try {
        skeleton = gun_skeleton(model);
        bind = skeleton_bind_pose(skeleton);
    } catch (const std::exception& error) {
        for (const auto& lod : result->keys)
            for (const auto& key : lod)
                if (!key.empty())
                    note("models/" + key + " rejected for " + name + ".HOG (the gun's bones are not readable: " + error.what() +
                         "); the disc's model is drawn");
        return nullptr;
    }
    // The cache tag tells apart two guns of one name with different bones (a mission's copy and COMMON's).
    std::string tag = "#";
    for (std::size_t k = 0; k < bind.size(); ++k) {
        const auto& m = bind[k].model;
        char text[96];
        std::snprintf(text, sizeof text, "%a,%a,%a;", static_cast<double>(m[12]), static_cast<double>(m[13]), static_cast<double>(m[14]));
        tag += text;
        if (k < result->bind.size()) result->bind[k] = {m[12], m[13], m[14]};
    }
    auto& c = cache();
    std::lock_guard lock(c.mutex);
    sync(c);
    bool any = false;
    std::vector<Fresh> fresh;
    for (std::size_t lod = 0; lod < 2; ++lod)
        for (std::size_t p = 0; p < parts; ++p) {
            auto& slot = result->parts[lod][p];
            slot = lookup(*set, c, skeleton, name + ".HOG", result->keys[lod][p], fresh, tag);
            if (!slot && lod == 1) slot = result->parts[0][p];
            any = any || slot;
        }
    build_lods_for(fresh);
    if (!any) return nullptr;
    return result;
}

namespace {
std::array<SkinPaletteBone, std::tuple_size_v<GunSlotMatrices>> gun_palette(const GunModelOverrides& gun,
                                                                             const GunSlotMatrices& matrices) {
    std::array<SkinPaletteBone, std::tuple_size_v<GunSlotMatrices>> palette{};
    for (std::size_t k = 0; k < matrices.size(); ++k) {
        const auto& m = matrices[k];
        const auto& b = gun.bind[k];
        SkinMatrix s{};
        for (std::size_t i = 0; i < 4; ++i)
            for (std::size_t j = 0; j < 4; ++j) s[4 * i + j] = m[i][j];
        for (std::size_t c = 0; c < 3; ++c) s[12 + c] -= (m[0][c] * b[0] + m[1][c] * b[1]) + m[2][c] * b[2];
        palette[k] = {s, s};
    }
    return palette;
}
}  // namespace

void skin_gun_replacement_into(const GunModelOverrides& gun, const gfx::ReplacementModel& model,
                               const GunSlotMatrices& matrices, SkinnedMeshGeometry& output) {
    gfx::skin_replacement_into(model, gun_palette(gun, matrices), output);
}

gfx::ReplacementDraw gun_replacement_draw(const GunModelOverrides& gun, const gfx::ReplacementModel& model,
                                          const GunSlotMatrices& matrices) {
    const auto palette = gun_palette(gun, matrices);
    return {&model, {palette.begin(), palette.end()}, nullptr};
}

std::vector<GuiVertex> gun_preview_vertices(const SkinnedMeshGeometry& geometry, const FrontendLightSet& lights, bool flare,
                                            std::int32_t c, std::int32_t alpha) {
    std::vector<std::array<std::int32_t, 4>> colour(geometry.vertices.size());
    for (std::size_t i = 0; i < colour.size(); ++i) {
        if (flare) {
            colour[i] = {c, c, c, alpha};
            continue;
        }
        const auto& n = geometry.vertices[i].normal;
        const auto lit = frontend_light_colour(lights, {n[0], n[1], n[2]});
        colour[i] = {lit[0], lit[1], lit[2], alpha};
    }
    std::vector<GuiVertex> out;
    out.reserve(3 * geometry.triangles.size());
    for (const auto& t : geometry.triangles)
        for (const auto i : t) {
            const auto& v = geometry.vertices.at(i);
            GuiVertex g;
            g.pos = {v.position[0], v.position[1], v.position[2], 1.0F};
            g.colour = colour[i];
            g.uv = {v.uv[0] + 2048.0F, v.uv[1] + 2048.0F, 1.0F, 0.0F};
            out.push_back(g);
        }
    return out;
}

const GunPreviewCache::Entry* GunPreviewCache::find(const gfx::ReplacementModel* model, const GunSlotMatrices& matrices,
                                                    bool flare, std::int32_t alpha) const {
    const auto it = entries_.find(model);
    if (it == entries_.end()) return nullptr;
    const Entry& e = it->second;
    if (e.flare != flare || e.alpha != alpha || std::memcmp(&e.matrices, &matrices, sizeof matrices) != 0) return nullptr;
    return &e;
}

const GunPreviewCache::Entry& GunPreviewCache::store(const gfx::ReplacementModel* model, Entry entry) {
    return entries_[model] = std::move(entry);
}

}  // namespace omega::mods
