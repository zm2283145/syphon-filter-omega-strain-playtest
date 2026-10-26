#include "skinned_mesh.hpp"
#include "zone_profile.hpp"
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <algorithm>
#if defined(__arm__) && defined(__ARM_NEON) && !defined(OMEGA_SKIN_NO_NEON)
#include <arm_neon.h>
#define OMEGA_SKIN_NEON 1
#include <atomic>
#if defined(OMEGA_SKIN_NEON_CHECK)
#include <cstring>
#include <iostream>
#endif
#else
#define OMEGA_SKIN_NEON 0
#endif

namespace omega {
namespace {
[[noreturn]] void fail(const char* message) { throw std::runtime_error(message); }
void check(bool condition, const char* message) { if (!condition) fail(message); }
// Checkpoint 132win9 (port-side speed only): the finiteness test on the bits (MSVC's std::isfinite is an
// out-of-line _fdclass call, and every skinning multiply / add passes through here).
inline float finite(float value) {
    check((std::bit_cast<std::uint32_t>(value) & 0x7f800000u) != 0x7f800000u, "SKM nonfinite input or arithmetic");
    return value;
}
float mul(float a, float b) { return finite(a * b); }
float add(float a, float b) { return finite(a + b); }
struct Reader {
    std::span<const std::uint8_t> data;
    std::size_t cursor{};
    std::uint16_t u16() {
        check(cursor + 2 <= data.size(), "SKM truncated halfword");
        const auto value = static_cast<std::uint16_t>(data[cursor] | (data[cursor + 1] << 8));
        cursor += 2; return value;
    }
    std::uint32_t u32() {
        const auto a = u16(), b = u16();
        return std::uint32_t(a) | (std::uint32_t(b) << 16);
    }
    std::uint32_t peek() { const auto old = cursor; const auto v = u32(); cursor = old; return v; }
    float real() { return finite(std::bit_cast<float>(u32())); }
    void expect(std::uint32_t word) { check(u32() == word, "SKM unsupported VIF command/order"); }
    void unpack(std::uint32_t command, std::uint32_t count, std::uint32_t destination) {
        check(count > 0 && count <= 256, "SKM unsupported transfer count");
        expect((command << 24) | ((count & 255) << 16) | destination);
    }
};
float attribute(std::uint16_t value) { return static_cast<float>(int(value) - 32768) / 256.0f; }
SkinChunk decode_chunk(std::span<const std::uint8_t> bytes, std::uint32_t vertices,
                       std::uint32_t bones) {
    Reader reader{bytes};
    SkinChunk result;
    result.packet_quadwords = static_cast<std::uint32_t>(bytes.size() / 16);
    if (reader.peek() == 0x6c014075) {
        reader.u32();
        std::array<std::uint32_t, 4> constants{};
        for (auto& value : constants) value = reader.u32();
        result.initial_constants = constants;
    }
    const auto metadata = reader.peek();
    const auto record_count = (metadata >> 16) & 255;
    check(record_count >= 2 && record_count <= 9, "SKM unsupported influence count");
    reader.unpack(0x6d, record_count, 0xc000 | (104 - record_count));
    std::vector<std::array<std::uint16_t, 4>> records(record_count);
    for (auto& record : records) for (auto& value : record) value = reader.u16();
    const auto control = records.back();
    check(vertices > 0 && vertices <= 30 && control[0] == vertices,
          "SKM inconsistent vertex count");
    check(control[2] == record_count - 1 && control[1] > 0,
          "SKM inconsistent control record");
    result.control_flags = control[3];
    for (std::size_t i = records.size() - 1; i-- > 0;) {
        const auto& raw = records[i];
        check(raw[1] > 0 && std::uint32_t(raw[3]) + raw[1] <= vertices,
              "SKM influence range outside vertices");
        check(raw[2] % 7 == 0 && raw[2] >= 133 && (raw[2] / 7 - 19u) < bones,
              "SKM influence bone outside skeleton");
        check(raw[0] >= raw[3], "SKM negative output base");
        const auto base = std::uint32_t(raw[0] - raw[3]);
        if (result.groups.empty()) {
            check(raw[3] == 0, "SKM unsupported first influence range");
            result.output_base = base;
        }
        check(base == result.output_base, "SKM inconsistent output addressing");
        result.groups.push_back({raw[0], raw[2], raw[2] / 7 - 19u, raw[3],
                                 std::vector<float>(raw[1], 1.0f)});
    }
    reader.expect(0x05000001);
    reader.unpack(0x6d, 2 * vertices, 0xc004);
    for (std::uint32_t i = 0; i < vertices; ++i) {
        std::array<float, 8> values{};
        for (auto& value : values) value = attribute(reader.u16());
        result.vertices.push_back({{values[0], values[1], values[2]},
                                   {values[6], values[7], values[3]},
                                   {values[4], values[5]}});
    }
    reader.expect(0x05000000);
    std::uint32_t weight_vectors = 0;
    if (result.groups.size() > 1) {
        for (const auto& group : result.groups)
            weight_vectors += static_cast<std::uint32_t>((group.weights.size() + 3) / 4);
        reader.unpack(0x6c, weight_vectors, 0xc000 | (4 + 2 * vertices));
        std::vector<std::uint32_t> words;
        for (auto& group : result.groups) {
            for (auto& weight : group.weights) { words.push_back(reader.peek()); weight = reader.real(); }
            for (std::size_t i = group.weights.size(); i % 4; ++i) words.push_back(reader.u32());
        }
        for (std::size_t q = 0; q < words.size(); q += 4)
            result.unpack_tail.push_back({words[q], words[q + 1], words[q + 2], words[q + 3]});
    }
    const auto index_vectors = (std::uint32_t(control[1]) + 3) / 4;
    reader.unpack(0x6e, index_vectors, 0x8000 | (4 + 2 * vertices + weight_vectors));
    check(reader.cursor + index_vectors * 4 <= bytes.size(), "SKM truncated primitive stream");
    for (std::uint32_t i = 0; i < control[1]; ++i) {
        const auto value = bytes[reader.cursor + i];
        const auto address = std::uint32_t(value & 127);
        check(address < 38, "SKM primitive outside supported output slots");
        result.primitives.push_back({address, (value & 128) != 0, value});
    }
    for (std::uint32_t q = 0; q < index_vectors; ++q) {
        std::array<std::uint32_t, 4> words{};
        for (std::size_t k = 0; k < 4; ++k)
            words[k] = static_cast<std::uint32_t>(static_cast<std::int32_t>(
                static_cast<std::int8_t>(bytes[reader.cursor + 4 * q + k])));
        result.unpack_tail.push_back(words);
    }
    reader.cursor += index_vectors * 4;
    const auto execute = reader.u32();
    check((execute & 0xffff0000) == 0x14000000, "SKM missing MSCAL");
    result.microprogram_entry = static_cast<std::uint16_t>(execute);
    result.microprogram_entries.push_back(result.microprogram_entry);
    // The recovered head packets may issue two further render-packet actions.
    // Retain them as instructions; neither is padding or another vertex block.
    if (reader.cursor < bytes.size() && reader.peek() != 0) {
        check(result.microprogram_entry == 22, "SKM unsupported continuation owner");
        const auto first = reader.u32();
        check(first == 0x14000006 || first == 0x14000002, "SKM unsupported render continuation");
        reader.expect(0x14000006);
        result.microprogram_entries.push_back(static_cast<std::uint16_t>(first));
        result.microprogram_entries.push_back(6);
    }
    while (reader.cursor < bytes.size()) reader.expect(0);
    return result;
}
void matrix_check(const SkinMatrix& matrix) { for (float value : matrix) finite(value); }
SkinMatrix multiply(const SkinMatrix& left, const SkinMatrix& right) {
    SkinMatrix result{};
    for (std::size_t c = 0; c < 4; ++c) for (std::size_t r = 0; r < 4; ++r) {
        float value = mul(left[r], right[c * 4]);
        for (std::size_t k = 1; k < 4; ++k) value = add(value, mul(left[k * 4 + r], right[c * 4 + k]));
        result[c * 4 + r] = value;
    }
    return result;
}
std::array<float, 4> transform(const SkinMatrix& matrix, const Vec3& vector, bool point) {
    std::array<float, 4> result{};
    for (std::size_t r = 0; r < 4; ++r) {
        float value = mul(matrix[r], vector.x);
        value = add(value, mul(matrix[4 + r], vector.y));
        value = add(value, mul(matrix[8 + r], vector.z));
        if (point) value = add(value, matrix[12 + r]);
        result[r] = value;
    }
    return result;
}
template<class T> void write_values(std::ostream& out, const T& values) {
    out << '['; bool first = true;
    for (const auto value : values) { if (!first) out << ','; first = false; out << value; }
    out << ']';
}
void write_vec3(std::ostream& out, const Vec3& v) { write_values(out, std::array{v.x, v.y, v.z}); }
}

SkinnedMesh decode_skinned_mesh(std::span<const std::uint8_t> bytes, std::uint32_t bones) {
    OMEGA_ZONE("load.decode_skm");  // port-only (132vt2e): the PS Vita's load-time zones
    check(bytes.size() >= 16 && bytes.size() <= 16 * 1024 * 1024 && bytes.size() % 16 == 0,
          "SKM unsupported file extent");
    check(bones >= 1 && bones <= 21, "SKM unsupported skeleton size");
    check(bytes[0] > 0 && bytes[0] <= 127 && bytes[1] == 3, "SKM unsupported envelope");
    const auto count = bytes[0];
    std::size_t cursor = (2 + 2 * std::size_t(count) + 15) & ~std::size_t(15);
    check(cursor <= bytes.size(), "SKM truncated descriptors");
    SkinnedMesh mesh{bones, {}};
    std::array<bool, 38> initialized{};
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t length = bytes[2 + 2 * i] * 16;
        check(length > 0 && length <= bytes.size() - cursor, "SKM chunk extent outside file");
        mesh.chunks.push_back(decode_chunk(bytes.subspan(cursor, length), bytes[3 + 2 * i], bones));
        const auto& chunk = mesh.chunks.back();
        check(chunk.output_base <= initialized.size() && chunk.vertices.size() <= initialized.size() - chunk.output_base,
              "SKM output range exceeds retained slots");
        for (std::size_t k = 0; k < chunk.vertices.size(); ++k) initialized[chunk.output_base + k] = true;
        for (const auto& primitive : chunk.primitives)
            check(initialized[primitive.vertex_index], "SKM references unwritten output slot");
        cursor += length;
    }
    for (; cursor < bytes.size(); ++cursor) check(bytes[cursor] == 0, "SKM unsupported trailing data");
    return mesh;
}

#if OMEGA_SKIN_NEON
namespace {
bool palette_neon_mode() {
    std::uint32_t fpscr;
    asm volatile("vmrs %0, fpscr" : "=r"(fpscr));
    return (fpscr & 0x01f70000u) == 0x01000000u;  // FZ set; RMode nearest; Stride, Len 0
}
inline uint32x4_t matrix_nonfinite(float32x4_t v) {
    const uint32x4_t e = vdupq_n_u32(0x7f800000u);
    return vceqq_u32(vandq_u32(vreinterpretq_u32_f32(v), e), e);
}
inline bool matrix_clean(uint32x4_t bad) {
    const uint32x2_t h = vorr_u32(vget_low_u32(bad), vget_high_u32(bad));
    return vget_lane_u32(vpmax_u32(h, h), 0) == 0;
}
}  // namespace
#endif

bool skin_matrix_multiply_neon(const SkinMatrix& left, const SkinMatrix& right, SkinMatrix& out) {
#if OMEGA_SKIN_NEON
    if (!palette_neon_mode()) return false;
    // multiply(): result[c*4+r] = ((l[r]*R[c*4] + l[4+r]*R[c*4+1]) + l[8+r]*R[c*4+2]) + l[12+r]*R[c*4+3], every
    // product and sum checked finite; the rows r are the four lanes. Inputs are checked too (an infinity in a column
    // of `right` meeting a zero would give a NaN anyway, but the scalar path throws on its inputs' products first).
    const float32x4_t l0 = vld1q_f32(left.data()), l1 = vld1q_f32(left.data() + 4), l2 = vld1q_f32(left.data() + 8),
                      l3 = vld1q_f32(left.data() + 12);
    uint32x4_t bad = vorrq_u32(vorrq_u32(matrix_nonfinite(l0), matrix_nonfinite(l1)),
                               vorrq_u32(matrix_nonfinite(l2), matrix_nonfinite(l3)));
    for (std::size_t c = 0; c < 4; ++c) {
        const float32x4_t rc = vld1q_f32(right.data() + c * 4);
        bad = vorrq_u32(bad, matrix_nonfinite(rc));
        float32x4_t v = vmulq_n_f32(l0, right[c * 4]);
        bad = vorrq_u32(bad, matrix_nonfinite(v));
        float32x4_t p = vmulq_n_f32(l1, right[c * 4 + 1]);
        v = vaddq_f32(v, p);
        bad = vorrq_u32(bad, vorrq_u32(matrix_nonfinite(p), matrix_nonfinite(v)));
        p = vmulq_n_f32(l2, right[c * 4 + 2]);
        v = vaddq_f32(v, p);
        bad = vorrq_u32(bad, vorrq_u32(matrix_nonfinite(p), matrix_nonfinite(v)));
        p = vmulq_n_f32(l3, right[c * 4 + 3]);
        v = vaddq_f32(v, p);
        bad = vorrq_u32(bad, vorrq_u32(matrix_nonfinite(p), matrix_nonfinite(v)));
        vst1q_f32(out.data() + c * 4, v);
    }
    return matrix_clean(bad);
#else
    (void)left, (void)right, (void)out;
    return false;
#endif
}

std::vector<SkinPaletteBone> make_skin_palette(std::span<const SkinMatrix> world,
    std::span<const SkinMatrix> inverse_bind, std::span<const SkinMatrix> scales) {
    check(!world.empty() && world.size() <= 21 && world.size() == inverse_bind.size() &&
          (scales.empty() || scales.size() == world.size()), "SKM palette count mismatch");
    std::vector<SkinPaletteBone> result;
    result.reserve(world.size());
    for (std::size_t i = 0; i < world.size(); ++i) {
#if OMEGA_SKIN_NEON
        {   // 132vc1 hw6 (port-side speed only): the same products on NEON; any doubt runs the scalar bone below
            SkinMatrix normal, position, scaled;
            if (skin_matrix_multiply_neon(world[i], inverse_bind[i], normal) &&
                (scales.empty() ? (position = normal, true)
                                : skin_matrix_multiply_neon(scales[i], inverse_bind[i], scaled) &&
                                      skin_matrix_multiply_neon(world[i], scaled, position))) {
                result.push_back({position, normal});
                continue;
            }
        }
#endif
        matrix_check(world[i]); matrix_check(inverse_bind[i]);
        if (!scales.empty()) matrix_check(scales[i]);
        const auto normal = multiply(world[i], inverse_bind[i]);
        const auto position = scales.empty() ? normal : multiply(world[i], multiply(scales[i], inverse_bind[i]));
        result.push_back({position, normal});
    }
    return result;
}

namespace {
// Checkpoint 132win9 (port-side speed only): skin_mesh_chunk's body writing into value-initialised storage the
// caller owns (skin_skinned_mesh writes each chunk straight into the mesh's vertex array instead of a vector per
// chunk); the same checks in the same order and the same arithmetic.
void skin_chunk_scalar(const SkinChunk& chunk, std::span<const SkinPaletteBone> palette,
                       std::optional<SkinMorphContext> morph, std::span<SkinnedVertex> output) {
    check(!morph || morph->active_targets == 0, "SKM active morph targets are not supported");
    const auto entry=chunk.microprogram_entry;
    if (entry == 12 || entry == 14) {
        // The flare programs (character program 0x518 / 0x570): every input vertex
        // (control.X) by the first record's palette position matrix only; the
        // normals and any further records are not read (research/COMBAT.md 131b).
        check(!chunk.vertices.empty() && chunk.vertices.size() <= 30 && !chunk.groups.empty() &&
              chunk.groups.size() <= 8, "SKM unsupported skin chunk");
        const auto& group = chunk.groups.front();
        check(group.bone < palette.size(), "SKM invalid skin influence");
        const auto& bone = palette[group.bone];
        matrix_check(bone.position);
        for (std::size_t i = 0; i < chunk.vertices.size(); ++i) {
            const auto& vertex = chunk.vertices[i];
            for (float v : vertex.uv) finite(v);
            output[i].uv = vertex.uv;
            output[i].position = transform(bone.position, vertex.position, true);
        }
        return;
    }
    check(entry==8 || entry==10 || entry==16 || entry==18 || entry==20 || entry==22 || entry==24 || entry==26 || entry==28 || entry==30,
          "SKM microprogram has no recovered model-space skinning contract");
    check((entry!=16 && entry!=18 && entry!=20 && entry!=22) || morph.has_value(),
          "SKM morph program requires explicit base-shape context");
    check((entry!=8 && entry!=10 && entry!=16 && entry!=18) || chunk.groups.size()==1,
          "SKM microprogram requires single-bone render branch");
    check(!chunk.vertices.empty() && chunk.vertices.size() <= 30 && !chunk.groups.empty() &&
          chunk.groups.size() <= 8, "SKM unsupported skin chunk");
    for (std::size_t i = 0; i < chunk.vertices.size(); ++i) {
        const auto& vertex = chunk.vertices[i];
        finite(vertex.position.x); finite(vertex.position.y); finite(vertex.position.z);
        finite(vertex.normal.x); finite(vertex.normal.y); finite(vertex.normal.z);
        for (float v : vertex.uv) finite(v);
        output[i].uv = vertex.uv;
    }
    for (std::size_t g = 0; g < chunk.groups.size(); ++g) {
        const auto& group = chunk.groups[g];
        check(group.bone < palette.size() && !group.weights.empty() &&
              group.first_vertex <= output.size() && group.weights.size() <= output.size() - group.first_vertex,
              "SKM invalid skin influence");
        if (g == 0) check(group.first_vertex == 0, "SKM unsupported first skin range");
        const auto& bone = palette[group.bone];
        matrix_check(bone.position); matrix_check(bone.normal);
        const bool weighted = chunk.groups.size() != 1;
        if (!weighted) for (float weight : group.weights)
            check(finite(weight) == 1, "SKM single influence must use implicit weight one");
        // The render program's single-bone branch uses control.X (all input
        // vertices), unlike the VIF0 query branch which uses group.Y.
        const auto affected = weighted ? group.weights.size() : output.size();
        for (std::size_t k = 0; k < affected; ++k) {
            const float weight = weighted ? finite(group.weights[k]) : 1.0f;
            const auto index = group.first_vertex + k;
            const auto position = transform(bone.position, chunk.vertices[index].position, true);
            const auto normal = transform(bone.normal, chunk.vertices[index].normal, false);
            for (std::size_t c = 0; c < 4; ++c) {
                const float p = weighted ? mul(position[c], weight) : position[c];
                const float n = weighted ? mul(normal[c], weight) : normal[c];
                output[index].position[c] = g == 0 ? p : add(output[index].position[c], p);
                output[index].normal[c] = g == 0 ? n : add(output[index].normal[c], n);
            }
        }
    }
}

#if OMEGA_SKIN_NEON
// Port-side speed only (132vc1, research/VITA.md "Render pass on the Vita's cores (132vc1)"; the same bits): the PS
// Vita skins a chunk's groups four lanes at a time. Each lane is the scalar path's arithmetic in its order (column 0
// times x, plus column 1 times y, plus column 2 times z, plus column 3 for a point; then the weight; then the sum
// over the groups), each operation a binary32 one: VMUL / VADD round like the VFP instructions, and Advanced SIMD's
// flush-to-zero is the VFP's under the play threads' FPSCR (FZ set, round to nearest), the only mode this path runs
// in (skin_neon_mode). The scalar path's checks are evaluated as conditions: a structural one that fails, an input
// or a result that is not finite (an infinity or NaN reaches the outputs, which are checked) or any other doubt
// returns false before or after writing, and the caller then runs the scalar path on value-initialised outputs,
// which gives the result or the error it always gave.
std::atomic<std::uint64_t> g_skin_neon_chunks{0}, g_skin_scalar_chunks{0};
bool skin_neon_mode() {
    std::uint32_t fpscr;
    asm volatile("vmrs %0, fpscr" : "=r"(fpscr));
    return (fpscr & 0x01f70000u) == 0x01000000u;  // FZ set; RMode nearest; Stride, Len 0
}
inline uint32x4_t skin_nonfinite(float32x4_t v) {
    const uint32x4_t e = vdupq_n_u32(0x7f800000u);
    return vceqq_u32(vandq_u32(vreinterpretq_u32_f32(v), e), e);
}
bool skin_chunk_neon(const SkinChunk& chunk, std::span<const SkinPaletteBone> palette, bool has_morph,
                     std::span<SkinnedVertex> output) {
    const auto entry = chunk.microprogram_entry;
    if (!(entry == 8 || entry == 10 || entry == 16 || entry == 18 || entry == 20 || entry == 22 || entry == 24 ||
          entry == 26 || entry == 28 || entry == 30))
        return false;
    if ((entry == 16 || entry == 18 || entry == 20 || entry == 22) && !has_morph) return false;
    if ((entry == 8 || entry == 10 || entry == 16 || entry == 18) && chunk.groups.size() != 1) return false;
    if (chunk.vertices.empty() || chunk.vertices.size() > 30 || chunk.groups.empty() || chunk.groups.size() > 8 ||
        output.size() != chunk.vertices.size())
        return false;
    static_assert(sizeof(SkinVertex) == 32 && sizeof(SkinnedVertex) == 40, "SkinVertex / SkinnedVertex layout");
    uint32x4_t bad = vdupq_n_u32(0);
    for (std::size_t i = 0; i < chunk.vertices.size(); ++i) {
        const float* v = &chunk.vertices[i].position.x;  // position xyz, normal xyz, uv
        bad = vorrq_u32(bad, vorrq_u32(skin_nonfinite(vld1q_f32(v)), skin_nonfinite(vld1q_f32(v + 4))));
        output[i].uv = chunk.vertices[i].uv;
    }
    for (std::size_t g = 0; g < chunk.groups.size(); ++g) {
        const auto& group = chunk.groups[g];
        if (!(group.bone < palette.size() && !group.weights.empty() && group.first_vertex <= output.size() &&
              group.weights.size() <= output.size() - group.first_vertex))
            return false;
        if (g == 0 && group.first_vertex != 0) return false;
        const auto& bone = palette[group.bone];
        const float* pm = bone.position.data();
        const float* nm = bone.normal.data();
        const float32x4_t p0 = vld1q_f32(pm), p1 = vld1q_f32(pm + 4), p2 = vld1q_f32(pm + 8), p3 = vld1q_f32(pm + 12);
        const float32x4_t n0 = vld1q_f32(nm), n1 = vld1q_f32(nm + 4), n2 = vld1q_f32(nm + 8), n3 = vld1q_f32(nm + 12);
        bad = vorrq_u32(bad, vorrq_u32(vorrq_u32(skin_nonfinite(p0), skin_nonfinite(p1)),
                                       vorrq_u32(skin_nonfinite(p2), skin_nonfinite(p3))));
        bad = vorrq_u32(bad, vorrq_u32(vorrq_u32(skin_nonfinite(n0), skin_nonfinite(n1)),
                                       vorrq_u32(skin_nonfinite(n2), skin_nonfinite(n3))));
        const bool weighted = chunk.groups.size() != 1;
        if (!weighted)
            for (const float& weight : group.weights) {
                // the scalar check (an infinity or NaN is not 1 either), on the bits: 1.0f has one encoding, and an
                // integer compare needs no VFP compare and status transfer (132vc1 hw6)
                std::uint32_t bits;
                std::memcpy(&bits, &weight, sizeof bits);
                if (bits != 0x3f800000u) return false;
            }
        const std::size_t affected = weighted ? group.weights.size() : output.size();
        for (std::size_t k = 0; k < affected; ++k) {
            const std::size_t index = group.first_vertex + k;
            const SkinVertex& in = chunk.vertices[index];
            float32x4_t p = vmulq_n_f32(p0, in.position.x);
            p = vaddq_f32(p, vmulq_n_f32(p1, in.position.y));
            p = vaddq_f32(p, vmulq_n_f32(p2, in.position.z));
            p = vaddq_f32(p, p3);
            float32x4_t n = vmulq_n_f32(n0, in.normal.x);
            n = vaddq_f32(n, vmulq_n_f32(n1, in.normal.y));
            n = vaddq_f32(n, vmulq_n_f32(n2, in.normal.z));
            if (weighted) {
                const float weight = group.weights[k];
                bad = vorrq_u32(bad, skin_nonfinite(vdupq_n_f32(weight)));
                p = vmulq_n_f32(p, weight);
                n = vmulq_n_f32(n, weight);
            }
            float* out_p = output[index].position.data();
            float* out_n = output[index].normal.data();
            if (g != 0) {
                p = vaddq_f32(vld1q_f32(out_p), p);
                n = vaddq_f32(vld1q_f32(out_n), n);
            }
            // An infinity or NaN anywhere in the lane's operations stays one in its result (no later operation
            // here turns it finite), so checking the results checks every operation the scalar path checks.
            bad = vorrq_u32(bad, vorrq_u32(skin_nonfinite(p), skin_nonfinite(n)));
            vst1q_f32(out_p, p);
            vst1q_f32(out_n, n);
        }
    }
    const uint32x2_t h = vorr_u32(vget_low_u32(bad), vget_high_u32(bad));
    return vget_lane_u32(vpmax_u32(h, h), 0) == 0;
}
#endif

void skin_chunk_into(const SkinChunk& chunk, std::span<const SkinPaletteBone> palette,
                     std::optional<SkinMorphContext> morph, std::span<SkinnedVertex> output) {
#if OMEGA_SKIN_NEON
    if ((!morph || morph->active_targets == 0) && chunk.microprogram_entry != 12 && chunk.microprogram_entry != 14 &&
        skin_neon_mode()) {
#if defined(OMEGA_SKIN_NEON_CHECK)
        // The check build (-DOMEGA_SKIN_NEON_CHECK): a chunk NEON took is skinned again by the scalar path and compared.
        const std::vector<SkinnedVertex> before(output.begin(), output.end());
        if (skin_chunk_neon(chunk, palette, morph.has_value(), output)) {
            const std::vector<SkinnedVertex> neon(output.begin(), output.end());
            std::copy(before.begin(), before.end(), output.begin());
            skin_chunk_scalar(chunk, palette, morph, output);
            if (std::memcmp(neon.data(), output.data(), neon.size() * sizeof(SkinnedVertex)) != 0)
                fail("SKM NEON skinning check: a chunk differs from the scalar path");
            g_skin_neon_chunks.fetch_add(1, std::memory_order_relaxed);
            static std::atomic<std::uint64_t> checked{0};
            if ((checked.fetch_add(1, std::memory_order_relaxed) & 0xffffu) == 0xffffu)
                std::cerr << "SKM NEON skinning check: " << checked.load() << " chunks compared, 0 differences\n";
            return;
        }
#else
        if (skin_chunk_neon(chunk, palette, morph.has_value(), output)) {
            g_skin_neon_chunks.fetch_add(1, std::memory_order_relaxed);
            return;
        }
#endif
        std::fill(output.begin(), output.end(), SkinnedVertex{});  // as the caller's resize left them
    }
    g_skin_scalar_chunks.fetch_add(1, std::memory_order_relaxed);
#endif
    skin_chunk_scalar(chunk, palette, morph, output);
}
}  // namespace

void skin_neon_counts(std::uint64_t& neon, std::uint64_t& scalar) {
#if OMEGA_SKIN_NEON
    neon = g_skin_neon_chunks.load(std::memory_order_relaxed);
    scalar = g_skin_scalar_chunks.load(std::memory_order_relaxed);
#else
    neon = scalar = 0;
#endif
}

std::vector<SkinnedVertex> skin_mesh_chunk(const SkinChunk& chunk,
    std::span<const SkinPaletteBone> palette, std::optional<SkinMorphContext> morph) {
    std::vector<SkinnedVertex> output(chunk.vertices.size());
    skin_chunk_into(chunk, palette, morph, output);
    return output;
}

std::vector<std::array<std::uint32_t, 3>> skinned_mesh_triangles(const SkinChunk& chunk) {
    std::vector<std::array<std::uint32_t, 3>> triangles;
    for (std::size_t i = 0; i < chunk.primitives.size(); ++i) {
        check(chunk.primitives[i].vertex_index < 38, "SKM triangle outside supported output slots");
        if (i < 2 || chunk.primitives[i].adc) continue;
        auto a = chunk.primitives[i - 2].vertex_index, b = chunk.primitives[i - 1].vertex_index;
        if (i % 2) std::swap(a, b);
        triangles.push_back({a, b, chunk.primitives[i].vertex_index});
    }
    return triangles;
}

SkinnedMeshGeometry skin_skinned_mesh(const SkinnedMesh& mesh,
    std::span<const SkinPaletteBone> palette, std::optional<SkinMorphContext> morph) {
    SkinnedMeshGeometry result;
    skin_skinned_mesh_into(mesh, palette, morph, result);
    return result;
}

// Checkpoint 132win10 (port-side speed only): the same skinning into caller-owned geometry, its vectors cleared and
// refilled so a per-frame caller reuses their capacity instead of allocating three vectors per part per frame.
void skin_skinned_mesh_into(const SkinnedMesh& mesh, std::span<const SkinPaletteBone> palette,
    std::optional<SkinMorphContext> morph, SkinnedMeshGeometry& result) {
    result.vertices.clear(); result.triangles.clear(); result.triangle_chunks.clear();
    check(!morph || morph->active_targets == 0, "SKM active morph targets are not supported");
    check(!mesh.chunks.empty() && mesh.chunks.size() <= 127, "SKM unsupported mesh chunk count");
    std::array<std::uint32_t, 38> slots{};
    slots.fill(std::numeric_limits<std::uint32_t>::max());
    // Checkpoint 132win9 (port-side speed only): sized once, each chunk skinned in place and its strip walked
    // inline (skinned_mesh_triangles' walk; the slot check above it already bounds every index below 38).
    std::size_t vertex_total = 0, primitive_total = 0;
    for (const auto& chunk : mesh.chunks) { vertex_total += chunk.vertices.size(); primitive_total += chunk.primitives.size(); }
    result.vertices.reserve(vertex_total);
    result.triangles.reserve(primitive_total);
    result.triangle_chunks.reserve(primitive_total);
    for (std::size_t c = 0; c < mesh.chunks.size(); ++c) {
        const auto& chunk = mesh.chunks[c];
#if OMEGA_SKIN_NEON
        if (c + 1 < mesh.chunks.size()) {  // 132vc1 hw5 (port-side speed only): the next chunk's inputs on their way
            const auto& next = mesh.chunks[c + 1];
            const char* in = reinterpret_cast<const char*>(next.vertices.data());
            for (std::size_t b = 0; b < next.vertices.size() * sizeof(SkinVertex); b += 32) __builtin_prefetch(in + b);
            for (const auto& group : next.groups) __builtin_prefetch(group.weights.data());
        }
#endif
        const auto first = result.vertices.size(), count = chunk.vertices.size();
        result.vertices.resize(first + count);
        skin_chunk_into(chunk, palette, morph, std::span<SkinnedVertex>(result.vertices).subspan(first, count));
        check(chunk.output_base <= slots.size() && count <= slots.size() - chunk.output_base,
              "SKM skin output range exceeds retained slots");
        for (std::size_t i = 0; i < count; ++i) slots[chunk.output_base + i] = static_cast<std::uint32_t>(first + i);
        for (const auto& primitive : chunk.primitives) {
            check(primitive.vertex_index < slots.size() &&
                  slots[primitive.vertex_index] != std::numeric_limits<std::uint32_t>::max(),
                  "SKM skin references unwritten output slot");
        }
        for (std::size_t i = 2; i < chunk.primitives.size(); ++i) {
            if (chunk.primitives[i].adc) continue;
            auto a = chunk.primitives[i - 2].vertex_index, b = chunk.primitives[i - 1].vertex_index;
            if (i % 2) std::swap(a, b);
            result.triangles.push_back({slots[a], slots[b], slots[chunk.primitives[i].vertex_index]});
            result.triangle_chunks.push_back(static_cast<std::uint32_t>(c));
        }
    }
}

namespace {
// deform_skinned_mesh's checks and deltas writing into caller-owned storage; false (output untouched) when the
// targets leave the mesh unchanged.
bool deform_skinned_mesh_into(const SkinnedMesh& mesh, const MorphData& morph,
    std::span<const SkinMorphTarget> targets, SkinnedMesh& result) {
    const bool face = morph.kind == MorphKind::face;
    check(face || morph.kind == MorphKind::hand, "SKM unknown morph kind");
    check(!mesh.chunks.empty() && mesh.chunks.size() <= 127 && morph.chunk_count == mesh.chunks.size(),
          "SKM morph chunk count mismatch");
    check(!morph.shapes.empty() && morph.shapes.size()<=256, "SKM morph shape count outside bounded domain");
    check(targets.size() <= (face ? 8U : 2U), "SKM morph target limit");
    // Checkpoint 132win10 (port-side speed only): std::isfinite && fpclassify != FP_SUBNORMAL on the bits.
    auto scalar = [](float v) {
        const auto bits = std::bit_cast<std::uint32_t>(v), exponent = bits & 0x7f800000u;
        check(exponent != 0x7f800000u && (exponent != 0 || (bits & 0x007fffffu) == 0), "SKM unsupported morph scalar or arithmetic");
        return v;
    };
    for (const auto& target : targets) {
        scalar(target.weight);
        check(target.shape_index < morph.shapes.size(), "SKM morph shape index outside table");
        const auto& shape = morph.shapes[target.shape_index];
        check(shape.chunks.size() == mesh.chunks.size(), "SKM malformed morph shape chunk count");
        for (const auto& block : shape.chunks) {
            check(face || block.has_value(), "SKM dense hand shape has absent block");
            check(!block || block->size() == (face ? 32U : 48U), "SKM morph block extent");
        }
    }
    for (const auto& chunk : mesh.chunks) {
        check(face ? (chunk.microprogram_entry == 20 || chunk.microprogram_entry == 22) :
                     (chunk.microprogram_entry == 16 || chunk.microprogram_entry == 18),
              "SKM shape kind does not match microprogram");
        check(!chunk.vertices.empty() && chunk.vertices.size() <= (face ? 8U : 16U),
              "SKM morph vertex count outside delta bank");
        check(face || chunk.groups.size() == 1, "SKM hand morph requires single-bone render branch");
        for (const auto& v : chunk.vertices) {
            for (float x : {v.position.x,v.position.y,v.position.z,v.normal.x,v.normal.y,v.normal.z,v.uv[0],v.uv[1]}) scalar(x);
        }
    }
    if (targets.empty() || (face && targets.size()==1 && targets[0].weight==0) ||
        (!face && targets[0].weight==0 && (targets.size()==1 || targets[1].weight==0))) return false;
    result = mesh;
    for (std::size_t c=0;c<result.chunks.size();++c) {
        auto& chunk=result.chunks[c];
        auto delta = [&](std::size_t slot,std::size_t vertex,unsigned component) {
            const auto& block=morph.shapes[targets[slot].shape_index].chunks[c];
            if (!block) return 0.0F; // CPU initializes missing sparse blocks with unsigned byte 128.
            return static_cast<float>(int((*block)[vertex*(face?4:3)+component])-128)/256.0F;
        };
        const auto used = !face && targets.size()==2 && targets[1].weight==0 ? 1U : targets.size();
        for (std::size_t i=0;i<chunk.vertices.size();++i) {
            auto& v=chunk.vertices[i];
            std::array<float*,4> fields{&v.position.x,&v.position.y,&v.position.z,&v.normal.z};
            for (unsigned component=0;component<(face?4U:3U);++component) {
                float sum=scalar(delta(0,i,component)*targets[0].weight);
                for (std::size_t t=1;t<used;++t)
                    sum=scalar(sum+scalar(delta(t,i,component)*targets[t].weight));
                *fields[component]=scalar(sum+*fields[component]);
            }
        }
    }
    return true;
}
}  // namespace

SkinnedMesh deform_skinned_mesh(const SkinnedMesh& mesh, const MorphData& morph,
    std::span<const SkinMorphTarget> targets) {
    SkinnedMesh result;
    if (!deform_skinned_mesh_into(mesh, morph, targets, result)) return mesh;
    return result;
}

SkinnedMeshGeometry skin_morphed_mesh(const SkinnedMesh& mesh,
    std::span<const SkinPaletteBone> palette,const MorphData& morph,
    std::span<const SkinMorphTarget> targets) {
    SkinnedMesh scratch;
    SkinnedMeshGeometry result;
    skin_morphed_mesh_into(mesh, palette, morph, targets, scratch, result);
    return result;
}

void skin_morphed_mesh_into(const SkinnedMesh& mesh, std::span<const SkinPaletteBone> palette, const MorphData& morph,
    std::span<const SkinMorphTarget> targets, SkinnedMesh& scratch, SkinnedMeshGeometry& output) {
    if (morph.kind==MorphKind::face && !targets.empty() && !(targets.size()==1 && targets[0].weight==0))
        for (const auto& chunk:mesh.chunks)
            if (chunk.groups.size()==1)
                check(chunk.groups[0].first_vertex==0 && chunk.groups[0].weights.size()==chunk.vertices.size(),
                      "SKM active face single-bone range differs from render vertex count");
    // An unchanged mesh is skinned directly (deform_skinned_mesh would return an identical copy).
    const bool deformed = deform_skinned_mesh_into(mesh, morph, targets, scratch);
    skin_skinned_mesh_into(deformed ? scratch : mesh, palette, SkinMorphContext{0}, output);
}

void write_skinned_mesh(std::ostream& out, const SkinnedMesh& mesh) {
    const auto previous_precision = out.precision(std::numeric_limits<float>::max_digits10);
    out << "{\"bone_count\":" << mesh.bone_count << ",\"chunks\":[";
    bool first_chunk = true;
    for (const auto& chunk : mesh.chunks) {
        if (!first_chunk) out << ','; first_chunk = false;
        out << "{\"packet_quadwords\":" << chunk.packet_quadwords << ",\"output_base\":" << chunk.output_base
            << ",\"control_flags\":" << chunk.control_flags << ",\"microprogram_entry\":" << chunk.microprogram_entry
            << ",\"microprogram_entries\":";
        write_values(out, chunk.microprogram_entries);
        out << ",\"initial_constants\":";
        if (chunk.initial_constants) write_values(out, *chunk.initial_constants); else out << "null";
        out << ",\"vertices\":["; bool first = true;
        for (const auto& v : chunk.vertices) {
            if (!first) out << ','; first = false;
            out << "{\"position\":"; write_vec3(out, v.position);
            out << ",\"normal\":"; write_vec3(out, v.normal);
            out << ",\"uv\":"; write_values(out, v.uv); out << '}';
        }
        out << "],\"groups\":["; first = true;
        for (const auto& g : chunk.groups) {
            if (!first) out << ','; first = false;
            out << "{\"output_address\":" << g.output_address << ",\"palette_address\":" << g.palette_address
                << ",\"bone\":" << g.bone << ",\"first_vertex\":" << g.first_vertex << ",\"weights\":";
            write_values(out, g.weights); out << '}';
        }
        out << "],\"primitives\":["; first = true;
        for (const auto& p : chunk.primitives) {
            if (!first) out << ','; first = false;
            out << "{\"vertex_index\":" << p.vertex_index << ",\"adc\":" << (p.adc ? "true" : "false")
                << ",\"raw_index\":" << unsigned(p.raw_index) << '}';
        }
        out << "],\"triangles\":["; first = true;
        for (const auto& triangle : skinned_mesh_triangles(chunk)) {
            if (!first) out << ','; first = false; write_values(out, triangle);
        }
        out << "]}";
    }
    out << "]}";
    out.precision(previous_precision);
}
}
