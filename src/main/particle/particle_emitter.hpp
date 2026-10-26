#pragma once
// Particle emitters (particle.cc), Checkpoint 122d (the TORONTO1 office fire):
// the effect instance 003a2dd0 / 003a3180 and its update 003a2400, the type-1
// emitter constructor 003a0ea0 (base 003a1990, curve 0039fc10), the emitter
// update 003a0540 / 003a1760 with the child spawn 003a1450 / 003a26a0 /
// 003a2930, isDead 00397ab0, and the draw 003a0050 up to the VU1 particle
// program 0x29c (particle_vu1.hpp). The type-0 (physics) emitters: the
// constructor 00398e20 and the update 00398b20 (with 00398470); their draw
// 00398590 with the VU1 program 0x170 (Checkpoint 122f). research/EFFECTS.md
// "Checkpoint 122d office fire" and "Checkpoint 122f".
//
// Emitters and instances are kept as the EE objects' byte images (0x180 and
// 0x70 bytes) with the pointer fields held as host links; the emitter list
// (the sentinels at instance +4 / +10) is a std::list in the same order.
#include "particle_def.hpp"
#include "particle_vu1.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <list>
#include <memory>
#include <optional>
#include <vector>

namespace omega::particle {

constexpr std::size_t kEmitterSize = 0x180;   // 003a26a0 / 003a2dd0 allocate 0x180
constexpr std::size_t kInstanceSize = 0x70;
constexpr std::uint32_t kEmitterVtable = 0x004dfb30;  // type-1 emitter (003a0ea0)
constexpr std::uint32_t kType0EmitterVtable = 0x004dfa80;  // type-0 emitter (00398e20, 0x130 bytes; def at +120)
constexpr std::size_t kType0EmitterSize = 0x130;           // 003a27c0 / 003a30e8 allocate 0x130
constexpr std::uint32_t kShatterVtable = 0x004dfb10;     // ShatterWindow emitter (0039f860, 0x180 bytes; def at +120)
constexpr std::size_t kShardSize = 0xb0;                // one 0039cb70 shard (the block at em+124)
constexpr std::uint32_t kParticleTop = 0x100;          // the VU1 TOPS base used here (either buffer gives the same output)
// Checkpoint 132ms2 (stage A2): the snow / rain emitter 0039b3a0 (0x140 bytes,
// def at +100) the weather records build (special[1..3], 003a3000).
constexpr std::uint32_t kRainVtable = 0x004dfaf0;
constexpr std::size_t kRainEmitterSize = 0x140;
constexpr std::size_t kRainDropSize = 0x20;  // pos 0..8, vel c..14, alpha 18, hidden byte 1c

// The random table 0053b6a0: the counter word (each emitter takes the next
// value), then 0x103 floats in [0, 1).
struct RandomTable {
    std::array<std::uint32_t, 0x104> words{};
    // The static constructor 004d54c0 on the boot RNG (seed 1): word 1 + k =
    // mul.s(00131e70 int -> float of the next 0x7fffffff-masked LCG value, 2^-31).
    static RandomTable boot();
    float word(std::size_t k) const;
    // 00398ae0(table, i) points at word 1 + (i & 0xff); LQ aligns it down.
    Qword qword_at(std::uint32_t i) const;
};

// Checkpoint 132ms2: what the rain emitter reads through its camera em+104
// (the object *(00159560(004ffc14))): vt+e4 (the position qword), vt+e8 (the
// matrix; row 1 is the view direction) and the word +60 (3a3310's f13).
struct WeatherCamera {
    std::array<float, 4> position{};
    std::array<std::array<float, 4>, 4> matrix{};
    float word_60 = 0.0f;
};

struct Emitter {
    std::array<std::uint8_t, kEmitterSize> bytes{};
    Emitter* parent = nullptr;            // +74
    ParticleDefinition* def = nullptr;    // +100
    // Checkpoint 132g: the ShatterWindow shard block em+124 (+15c shards of
    // 0xb0 bytes, allocated by 0039cb70); empty = the null pointer.
    std::vector<std::uint8_t> shards;
    // Checkpoint 132ms2: the rain emitter's camera em+104 and its drop pool
    // entry em+108 (an index into ParticleSystem::rain_pool; -1 = null).
    const WeatherCamera* camera = nullptr;
    int pool = -1;

    float f32(std::size_t o) const { float v; std::memcpy(&v, bytes.data() + o, 4); return v; }
    std::int32_t i32(std::size_t o) const { std::int32_t v; std::memcpy(&v, bytes.data() + o, 4); return v; }
    std::uint32_t u32(std::size_t o) const { return static_cast<std::uint32_t>(i32(o)); }
    void set_f32(std::size_t o, float v) { std::memcpy(bytes.data() + o, &v, 4); }
    void set_u32(std::size_t o, std::uint32_t v) { std::memcpy(bytes.data() + o, &v, 4); }
    bool drawn() const { return bytes[0x84] != 0; }  // set by 003a1760, gates the draw
};

struct EffectInstance {
    std::array<std::uint8_t, kInstanceSize> bytes{};
    ParticleDefinition* def = nullptr;  // the slot at +24
    std::list<std::unique_ptr<Emitter>> emitters;  // inst+8 (head) .. inst+10 (tail sentinel)
    int freed = 0;                                 // emitters unlinked by 003a2400 (0036b130)

    float f32(std::size_t o) const { float v; std::memcpy(&v, bytes.data() + o, 4); return v; }
    std::uint32_t u32(std::size_t o) const { std::uint32_t v; std::memcpy(&v, bytes.data() + o, 4); return v; }
    void set_f32(std::size_t o, float v) { std::memcpy(bytes.data() + o, &v, 4); }
    void set_u32(std::size_t o, std::uint32_t v) { std::memcpy(bytes.data() + o, &v, 4); }
    float time() const { return f32(0x50); }
    float scale() const { return f32(0x54); }
    float alpha() const { return f32(0x5c); }
};

// One MSCAL of 003a0050: the particle program's n output qwords.
struct ParticleBatch {
    const Emitter* emitter = nullptr;
    const ParticleDefinition* def = nullptr;
    std::uint32_t stop = 0;   // 0x5e0 plain sprites, 0x8a0 flipped texture, 0x1cc8 heat haze
    std::int32_t pass = 0;    // s4 (the motion-blur pass)
    std::vector<Qword> position;  // vf10: instance-local xyz, size in w
    std::vector<Qword> colour;    // vf13: ftoi0 RGBA
    // TOP+6 / TOP+7 (def+e0 / def+f0 in the 12 qwords uploaded from def+80):
    // the box the sprite programs' batch cull (cull_batch) tests. The loader
    // 003a58e0 zeroes both (write_packet_template, Checkpoint 122e), so the
    // cull tests the emitter origin only.
    Qword bound_min{}, bound_max{};
    // Port-only (checkpoint 132mt3): 1 + the index of its DeferredProgram record while that has not run
    // (ParticleSystem::deferred); 0 for a batch its program has filled.
    std::uint32_t deferred = 0;
};

// Port-only (checkpoint 132mt3, research/THREADING.md step 5): one MSCAL of the draws 003a0050 / 00398590 / 0039c680
// recorded instead of run, so that the VU1 program can run later on a worker thread. The draw writes the emitter and
// definition words and uploads the VU data exactly as without it (the game state stays on the simulation thread in the
// original order); only the program and the read-back of its outputs move. Each of the three programs (0x29c, 0x170,
// 0x208, particle_vu1.hpp) reads nothing but data 0xf..0x20 (the draw's uploads) and TOP..TOP+0xc (the def+80 block,
// the count, the random qword), and writes every register before it reads it (static reading of the three ports;
// tests/particle_deferred_tests.cpp runs them with every other register and data word scrambled and gets the same
// bits), so the recorded words on a fresh Vu1State give the batch the serial draw gives.
struct DeferredProgram {
    enum Kind : std::uint8_t { particle_29c, type0_170, shatter_208 };
    Kind kind = particle_29c;
    std::array<Qword, 0x12> uploads{};  // data 0xf..0x20 at the MSCAL
    std::array<Qword, 0xd> block{};     // data TOP..TOP+0xc at the MSCAL
};
// Runs a recorded MSCAL on this thread's Vu1State (thread_vu1_state: the registers as a fresh state's; the data words
// it does not write are never read) and fills its batch (stop, bounds, positions, colours) as the draw does.
// The ShatterWindow heat-haze stop 0x19a8 throws as in 0039c680. `state` (tests): run on that state instead, with
// only the recorded words written into it.
void run_deferred_program(const DeferredProgram& program, ParticleBatch& batch, Vu1State* state = nullptr);

// The emitter vtable +1c: 003a23f0 (type 1) gives 0; 00397a90 (type 0) gives
// em+e0 (the node matrix captured by 00398b20) once byte +128 is set.
const std::uint8_t* attach_matrix(const Emitter& em);

// Checkpoint 132cp10: 003a2a90(inst), the instance's local box (the globals
// 0053b660 / 0053b670): def+890 / def+8a0 while byte +69 is clear; with an
// attach offset (+69 set, inst+40) the union of that box and the box moved
// by +40 (vadd.xyzw, then vmini.xyz / vmax.xyz from the def box; w the def's).
// Throws (naming 003a2a90) for VU operands the port does not model
// (exponent 255, a VMINI / VMAX of +0 against -0).
struct EffectBounds { std::array<float, 4> lo{}, hi{}; };
EffectBounds effect_bounds(const EffectInstance& inst);
// 00394450..00394504 (the PLR / NIE asset's vt+88 003943a0 when +70 != 1.0):
// x and y of both corners times `scale` (mul.s, flush to zero; z and w kept).
// A product past the float range throws (naming 0039447c).
EffectBounds scale_effect_bounds(EffectBounds b, float scale);

// Checkpoint 132g: what the ShatterWindow setup 0039ef90 reads from the glass
// object (a1) and the other object (a3).
struct ShatterGlass {
    std::array<float, 4> position{};        // glass vt+e4
    std::array<float, 4> other_position{};  // other vt+e4
    // vt+b8 gives P with *(P)+50 a node and *(P+4) a mesh of 4 vertices
    // (+1c == 4): the node rows (node+10, after its vt+58 refresh when byte
    // +50 is set) and the mesh vertices (00182270 / 00231ec0).
    bool quad = false;
    std::array<std::array<float, 4>, 4> node{};
    std::array<std::array<float, 4>, 4> vertices{};
    // Otherwise row 1 of the vt+e8 matrix and the two vt+88 bound qwords.
    std::array<float, 4> matrix_row1{};
    std::array<std::array<float, 4>, 2> bounds{};
    // a2 (the model): null takes def+74; else r = *(model+68) gives
    // *(*(r+4)+14) != 0 ? (s16)**(r+8) : -1.
    bool model = false;
    std::int32_t model_texture = -1;
};

// One 0039b7b0 vertex: the position (0025ca40 / 001325e0), the colour source
// 0053bb30 (0025c9d0) and the st qword (001325e0).
struct ShatterVertex {
    std::array<float, 4> position{}, colour{}, st{};
};
// One 00371200 flush: the ALPHA word at 005392d8 and its vertices (triangles).
struct ShatterFlush {
    const Emitter* emitter = nullptr;
    std::int32_t texture = -1;  // em+170 (the 003820e0 bind)
    std::uint64_t alpha = 0;
    std::vector<ShatterVertex> vertices;
};

// Checkpoint 132ms2: one flush of the rain / snow draw 00399080: the sprite
// flush 00371170 (VU1 program 0x14, GS sprites: each vertex the drop with its
// size in w) or the triangle flush 00371200 (the rain streaks). `def` is the
// definition whose +74 texture the draw binds (003820e0): the splash child's
// for the first sprites, the emitter's own otherwise. Colours are the 0025c9d0
// inputs (128 * c truncated is the GS word), st the 001325e0 qword.
struct WeatherFlush {
    const Emitter* emitter = nullptr;
    const ParticleDefinition* def = nullptr;
    std::uint64_t alpha = 0;
    bool sprites = false;
    std::vector<ShatterVertex> vertices;
};

// Checkpoint 132ms2: the weather wind group 0053b5c0 (0xa0 bytes of BSS):
// rows +0..+3c (row 0 the drift, rows 1..3 the 3a5e30 sample matrix), the
// periods +40..+7c, the drift steps +80..+88, +8c / +90 the init f15 / f16,
// the time +94 and +98 = -1.
struct WeatherGroup {
    std::array<std::uint8_t, 0xa0> bytes{};
    float f32(std::size_t o) const { float v; std::memcpy(&v, bytes.data() + o, 4); return v; }
    void set_f32(std::size_t o, float v) { std::memcpy(bytes.data() + o, &v, 4); }
};
// One entry of the drop pool 0053b560 (4 x {buf, owner, dist, count}).
struct RainPoolEntry {
    std::vector<std::uint8_t> buffer;  // count * 0x20 (null when empty)
    const Emitter* owner = nullptr;
    float distance = 0.0f;
    std::int32_t count = 0;
};

// Checkpoint 132g: game::Shatter 00230de0(glass, other, 0)'s placement of the
// ShatterWindow setup. Two LOS requests (class 005435a0, vtable 004d9180, the
// service 003caa50(004ffb50, 004ea0b0) vt+c): +30 the origin (w forced 1),
// +40 the displacement (w 0), +50 the glass, +54 = 0, +58 = 4 (excluded
// flags), +5c = 0; the answer byte +0 and on a hit the point +10 / normal +50.
struct ShatterLos {
    std::array<float, 4> origin{}, displacement{};
};
struct ShatterLosHit {
    std::array<float, 4> point{}, normal{};
};
struct ShatterPlacement {
    std::array<float, 4> point{}, normal{};  // 003a3420 t0 (sp a60) / t1 (sp 990)
    float floor = 0.0f;                      // f12 (f20)
    std::vector<ShatterLos> queries;
};
// The glass's vt+88 bound qword 0 gives the starting floor (+8); `glass`
// carries the quad (vt+b8 node / vertices) or the fallback (vt+e8 row 1).
ShatterPlacement shatter_placement(const ShatterGlass& glass,
                                   const std::function<std::optional<ShatterLosHit>(const ShatterLos&)>& los);

class ParticleSystem {
public:
    // The loaded definitions in slot order (00234bb0 id -> slot); 003a3430
    // finds them by the 16-byte effectName at +170.
    explicit ParticleSystem(std::vector<ParticleDefinition*> definitions);

    RandomTable& table() { return table_; }
    const RandomTable& table() const { return table_; }
    int find(const std::uint8_t* name16) const;  // 003a3430 (-1 when absent)
    ParticleDefinition* slot(int id) const;      // 00234bb0

    // 003a2dd0(inst, id, matrix, f12 = scale) with its tail 003a3180. The
    // matrix argument is not read for a type-1 / type-0 root emitter (003a0ea0
    // copies def+8b0); the ShatterWindow root (special[0], 0039f860) takes
    // its row 1 (null throws).
    void construct(EffectInstance& inst, int id, float scale, const std::uint8_t* matrix = nullptr);
    // Checkpoint 132g: 003a3420(inst, glass, model, other, &point, &normal,
    // f12 = floor) = 0039ef90 on the instance's first emitter.
    void setup_shatter(EffectInstance& inst, const ShatterGlass& glass, const std::array<float, 4>& point,
                       const std::array<float, 4>& normal, float floor);
    // 0039b7b0(em, 2, f12 = t, ...) for an emitter with a texture (em+170 >=
    // 0): the shards as textured triangles. `random` is 00127dd0.
    void draw_shatter(Emitter& em, std::vector<ShatterFlush>& out);
    // 003a2400(inst, matrix, flag, f12 = dt) with the emitters' vtable +10
    // (003a0540 type 1, 00398b20 type 0). `matrix` is the 64-byte node world
    // (node+10, rows) the type-0 update and the attach offset read; null
    // throws (00394590) only when one of them needs it. flag != 0 (wind)
    // throws.
    void update(EffectInstance& inst, const std::uint8_t* matrix, std::uint32_t flag, float dt);
    // 003a0050(em, 2, f12 = t, f13, f14 = scale, f15 = alpha); `night_vision`
    // is the byte at 0053b540.
    void draw(Emitter& em, float t, float f13, float scale, float alpha, std::vector<ParticleBatch>& out);
    // Checkpoint 122f: the type-0 (physics) emitter draw 00398590(em, 2, f12 =
    // t, f13, f14 = scale, f15 = alpha) with the VU1 program 0x170 and powf
    // 00121b30 (the frame of each motion-blur pass). Writes em+ac / bc / c0..cc
    // and def+920 / 930 / 980..98c like the original.
    void draw_type0(Emitter& em, float t, float f13, float scale, float alpha, std::vector<ParticleBatch>& out);
    // Checkpoint 132cp8: 0039b7b0(em, 2, f12 = t, f13, f14 = scale, f15 =
    // alpha) for a ShatterWindow emitter without a texture (em+170 < 0), the
    // path 0039c680 with the VU1 program 0x208 (run_shatter_program).
    void draw_shatter_vu1(Emitter& em, float t, float f13, float scale, float alpha, std::vector<ParticleBatch>& out);
    // 003a1ad0(inst, flags, matrix): the draw of every emitter whose +84 byte
    // is set and +6c count is non-zero, in list order, with f13 and the
    // instance time / scale / alpha. Flags 0x80 draw nothing. f13 is 255
    // unless flags has 0x10 (the scene walk's cull code: the box reaches the
    // near plane), where 003a1cf4..003a1e04 take the distance fade
    // clamp(renderer +180 + renderer +190 / w, 0, 255), w the clip w of the
    // node origin (renderer +f00 after the node push and 0037d9a0) - the
    // caller's `distance_fade` (shadow_fog_factor's math); unset with 0x10 throws.
    void draw_instance(EffectInstance& inst, std::uint32_t flags, std::vector<ParticleBatch>& out,
                       std::vector<ShatterFlush>* shatter = nullptr, std::optional<float> distance_fade = std::nullopt,
                       std::vector<WeatherFlush>* weather = nullptr);

    // ---- Checkpoint 132ms2: the weather (rain / snow) emitter 0039b3a0 ----
    // 003a5f20(group, 0, 0, f12..f16) (the a1 / a2 words are not read).
    void init_weather_group(float f12, float f13, float f14, float f15, float f16);
    // 003a5c50(group, dt).
    void update_weather_group(float dt);
    // 003a5e30(out, group, x, y, z): (row1 x + row2 y) + row3 z.
    std::array<float, 4> sample_weather_group(float x, float y, float z) const;
    // 0039b210(count): every pool entry gets a new count * 0x20 buffer (the old
    // ones leak in the original; the host's are zero-filled).
    void alloc_rain_pool(std::int32_t count);
    // 0039b110(em, &em+6c, f12 = distance): 2 kept, 1 claimed, 0 none; em+6c = the entry's count (0 for none).
    int claim_rain_pool(Emitter& em, float distance);
    // The drop s of em's pool entry while em owns it (null otherwise).
    std::uint8_t* rain_drop(const Emitter& em, std::int32_t s);
    // 003a3310(inst, f12, f13) / 003a3340(inst, cam, box, a3) / 003a33b0(inst,
    // cam, a2, f12, f13, f14) on the instance's first emitter (its parent +74 if set).
    void set_weather_fraction(EffectInstance& inst, float f12, float f13);
    void set_weather_box(EffectInstance& inst, const WeatherCamera* camera, const std::array<float, 4>& lo,
                         const std::array<float, 4>& hi, std::uint32_t a3);
    void set_weather_range(EffectInstance& inst, const WeatherCamera* camera, std::uint32_t a2, float f12, float f13,
                           float f14);
    // 00399080(em): the rain / snow draw (the pool's drops).
    void draw_rain(Emitter& em, std::vector<WeatherFlush>& out);
    // 003a2ba0(inst, -1) as the records' destructors use it: every emitter's
    // destructor (0039b2f0 releases the rain pool entry), then the list is empty.
    void destroy(EffectInstance& inst);
    WeatherGroup weather_group;                // 0053b5c0
    std::array<RainPoolEntry, 4> rain_pool{};  // 0053b560

    bool night_vision = false;            // byte 0053b540 (0 at boot)
    Qword light{};                        // 00538350..5c, copied to em+a0 (0 at boot)
    // 0053b5a0 / 5a8 / 5b0 / 5b8: the ids 00397b20 stores for the requests
    // "ShatterWindow.par" / "SnowBelarus2.par" / "RainTokyo.par" /
    // "RainLorelei.par" (0 at boot); 003a2dd0 builds their special emitters.
    std::array<std::int32_t, 4> special{};
    // Checkpoint 131c: 003a1760's light sample (applyGameLight, once the emitter
    // time is more than 0.25 past em+68): the manager 00535d20 takes the update
    // matrix's row 3 as its reference (003a17c8..003a183c), 00373320(manager,
    // 0x16, 1, null) gathers, and em+50 = the colour 00537d80 with xyz scaled
    // by def+140..148 / 128. The host returns the colour; unset, the sample is
    // a named stop.
    std::function<std::array<float, 4>(const std::array<float, 4>& point)> game_light;
    Vu1State vu;
    // Checkpoint 132g (ShatterWindow): the frame word 005383c8 the shard hits
    // copy to 0053bb20 (`last_hit`); 00127dd0 (the draw's random shard);
    // the texture slots 00380fb0 adds (setup) and 003813a0 releases (the
    // destructor 0039f740); the draws of shatter children without a
    // texture (em+170 < 0: the VU1 path 0039c680, program 0x208) and their
    // batches are counted.
    std::uint32_t frame = 0;
    std::uint32_t last_hit = 0;
    std::function<std::uint32_t()> random;
    std::vector<std::int32_t> texture_addrefs;
    std::vector<std::int32_t> texture_releases;
    std::uint64_t shatter_particle_draws = 0;
    std::uint64_t shatter_particle_batches = 0;
    // Port-only (checkpoint 132mt3): when set, draw / draw_type0 / draw_shatter_vu1 record each MSCAL here
    // (DeferredProgram) and push its batch with only emitter / def / pass and `deferred` set; the caller runs the records
    // (run_deferred_program) before it reads the batches. The VU1 registers and output words are then not carried
    // from one MSCAL to the next: no program reads them (DeferredProgram).
    std::vector<DeferredProgram>* deferred = nullptr;

private:
    void defer(DeferredProgram::Kind kind, ParticleBatch& batch);
    std::unique_ptr<Emitter> make_base(Emitter* parent, ParticleDefinition* def, float t);
    std::unique_ptr<Emitter> make_type0_emitter(Emitter* parent, ParticleDefinition* def, const std::uint8_t* matrix, float t);
    std::unique_ptr<Emitter> make_emitter(Emitter* parent, ParticleDefinition* def, const std::uint8_t* matrix, float t);
    void insert_sorted(EffectInstance& inst, std::unique_ptr<Emitter> em);
    void spawn(Emitter& em, EffectInstance& inst, float t);
    void update_body(Emitter& em, EffectInstance& inst, const std::uint8_t* matrix, float t);
    void update_emitter(Emitter& em, EffectInstance& inst, const std::uint8_t* matrix, std::uint32_t flag, float t);
    void update_type0(Emitter& em, EffectInstance& inst, const std::uint8_t* matrix, std::uint32_t flag, float t);
    bool dead(const Emitter& em, float t) const;
    std::unique_ptr<Emitter> make_shatter(Emitter* parent, ParticleDefinition* def, const std::uint8_t* matrix, float t);
    void update_shatter(Emitter& em, EffectInstance& inst, const std::uint8_t* matrix, std::uint32_t flag, float t);
    std::unique_ptr<Emitter> make_rain(Emitter* parent, ParticleDefinition* def, const std::uint8_t* matrix, float t);
    void update_rain(Emitter& em, EffectInstance& inst, const std::uint8_t* matrix, std::uint32_t flag, float t);
    Emitter& weather_target(EffectInstance& inst);
    void build_shards(Emitter& em, EffectInstance& inst);
    void release(EffectInstance& inst, Emitter& em);
    Emitter* spawn_child(EffectInstance& inst, Emitter* parent, int id, const std::uint8_t* matrix, float t);

    std::vector<ParticleDefinition*> defs_;
    RandomTable table_;
};

}  // namespace omega::particle
