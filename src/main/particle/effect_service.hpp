#pragma once
// The timed effect service (Checkpoint 131b, research/COMBAT.md "Checkpoint
// 131b"): the 0x18-byte effect objects 003ebfb0 allocates (base constructor
// 003ec1d0, global list 0055c0a8), their schedule slots (pool 0055a890,
// 0x180 x 16 bytes, filled by 00232050), the per-frame update 003ec4c0 (at
// 00133780, after the bullets 00275670), the draw 003ec300 (at 0013e95c) and
// the level-end release 003ec3e0; the scheduled components the weapon
// effects use (the particle component 004dbe90 and the point light 004d9200);
// the effect table 004f7a50 (loaded by 00236ff0, read by 00236f20); and the
// muzzle effect 00236a20.
#include "level_lights.hpp"
#include "particle_asset.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace omega::particle {

// A scheduled component: byte +1 is its done flag, its vtable (at +4) has
// destroy +8 (the C++ destructor here), start +c, stop +10, update +14 and
// draw +18.
class EffectComponent {
public:
    virtual ~EffectComponent() = default;
    bool done = false;                 // +1
    virtual void start() = 0;          // vt+c
    virtual void stop() = 0;           // vt+10 (not called by the scheduler)
    virtual void update(float dt) = 0; // vt+14
    // vt+18: the particle component's sprites with its world rows.
    struct Drawn {
        std::vector<ParticleBatch> batches;
        std::array<std::array<float, 4>, 4> world{};  // rows r0..r3 (r3 = position)
    };
    virtual void draw(std::uint32_t flags, std::vector<Drawn>& out) = 0;
};

// The particle component (vtable 004dbe90, 0x50 bytes): +8 the PAR id, +c the
// instance, +10 the 64-byte world matrix (rows).
//   start  236de0: 0036b180(0x70) and 003a2dd0(inst, id, 1.0, matrix)
//   stop   236d20: 003a2ba0(inst, -1), free, inst = 0
//   update 236cd0: 003a2400(inst, matrix, 0, dt); done when inst+1c is 0
//   draw   236be0: 003a2a90 bounds through the matrix, 00379b20, 003a1ad0
//   destroy 236ea0: stop, then the base vtable and delete
class ParticleEffectComponent final : public EffectComponent {
public:
    ParticleEffectComponent(ParticleSystem& system, std::int32_t id, const std::array<std::uint8_t, 64>& matrix)
        : system_(system), id_(id), matrix_(matrix) {}
    ~ParticleEffectComponent() override { stop(); }
    void start() override;
    void stop() override;
    void update(float dt) override;
    // `flags` stands for the visibility 00379b20(0x5381f0, bounds) returns
    // (host: 2, the clipping is the renderer's, as for the NIE walk 0013e250).
    void draw(std::uint32_t flags, std::vector<Drawn>& out) override;
    std::int32_t id() const { return id_; }
    const EffectInstance* instance() const { return instance_.get(); }

private:
    ParticleSystem& system_;
    std::int32_t id_;
    std::array<std::uint8_t, 64> matrix_;
    std::unique_ptr<EffectInstance> instance_;
};

// A point light (pool entry of 00535d20, vtable 004d9200) as a component:
//   start  1479d0: vt+2c (00144b80, enabled)
//   stop   1479e0: vt+30 (00144980, disabled)
//   update 147a00, draw 147a10: nothing
//   destroy 144a60(1): disabled, id -1 (00373d70), vtable 004d9280
class LightEffectComponent final : public EffectComponent {
public:
    LightEffectComponent(LevelLights& lights, std::size_t index) : lights_(lights), index_(index) {}
    ~LightEffectComponent() override;
    void start() override;
    void stop() override;
    void update(float) override {}
    void draw(std::uint32_t, std::vector<Drawn>&) override {}
    std::size_t index() const { return index_; }

private:
    LevelLights& lights_;
    std::size_t index_;
};

enum class EffectKind : std::uint8_t {
    base,     // 004e0760 (destroy 3ec050)
    muzzle,   // 004dbae0 (destroy 237130), 00236a20
    spawner,  // 004dbb00 (destroy 237190), 00232460
    light,    // 004dbb20 (destroy 2371f0), 00231f60 (game::SpawnLight's 00231f00)
};

class EffectService {
public:
    static constexpr std::size_t kSlots = 0x180;   // pool 0055a890
    struct Slot {
        std::unique_ptr<EffectComponent> component;   // +0 (null: free)
        float start = 0.0f, end = 0.0f;               // +4, +8 (clock-relative when scheduled)
        bool started = false;                         // +c
    };
    struct Effect {
        EffectKind kind = EffectKind::base;
        std::list<std::size_t> slots;   // +0 count, +4 / +8 the list of slot pointers
        bool done = false;              // +c
        float clock = 0.0f;             // +10
        std::uint64_t serial = 0;       // host: creation order (logs)
    };

    // 003ebfb0 + 003ec1d0: a new effect appended to the global list.
    Effect& create(EffectKind kind);
    // 00232050(effect, component, f12 start, f13 end): the first free slot
    // takes the component with start + clock and end + clock, the global
    // count 0055c090 grows and the slot joins the effect's list. Without a
    // free slot nothing happens (the component is leaked by the original;
    // here it is released and counted in `dropped`).
    void schedule(Effect& effect, std::unique_ptr<EffectComponent> component, float start, float end);
    // 003ec4c0(dt): each effect's vt+c (003ec6c0: clock += dt, 003ec710, done
    // when no slot remains), then vt+8(1) and removal for the done ones.
    void update(float dt);
    // 003ec300: each effect's vt+10 (003ec5e0): every slot's component
    // vt+18 in list order (no started test).
    void draw(std::uint32_t flags, std::vector<EffectComponent::Drawn>& out);
    // 003ec3e0: each effect's vt+8(1); the slots stay as they are.
    void destroy_all();

    std::size_t effects() const { return effects_.size(); }
    std::int32_t scheduled() const { return count_; }   // 0055c090
    std::uint64_t dropped = 0;
    const std::array<Slot, kSlots>& slots() const { return slots_; }
    const std::list<Effect>& list() const { return effects_; }

private:
    std::array<Slot, kSlots> slots_{};
    std::list<Effect> effects_;
    std::int32_t count_ = 0;
    std::uint64_t serial_ = 0;
};

// The effect table 004f7a50 (41 x {+0 id, +4 loaded byte}) that 00236ff0
// fills during the PLR load 001c1ce0: entries 0..25 request their PAR names
// (00398070, resolved by 00397b20), entries 26..40 load their TDX textures
// from %s\COMMON\SPECFX.HOG (00380a40; 00381000 when that fails).
struct EffectTable {
    static constexpr std::size_t kEntries = 0x29;
    static constexpr std::size_t kParticles = 0x1a;
    static const std::array<const char*, kEntries>& names();   // 0048acd0 (26) and 0048ad40 (15)
    struct Entry { std::int32_t id = 0; bool loaded = false; };
    std::array<Entry, kEntries> entries{};
    // 00236f20(i): i as a signed byte; null unless the loaded byte is set
    // and i < 0x29.
    const Entry* find(std::int32_t index) const;
};
// 00236ff0: `request` is 00398070 (+ 00397b20) for a PAR name, `texture` is
// 00380a40 for a SPECFX.HOG member (the texture slot, negative when absent).
// A failed texture load takes the 00381000 handle, which is not hosted:
// that throws.
EffectTable load_effect_table(const std::function<std::int32_t(const std::string&)>& request,
                              const std::function<std::int32_t(const std::string&)>& texture);

// 00236a20(effect, {+0 flag, +4 the request frame, +8 id}) on a new muzzle
// effect (vtable 004dbae0): with the flag, a point light from 00373e70(4)
// (00179540 colour (1, 1, 0.7, 0), position frame row 3, aux 3; vtable
// 004d9200; inner 0 (00179530), outer 10 (00179520)) scheduled over
// [0, 0.1]; with id > 0 a particle component of the frame scheduled over
// [0, 0]. The light is skipped when the point pool is full.
struct MuzzleEffectRequest {
    bool light = false;
    std::array<std::array<float, 4>, 4> frame{};   // rows 0..3 (row 3 the muzzle)
    std::int32_t id = 0;
};
struct MuzzleEffectResult {
    std::optional<std::size_t> light;   // point pool index
    bool particle = false;
};
MuzzleEffectResult spawn_muzzle_effect(EffectService& service, ParticleSystem* system, LevelLights* lights,
                                       const MuzzleEffectRequest& request);

// 00231f00(frame, f12 outer, f13 end) (Checkpoint 132sx5, game::SpawnLight
// 00230610 -> 00230660): the request {+0 colour (1, 1, 1), +10 frame row 3,
// +20 inner 0, +24 f12, +28 f13} for a new light effect (003ebfb0(0x18),
// 00231f60: 003ec1d0, vtable 004dbb20): a point light from 00373e70(4)
// (00178b80 colour w = 0.0; 00179540(light, 4, colour, row 3, 3); vtable
// 004d9200; 00179530 inner 0, 00179520 outer f12) scheduled over [0, f13]
// (00232050). Without a free point entry the effect stays empty. Returns the
// point pool index.
std::optional<std::size_t> spawn_light_effect(EffectService& service, LevelLights* lights,
                                              const std::array<std::array<float, 4>, 4>& frame, float outer, float end);

// 00232460(id, frame): a spawner effect (vtable 004dbb00, 003ec1d0) and,
// with id >= 0, a particle component (0x50 bytes from 003ec9f0, vtable
// 004dbe90, +0 the byte at 0048ad80, +8 id, +10 the frame) scheduled over
// [0, 0]. Returns whether the component was scheduled.
bool spawn_particle_effect(EffectService& service, ParticleSystem* system, std::int32_t id,
                           const std::array<std::array<float, 4>, 4>& frame);
// 00232120(index, frame): 00232460(*00236f20(index), frame). An entry
// 00236f20 refuses is read at address 0 by the original: that throws.
bool spawn_table_effect(EffectService& service, ParticleSystem* system, const EffectTable& table, std::int32_t index,
                        const std::array<std::array<float, 4>, 4>& frame);

// Checkpoint 132b: the wound effect ids 004933d0 (five words; static data
// -1, -1, -1, -1, 0) that the hit volumes 00188140 read through 00188760(i)
// (word(004933d0 + 4 i)): entry 1 (2 for part code 0xc, 3 for 0x10) at the
// entry wound, entry 0 at the exit wound.
struct WoundEffectIds {
    std::array<std::int32_t, 5> ids{-1, -1, -1, -1, 0};
    // 00188760(i); i outside 0..4 reads past the table: that throws.
    std::int32_t at(std::uint32_t index) const;
};
// 00391300's head (Expression_Init, at 001c1f50 in the level load 001c1e70
// and at 0022a308): a -1 entry 0 becomes 0, then every -1 entry 1..4 takes
// entry 0. (Its tail, 003ffa70 on 004bc3f0 / 004bc400 into 0053b340.., is the
// morph expressions' and not part of the table.)
void normalize_wound_effect_ids(WoundEffectIds& ids);
// 001368d0's tail (00136928..00136974, after the PAR.HOG load at 001357e0,
// which follows the level load 001c4190 and so the effect table 00236ff0):
// 0038b8b0(1, *00236f20(0)), 0038b8b0(0, *00236f20(1)), 0038b8b0(2,
// *00236f20(2)), 0038b8b0(3, *00236f20(3)); 0038b8b0(i, id) stores
// word(004933d0 + 4 (i & 0xff)) = id. An entry 00236f20 refuses is read at
// address 0 by the original: that throws.
void set_wound_effect_ids(WoundEffectIds& ids, const EffectTable& table);

// 00232180(id, point, direction)'s frame: identity rows (vf0, pcpyud / pexcw)
// with row 3 = (point.xyz, 1). Unless direction x and y are both 0 (c.eq.s):
// up = (0, 0, 1, 1), a = normalize(direction x up), b = normalize(direction x
// a), c = normalize(a x b) (vopmula / vopmsub, w = vf0.w - vf0.w; vmul /
// vmulax / vmadday / vmaddz, vrsqrt, vmulq.xyz), rows 0..2 = a, b, c. With x
// and y 0 and z < 0, 0013ee20 transposes the identity and 0013edb0 finds row 3
// still (0, 0, 0, 1) (the point is stored after), so the rows stay the
// identity; the 00133ec0 / 00132020 / 0013ed90 / 0013ed60 / 00131e70 block
// after it is unreachable.
std::array<std::array<float, 4>, 4> directional_effect_frame(const std::array<float, 4>& point,
                                                             const std::array<float, 4>& direction);
// 00232180: 00232460(id, directional_effect_frame(point, direction)).
bool spawn_directional_effect(EffectService& service, ParticleSystem* system, std::int32_t id,
                              const std::array<float, 4>& point, const std::array<float, 4>& direction);

}  // namespace omega::particle
