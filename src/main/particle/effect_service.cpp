#include "effect_service.hpp"
#include "vu_float.hpp"

#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace omega::particle {
namespace {
[[noreturn]] void fail(const std::string& what) { throw std::runtime_error("Effect service: " + what); }
}  // namespace

void ParticleEffectComponent::start() {
    // 236de0: 0036b180(0x70) then 003a2dd0(mem, id, 1.0, +10) into +c.
    auto inst = std::make_unique<EffectInstance>();
    system_.construct(*inst, id_, 1.0f);
    instance_ = std::move(inst);
}

void ParticleEffectComponent::stop() {
    // 236d20: 003a2ba0(inst, -1), 0036b130, +c = 0.
    instance_.reset();
}

void ParticleEffectComponent::update(float dt) {
    // 236cd0
    if (!instance_) return;
    system_.update(*instance_, matrix_.data(), 0, dt);
    if (instance_->u32(0x1c) == 0) done = true;
}

void ParticleEffectComponent::draw(std::uint32_t flags, std::vector<Drawn>& out) {
    // 236be0: the instance's bounds through +10 feed 00379b20, whose flags
    // go to 003a1ad0(inst, flags, +10); the matrix is the VU1 transform's.
    if (!instance_) return;
    Drawn d;
    for (std::size_t r = 0; r < 4; ++r)
        for (std::size_t c = 0; c < 4; ++c) std::memcpy(&d.world[r][c], matrix_.data() + 16 * r + 4 * c, 4);
    system_.draw_instance(*instance_, flags, d.batches);
    out.push_back(std::move(d));
}

LightEffectComponent::~LightEffectComponent() {
    // 144a60(light, 1): vtable 004d9240, id -1, vt+30 (00144980), vtable
    // 004d9280, then 00373d70 (type 4: id -1). 00144980 raises the manager's
    // +42 (00535d62) when the light was enabled.
    if (index_ >= lights_.point.size()) return;
    auto& l = lights_.point[index_];
    if (l.enabled) lights_.dirty_42 = 1;
    l.id = -1;
    l.enabled = false;
    l.vtable = level_light_vtable_released;
}

void LightEffectComponent::start() {
    if (index_ >= lights_.point.size()) fail("light outside the point pool");
    auto& l = lights_.point[index_];
    if (!l.enabled) lights_.dirty_42 = 1;   // vt+2c 00144b80: +42 (00535d62) on a change
    l.enabled = true;
}

void LightEffectComponent::stop() {
    if (index_ >= lights_.point.size()) fail("light outside the point pool");
    auto& l = lights_.point[index_];
    if (l.enabled) lights_.dirty_42 = 1;    // vt+30 00144980: +42 (00535d62) on a change
    l.enabled = false;
}

EffectService::Effect& EffectService::create(EffectKind kind) {
    // 003ec1d0: done = 0, clock = 0; 003ec2a0 appends to 0055c0a8.
    Effect e;
    e.kind = kind;
    e.serial = ++serial_;
    effects_.push_back(std::move(e));
    return effects_.back();
}

void EffectService::schedule(Effect& effect, std::unique_ptr<EffectComponent> component, float start, float end) {
    // 00232050
    for (std::size_t i = 0; i < kSlots; ++i) {
        auto& s = slots_[i];
        if (s.component) continue;
        s.component = std::move(component);
        s.start = start + effect.clock;
        s.end = end + effect.clock;
        ++count_;
        effect.slots.push_back(i);   // 002320f0 -> 00138ea0 (before the sentinel)
        return;
    }
    ++dropped;
}

void EffectService::update(float dt) {
    // 003ec4c0
    for (auto it = effects_.begin(); it != effects_.end();) {
        auto& e = *it;
        // 003ec6c0
        e.clock = e.clock + dt;
        // 003ec710
        for (auto s = e.slots.begin(); s != e.slots.end();) {
            auto& slot = slots_[*s];
            if (!slot.component) fail("a scheduled slot without a component");
            auto& c = *slot.component;
            if (!slot.started && !(e.clock < slot.start)) {
                slot.started = true;
                c.start();
            }
            if (slot.started) c.update(dt);
            if (!(slot.end <= 0.0f) && !(e.clock < slot.end)) c.done = true;
            if (c.done) {
                slot.component.reset();   // vt+8(1)
                slot.started = false;
                --count_;
                s = e.slots.erase(s);
            } else {
                ++s;
            }
        }
        if (e.slots.empty()) e.done = true;
        if (e.done) it = effects_.erase(it);   // vt+8(1): the node list is empty
        else ++it;
    }
}

void EffectService::draw(std::uint32_t flags, std::vector<EffectComponent::Drawn>& out) {
    // 003ec300 -> 003ec5e0
    for (auto& e : effects_)
        for (const auto i : e.slots) {
            auto& slot = slots_[i];
            if (!slot.component) fail("a scheduled slot without a component");
            slot.component->draw(flags, out);
        }
}

void EffectService::destroy_all() {
    // 003ec3e0: vt+8(1) (003ec050 clears the node list); the slots keep
    // their components and the count.
    effects_.clear();
}

const std::array<const char*, EffectTable::kEntries>& EffectTable::names() {
    static const std::array<const char*, kEntries> n{
        "BulFX_bloodent.par", "BulFX_bloodexit.par", "BulFX_Flak1.par", "BulFX_sparks.par", "BulFX_stone1.par",
        "gunsmokebarrel.par", "gunsmoke_m16.par", "FireHuman.par", "FireHumanSmoke.par", "ShatterWindow.par",
        "Explo_FBs_Med1a.par", "Tazer_Shock1.par", "Tazer_Line_glo1.par", "Exp_nfb_lg1a.par", "Water_splsh_lg1.par",
        "Water_splsh_med1.par", "Water_splsh_sm1.par", "Waterfx_human.par", "Waterfx_bullet.par", "Waterfx_feet.par",
        "FogMyanmar", "FogToronto3", "SnowBelarus2", "RainTokyo", "RainLorelei", "BulFX_knifwound",
        "tracer01.tdx", "Corona1.tdx", "flare0.tdx", "flare1.tdx", "flare2.tdx", "flare3.tdx", "flare4.tdx",
        "a_noise1.tdx", "bhole1.tdx", "bhole2.tdx", "bholeSUB.tdx", "blastmark.tdx", "blood_01.tdx",
        "a_ripple1.tdx", "froth_01.tdx"};
    return n;
}

const EffectTable::Entry* EffectTable::find(std::int32_t index) const {
    const auto i = static_cast<std::int32_t>(static_cast<std::int8_t>(index & 0xff));
    if (i < 0 || static_cast<std::size_t>(i) >= kEntries) return nullptr;
    const auto& e = entries[static_cast<std::size_t>(i)];
    return e.loaded ? &e : nullptr;
}

EffectTable load_effect_table(const std::function<std::int32_t(const std::string&)>& request,
                              const std::function<std::int32_t(const std::string&)>& texture) {
    EffectTable t;
    const auto& n = EffectTable::names();
    for (std::size_t i = 0; i < EffectTable::kParticles; ++i) {
        t.entries[i].id = request(n[i]);
        t.entries[i].loaded = true;
    }
    for (std::size_t i = EffectTable::kParticles; i < EffectTable::kEntries; ++i) {
        const auto id = texture(n[i]);
        t.entries[i].id = id;
        t.entries[i].loaded = id >= 0;
        if (!t.entries[i].loaded) fail(std::string("the default texture 00381000 for ") + n[i] + " is not hosted");
    }
    return t;
}

namespace {
// 00373e70(00535d20, 4), then 00178b80(colour, 0.0), 00179540(light, 4,
// colour, position, 3), vtable 004d9200, 00179530(inner) and 00179520(outer)
// (00236a20 and 00231f60). Null when the point pool is full.
std::optional<std::size_t> take_point_light(LevelLights* lights, const ShadowVector& colour,
                                            const std::array<float, 4>& position, float inner, float outer) {
    if (!lights) fail("the light manager 00535d20 (00373e70) is not hosted");
    auto& pool = lights->point;
    std::optional<std::size_t> found;
    for (std::size_t i = 0; i < pool.size(); ++i) {
        if (pool[i].id != -1) continue;
        pool[i].id = static_cast<std::int32_t>(i + 1);
        found = i;
        break;
    }
    if (!found) return std::nullopt;
    auto& l = pool[*found];
    l.intensity = 0.0f;
    l.enabled = false;
    l.aux = 3;
    l.type = 4;
    l.colour = colour;
    l.position = {position[0], position[1], position[2], position[3]};
    const float sum = (colour[0] * colour[0] + colour[1] * colour[1]) + colour[2] * colour[2];
    l.weight = 1.0f / sum;
    l.vtable = level_light_vtable_point;   // 004d9200
    l.inner = inner;                       // 00179530: +10
    l.outer = outer;                       // 00179520: +14
    return found;
}
}  // namespace

MuzzleEffectResult spawn_muzzle_effect(EffectService& service, ParticleSystem* system, LevelLights* lights,
                                       const MuzzleEffectRequest& request) {
    MuzzleEffectResult r;
    auto& effect = service.create(EffectKind::muzzle);
    std::unique_ptr<EffectComponent> light;
    if (request.light) {
        // 00178b80(colour (1, 1, 0.7), 0.0); 00179540(light, 4, colour, frame row 3, 3); inner 0, outer 10
        const ShadowVector colour{1.0f, 1.0f, std::bit_cast<float>(0x3f333333u), 0.0f};
        if (const auto found = take_point_light(lights, colour, request.frame[3], 0.0f, 10.0f)) {
            light = std::make_unique<LightEffectComponent>(*lights, *found);
            r.light = found;
        }
    }
    std::unique_ptr<EffectComponent> particle;
    if (request.id > 0) {
        if (!system) fail("the particle system (003a2dd0) is not hosted");
        std::array<std::uint8_t, 64> m{};
        for (std::size_t row = 0; row < 4; ++row) std::memcpy(m.data() + 16 * row, request.frame[row].data(), 16);
        particle = std::make_unique<ParticleEffectComponent>(*system, request.id, m);
        r.particle = true;
    }
    if (light) service.schedule(effect, std::move(light), 0.0f, std::bit_cast<float>(0x3dcccccdu));
    if (particle) service.schedule(effect, std::move(particle), 0.0f, 0.0f);
    return r;
}

std::optional<std::size_t> spawn_light_effect(EffectService& service, LevelLights* lights,
                                              const std::array<std::array<float, 4>, 4>& frame, float outer, float end) {
    // 00231f00: 003ebfb0(0x18) -> 00231f60(effect, request): 003ec1d0, vtable 004dbb20.
    auto& effect = service.create(EffectKind::light);
    // 00231f60: 00178b80(request colour (1, 1, 1), 0.0), 00179540(light, 4, colour, request +10, 3),
    // 00179530(request +20 = 0), 00179520(request +24 = f12); then 00232050(effect, light, 0.0, request +28).
    const auto found = take_point_light(lights, ShadowVector{1.0f, 1.0f, 1.0f, 0.0f}, frame[3], 0.0f, outer);
    if (found) service.schedule(effect, std::make_unique<LightEffectComponent>(*lights, *found), 0.0f, end);
    return found;
}

bool spawn_particle_effect(EffectService& service, ParticleSystem* system, std::int32_t id,
                           const std::array<std::array<float, 4>, 4>& frame) {
    // 00232460 -> 002324a0
    auto& effect = service.create(EffectKind::spawner);
    if (id < 0) return false;
    if (!system) fail("the particle system (003a2dd0) is not hosted");
    std::array<std::uint8_t, 64> m{};
    for (std::size_t row = 0; row < 4; ++row) std::memcpy(m.data() + 16 * row, frame[row].data(), 16);
    service.schedule(effect, std::make_unique<ParticleEffectComponent>(*system, id, m), 0.0f, 0.0f);
    return true;
}

bool spawn_table_effect(EffectService& service, ParticleSystem* system, const EffectTable& table, std::int32_t index,
                        const std::array<std::array<float, 4>, 4>& frame) {
    const auto* e = table.find(index);
    if (!e) fail("00232120(" + std::to_string(index) + "): the effect table entry is not loaded (read at address 0)");
    return spawn_particle_effect(service, system, e->id, frame);
}

namespace {
// VU results flush denormals to zero (sign kept); each product and sum is one
// binary32 operation (VU_ROUNDING.md).
float vu_flush(float v) { return std::fpclassify(v) == FP_SUBNORMAL ? (std::signbit(v) ? -0.0f : 0.0f) : v; }
float vu_mul(float a, float b) { return vu_flush(a * b); }
float vu_add(float a, float b) { return vu_flush(a + b); }
// acc - s * t: the vopmsub product is rounded but not flushed; only the result is.
float vu_msub(float acc, float s, float t) {
    const float p = s * t;
    return vu_flush(acc - p);
}
using Vec = std::array<float, 4>;
// vopmula.xyz ACC, a, b; vopmsub.xyz d, b, a: a x b; vsubw.w d.w = vf0.w - vf0.w = 0.
Vec vu_cross(const Vec& a, const Vec& b) {
    return {vu_msub(vu_mul(a[1], b[2]), b[1], a[2]), vu_msub(vu_mul(a[2], b[0]), b[2], a[0]),
            vu_msub(vu_mul(a[0], b[1]), b[0], a[1]), 0.0f};
}
// vmul.xyz; vmulax.w / vmadday.w / vmaddz.w: (x*x + y*y) + z*z; vrsqrt Q = 1 /
// sqrt (the VU's maximum for a zero sum); vmulq.xyz (w kept).
Vec vu_normalize(Vec v) {
    const float s = vu_add(vu_add(vu_mul(v[0], v[0]), vu_mul(v[1], v[1])), vu_mul(v[2], v[2]));
    if (!std::isfinite(s)) fail("00232180: a squared length past the float range (the VU's clamp is not reproduced)");
    const float q = s == 0.0f ? std::numeric_limits<float>::max() : vu_flush(vu::rsqrt(s));
    for (std::size_t i = 0; i < 3; ++i) {
        v[i] = vu_mul(v[i], q);
        if (!std::isfinite(v[i])) fail("00232180: a normalized axis past the float range (the VU's clamp is not reproduced)");
    }
    return v;
}
}  // namespace

std::int32_t WoundEffectIds::at(std::uint32_t index) const {
    if (index >= ids.size()) fail("00188760(" + std::to_string(index) + "): past the wound ids 004933d0");
    return ids[index];
}

void normalize_wound_effect_ids(WoundEffectIds& t) {
    // 00391300: lw 004933d0, -1 -> sw zero; a1 = 1..4: -1 -> word(004933d0).
    if (t.ids[0] == -1) t.ids[0] = 0;
    for (std::size_t i = 1; i < 5; ++i)
        if (t.ids[i] == -1) t.ids[i] = t.ids[0];
}

void set_wound_effect_ids(WoundEffectIds& t, const EffectTable& table) {
    // 00136928..00136974: (index into 004933d0, effect table entry).
    constexpr std::array<std::pair<std::uint32_t, std::int32_t>, 4> order{{{1, 0}, {0, 1}, {2, 2}, {3, 3}}};
    for (const auto& [slot, entry] : order) {
        const auto* e = table.find(entry);
        if (!e) fail("001368d0: effect table entry " + std::to_string(entry) + " is not loaded (read at address 0)");
        t.ids[slot & 0xff] = e->id;   // 0038b8b0
    }
}

std::array<std::array<float, 4>, 4> directional_effect_frame(const std::array<float, 4>& point,
                                                             const std::array<float, 4>& d) {
    std::array<std::array<float, 4>, 4> m{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}};
    if (!(d[0] == 0.0f && d[1] == 0.0f)) {
        // 00232258..00232420
        const Vec up{0.0f, 0.0f, 1.0f, 1.0f};
        const Vec a = vu_normalize(vu_cross(d, up));
        const Vec b = vu_normalize(vu_cross(d, a));
        const Vec c = vu_normalize(vu_cross(a, b));
        m[0] = a;
        m[1] = b;
        m[2] = c;
    }
    // 002321f0..0023224c: z < 0 runs 0013ee20 (a transpose of the identity) and
    // 0013edb0 (row 3 is vf0's (0, 0, 0, 1): nonzero, so the frame is kept).
    // 00232428: row 3 = the point with w = 1.0 (sp+dc).
    m[3] = {point[0], point[1], point[2], 1.0f};
    return m;
}

bool spawn_directional_effect(EffectService& service, ParticleSystem* system, std::int32_t id,
                              const std::array<float, 4>& point, const std::array<float, 4>& direction) {
    return spawn_particle_effect(service, system, id, directional_effect_frame(point, direction));
}

}  // namespace omega::particle
