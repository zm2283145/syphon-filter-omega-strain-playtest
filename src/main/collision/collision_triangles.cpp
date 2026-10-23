#include "float_bits.hpp"
#include "collision_triangles.hpp"
#include "binary.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
float value(float x){
    if(::omega::float_bits::nonfinite_or_subnormal(x))
        throw std::runtime_error("Collision expansion requires finite normal-or-zero arithmetic");
    return x;
}
float add(float a,float b){return value(value(a)+value(b));}
float sub(float a,float b){return value(value(a)-value(b));}
float mul(float a,float b){return value(value(a)*value(b));}
// 003c1254 vmulx.xyz of the cross product by the triangle's +0 word. The COL
// converter stores 0x7f800000 for collinear triangles, whose cross product is
// exactly zero: the VU has no infinity, so exponent 255 is the normal binade
// 1.F x 2^128 and the product is a signed zero (sign of the two operands).
// Any other product with such an operand clamps to +-MAX, which an IEEE float
// cannot carry.
float scale(float cross,float coefficient){
    if(((std::bit_cast<std::uint32_t>(coefficient)>>23)&0xff)!=0xff)return mul(cross,coefficient);
    if(value(cross)!=0)
        throw std::runtime_error("003c1254: a nonzero cross product times an exponent-255 COL coefficient clamps to +-MAX (not ported)");
    return std::signbit(cross)!=std::signbit(coefficient)?-0.0F:0.0F;
}
RootVector transform(const CollisionMatrix& m,const RootVector& p){
    RootVector result{};
    for(unsigned row=0;row<4;++row){
        auto x=mul(m[row],p[0]);x=add(x,mul(m[4+row],p[1]));
        x=add(x,mul(m[8+row],p[2]));result[row]=add(x,mul(m[12+row],p[3]));
    }
    return result;
}
}
CollisionTriangleDraft expand_col5_triangle(std::span<const std::uint8_t> bytes,
    CollisionTriangleSource source,const CollisionTriangleOwner& owner){
    checked_bytes(bytes,0,48);
    if(bytes.size()>128*1024*1024||bytes[0]!='C'||bytes[1]!='O'||bytes[2]!='L'||bytes[3]!=5)
        throw std::runtime_error("Collision expansion requires raw bounded COL5");
    if(!source.geometry||!owner)throw std::runtime_error("Collision expansion needs geometry identity and explicit owner resolver");
    const auto triangles=read_u32(bytes,20),triangle_at=read_u32(bytes,24);
    const auto vertices=read_u32(bytes,28),vertex_at=read_u32(bytes,32);
    const auto surfaces=read_u32(bytes,40),surface_at=read_u32(bytes,44);
    if(triangles>1000000||vertices>65536||surfaces>65536||source.triangle>=triangles)
        throw std::runtime_error("Collision expansion table/index extent unsupported");
    // Checkpoint 132win9 (port-side speed only): at most 4 regions, kept on the stack (no allocation per triangle).
    std::array<std::pair<std::size_t,std::size_t>,4> regions{};regions[0]={0,48};std::size_t region_count=1;
    for(const auto [count,start]:{std::pair{triangles,triangle_at},std::pair{vertices,vertex_at},std::pair{surfaces,surface_at}}){
        checked_bytes(bytes,start,std::size_t(count)*16);
        if(count){
            if(start<48||start%16)throw std::runtime_error("Collision expansion table alignment/header overlap");
            regions[region_count++]={start,std::size_t(start)+std::size_t(count)*16};
        }
    }
    std::sort(regions.begin(),regions.begin()+region_count);
    for(std::size_t i=1;i<region_count;++i)if(regions[i-1].second>regions[i].first)
        throw std::runtime_error("Collision expansion tables overlap");
    const auto record=std::size_t(triangle_at)+std::size_t(source.triangle)*16;
    const auto surface=static_cast<std::uint32_t>(bytes[record+10]>>1);
    if(surface>=surfaces)throw std::runtime_error("Collision expansion surface outside table");
    CollisionTriangleDraft result{};result.source=source;
    for(unsigned corner=0;corner<3;++corner){
        const auto index=read_u16(bytes,record+4+corner*2);
        if(index>=vertices)throw std::runtime_error("Collision expansion vertex outside table");
        for(unsigned lane=0;lane<4;++lane)
            result.vertices[corner][lane]=value(std::bit_cast<float>(read_u32(bytes,std::size_t(vertex_at)+index*16+lane*4)));
    }
    // Merely copying this word is not arithmetic; finalization validates it.
    result.coefficient=std::bit_cast<float>(read_u32(bytes,record));
    if(const auto matrix=owner())for(auto& vertex:result.vertices)vertex=transform(*matrix,vertex);
    const auto metadata=checked_bytes(bytes,std::size_t(surface_at)+surface*16,16);
    std::copy(metadata.begin(),metadata.end(),result.surface.begin());
    result.edge_flags={false,(bytes[record+11]&128)!=0,(bytes[record+12]&128)!=0};
    return result;
}
std::vector<ExpandedCollisionTriangle> finalize_collision_triangles(std::span<const CollisionTriangleDraft> drafts){
    std::vector<ExpandedCollisionTriangle> result;
    finalize_collision_triangles(drafts,result);
    return result;
}
void finalize_collision_triangles(std::span<const CollisionTriangleDraft> drafts,std::vector<ExpandedCollisionTriangle>& result){
    if(drafts.size()>1000000)throw std::runtime_error("Collision triangle collection exceeds bounded extent");
    result.clear();result.reserve(drafts.size());
    for(const auto& draft:drafts){
      try{
        ExpandedCollisionTriangle triangle;static_cast<CollisionTriangleDraft&>(triangle)=draft;
        RootVector a{},b{};
        for(unsigned lane=0;lane<4;++lane){
            a[lane]=sub(draft.vertices[1][lane],draft.vertices[0][lane]);
            b[lane]=sub(draft.vertices[2][lane],draft.vertices[0][lane]);
        }
        for(unsigned lane=0;lane<3;++lane){
            const auto j=(lane+1)%3,k=(lane+2)%3;
            triangle.normal[lane]=scale(sub(mul(a[j],b[k]),mul(a[k],b[j])),draft.coefficient);
        }
        triangle.normal[3]=0;result.push_back(std::move(triangle));
      }catch(const std::runtime_error& error){
        throw std::runtime_error(std::string(error.what())+" (normal of COL geometry "+std::to_string(draft.source.geometry)
            +" triangle "+std::to_string(draft.source.triangle)+", object "+std::to_string(draft.source.object)+")");
      }
    }
}
}
