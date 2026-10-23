#pragma once
// The level's sky (skybox.cc, cSKYBOX_GOBJ; research/LEVEL_OBJECTS.md "Checkpoint 132o2").
//
// DATA.POP SKY: (loader 001c1b60): per record group, id, a NUL-terminated name padded to 4, three floats and a
// byte padded to 4. The constructor 001c5e50 makes one sky per run of records sharing the first record's group
// (its key: the last record's id | group << 16) and one layer parameter block per record; the layer load
// 003bbdc0 (vt +44) builds the layers (003bcc70, a model "<name>.VUM" from "<name>.HOG") and sets their motion
// from the box of the last layer flagged 0x10. Each frame the update 003bc520 (vt +48) steps every layer
// (003bcb90); the draw 003bc280 (vt +58) pushes the camera matrix without its translation (the sky is centred on
// the eye), sets the TEST ZTST ALWAYS with the z write masked, and draws the layers last to first (003bc7f0).
#include "ni_event_object.hpp"
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace omega {

struct SkyRecord {
    std::uint32_t group{}, id{};
    std::string name;
    float f1{}, f0{}, f2{};   // file order
    std::uint8_t byte{};
};
// The SKY: section body after its tag and count.
std::vector<SkyRecord> decode_sky_records(std::span<const std::uint8_t> body, std::uint32_t count);

// One layer's parameter block (001c5e50: 64 bytes on the constructor's stack, at most 8).
struct SkyLayerParams {
    std::string name;                 // +0x10
    std::array<float, 4> a{}, b{};    // +0x20 (f1, f0, 0, 0), +0x30 (0, 0, f2, 0)
    std::uint32_t flags{};            // +0x40: 0x10 when the byte is set, else 4; | 1; | 8 when f2 != 0 and no byte
};
struct SkyDefinition {
    std::uint32_t key{};              // sky +0x0c
    std::vector<SkyLayerParams> layers;
    // The environment link (00178630): an ENV's +a4 is the first world object of kind 5 whose
    // (key & 0xff0000) >> 16 matches the ENV's.
    std::uint32_t category() const { return (key >> 16) & 0xffU; }
};
// 001c1b60 / 001c5e50: the skies in file order.
std::vector<SkyDefinition> build_sky_definitions(const std::vector<SkyRecord>& records);

// A layer's motion block (003bcc70 zeroes +1b0..+20c and sets +1ec = 1).
struct SkyLayer {
    std::uint32_t flags{};                          // +0x20c
    std::array<float, 4> rotation{};                // +0x1b0 (x, y, z angles)
    std::array<float, 4> rotation_step{};           // +0x1c0
    std::array<float, 4> translation{0, 0, 0, 1};   // +0x1e0
    std::array<float, 4> direction{};               // +0x1f0
    float offset{};                                 // +0x200
    float speed{};                                  // +0x204
    float extent{};                                 // +0x208
    float follow_yaw{};                             // +0x1d8: the flag-8 draw's last camera yaw (0 from 003bcc70)
};
// A layer model's box (model header +0x30 minimum, +0x40 maximum; w as stored).
struct SkyModelBox { std::array<float, 4> minimum{}, maximum{}; };
// 003bbdc0 after the models load: one box per layer, in parameter order. Throws when no layer is flagged 0x10
// (the EE reads the box through a null layer).
std::vector<SkyLayer> setup_sky_layers(const SkyDefinition& sky, const std::vector<SkyModelBox>& boxes);
// 003bcb90: one frame of a layer's motion.
void update_sky_layer(SkyLayer& layer);

struct SkyLayerDraw {
    bool colour_mode{};                 // draw mode bit 0x4000 (005392d0)
    std::array<float, 4> colour{};      // the colour vector 005392e0 (128 x (0, 0, 0, -fade))
    NiMatrix matrix{};                  // the model matrix passed to 00376250 (mode 2)
};
// 003bc7f0 up to the model draw. `view_row_1` is row 1 (+0x10) of the matrix-stack top (renderer +0xa80 +
// 64 [00538c64]), which the sky draw 003bc280 sets to the camera view with its translation cleared.
// Checkpoint 132sx12: a flag-8 layer (f2 != 0 and no byte) follows the camera yaw (003bc930..003bca44):
// a = atan2f(row1.y, row1.x) (00121ab8 -> 0011d7a0); when a differs from +1d8 the z angle +1b8 takes
// +1c8 * (a - last) with the +-pi wrap of 003bc990..003bca24, and +1d8 = a.
SkyLayerDraw sky_layer_draw(SkyLayer& layer, const std::array<float, 4>& view_row_1);
// The colour mode on the VU (Checkpoint 132zf9): 0037c870 sees draw mode bit 0x4000, unpacks the colour vector
// (renderer +0x10f0 = 005392e0) to VU1 qword 0x1e and sets draw flag 4; microcode set 0's colour loop
// (0x1a4..0x1b2) loads qword 30, adds it to the vertex colour after itof0 and stores ftoi0 of the sum. Takes and
// returns a vertex colour in the port's units (0x80 = 1.0). A negative lane (the GS would keep its low 8 bits)
// throws naming 0x1b2.
std::array<float, 4> sky_vertex_colour(const std::array<float, 4>& vertex, const std::array<float, 4>& colour);

}  // namespace omega
