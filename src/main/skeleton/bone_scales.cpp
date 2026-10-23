#include "bone_scales.hpp"
#include <bit>
#include <cmath>
#include <iomanip>
#include <stdexcept>
namespace omega {
BoneScalePreset decode_bone_scales(std::span<const std::uint8_t> bytes,unsigned bones) {
    if(!bones||bones>21||bytes.size()<16*bones||bytes.size()>1024*1024)
        throw std::runtime_error("Bone scale preset extent");
    auto word=[&](std::size_t p){return std::uint32_t(bytes[p])|(std::uint32_t(bytes[p+1])<<8)|
        (std::uint32_t(bytes[p+2])<<16)|(std::uint32_t(bytes[p+3])<<24);};
    BoneScalePreset result;result.stored_bytes=bytes.size();
    for(unsigned i=0;i<bones;++i){
        Vec3 s{std::bit_cast<float>(word(i*16)),std::bit_cast<float>(word(i*16+4)),std::bit_cast<float>(word(i*16+8))};
        if(!std::isfinite(s.x)||!std::isfinite(s.y)||!std::isfinite(s.z))throw std::runtime_error("Nonfinite bone scale");
        result.scales.push_back(s);result.unused_words.push_back(word(i*16+12));
    }
    return result;
}
std::vector<SkinMatrix> bone_scale_matrices(const BoneScalePreset& preset) {
    if(preset.scales.empty()||preset.scales.size()>21)throw std::runtime_error("Bone scale matrix count");
    std::vector<SkinMatrix> result;
    for(const auto& s:preset.scales){
        if(!std::isfinite(s.x)||!std::isfinite(s.y)||!std::isfinite(s.z))throw std::runtime_error("Nonfinite bone scale");
        result.push_back({s.x,0,0,0,0,s.y,0,0,0,0,s.z,0,0,0,0,1});
    }
    return result;
}
void write_bone_scales(std::ostream& out,const BoneScalePreset& preset) {
    if(preset.scales.size()!=preset.unused_words.size())throw std::runtime_error("Bone scale metadata mismatch");
    out<<std::setprecision(9)<<"{\"stored_bytes\":"<<preset.stored_bytes<<",\"scales\":[";
    for(std::size_t i=0;i<preset.scales.size();++i){if(i)out<<',';const auto& s=preset.scales[i];out<<'['<<s.x<<','<<s.y<<','<<s.z<<']';}
    out<<"],\"unused_words\":[";for(std::size_t i=0;i<preset.unused_words.size();++i){if(i)out<<',';out<<preset.unused_words[i];}out<<"]}";
}
}
