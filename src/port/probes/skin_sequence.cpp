#include "skin_sequence.hpp"
#include <cmath>
#include <fstream>
#include <iomanip>
#include <stdexcept>

namespace omega {
namespace {
template<class T>T read(std::istream& input){T value{};if(!(input>>value))throw std::runtime_error("Invalid skin sequence value");return value;}
void token(std::istream& input,const char* expected){if(read<std::string>(input)!=expected)throw std::runtime_error("Invalid skin sequence token");}
std::vector<SkinMatrix> matrices(std::istream& input,unsigned count){
    std::vector<SkinMatrix> result(count);
    for(auto& matrix:result)for(auto& value:matrix){value=read<float>(input);if(!std::isfinite(value))throw std::runtime_error("Nonfinite skin matrix");}
    return result;
}
template<class T>void array(std::ostream& out,const T& values){out<<'[';bool first=true;for(auto value:values){if(!first)out<<',';first=false;out<<value;}out<<']';}
}
void write_skin_sequence(std::ostream& output,const SkinnedMesh& mesh,
    const std::filesystem::path& path,const std::filesystem::path& destination){
    namespace fs=std::filesystem;
    if(fs::absolute(path).lexically_normal()==fs::absolute(destination).lexically_normal()||
        (fs::exists(destination)&&fs::equivalent(path,destination)))throw std::runtime_error("Skin output would overwrite sequence input");
    if(fs::file_size(path)>16*1024*1024)throw std::runtime_error("Skin sequence input limit");
    std::ifstream input(path);token(input,"OMEGA_SKIN_CASES");
    if(read<unsigned>(input)!=1)throw std::runtime_error("Skin sequence version");
    const auto count=read<unsigned>(input);if(!count||count>256)throw std::runtime_error("Skin case count");
    std::uint64_t vertices=0;
    for(const auto& chunk:mesh.chunks)vertices+=chunk.vertices.size();
    if(vertices*count>2000000)throw std::runtime_error("Skin sequence output limit");
    output<<std::setprecision(9)<<"{\"cases\":[";
    for(unsigned i=0;i<count;++i){
        token(input,"BASE_MORPH");const auto base=read<unsigned>(input);
        if(base>1)throw std::runtime_error("Skin base morph flag");
        token(input,"BONES");const auto bones=read<unsigned>(input);
        if(!bones||bones>21||bones!=mesh.bone_count)throw std::runtime_error("Skin palette bone count");
        token(input,"WORLD");const auto world=matrices(input,bones);
        token(input,"INVERSE");const auto inverse=matrices(input,bones);
        token(input,"SCALES");const auto scales_count=read<unsigned>(input);
        if(scales_count&&scales_count!=bones)throw std::runtime_error("Skin scale count");
        const auto scales=matrices(input,scales_count);
        const auto palette=make_skin_palette(world,inverse,scales);
        const auto shape=skin_skinned_mesh(mesh,palette,base?std::optional{SkinMorphContext{0}}:std::nullopt);
        if(i)output<<',';
        output<<"{\"vertices\":[";
        for(std::size_t v=0;v<shape.vertices.size();++v){if(v)output<<',';const auto& vertex=shape.vertices[v];
            output<<"{\"position\":";array(output,vertex.position);output<<",\"normal\":";array(output,vertex.normal);
            output<<",\"uv\":";array(output,vertex.uv);output<<'}';}
        output<<"],\"triangles\":[";
        for(std::size_t t=0;t<shape.triangles.size();++t){if(t)output<<',';array(output,shape.triangles[t]);}
        output<<"],\"triangle_chunks\":";array(output,shape.triangle_chunks);output<<'}';
    }
    std::string trailing;if(input>>trailing)throw std::runtime_error("Trailing skin sequence data");
    if(!input.eof())throw std::runtime_error("Cannot finish skin sequence");
    output<<"]}";
}
}
