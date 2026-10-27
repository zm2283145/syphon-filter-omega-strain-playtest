#pragma once
// The particle asset of a kind-2 NIEventOBJ (vtable 004daa10, 0x74 bytes),
// Checkpoint 122d (the TORONTO1 office fire): built by 001c5950 from the
// NIE's kind-2 constructor arm 0023c1c0 (base 003cf0e0 / 003cb090), its PAR
// id requested by vt+44 = 00394720 -> 00398070 and resolved at level load by
// 00397b20 (003a58e0 loads the PAR and its children, 003a35c0 finds the id).
//   present  vt+5c = 00394270 (003a2dd0 builds the effect instance)
//   absent   vt+60 = 003941a0 (003a2ba0 + free)
//   update   vt+48 = 001c57a0 -> 00394590 (003a2400 while emitters remain)
//   bounds   vt+88 = 003943a0 (non-null iff an instance exists)
//   draw     vt+58 = 00394520 -> 003a1ad0 (each emitter's 003a0050)
// research/EFFECTS.md "Checkpoint 122d office fire".
#include "particle_emitter.hpp"
#include "particle_vu1.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace omega::particle {

constexpr std::uint32_t kParticleAssetVtable = 0x004daa10;

// The PAR definitions of a level, as 00397b20 resolves the asset requests
// (00398070) at level load: each requested name (".par" appended when it has
// no extension) is loaded by 003a58e0, which returns at once when 003a35c0
// already finds it, else parses the file (003a5630), takes its texture
// (00380a40 -> def+74, kept here as the TDX name) and then loads the children
// def+190 + k*0xe0 (k < def+180) with ".par" in turn: pre-order. The id is
// the definition's slot. `read` returns the file text or nothing when the
// file does not exist (003a58e0 then loads nothing and the id stays -1).
class ParticleLibrary {
public:
    using Reader = std::function<std::optional<std::string>(const std::string& file)>;
    explicit ParticleLibrary(Reader read) : read_(std::move(read)) {}
    // 00397b20 for one request: returns the id 003a35c0 gives afterwards.
    int request(const std::string& name);
    std::vector<ParticleDefinition*> definitions() const;
    const std::vector<std::string>& textures() const { return textures_; }
    std::size_t size() const { return defs_.size(); }
    // 0053b5a0 / 5a8 / 5b0 / 5b8 after the requests (0 at boot): copy into
    // ParticleSystem::special.
    const std::array<std::int32_t, 4>& special() const { return special_; }
    // 003a35c0: the id of a loaded definition by name (-1: none); also the runtime's lookups (00235f60's
    // effect-object emitters, the air taser's 00265b90 / 00263890; Checkpoint 132fx3).
    int lookup(const std::string& name) const;

private:
    void load(const std::string& file);         // 003a58e0
    Reader read_;
    std::vector<std::unique_ptr<ParticleDefinition>> defs_;
    std::vector<std::string> textures_;
    std::array<std::int32_t, 4> special_{};
};

// Checkpoint 122e: what the GS draws of one batch. The sprite program
// (run_sprite_program, VU1 0x5e0 plain / 0x8a0 flipped texture; 0x29c returns
// the stop) runs under the renderer frame block `frame` after the batch cull
// (cull_batch with the data 1..4 matrix `cull`): per particle the GIF PACKED
// STQ / RGBAQ / XYZ2 of the corner centre - half, then those of
// centre + half, half = (qword 0xe).xy * vf5.xy * (1, -1) * size / w. A
// sprite whose XYZ2 carries the ADC bit (w = 0xffff8000: a CLIPW flag of its
// centre) is not drawn by the GS and is dropped here, as is a culled batch
// (TOP+6.w = 0, the empty XGKICK).
//   x / y   XYZ2 x / y: the ftoi0 screen position in 1/16 pixel (the frame's
//           vf6 offset included); the GS reads bits 15:0, kept here as int32
//   z       XYZ2 z (the same for both corners: half.z = 0)
//   stq     the STQ qword: texture (S/Q, T/Q), Q = 1/w for both corners
//           (0x5e0: (0,0) and (1,1); 0x8a0: vf12.xy / vf11.xy, rotated by
//           mr32 after each particle from (1,1,0,0) / (0,0,1,1): period 4)
//   rgba    RGBAQ: bits 7:0 of each vf13 word
// Checkpoint 132cp4: a heat-haze batch (stop 0x1cc8, run_heat_haze_program)
// gives per particle the same corners, z, RGBA and ADC word for both of its
// GS sprites: stq is the first (context 2, the particle texture into the
// frame's alpha: (0, 0, q) / (q, q, q)), uv the ST of the second (context 1,
// ScreenFxBuf at the normalized screen position, Q = 1).
struct ParticleSprite {
    std::array<std::int32_t, 2> xy_first{}, xy_second{};
    std::uint32_t z = 0;
    std::array<float, 3> stq_first{}, stq_second{};
    std::array<std::uint8_t, 4> rgba{};
    std::array<float, 2> uv_first{}, uv_second{};   // heat haze only
};

struct ParticleSpriteBatch {
    const ParticleDefinition* def = nullptr;
    std::uint32_t stop = 0;
    std::uint64_t alpha = 0;      // def+d0: the GS ALPHA_1 register of the template (003a58e0); 0 for heat haze
    std::uint32_t mode = 0;       // TOP+6.w after the cull (1: all corners inside)
    std::size_t adc = 0;          // sprites dropped for the ADC bit
    std::size_t range = 0;        // particles with an exponent-255 operand (PS2 VU float rules; checkpoint 132cp8)
    std::size_t range_adc = 0;    // of those, the ones the sprite program clipped (ADC)
    std::vector<ParticleSprite> sprites;
};

// The heat-haze stop 0x1cc8 runs run_heat_haze_program under the def+d0 tag
// (Checkpoint 132cp4); `skip_heat_haze` (port-only, --no-heat-haze) drops
// those batches instead. Other stops than 0x5e0 / 0x8a0 / 0x1cc8 throw.
std::vector<ParticleSpriteBatch> particle_sprites(const std::vector<ParticleBatch>& batches,
                                                  const std::array<Qword, 4>& cull, const SpriteFrame& frame,
                                                  bool skip_heat_haze);
// One batch of particle_sprites (nullopt: skipped or culled). Port-only split (checkpoint 132mt3): each batch is a
// pure function of the batch and the frame, so the batches can run on worker threads and be kept in batch order.
std::optional<ParticleSpriteBatch> particle_sprite_batch(const ParticleBatch& batch, const std::array<Qword, 4>& cull,
                                                         const SpriteFrame& frame, bool skip_heat_haze);
// Checkpoint 132ms5b: the weather sprite flush 00371170 (the snow drops and
// the splash-cloud sprites of 00399080). 0037c870(r, r+10e0 | 0x40000) sets
// data 0 flag bit 0 (0037c90c, the unpack at 0037d26c), so the batch cull is
// skipped (pairs 0x37..0x3a, mode 2); 00370cc0 uploads per chunk the GIF tag
// (NLOOP n, PRIM 6, REGS 0x412412) at TOP+0xb, the n positions (xyz, size in
// w) at TOP+0xc and the n 0025c9d0 colours (cvt.w.s(128 c)) after them, and
// MSCALs index 0x14: 00538080[0x14] = 0xbc in microcode set 1 (005380f8[0x14]
// = 1), i.e. the sprite program 0x5e0 (run_sprite_program). Throws for a
// triangle flush. Checkpoint 132ms5c: the header's TOP+2..5 (0037f3bc) are an
// A+D tag and three GS NOP (0x7f) writes; TME / ABE come from the PRMODE
// that 0037c870 sends (PRMODECONT 0, mode word bit 0 = TME, ABE 1).
ParticleSpriteBatch weather_sprite_batch(const WeatherFlush& flush, const SpriteFrame& frame);

class ParticleAsset {
public:
    // `id` is +60 after 00397b20 (-1: the PAR was not found).
    explicit ParticleAsset(int id);
    // Checkpoint 122f: a PLR: placement (001c59d0): +c = the record key.
    ParticleAsset(int id, std::uint32_t key);

    int id() const { return id_; }
    bool on() const { return on_; }              // +2c (vt+6c 0015bfa0)
    bool present() const { return present_; }    // +2d
    float time() const { return time_; }         // +6c
    float scale() const { return scale_; }       // +70
    EffectInstance* instance() { return instance_.get(); }   // +64
    const EffectInstance* instance() const { return instance_.get(); }

    // vt+5c 00394270: builds the instance (003a2dd0, f12 = 1.0) when the id
    // is valid and none exists, then 003ce980 sets +2d. `node_matrix` is the
    // node world (node+10) 00394270 passes as 003a2dd0's matrix (read only by
    // the ShatterWindow root 0039f860).
    void set_present(ParticleSystem& system, const std::uint8_t* node_matrix = nullptr);
    // vt+60 003941a0: 003a2ba0(inst, -1), free, +64 = 0; 003ce790 clears +2d.
    void set_absent();
    // Checkpoint 132zf9: the NIE SHUTTLE reset 0023bb58: +6c = 0, vt+60(1)
    // (set_absent), vt+5c(1) (set_present): the effect restarts from time 0.
    void reset(ParticleSystem& system, const std::uint8_t* node_matrix = nullptr);
    // vt+64 001c5710 / vt+68 001c5680 (Enable / Disable(PARTICLE_GOBJ)):
    // +2c = 1 / 0; the networked message (003ff930 / 003ff650) throws.
    void set_on(bool on);
    // vt+8c 001c5670 (ScaleBBox(PARTICLE_GOBJ)): +70 = f12.
    void set_scale(float scale) { scale_ = scale; }
    // vt+48 001c57a0 (dt in f12); `node_matrix` is the asset node's 64-byte
    // world (node+10) or null when its rows are not valid. Checkpoint 132g:
    // when the instance's byte +68 is 1 (a ShatterWindow shard hit, 0039cb70)
    // and the asset id is the effect table's entry 9 (00236f20(9),
    // `shatter_id`; unset throws 001c57e0) on a frame word 005383c8 with the
    // low three bits clear, 001c5804..001c5840 post "O_GLASS_BREAK_S"
    // (0x49f3a0) at the instance's +30 (00394140): returned here.
    std::optional<std::array<float, 4>> update(ParticleSystem& system, const std::uint8_t* node_matrix, float dt,
                                               std::optional<std::int32_t> shatter_id = std::nullopt);
    // Checkpoint 132g: 00394648..003946ec for a type-e7 key (the Shatter asset
    // 00230de0 builds): 0040c9a0 unregisters it (no further update / draw),
    // 003cb230(asset, 0.1) deletes it, and the instance is released.
    bool released() const { return released_; }
    // vt+88 003943a0: null without an instance (the walker 0013e250 then
    // skips the cull 003c8990 and keeps +38 = 2, so vt+58 still runs);
    // +70 == 1.0 (c.eq.s) returns 003a2a90(inst) unscaled, otherwise its two
    // qwords with x and y multiplied by +70 (mul.s; z and w kept) in the
    // globals 0053b4d0 / 0053b4e0.
    bool has_bounds() const { return instance_ != nullptr; }
    std::optional<EffectBounds> bounds() const;
    // vt+58 00394520 with +38 = `flags` (0023a5a0's draw 00239fa0 copies the
    // NIE's +38, the walker's cull code or 2): 003a1ad0(inst, flags, node+10).
    // `shatter` receives the ShatterWindow emitters' 0039b7b0 flushes;
    // `distance_fade` is 003a1ad0's f20 for a flags word with 0x10.
    void draw(ParticleSystem& system, std::uint32_t flags, std::vector<ParticleBatch>& out,
              std::vector<ShatterFlush>* shatter = nullptr, std::optional<float> distance_fade = std::nullopt);

private:
    int id_ = -1;
    bool on_ = true;        // 003cf0e0 +2c = 1
    bool present_ = false;  // 003cf0e0 +2d = 0
    std::uint32_t type_word_ = 0;  // +c = *a1 = 0 (001c5950 passes &0); 001c59d0: the PLR key
    float time_ = 0.0f;     // +6c (00394740 clears it)
    float scale_ = 1.0f;    // +70 (001c5950)
    bool released_ = false;
    std::unique_ptr<EffectInstance> instance_;
};

// Checkpoint 122f: the node world (node+10) of a PLR: placement. 001c59d0
// reads the position and the nine basis words, 00131e90 writes the rows
// (b0 b1 b2 0) (b3 b4 b5 0) (b6 b7 b8 0), 00131e70 the row (0 0 0 1);
// vt+40 00394740 -> vt+90 003cdff0 copies them into the 0x60 node (004df6a0)
// at +10 and node vt+14 001b4960 sets +40 = (position, 1).
std::array<std::array<float, 4>, 4> placement_node_matrix(const std::array<float, 3>& position,
                                                          const std::array<float, 9>& basis);

}  // namespace omega::particle
