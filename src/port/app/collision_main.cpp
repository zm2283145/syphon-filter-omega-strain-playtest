#include "collision_face.hpp"
#include "los_query.hpp"
#include "col_gather.hpp"
#include "collision_triangles.hpp"
#include <iomanip>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {
template<class T> T read(std::istream& in) {
    T value{};
    if (!(in>>value)) throw std::runtime_error("Truncated or invalid face-query input");
    return value;
}
std::uint32_t integer(std::istream& in, std::uint64_t maximum) {
    const auto value=read<std::uint64_t>(in);
    if(value>maximum) throw std::runtime_error("Face-query integer exceeds limit");
    return static_cast<std::uint32_t>(value);
}
omega::Vec3 vector(std::istream& in) { return {read<float>(in),read<float>(in),read<float>(in)}; }
struct Case { std::string name; unsigned repeats; omega::FaceQuery query; std::vector<omega::FaceCandidate> candidates; };
omega::RootVector lanes(const omega::Vec3& value,float w){return {value.x,value.y,value.z,w};}
void write_los(std::ostream& out,const omega::LosResult& result){
    const auto vector4=[&](const omega::RootVector& value){out<<'[';for(unsigned i=0;i<4;++i){if(i)out<<',';out<<value[i];}out<<']';};
    out<<"{\"displacement\":";vector4(result.displacement);
    out<<",\"visited\":"<<result.visited<<",\"filtered\":"<<result.filtered<<",\"hit\":";
    if(!result.hit)out<<"null";
    else{
        const auto& hit=*result.hit;
        out<<"{\"candidate_index\":"<<hit.candidate_index<<",\"source_id\":"<<hit.source[0]<<",\"point\":";
        vector4(hit.point);out<<",\"normal\":";vector4(hit.normal);out<<'}';
    }
    out<<'}';
}
void gather_cases(std::istream& input,std::span<const std::uint8_t> bytes,bool expand){
    if(bytes.empty())throw std::runtime_error("COL gather requires --col");
    if(read<std::string>(input)!="OMEGA_COL_GATHER_CASES"||integer(input,1)!=1)
        throw std::runtime_error("Unknown COL gather case format");
    const auto count=integer(input,10000);
    std::ostringstream report;
    report<<std::setprecision(std::numeric_limits<float>::max_digits10);
    report<<"{\"scope\":\"Static local COL5 tree gather; not world object discovery or LOS corridor gather\",\"cases\":[";
    for(unsigned i=0;i<count;++i){
        const auto name=read<std::string>(input);
        if(name.empty()||name.size()>80||name.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")!=std::string::npos)
            throw std::runtime_error("Invalid COL gather case name");
        const omega::Bounds query{vector(input),vector(input)};
        const auto excluded=static_cast<std::uint8_t>(integer(input,255));
        const auto result=omega::gather_col5_triangles(bytes,query,excluded);
        if(i)report<<',';
        report<<"{\"name\":\""<<name<<"\",\"indices\":[";
        for(std::size_t j=0;j<result.triangle_indices.size();++j){if(j)report<<',';report<<result.triangle_indices[j];}
        report<<"],\"nodes_visited\":"<<result.nodes_visited<<",\"leaves_visited\":"<<result.leaves_visited
            <<",\"triangle_references\":"<<result.triangle_references<<",\"triangle_aabb_pass\":"<<result.triangle_aabb_pass
            <<",\"surface_rejected\":"<<result.surface_rejected;
        if(expand){
            std::vector<omega::CollisionTriangleDraft> drafts;
            for(const auto index:result.triangle_indices)
                drafts.push_back(omega::expand_col5_triangle(bytes,{1,index},[]{return std::optional<omega::CollisionMatrix>{};}));
            const auto triangles=omega::finalize_collision_triangles(drafts);
            const auto write_vector=[&](const omega::RootVector& v){
                report<<'[';for(unsigned k=0;k<4;++k){if(k)report<<',';report<<v[k];}report<<']';
            };
            report<<",\"triangles\":[";
            for(std::size_t j=0;j<triangles.size();++j){
                if(j)report<<',';
                const auto& t=triangles[j];
                report<<"{\"source_id\":"<<t.source.triangle<<",\"vertices\":[";
                for(unsigned k=0;k<3;++k){if(k)report<<',';write_vector(t.vertices[k]);}
                report<<"],\"coefficient\":"<<t.coefficient<<",\"normal\":";write_vector(t.normal);
                report<<",\"surface\":[";
                for(unsigned k=0;k<16;++k){if(k)report<<',';report<<unsigned(t.surface[k]);}
                report<<"],\"edge_flags\":[";
                for(unsigned k=0;k<3;++k){if(k)report<<',';report<<(t.edge_flags[k]?"true":"false");}
                report<<"]}";
            }
            report<<']';
        }
        report<<'}';
    }
    std::string trailing;if(input>>trailing)throw std::runtime_error("Trailing COL gather cases");
    report<<"]}\n";std::cout<<report.str();
}
}
int main(int argc,char** argv) {
    try {
        if(argc==2 && std::string(argv[1])=="--help") {
            std::cout<<"Usage: omega_collision --input <diagnostic cases> [--col <local COL5 file>] [--edges | --los | --gather]\n"
                "--los uses OMEGA_LOS_CASES: ordered supplied triangles, not original world gathering.\n"
                "--gather uses OMEGA_COL_GATHER_CASES and requires --col: original static local AABB tree order.\n"
                "--gather --expand also emits selected static triangles and finalized normals.\n"
                "Synthetic/supplied-candidate mode-0 face probe; no game actor or full collision simulation.\n";
            return 0;
        }
        std::string input_path,col_path; bool include_edges=false,los=false,gather=false,expand=false;
        for(int i=1;i<argc;++i) {
            const std::string arg=argv[i];
            if(arg=="--input" && input_path.empty() && i+1<argc) input_path=argv[++i];
            else if(arg=="--col" && col_path.empty() && i+1<argc) col_path=argv[++i];
            else if(arg=="--edges" && !include_edges) include_edges=true;
            else if(arg=="--los" && !los) los=true;
            else if(arg=="--gather" && !gather) gather=true;
            else if(arg=="--expand" && !expand) expand=true;
            else throw std::runtime_error("Unknown, duplicate or incomplete collision argument");
        }
        if(input_path.empty()) throw std::runtime_error("Use --input <diagnostic cases>");
        if(unsigned(los)+unsigned(include_edges)+unsigned(gather)>1)throw std::runtime_error("Choose one collision diagnostic mode");
        if(expand&&!gather)throw std::runtime_error("--expand requires --gather");
        std::vector<omega::FaceCandidate> external;
        std::vector<std::uint8_t> bytes;
        if(!col_path.empty()) {
            const auto size=std::filesystem::file_size(col_path);
            if(size>64*1024*1024) throw std::runtime_error("COL5 diagnostic input exceeds 64 MiB");
            std::ifstream file(col_path,std::ios::binary);
            bytes.resize(static_cast<std::size_t>(size));
            if(!file.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(size)))
                throw std::runtime_error("Cannot read COL5 diagnostic input");
            if(!gather)external=omega::decode_col_faces(bytes);
        }
        std::ifstream input(input_path);
        if(!input) throw std::runtime_error("Cannot open diagnostic cases");
        if(gather){gather_cases(input,bytes,expand);return 0;}
        if(read<std::string>(input)!=(los?"OMEGA_LOS_CASES":"OMEGA_FACE_CASES") || integer(input,1)!=1)
            throw std::runtime_error("Unknown diagnostic case format");
        const auto count=integer(input,10000);
        std::vector<Case> cases;
        std::size_t total=0;
        for(unsigned i=0;i<count;++i) {
            Case c;
            c.name=read<std::string>(input);
            if(c.name.empty() || c.name.size()>80 || c.name.find_first_not_of(
                "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")!=std::string::npos)
                throw std::runtime_error("Invalid diagnostic case name");
            c.repeats=integer(input,16);
            if(!c.repeats) throw std::runtime_error("Case repeat count must be positive");
            auto& q=c.query;
            q.origin=vector(input); q.displacement=vector(input);
            if(!los){
                q.radius=read<float>(input);
                q.min_start_distance=read<float>(input); q.min_approach_cosine=read<float>(input);
                q.min_direct_cosine=read<float>(input); q.max_end_distance=read<float>(input);
            }
            q.required_flags=static_cast<std::uint8_t>(integer(input,255));
            q.excluded_flags=static_cast<std::uint8_t>(integer(input,255));
            const auto size=integer(input,1000000);
            if(!col_path.empty() && size) throw std::runtime_error("External COL5 cases must not supply inline candidates");
            total+=!col_path.empty() ? external.size() : size;
            if(total>1000000) throw std::runtime_error("Too many diagnostic candidates");
            c.candidates.reserve(size);
            for(unsigned j=0;j<size;++j) {
                omega::FaceCandidate candidate;
                for(auto& v:candidate.vertices) v=vector(input);
                candidate.normal=vector(input);
                candidate.flags=static_cast<std::uint8_t>(integer(input,255));
                candidate.source_id=integer(input,std::numeric_limits<std::uint32_t>::max());
                c.candidates.push_back(candidate);
            }
            if(!col_path.empty())c.candidates=external;
            cases.push_back(std::move(c));
        }
        std::string trailing;
        if(input>>trailing) throw std::runtime_error("Trailing diagnostic input");
        // Build the report before publishing: a failed later case must not look like valid JSON.
        std::ostringstream report;
        report<<std::setprecision(std::numeric_limits<float>::max_digits10);
        report<<"{\"scope\":\""<<(los ? "LOS supplied-triangle diagnostic; file order is not world gathering order" :
            include_edges ? "Static mode-0 supplied-candidate contact diagnostic" :
            "Static mode-0 face diagnostic; edge and vertex fallback unsupported")<<"\",\"cases\":[";
        for(std::size_t i=0;i<cases.size();++i) {
            auto& c=cases[i]; if(i)report<<',';
            report<<"{\"name\":\""<<c.name<<"\",\"runs\":[";
            std::vector<omega::LosTriangle> triangles;
            if(los)for(const auto& source:c.candidates){
                omega::LosTriangle t;
                for(unsigned v=0;v<3;++v)t.vertices[v]=lanes(source.vertices[v],1);
                t.normal=lanes(source.normal,0);t.flags=source.flags;t.source[0]=source.source_id;triangles.push_back(t);
            }
            for(unsigned j=0;j<c.repeats;++j) {
                if(j)report<<',';
                if(los)write_los(report,omega::trace_los_triangles({lanes(c.query.origin,1),lanes(c.query.displacement,0),
                    c.query.required_flags,c.query.excluded_flags},triangles));
                else{
                    const auto result=omega::query_faces(c.query,c.candidates,include_edges);
                    omega::write_face_result(report,result,c.candidates);
                }
            }
            report<<"]}";
        }
        report<<"]}\n";
        std::cout<<report.str();
        return 0;
    } catch(const std::exception& e) {
        std::cerr<<"Face diagnostic failed: "<<e.what()<<'\n'; return 1;
    }
}
