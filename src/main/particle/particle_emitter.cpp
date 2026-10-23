#include "particle_emitter.hpp"

#include "../WaterFx/water_effects.hpp"
#include "game_libm.hpp"
#include "vu_float.hpp"

#include <bit>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace omega::particle {
namespace {

[[noreturn]] void unhosted(std::uint32_t address, const char* what) {
    char b[160];
    std::snprintf(b, sizeof b, "particle runtime: unhosted %s (%08x)", what, address);
    throw std::runtime_error(b);
}

// EE COP1 results flush denormals to signed zero (tools/ee/ee.py ftz).
float ftz(float x) {
    if (x != 0.0f && std::fabs(x) < FLT_MIN) return std::copysign(0.0f, x);
    return x;
}
float add_s(float a, float b) { return ftz(a + b); }
float sub_s(float a, float b) { return ftz(a - b); }
float mul_s(float a, float b) { return ftz(a * b); }
float div_s(float a, float b) {
    if (b == 0.0f) return std::copysign(FLT_MAX, (a == 0.0f ? 1.0f : a) * b);
    return ftz(a / b);
}
float cvt_s_w(std::int32_t v) { return static_cast<float>(v); }
// The batch words a particle program leaves (TOP+6 / TOP+7, the count at TOP+0xb, n qwords vf10 at TOP+0xc and n
// qwords vf13 after them), read back by 003a0050 / 00398590 / 0039c680.
void read_batch(const Vu1State& vu, ParticleBatch& batch) {
    batch.bound_min = vu.data[kParticleTop + 6];
    batch.bound_max = vu.data[kParticleTop + 7];
    const std::uint32_t count = vu.data[kParticleTop + 0xb][0] & 0x7fff;
    batch.position.reserve(count);
    batch.colour.reserve(count);
    for (std::uint32_t k = 0; k < count; ++k) batch.position.push_back(vu.data[(kParticleTop + 0xc + k) & 0x3ff]);
    for (std::uint32_t k = 0; k < count; ++k) batch.colour.push_back(vu.data[(kParticleTop + 0xc + count + k) & 0x3ff]);
}
constexpr const char* kShatterHazeStop = "the ShatterWindow heat-haze sprite program (VU1 0x208 -> 0x19a8)";

std::int32_t cvt_w_s(float f) {
    const double v = f;
    if (v >= 2147483647.0) return 2147483647;
    if (v <= -2147483648.0) return INT32_MIN;
    return static_cast<std::int32_t>(v);
}
// The unsigned word -> float idiom (srl / andi / or / cvt.s.w / add.s) of
// 003a0ea0 for def+60 and def+a3c.
float u32_to_float(std::uint32_t u) {
    if (static_cast<std::int32_t>(u) >= 0) return cvt_s_w(static_cast<std::int32_t>(u));
    const float f = cvt_s_w(static_cast<std::int32_t>((u >> 1) | (u & 1)));
    return add_s(f, f);
}

using V4 = std::array<float, 4>;

V4 load4(const std::uint8_t* p) {
    V4 v;
    std::memcpy(v.data(), p, 16);
    return v;
}
void store4(std::uint8_t* p, const V4& v) { std::memcpy(p, v.data(), 16); }

// COP2 macro mode as tools/ee/ee.py models it: products and sums round to
// binary32, every stored register (and the ACC) flushes denormals.
V4 mula(const V4& s, float t) {
    V4 r;
    for (int k = 0; k < 4; ++k) r[k] = ftz(s[k] * t);
    return r;
}
V4 madda(const V4& acc, const V4& s, float t) {
    V4 r;
    for (int k = 0; k < 4; ++k) r[k] = ftz(acc[k] + s[k] * t);
    return r;
}

// 0x4bc560 as 00131f00 builds it on the first 0039fc10 (rows (3,-6,3,0),
// (-3,3,0,0), (1,0,0,0), 0).
constexpr std::array<V4, 4> kCurveBasis{{{3.0f, -6.0f, 3.0f, 0.0f}, {-3.0f, 3.0f, 0.0f, 0.0f},
                                          {1.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f}}};

// The vmulax / vmadday / vmaddaz / vmaddw chain: rows[0]*v.x + rows[1]*v.y + rows[2]*v.z + rows[3]*v.w.
V4 transform4(const std::array<V4, 4>& rows, const V4& v) {
    V4 acc = mula(rows[0], v[0]);
    acc = madda(acc, rows[1], v[1]);
    acc = madda(acc, rows[2], v[2]);
    return madda(acc, rows[3], v[3]);
}

// The vmulax / vmadday / vmaddz .xyz chain; w keeps the input's w.
V4 transform3(const V4& r0, const V4& r1, const V4& r2, const V4& v) {
    V4 acc = mula(r0, v[0]);
    acc = madda(acc, r1, v[1]);
    V4 out = madda(acc, r2, v[2]);
    out[3] = v[3];
    return out;
}

// vmul.xyz / vmulax.w / vmadday.w / vmaddz.w / vsqrt / vaddq.x.
float length3(const V4& v) {
    float sq[3];
    for (int k = 0; k < 3; ++k) sq[k] = ftz(v[k] * v[k]);
    float acc = ftz(1.0f * sq[0]);
    acc = ftz(acc + 1.0f * sq[1]);
    const float w = ftz(acc + 1.0f * sq[2]);
    const float q = ftz(static_cast<float>(std::sqrt(std::fabs(static_cast<double>(w)))));
    return ftz(0.0f + q);
}

V4 sub4(const V4& a, const V4& b) {
    V4 r;
    for (int k = 0; k < 4; ++k) r[k] = ftz(a[k] - b[k]);
    return r;
}

bool same_name(const std::uint8_t* a, const std::uint8_t* b) { return std::memcmp(a, b, 16) == 0; }

// ---- Checkpoint 132g (ShatterWindow) vector forms ----
V4 add4(const V4& a, const V4& b) {
    V4 r;
    for (int k = 0; k < 4; ++k) r[k] = ftz(a[k] + b[k]);
    return r;
}
// vmulx.xyzw (0017f670 / 001e5920).
V4 scale4(const V4& a, float s) {
    V4 r;
    for (int k = 0; k < 4; ++k) r[k] = ftz(a[k] * s);
    return r;
}
// vmulax.xyzw ACC, vf0, vf0x; vmsubw.xyzw d, v, vf0w: 0 - v in every lane.
V4 neg4(const V4& a) {
    V4 r;
    for (int k = 0; k < 4; ++k) r[k] = ftz(0.0f - a[k]);
    return r;
}
// vopmula.xyz ACC, a, b; vopmsub.xyz d, b, a; vsubw.w d, vf0, vf0 (001320f0 /
// 0039eef0): a x b with w = 0 (the product subtracted is rounded, the result flushed).
V4 cross4(const V4& a, const V4& b) {
    const auto msub = [](float acc, float s, float t) { return ftz(acc - s * t); };
    return {msub(ftz(a[1] * b[2]), b[1], a[2]), msub(ftz(a[2] * b[0]), b[2], a[0]),
            msub(ftz(a[0] * b[1]), b[0], a[1]), 0.0f};
}
// The vmul.xyz / vmulax.w / vmadday.w / vmaddz.w sum (x*x + y*y) + z*z.
float square3(const V4& v) {
    float acc = ftz(1.0f * ftz(v[0] * v[0]));
    acc = ftz(acc + 1.0f * ftz(v[1] * v[1]));
    return ftz(acc + 1.0f * ftz(v[2] * v[2]));
}
// The vmul.xyzw / vmulax.w / vmadday.w / vmaddz.w dot of xyz.
float dot3(const V4& a, const V4& b) {
    V4 p;
    for (int k = 0; k < 4; ++k) p[k] = ftz(a[k] * b[k]);
    float acc = ftz(1.0f * p[0]);
    acc = ftz(acc + 1.0f * p[1]);
    return ftz(acc + 1.0f * p[2]);
}
// vrsqrt Q, vf0w, s; vmulq.xyz (00132030 / 00132060; the VU's maximum for a zero sum), w kept.
V4 normalize4(V4 v) {
    const float s = square3(v);
    if (!std::isfinite(s)) unhosted(0x132030, "a squared length past the float range (the VU clamp)");
    const float q = s == 0.0f ? FLT_MAX : ftz(vu::rsqrt(s));
    for (int k = 0; k < 3; ++k) v[k] = ftz(v[k] * q);
    return v;
}
// 001d10c0(v, &len): vsqrt; vaddq.x len = 0 + Q; vdiv Q = 1 / len; vmulq.xyz.
V4 normalize_len(V4 v, float& len) {
    len = length3(v);
    const float q = len == 0.0f ? FLT_MAX : ftz(vu::div(1.0f, len));
    for (int k = 0; k < 3; ++k) v[k] = ftz(v[k] * q);
    return v;
}
// 00132130(m, x, y, z, angle): the axis-angle rotation (00121928 sinf, 00121688 cosf).
std::array<V4, 4> axis_angle(float x, float y, float z, float angle) {
    const float s = game_sinf(angle);
    const float c = game_cosf(angle);
    const float f3 = sub_s(1.0f, c);
    const float f9 = mul_s(y, mul_s(f3, x));
    const float f8 = mul_s(z, mul_s(f3, y));
    const float f7 = mul_s(x, mul_s(f3, z));
    const float sz = mul_s(s, z), sy = mul_s(s, y), sx = mul_s(s, x);
    std::array<V4, 4> m{};
    m[0] = {add_s(c, mul_s(f3, mul_s(x, x))), add_s(f9, sz), sub_s(f7, sy), 0.0f};
    m[1] = {sub_s(f9, sz), add_s(c, mul_s(f3, mul_s(y, y))), add_s(f8, sx), 0.0f};
    m[2] = {add_s(f7, sy), sub_s(f8, sx), add_s(c, mul_s(f3, mul_s(z, z))), 0.0f};
    m[3] = {0.0f, 0.0f, 0.0f, 1.0f};
    return m;
}
constexpr V4 kUp{0.0f, 0.0f, 1.0f, 1.0f};  // 0x4bc550
// 0x4bc1b0..0x4bc1c8: the GS ALPHA words 0039b7b0 indexes by alphaMode (def+38).
constexpr std::uint64_t kShatterAlpha[4] = {0x0000008000000044ull, 0x0000008000000068ull, 0x0000008000000048ull,
                                            0x0000008000000042ull};
constexpr std::array<V4, 4> kIdentity{{{1.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f, 0.0f},
                                       {0.0f, 0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}}};

std::array<std::uint8_t, 64> matrix_bytes(const std::array<V4, 4>& m) {
    std::array<std::uint8_t, 64> b{};
    for (int r = 0; r < 4; ++r) std::memcpy(b.data() + 16 * r, m[static_cast<std::size_t>(r)].data(), 16);
    return b;
}

}  // namespace

RandomTable RandomTable::boot() {
    RandomTable t;
    std::uint64_t state = 1;
    t.words[0] = 0;
    for (std::size_t k = 1; k < t.words.size(); ++k) {
        // cvt.s.w rounds to nearest here (ee.py; the golden table needs it, the
        // truncating game_int_to_float misses word 2).
        const float f = cvt_s_w(static_cast<std::int32_t>(next_game_random(state)));
        t.words[k] = std::bit_cast<std::uint32_t>(mul_s(f, 4.656612873077393e-10f));  // 2^-31
    }
    return t;
}

float RandomTable::word(std::size_t k) const { return std::bit_cast<float>(words.at(k)); }

Qword RandomTable::qword_at(std::uint32_t i) const {
    const std::size_t w = (1 + (i & 0xff)) & ~std::size_t{3};
    return {words[w], words[w + 1], words[w + 2], words[w + 3]};
}

ParticleSystem::ParticleSystem(std::vector<ParticleDefinition*> definitions)
    : defs_(std::move(definitions)), table_(RandomTable::boot()) {}

int ParticleSystem::find(const std::uint8_t* name16) const {
    for (std::size_t k = 0; k < defs_.size(); ++k)
        if (same_name(defs_[k]->bytes.data() + def::effect_name, name16)) return static_cast<int>(k);
    return -1;
}

ParticleDefinition* ParticleSystem::slot(int id) const {
    if (id < 0 || static_cast<std::size_t>(id) >= defs_.size()) unhosted(0x234bb0, "definition id");
    return defs_[static_cast<std::size_t>(id)];
}

// 003a0ea0(em, parent, def, matrix, f12 = t) with the base 003a1990.
std::unique_ptr<Emitter> ParticleSystem::make_base(Emitter* parent, ParticleDefinition* d, float t) {
    auto em = std::make_unique<Emitter>();
    Emitter& e = *em;
    e.bytes.fill(0);  // host allocation; every byte the golden reads is written below
    // 003a1990
    e.set_u32(0x00, kEmitterVtable);
    e.bytes[0x0c] = 0;
    e.set_u32(0x60, 0);
    e.set_f32(0x64, t);
    e.set_u32(0x80, 0);
    e.bytes[0x84] = 0;
    e.parent = parent;
    for (std::int32_t i = 0; i < d->i32(def::num_children); ++i) e.bytes[0x78 + static_cast<std::size_t>(i)] = 0;
    table_.words[0] += 1;
    e.set_u32(0x70, table_.words[0]);
    e.set_f32(0x68, sub_s(sub_s(t, 0.25f), 1.0f));
    const float r = table_.word(4 + e.bytes[0x70]);
    const float extra = add_s(0.5f, mul_s(d->f32(def::num_particles_mask), r));
    e.set_u32(0x6c, static_cast<std::uint32_t>(d->i32(def::num_particles)) + static_cast<std::uint32_t>(cvt_w_s(extra)));
    return em;
}

// 00398e20(em, parent, def, matrix, f12 = t): the emitterType-0 emitter
// (0x130 bytes, vtable 004dfa80); 0013ab00 over +a0..+e0 does nothing.
std::unique_ptr<Emitter> ParticleSystem::make_type0_emitter(Emitter* parent, ParticleDefinition* d,
                                                            const std::uint8_t* matrix, float t) {
    std::unique_ptr<Emitter> em = make_base(parent, d, t);
    Emitter& e = *em;
    e.set_u32(0x00, kType0EmitterVtable);
    e.def = d;  // +120
    e.set_u32(0x124, static_cast<std::uint32_t>(d->i32(0x9c8) - 1));  // framesToEmit - 1
    if (d->bytes[0x9cc] != 0) e.set_u32(0x124, 0x7fffffffu);            // emitForever
    if (!parent) {
        std::memcpy(e.bytes.data() + 0x10, d->bytes.data() + def::matrix, 0x40);
    } else {  // 00398ee0: the matrix rows through the parent's +10..+4f (vmulax / madday / maddaz / maddw)
        const std::array<V4, 4> p{load4(parent->bytes.data() + 0x10), load4(parent->bytes.data() + 0x20),
                                  load4(parent->bytes.data() + 0x30), load4(parent->bytes.data() + 0x40)};
        for (int row = 0; row < 4; ++row) store4(e.bytes.data() + 0x10 + 0x10 * row, transform4(p, load4(matrix + 0x10 * row)));
    }
    e.set_u32(0x9c, d->u32(def::size));
    std::memcpy(e.bytes.data() + 0xa0, light.data(), 16);
    std::memcpy(e.bytes.data() + 0xb0, e.bytes.data() + 0x30, 16);
    e.set_f32(0x60, add_s(d->f32(0x9b8), d->f32(0x9bc)));  // life + lifeMask
    e.bytes[0x128] = 0;
    return em;
}

std::unique_ptr<Emitter> ParticleSystem::make_emitter(Emitter* parent, ParticleDefinition* d, const std::uint8_t* matrix, float t) {
    std::unique_ptr<Emitter> em = make_base(parent, d, t);
    Emitter& e = *em;
    // 003a0ea0
    e.def = d;
    if (!parent) {
        std::memcpy(e.bytes.data() + 0x10, d->bytes.data() + def::matrix, 0x40);
    } else {
        const std::array<V4, 4> p{load4(parent->bytes.data() + 0x10), load4(parent->bytes.data() + 0x20),
                                  load4(parent->bytes.data() + 0x30), load4(parent->bytes.data() + 0x40)};
        for (int row = 0; row < 4; ++row) store4(e.bytes.data() + 0x10 + 0x10 * row, transform4(p, load4(matrix + 0x10 * row)));
    }
    std::memcpy(e.bytes.data() + 0x90, e.bytes.data() + 0x40, 16);
    e.set_u32(0x9c, d->u32(def::size));
    std::memcpy(e.bytes.data() + 0xb0, e.bytes.data() + 0x30, 16);
    e.set_f32(0xbc, u32_to_float(d->u32(def::flip_texture)));
    std::memcpy(e.bytes.data() + 0xa0, light.data(), 16);
    {
        const float cycles = u32_to_float(d->u32(def::num_cycles));
        const float span = add_s(d->f32(def::cycle_time), d->f32(def::cycle_variation));
        e.set_f32(0x60, add_s(1.0f, mul_s(cycles, span)));
    }
    const V4 r0 = load4(e.bytes.data() + 0x10), r1 = load4(e.bytes.data() + 0x20), r2 = load4(e.bytes.data() + 0x30);
    std::array<V4, 3> pk;
    for (int k = 0; k < 3; ++k) {
        pk[k] = transform3(r0, r1, r2, load4(d->bytes.data() + def::control_points + 0x10 * k));
        store4(e.bytes.data() + 0x110 + 0x10 * k, pk[k]);
    }
    std::memset(e.bytes.data() + 0x140, 0, 0x30);
    e.set_f32(0x170, length3(pk[0]));
    e.set_f32(0x174, length3(sub4(pk[1], pk[0])));
    e.set_f32(0x178, length3(sub4(pk[2], pk[1])));
    // 0039fc10(em, p0..p2, em+d0): the Bezier coefficients per axis.
    for (int k = 0; k < 3; ++k) {
        const V4 v{pk[0][k], pk[1][k], pk[2][k], 0.0f};
        V4 c = transform4(kCurveBasis, v);
        c[3] = 0.0f;
        store4(e.bytes.data() + 0xd0 + 0x10 * k, c);
    }
    return em;
}

// 003a2930 with fp = 0: before the first emitter whose definition's sort
// influence (+5c, compared as signed words) is not above the new one's, else
// at the tail.
void ParticleSystem::insert_sorted(EffectInstance& inst, std::unique_ptr<Emitter> em) {
    const std::int32_t key = em->def->i32(def::sort_influence);
    auto it = inst.emitters.begin();
    for (; it != inst.emitters.end(); ++it)
        if (!(key < (*it)->def->i32(def::sort_influence))) break;
    inst.emitters.insert(it, std::move(em));
    inst.set_u32(0x1c, inst.u32(0x1c) + 1);
}

// 0039f860(em, parent, def, matrix, f12 = t): the ShatterWindow emitter (0x180
// bytes, vtable 004dfb10, def at +120). The root (parent null) copies def+8b0
// and takes the matrix's row 1 as its normal +b0; a child transforms the child
// matrix rows through the parent's +10..+4f and copies the parent's +b0 and
// the xyz of +e0.
std::unique_ptr<Emitter> ParticleSystem::make_shatter(Emitter* parent, ParticleDefinition* d, const std::uint8_t* matrix,
                                                      float t) {
    if (!matrix) unhosted(0x39f860, "the ShatterWindow emitter (0x180 bytes) of an effect instance without a matrix");
    std::unique_ptr<Emitter> em = make_base(parent, d, t);
    Emitter& e = *em;
    e.set_u32(0x00, kShatterVtable);
    e.def = d;  // +120
    e.bytes[0x0c] = 1;
    if (!parent) {
        std::memcpy(e.bytes.data() + 0x10, d->bytes.data() + def::matrix, 0x40);
        std::memcpy(e.bytes.data() + 0xb0, matrix + 0x10, 16);
    } else {  // 0039f8f0: vmulax / madday / maddaz / maddw per row
        const std::array<V4, 4> p{load4(parent->bytes.data() + 0x10), load4(parent->bytes.data() + 0x20),
                                  load4(parent->bytes.data() + 0x30), load4(parent->bytes.data() + 0x40)};
        for (int row = 0; row < 4; ++row) store4(e.bytes.data() + 0x10 + 0x10 * row, transform4(p, load4(matrix + 0x10 * row)));
        std::memcpy(e.bytes.data() + 0xb0, parent->bytes.data() + 0xb0, 16);
        std::memcpy(e.bytes.data() + 0xe0, parent->bytes.data() + 0xe0, 12);  // lq / sq with +ec kept
    }
    std::memcpy(e.bytes.data() + 0x90, e.bytes.data() + 0x40, 16);
    e.set_u32(0x9c, d->u32(def::size));
    std::memcpy(e.bytes.data() + 0xa0, light.data(), 16);
    {  // the 00398470 form on a3: (0, 0, 1, 1) through the transposed upper 3x3, w kept
        const V4 m0 = load4(matrix), m1 = load4(matrix + 0x10), m2 = load4(matrix + 0x20);
        const V4 t0{m0[0], m1[0], m2[0], m0[3]}, t1{m0[1], m1[1], m2[1], m1[3]}, t2{m0[2], m1[2], m2[2], m2[3]};
        store4(e.bytes.data() + 0xd0, transform3(t0, t1, t2, V4{0.0f, 0.0f, 1.0f, 1.0f}));
    }
    e.set_f32(0xdc, cvt_s_w(d->u8(def::heat_haze)));
    e.set_f32(0x60, add_s(add_s(add_s(d->f32(0x9b0), d->f32(0x9b4)), d->f32(0x9b8)), d->f32(0x9bc)));
    e.set_u32(0x170, 0xffffffffu);
    e.set_u32(0x124, 0);
    e.shards.clear();
    e.bytes[0x16c] = 0;
    return em;
}

ShatterPlacement shatter_placement(const ShatterGlass& glass,
                                   const std::function<std::optional<ShatterLosHit>(const ShatterLos&)>& los) {
    ShatterPlacement out;
    // 00231024..002310ac: sp ab0 = the glass - other direction (vrsqrt, w kept).
    const V4 dir = normalize4(sub4(glass.position, glass.other_position));
    V4 origin, n;
    if (glass.quad) {
        // 00231104..00231250: the mesh vertices' mean ((v0 + v1) + v2) + v3, x 0.25 (sp a50).
        const auto& v = glass.vertices;
        const V4 c = scale4(add4(add4(add4(v[0], v[1]), v[2]), v[3]), 0.25f);
        // 00231260..0023132c: the last corner below the mean and left of it (x + y), from 1.
        std::size_t pick = 0;
        for (std::size_t i = 1; i < 4; ++i)
            if (v[i][2] < c[2] && add_s(v[i][0], v[i][1]) < add_s(c[0], c[1])) pick = i;
        // 00231358..00231424: the farthest corner from it (s6) and the previous farthest (fp).
        const V4 lo = v[pick];
        std::size_t s6 = 3 - pick, fp = 4;
        float far = 0.0f;
        for (std::size_t i = 0; i < 4; ++i) {
            const float d = square3(sub4(lo, v[i]));
            if (!(d <= far)) {
                fp = s6;
                far = d;
                s6 = i;
            }
        }
        if (fp == 4) unhosted(0x231408, "a shatter quad whose corners all coincide (fp never written)");
        // 002314a8..00231578: through the node rows (node+10; a node with byte +50 set refreshes
        // through vt+58 first, 00231498: the caller passes refreshed rows).
        const V4 a80 = transform4(glass.node, lo), a70 = transform4(glass.node, v[s6]),
                 a40 = transform4(glass.node, v[fp]);
        origin = scale4(add4(a80, a70), 0.5f);                                // sp aa0
        n = normalize4(cross4(sub4(a80, a40), sub4(a70, a40)));             // sp a90 (w 0)
        // 002316e8: sp a60 = the node-transformed mean, overwritten below in every path.
    } else {
        // 00231730..002317dc: vt+e4, row 1 of vt+e8; the three vt+88 qwords (a70 / a80 / a60) are dead.
        origin = glass.position;
        n = glass.matrix_row1;
    }
    if (dot3(n, dir) < 0.0f) n = neg4(n);  // 002317e0..0023185c
    const auto query = [&](const V4& o, const V4& d) {
        ShatterLos q;
        q.origin = o;
        q.origin[3] = 1.0f;  // +3c
        q.displacement = d;
        q.displacement[3] = 0.0f;  // +4c
        out.queries.push_back(q);
        return los(q);
    };
    // 00231860..00231984: the first request along 40 x the normal.
    const auto hit1 = query(origin, scale4(n, 40.0f));
    out.floor = glass.bounds[0][2];  // 00231998: (vt+88)+8
    float step = 3.0f;
    if (hit1) {
        // 002319b0..00231a58: min.s(3, 0.5 x the hit distance).
        out.point = hit1->point;
        out.normal = hit1->normal;
        step = std::min(3.0f, mul_s(0.5f, length3(sub4(out.point, origin))));
    } else {
        // 00231a60..00231b00
        out.point = add4(origin, scale4(n, 40.0f));
        out.normal = neg4(n);
    }
    // 00231b08..00231b34: a second request straight down 200 unless the first hit a near-horizontal face.
    if (!hit1 || std::fabs(out.normal[2]) < 0.9f) {
        origin = add4(origin, scale4(n, step));
        const auto hit2 = query(origin, V4{0.0f, 0.0f, -200.0f, 0.0f});
        if (hit2) {
            out.floor = hit2->point[2];  // 00231c5c
        } else {
            out.point = origin;  // 00231c60..00231ca4
            out.normal = neg4(n);
        }
    }
    return out;
}

void ParticleSystem::setup_shatter(EffectInstance& inst, const ShatterGlass& glass, const std::array<float, 4>& point,
                                   const std::array<float, 4>& normal, float floor) {
    // 003a3420: 0039ef90 on the instance's first emitter.
    if (inst.emitters.empty() || inst.emitters.front()->u32(0x00) != kShatterVtable)
        unhosted(0x3a3420, "the shatter setup of an instance whose first emitter is not a ShatterWindow emitter");
    Emitter& e = *inst.emitters.front();
    ParticleDefinition& d = *e.def;
    store4(e.bytes.data() + 0x100, point);
    store4(e.bytes.data() + 0x110, normal);
    e.set_f32(0x154, floor);
    store4(e.bytes.data() + 0xf0, normalize4(sub4(glass.position, glass.other_position)));
    V4 n;
    if (glass.quad) {
        // 0039f0a0..0039f51c: the node-transformed vertices, their mean c, the
        // corner below c and left of it (+130), the farthest corner from it
        // (+140) and the one before (sp240).
        std::array<V4, 4> w;
        for (std::size_t k = 0; k < 4; ++k) w[k] = transform4(glass.node, glass.vertices[k]);
        const V4 c = scale4(add4(add4(add4(w[0], w[1]), w[2]), w[3]), 0.25f);
        const float fc = add_s(c[0], c[1]);
        std::size_t a1 = 0;
        for (std::size_t a0 = 1; a0 < 4; ++a0)
            if (w[a0][2] < c[2] && add_s(w[a0][0], w[a0][1]) < fc) a1 = a0;
        const V4 lo = w[a1];
        std::size_t t4 = 3 - a1, s1 = 0;
        float f1 = 0.0f;
        for (std::size_t t3 = 0; t3 < 4; ++t3) {
            const float dist = square3(sub4(lo, w[t3]));
            if (!(dist <= f1)) {
                s1 = t4;
                f1 = dist;
                t4 = t3;
            }
        }
        const V4 third = w[s1], hi = w[t4];
        const V4 span = sub4(hi, lo);
        std::memcpy(e.bytes.data() + 0xe0, span.data(), 12);  // +ec kept
        n = normalize4(cross4(sub4(lo, third), sub4(hi, third)));  // before the centring (0039f42c)
        store4(e.bytes.data() + 0x130, sub4(lo, c));
        store4(e.bytes.data() + 0x140, sub4(hi, c));
    } else {
        // 0039f528: vt+e8 row 1; vt+88 twice for the bound qwords.
        n = glass.matrix_row1;
        store4(e.bytes.data() + 0x140, glass.bounds[1]);
        store4(e.bytes.data() + 0x130, glass.bounds[0]);
        const V4 span = sub4(glass.bounds[1], glass.bounds[0]);
        std::memcpy(e.bytes.data() + 0xe0, span.data(), 12);
    }
    if (!(dot3(n, load4(e.bytes.data() + 0xf0)) <= 0.0f)) n = neg4(n);
    store4(e.bytes.data() + 0xb0, n);
    const std::int32_t slot = glass.model ? glass.model_texture : d.i32(0x74);
    e.set_u32(0x170, static_cast<std::uint32_t>(slot));
    if (slot >= 0) texture_addrefs.push_back(slot);  // 00380fb0
}

// 0039cb70 from its entry to the init 0039e5cc: the shard grid. +160 / +164 /
// +168 = 4; the 0x20-byte points (position, uv) are allocated one by one
// with their table and freed after the cells (26 frees), the shard block
// (+15c shards of 0xb0 bytes) stays at +124.
void ParticleSystem::build_shards(Emitter& em, EffectInstance& inst) {
    em.set_u32(0x160, 4);
    em.set_u32(0x164, 4);
    em.set_u32(0x168, 4);
    const V4 a = load4(em.bytes.data() + 0x130), b = load4(em.bytes.data() + 0x140);
    const V4 mid = scale4(add4(a, b), 0.5f);
    const V4 e = load4(em.bytes.data() + 0xe0);
    const std::int32_t rows = em.i32(0x164);
    const std::int32_t sp180 = (rows + 1) >> 1;
    std::int32_t s3;
    float f28;
    if (e[0] < e[1]) {
        s3 = em.i32(0x168);
        f28 = div_s(sub_s(mid[1], a[1]), e[1]);
    } else {
        s3 = em.i32(0x160);
        f28 = div_s(sub_s(mid[0], a[0]), e[0]);
    }
    const std::int32_t s2 = (s3 + 1) >> 1;
    const std::int32_t R = s3 + 1;
    em.set_u32(0x158, static_cast<std::uint32_t>(R * (rows + 1)));
    em.set_u32(0x15c, static_cast<std::uint32_t>(s3 * rows * 2));
    const std::int32_t count = em.i32(0x15c);
    em.shards.assign(static_cast<std::size_t>(count) * kShardSize, 0);
    struct Point {
        V4 pos{}, uv{};
    };
    std::vector<Point> P(static_cast<std::size_t>(em.i32(0x158)));
    const float f27 = div_s(sub_s(mid[2], a[2]), e[2]);
    // 0053bb10..1c: 0.75 in each lane (set on the first use).
    const V4 base{0.75f, 0.75f, 0.75f, 0.75f};
    auto put = [&](std::int32_t i, const V4& pos, const V4& uv) {
        if (i < 0 || static_cast<std::size_t>(i) >= P.size()) unhosted(0x39cd00, "a shard grid point outside +158");
        P[static_cast<std::size_t>(i)] = {pos, uv};
    };
    for (std::int32_t s1 = 0; s1 <= s2; ++s1) {
        bool fp = false;
        float f24 = 0, f23 = 0, f22 = 0, f21 = 0, f26 = 0, f25 = 0, f20 = 0;
        if (s1 == 0) {
            f24 = a[0]; f23 = b[0]; f22 = a[1]; f21 = b[1]; f26 = 0.0f; f25 = 1.0f;
        } else if (s1 == s2) {
            f24 = mid[0]; f23 = mid[0]; f22 = mid[1]; f21 = mid[1]; f26 = f28; f25 = f28;
        } else {
            fp = true;
            f20 = std::sqrt(div_s(cvt_s_w(s1), cvt_s_w(s2)));
        }
        for (std::int32_t s0 = 0; s0 <= sp180; ++s0) {
            const std::int32_t s6 = s0 * R, s7 = R * (rows - s0);
            table_.words[0] += 1;
            const Qword q = table_.qword_at(table_.words[0]);
            V4 qv;
            for (int k = 0; k < 4; ++k) qv[static_cast<std::size_t>(k)] = std::bit_cast<float>(q[static_cast<std::size_t>(k)]);
            const V4 r = add4(base, scale4(qv, 0.5f));
            float f1, f0, f3, f2;
            if (s0 == 0) {
                f1 = a[2]; f0 = b[2]; f3 = 0.0f; f2 = 1.0f;
            } else if (s0 == sp180) {
                f1 = mid[2]; f0 = mid[2]; f3 = f27; f2 = f27;
            } else {
                const float s = std::sqrt(div_s(cvt_s_w(s0), cvt_s_w(sp180)));
                f3 = mul_s(f27, mul_s(s, r[2]));
                f2 = sub_s(1.0f, mul_s(sub_s(1.0f, f27), mul_s(s, r[3])));
                f1 = add_s(a[2], mul_s(e[2], f3));
                f0 = add_s(a[2], mul_s(e[2], f2));
            }
            if (fp) {
                f26 = mul_s(f28, mul_s(f20, r[0]));
                f25 = sub_s(1.0f, mul_s(sub_s(1.0f, f28), mul_s(f20, r[1])));
                f24 = add_s(a[0], mul_s(e[0], f26));
                f23 = add_s(a[0], mul_s(e[0], f25));
                f22 = add_s(a[1], mul_s(e[1], f26));
                f21 = add_s(a[1], mul_s(e[1], f25));
            }
            put(s6 + s1, {f24, f22, f0, 1.0f}, {f26, f2, 1.0f, 0.0f});
            put(s7 + s3 - s1, {f23, f21, f1, 1.0f}, {f25, f3, 1.0f, 0.0f});
            if (s1 < s2) {
                put(s6 + s3 - s1, {f23, f21, f0, 1.0f}, {f25, f2, 1.0f, 0.0f});
                put(s7 + s1, {f24, f22, f1, 1.0f}, {f26, f3, 1.0f, 0.0f});
            }
        }
    }
    // The cells: eight triangles per (i, j); each shard keeps the centroid
    // (+10) and the corners relative to it (+40 / +50 / +60, 001ae380) with
    // their uvs (+70 / +80 / +90).
    auto tri = [&](std::int32_t shard, std::int32_t A, std::int32_t B, std::int32_t C) {
        if (shard < 0 || shard >= count) unhosted(0x39dd00, "a shard index outside +15c");
        for (std::int32_t v : {A, B, C})
            if (v < 0 || static_cast<std::size_t>(v) >= P.size()) unhosted(0x39dd00, "a shard corner outside +158");
        std::uint8_t* s = em.shards.data() + static_cast<std::size_t>(shard) * kShardSize;
        const Point &pa = P[static_cast<std::size_t>(A)], &pb = P[static_cast<std::size_t>(B)], &pc = P[static_cast<std::size_t>(C)];
        const V4 c = scale4(add4(add4(pa.pos, pb.pos), pc.pos), std::bit_cast<float>(0x3eaaaaabu));
        store4(s + 0x40, sub4(pa.pos, c));
        store4(s + 0x50, sub4(pb.pos, c));
        store4(s + 0x60, sub4(pc.pos, c));
        store4(s + 0x70, pa.uv);
        store4(s + 0x80, pb.uv);
        store4(s + 0x90, pc.uv);
        store4(s + 0x10, c);
    };
    for (std::int32_t j = 0; j < sp180; ++j)
        for (std::int32_t i = 0; i < s2; ++i) {
            const std::int32_t s0 = i + j * R, s6 = s0 + sp180 * R;
            const std::int32_t s7 = 2 * (i + j * s3), fp = s7 + 2 * sp180 * s3;
            tri(s7, s0, s0 + R, s0 + R + 1);
            tri(s7 + 1, s0, s0 + 1, s0 + R + 1);
            tri(s7 + 2 * s2, s0 + s2, s0 + s2 + 1, s0 + s2 + R);
            tri(s7 + 2 * s2 + 1, s0 + s2 + 1, s0 + s2 + R + 1, s0 + s2 + R);
            tri(fp, s6, s6 + 1, s6 + R);
            tri(fp + 1, s6 + 1, s6 + R, s6 + R + 1);
            tri(fp + 2 * s2, s6 + s2, s6 + s2 + R + 1, s6 + s2 + R);
            tri(fp + 2 * s2 + 1, s6 + s2, s6 + s2 + 1, s6 + s2 + R + 1);
        }
    inst.freed += static_cast<int>(P.size()) + 1;  // each point, then the table (0036b130)
    em.set_u32(0x124, 1);                        // the block pointer (masked by the goldens)

    // 0039e5cc: the shard velocities (+20), spin axes (+30), gravity (+0) and friction (+4).
    const V4 h = scale4(load4(em.bytes.data() + 0xe0), 0.5f);
    const float f20 = mul_s(0.5f, div_s(1.0f, length3(h)));
    const V4 nb = load4(em.bytes.data() + 0xb0);
    for (std::int32_t k = 0; k < count; ++k) {
        std::uint8_t* s = em.shards.data() + static_cast<std::size_t>(k) * kShardSize;
        table_.words[0] += 1;
        const Qword q = table_.qword_at(table_.words[0]);
        V4 r;
        for (std::size_t l = 0; l < 4; ++l) r[l] = sub_s(std::bit_cast<float>(q[l]), 0.5f);
        r = add4(r, r);
        float len = 0.0f;
        const V4 n = normalize_len(load4(s + 0x10), len);
        V4 axis = neg4(cross4(nb, n));  // 0039eef0, then vmulax ACC, vf0, vf0x / vmsubw
        axis[3] = 0.0f;                 // 0039e794: the angle +3c starts at 0
        store4(s + 0x30, axis);
        const float kk = mul_s(len, f20);
        const V4 v = sub4(scale4(n, kk), scale4(nb, sub_s(1.0f, kk)));  // 0017f670 twice, 001ae380
        const float f21 = add_s(em.def->f32(0x998), mul_s(em.def->f32(0x99c), r[0]));
        store4(s + 0x20, scale4(v, f21));  // 001e5920
        const float gravity = add_s(em.def->f32(0x9a0), mul_s(em.def->f32(0x9a4), r[2]));
        const float friction = add_s(em.def->f32(0x9a8), mul_s(em.def->f32(0x9ac), r[3]));
        std::memcpy(s + 0x00, &gravity, 4);
        std::memcpy(s + 0x04, &friction, 4);
        s[0xa0] = 0;
    }
}

// The hit matrix of 0039ea98..0039ec50: identity when the shard reached the
// floor or the normal +110 is exactly 0x4bc550's xyz; otherwise the rows
// (|up x n|, |up|, row0 x row1, (0, 0, 0, 1)) (001320f0 / 00132030 / 00132060 /
// 00131f00). A cross product of zero takes 00131c70 (not hosted).
static std::array<V4, 4> shatter_hit_rows(const V4& normal) {
    if (normal[0] == kUp[0] && normal[1] == kUp[1] && normal[2] == kUp[2]) return kIdentity;
    const V4 c = cross4(kUp, normal);
    if (c[0] == 0.0f && c[1] == 0.0f && c[2] == 0.0f)
        unhosted(0x131c70, "the shard hit matrix of a normal parallel to 0x4bc550");
    const V4 r0 = normalize4(c);
    const V4 r1 = normalize4(kUp);
    return {r0, r1, cross4(r0, r1), V4{0.0f, 0.0f, 0.0f, 1.0f}};
}

// 003a26a0(inst, parent, id, matrix, f12 = t) with the insert 003a2930 (fp = 0).
Emitter* ParticleSystem::spawn_child(EffectInstance& inst, Emitter* parent, int id, const std::uint8_t* matrix, float t) {
    ParticleDefinition* cd = slot(id);
    std::unique_ptr<Emitter> made;
    if (cd->u8(def::type) == 1) {
        made = make_emitter(parent, cd, matrix, t);
    } else if (parent && parent->bytes[0x0c] != 0) {  // 003a27b0 -> 003a2880
        made = make_shatter(parent, cd, matrix, inst.f32(0x50));
    } else {
        // 003a27c0: 0x130 bytes, f12 = inst+50; fp = 1 when the parent's
        // vtable +1c is non-zero (003a2860).
        if (parent && attach_matrix(*parent)) unhosted(0x3a2878, "the child insert under an attached type-0 emitter (fp = 1)");
        made = make_type0_emitter(parent, cd, matrix, inst.f32(0x50));
    }
    Emitter* child = made.get();
    insert_sorted(inst, std::move(made));
    return child;
}

// 0039cb70(em, inst, matrix, wind, f12 = t): the ShatterWindow update.
void ParticleSystem::update_shatter(Emitter& em, EffectInstance& inst, const std::uint8_t* matrix, std::uint32_t flag, float t) {
    em.bytes[0x16c] = 1;
    if (em.bytes[0x84] == 0) {
        em.bytes[0x16c] = 0;
        if (em.i32(0x170) >= 0) build_shards(em, inst);
        // 0039e8e8: the point and the floor relative to the matrix row 3.
        if (!matrix) unhosted(0x39e8e8, "the node matrix (node+10) of the ShatterWindow update");
        float m[3];
        std::memcpy(m, matrix + 0x30, 12);
        em.set_f32(0x100, sub_s(em.f32(0x100), m[0]));
        em.set_f32(0x104, sub_s(em.f32(0x104), m[1]));
        em.set_f32(0x108, sub_s(em.f32(0x108), m[2]));
        em.set_f32(0x154, sub_s(em.f32(0x154), m[2]));
    }
    // 0039e938: the shards.
    if (em.i32(0x170) >= 0) {
        ParticleDefinition& d = *em.def;
        const V4 normal = load4(em.bytes.data() + 0x110);
        for (std::int32_t k = 0; k < em.i32(0x15c); ++k) {
            std::uint8_t* s = em.shards.data() + static_cast<std::size_t>(k) * kShardSize;
            if (s[0xa0] != 0) continue;
            em.bytes[0x16c] = 0;
            store4(s + 0x10, add4(load4(s + 0x10), load4(s + 0x20)));
            const float one = 1.0f;
            std::memcpy(s + 0x1c, &one, 4);
            const V4 dir = normalize4(sub4(load4(s + 0x10), load4(em.bytes.data() + 0x100)));  // 00183380, 00132030
            float z, floor = em.f32(0x154);
            std::memcpy(&z, s + 0x18, 4);
            const bool on_floor = z < floor;
            if (on_floor || dot3(dir, normal) < 0.0f) {
                s[0xa0] = 1;
                last_hit = frame;  // 005383c8 -> 0053bb20
                for (std::int32_t c = 0; c < d.i32(def::num_children); ++c) {
                    const std::size_t off = kChildStride * static_cast<std::uint32_t>(c);
                    if (!(d.f32(def::child_trigger_time + off) < 0.0f)) continue;
                    V4 pos = load4(s + 0x10);
                    std::array<V4, 4> rows = on_floor ? kIdentity : shatter_hit_rows(normal);
                    if (on_floor)
                        pos[2] = add_s(floor, 0.5f);
                    else
                        pos = add4(pos, normal);
                    pos[3] = 1.0f;
                    rows[3] = pos;
                    const int id = find(d.bytes.data() + def::child_name + off);
                    if (id < 0) unhosted(0x3a3430, "the shard hit child effect name");
                    const auto mb = matrix_bytes(rows);
                    em.bytes[0x0c] = 0;
                    spawn_child(inst, &em, id, mb.data(), t);
                    em.bytes[0x0c] = 1;
                    if (inst.bytes[0x68] == 0) {
                        inst.bytes[0x68] = 1;
                        store4(inst.bytes.data() + 0x30, pos);
                    }
                }
            }
            store4(s + 0x20, scale4(load4(s + 0x20), [&] { float f; std::memcpy(&f, s + 4, 4); return f; }()));
            float vz, g, angle;
            std::memcpy(&vz, s + 0x28, 4);
            std::memcpy(&g, s + 0x00, 4);
            vz = add_s(vz, g);
            std::memcpy(s + 0x28, &vz, 4);
            std::memcpy(&angle, s + 0x3c, 4);
            angle = add_s(angle, div_s(length3(load4(s + 0x20)), 4.0f));
            std::memcpy(s + 0x3c, &angle, 4);
        }
    }
    // 0039ed98: 003a1760, the wind and the cycle.
    update_body(em, inst, matrix, t);
    if (flag != 0 && !(em.def->f32(def::wind) <= 0.01f)) unhosted(0x39edf4, "ShatterWindow emitter wind (0019f2a0)");
    if (em.def->u32(def::cycle_forever) != 0 && !(sub_s(t, em.f32(0x64)) <= em.f32(0x60))) {
        em.set_f32(0x64, t);
        std::memcpy(em.bytes.data() + 0x90, em.bytes.data() + 0x40, 16);
    }
}

// 0039f740 (the destructor) before the emitter free: 003813a0 releases the
// texture slot, the shard block is freed.
void ParticleSystem::release(EffectInstance& inst, Emitter& em) {
    if (em.u32(0x00) == kRainVtable) {  // 0039b2f0: the first pool entry em owns is freed (owner = 0)
        for (auto& e : rain_pool)
            if (e.owner == &em) {
                e.owner = nullptr;
                break;
            }
        return;
    }
    if (em.u32(0x00) != kShatterVtable) return;
    if (em.i32(0x170) >= 0) texture_releases.push_back(em.i32(0x170));
    if (em.u32(0x124) != 0) {
        em.shards.clear();
        em.set_u32(0x124, 0);
        ++inst.freed;
    }
}

// 0039b7b0(em, 2, f12 = t, ...) with em+170 >= 0.
void ParticleSystem::draw_shatter(Emitter& em, std::vector<ShatterFlush>& out) {
    if (em.i32(0x170) < 0) unhosted(0x39c680, "the ShatterWindow VU1 particle draw (em+170 < 0) in the triangle path (draw_shatter_vu1)");
    if (!random) unhosted(0x127dd0, "the game RNG of the ShatterWindow draw");
    const std::int32_t mode = em.def->i32(def::alpha_mode);
    if (mode < 0 || mode > 3) unhosted(0x39b850, "an alphaMode outside the ALPHA table 0x4bc1b0..0x4bc1c8");
    const std::uint32_t rnd = random();
    const std::int32_t count = em.i32(0x15c);
    const float f2 = div_s(cvt_s_w(static_cast<std::int32_t>(rnd)), add_s(1.0f, 2147483648.0f));
    const std::int32_t pick = cvt_w_s(add_s(0.0f, mul_s(cvt_s_w(count - 1), f2)));
    const V4 colour{1.0f, 1.0f, 1.0f, 1.0f};  // 0053bb30 (set on the first draw)
    auto shard_vertices = [&](const std::uint8_t* s, std::vector<ShatterVertex>& v) {
        float a[4];
        std::memcpy(a, s + 0x30, 16);
        std::array<V4, 4> m = axis_angle(a[0], a[1], a[2], a[3]);  // 00132130
        const V4 centre = load4(s + 0x10);
        m[3] = centre;
        for (std::size_t c = 0; c < 3; ++c) {
            const V4 p = add4(transform4(m, load4(s + 0x40 + 0x10 * c)), centre);
            const V4 uv = load4(s + 0x70 + 0x10 * c);
            v.push_back({p, colour, {add_s(uv[0], 2048.0f), add_s(uv[1], 2048.0f), uv[2], uv[3]}});  // 0025f220 = 2048
        }
    };
    ShatterFlush f{&em, em.i32(0x170), kShatterAlpha[mode], {}};
    for (std::int32_t k = 0; k < count; ++k) {
        const std::uint8_t* s = em.shards.data() + static_cast<std::size_t>(k) * kShardSize;
        if (s[0xa0] != 0 || k == pick) continue;
        shard_vertices(s, f.vertices);
    }
    if (!f.vertices.empty()) out.push_back(std::move(f));  // 00371200
    if (pick < 0 || pick >= count) unhosted(0x39c178, "the random shard outside +15c");
    const std::uint8_t* s = em.shards.data() + static_cast<std::size_t>(pick) * kShardSize;
    if (s[0xa0] == 0) {
        ShatterFlush g{&em, em.i32(0x170), kShatterAlpha[1], {}};  // 0039c1a4: ld 0x4bc1b8
        shard_vertices(s, g.vertices);
        out.push_back(std::move(g));
    }
}

// ---- Checkpoint 132ms2 (stage A2): the weather emitter 0039b3a0 / 0039a000 / 00399080 ----

// 0039b3a0(em, parent, def, matrix, f12 = t): 0x140 bytes, vtable 004dfaf0, def at +100.
std::unique_ptr<Emitter> ParticleSystem::make_rain(Emitter* parent, ParticleDefinition* d, const std::uint8_t* matrix,
                                                   float t) {
    if (!matrix) unhosted(0x39b3a0, "the rain / snow emitter (0x140 bytes) of an effect instance without a matrix");
    std::unique_ptr<Emitter> em = make_base(parent, d, t);
    Emitter& e = *em;
    e.set_u32(0x00, kRainVtable);
    e.def = d;  // +100
    if (!parent) {
        std::memcpy(e.bytes.data() + 0x10, d->bytes.data() + def::matrix, 0x40);
        std::memcpy(e.bytes.data() + 0xb0, matrix + 0x10, 16);
    } else {
        const std::array<V4, 4> p{load4(parent->bytes.data() + 0x10), load4(parent->bytes.data() + 0x20),
                                  load4(parent->bytes.data() + 0x30), load4(parent->bytes.data() + 0x40)};
        for (int row = 0; row < 4; ++row) store4(e.bytes.data() + 0x10 + 0x10 * row, transform4(p, load4(matrix + 0x10 * row)));
        std::memcpy(e.bytes.data() + 0xb0, parent->bytes.data() + 0xb0, 16);
        std::memcpy(e.bytes.data() + 0xe0, parent->bytes.data() + 0xe0, 12);
    }
    std::memcpy(e.bytes.data() + 0x90, e.bytes.data() + 0x40, 16);
    e.set_u32(0x9c, d->u32(def::size));
    std::memcpy(e.bytes.data() + 0xa0, light.data(), 16);
    {  // (0, 0, 1, 1) through the transposed upper 3x3 of the matrix, w kept
        const V4 m0 = load4(matrix), m1 = load4(matrix + 0x10), m2 = load4(matrix + 0x20);
        const V4 t0{m0[0], m1[0], m2[0], m0[3]}, t1{m0[1], m1[1], m2[1], m1[3]}, t2{m0[2], m1[2], m2[2], m2[3]};
        store4(e.bytes.data() + 0xd0, transform3(t0, t1, t2, kUp));
    }
    e.set_f32(0xdc, cvt_s_w(d->u8(def::heat_haze)));
    e.set_f32(0x60, add_s(add_s(add_s(d->f32(0x9b0), d->f32(0x9b4)), d->f32(0x9b8)), d->f32(0x9bc)));
    e.camera = nullptr;  // +104
    e.set_u32(0x104, 0);
    e.set_u32(0x12c, 0xffffffffu);
    for (std::int32_t i = 0; i < d->i32(def::num_children); ++i) {
        const std::size_t c = kChildStride * static_cast<std::uint32_t>(i);
        if (d->f32(def::child_trigger_time + c) < 0.0f)  // the splash child (the last one wins)
            e.set_u32(0x12c, static_cast<std::uint32_t>(find(d->bytes.data() + def::child_name + c)));
    }
    e.pool = -1;  // +108
    e.set_u32(0x108, 0);
    return em;
}

// 003a5f20(group, a1, a2, f12..f16).
void ParticleSystem::init_weather_group(float f12, float f13, float f14, float f15, float f16) {
    WeatherGroup& g = weather_group;
    const float k = std::bit_cast<float>(0x3cc83fb7u);
    g.set_f32(0x80, mul_s(k, f12));
    g.set_f32(0x84, mul_s(k, f13));
    g.set_f32(0x88, mul_s(k, f14));
    g.set_f32(0x8c, f15);
    g.set_f32(0x90, f16);
    std::memset(g.bytes.data() + 0x98, 0xff, 4);  // sw -1 (the word, not a float)
    const float s = mul_s(2.0f, f16);
    for (std::size_t j = 0; j < 4; ++j) {
        table_.words[0] += 1;
        const Qword q = table_.qword_at(table_.words[0]);
        V4 v;
        std::memcpy(v.data(), q.data(), 16);
        const V4 row = add4(scale4(sub4(v, V4{0.5f, 0.5f, 0.5f, 0.0f}), s), V4{f15, f15, f15, 0.0f});
        store4(g.bytes.data() + 0x40 + 0x10 * j, row);
    }
    g.set_f32(0x94, 0.0f);
}

// 003a5c50(group, dt).
void ParticleSystem::update_weather_group(float dt) {
    WeatherGroup& g = weather_group;
    const float pi = std::bit_cast<float>(0x40490fdbu);
    for (std::size_t j = 0; j < 4; ++j)
        for (std::size_t a = 0; a < 3; ++a) {
            const std::size_t o = 0x10 * j + 4 * a;
            const float f22 = div_s(mul_s(pi, mul_s(2.0f, g.f32(0x94))), g.f32(0x40 + o));
            const float s1 = game_sinf(f22);
            const float s0 = game_sinf(mul_s(0.5f, f22));
            g.set_f32(o, mul_s(mul_s(s0, s1), g.f32(0x88)));
        }
    g.set_f32(0x0, add_s(g.f32(0x0), g.f32(0x80)));
    g.set_f32(0x4, add_s(g.f32(0x4), g.f32(0x84)));
    g.set_f32(0x8, mul_s(g.f32(0x8), std::bit_cast<float>(0x3dcccccdu)));
    g.set_f32(0x94, add_s(g.f32(0x94), dt));
}

std::array<float, 4> ParticleSystem::sample_weather_group(float x, float y, float z) const {
    const auto& g = weather_group.bytes;
    return add4(add4(scale4(load4(g.data() + 0x10), x), scale4(load4(g.data() + 0x20), y)), scale4(load4(g.data() + 0x30), z));
}

void ParticleSystem::alloc_rain_pool(std::int32_t count) {
    if (count < 0) unhosted(0x39b210, "a negative drop count");
    for (auto& e : rain_pool) {
        e.buffer.assign(static_cast<std::size_t>(count) * kRainDropSize, 0);
        e.count = count;
    }
}

int ParticleSystem::claim_rain_pool(Emitter& em, float distance) {
    int result = 0;
    std::int32_t count = 0;
    if (em.pool >= 0 && rain_pool[static_cast<std::size_t>(em.pool)].owner == &em) {
        auto& e = rain_pool[static_cast<std::size_t>(em.pool)];
        e.distance = distance;
        count = e.count;
        result = 2;
    } else {
        em.pool = -1;
        for (std::size_t i = 0; i < rain_pool.size(); ++i) {
            auto& e = rain_pool[i];
            if (e.buffer.empty()) continue;
            if (e.owner == nullptr || distance < add_s(5.0f, e.distance)) {
                e.owner = &em;
                e.distance = distance;
                em.pool = static_cast<int>(i);
                count = e.count;
                result = 1;
                break;
            }
        }
    }
    em.set_u32(0x6c, static_cast<std::uint32_t>(count));
    return result;
}

std::uint8_t* ParticleSystem::rain_drop(const Emitter& em, std::int32_t s) {
    if (em.pool < 0) return nullptr;
    auto& e = rain_pool[static_cast<std::size_t>(em.pool)];
    if (e.owner != &em) return nullptr;
    const std::size_t at = static_cast<std::size_t>(s) * kRainDropSize;
    if (s < 0 || at + kRainDropSize > e.buffer.size()) unhosted(0x39a2c0, "a drop past the pool entry's buffer");
    return e.buffer.data() + at;
}

Emitter& ParticleSystem::weather_target(EffectInstance& inst) {
    if (inst.emitters.empty()) unhosted(0x3a3310, "a weather setter on an instance without emitters");
    Emitter& first = *inst.emitters.front();
    Emitter& e = first.parent ? *first.parent : first;
    if (e.u32(0x00) != kRainVtable) unhosted(0x3a3310, "a weather setter on an emitter that is not 0039b3a0's");
    return e;
}

void ParticleSystem::set_weather_fraction(EffectInstance& inst, float f12, float f13) {
    Emitter& e = weather_target(inst);
    e.set_f32(0x120, f12);
    e.set_f32(0x124, f13);
}

void ParticleSystem::set_weather_box(EffectInstance& inst, const WeatherCamera* camera, const std::array<float, 4>& lo,
                                     const std::array<float, 4>& hi, std::uint32_t a3) {
    Emitter& e = weather_target(inst);
    e.camera = camera;
    e.bytes[0x130] = a3 == 0 ? 1 : 0;
    e.set_f32(0xe0, sub_s(hi[0], lo[0]));
    e.set_f32(0xe4, sub_s(hi[1], lo[1]));
    e.set_f32(0xe8, sub_s(hi[2], lo[2]));
    e.set_f32(0x11c, lo[2]);
    e.bytes[0x131] = 0;
}

void ParticleSystem::set_weather_range(EffectInstance& inst, const WeatherCamera* camera, std::uint32_t a2, float f12,
                                       float f13, float f14) {
    Emitter& e = weather_target(inst);
    e.camera = camera;
    e.bytes[0x130] = a2 == 0 ? 1 : 0;
    e.set_f32(0x10c, f12);
    e.set_f32(0x110, f13);
    e.set_f32(0x114, f14);
    e.set_f32(0x118, mul_s(0.5f, f13));
    e.set_f32(0xe0, f13);
    e.set_f32(0xe4, f13);
    e.set_f32(0xe8, f13);
    e.bytes[0x131] = 1;
}

namespace {
float msub_s(float acc, float s, float t) { return ftz(acc - s * t); }
float min_s(float a, float b) { return b < a ? b : a; }
float max_s(float a, float b) { return b > a ? b : a; }
float drop_f(const std::uint8_t* d, std::size_t o) { float v; std::memcpy(&v, d + o, 4); return v; }
void set_drop_f(std::uint8_t* d, std::size_t o, float v) { std::memcpy(d + o, &v, 4); }
}  // namespace

// 0039a000(em, inst, matrix, wind, f12 = t).
void ParticleSystem::update_rain(Emitter& em, EffectInstance& inst, const std::uint8_t* matrix, std::uint32_t flag, float t) {
    if (!em.camera) return;
    update_body(em, inst, matrix, t);
    if (!matrix) unhosted(0x39a050, "the node matrix (node+10) of the rain update");
    const WeatherCamera& cam = *em.camera;
    ParticleDefinition& d = *em.def;
    const V4 pos = load4(matrix + 0x30);
    const V4 eye = cam.position;
    const float distance = length3(sub4(pos, eye));
    V4 delta = sub4(eye, load4(em.bytes.data() + 0xf0));
    store4(em.bytes.data() + 0xf0, eye);
    // The splash child: the first child whose definition has no particles (+c == 0); s7 = em+6c / 12.
    const ParticleDefinition* fp = nullptr;
    std::int32_t s7 = 0;
    for (std::int32_t i = 0; i < d.i32(def::num_children); ++i) {
        const int id = find(d.bytes.data() + def::child_name + kChildStride * static_cast<std::uint32_t>(i));
        if (id < 0) unhosted(0x3a3430, "the rain emitter's child effect name");
        const ParticleDefinition* cd = slot(id);
        if (cd->i32(def::num_particles) == 0) {
            fp = cd;
            s7 = em.i32(0x6c) / 12;
            break;
        }
    }
    const int claimed = claim_rain_pool(em, distance);
    if (claimed == 0) return;
    if (!random) unhosted(0x127dd0, "the game RNG of the rain update");
    const auto rnd = [&] { return div_s(cvt_s_w(static_cast<std::int32_t>(random())), add_s(1.0f, 2147483648.0f)); };
    const V4 box = load4(em.bytes.data() + 0xe0);
    const V4 half = scale4(box, 0.5f);
    const V4 r1 = cam.matrix[1];
    const float half_pi = std::bit_cast<float>(0x3fc90fdbu);
    const float pitch = game_atan2f(r1[2], length3(V4{r1[0], r1[1], 0.0f, r1[3]}));
    const float tn = game_tanf(em.f32(0x124));
    const float up = min_s(max_s(add_s(pitch, tn), tn), half_pi);
    const float down = max_s(min_s(sub_s(pitch, tn), -tn), -half_pi);
    const float f20 = mul_s(0.5f, add_s(em.f32(0x110), em.f32(0x10c)));
    const float top = add_s(eye[2], mul_s(f20, div_s(up, half_pi)));
    const float bottom = max_s(add_s(eye[2], mul_s(f20, div_s(down, half_pi))), sub_s(pos[2], half[2]));
    const float range = sub_s(top, bottom);
    const std::int32_t s4 = cvt_w_s(mul_s(cvt_s_w(em.i32(0x6c)), em.f32(0x120)));
    const bool snow = em.bytes[0x130] == 1;
    const float near_ = em.f32(0x10c), far_ = em.f32(0x110);
    for (std::int32_t s3 = 0; s3 < em.i32(0x6c); ++s3) {
        std::uint8_t* drop = rain_drop(em, s3);
        if (!drop) continue;
        if (claimed == 1) {
            const float x = sub_s(mul_s(box[0], rnd()), half[0]);
            const float y = sub_s(mul_s(box[1], rnd()), half[1]);
            const float z = msub_s(mul_s(range, rnd()), 0.5f, range);
            if (snow) {
                const V4 v = scale4(normalize4(V4{x, y, z, 0.0f}), std::bit_cast<float>(0x3ecccccdu));
                std::memcpy(drop + 0x0c, v.data(), 12);
            }
            set_drop_f(drop, 0x0, add_s(x, pos[0]));
            set_drop_f(drop, 0x4, add_s(y, pos[1]));
            set_drop_f(drop, 0x8, add_s(z, bottom));
            set_drop_f(drop, 0x18, 0.0f);
            drop[0x1c] = s4 < s3 ? 1 : 0;
            delta = V4{};
        }
        if (s4 < s3 && drop[0x1c] != 0) {
            set_drop_f(drop, 0x18, 0.0f);
            continue;
        }
        if (drop[0x1c] != 0) {
            drop[0x1c] = 0;
            if (!snow) set_drop_f(drop, 0x8, add_s(top, mul_s(range, rnd())));
        }
        V4 p{drop_f(drop, 0x0), drop_f(drop, 0x4), drop_f(drop, 0x8), 1.0f};
        p[2] = sub_s(p[2], d.f32(0x998));
        const float wind = d.f32(def::wind);
        if (!(wind <= std::bit_cast<float>(0x3c23d70au))) {
            if (flag != 0) unhosted(0x39a730, "rain emitter wind (the update's wind vector)");
            const V4 s = snow ? sample_weather_group(drop_f(drop, 0xc), drop_f(drop, 0x10), drop_f(drop, 0x14)) : [&] {
                const V4 n = scale4(normalize4(p), std::bit_cast<float>(0x3ecccccdu));
                return sample_weather_group(n[0], n[1], n[2]);
            }();
            const V4 w = add4(load4(weather_group.bytes.data()), s);
            p = add4(p, scale4(w, wind));
        }
        const float m = s3 < s7 ? mul_s(0.5f, fp->f32(def::size)) : 1.0f;
        bool any = false, rx = false, ry = false, rz = false;
        const auto wrap = [&](int k) {
            const float dk = sub_s(p[k], pos[k]);
            const float e = add_s(half[k], m);
            if (dk < -e) {
                p[k] = add_s(pos[k], e);
                return true;
            }
            if (!(dk <= e)) {
                p[k] = sub_s(pos[k], e);
                return true;
            }
            return false;
        };
        if (wrap(0)) {
            ry = rz = any = true;
        } else if (wrap(1)) {
            rx = rz = any = true;
        }
        if (p[2] < sub_s(bottom, m)) {
            if (em.i32(0x12c) >= 0 && !(drop_f(drop, 0x18) <= 0.01f) && r1[2] <= 0.5f) {
                std::array<V4, 4> rows = kIdentity;
                rows[3] = V4{p[0], p[1], sub_s(pos[2], half[2]), p[3]};
                const auto mb = matrix_bytes(rows);
                spawn_child(inst, &em, em.i32(0x12c), mb.data(), t);
            }
            rx = ry = any = true;
            p[2] = add_s(p[2], add_s(m, range));
            if (s4 < s3) drop[0x1c] = 1;
        } else if (!(p[2] <= add_s(top, m))) {
            rx = ry = any = true;
            p[2] = add_s(p[2], sub_s(sub_s(bottom, top), m));
        }
        if (rx) p[0] = sub_s(add_s(pos[0], mul_s(box[0], rnd())), half[0]);
        if (ry) p[1] = sub_s(add_s(pos[1], mul_s(box[1], rnd())), half[1]);
        if (rz) {
            p[2] = msub_s(add_s(bottom, mul_s(range, rnd())), 0.5f, range);
            if (!snow) p[2] = add_s(p[2], range);
        }
        if (!snow)
            for (std::size_t k = 0; k < 3; ++k) set_drop_f(drop, 0xc + 4 * k, sub_s(p[k], drop_f(drop, 4 * k)));
        float alpha = drop_f(drop, 0x18);
        float a = 0.0f;
        bool lerp = true;
        if (s3 < s7) {
            p = add4(p, scale4(delta, 0.5f));
            const V4 dv = sub4(p, eye);
            if (r1[2] <= 0.5f && dot3(dv, r1) <= 0.0f) {
                lerp = false;
            } else {
                const float mid = mul_s(0.5f, add_s(far_, near_));
                const float span = sub_s(far_, near_);
                const float dist = length3(dv);
                if (!(dist < near_) && dist <= far_) {
                    const float q = div_s(dv[2], add_s(box[2], box[2]));
                    a = mul_s(sub_s(1.0f, div_s(std::fabs(sub_s(dist, mid)), mul_s(0.5f, span))), max_s(mul_s(q, q), 1.0f));
                }
                a = mul_s(max_s(min_s(a, 1.0f), 0.0f), fp->f32(def::alpha));
                if (any) alpha = 0.0f;
            }
        } else if (any && !snow) {
            a = 0.0f;
        } else {
            const V4 dv = sub4(p, eye);
            if (r1[2] <= 0.5f && dot3(dv, r1) <= 0.0f) {
                a = 0.0f;
            } else {
                const float dist = length3(dv);
                const float base = d.f32(def::alpha);
                if (!(dist < far_))
                    a = std::bit_cast<float>(0x3e4ccccdu);
                else if (dist <= near_)
                    a = base;
                else
                    a = min_s(mul_s(base, sub_s(std::bit_cast<float>(0x3f99999au), div_s(sub_s(dist, near_), sub_s(far_, near_)))),
                              base);
            }
        }
        if (lerp && alpha < a) a = add_s(alpha, mul_s(std::bit_cast<float>(0x3e19999au), sub_s(a, alpha)));
        std::memcpy(drop, p.data(), 12);
        set_drop_f(drop, 0x18, a);
    }
}

// 00399080(em): the splash sprites (the first em+6c / 12 drops, the splash
// child's texture and size), then the snow sprites or the rain streaks.
void ParticleSystem::draw_rain(Emitter& em, std::vector<WeatherFlush>& out) {
    if (em.pool < 0 || rain_pool[static_cast<std::size_t>(em.pool)].owner != &em) return;
    const ParticleDefinition& d = *em.def;
    const std::int32_t mode = d.i32(def::alpha_mode);
    if (mode < 0 || mode > 3) unhosted(0x399104, "an alphaMode outside the ALPHA table 0x4bc1b0..0x4bc1c8");
    const std::uint64_t alpha_word = kShatterAlpha[mode];
    V4 colour{d.f32(0x140), d.f32(0x144), d.f32(0x148), 1.0f};
    const ParticleDefinition* fp = nullptr;
    for (std::int32_t i = 0; i < d.i32(def::num_children); ++i) {
        const int id = find(d.bytes.data() + def::child_name + kChildStride * static_cast<std::uint32_t>(i));
        if (id < 0) unhosted(0x3a3430, "the rain draw's child effect name");
        const ParticleDefinition* cd = slot(id);
        if (cd->i32(def::num_particles) == 0) {
            fp = cd;
            break;
        }
    }
    const auto sprites = [&](const ParticleDefinition* owner, std::int32_t from, std::int32_t to, float size) {
        WeatherFlush f{&em, owner, alpha_word, true, {}};
        for (std::int32_t s = from; s < to; ++s) {
            const std::uint8_t* drop = rain_drop(em, s);
            if (!drop) continue;
            const float a = drop_f(drop, 0x18);
            colour[3] = a;
            if (a <= std::bit_cast<float>(0x3a83126fu)) continue;
            f.vertices.push_back({V4{drop_f(drop, 0), drop_f(drop, 4), drop_f(drop, 8), size}, colour, {}});
            if (f.vertices.size() == 0x21) {  // 00371170 at 33 vertices
                out.push_back(f);
                f.vertices.clear();
            }
        }
        if (!f.vertices.empty()) out.push_back(std::move(f));
    };
    std::int32_t s1 = 0;
    if (fp) {
        s1 = em.i32(0x6c) / 12;
        sprites(fp, 0, s1, fp->f32(def::size));
    }
    if (em.bytes[0x130] == 1) {
        sprites(&d, s1, em.i32(0x6c), d.f32(def::size));
        return;
    }
    if (!em.camera) unhosted(0x3999b4, "the rain draw without its camera (em+104 vt+e8)");
    const V4 r1 = em.camera->matrix[1];
    const V4 side = scale4(normalize4(cross4(r1, kUp)), std::bit_cast<float>(0x3d8f5c29u));
    // 0x4bc5a0..0x4bc5cc (set on the first draw): the three st qwords; 0025f220 gives 2048.
    constexpr V4 st[3] = {{0.0f, 0.0f, 1.0f, 0.0f}, {0.5f, 1.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 1.0f, 0.0f}};
    const auto st_of = [](const V4& s) { return V4{add_s(s[0], 2048.0f), add_s(s[1], 2048.0f), s[2], s[3]}; };
    WeatherFlush f{&em, &d, alpha_word, false, {}};
    for (std::int32_t s = s1; s < em.i32(0x6c); ++s) {
        const std::uint8_t* drop = rain_drop(em, s);
        if (!drop) continue;
        const float a = drop_f(drop, 0x18);
        colour[3] = a;
        if (a <= std::bit_cast<float>(0x3a83126fu)) continue;
        const V4 p{drop_f(drop, 0), drop_f(drop, 4), drop_f(drop, 8), d.f32(def::size)};
        const V4 v{drop_f(drop, 0xc), drop_f(drop, 0x10), drop_f(drop, 0x14), 1.0f};
        f.vertices.push_back({sub4(p, side), colour, st_of(st[0])});
        f.vertices.push_back({sub4(p, scale4(v, d.f32(def::size))), colour, st_of(st[1])});
        f.vertices.push_back({add4(p, side), colour, st_of(st[2])});
    }
    if (!f.vertices.empty()) out.push_back(std::move(f));  // 00371200
}

void ParticleSystem::destroy(EffectInstance& inst) {
    for (auto& em : inst.emitters) release(inst, *em);
    inst.emitters.clear();
    inst.set_u32(0x1c, 0);
}

void ParticleSystem::construct(EffectInstance& inst, int id, float scale, const std::uint8_t* matrix) {
    inst.set_u32(0x00, 0x004dfac8);
    inst.set_u32(0x04, 0x004dfab8);
    inst.set_u32(0x10, 0x004dfab8);
    inst.set_u32(0x1c, 0);
    inst.set_f32(0x50, 0.0f);
    inst.bytes[0x68] = 0;
    inst.set_u32(0x20, 0);
    inst.set_f32(0x64, 0.0f);
    inst.set_f32(0x5c, 1.0f);
    inst.bytes[0x6a] = 0;
    inst.emitters.clear();
    ParticleDefinition* d = slot(id);
    inst.def = d;
    std::unique_ptr<Emitter> em;
    if (d->u8(def::type) == 1) {
        em = make_emitter(nullptr, d, nullptr, inst.f32(0x50));
    } else if (id == special[0]) {  // 003a2f38: *0053b5a0 (ShatterWindow.par), f12 = inst+50
        em = make_shatter(nullptr, d, matrix, inst.f32(0x50));
    } else if (id == special[1] || id == special[2] || id == special[3]) {  // 003a3000: 0053b5a8 / 5b0 / 5b8
        em = make_rain(nullptr, d, matrix, inst.f32(0x50));  // 0x140 bytes, 0039b3a0(em, 0, def, matrix, f12 = inst+50)
    } else {
        em = make_type0_emitter(nullptr, d, nullptr, inst.f32(0x50));  // 003a30e8: 0x130 bytes
    }
    // 003a3180
    const float r = table_.word(3 + em->bytes[0x70]);
    inst.set_f32(0x58, mul_s(d->f32(def::effect_scale_mask), mul_s(2.0f, sub_s(r, 0.5f))));
    inst.set_f32(0x54, add_s(scale, mul_s(scale, inst.f32(0x58))));
    inst.emitters.push_back(std::move(em));
    inst.set_u32(0x1c, inst.u32(0x1c) + 1);
}

bool ParticleSystem::dead(const Emitter& em, float t) const {
    if (em.u32(0x00) == kRainVtable) return false;  // 003a6070 (the rain isDead) returns 0
    // 0039ef10 (ShatterWindow isDead): the root is dead once an update found
    // no flying shard (byte +16c); the children use the 00397ab0 test.
    if (em.u32(0x00) == kShatterVtable && !em.parent) return em.bytes[0x16c] != 0;
    if (em.def->u32(def::cycle_forever) != 0) return false;  // 00397ab0
    return !(sub_s(t, em.f32(0x64)) <= em.f32(0x60));
}

// 003a1450(em, inst, f12 = t - em+64).
void ParticleSystem::spawn(Emitter& em, EffectInstance& inst, float t) {
    ParticleDefinition& d = *em.def;
    for (std::int32_t i = 0; i < d.i32(def::num_children); ++i) {
        const std::size_t c = kChildStride * static_cast<std::uint32_t>(i);
        if (em.u32(0x80) & (1u << i)) continue;
        if (d.i32(def::child_trigger_count + c) == 0) continue;
        std::uint8_t& count = em.bytes[0x78 + static_cast<std::size_t>(i)];
        const std::size_t at = def::child_trigger_time + c + 4u * count;
        // Host guard: 003a1450 would read past the 0xa70-byte definition.
        if (at + 4 > sizeof(d.bytes)) unhosted(0x3a1450, "child trigger time beyond the definition");
        const float when = d.f32(at);
        if (when < 0.0f) continue;
        if (t < when) continue;
        for (;;) {
            const int id = find(d.bytes.data() + def::child_name + c);
            if (id < 0) unhosted(0x3a3430, "child effect name");
            // 003a26a0(inst, em, id, def+230+c, f12 = when + em+64): the type-0
            // and ShatterWindow children take inst+50 instead.
            Emitter* child = spawn_child(inst, &em, id, d.bytes.data() + def::child_matrix + c, add_s(when, em.f32(0x64)));
            if (child->def == em.def) {
                for (std::int32_t s = 0; s < d.i32(def::num_children); ++s)
                    if (same_name(d.bytes.data() + def::effect_name, d.bytes.data() + def::child_name + kChildStride * static_cast<std::uint32_t>(s)))
                        child->set_u32(0x80, child->u32(0x80) | (1u << s));
            }
            ++count;
            if (!(count < d.u32(def::child_trigger_count + c))) {
                em.set_u32(0x80, em.u32(0x80) | (1u << i));
                break;
            }
            if (t < when) break;
        }
    }
}

// 003a1760(em, inst, matrix, wind, f12 = t).
void ParticleSystem::update_body(Emitter& em, EffectInstance& inst, const std::uint8_t* matrix, float t) {
    spawn(em, inst, sub_s(t, em.f32(0x64)));
    ParticleDefinition& d = *em.def;   // vtable +14
    if (d.u32(def::apply_game_light) != 0 && !(sub_s(t, em.f32(0x68)) <= 0.25f)) {
        if (!game_light) unhosted(0x3a17c8, "game light sample (applyGameLight)");
        if (!matrix) unhosted(0x394590, "the node matrix (node+10) of the game light sample");
        // 003a17cc: the reference is a2 + 30; 003a1850: 00373320(00535d20, 0x16, 1, 0); lq / sq 00537d80 -> em+50.
        store4(em.bytes.data() + 0x50, game_light(load4(matrix + 0x30)));
        // 003a1884..003a1900: em+50+4k = 1/128 (0x3c000000) * (em+50+4k * def+140+4k), k = 0..2.
        for (std::size_t k = 0; k < 3; ++k)
            em.set_f32(0x50 + 4 * k, mul_s(0.0078125f, mul_s(em.f32(0x50 + 4 * k), d.f32(0x140 + 4 * k))));
        em.set_f32(0x68, t);
    }
    em.bytes[0x84] = 1;
}

// 003a0540 -> 003a1760, then the wind check.
void ParticleSystem::update_emitter(Emitter& em, EffectInstance& inst, const std::uint8_t* matrix, std::uint32_t flag, float t) {
    update_body(em, inst, matrix, t);
    if (flag != 0 && !(em.def->f32(def::wind) <= 0.01f)) unhosted(0x3a057c, "emitter wind");
}

// The emitter vtable +1c: 003a23f0 (type 1) returns 0; 00397a90 (type 0)
// returns em+e0 once byte +128 is set.
const std::uint8_t* attach_matrix(const Emitter& em) {
    if (em.u32(0x00) == kType0EmitterVtable && em.bytes[0x128] != 0) return em.bytes.data() + 0xe0;
    return nullptr;
}

namespace {
bool vu_unmodelled(float f) {
    const std::uint32_t b = std::bit_cast<std::uint32_t>(f);
    return (b & 0x7f800000u) == 0x7f800000u || ((b & 0x7f800000u) == 0 && (b & 0x007fffffu) != 0);
}
// VADD (ee.py: one binary32 sum, denormals flushed); an exponent-255 operand or an overflowing sum throws.
float bounds_vadd(float a, float b) {
    if (vu_unmodelled(a) || vu_unmodelled(b)) unhosted(0x3a2ae0, "a VADD operand with exponent 255 or a denormal in the effect bounds");
    const float r = ftz(a + b);
    if (!std::isfinite(r)) unhosted(0x3a2ae0, "a VADD overflow in the effect bounds (the VU clamp)");
    return r;
}
float bounds_minmax(float a, float b, bool max) {
    if (vu_unmodelled(a) || vu_unmodelled(b)) unhosted(0x3a2b1c, "a VMINI / VMAX operand with exponent 255 or a denormal in the effect bounds");
    if (a == b && std::bit_cast<std::uint32_t>(a) != std::bit_cast<std::uint32_t>(b))
        unhosted(0x3a2b1c, "a VMINI / VMAX of +0 against -0 in the effect bounds");
    return max ? (b > a ? b : a) : (b < a ? b : a);
}
}  // namespace

EffectBounds effect_bounds(const EffectInstance& inst) {
    if (!inst.def) unhosted(0x3a2a9c, "the effect bounds of an instance without a definition (+24)");
    const std::uint8_t* d = inst.def->bytes.data();
    EffectBounds r;
    std::memcpy(r.lo.data(), d + 0x890, 16);
    std::memcpy(r.hi.data(), d + 0x8a0, 16);
    if (inst.bytes[0x69] == 0) return r;  // 003a2a94: def+890 / def+8a0
    // 003a2aa8..003a2b8c: the globals take the def box, then each corner moved by inst+40 widens them.
    const V4 offset = load4(inst.bytes.data() + 0x40);
    for (const std::size_t corner : {std::size_t{0x890}, std::size_t{0x8a0}}) {
        const V4 c = load4(d + corner);
        V4 p;
        for (int k = 0; k < 4; ++k) p[k] = bounds_vadd(offset[k], c[k]);
        for (int k = 0; k < 3; ++k) {
            r.lo[k] = bounds_minmax(r.lo[k], p[k], false);
            r.hi[k] = bounds_minmax(r.hi[k], p[k], true);
        }
    }
    return r;
}

EffectBounds scale_effect_bounds(EffectBounds b, float scale) {
    // 00394450..00394504: mul.s +70 by x and y of each corner (lwc1 / swc1 into 0053b4d0 / 0053b4e0).
    const auto mul = [](float a, float c) {
        if (!std::isfinite(a) || !std::isfinite(c)) unhosted(0x39447c, "a bounds scale operand with exponent 255 (the EE FPU)");
        const float p = mul_s(ftz(a), ftz(c));   // denormal operands read as zero
        if (!std::isfinite(p)) unhosted(0x39447c, "a bounds scale product past the float range (the EE FPU clamp)");
        return p;
    };
    for (int k = 0; k < 2; ++k) {
        b.lo[k] = mul(scale, b.lo[k]);
        b.hi[k] = mul(scale, b.hi[k]);
    }
    return b;
}

// 00398b20(em, inst, matrix, wind, f12 = t): the type-0 emitter update.
void ParticleSystem::update_type0(Emitter& em, EffectInstance& inst, const std::uint8_t* matrix, std::uint32_t flag, float t) {
    if (!matrix) unhosted(0x394590, "the node matrix (node+10) of a type-0 emitter update");
    ParticleDefinition& d = *em.def;
    if (em.bytes[0x84] == 0) {
        const float keep = em.f32(0x9c);  // lq / sq of +40 into +90, then swc1 +9c
        std::memcpy(em.bytes.data() + 0x90, em.bytes.data() + 0x40, 16);
        em.set_f32(0x9c, keep);
        if (em.i32(0x124) > 0) {
            std::memcpy(em.bytes.data() + 0xe0, matrix, 0x40);
            em.bytes[0x128] = 1;
        }
    }
    update_body(em, inst, matrix, t);
    // 00398470(em, matrix): em+d0 = 0x4bc550 (0, 0, 1, 1) through the
    // transposed upper 3x3 (vmulax / vmadday / vmaddz .xyz), w kept; then
    // em+dc = cvt.s.w of the byte def+64 (heatHaze).
    {
        const V4 m0 = load4(matrix), m1 = load4(matrix + 0x10), m2 = load4(matrix + 0x20);
        const V4 t0{m0[0], m1[0], m2[0], m0[3]}, t1{m0[1], m1[1], m2[1], m1[3]}, t2{m0[2], m1[2], m2[2], m2[3]};
        store4(em.bytes.data() + 0xd0, transform3(t0, t1, t2, V4{0.0f, 0.0f, 1.0f, 1.0f}));
        em.set_f32(0xdc, cvt_s_w(d.u8(0x64)));
    }
    if (flag != 0 && !(d.f32(def::wind) <= 0.01f)) unhosted(0x398bfc, "type-0 emitter wind (00398d90 / 0013ed60)");
    if (inst.bytes[0x6a] != 0) return;
    if (em.i32(0x124) > 0) {
        // 003a3430 of the definition's own effectName, then 003a26a0(inst, 0,
        // id, identity, t): with no parent the type-0 path 003a27c0 builds
        // 00398e20(em, 0, def, identity, f12 = inst+50) and inserts it (fp = 0).
        const int id = find(d.bytes.data() + def::effect_name);
        if (id < 0) unhosted(0x3a3430, "the type-0 emitter's own effectName");
        ParticleDefinition* cd = slot(id);
        std::unique_ptr<Emitter> made = cd->u8(def::type) == 1 ? make_emitter(nullptr, cd, nullptr, t)
                                                               : make_type0_emitter(nullptr, cd, nullptr, inst.f32(0x50));
        Emitter* clone = made.get();
        insert_sorted(inst, std::move(made));
        if (d.bytes[0x9cc] == 0) em.set_u32(0x124, em.u32(0x124) - 1);
        clone->set_u32(0x124, em.u32(0x124));
        em.set_u32(0x124, 0);
    }
    if (d.u32(def::cycle_forever) != 0 && !(sub_s(t, em.f32(0x64)) <= em.f32(0x60))) {
        em.set_f32(0x64, t);
        const float keep = em.f32(0x9c);
        std::memcpy(em.bytes.data() + 0x90, em.bytes.data() + 0x40, 16);
        em.set_f32(0x9c, keep);
    }
}

void ParticleSystem::update(EffectInstance& inst, const std::uint8_t* matrix, std::uint32_t flag, float dt) {
    inst.set_f32(0x50, add_s(inst.f32(0x50), dt));
    inst.bytes[0x68] = 0;
    inst.bytes[0x69] = 0;
    if (!(inst.f32(0x64) <= 0.0f)) {
        inst.set_f32(0x5c, add_s(inst.f32(0x5c), inst.f32(0x60)));
        inst.set_f32(0x64, sub_s(inst.f32(0x64), dt));
    }
    if (inst.u32(0x1c) == 0) return;
    for (auto it = inst.emitters.begin(); it != inst.emitters.end();) {
        const auto next = std::next(it);
        Emitter& em = **it;
        if (dead(em, inst.f32(0x50))) {
            release(inst, em);  // the vtable +8 destructor (0039f740 for ShatterWindow)
            inst.emitters.erase(it);
            inst.set_u32(0x1c, inst.u32(0x1c) - 1);
            ++inst.freed;
        } else {
            // 003a2550: an attached emitter (vtable +1c non-zero) whose instance
            // has no offset yet takes inst+40 = last emitter's +1c row 3 -
            // matrix row 3 (vsub.xyzw) when the tail is attached too.
            if (attach_matrix(em) && inst.bytes[0x69] == 0) {
                if (const std::uint8_t* last = attach_matrix(*inst.emitters.back())) {
                    if (!matrix) unhosted(0x394590, "the node matrix (node+10) of the attach offset");
                    store4(inst.bytes.data() + 0x40, sub4(load4(last + 0x30), load4(matrix + 0x30)));
                    inst.bytes[0x69] = 1;
                }
            }
            if (em.u32(0x00) == kType0EmitterVtable)
                update_type0(em, inst, matrix, flag, inst.f32(0x50));  // vtable 004dfa80 +10
            else if (em.u32(0x00) == kShatterVtable)
                update_shatter(em, inst, matrix, flag, inst.f32(0x50));  // vtable 004dfb10 +10
            else if (em.u32(0x00) == kRainVtable)
                update_rain(em, inst, matrix, flag, inst.f32(0x50));  // vtable 004dfaf0 +10: 0039a000
            else
                update_emitter(em, inst, matrix, flag, inst.f32(0x50));
        }
        it = next;
    }
}

void ParticleSystem::draw(Emitter& em, float t, float f13, float scale, float alpha, std::vector<ParticleBatch>& out) {
    if (em.i32(0x6c) <= 0) return;
    ParticleDefinition& d = *em.def;
    float f23 = 0.0f;
    const float f22 = div_s(1.0f, add_s(1.0f, f23));
    float f21 = f23, f20 = f23;
    em.set_f32(0xac, f13);
    d.set_f32(0x94c, sub_s(t, em.f32(0x64)));
    d.set_f32(0x970, scale);
    if (d.u32(def::apply_game_light) != 0) {
        const float keep = d.f32(0x90c);
        std::memcpy(d.bytes.data() + 0x900, em.bytes.data() + 0x50, 16);
        d.set_f32(0x90c, keep);
    }
    if (night_vision && d.u32(def::use_night_vision) != 0)
        d.set_f32(0x90c, mul_s(d.f32(def::night_vision_alpha), alpha));
    else
        d.set_f32(0x90c, mul_s(d.f32(def::alpha), alpha));
    for (std::int32_t pass = 0; pass < d.i32(def::motion_blur) + 1; ++pass) {
        em.set_f32(0xc0, f23);
        em.set_f32(0xc4, f22);
        em.set_f32(0xc8, f21);
        em.set_f32(0xcc, f20);
        for (int q = 0; q < 7; ++q) std::memcpy(vu.data[0xf + q].data(), em.bytes.data() + 0x90 + 0x10 * q, 16);
        for (int q = 0; q < 11; ++q) std::memcpy(vu.data[0x16 + q].data(), d.bytes.data() + 0x900 + 0x10 * q, 16);
        f23 = sub_s(f23, d.f32(def::frame_delta));
        f21 = add_s(f21, d.f32(def::alpha_delta));
        f20 = add_s(f20, d.f32(def::size_delta));
        // def+b0 / def+c0: the GS TEX1 / TEX0 words of the texture
        // (003797d0 / 00398b00 / 003bb620 / 00379700) stay with the host.
        std::int32_t per_batch = 0x21;
        if (d.u8(def::heat_haze) != 0) {
            d.set_u32(0xb8, 0x15); d.set_u32(0xbc, 0);
            d.set_u32(0xc8, 7); d.set_u32(0xcc, 0);
            d.set_u32(0xd0, 1); d.set_u32(0xd4, 0x612f4000);
            d.set_u32(0xd8, 1); d.set_u32(0xdc, 0x602f4000);
            per_batch = 0xe;
        }
        std::int32_t left = em.i32(0x6c);
        const std::uint32_t serial = em.u32(0x70);
        while (left > 0) {
            const std::int32_t n = left < per_batch ? left : per_batch;
            d.set_u32(0x130, 0x8000u | static_cast<std::uint32_t>(n));
            d.set_u32(0x134, 0x602f4000u);
            for (int q = 0; q < 12; ++q) std::memcpy(vu.data[kParticleTop + q].data(), d.bytes.data() + 0x80 + 0x10 * q, 16);
            vu.data[kParticleTop + 0xc] = table_.qword_at(serial + static_cast<std::uint32_t>(left));
            ParticleBatch batch;
            batch.emitter = &em;
            batch.def = &d;
            batch.pass = pass;
            if (deferred) {
                defer(DeferredProgram::particle_29c, batch);  // port-only (132mt3)
            } else {
                batch.stop = run_particle_program(vu, kParticleTop);
                read_batch(vu, batch);
            }
            out.push_back(std::move(batch));
            left -= n;
        }
    }
}

void ParticleSystem::draw_type0(Emitter& em, float t, float f13, float scale, float alpha, std::vector<ParticleBatch>& out) {
    // 00398590 (vtable 004dfa80 +0c): one pass per motion-blur step, each a
    // packet A (FLUSH; UNPACK em+90..d0 -> VU1 0xf..0x13, def+900..980 ->
    // 0x14..0x1c) and a packet B of batches (TOP = def+80..13f, TOP+0xc = the
    // random qword, MSCAL 0x16 -> program 0x170).
    if (em.i32(0x6c) <= 0) return;
    ParticleDefinition& d = *em.def;  // em+120
    float f22 = 0.0f, f21 = 0.0f;
    const float f20 = sub_s(add_s(t, d.f32(0x9b0)), em.f32(0x64));
    float f24 = mul_s(60.0f, f20);
    const float f23 = d.i32(def::motion_blur) > 0 ? mul_s(60.0f, d.f32(def::frame_delta)) : f22;
    if (d.u32(def::apply_game_light) != 0) {
        const float keep = d.f32(0x98c);
        std::memcpy(d.bytes.data() + 0x980, em.bytes.data() + 0x50, 16);
        d.set_f32(0x98c, keep);
    }
    if (night_vision && d.u32(def::use_night_vision) != 0)
        d.set_f32(0x98c, mul_s(d.f32(def::night_vision_alpha), alpha));
    else
        d.set_f32(0x98c, mul_s(d.f32(def::alpha), alpha));
    em.set_f32(0xac, f13);
    em.set_f32(0xbc, scale);
    for (std::int32_t pass = 0; pass < d.i32(def::motion_blur) + 1; ++pass) {
        const float f0 = game_powf(f24, d.f32(0x9a8));  // 00121b30
        em.set_f32(0xc0, f20);
        em.set_f32(0xc4, f24);
        em.set_f32(0xc8, f22);
        em.set_f32(0xcc, f21);
        d.set_f32(0x920, f0);
        d.set_f32(0x930, mul_s(f0, d.f32(0x9ac)));
        for (int q = 0; q < 5; ++q) std::memcpy(vu.data[0xf + q].data(), em.bytes.data() + 0x90 + 0x10 * q, 16);
        for (int q = 0; q < 9; ++q) std::memcpy(vu.data[0x14 + q].data(), d.bytes.data() + 0x900 + 0x10 * q, 16);
        f24 = sub_s(f24, f23);
        f22 = add_s(f22, d.f32(def::alpha_delta));
        f21 = add_s(f21, d.f32(def::size_delta));
        // def+b0 / def+c0 (003797d0 / 00398b00 / 003bb620 / 00379700): the GS
        // TEX1 / TEX0 words of the texture stay with the host.
        std::int32_t per_batch = 0x21;
        if (d.u8(def::heat_haze) != 0) {
            d.set_u32(0xb8, 0x15); d.set_u32(0xbc, 0);
            d.set_u32(0xc8, 7); d.set_u32(0xcc, 0);
            d.set_u32(0xd0, 1); d.set_u32(0xd4, 0x612f4000);
            d.set_u32(0xd8, 1); d.set_u32(0xdc, 0x602f4000);
            per_batch = 0xe;
        }
        std::int32_t left = em.i32(0x6c);
        const std::uint32_t serial = em.u32(0x70);
        while (left > 0) {
            const std::int32_t n = left < per_batch ? left : per_batch;
            d.set_u32(0x130, 0x8000u | static_cast<std::uint32_t>(n));
            d.set_u32(0x134, 0x602f4000u);
            for (int q = 0; q < 12; ++q) std::memcpy(vu.data[kParticleTop + q].data(), d.bytes.data() + 0x80 + 0x10 * q, 16);
            vu.data[kParticleTop + 0xc] = table_.qword_at(serial + static_cast<std::uint32_t>(left));  // 00398ae0
            ParticleBatch batch;
            batch.emitter = &em;
            batch.def = &d;
            batch.pass = pass;
            if (deferred) {
                defer(DeferredProgram::type0_170, batch);  // port-only (132mt3)
            } else {
                batch.stop = run_type0_program(vu, kParticleTop);
                read_batch(vu, batch);
            }
            out.push_back(std::move(batch));
            left -= n;
        }
    }
}

void ParticleSystem::draw_shatter_vu1(Emitter& em, float t, float f13, float scale, float alpha,
                                      std::vector<ParticleBatch>& out) {
    // 0039c680 (0039b7b0 with em+170 < 0): draw_type0's packets with 30
    // frames per second, the motion-blur offset in the first frame, no
    // def+9b0 term and no game light; em+90..ef -> VU1 0xf..0x14, def+900..98f
    // -> 0x15..0x1d, MSCAL 0x17 -> program 0x208.
    if (em.i32(0x6c) <= 0) return;
    ++shatter_particle_draws;
    ParticleDefinition& d = *em.def;  // em+120
    float f22 = 0.0f, f21 = 0.0f;
    const float f20 = sub_s(t, em.f32(0x64));
    float f24, f23;
    if (d.i32(def::motion_blur) > 0) {
        const float fd = d.f32(def::frame_delta);
        f24 = mul_s(30.0f, add_s(f20, mul_s(static_cast<float>(d.i32(def::motion_blur)), fd)));
        f23 = mul_s(30.0f, fd);
    } else {
        f24 = mul_s(30.0f, f20);
        f23 = f22;
    }
    em.set_f32(0xac, f13);
    em.set_f32(0xbc, scale);
    if (night_vision && d.u32(def::use_night_vision) != 0)
        d.set_f32(0x98c, mul_s(d.f32(def::night_vision_alpha), alpha));
    else
        d.set_f32(0x98c, mul_s(d.f32(def::alpha), alpha));
    for (std::int32_t pass = 0; pass < d.i32(def::motion_blur) + 1; ++pass) {
        const float f0 = game_powf(f24, d.f32(0x9a8));  // 00121b30
        em.set_f32(0xc0, f20);
        em.set_f32(0xc4, f24);
        em.set_f32(0xc8, f22);
        em.set_f32(0xcc, f21);
        d.set_f32(0x920, f0);
        d.set_f32(0x930, mul_s(f0, d.f32(0x9ac)));
        for (int q = 0; q < 6; ++q) std::memcpy(vu.data[0xf + q].data(), em.bytes.data() + 0x90 + 0x10 * q, 16);
        for (int q = 0; q < 9; ++q) std::memcpy(vu.data[0x15 + q].data(), d.bytes.data() + 0x900 + 0x10 * q, 16);
        f24 = sub_s(f24, f23);
        f22 = add_s(f22, d.f32(def::alpha_delta));
        f21 = add_s(f21, d.f32(def::size_delta));
        // def+b0 / def+c0: the GS TEX1 / TEX0 words stay with the host.
        std::int32_t per_batch = 0x21;
        if (d.u8(def::heat_haze) != 0) {
            d.set_u32(0xb8, 0x15); d.set_u32(0xbc, 0);
            d.set_u32(0xc8, 7); d.set_u32(0xcc, 0);
            d.set_u32(0xd0, 1); d.set_u32(0xd4, 0x612f4000);
            d.set_u32(0xd8, 1); d.set_u32(0xdc, 0x602f4000);
            per_batch = 0xe;
        }
        std::int32_t left = em.i32(0x6c);
        const std::uint32_t serial = em.u32(0x70);
        while (left > 0) {
            const std::int32_t n = left < per_batch ? left : per_batch;
            d.set_u32(0x130, 0x8000u | static_cast<std::uint32_t>(n));
            d.set_u32(0x134, 0x602f4000u);
            for (int q = 0; q < 12; ++q) std::memcpy(vu.data[kParticleTop + q].data(), d.bytes.data() + 0x80 + 0x10 * q, 16);
            vu.data[kParticleTop + 0xc] = table_.qword_at(serial + static_cast<std::uint32_t>(left));  // 00398ae0
            ParticleBatch batch;
            batch.emitter = &em;
            batch.def = &d;
            batch.pass = pass;
            if (deferred) {
                // Port-only (132mt3): a 0x19a8 stop throws when the record runs (run_deferred_program).
                defer(DeferredProgram::shatter_208, batch);
                ++shatter_particle_batches;
            } else {
                batch.stop = run_shatter_program(vu, kParticleTop);
                if (batch.stop == 0x19a8) unhosted(0x19a8, kShatterHazeStop);
                ++shatter_particle_batches;
                read_batch(vu, batch);
            }
            out.push_back(std::move(batch));
            left -= n;
        }
    }
}

void ParticleSystem::defer(DeferredProgram::Kind kind, ParticleBatch& batch) {
    DeferredProgram p;
    p.kind = kind;
    for (std::size_t q = 0; q < p.uploads.size(); ++q) p.uploads[q] = vu.data[0xf + q];
    for (std::size_t q = 0; q < p.block.size(); ++q) p.block[q] = vu.data[(kParticleTop + q) & 0x3ff];
    deferred->push_back(p);
    batch.deferred = static_cast<std::uint32_t>(deferred->size());
}

void run_deferred_program(const DeferredProgram& p, ParticleBatch& batch, Vu1State* state) {
    std::optional<Vu1State> fresh;  // vf0 = (0, 0, 0, 1), everything else zero: no program reads it (DeferredProgram)
    if (!state) state = &fresh.emplace();
    Vu1State& vu = *state;
    for (std::size_t q = 0; q < p.uploads.size(); ++q) vu.data[0xf + q] = p.uploads[q];
    for (std::size_t q = 0; q < p.block.size(); ++q) vu.data[(kParticleTop + q) & 0x3ff] = p.block[q];
    switch (p.kind) {
    case DeferredProgram::particle_29c: batch.stop = run_particle_program(vu, kParticleTop); break;
    case DeferredProgram::type0_170: batch.stop = run_type0_program(vu, kParticleTop); break;
    case DeferredProgram::shatter_208:
        batch.stop = run_shatter_program(vu, kParticleTop);
        if (batch.stop == 0x19a8) unhosted(0x19a8, kShatterHazeStop);
        break;
    default: throw std::logic_error("run_deferred_program: unknown program");
    }
    read_batch(vu, batch);
    batch.deferred = 0;
}

void ParticleSystem::draw_instance(EffectInstance& inst, std::uint32_t flags, std::vector<ParticleBatch>& out,
                                   std::vector<ShatterFlush>* shatter, std::optional<float> distance_fade,
                                   std::vector<WeatherFlush>* weather) {
    // 003a1ad0: nothing for flags 0x80 (not visible) or an empty instance;
    // flag 0x10 is the distance fade of f20 (003a1cf4..003a1e04: (0,0,0,1)
    // through renderer +f00, base [00538370] + scale [00538380] / w, clamped).
    if (flags == 0x80 || inst.u32(0x1c) == 0) return;
    float f20 = 255.0f;  // 003a1ce4: lui 0x437f
    if ((flags & 0x10) != 0) {
        if (!distance_fade) unhosted(0x3a1cf4, "the distance fade of the particle draw");
        f20 = *distance_fade;
    }
    // The matrix push (0037c350) and the heat-haze state bit 0x2000 are GS
    // state (the 0x2000 path 0037d3d8 is the host's haze pipeline,
    // particle_renderer.hpp); the type-1 vtable +1c (003a23f0) gives no attach matrix.
    for (auto& em : inst.emitters)
        if (em->drawn() && em->i32(0x6c) != 0) {
            // 003a1fc0: an attached emitter (vtable +1c non-zero) re-pushes the
            // matrix as stack * em+e0 for itself and the emitters after it.
            if (attach_matrix(*em)) unhosted(0x3a1fc0, "the attach matrix push (stack * em+e0) of the particle draw");
            if (em->u32(0x00) == kType0EmitterVtable) {
                draw_type0(*em, inst.time(), f20, inst.scale(), inst.alpha(), out);  // vtable 004dfa80 +0c
            } else if (em->u32(0x00) == kRainVtable) {  // vtable 004dfaf0 +0c: 00399080
                if (!weather) unhosted(0x399080, "the rain / snow draw without a primitive sink");
                draw_rain(*em, *weather);
            } else if (em->u32(0x00) == kShatterVtable) {  // vtable 004dfb10 +0c: 0039b7b0
                if (em->i32(0x170) < 0) {
                    draw_shatter_vu1(*em, inst.time(), f20, inst.scale(), inst.alpha(), out);  // 0039c680
                } else {
                    if (!shatter) unhosted(0x39b7b0, "the ShatterWindow shard draw without a primitive sink");
                    draw_shatter(*em, *shatter);
                }
            } else
                draw(*em, inst.time(), f20, inst.scale(), inst.alpha(), out);  // vtable +0c 003a0050
        }
}

}  // namespace omega::particle
