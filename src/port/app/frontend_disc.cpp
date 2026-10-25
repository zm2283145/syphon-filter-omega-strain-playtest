#include "frontend_disc.hpp"

#include "agent_camera.hpp"
#include "disc.hpp"
#include "mod_movie.hpp"
#include "mod_set.hpp"
#include "movie_pss.hpp"
#include "movie_subtitle_overlay.hpp"
#include "movie_subtitles.hpp"
#include "string_pool.hpp"
#include "zone_profile.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace omega {

FrontendDisc::FrontendDisc(DiscImage& disc, const std::filesystem::path& iso)
    : disc_(disc),
      fonts_(load_gui_fonts(disc)),
      strings_(decode_string_pool(disc.read(disc.file({"GAMEDATA", "COMMON", "STRINGS.DAT"})))),
      awards_(load_profile_awards(disc)),
      assets_(disc),
      movie_stream_(iso, std::ios::binary),
      movie_members_(disc.archive_member_names(movie::movies_hog())) {
    if (!movie_stream_) throw std::runtime_error("Cannot open the ISO for the movies");
    try {
        zeus_members_ = disc.archive_member_names(zeus_hog());
        zeus_present_ = true;
    } catch (const std::exception&) {
        zeus_present_ = false;
    }
}

const std::vector<std::string>& FrontendDisc::zeus_hog() {
    static const std::vector<std::string> path{"GAMEDATA", "FRONTEND", "LANG", "ENGLISH", "ZEUS.HOG"};
    return path;
}

namespace {
bool same_name(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::toupper(static_cast<unsigned char>(x)) == std::toupper(static_cast<unsigned char>(y));
           });
}
}  // namespace

std::int32_t FrontendDisc::zeus_lookup(const std::string& name) const {  // 0036d2a0
    for (std::size_t i = 0; i < zeus_members_.size(); ++i)
        if (same_name(zeus_members_[i], name)) return static_cast<std::int32_t>(i);
    return -1;
}

gui::GuiHost::BitmapTexture FrontendDisc::zeus_load(const std::string& name) {
    // 00380a40: one slot per stem (after the last '\', up to the first '.',
    // without case), shared and counted (+64); a missing member throws.
    if (name.size() < 3 || !same_name(name.substr(name.size() - 3), "tdx"))
        throw std::runtime_error("unhosted(0x380c68): texture " + name + " is not a TDX (answers -1)");
    std::string stem = name.substr(name.rfind('\\') == std::string::npos ? 0 : name.rfind('\\') + 1);
    stem = stem.substr(0, stem.find('.'));
    for (char& c : stem) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (const auto it = zeus_slots_.find(stem); it != zeus_slots_.end()) {
        ++zeus_by_slot_.at(it->second).refs;
        if (zeus_released_.count(it->second)) zeus_restore(it->second);
        const Texture& t = zeus_images_.at(static_cast<std::uint32_t>(it->second));
        return {it->second, static_cast<std::int32_t>(t.width), static_cast<std::int32_t>(t.height)};
    }
    const std::int32_t index = zeus_lookup(name);
    if (index < 0) throw std::runtime_error("unhosted(0x380a40): texture " + name + " is not in ZEUS.HOG");
    return slot_load(zeus_hog(), zeus_members_[static_cast<std::size_t>(index)], stem);
}

gui::GuiHost::BitmapTexture FrontendDisc::slot_load(const std::vector<std::string>& archive, const std::string& member,
                                                    const std::string& stem) {
    Texture t = decode_tdx(disc_.archive_member(archive, member));
    // Port: slot numbers are never reused, so a renderer cannot show a stale picture.
    const std::int32_t slot = zeus_first_slot + zeus_next_++;
    const std::int32_t w = static_cast<std::int32_t>(t.width), h = static_cast<std::int32_t>(t.height);
    zeus_images_.emplace(static_cast<std::uint32_t>(slot), std::move(t));
    zeus_slots_.emplace(stem, slot);
    zeus_by_slot_.emplace(slot, ZeusSlot{stem, 1});
    zeus_sources_.emplace(slot, std::make_pair(archive, member));
    return {slot, w, h};
}

void FrontendDisc::zeus_restore(std::int32_t slot) {
    const auto& [archive, member] = zeus_sources_.at(slot);
    zeus_images_.emplace(static_cast<std::uint32_t>(slot), decode_tdx(disc_.archive_member(archive, member)));
    zeus_released_.erase(slot);
}

std::size_t FrontendDisc::release_images() {
    const auto count = [](const Texture& t) {
        std::size_t n = t.rgba.size();
        for (const auto& m : t.mips) n += m.size();
        for (const auto& a : t.raw_alpha) n += a.size();
        return n;
    };
    std::size_t bytes = assets_.release_images();
    for (const auto& [slot, source] : zeus_sources_) {
        const auto it = zeus_images_.find(static_cast<std::uint32_t>(slot));
        if (it == zeus_images_.end()) continue;
        bytes += count(it->second);
        for (const auto& f : it->second.animation) bytes += count(f);
        zeus_images_.erase(it);
        zeus_released_.insert(slot);
    }
    return bytes;
}

void FrontendDisc::restore_images() {
    assets_.restore_images();
    while (!zeus_released_.empty()) zeus_restore(*zeus_released_.begin());
}

namespace {
const std::vector<std::string>& frontend_mantex() {
    static const std::vector<std::string> path{"GAMEDATA", "FRONTEND", "MANTEX.HOG"};
    return path;
}
}  // namespace

gui::GuiHost::BitmapTexture FrontendDisc::mantex_load(const std::string& name) {
    // 0036e4b0 (archive 0): the file by name; the front end's MANTEX.HOG members are its patch
    // pictures. Shared and counted per name like 00380a40's stems (the slot map is one table).
    if (mantex_members_.empty()) mantex_members_ = disc_.archive_member_names(frontend_mantex());
    std::string key = "MANTEX\\" + name;
    for (char& c : key) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (const auto it = zeus_slots_.find(key); it != zeus_slots_.end()) {
        ++zeus_by_slot_.at(it->second).refs;
        if (zeus_released_.count(it->second)) zeus_restore(it->second);
        const Texture& t = zeus_images_.at(static_cast<std::uint32_t>(it->second));
        return {it->second, static_cast<std::int32_t>(t.width), static_cast<std::int32_t>(t.height)};
    }
    for (const auto& m : mantex_members_)
        if (same_name(m, name)) return slot_load(frontend_mantex(), m, key);
    throw std::runtime_error("unhosted(0x36e4b0): texture " + name + " is not in FRONTEND/MANTEX.HOG");
}

AgentPartCache* FrontendDisc::agent_parts() {
    if (agent_parts_) return agent_parts_.get();
    // frontCharacter (0022a220) over GAMEDATA/FRONTEND: 004037f0 SWAPIT.TBL, 00403d20 MANTEX.HOG,
    // 00404060 MORPH.HOG (its ".BIN" lists too), 00404aa0 MANSKM.HOG (its ".BON" presets too).
    const std::vector<std::string> morph{"GAMEDATA", "FRONTEND", "MORPH.HOG"};
    const std::vector<std::string> manskm{"GAMEDATA", "FRONTEND", "MANSKM.HOG"};
    const auto members = [&](const std::vector<std::string>& archive) {
        std::vector<AgentArchiveMember> out;
        for (const auto& n : disc_.archive_member_names(archive))
            out.push_back({n, static_cast<std::size_t>(disc_.archive_member_extent(archive, n).size)});
        return out;
    };
    auto c = std::make_unique<AgentPartCache>();
    agent_cache_load_rules(*c, disc_.read(disc_.file({"GAMEDATA", "FRONTEND", "SWAPIT.TBL"})));
    // Checkpoint 132cc6: the game init's 00403490 (00169574) parse of MANSORT2.TXT into +11c10.
    agent_cache_load_sort(*c, disc_.read(disc_.file({"GAMEDATA", "FRONTEND", "MANSORT2.TXT"})));
    if (mantex_members_.empty()) mantex_members_ = disc_.archive_member_names(frontend_mantex());
    agent_cache_register_textures(*c, mantex_members_);
    const auto morphs = members(morph);
    agent_cache_register_morphs(*c, morphs);
    const auto skm = members(manskm);
    std::vector<std::string> skm_names;
    for (const auto& m : skm) skm_names.push_back(m.name);
    agent_cache_register_meshes(*c, skm_names);
    agent_cache_register_bone_presets(*c, skm);
    for (const auto& m : morphs) {
        const auto dot = m.name.rfind('.');
        if (m.size == 0 || dot == std::string::npos || !same_name(m.name.substr(dot), ".BIN")) continue;
        agent_cache_register_morph_list(*c, m.name, disc_.archive_member(morph, m.name));
    }
    agent_parts_ = std::move(c);
    return agent_parts_.get();
}

const FrontendLights& FrontendDisc::lights() {
    if (!lights_) {
        // 004053b0(0055dda0) from the game init (00169574): MANLIGHT.TXT / GUNLIGHT.TXT through 004029b0.
        // A file that does not open takes 004029b0's default lights (00402d80).
        const auto read = [this](const char* name) -> std::optional<std::string> {
            Extent extent{};
            try {
                extent = disc_.file({"GAMEDATA", "FRONTEND", name});
            } catch (const std::exception&) {
                return std::nullopt;
            }
            const auto bytes = disc_.read(extent);
            return std::string(bytes.begin(), bytes.end());
        };
        const auto man = read("MANLIGHT.TXT"), gun = read("GUNLIGHT.TXT");
        lights_ = frontend_lights_4053b0(man ? std::optional<std::string_view>(*man) : std::nullopt,
                                         gun ? std::optional<std::string_view>(*gun) : std::nullopt);
    }
    return *lights_;
}

WeaponModelLibrary* FrontendDisc::weapons() {
    if (weapon_library_) return weapon_library_.get();
    weapon_db_ = std::make_unique<WeaponDatabase>(
        WeaponDatabase::load(disc_.read(disc_.file({"GAMEDATA", "COMMON", "WEAPONS.WDB"}))));
    const std::vector<std::string> wpnhog{"GAMEDATA", "COMMON", "WPNHOG.HOG"};
    WeaponModelArchive archive;
    archive.names = disc_.archive_member_names(wpnhog);
    archive.read = [this, wpnhog, names = archive.names](std::size_t i) {
        return disc_.archive_member(wpnhog, names.at(i));
    };
    weapon_library_ = std::make_unique<WeaponModelLibrary>(*weapon_db_, std::move(archive),
                                                           disc_.read(disc_.file({"GAMEDATA", "COMMON", "ERROR.TDX"})));
    return weapon_library_.get();
}

void FrontendDisc::weapon_display_draw(GuiDrawList& out, IeDrawState* st, const gui::Widget& w, std::uint32_t frame) {
    const auto& d = *w.wd;
    WeaponModelLibrary* lib = weapons();
    if (!lib->contains(d.model))
        throw std::runtime_error("00440580: weapon model handle " + std::to_string(d.model) + " is not loaded");
    if (!st) throw std::runtime_error("00440580: GuiWeaponDisplay without the IE matrix stack");
    const GunModel& model = lib->model(d.model);
    if (!gun_model_drawn(model) || d.fade == 1.0F) return;
    std::array<float, 4> lo{}, hi{};
    try {
        for (std::size_t c = 0; c < 4; ++c) {
            lo[c] = model.real(0x3e0 + 4 * c);
            hi[c] = model.real(0x3f0 + 4 * c);
        }
    } catch (const std::exception& e) {
        throw std::runtime_error("00440580: model " + std::to_string(d.model) + " bounds +3e0: " + e.what());
    }
    if (!(lo[0] <= hi[0])) return;
    const auto m = weapon_display_matrices(st->top(), w.rect, lo, hi, d.rot, d.fade);
    // 003836b0(model, 2, a2, 1): one batch per drawn part, lit by the VU1 model program from GUNLIGHT's
    // rows (frontend_light_colour on the model-frame normals); the flare programs 12 / 14 write 0x7f,
    // unlit. Host choice: texture filter linear; the +416 alpha below 0x80 as the vertex alpha.
    if (gun_generation_ != lib->generation()) {
        gun_parts_.clear();
        gun_slots_.clear();
        gun_overrides_.clear();
        gun_mod_slots_.clear();
        gun_preview_cache_.clear();
        gun_generation_ = lib->generation();
    }
    const FrontendLightSet& lit = lights().gun;
    const auto matrices = gun_preview_matrices(model, frame);
    auto overrides = gun_overrides_.find(d.model);
    if (overrides == gun_overrides_.end()) {
        std::shared_ptr<const mods::GunModelOverrides> o;
        try {
            o = mods::gun_model_overrides(model, lib->hog(d.model), {});
        } catch (const std::exception&) {
        }
        overrides = gun_overrides_.emplace(d.model, std::move(o)).first;
    }
    for (const auto part : gun_preview_parts(model)) {
        auto it = gun_parts_.find({d.model, part});
        if (it == gun_parts_.end()) {
            try {
                it = gun_parts_
                         .emplace(std::make_pair(d.model, part),
                                  std::make_shared<const GunPartMesh>(gun_part_mesh(model, lib->hog(d.model), part, 0)))
                         .first;
            } catch (const std::exception& e) {
                throw std::runtime_error("003836b0: model " + std::to_string(d.model) + " part " + std::to_string(part) +
                                         ": " + e.what());
            }
        }
        const GunPartMesh& decoded = *it->second;
        if (!gun_part_skinnable(decoded.mesh))
            throw std::runtime_error("003836b0: model " + std::to_string(d.model) + " part " + std::to_string(part) +
                                     " runs a VU1 program that is not hosted");
        const auto id = decoded.texture;
        const auto& list = lib->textures();
        if (id < 0 || static_cast<std::size_t>(id) >= list.size())
            throw std::runtime_error("003836b0: model " + std::to_string(d.model) + " part " + std::to_string(part) +
                                     " texture id " + std::to_string(id) + " not loaded");
        auto slot = gun_slots_.find(id);
        if (slot == gun_slots_.end()) {
            const auto& tex = list[static_cast<std::size_t>(id)];
            Texture image = tex.tdx.empty() ? decode_tdx(disc_.read(disc_.file({"GAMEDATA", "COMMON", "ERROR.TDX"})))
                                            : decode_tdx(tex.tdx);
            const std::int32_t s = zeus_first_slot + zeus_next_++;
            zeus_images_.emplace(static_cast<std::uint32_t>(s), std::move(image));
            slot = gun_slots_.emplace(id, s).first;
        }
        std::int32_t slot_id = slot->second;
        const auto* swap = overrides->second ? overrides->second->part(part, 0) : nullptr;
        const bool flare = gun_part_flare_program(decoded.mesh);
        const std::int32_t c = flare ? 0x7f : 0x80;
        const std::int32_t a = m.alpha_416 < 0x80 ? m.alpha_416 : c;
        SkinnedMeshGeometry geometry;
        // 132md7 (port-only, research/MODDING.md "Speed"): a replaced part is skinned and lit once, its vertices shared
        // across frames while its matrices, program and alpha are unchanged.
        const auto* kept = swap ? gun_preview_cache_.find(swap, matrices, flare, a) : nullptr;
        if (swap && !kept) {
            mods::skin_gun_replacement_into(*overrides->second, *swap, matrices, geometry);
            if (swap->image) {
                auto own = gun_mod_slots_.find(swap);
                if (own == gun_mod_slots_.end()) {
                    const std::int32_t s = zeus_first_slot + zeus_next_++;
                    zeus_images_.emplace(static_cast<std::uint32_t>(s),
                                         mods::model_image_texture(*swap->image, zeus_images_.at(static_cast<std::uint32_t>(slot->second))));
                    own = gun_mod_slots_.emplace(swap, s).first;
                }
                slot_id = own->second;
            }
            kept = &gun_preview_cache_.store(swap, {matrices, flare, a, static_cast<std::uint32_t>(slot_id),
                                                    std::make_shared<const std::vector<GuiVertex>>(
                                                        mods::gun_preview_vertices(geometry, lit, flare, c, a))});
        } else if (!swap) {
            geometry = gun_part_geometry(decoded.mesh, matrices);
        }
        GuiDrawBatch b;
        b.flush = 0x3836b0;
        b.render_mode = 1;
        b.slot = static_cast<std::uint32_t>(slot_id);
        b.filter = 0x20;
        b.matrix = m.model;
        if (kept) {
            if (kept->vertices->empty()) continue;
            b.slot = kept->slot;
            b.shared_vertices = kept->vertices;
            b.strips = {static_cast<std::int32_t>(kept->vertices->size())};
            out.batches.push_back(std::move(b));
            continue;
        }
        for (const auto& t : geometry.triangles)
            for (const auto i : t) {
                const auto& v = geometry.vertices.at(i);
                GuiVertex g;
                g.pos = {v.position[0], v.position[1], v.position[2], 1.0F};
                if (flare)
                    g.colour = {c, c, c, a};
                else {
                    const auto l = frontend_light_colour(lit, {v.normal[0], v.normal[1], v.normal[2]});
                    g.colour = {l[0], l[1], l[2], a};
                }
                g.uv = {v.uv[0] + 2048.0F, v.uv[1] + 2048.0F, 1.0F, 0.0F};
                b.vertices.push_back(g);
            }
        if (b.vertices.empty()) continue;
        b.strips = {static_cast<std::int32_t>(b.vertices.size())};
        out.batches.push_back(std::move(b));
    }
}

std::string FrontendDisc::agent_figure_draw(GuiDrawList& out, const AgentPacking& packing, const AgentCamera& camera,
                                            float seconds, std::uint32_t flush, const std::array<float, 4>& turns) {
    lights();
    if (!preview_) preview_ = std::make_unique<AgentPreviewRenderer>(disc_);
    const AgentCameraView view = agent_camera_view(camera, turns);
    const auto& m = view.model;
    // The agent screens' camera turns +b0..+b8 are zero (R the identity), so MANLIGHT's rows light the
    // world-frame normals as they are. GuiAgentDisplay's (0, 0, pi) turn them by R as the stats figure
    // does (figure_camera_direction); its draw 00361e70 makes no 00373670 call, so the rows are the ones
    // the frame loop set (0013e820: 00373670(00535d20), MANLIGHT).
    FrontendLightSet lit = lights_->man;
    if (turns != std::array<float, 4>{})
        for (auto& d : lit.directions)
            if (d != std::array<float, 3>{}) d = figure_camera_direction(view.rotation, d);
    std::size_t parts = 0, triangles = 0;
    auto&& posed = [&]() -> decltype(auto) {
        OMEGA_ZONE("rec.figure.posed");
        return preview_->posed(packing, seconds);
    }();
    OMEGA_ZONE("rec.figure.vertices");
    for (const auto& part : posed) {
        GuiDrawBatch b = out.take();
        b.flush = flush;
        b.clip = view.clip;
        b.filter = 0x20;
        b.render_mode = part.image ? 1U : 0U;
        if (part.image) {
            auto it = figure_slots_.find(part.texture);
            if (it == figure_slots_.end()) {
                const std::int32_t slot = zeus_first_slot + zeus_next_++;
                zeus_images_.emplace(static_cast<std::uint32_t>(slot), *part.image);
                it = figure_slots_.emplace(part.texture, slot).first;
            }
            b.slot = static_cast<std::uint32_t>(it->second);
        }
        const auto& g = *part.geometry;
        b.vertices.reserve(g.triangles.size() * 3);
        // Port-only: a vertex shared by several triangles is transformed and lit once (the same arithmetic, so the
        // same bits), then copied per corner.
        figure_unique_.resize(g.vertices.size());
        figure_done_.assign(g.vertices.size(), 0);
        for (const auto& t : g.triangles)
            for (const auto k : t) {
                const auto& v = g.vertices.at(k);
                if (figure_done_[k]) {
                    b.vertices.push_back(figure_unique_[k]);
                    continue;
                }
                GuiVertex& gv = figure_unique_[k];
                const auto& p = v.position;
                gv.pos = {p[0] * m[0][0] + p[1] * m[1][0] + p[2] * m[2][0] + m[3][0],
                          p[0] * m[0][1] + p[1] * m[1][1] + p[2] * m[2][1] + m[3][1],
                          p[0] * m[0][2] + p[1] * m[1][2] + p[2] * m[2][2] + m[3][2], 1.0F};
                const auto& n = v.normal;
                const std::array<float, 3> nw{n[0] * m[0][0] + n[1] * m[1][0] + n[2] * m[2][0],
                                              n[0] * m[0][1] + n[1] * m[1][1] + n[2] * m[2][1],
                                              n[0] * m[0][2] + n[1] * m[1][2] + n[2] * m[2][2]};
                const auto c = frontend_light_colour(lit, nw);
                gv.colour = {c[0], c[1], c[2], 0x80};
                gv.uv = {v.uv[0] + 2048.0F, v.uv[1] + 2048.0F, 1.0F, 0.0F};
                figure_done_[k] = 1;
                b.vertices.push_back(gv);
            }
        if (b.vertices.empty()) continue;
        triangles += b.vertices.size() / 3;
        b.strips = {static_cast<std::int32_t>(b.vertices.size())};
        out.batches.push_back(std::move(b));
        ++parts;
    }
    std::string note = std::to_string(parts) + " parts, " + std::to_string(triangles) + " triangles";
    if (!preview_->skipped().empty()) {
        note += "; missing meshes:";
        for (const auto& s : preview_->skipped()) note += " " + s;
    }
    if (!preview_->replaced().empty()) {   // 132md3 (port-only)
        note += "; replaced by mods:";
        for (const auto& s : preview_->replaced()) note += " " + s;
    }
    return note;
}

std::vector<std::uint8_t> FrontendDisc::frontend_file(const std::string& path) {
    std::vector<std::string> parts;
    std::string part;
    for (const char c : path) {
        if (c == '\\' || c == '/') {
            if (!part.empty()) parts.push_back(part);
            part.clear();
        } else {
            part += c;
        }
    }
    if (!part.empty()) parts.push_back(part);
    if (parts.empty()) throw std::runtime_error("unhosted(0x402840): Skl_Load of an empty path");
    return disc_.read(disc_.file(parts));
}

void FrontendDisc::zeus_release(std::int32_t slot) {  // 003813a0(manager, slot, 0)
    const auto it = zeus_by_slot_.find(slot);
    if (it == zeus_by_slot_.end())
        throw std::runtime_error("unhosted(0x3813a0): release of texture slot " + std::to_string(slot) + " not loaded");
    if (--it->second.refs > 0) return;
    zeus_slots_.erase(it->second.stem);
    zeus_images_.erase(static_cast<std::uint32_t>(slot));
    zeus_sources_.erase(slot);
    zeus_released_.erase(slot);
    zeus_by_slot_.erase(it);
}

FrontendServices FrontendDisc::services(const std::filesystem::path& card_folder) {
    FrontendServices services;
    services.strings = [this](const std::string& key) -> std::optional<std::string> {
        const auto s = strings_.resolve(key);
        return s ? std::optional<std::string>(std::string(*s)) : std::nullopt;
    };
    services.font = [this](const std::string& name) { return fonts_.handle(name); };
    services.default_font = GuiDiscFonts::default_handle;
    services.font_height = [this](std::uint32_t h) -> std::uint8_t {
        const GuiFont* f = fonts_.font(h);
        if (!f) throw std::runtime_error("unhosted(0x36a2f0): font handle " + std::to_string(h));
        return f->height();
    };
    services.font_width = [this](std::uint32_t h, const std::string& s, std::int32_t n) -> std::int32_t {
        const GuiFont* f = fonts_.font(h);
        if (!f) throw std::runtime_error("unhosted(0x36aa50): font handle " + std::to_string(h));
        std::vector<std::uint8_t> bytes(s.begin(), s.end());
        bytes.push_back(0);
        return gui_font_width(*f, bytes, n);
    };
    services.gui_resources = disc_.archive_member_names({"GAMEDATA", "FRONTEND", "NTSC.HOG"});
    services.gui_file = [this](const std::string& screen) { return load_frontend_gui(disc_, screen); };
    services.decor = [this](const std::string& name) { return assets_.resolve(name); };
    services.awards = &awards_;
    try {
        services.charmap = disc_.read(disc_.file({"GAMEDATA", "COMMON", "SFO_CHAR.MAP"}));
    } catch (const std::exception&) {
        services.charmap = std::nullopt;
    }
    services.card_root = card_folder;
    services.movie_members = movie_members_;
    services.open_movie = [this](const std::string& name) -> std::unique_ptr<movie::MovieStream> {
        for (const auto& m : movie_members_) {
            if (m.size() != name.size() ||
                !std::equal(m.begin(), m.end(), name.begin(), [](char a, char b) {
                    return std::toupper(static_cast<unsigned char>(a)) == std::toupper(static_cast<unsigned char>(b));
                }))
                continue;
            const auto disc_movie = [this, m] {
                const Extent e = disc_.archive_member_extent(movie::movies_hog(), m);
                movie_stream_.clear();
                auto movie = std::make_unique<movie::PssMovie>(movie::stream_source(movie_stream_, e.offset, e.size), 256 * 1024);
                // Port-only (checkpoint 132win5): the 256 KiB read's pictures are decoded as they are shown, not all in
                // the frame that read them (a 40-56 ms stall every ~24 frames); the frames and audio are unchanged.
                movie->set_video_slice(2048);
                return movie;
            };
            // Port-only (checkpoint 132md14, research/MODDING.md "Movie overrides"): a mod's movie_overrides= picture
            // and / or sound (the session's clock, subtitles and end are unchanged). A replacement that cannot be opened
            // is logged and the disc's movie plays.
            if (const auto* set = mods::active())
                if (const auto source = set->movie(m); source && (!source->video.empty() || !source->audio.empty())) {
                    try {
                        auto replaced = std::make_unique<mods::ModMovie>(*source, disc_movie);
                        mods::note("movie " + mods::upper(m) + " <- " + source->mod->folder + ": " + replaced->describe());
                        return replaced;
                    } catch (const std::exception& e) {
                        mods::note("movie " + mods::upper(m) + " from " + source->mod->folder + " not played (" + e.what() +
                                   "); the disc's plays");
                    }
                }
            return disc_movie();
        }
        return nullptr;
    };
    services.movie_member = [this](const std::string& name) -> std::optional<std::vector<std::uint8_t>> {
        // Port-only (checkpoint 132md14): a movie_overrides= item's .sub is the movie's subtitles (<NAME>.SUB), whether
        // or not the disc has them; one that does not parse is logged and the disc's (if any) are used.
        if (const auto* set = mods::active(); set && set->has_movies()) {
            const auto upper = mods::upper(name);
            if (upper.ends_with(".SUB"))
                if (const auto source = set->movie(upper.substr(0, upper.size() - 3) + "PSS"); source && !source->subtitles.empty()) {
                    try {
                        auto bytes = mods::read_file(source->subtitles);
                        (void)movie::parse_subtitles(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
                        mods::note("movie subtitles " + upper + " <- " + source->mod->folder + ": " + source->subtitles.filename().string());
                        return bytes;
                    } catch (const std::exception& e) {
                        mods::note("movie subtitles " + upper + " from " + source->mod->folder + " not used (" + e.what() +
                                   "); the disc's are");
                    }
                }
        }
        for (const auto& m : movie_members_)
            if (m.size() == name.size() &&
                std::equal(m.begin(), m.end(), name.begin(), [](char a, char b) {
                    return std::toupper(static_cast<unsigned char>(a)) == std::toupper(static_cast<unsigned char>(b));
                }))
                return disc_.archive_member(movie::movies_hog(), m);
        return std::nullopt;
    };
    if (zeus_present_) {
        services.zeus_lookup = [this](const std::string& name) { return zeus_lookup(name); };
        services.zeus_load = [this](const std::string& name) { return zeus_load(name); };
    }
    services.zeus_release = [this](std::int32_t slot) { zeus_release(slot); };
    services.agent_parts = [this] { return agent_parts(); };
    services.file_load = [this](const std::string& name) { return mantex_load(name); };
    services.loc_find = [this](const std::string& key) { return strings_.lookup(key); };
    services.loc_text = [this](std::int32_t id) -> std::optional<std::string> {
        const auto s = strings_.text(id);
        return s ? std::optional<std::string>(std::string(*s)) : std::nullopt;
    };
    services.frontend_file = [this](const std::string& path) { return frontend_file(path); };
    services.weapons = [this] { return weapons(); };
    return services;
}

const char* frontend_class_name(gui::WidgetClass c) {
    switch (c) {
        case gui::WidgetClass::title: return "TITLESCR";
        case gui::WidgetClass::agentload: return "AGENTOPN";
        case gui::WidgetClass::agentcreate: return "AGENTNEW";
        case gui::WidgetClass::agentinfo: return "AGNTINFO";
        case gui::WidgetClass::agentsave: return "AGENTSAVE";
        case gui::WidgetClass::onoffsel: return "ONOFFSEL";
        case gui::WidgetClass::cmdcentr: return "CMDCENTR";
        case gui::WidgetClass::onlinecn: return "ONLINECN";
        case gui::WidgetClass::msnsetup: return "MSNSETUP";
        case gui::WidgetClass::netjoin: return "JOINMIS";
        case gui::WidgetClass::netcontacts: return "CONTACTS";  // checkpoint 132on2h
        case gui::WidgetClass::netmess: return "NETMESS";       // checkpoint 132on2h
        case gui::WidgetClass::acntlogn: return "ACNTLOGN";
        case gui::WidgetClass::msgbox: return "POPUP";
        case gui::WidgetClass::keyboard: return "KEYBOARD";
        case gui::WidgetClass::slideshow: return "ZEUSHELL";
        case gui::WidgetClass::missionstats: return "STATS";
        default: return "SCREEN";
    }
}

GuiDrawList movie_list(float aspect) {
    GuiDrawList list;
    GuiDrawBatch b;
    b.render_mode = 5;
    b.slot = movie_slot;
    b.strips = {2};
    GuiVertex tl, br;
    float hx = 320.0F, hy = 224.0F;
    if (aspect > 4.0F / 3.0F + 0.01F) {
        hy = 224.0F * (4.0F / 3.0F) / aspect;
        b.port_picture_aspect = aspect;
    } else if (aspect > 0.0F && aspect < 4.0F / 3.0F - 0.01F) {
        hx = 320.0F * aspect / (4.0F / 3.0F);
    }
    tl.pos = {-hx, hy, 0.0F, 1.0F};
    br.pos = {hx, -hy, 0.0F, 1.0F};
    tl.colour = br.colour = {128, 128, 128, 128};
    tl.uv = {2048.0F, 2048.0F, 1.0F, 0.0F};
    br.uv = {2049.0F, 2049.0F, 1.0F, 0.0F};
    b.vertices = {tl, br};
    b.port_picture = true;  // port-only (132ws): a 4:3 picture under the wide aspect (bars, or movie_fill zoom)
    b.port_movie = true;    // port-only (132vt2a2): fitted by movies= (fill, bars or stretch)
    list.batches.push_back(std::move(b));
    return list;
}

Texture movie_texture(const mpeg2::Frame& frame) {
    Texture t;
    t.width = static_cast<std::uint32_t>(frame.width);
    t.height = static_cast<std::uint32_t>(frame.height);
#ifdef __vita__
    if (const auto& take = vita_movie_hooks().take_rgba; take && take(t.rgba)) return t;
#endif
    movie::frame_to_rgba(frame, t.rgba);
    return t;
}

#ifdef __vita__
VitaMovieHooks& vita_movie_hooks() {
    static VitaMovieHooks hooks;
    return hooks;
}
#endif

const movie::SubtitleProjection& front_end_subtitle_projection() {
    static const movie::SubtitleProjection projection = movie::subtitle_projection();
    return projection;
}

void append_subtitles(GuiDrawList& list, const GuiDiscFonts& fonts, const movie::SubtitleTrack& track,
                      const std::array<float, 4>& colour, std::uint32_t render_mode) {
    const GuiFont* font = fonts.font(GuiDiscFonts::default_handle);
    const auto image = font ? fonts.images.find(font->slot()) : fonts.images.end();
    if (image == fonts.images.end()) throw std::runtime_error("unhosted(0x4118e0): the DEFAULT font is not loaded");
    const movie::SubtitleProjection& projection = front_end_subtitle_projection();
    const auto th = static_cast<std::uint16_t>(image->second.height);
    const movie::SubtitlePacket packet =
        movie::subtitle_packet_004118e0(movie::subtitle_overlay_state(track, colour), *font, font->tex_width, th,
                                        projection, 0, movie::subtitle_tex1_003bb620(0, render_mode));
    if (packet.pairs.size() > 4) list.batches.push_back(movie::subtitle_batch(packet, font->slot(), font->tex_width, th));
}

}  // namespace omega
