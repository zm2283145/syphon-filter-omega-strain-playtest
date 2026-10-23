#include "clip_playback_sequence.hpp"
#include "animation_sampling.hpp"
#include "animation_root_clip.hpp"
#include <fstream>
#include <sstream>
#include <stdexcept>
namespace omega {
void write_clip_playback_sequence(std::ostream& output,const AnimationClip& clip,
    const SkeletonDefinition& skeleton,const std::filesystem::path& path) {
    if(std::filesystem::file_size(path)>1024*1024) throw std::runtime_error("Clip playback cases too large");
    std::ifstream input(path);std::string magic;unsigned version{},count{};
    if(!(input>>magic>>version>>count)||magic!="OMEGA_CLIP_PLAYBACK"||version!=1||count>4096)
        throw std::runtime_error("Invalid clip playback case header");
    std::ostringstream report;report<<"{\"cases\":[";
    for(unsigned i=0;i<count;++i) {
        float phase{},advance{};unsigned reverse{},mirror{},loop{};ClipPlaybackControl control;
        RootVector rotation;
        if(!(input>>phase>>advance>>reverse>>mirror>>loop>>control.range_start>>control.range_end>>control.phase_offset
            >>rotation[0]>>rotation[1]>>rotation[2]>>rotation[3])||reverse>1||mirror>1||loop>1)
            throw std::runtime_error("Invalid clip playback case");
        control.reverse=reverse!=0;control.mirror=mirror!=0;control.looping=loop!=0;
        const auto bones=sample_nonroot_bones(clip,skeleton,phase,control);
        const auto root=sample_root_clip(clip,skeleton,phase,advance,control,rotation);
        if(i)report<<',';report<<"{\"nonroot\":";write_nonroot_clip_sample(report,bones);
        report<<",\"root\":";write_root_clip_sample(report,root);report<<'}';
    }
    std::string extra;if(input>>extra) throw std::runtime_error("Trailing clip playback data");
    report<<"]}";output<<report.str();
}
}
