#pragma once
// Particle definitions (.PAR text, particle.cc), Checkpoint 122d (the
// TORONTO1 office fire): the parser 003a5630 with its common part 003a43a0,
// the type-1 ("emitterType 1") constructor 003a5890, parse 003a3a40 and
// finalize 003a39b0.
//
// The definition is kept as the EE object's byte image (0xa70 bytes for
// type 1) so the emitter runtime reads the same offsets the game does. Bytes
// the parser does not write keep the caller's fill (the EE heap block from
// 0036b180 is not cleared).
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace omega::particle {

constexpr std::size_t kDefinitionSize = 0xa70;  // 003a5630 allocates 0xa70 for type 1 (0x9d0 for type 0)
constexpr std::uint32_t kType1Vtable = 0x004dfb70;
constexpr std::uint32_t kType0Vtable = 0x004dfb90;  // parse +0c 003a3f80, finalize +10 003a3ef0
constexpr std::size_t kType0Size = 0x9d0;
constexpr std::uint32_t kChildStride = 0xe0;
constexpr std::int32_t kMaxChildren = 8;  // +190 + 8*0xe0 = +890 (the bounds); more would overrun them

// Offsets named after the PAR text comments (the line the value comes from).
namespace def {
constexpr std::size_t vtable = 0x000, type = 0x004, num_particles = 0x00c, num_particles_mask = 0x010,
                      size = 0x014, size_mask = 0x018, alpha_mode = 0x038, motion_blur = 0x03c,
                      frame_delta = 0x040, alpha_delta = 0x044, size_delta = 0x048, effect_scale_mask = 0x04c,
                      alpha = 0x050, night_vision_alpha = 0x054, use_night_vision = 0x058, sort_influence = 0x05c,
                      flip_texture = 0x060, heat_haze = 0x064, wind = 0x068, color = 0x140, alpha_copy = 0x14c,
                      effect_name = 0x170, num_children = 0x180, child_name = 0x190, child_trigger_count = 0x1a0,
                      child_trigger_time = 0x1a4, child_trigger_value = 0x1e4, child_matrix = 0x230,
                      min_bounds = 0x890, max_bounds = 0x8a0, matrix = 0x8b0, apply_game_light = 0x8f0,
                      cycle_forever = 0x8f4, vu_block = 0x900, radius = 0x980, life_start_mask = 0xa20,
                      life = 0xa24, life_end_mask = 0xa28, spread = 0xa2c, time_start = 0xa30, cycle_time = 0xa34,
                      cycle_variation = 0xa38, num_cycles = 0xa3c, control_points = 0xa40;
}

struct ParticleDefinition {
    std::array<std::uint8_t, kDefinitionSize> bytes{};

    float f32(std::size_t o) const { float v; std::memcpy(&v, bytes.data() + o, 4); return v; }
    std::int32_t i32(std::size_t o) const { std::int32_t v; std::memcpy(&v, bytes.data() + o, 4); return v; }
    std::uint32_t u32(std::size_t o) const { return static_cast<std::uint32_t>(i32(o)); }
    std::uint8_t u8(std::size_t o) const { return bytes.at(o); }
    void set_f32(std::size_t o, float v) { std::memcpy(bytes.data() + o, &v, 4); }
    void set_i32(std::size_t o, std::int32_t v) { std::memcpy(bytes.data() + o, &v, 4); }
    void set_u32(std::size_t o, std::uint32_t v) { std::memcpy(bytes.data() + o, &v, 4); }
    // The 16-byte names 003a5550 writes (effectName at +170, child i at +190 + i*0xe0).
    std::string name_at(std::size_t o) const;
    std::string effect_name() const { return name_at(def::effect_name); }
    std::string child_name(std::int32_t i) const { return name_at(def::child_name + kChildStride * std::uint32_t(i)); }
};

// The soft-float double -> float conversion 0011c928 as the PAR values reach
// it: the EE goldens show it truncates toward zero (0.15 -> 0x3e199999,
// 0.05 -> 0x3d4ccccc). Values outside the normal float range throw
// (unhosted 0011c928: no PAR value reaches them).
float soft_double_to_float(double d);

// 003a5550(dst, src, length): the name copy into a 16-byte field. The source
// (to the end when length is 0, else `length` bytes) is cut at its first '.',
// the part after the last '\' is lowered (0012b270, newlib _ctype_ upper bit)
// and min(strlen + 1, 16) bytes are stored.
void copy_particle_name(std::uint8_t* dst16, std::string_view src, std::size_t length);

// 003a5630(0, text, texture_name, 0): returns false when the version is above
// 2.1 (the EE returns 0 without allocating). `def` stands for the 0036b180
// block (keeps its fill where the parser writes nothing); `texture_name` gets
// the 003a43a0 texture name (the tgaFilename base name with ".tdx", empty
// when that line starts with a space). Type 0 (physics particles) ends with
// its parse 003a3f80 / finalize 003a3ef0 and writes only the first 0x9d0
// bytes; other types throw unhosted 003a5850 (the EE stores to address 4).
bool parse_particle_definition(const std::string& text, ParticleDefinition& def, std::string& texture_name);

// 003a3910 (the particle system constructor): the GS ALPHA register table
// 0053b680 indexed by alphaMode (def+38).
inline constexpr std::uint64_t kParticleAlphaTable[4] = {0x0000008000000044ull, 0x0000008000000068ull,
                                                         0x0000008000000048ull, 0x0000008000000046ull};

// 003a58e0 after the parse (0x3a59a0..0x3a5b1c): the GS packet template the
// draws upload as VU1 TOP+0..0xb (def+80..13f): +80 / +90 zero, the GIF tag
// +a0 = 0x1000000000000003 / +a8 = 0xe, the register pairs +b0/b8 (0, TEX1_1
// = 0x14), +c0/c8 (0, TEX0_1 = 6), +d0/d8 (kParticleAlphaTable[alphaMode],
// ALPHA_1),
// the batch bounds +e0 / +f0 zero (so the sprite programs' cull 0x3b..0x70
// sees only the emitter origin), +100..12f twelve words 2 and +138 the dword
// 0x412412. +130..137 is left alone (the draws write it). def+74 (the texture
// handle from 00380a40) is the host's texture binding and is not written.
// An alphaMode outside 0..3 throws unhosted 003a5a98 (the EE reads past the
// table into the random table 0053b6a0).
void write_packet_template(ParticleDefinition& def);

}  // namespace omega::particle
