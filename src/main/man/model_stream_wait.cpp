#include "model_stream_wait.hpp"

#include <stdexcept>
#include <string>

namespace omega {

std::int8_t model_stream_wait(ModelStreamWaitModel& model, const ModelStreamWaitHost& host) {
    if (!host.head_resident || !host.cache_record || !host.texture_pending || !host.attach_points)
        throw std::runtime_error("00389a50: the stream-wait host is incomplete");
    const std::uint16_t count = model.part_count;   // lhu +23c0
    if (count > 32) throw std::runtime_error("00389a50: +23c0 past the 32 part entries");
    const auto bump = [&] { model.streaming_2bdb = static_cast<std::int8_t>(static_cast<std::uint8_t>(model.streaming_2bdb) + 1U); };
    model.streaming_2bdb = 0;
    if (const auto head = host.head_resident(); head && !*head) bump();   // 0x00389a74..0x00389aa8
    for (int t = 0; t < 2; ++t) {                                           // 0x00389ac8..0x00389bcc
        for (std::uint16_t i = 0; i < count; ++i) {
            ModelPartStreamEntry& e = model.parts[t][i];
            if (e.mesh != 0 || e.cache_index == 0) continue;
            const std::uint16_t index = e.cache_index;
            const ModelPartStreamEntry record = host.cache_record(index, t);
            if (record.mesh == 0) { bump(); continue; }
            e = record;
            model.parts[t ^ 1][i] = host.cache_record(index, t ^ 1);
        }
    }
    for (std::uint16_t i = 0; i < count; ++i) {                             // 0x00389bd4..0x00389f6c
        for (const std::int16_t h : {model.textures_lod0[i], model.textures_lod1[i]}) {
            if (h < 0) throw std::runtime_error("00389a50: negative texture handle " + std::to_string(h));
            if (host.texture_pending(h)) bump();
        }
    }
    if (model.streaming_2bdb == 0) {                                        // 0x00389f70..0x00389f80
        model.byte_2bdc = 0;
        host.attach_points();
    }
    return model.streaming_2bdb;
}

}  // namespace omega
