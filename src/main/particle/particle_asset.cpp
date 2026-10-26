#include "particle_asset.hpp"

#include <algorithm>
#include <bit>
#include <climits>
#include <cstring>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>

namespace omega::particle {
namespace {

[[noreturn]] void unhosted(std::uint32_t address, const char* what) {
    char b[160];
    std::snprintf(b, sizeof b, "particle asset: unhosted %s (%08x)", what, address);
    throw std::runtime_error(b);
}

constexpr std::uint8_t kMultiplayer = 0;  // byte 005721c8: solo play

}  // namespace

ParticleAsset::ParticleAsset(int id) : id_(id) {}
ParticleAsset::ParticleAsset(int id, std::uint32_t key) : id_(id), type_word_(key) {}

std::array<std::array<float, 4>, 4> placement_node_matrix(const std::array<float, 3>& position,
                                                          const std::array<float, 9>& basis) {
    return {{{basis[0], basis[1], basis[2], 0.0f},
             {basis[3], basis[4], basis[5], 0.0f},
             {basis[6], basis[7], basis[8], 0.0f},
             {position[0], position[1], position[2], 1.0f}}};
}

namespace {
// 00398070: strstr(name, ".par") (0x004bc640) or strcat it.
std::string request_file(const std::string& name) {
    return name.find(".par") == std::string::npos ? name + ".par" : name;
}
}  // namespace

int ParticleLibrary::lookup(const std::string& name) const {
    std::uint8_t key[16] = {};
    copy_particle_name(key, name, 0);
    return ParticleSystem(definitions()).find(key);  // 003a3430's name compare
}

void ParticleLibrary::load(const std::string& file) {
    if (lookup(file) != -1) return;
    const auto text = read_(file);
    if (!text) return;
    auto d = std::make_unique<ParticleDefinition>();
    d->bytes.fill(0xa5);  // the 0036b180 block is not cleared; the goldens use this fill
    std::string texture;
    if (!parse_particle_definition(*text, *d, texture)) return;
    write_packet_template(*d);  // 003a58e0 0x3a59a0..0x3a5b1c
    ParticleDefinition& def = *d;
    defs_.push_back(std::move(d));
    textures_.push_back(texture);
    const std::int32_t children = def.i32(def::num_children);
    for (std::int32_t k = 0; k < children; ++k) load(def.child_name(k) + ".par");
}

int ParticleLibrary::request(const std::string& name) {
    const std::string file = request_file(name);
    load(file);
    const int id = lookup(file);
    // 00397bb0..00397c30: strcmp (00129218) of the record name with
    // 004bc5e0 / 004bc600 / 004bc618 / 004bc630 stores the id in
    // 0053b5a0 / 5a8 / 5b0 / 5b8.
    static constexpr const char* kSpecial[4] = {"ShatterWindow.par", "SnowBelarus2.par", "RainTokyo.par",
                                                "RainLorelei.par"};
    for (int k = 0; k < 4; ++k)
        if (file == kSpecial[k]) {
            special_[static_cast<std::size_t>(k)] = id;
            break;
        }
    return id;
}

std::vector<ParticleDefinition*> ParticleLibrary::definitions() const {
    std::vector<ParticleDefinition*> r;
    for (const auto& d : defs_) r.push_back(d.get());
    return r;
}

void ParticleAsset::set_present(ParticleSystem& system, const std::uint8_t* node_matrix) {
    if (id_ != -1 && !instance_) {
        // 00394270: 0036b180(0x70); the node refresh (vt+58 when byte +50)
        // only feeds the matrix (node+10), which 003a2dd0 reads only for the
        // ShatterWindow root (0039f860, row 1).
        auto inst = std::make_unique<EffectInstance>();
        system.construct(*inst, id_, 1.0f, node_matrix);
        instance_ = std::move(inst);
        instance_->set_f32(0x50, time_);  // inst+50 = +6c
    }
    present_ = true;  // 003ce980
}

void ParticleAsset::set_absent() {
    // 003a2ba0(inst, -1) releases the emitters; 0036b130 frees the block.
    instance_.reset();
    present_ = false;  // 003ce790
}

void ParticleAsset::reset(ParticleSystem& system, const std::uint8_t* node_matrix) {
    time_ = 0.0f;  // 0023bb58 sw $zero, 0x6c
    set_absent();
    set_present(system, node_matrix);
}

void ParticleAsset::set_on(bool on) {
    if (kMultiplayer != 0) unhosted(on ? 0x1c572c : 0x1c569c, "the multiplayer Enable / Disable message");
    on_ = on;  // 001c5784 / 001c56f0
}

std::optional<std::array<float, 4>> ParticleAsset::update(ParticleSystem& system, const std::uint8_t* node_matrix,
                                                          float dt, std::optional<std::int32_t> shatter_id) {
    if (released_) unhosted(0x40c9a0, "the update of an unregistered (released type-e7) asset");
    // 00394590
    if (instance_) {
        if (instance_->u32(0x1c) != 0) {
            system.update(*instance_, node_matrix, 0, dt);  // 003a2400(inst, node+10, 0, dt)
        } else if (on_) {                      // vt+6c 0015bfa0
            if (kMultiplayer != 0) unhosted(0x1c569c, "the multiplayer switch-off message");
            on_ = false;                       // vt+68 001c5680
            if (((type_word_ >> 16) & 0xff) == 0xe7) {
                // 00394648..003946ec: 0040c9a0 (unregister), 003cb230(asset, 0.1f) (the delayed
                // delete, a 0x10 record of vtable 004d9de0), then 003a2ba0(inst, -1), free, +64 = 0.
                released_ = true;
                instance_.reset();
            }
        }
        // 00394590 +6c += dt (single precision, as add.s).
        time_ = time_ + dt;
    }
    // 001c57a0: 00394170 (instance byte +68 != 0), 00394140 (sp370 = inst+30, the byte).
    if (!instance_ || instance_->bytes[0x68] == 0) return std::nullopt;
    if (instance_->bytes[0x68] != 1) return std::nullopt;  // 001c57d4
    if (!shatter_id) unhosted(0x1c57e0, "the effect table entry 9 (00236f20) of a shard-hit message");
    if (id_ != *shatter_id) return std::nullopt;  // 001c57ec: +60 against *00236f20(9)
    if ((system.frame & 7) != 0) return std::nullopt;  // 001c57f4: *005383c8 & 7
    std::array<float, 4> at;
    std::memcpy(at.data(), instance_->bytes.data() + 0x30, 16);
    return at;
}

std::optional<EffectBounds> ParticleAsset::bounds() const {
    if (!instance_) return std::nullopt;  // 003943b4
    const EffectBounds b = effect_bounds(*instance_);
    if (scale_ == 1.0f) return b;         // 003943cc c.eq.s
    // 00394450..00394504: each 003a2a90 call recomputes the same box.
    return scale_effect_bounds(b, scale_);
}

void ParticleAsset::draw(ParticleSystem& system, std::uint32_t flags, std::vector<ParticleBatch>& out,
                         std::vector<ShatterFlush>* shatter, std::optional<float> distance_fade) {
    // 00394520: the node refresh, then 003a1ad0(inst, +38, node+10); the node
    // matrix goes to the VU1 transform (0037c350), i.e. to the host.
    if (released_) unhosted(0x40c9a0, "the draw of an unregistered (released type-e7) asset");
    if (instance_) system.draw_instance(*instance_, flags, out, shatter, distance_fade);
}

std::optional<ParticleSpriteBatch> particle_sprite_batch(const ParticleBatch& b, const std::array<Qword, 4>& cull,
                                                         const SpriteFrame& frame, bool skip_heat_haze) {
    {
        const bool haze = b.stop == 0x1cc8;
        if (haze && skip_heat_haze) return std::nullopt;
        if (!haze && b.stop != 0x5e0 && b.stop != 0x8a0) unhosted(b.stop, "a particle sprite program");
        ParticleSpriteBatch out;
        out.def = b.def;
        out.stop = b.stop;
        if (haze && !b.def) unhosted(0x1cc8, "a heat-haze batch without its definition (the def+d0 GIF tags)");
        if (b.def && !haze) std::memcpy(&out.alpha, b.def->bytes.data() + 0xd0, 8);
        out.mode = cull_batch(b.bound_min, b.bound_max, cull);
        if (out.mode == 0) return std::nullopt;
        // 132vc1 (port-side speed only, research/VITA.md "Render pass on the Vita's cores (132vc1)"): the program's
        // words in this thread's buffer, reused from batch to batch (the PS Vita's three cores share newlib's heap lock).
        thread_local std::vector<Qword> words;
        std::vector<Qword>& q = words;
        try {
            if (haze) {
                // TOP+5: the template's def+d0 qword, the two GIF tags (particle_emitter's heatHaze words).
                Qword tag;
                std::memcpy(tag.data(), b.def->bytes.data() + 0xd0, 16);
                run_heat_haze_program_into(q, b.position, b.colour, frame, tag);
            } else {
                run_sprite_program_into(q, b.stop, b.position, b.colour, frame);
            }
        } catch (const std::runtime_error& e) {
            // Name the batch: the definition, its emitter vtable (type 0 / 1) and the first position whose
            // words carry an exponent of 254 or 255 (a clamped or raw overflow from the particle program).
            std::string what = e.what();
            char buf[160];
            std::snprintf(buf, sizeof buf, "; batch of %zu, stop %03x, pass %d, emitter vtable %08x", b.position.size(),
                          b.stop, b.pass, b.emitter ? b.emitter->u32(0) : 0u);
            what += buf;
            if (b.def) what += ", effect \"" + b.def->effect_name() + "\"";
            for (std::size_t k = 0; k < b.position.size(); ++k) {
                const Qword& p = b.position[k];
                if (std::none_of(p.begin(), p.end(), [](std::uint32_t w) { return ((w >> 23) & 0xff) >= 0xfe; }))
                    continue;
                std::snprintf(buf, sizeof buf, ", position %zu %08x %08x %08x %08x", k, p[0], p[1], p[2], p[3]);
                what += buf;
                break;
            }
            throw std::runtime_error(what);
        }
        const auto f = [](std::uint32_t w) { return std::bit_cast<float>(w); };
        // Checkpoint 132cp8: particles with an exponent-255 operand run under the PS2 VU float rules
        // (particle_vu1.hpp) and are drawn or clipped like the rest; counted for the play log.
        const auto range = [&](std::size_t k) {
            const auto e255 = [](const Qword& w) {
                return std::any_of(w.begin(), w.end(), [](std::uint32_t x) { return ((x >> 23) & 0xff) == 0xff; });
            };
            const bool hit = (k < b.position.size() && e255(b.position[k])) || (k < b.colour.size() && e255(b.colour[k]));
            out.range += hit;
            return hit;
        };
        if (haze) {
            // tag0, STQ0, RGBAQ, XYZ0, STQ1, RGBAQ, XYZ1, tag1, ST0, RGBAQ, XYZ0, ST1, RGBAQ, XYZ1: both sprites
            // share the corners and the ADC word.
            out.sprites.reserve(q.size() / 14);  // 132vc1: one block (the same sprites)
            for (std::size_t i = 0; i + 14 <= q.size(); i += 14) {
                const bool in_range = range(i / 14);
                if (q[i + 6][3] == 0xffff8000u) {
                    ++out.adc;
                    out.range_adc += in_range;
                    continue;
                }
                ParticleSprite s;
                s.stq_first = {f(q[i + 1][0]), f(q[i + 1][1]), f(q[i + 1][2])};
                s.stq_second = {f(q[i + 4][0]), f(q[i + 4][1]), f(q[i + 4][2])};
                for (int k = 0; k < 4; ++k) s.rgba[k] = static_cast<std::uint8_t>(q[i + 2][k] & 0xff);
                s.xy_first = {static_cast<std::int32_t>(q[i + 3][0]), static_cast<std::int32_t>(q[i + 3][1])};
                s.xy_second = {static_cast<std::int32_t>(q[i + 6][0]), static_cast<std::int32_t>(q[i + 6][1])};
                s.z = q[i + 6][2];
                s.uv_first = {f(q[i + 8][0]), f(q[i + 8][1])};
                s.uv_second = {f(q[i + 11][0]), f(q[i + 11][1])};
                out.sprites.push_back(s);
            }
            return out;
        }
        out.sprites.reserve(q.size() / 6);  // 132vc1: one block (the same sprites)
        for (std::size_t i = 0; i + 6 <= q.size(); i += 6) {
            // STQ0, RGBAQ, XYZ0, STQ1, RGBAQ, XYZ1
            const bool in_range = range(i / 6);
            if (q[i + 5][3] == 0xffff8000u) {
                ++out.adc;
                out.range_adc += in_range;
                continue;
            }
            ParticleSprite s;
            s.stq_first = {f(q[i][0]), f(q[i][1]), f(q[i][2])};
            s.stq_second = {f(q[i + 3][0]), f(q[i + 3][1]), f(q[i + 3][2])};
            for (int k = 0; k < 4; ++k) s.rgba[k] = static_cast<std::uint8_t>(q[i + 4][k] & 0xff);
            s.xy_first = {static_cast<std::int32_t>(q[i + 2][0]), static_cast<std::int32_t>(q[i + 2][1])};
            s.xy_second = {static_cast<std::int32_t>(q[i + 5][0]), static_cast<std::int32_t>(q[i + 5][1])};
            s.z = q[i + 5][2];
            out.sprites.push_back(s);
        }
        return out;
    }
}

std::vector<ParticleSpriteBatch> particle_sprites(const std::vector<ParticleBatch>& batches,
                                                  const std::array<Qword, 4>& cull, const SpriteFrame& frame,
                                                  bool skip_heat_haze) {
    std::vector<ParticleSpriteBatch> r;
    for (const ParticleBatch& b : batches)
        if (auto s = particle_sprite_batch(b, cull, frame, skip_heat_haze)) r.push_back(std::move(*s));
    return r;
}

ParticleSpriteBatch weather_sprite_batch(const WeatherFlush& flush, const SpriteFrame& frame) {
    if (!flush.sprites) throw std::runtime_error("weather_sprite_batch: a triangle flush (00371200)");
    ParticleSpriteBatch out;
    out.def = flush.def;
    out.stop = 0x5e0;
    out.alpha = flush.alpha;
    out.mode = 2;  // data 0 flag bit 0 (0037c90c): pairs 0x37..0x3a skip the cull with vi5 = 1
    std::vector<Qword> position, colour;
    position.reserve(flush.vertices.size());
    colour.reserve(flush.vertices.size());
    for (const ShatterVertex& v : flush.vertices) {
        position.push_back({std::bit_cast<std::uint32_t>(v.position[0]), std::bit_cast<std::uint32_t>(v.position[1]),
                            std::bit_cast<std::uint32_t>(v.position[2]), std::bit_cast<std::uint32_t>(v.position[3])});
        // 0025c9d0: cvt.w.s(128.0 * c) per component (the product is exact, cvt.w.s truncates).
        Qword c{};
        for (int k = 0; k < 4; ++k) {
            const double x = static_cast<double>(v.colour[k]) * 128.0;
            c[k] = static_cast<std::uint32_t>(x >= 2147483647.0 ? INT32_MAX
                                              : x <= -2147483648.0 ? INT32_MIN
                                                                   : static_cast<std::int32_t>(x));
        }
        colour.push_back(c);
    }
    const std::vector<Qword> q = run_sprite_program(0x5e0, position, colour, frame);
    const auto f = [](std::uint32_t w) { return std::bit_cast<float>(w); };
    for (std::size_t i = 0; i + 6 <= q.size(); i += 6) {
        if (q[i + 5][3] == 0xffff8000u) {
            ++out.adc;
            continue;
        }
        ParticleSprite s;
        s.stq_first = {f(q[i][0]), f(q[i][1]), f(q[i][2])};
        s.stq_second = {f(q[i + 3][0]), f(q[i + 3][1]), f(q[i + 3][2])};
        for (int k = 0; k < 4; ++k) s.rgba[k] = static_cast<std::uint8_t>(q[i + 4][k] & 0xff);
        s.xy_first = {static_cast<std::int32_t>(q[i + 2][0]), static_cast<std::int32_t>(q[i + 2][1])};
        s.xy_second = {static_cast<std::int32_t>(q[i + 5][0]), static_cast<std::int32_t>(q[i + 5][1])};
        s.z = q[i + 5][2];
        out.sprites.push_back(s);
    }
    return out;
}
}  // namespace omega::particle
