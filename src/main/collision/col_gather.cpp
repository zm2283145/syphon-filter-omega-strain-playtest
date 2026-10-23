#include "col_gather.hpp"
#include "binary.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

namespace omega {
namespace {
float finite(float value){
    // Same test as !isfinite || subnormal, on the bits (the CRT classifier dominated play profiles).
    const auto bits=std::bit_cast<std::uint32_t>(value),exponent=bits&0x7f800000U;
    if(exponent==0x7f800000U||(exponent==0&&(bits&0x007fffffU)!=0))
        throw std::runtime_error("COL gather requires finite normal-or-zero coordinates");
    return value;
}
std::array<float,3> values(Vec3 v){return {v.x,v.y,v.z};}
bool overlap(const Bounds& a,const Bounds& b){
    const auto lo=values(a.minimum),hi=values(a.maximum),qlo=values(b.minimum),qhi=values(b.maximum);
    for(unsigned i=0;i<3;++i)if(finite(qhi[i]-lo[i])<0)return false;
    for(unsigned i=0;i<3;++i)if(finite(hi[i]-qlo[i])<0)return false;
    return true;
}
struct Node {Bounds bounds;std::array<std::uint32_t,8> children;bool empty{};};
struct Leaf {Bounds bounds;std::vector<std::uint32_t> triangles;};
void check_header(std::span<const std::uint8_t> bytes){
    if(bytes.size()>128*1024*1024||checked_bytes(bytes,0,4)[0]!='C'||bytes[1]!='O'||bytes[2]!='L'||bytes[3]!=5)
        throw std::runtime_error("COL gather requires bounded unrelocated COL5 input");
    checked_bytes(bytes,0,48);
}
void check_query(const Bounds& query){
    const auto qlo=values(query.minimum),qhi=values(query.maximum);
    for(unsigned i=0;i<3;++i)if(finite(qlo[i])>finite(qhi[i]))throw std::runtime_error("Reversed COL query bounds");
}
}
struct Col5Index {
    std::uint32_t node_count{},node_at{},leaf_count{},leaf_at{};
    std::vector<Node> nodes;std::vector<Leaf> leaves;
    std::vector<Bounds> triangle_bounds;std::vector<std::uint8_t> flags;
};
namespace {
// Parses and validates the whole resource (everything that does not depend on the query).
std::shared_ptr<const Col5Index> build_index(std::span<const std::uint8_t> bytes){
    auto number=[&](std::size_t at){return finite(std::bit_cast<float>(read_u32(bytes,at)));};
    auto vector=[&](std::size_t at){return Vec3{number(at),number(at+4),number(at+8)};};
    auto bounds=[&](std::size_t at,bool allow_empty=false){
        const Bounds result{vector(at),vector(at+16)};
        const auto lo=values(result.minimum),hi=values(result.maximum);
        const auto maximum=std::numeric_limits<float>::max();
        const bool sentinel=lo==std::array<float,3>{maximum,maximum,maximum}&&
            hi==std::array<float,3>{-maximum,-maximum,-maximum};
        if(allow_empty&&sentinel)return result;
        for(unsigned i=0;i<3;++i)if(lo[i]>hi[i])throw std::runtime_error("Reversed COL node/leaf bounds outside supported domain");
        return result;
    };
    const auto node_count=read_u32(bytes,4),node_at=read_u32(bytes,8),leaf_count=read_u32(bytes,12),leaf_at=read_u32(bytes,16);
    const auto triangle_count=read_u32(bytes,20),triangle_at=read_u32(bytes,24),vertex_count=read_u32(bytes,28),vertex_at=read_u32(bytes,32);
    const auto surface_count=read_u32(bytes,40),surface_at=read_u32(bytes,44);
    if(node_count>65536||leaf_count>1000000||triangle_count>1000000||vertex_count>65536||surface_count>65536)
        throw std::runtime_error("COL gather table count exceeds bound");
    std::vector<std::pair<std::size_t,std::size_t>> regions{{0,48}};
    auto table=[&](std::uint32_t count,std::uint32_t at,std::size_t stride){
        checked_bytes(bytes,at,std::size_t(count)*stride);
        if(count){
            if(at<48||at%16)throw std::runtime_error("COL gather table alignment/header overlap");
            regions.emplace_back(at,std::size_t(at)+std::size_t(count)*stride);
        }
    };
    table(node_count,node_at,64);table(leaf_count,leaf_at,48);table(triangle_count,triangle_at,16);
    table(vertex_count,vertex_at,16);table(surface_count,surface_at,16);
    std::sort(regions.begin(),regions.end());
    for(std::size_t i=1;i<regions.size();++i)if(regions[i-1].second>regions[i].first)
        throw std::runtime_error("COL gather structural tables overlap");
    if(node_count&&(leaf_at<std::size_t(node_at)+std::size_t(node_count)*64))
        throw std::runtime_error("COL gather node/leaf address classifier would be ambiguous");
    if(!leaf_count&&triangle_count)throw std::runtime_error("COL gather triangles have no leaf table");
    auto node_index=[&](std::uint32_t at){
        if(at<node_at||(at-node_at)%64||(at-node_at)/64>=node_count)
            throw std::runtime_error("COL gather child outside internal-node table");
        return (at-node_at)/64;
    };
    auto leaf_index=[&](std::uint32_t at){
        if(at<leaf_at||(at-leaf_at)%48||(at-leaf_at)/48>=leaf_count)
            throw std::runtime_error("COL gather child outside leaf table");
        return (at-leaf_at)/48;
    };
    auto index=std::make_shared<Col5Index>();
    index->node_count=node_count;index->node_at=node_at;index->leaf_count=leaf_count;index->leaf_at=leaf_at;
    auto& nodes=index->nodes;nodes.reserve(node_count);
    for(std::uint32_t i=0;i<node_count;++i){
        const std::size_t at=std::size_t(node_at)+i*64;
        Node item;
        for(unsigned j=0;j<8;++j){const auto child=read_u32(bytes,at+32+j*4);item.children[j]=child;
            if(child){if(child>=leaf_at)leaf_index(child);else node_index(child);}}
        item.empty=std::all_of(item.children.begin(),item.children.end(),[](auto child){return child==0;});
        item.bounds=bounds(at,item.empty);
        nodes.push_back(item);
    }
    auto& leaves=index->leaves;leaves.reserve(leaf_count);std::size_t total_references=0;
    for(std::uint32_t i=0;i<leaf_count;++i){
        const std::size_t at=std::size_t(leaf_at)+i*48;
        const auto count=read_u32(bytes,at+32),start=read_u32(bytes,at+36);
        if(count>1000000||total_references+count>1000000)throw std::runtime_error("COL gather leaf references exceed bound");
        total_references+=count;checked_bytes(bytes,start,std::size_t(count)*4);
        if(count){
            if(start%4)throw std::runtime_error("Unaligned COL leaf index list");
            const auto end=std::size_t(start)+std::size_t(count)*4;
            for(const auto& [lo,hi]:regions)if(start<hi&&lo<end)throw std::runtime_error("COL leaf indices overlap a structural table");
        }
        Leaf item;item.bounds=bounds(at);item.triangles.reserve(count);
        for(std::uint32_t j=0;j<count;++j){const auto id=read_u32(bytes,std::size_t(start)+j*4);
            if(id>=triangle_count)throw std::runtime_error("COL leaf triangle index outside table");item.triangles.push_back(id);}
        leaves.push_back(std::move(item));
    }
    // Validate all links before traversal; shared subtrees/leaves remain legal.
    std::vector<std::uint8_t> marks(node_count);
    std::function<void(std::uint32_t,unsigned)> validate_links=[&](std::uint32_t id,unsigned depth){
        if(depth>256||marks[id]==1)throw std::runtime_error("COL gather cyclic or excessive-depth tree");
        if(marks[id]==2)return;marks[id]=1;
        for(auto child:nodes[id].children)if(child&&child<leaf_at)validate_links(node_index(child),depth+1);
        marks[id]=2;
    };
    for(std::uint32_t i=0;i<node_count;++i)validate_links(i,0);
    auto& triangle_bounds=index->triangle_bounds;auto& flags=index->flags;
    triangle_bounds.reserve(triangle_count);flags.reserve(triangle_count);
    for(std::uint32_t i=0;i<triangle_count;++i){
        const std::size_t at=std::size_t(triangle_at)+i*16;
        std::array<Vec3,3> p{};
        for(unsigned j=0;j<3;++j){const auto index=read_u16(bytes,at+4+j*2);
            if(index>=vertex_count)throw std::runtime_error("COL triangle vertex outside table");p[j]=vector(std::size_t(vertex_at)+index*16);}
        triangle_bounds.push_back({{std::min({p[0].x,p[1].x,p[2].x}),std::min({p[0].y,p[1].y,p[2].y}),std::min({p[0].z,p[1].z,p[2].z})},
            {std::max({p[0].x,p[1].x,p[2].x}),std::max({p[0].y,p[1].y,p[2].y}),std::max({p[0].z,p[1].z,p[2].z})}});
        const auto surface=static_cast<std::uint32_t>(bytes[at+10]>>1);
        if(surface>=surface_count)throw std::runtime_error("COL gather surface index outside table");
        flags.push_back(bytes[std::size_t(surface_at)+surface*16]);
    }
    return index;
}
// Checkpoint 132win11 (port-side speed only): the recursion is a plain function and the result is
// the caller's (no std::function or result vector allocated per query). Same visit order.
struct Traversal {
    const Col5Index& index;const Bounds& query;std::uint8_t excluded;ColGatherResult& result;
    void leaf(std::uint32_t id){
        if(++result.leaves_visited>1000000)throw std::runtime_error("COL gather expanded leaf traversal exceeds bound");
        const auto& l=index.leaves[id];if(!overlap(l.bounds,query))return;
        for(auto triangle:l.triangles){
            if(++result.triangle_references>1000000)throw std::runtime_error("COL gather expanded references exceed bound");
            if(!overlap(index.triangle_bounds[triangle],query))continue;++result.triangle_aabb_pass;
            if(index.flags[triangle]&excluded){++result.surface_rejected;continue;}
            result.triangle_indices.push_back(triangle);
        }
    }
    void node(std::uint32_t id,unsigned depth){
        if(depth>256||++result.nodes_visited>1000000)throw std::runtime_error("COL gather expanded traversal exceeds bound");
        const auto& n=index.nodes[id];if(n.empty||!overlap(n.bounds,query))return;
        // links validated by build_index
        for(auto child:n.children)if(child){if(child>=index.leaf_at)leaf((child-index.leaf_at)/48);else node((child-index.node_at)/64,depth+1);}
    }
};
void traverse_into(const Col5Index& index,const Bounds& query,std::uint8_t excluded,ColGatherResult& result){
    result.triangle_indices.clear();
    result.nodes_visited=result.leaves_visited=result.triangle_references=result.triangle_aabb_pass=result.surface_rejected=0;
    Traversal t{index,query,excluded,result};
    if(index.node_count)t.node(0,0);else if(index.leaf_count)t.leaf(0);
}
ColGatherResult traverse(const Col5Index& index,const Bounds& query,std::uint8_t excluded){
    ColGatherResult result;
    traverse_into(index,query,excluded,result);
    return result;
}
}
std::shared_ptr<const Col5Index> index_col5(std::span<const std::uint8_t> bytes){
    check_header(bytes);
    return build_index(bytes);
}
ColGatherResult gather_col5_triangles(const Col5Index& index,const Bounds& query,std::uint8_t excluded){
    check_query(query);
    return traverse(index,query,excluded);
}
void gather_col5_triangles(const Col5Index& index,const Bounds& query,std::uint8_t excluded,ColGatherResult& result){
    check_query(query);
    traverse_into(index,query,excluded,result);
}
ColGatherResult gather_col5_triangles(std::span<const std::uint8_t> bytes,const Bounds& query,std::uint8_t excluded){
    check_header(bytes);
    check_query(query);
    return traverse(*build_index(bytes),query,excluded);
}
}
