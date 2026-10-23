#include "morph_host_sequence.hpp"
#include <cmath>
#include <fstream>
#include <iomanip>
#include <set>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class T>T read(std::istream& input){T value{};check(bool(input>>value),"Invalid morph host sequence value");return value;}
long long integer(std::istream& input,long long minimum,long long maximum){const auto value=read<long long>(input);check(value>=minimum&&value<=maximum,"Morph host sequence integer outside bounds");return value;}
float scalar(std::istream& input){const auto value=read<float>(input);check(std::isfinite(value)&&std::fpclassify(value)!=FP_SUBNORMAL,"Invalid morph sequence scalar");return value;}
void token(std::istream& input,const char* value){check(read<std::string>(input)==value,"Invalid morph host sequence token");}
void target(std::ostream& out,const SkinMorphTarget& value){out<<"{\"shape_index\":"<<value.shape_index<<",\"weight\":"<<value.weight<<'}';}
template<class Range>void targets(std::ostream& out,const Range& values){out<<'[';bool first=true;for(const auto& value:values){if(!first)out<<',';first=false;target(out,value);}out<<']';}
}
void write_morph_host_state(std::ostream& out,const MorphHostState& state){
    out<<std::setprecision(9)<<"{\"base_enabled\":"<<(state.base_enabled?"true":"false")
       <<",\"speech_active\":"<<(state.speech_active?"true":"false")<<",\"face_slots\":";
    targets(out,state.face_slots);out<<",\"active_count\":"<<unsigned(state.active_count)<<",\"grips\":[";
    for(std::size_t i=0;i<2;++i){if(i)out<<',';const auto& g=state.grips[i];
        out<<"{\"counter\":"<<unsigned(g.counter)<<",\"primary_shape\":"<<unsigned(g.primary_shape)
           <<",\"secondary_shape\":"<<unsigned(g.secondary_shape)<<",\"primary_weight\":"<<g.primary_weight
           <<",\"secondary_weight\":"<<g.secondary_weight<<'}';}
    out<<"],\"active_expression_counter\":"<<unsigned(state.active_expression_counter)
       <<",\"periodic_counter\":"<<state.periodic_counter<<",\"periodic_reset\":"<<state.periodic_reset<<",\"speech\":[";
    for(std::size_t i=0;i<21;++i){if(i)out<<',';const auto& c=state.speech[i];out<<"{\"keys\":[";
        for(std::size_t k=0;k<c.keys.size();++k){if(k)out<<',';out<<"{\"time\":"<<c.keys[k].time<<",\"weight\":"<<c.keys[k].weight<<'}';}
        out<<"],\"index\":"<<c.index<<",\"previous_time\":"<<c.previous_time<<",\"next_time\":"<<c.next_time
           <<",\"previous_weight\":"<<c.previous_weight<<",\"next_weight\":"<<c.next_weight<<'}';}
    out<<"],\"face_targets\":";targets(out,morph_face_targets(state));out<<",\"hand_targets\":[";
    targets(out,morph_hand_targets(state,0));out<<',';targets(out,morph_hand_targets(state,1));out<<"]}";
}
void write_morph_host_sequence(std::ostream& out,const std::filesystem::path& path){
    namespace fs=std::filesystem;
    check(fs::file_size(path)<=16*1024*1024,"Morph sequence file limit");std::ifstream input(path);
    token(input,"OMEGA_MORPH_HOST_CASES");check(integer(input,1,1)==1,"Morph sequence version");
    const auto cases=integer(input,1,128);std::uint64_t total=0,key_snapshots=0;std::set<std::string> names;
    out<<"{\"scope\":\"Actor morph producers with explicit resolved resources, clock, random value and head-space gaze\",\"cases\":[";
    for(long long c=0;c<cases;++c){
        std::string name,config_path;check(bool(input>>std::quoted(name)>>std::quoted(config_path)),"Invalid morph sequence case header");
        check(!name.empty()&&name.size()<=128&&names.insert(name).second,"Invalid or duplicate morph case name");
        for(unsigned char value:name)check(value>=32&&value<=126,"Nonprintable morph case name");
        check(fs::file_size(config_path)<=1024*1024,"Morph config size limit");std::ifstream config_stream(config_path);
        const auto config=read_morph_host_config(config_stream);const auto count=integer(input,0,1024);total+=count;check(total<=8192,"Morph sequence operation limit");
        MorphHostState state;
        if(c)out<<',';out<<"{\"name\":"<<std::quoted(name)<<",\"initial\":";write_morph_host_state(out,state);out<<",\"steps\":[";
        for(long long s=0;s<count;++s){
            const auto op=read<std::string>(input);
            if(op=="BASE"){
                const auto shape=integer(input,0,255);const bool enabled=integer(input,0,1)!=0;
                set_morph_base_shape(state,static_cast<std::uint8_t>(shape),enabled);
            }else if(op=="GRIP"){
                const auto hand=integer(input,0,1);auto& g=state.grips[hand];
                g.counter=static_cast<std::uint8_t>(integer(input,0,255));g.primary_shape=static_cast<std::uint8_t>(integer(input,0,255));
                g.secondary_shape=static_cast<std::uint8_t>(integer(input,0,255));g.primary_weight=scalar(input);g.secondary_weight=scalar(input);
            }else if(op=="EXPRESSION"){
                state.active_expression_counter=static_cast<std::uint8_t>(integer(input,0,255));
                state.periodic_counter=static_cast<std::int16_t>(integer(input,-32768,32767));state.periodic_reset=static_cast<std::uint16_t>(integer(input,0,65535));
            }else if(op=="SPEECH"){
                for(auto& channel:state.speech){
                    token(input,"KEYS");const auto n=integer(input,0,1024);channel.keys.clear();
                    channel.index=static_cast<std::uint16_t>(integer(input,0,65535));
                    channel.previous_time=static_cast<std::int16_t>(integer(input,-32768,32767));channel.next_time=static_cast<std::int16_t>(integer(input,-32768,32767));
                    channel.previous_weight=scalar(input);channel.next_weight=scalar(input);
                    for(long long k=0;k<n;++k){const auto time=integer(input,-32768,32767),weight=integer(input,0,65535);channel.keys.push_back({static_cast<std::int16_t>(time),static_cast<std::uint16_t>(weight)});}
                }
                state.speech_active=true;
            }else if(op=="LOAD_SPEECH"){
                std::string filename;check(bool(input>>std::quoted(filename)),"Invalid speech filename");
                const auto size=fs::file_size(filename);check(size<=4*1024*1024,"Speech input limit");
                std::ifstream source(filename,std::ios::binary);std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
                check(bool(source.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(size))),"Cannot read speech input");
                start_morph_speech(state,decode_morph_speech(bytes));
            }else if(op=="STOP_SPEECH")state.speech_active=false;
            else if(op=="STEP"){
                MorphHostInput frame;frame.expressions_enabled=integer(input,0,1)!=0;frame.speech_elapsed_tick=static_cast<std::int32_t>(integer(input,-2147483648LL,2147483647));
                const auto random=integer(input,-1,4294967295LL);if(random>=0)frame.periodic_random=static_cast<std::uint32_t>(random);
                const bool gaze=integer(input,0,1)!=0;std::array<float,3> direction{};for(auto& value:direction)value=scalar(input);if(gaze)frame.local_gaze=direction;
                for(auto& hand:frame.secondary_grip_shapes){const auto shape=integer(input,-1,255);if(shape>=0)hand=static_cast<std::uint8_t>(shape);}
                update_morph_host(state,config,frame);
            }else throw std::runtime_error("Unknown morph host operation");
            for(const auto& channel:state.speech)key_snapshots+=channel.keys.size();
            check(key_snapshots<=1000000,"Morph sequence repeated key output limit");
            if(s)out<<',';out<<"{\"op\":"<<std::quoted(op)<<",\"state\":";write_morph_host_state(out,state);out<<'}';
        }
        out<<"]}";
    }
    std::string trailing;check(!(input>>trailing)&&input.eof(),"Trailing morph sequence input");out<<"]}";
}
}
