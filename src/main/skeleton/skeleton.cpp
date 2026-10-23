#include "skeleton.hpp"
#include "binary.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <iomanip>
#include <stdexcept>

namespace omega {
SkeletonDefinition decode_skeleton(std::span<const std::uint8_t> bytes) {
    const auto count = read_u32(bytes, 0);
    if (!count || count > 256) throw std::runtime_error("Skeleton count outside supported range 1..256");
    if (bytes.size() != 16 + std::size_t(count) * 32)
        throw std::runtime_error("Skeleton record extent does not match count");
    SkeletonDefinition result;
    for (std::size_t i = 0; i < 3; ++i) result.header_words[i] = read_u32(bytes, 4 + i * 4);
    result.bones.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto record = bytes.subspan(16 + i * 32, 32);
        SkeletonBone bone;
        bone.parent = record[0] == 255 ? -1 : record[0];
        if (bone.parent >= static_cast<int>(count) || bone.parent == static_cast<int>(i))
            throw std::runtime_error("Invalid skeleton parent index");
        bone.local_offset = {std::bit_cast<float>(read_u32(record, 8)),
                             std::bit_cast<float>(read_u32(record, 12)),
                             std::bit_cast<float>(read_u32(record, 16))};
        if (!std::isfinite(bone.local_offset.x) || !std::isfinite(bone.local_offset.y) ||
            !std::isfinite(bone.local_offset.z)) throw std::runtime_error("Nonfinite skeleton local offset");
        std::copy(record.begin(), record.end(), bone.record_bytes.begin());
        result.bones.push_back(bone);
    }
    // Parent order need not be topological. Reject cycles without assuming file ordering.
    for (std::size_t i = 0; i < count; ++i) {
        auto parent = static_cast<int>(i);
        unsigned steps = 0;
        while (parent >= 0) {
            if (++steps > count) throw std::runtime_error("Skeleton parent cycle");
            parent = result.bones[static_cast<std::size_t>(parent)].parent;
        }
    }
    return result;
}

void write_skeleton(std::ostream& out, const SkeletonDefinition& skeleton) {
    out << std::setprecision(9) << "{\"header_words\":[";
    for (std::size_t i = 0; i < skeleton.header_words.size(); ++i) {
        if (i) out << ',';
        out << skeleton.header_words[i];
    }
    out << "],\"bones\":[";
    for (std::size_t i = 0; i < skeleton.bones.size(); ++i) {
        if (i) out << ',';
        const auto& bone = skeleton.bones[i];
        out << "{\"parent\":" << bone.parent << ",\"local_offset\":[" << bone.local_offset.x << ','
            << bone.local_offset.y << ',' << bone.local_offset.z << "],\"record_bytes\":[";
        for (std::size_t j = 0; j < bone.record_bytes.size(); ++j) {
            if (j) out << ',';
            out << static_cast<unsigned>(bone.record_bytes[j]);
        }
        out << "]}";
    }
    out << "]}";
}
}
