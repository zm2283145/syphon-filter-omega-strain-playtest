#pragma once
// 00389a50(model): the streaming wait state 0x12's first entry polls on the player's model
// (actor +0x60) before the loadout screen (00168550 at 0x0016858c; research/FRONTEND.md
// "Checkpoint 132f8"). It answers the count of model resources still streaming, stored in
// byte +2bdb (actor +2c3b, lb: a signed byte), and when that is 0 clears +2bdc and runs the
// hard-mesh attach points 0038e100 again (the meshes now resident):
//   1. +2bdb = 0; the head lookup 004046f0(0055dda0, quad +60): unless -1, a zero first word
//      of its cache record (005639b0 + 32 index) counts one;
//   2. for each LOD bank t = 0, 1 and part i < +23c0 (halfword): an entry +1ec0 + (t << 9) +
//      16 i whose mesh word is 0 and whose cache index (+4) is nonzero polls the cache
//      record 0055dda0 + 0x5c10 + 32 index + 16 t: a nonzero mesh copies the record's +0,
//      +4, +6, +7, +8 and +c into the entry and the other bank's record (+16 (t ^ 1)) into
//      the other bank's entry; a zero mesh counts one;
//   3. for i < +23c0, the texture handles +22c0 + 2i then +2300 + 2i (lh): a texture whose
//      +44 is the texture manager's placeholder (*00539248 + 0x440, the async load not done)
//      counts one (the loop runs eight at a time, then one at a time; the order is the same).
#include <array>
#include <cstdint>
#include <functional>
#include <optional>

namespace omega {

// One part entry (+1ec0 / +20c0 + 16 i) or one mesh-cache record half (0055dda0 + 0x5c10 +
// 32 index + 16 lod): the mesh word and the bytes copied with it.
struct ModelPartStreamEntry {
    std::uint32_t mesh{};          // +0
    std::uint16_t cache_index{};   // +4
    std::uint8_t byte_6{};         // +6
    std::uint8_t byte_7{};         // +7
    std::uint32_t word_8{};        // +8
    std::uint32_t word_c{};        // +c
    bool operator==(const ModelPartStreamEntry&) const = default;
};

struct ModelStreamWaitModel {
    std::uint16_t part_count{};                                    // +23c0
    std::array<std::array<ModelPartStreamEntry, 32>, 2> parts{};   // +1ec0 (LOD 0) / +20c0 (LOD 1)
    std::array<std::int16_t, 32> textures_lod0{};                  // +22c0
    std::array<std::int16_t, 32> textures_lod1{};                  // +2300
    std::int8_t streaming_2bdb{};                                  // +2bdb
    std::uint8_t byte_2bdc{};                                      // +2bdc
};

struct ModelStreamWaitHost {
    // 004046f0(0055dda0, +60) and the record's first word: nullopt for -1 (not in the cache),
    // else whether that word is nonzero.
    std::function<std::optional<bool>()> head_resident;
    // The mesh-cache record half at `index` for `lod` (0055dda0 + 0x5c10 + 32 index + 16 lod).
    std::function<ModelPartStreamEntry(std::uint16_t index, int lod)> cache_record;
    // The texture handle's +44 is *00539248 + 0x440 (a negative handle throws: the original reads
    // before the table).
    std::function<bool(std::int16_t handle)> texture_pending;
    // 0038e100 (Model_HardMeshAttachPoints) on the model.
    std::function<void()> attach_points;
};

// Returns +2bdb.
std::int8_t model_stream_wait(ModelStreamWaitModel& model, const ModelStreamWaitHost& host);

}  // namespace omega
