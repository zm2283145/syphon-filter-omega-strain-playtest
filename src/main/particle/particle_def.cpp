// Particle definition parse (see particle_def.hpp): 003a5630 / 003a43a0 /
// 003a5890 / 003a3a40 / 003a39b0 / 003a5550, ported from the static
// disassembly and checked against tools/ee/gen_particles.py (EE interpreter).
#include "particle_def.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>

namespace omega::particle {
namespace {

[[noreturn]] void unhosted(std::uint32_t address, const std::string& what) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08x", address);
    throw std::runtime_error("particle definition: unhosted " + what + " (" + buf + ")");
}

// The text cursor 003a43a0 keeps at sp+11c (003a5630 at sp+6c): strtod /
// strtol / strtoul advance it, `next_line` is s = strchr(s, '\n') + 1.
struct Cursor {
    const char* begin;
    const char* end;  // the terminating NUL
    const char* s;

    char at(std::ptrdiff_t i) const {
        const char* p = s + i;
        if (p < begin || p > end) unhosted(0x003a43a0, "a PAR read outside the text");
        return *p;
    }
    double number() {  // 0012ac20 strtod(s, &s)
        char* e = nullptr;
        const double d = std::strtod(s, &e);
        s = e;
        // the game's newlib strtod returns +0.0 for "-0.000000" and "-0" (EE golden, BULFX_DUST matrixRow0)
        return d == 0.0 ? 0.0 : d;
    }
    std::int32_t integer() {  // 0012b1f0 strtol(s, &s, 10), stored as its low word
        char* e = nullptr;
        const long long v = std::strtoll(s, &e, 10);
        s = e;
        return static_cast<std::int32_t>(v);
    }
    std::uint32_t unsigned_integer() {  // 0012af90 strtoul(s, &s, 10)
        char* e = nullptr;
        const unsigned long long v = std::strtoull(s, &e, 10);
        s = e;
        return static_cast<std::uint32_t>(v);
    }
    const char* find(char c) const {  // 00129088 strchr
        const char* p = static_cast<const char*>(std::memchr(s, c, static_cast<std::size_t>(end - s)));
        if (!p) unhosted(0x00129088, std::string("strchr '") + (c == '\n' ? "\\n" : std::string(1, c)) +
                                         "' past the end of the PAR text (the EE dereferences 0)");
        return p;
    }
    void next_line() { s = find('\n') + 1; }
    float f() { return soft_double_to_float(number()); }
};

// The reciprocal 003a43a0 stores for the time fields: 1 / max.s(x, 1e-7).
float reciprocal(float x) { return 1.0f / std::max(x, std::bit_cast<float>(0x33d6bf95u)); }

// The unsigned conversion the compiler emits (srl/andi/or, cvt.s.w, add.s)
// for 003a39b0 (+a3c) and 003a3a40 (+64): equal to the rounded u32 -> float.
float unsigned_to_float(std::uint32_t v) { return static_cast<float>(v); }

void set_quad(ParticleDefinition& d, std::size_t o, float x, float y, float z, float w) {
    d.set_f32(o, x);
    d.set_f32(o + 4, y);
    d.set_f32(o + 8, z);
    d.set_f32(o + 12, w);
}
void copy_quad(ParticleDefinition& d, std::size_t to, std::size_t from) {
    std::memcpy(d.bytes.data() + to, d.bytes.data() + from, 16);
}
void identity(ParticleDefinition& d, std::size_t o) {
    set_quad(d, o, 1, 0, 0, 0);
    set_quad(d, o + 0x10, 0, 1, 0, 0);
    set_quad(d, o + 0x20, 0, 0, 1, 0);
    set_quad(d, o + 0x30, 0, 0, 0, 1);
}

// 003a3a40: the type-1 parse (vtable 004dfb70 +0c).
void parse_type1(ParticleDefinition& d, Cursor& c) {
    d.set_f32(def::life_start_mask, c.f());
    d.set_f32(def::life, c.f());
    d.set_f32(def::life_end_mask, c.f());
    c.next_line();
    d.set_f32(def::spread, c.f());
    d.set_f32(def::time_start, c.f());
    c.next_line();
    d.set_f32(def::cycle_time, c.f());
    d.set_f32(def::cycle_variation, c.f());
    d.set_i32(def::num_cycles, c.integer());
    c.next_line();
    for (std::size_t k = 0; k < 3; ++k) {  // controlPoint[0..2] with w = 1.0
        const std::size_t o = def::control_points + 0x10 * k;
        d.set_f32(o, c.f());
        d.set_f32(o + 4, c.f());
        d.set_f32(o + 8, c.f());
        d.set_f32(o + 12, 1.0f);
        c.next_line();
    }
    for (std::size_t o : {0x980u, 0x988u, 0x9a0u, 0x9a8u}) d.set_f32(o, c.f());  // radius line 1
    c.next_line();
    for (std::size_t o : {0x984u, 0x98cu, 0x9a4u, 0x9acu}) d.set_f32(o, c.f());  // radius line 2
    c.next_line();
    set_quad(d, 0x990, d.f32(0x988), d.f32(0x98c), d.f32(0x9a0), d.f32(0x9a4));
    d.set_i32(def::cycle_forever, c.integer());
    if (d.i32(def::cycle_forever) != 0) d.set_i32(def::num_cycles, -1);
    d.set_f32(0x974, d.f32(def::effect_scale_mask));
    d.set_f32(0x978, unsigned_to_float(d.u8(def::heat_haze)));
    set_quad(d, 0x910, d.f32(0xa34), d.f32(0xa38), d.f32(0x2c), d.f32(0x18));
    set_quad(d, 0x920, d.f32(0x30), d.f32(0x34), d.f32(0x1c), d.f32(0x24));
    set_quad(d, 0x930, d.f32(0x15c), d.f32(0x16c), d.f32(0x20), d.f32(0x28));
    set_quad(d, 0x940, d.f32(0xa30), d.f32(0xa28), d.f32(0xa2c), 0.0f);
}

// 003a39b0: the type-1 finalize (vtable +10) with a1 = def+140, a2 = the
// colorIn quad (colorIn rgb, colorMask), a3 = the colorOut triple.
void finalize_type1(ParticleDefinition& d, const std::array<float, 4>& color_in,
                    const std::array<float, 3>& color_out) {
    copy_quad(d, 0x900, def::color);
    set_quad(d, 0x950, color_in[0], color_in[1], color_in[2], color_in[3]);
    set_quad(d, 0x960, color_out[0], color_out[1], color_out[2], unsigned_to_float(d.u32(def::num_cycles)));
}

// 003a3f80: the type-0 parse (vtable 004dfb90 +0c), f12 = the version.
void parse_type0(ParticleDefinition& d, Cursor& c, float version) {
    d.set_f32(0x990, c.f());  // angle; angleMask
    d.set_f32(0x994, c.f());
    c.next_line();
    d.set_f32(0x9a0, c.f());  // gravity; gravityMask
    d.set_f32(0x9a4, c.f());
    c.next_line();
    d.set_f32(0x998, c.f());  // velocity; velocityMask
    d.set_f32(0x99c, c.f());
    c.next_line();
    d.set_f32(0x9a8, c.f());  // frictionInAir; frictionInAirMask / frictionInAir (div.s)
    const float friction_mask = c.f();
    const float friction = d.f32(0x9a8);
    if (friction == 0.0f) unhosted(0x003a40a0, "a zero frictionInAir (the EE div.s by zero)");
    d.set_f32(0x9ac, friction_mask / friction);
    c.next_line();
    d.set_f32(0x9b0, c.f());  // lifeStart (the lifeStartMask is not read)
    d.set_i32(0x9b4, 0);
    c.next_line();
    d.set_f32(0x9b8, c.f());  // life; lifeMask
    d.set_f32(0x9bc, c.f());
    c.next_line();
    if (version < 1.5f) {  // c.olt.s 0x3fc00000
        d.set_i32(0x9c0, 0);
        d.set_i32(0x9c4, 0);
        d.set_i32(def::cycle_forever, 0);
    } else {
        d.set_f32(0x9c0, c.f());  // radius; radiusMask
        d.set_f32(0x9c4, c.f());
        c.next_line();
        d.set_i32(def::cycle_forever, c.integer());
        c.next_line();
    }
    if (version < 2.0f) {  // c.olt.s 0x40000000
        d.set_i32(0x9c8, 1);
        d.bytes[0x9cc] = 0;
    } else {
        d.set_i32(0x9c8, c.integer());  // framesToEmit (0 -> 1)
        if (d.i32(0x9c8) == 0) d.set_i32(0x9c8, 1);
        d.bytes[0x9cc] = c.integer() != 0 ? 1 : 0;  // emitForever (sltu)
        c.next_line();
    }
    set_quad(d, 0x900, d.f32(0x990), d.f32(0x998), d.f32(0x9b8), d.f32(0x9b0));
    set_quad(d, 0x910, d.f32(0x994), d.f32(0x99c), d.f32(0x9bc), d.f32(0x9b4));
    set_quad(d, 0x920, 0.0f, d.f32(0x9a0), d.f32(0x9c0), d.f32(0x9c4));
    set_quad(d, 0x930, 0.0f, d.f32(0x9a4), d.f32(0x2c), d.f32(0x18));
    set_quad(d, 0x940, d.f32(0x30), d.f32(0x34), d.f32(0x1c), d.f32(0x24));
    set_quad(d, 0x950, d.f32(0x15c), d.f32(0x16c), d.f32(0x20), d.f32(0x28));
}

// 003a3ef0: the type-0 finalize (vtable +10), the same arguments as 003a39b0.
void finalize_type0(ParticleDefinition& d, const std::array<float, 4>& color_in,
                    const std::array<float, 3>& color_out) {
    set_quad(d, 0x960, color_in[0], color_in[1], color_in[2], color_in[3]);
    set_quad(d, 0x970, color_out[0], color_out[1], color_out[2], unsigned_to_float(d.u32(def::flip_texture)));
    copy_quad(d, 0x980, def::color);
}

}  // namespace

std::string ParticleDefinition::name_at(std::size_t o) const {
    std::string out;
    for (std::size_t i = 0; i < 16 && bytes.at(o + i) != 0; ++i) out += static_cast<char>(bytes.at(o + i));
    return out;
}

float soft_double_to_float(double d) {
    const double a = std::fabs(d);
    if (std::isnan(d) || (a != 0.0 && (a < static_cast<double>(std::numeric_limits<float>::min()) ||
                                       a > static_cast<double>(std::numeric_limits<float>::max()))))
        unhosted(0x0011c928, "a double outside the normal float range");
    float f = static_cast<float>(d);
    if (std::fabs(static_cast<double>(f)) > a) f = std::nextafter(f, 0.0f);
    return f;
}

void copy_particle_name(std::uint8_t* dst16, std::string_view src, std::size_t length) {
    std::string buf;  // the 0x80-byte stack buffer at sp+40
    if (length == 0) {
        buf = std::string(src.substr(0, src.find('\0')));  // strcpy
    } else {
        if (length > src.size()) unhosted(0x003a5550, "a name length past the PAR text");
        buf = std::string(src.substr(0, length));  // strncpy(buf, src, n); buf[n] = 0
        buf = buf.substr(0, buf.find('\0'));
    }
    if (buf.size() >= 0x80) unhosted(0x003a5550, "a name of 0x80 or more characters (the stack buffer)");
    if (const auto dot = buf.find('.'); dot != std::string::npos) buf.resize(dot);
    if (const auto slash = buf.rfind('\\'); slash != std::string::npos) buf = buf.substr(slash + 1);
    const std::size_t n = std::min<std::size_t>(buf.size() + 1, 16);
    for (std::size_t i = 0; i < n; ++i) {
        const auto c = static_cast<std::uint8_t>(i < buf.size() ? buf[i] : 0);
        if (c >= 0x80) unhosted(0x0012b270, "tolower of a byte above 0x7f (a negative _ctype_ index)");
        dst16[i] = static_cast<std::uint8_t>(c >= 'A' && c <= 'Z' ? c + 0x20 : c);
    }
}

void write_packet_template(ParticleDefinition& d) {
    const auto put64 = [&](std::size_t o, std::uint64_t v) { std::memcpy(d.bytes.data() + o, &v, 8); };
    const std::int32_t mode = d.i32(def::alpha_mode);
    if (mode < 0 || mode > 3) unhosted(0x003a5a98, "an alphaMode outside 0..3 (the ALPHA table 0053b680 has 4)");
    put64(0x80, 0); put64(0x88, 0);          // 0x3a59e0..0x3a5a10: the zeroed stack qword
    put64(0x90, 0); put64(0x98, 0);          // 0x3a5a18..0x3a5a50: f0..f3 = 0
    put64(0xa0, 0x1000000000000003ull);      // 0x3a5a64
    put64(0xa8, 0xe);                        // 0x3a5a6c
    put64(0xb0, 0); put64(0xb8, 0x14);       // 0x3a5a74 / 0x3a5a7c
    put64(0xc0, 0); put64(0xc8, 6);          // 0x3a5a84 / 0x3a5a8c
    put64(0xd0, kParticleAlphaTable[mode]);  // 0x3a5a98..0x3a5aa8
    put64(0xd8, 0x42);                       // 0x3a5ab0
    for (std::size_t o = 0xe0; o < 0x100; o += 8) put64(o, 0);  // 0x3a5ab4..0x3a5af0: f4..f7 / f0..f3 = 0
    put64(0x138, 0x412412);                  // 0x3a5afc (delay slot)
    for (std::size_t k = 0; k < 12; ++k) d.set_u32(0x100 + 4 * k, 2);  // 0x3a5b00..0x3a5b1c
}

bool parse_particle_definition(const std::string& text, ParticleDefinition& d, std::string& texture_name) {
    Cursor c{text.c_str(), text.c_str() + text.size(), text.c_str()};
    texture_name.clear();

    // 003a5630
    const float version = c.f();
    if (!(version <= std::bit_cast<float>(0x40066666u))) return false;  // c.ole.s 2.1
    c.next_line();
    const auto type = static_cast<std::uint8_t>(c.integer() & 0xff);
    c.next_line();
    if (type > 1) unhosted(0x003a5850, "an emitterType above 1 (the EE stores the type to address 4)");
    // 003a5890 then the type's vtable (0x9d0 bytes for type 0, 0xa70 for
    // type 1); 0013ab00 over +900..+990 / +900..+a20 does nothing.
    d.set_u32(def::vtable, type == 0 ? kType0Vtable : kType1Vtable);
    for (std::size_t o : {0x890u, 0x894u, 0x898u}) d.set_u32(o, 0x7f7fffffu);
    d.set_f32(0x89c, 1.0f);
    for (std::size_t o : {0x8a0u, 0x8a4u, 0x8a8u}) d.set_u32(o, 0xff7fffffu);
    d.set_f32(0x8ac, 1.0f);
    d.bytes[def::type] = type;

    // 003a43a0
    const auto below = [&](std::uint32_t bits) { return version < std::bit_cast<float>(bits); };  // c.olt.s
    d.set_f32(0x140, c.f());
    d.set_f32(0x144, c.f());
    d.set_f32(0x148, c.f());
    c.next_line();
    const float color_mask = c.f();
    c.next_line();
    d.set_i32(def::num_particles, c.integer());
    d.set_f32(def::num_particles_mask, static_cast<float>(c.integer()));  // 001002d0
    c.next_line();
    d.set_f32(def::size, soft_double_to_float(0.5 * c.number()));
    d.set_f32(def::size_mask, c.f());
    c.next_line();
    d.set_f32(0x1c, soft_double_to_float(0.5 * c.number()));
    d.set_f32(0x20, reciprocal(c.f()));
    c.next_line();
    d.set_f32(0x24, soft_double_to_float(0.5 * c.number()));
    d.set_f32(0x28, reciprocal(c.f()));
    c.next_line();
    d.set_f32(def::alpha, c.f());
    d.set_f32(0x2c, c.f());
    if (below(0x3ff33333u)) {  // 1.9
        d.set_f32(def::alpha, d.f32(def::alpha) * 0.5f);
        d.set_f32(0x2c, d.f32(0x2c) * 0.5f);
    }
    d.set_f32(def::alpha_copy, d.f32(def::alpha));
    c.next_line();
    d.set_f32(0x30, d.f32(def::alpha_copy) / std::max(c.f(), std::bit_cast<float>(0x33d6bf95u)));
    d.set_f32(0x34, reciprocal(c.f()));
    c.next_line();
    if (below(0x40000000u)) {
        d.set_i32(def::use_night_vision, 0);
    } else {
        d.set_i32(def::use_night_vision, c.integer());
        d.set_f32(def::night_vision_alpha, c.f());
        c.next_line();
    }
    d.set_i32(def::alpha_mode, c.integer());
    c.next_line();
    d.set_i32(def::motion_blur, c.integer());
    d.set_f32(def::frame_delta, c.f());
    d.set_f32(def::alpha_delta, soft_double_to_float(128.0 * c.number()));
    d.set_f32(def::size_delta, c.f());
    c.next_line();
    if (below(0x3ff33333u)) {
        d.set_i32(def::effect_scale_mask, 0);
    } else {
        d.set_f32(def::effect_scale_mask, c.f());
        c.next_line();
    }
    if (const char* p = c.find(';'); p[1] == 's') {  // ";sortInfluence"
        d.set_i32(def::sort_influence, static_cast<std::int32_t>(c.unsigned_integer()));
        c.next_line();
    } else {
        d.set_i32(def::sort_influence, 0);
    }
    if (below(0x3fc00000u)) {  // 1.5
        d.set_i32(def::flip_texture, 0);
    } else {
        d.set_i32(def::flip_texture, c.integer());
        c.next_line();
    }
    if (below(0x3ff33333u)) {
        d.bytes[def::heat_haze] = 0;
        d.set_i32(def::wind, 0);
    } else {
        d.bytes[def::heat_haze] = c.number() != 0.0 ? 1 : 0;  // 001002b0(d, 0): __nedf2 != 0
        c.next_line();
        d.set_f32(0x68, c.f());
        d.set_f32(0x6c, c.f());
        d.set_f32(0x70, c.f());
        c.next_line();
    }
    if (below(0x40000000u)) {
        d.set_i32(def::apply_game_light, 0);
    } else {
        d.set_i32(def::apply_game_light, c.integer());
        c.next_line();
    }
    c.next_line();  // maxDisplayType
    if (c.at(0) != ' ') {
        // strchr(s, ';') - 5 skips "tga ;" (the 4-character extension and the
        // space); the base name runs back to the previous '\'. strncpy of n
        // bytes then strcat(name + n, ".tdx"): the caller's buffer is zero
        // past n (assumption, as in the golden).
        c.s = c.find(';') - 5;
        std::ptrdiff_t n = 0;
        while (c.at(-n - 1) != '\\') ++n;
        texture_name.assign(c.s - n, static_cast<std::size_t>(n));
        texture_name = texture_name.substr(0, texture_name.find('\0')) + ".tdx";
    }
    c.next_line();
    c.next_line();
    c.next_line();
    std::array<float, 4> color_in{};
    color_in[0] = c.f();
    color_in[1] = c.f();
    color_in[2] = c.f();
    color_in[3] = color_mask;
    d.set_f32(0x15c, reciprocal(c.f()));
    c.next_line();
    std::array<float, 3> color_out{c.f(), c.f(), c.f()};
    d.set_f32(0x16c, reciprocal(c.f()));
    c.next_line();
    const auto name_field = [&](std::size_t o) {
        std::memset(d.bytes.data() + o, 0, 16);
        const char* p = c.find(';');
        const std::ptrdiff_t len = p - c.s - 1;
        if (len < 0) unhosted(0x003a5550, "a name field with ';' in the first column (length -1)");
        copy_particle_name(d.bytes.data() + o, std::string_view(c.s, static_cast<std::size_t>(c.end - c.s)),
                           static_cast<std::size_t>(len));
    };
    name_field(def::effect_name);
    c.next_line();
    d.set_i32(def::num_children, c.integer());
    c.next_line();
    if (d.i32(def::num_children) > kMaxChildren)
        unhosted(0x003a4cb0, "more than 8 children (the child blocks overrun the bounds at +890)");
    for (std::int32_t i = 0; i < d.i32(def::num_children); ++i) {
        const std::size_t b = kChildStride * std::uint32_t(i);
        name_field(def::child_name + b);
        c.next_line();
        const std::int32_t n = c.integer();
        if (c.at(1) == 'n' || c.at(2) == 'n') {  // ";numTriggerFrames"
            d.set_i32(0x1a0 + b, n);
            if (n > 0) {
                if (n > 16) unhosted(0x003a4d68, "more than 16 trigger frames (they overrun +1e4)");
                c.next_line();
                for (std::int32_t k = 0; k < n; ++k) {
                    // strtoul's word is sign-extended into the 64-bit register
                    // and 0011b888 (__floatdisf) converts it signed: "-1" -> -1
                    // (EE golden, RAINLORELEI).
                    const auto frame = static_cast<std::int32_t>(c.unsigned_integer());
                    if (frame >= (1 << 24) || frame <= -(1 << 24))
                        unhosted(0x0011b888, "a trigger frame beyond 2^24 (the soft-double rounding)");
                    d.set_f32(0x1a4 + b + 4u * std::uint32_t(k), static_cast<float>(frame) / 60.0f);  // div.s 60
                    d.set_f32(0x1e4 + b + 4u * std::uint32_t(k), c.f());
                }
            }
        } else {
            d.set_i32(0x1a0 + b, 1);
            d.set_f32(0x1a4 + b, 0.0f);
            d.set_f32(0x1e4 + b, 1.0f);
        }
        c.next_line();
        const std::size_t m = def::child_matrix + b;
        identity(d, m);
        d.set_f32(m, c.f());
        d.set_f32(m + 4, c.f());
        d.set_f32(m + 8, c.f());
        c.next_line();
        d.set_f32(m + 0x10, c.f());
        d.set_f32(m + 0x14, c.f());
        d.set_f32(m + 0x18, c.f());
        if (c.at(1) == 'm' || c.at(2) == 'm') {  // ";matrixRow1" then rows 2 and 3
            c.next_line();
            d.set_f32(m + 0x20, c.f());
            d.set_f32(m + 0x24, c.f());
            d.set_f32(m + 0x28, c.f());
            c.next_line();
            d.set_f32(m + 0x30, c.f());
            d.set_f32(m + 0x34, c.f());
            d.set_f32(m + 0x38, c.f());
            c.next_line();
        } else {
            copy_quad(d, m + 0x30, m);  // row 3 = row 0 (lq +230 / sq +260)
            c.next_line();
        }
    }
    std::array<float, 4> min{0, 0, 0, 1}, max{0, 0, 0, 1};
    if (!below(0x3fcccccdu)) {  // 1.6
        min = {c.f(), c.f(), c.f(), 1.0f};
        c.next_line();
        if (!below(0x3fd9999au)) {  // 1.7
            max = {c.f(), c.f(), c.f(), 1.0f};
            c.next_line();
        } else {
            min = {0, 0, 0, 1};  // 003a5248 stores (0,0,0,1) over the read minBounds too
        }
    }
    set_quad(d, def::min_bounds, min[0], min[1], min[2], min[3]);
    set_quad(d, def::max_bounds, max[0], max[1], max[2], max[3]);
    identity(d, def::matrix);
    if (!below(0x3ff33333u)) {
        for (std::size_t r = 0; r < 3; ++r) {
            d.set_f32(def::matrix + 0x10 * r, c.f());
            d.set_f32(def::matrix + 0x10 * r + 4, c.f());
            d.set_f32(def::matrix + 0x10 * r + 8, c.f());
            c.next_line();
        }
    }
    if (type == 0) {
        parse_type0(d, c, version);
        finalize_type0(d, color_in, color_out);
    } else {
        parse_type1(d, c);
        finalize_type1(d, color_in, color_out);
    }
    return true;
}

}  // namespace omega::particle
