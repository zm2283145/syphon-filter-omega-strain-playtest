#include "world_los.hpp"
#include "los_query.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <stdexcept>

namespace omega {
namespace {
struct Gathered {
    std::vector<LosTriangle> candidates;
    std::vector<CollisionTriangleSource> sources;
};
// The candidates of a segment: the COL triangles whose bounds meet the
// segment's box widened by the corridor radius 0.01 (003c2210's radius).
Gathered gather(const SoloCollisionWorld& world,const RootVector& origin,const RootVector& displacement,
    const std::array<std::uint64_t,2>& excluded_objects={}){
    const float r=0.01F;
    Bounds box;
    box.minimum={std::min(origin[0],origin[0]+displacement[0])-r,std::min(origin[1],origin[1]+displacement[1])-r,
                 std::min(origin[2],origin[2]+displacement[2])-r};
    box.maximum={std::max(origin[0],origin[0]+displacement[0])+r,std::max(origin[1],origin[1]+displacement[1])+r,
                 std::max(origin[2],origin[2]+displacement[2])+r};
    const auto expanded=world.expand(box);
    Gathered g;
    g.candidates.reserve(expanded.size());g.sources.reserve(expanded.size());
    for(const auto& t:expanded){
        // 003c501c..003c5030: an excluded object's direct geometry is skipped.
        if(los_object_excluded(excluded_objects,world.game_object(t.source.geometry)))continue;
        LosTriangle c;
        c.vertices=t.vertices;c.normal=t.normal;c.flags=t.surface[0];
        c.source={static_cast<std::uint32_t>(t.source.geometry),t.source.triangle,static_cast<std::uint32_t>(t.source.object),0};
        g.candidates.push_back(c);g.sources.push_back(t.source);
    }
    return g;
}
bool finite_query(RootVector& origin,RootVector& displacement){
    origin[3]=1.0F;displacement[3]=0.0F;
    for(std::size_t k=0;k<4;++k)
        if(!std::isfinite(origin[k])||!std::isfinite(displacement[k]))throw std::runtime_error("World LOS: nonfinite query");
    return !(displacement[0]==0.0F&&displacement[1]==0.0F&&displacement[2]==0.0F);   // 003c26d0 early return
}
}

bool los_object_excluded(const std::array<std::uint64_t,2>& excluded,std::uint64_t object){
    return object!=0&&(object==excluded[0]||object==excluded[1]);
}

std::optional<WorldLosHit> trace_world_los(const SoloCollisionWorld& world,const WorldLosQuery& query){
    RootVector origin=query.origin,displacement=query.displacement;
    if(!finite_query(origin,displacement))return std::nullopt;
    auto g=gather(world,origin,displacement,query.excluded_objects);
    const auto result=trace_los_triangles({origin,displacement,query.required,query.excluded},g.candidates);
    if(!result.hit)return std::nullopt;
    return WorldLosHit{result.hit->point,result.hit->normal,g.sources.at(result.hit->candidate_index)};
}

LosServiceResult answer_los_request(const SoloCollisionWorld& world,const LosServiceRequest& request){
    if(request.mode_5c!=0)throw std::runtime_error("World LOS: queued requests (+5c nonzero) are not hosted");
    const auto hit=trace_world_los(world,{request.origin_30,request.displacement_40,0,static_cast<std::uint8_t>(request.mask_58&0xffU),
        {request.object_50,request.object_54}});
    LosServiceResult result;
    if(hit){result.hit=true;result.point=hit->point;result.object=hit->source.geometry;}
    return result;
}

std::optional<BulletStaticHit> bullet_static_los(const SoloCollisionWorld& world,const RootVector& start,const RootVector& displacement,
    const std::function<void(const RootVector& point,const CollisionTriangleSource& source)>& water){
    RootVector origin=start,d=displacement;
    if(!finite_query(origin,d))return std::nullopt;
    auto g=gather(world,origin,d);
    const auto pass=trace_los_pass_triangles({origin,d,0,0x10U},0x80U,g.candidates);
    // 003c4a80 -> (*00504030)->vtable 004dcd88 +8 = 00278160 for each crossed water triangle.
    for(const auto& c:pass.crossings){
        if(!water)throw std::runtime_error("World LOS: the bullet crossed a water surface (0x80); its callback 00278160 is not hosted");
        water(c.point,g.sources.at(c.candidate_index));
    }
    const auto& result=pass.first;
    if(!result.hit)return std::nullopt;
    BulletStaticHit hit;
    hit.point=result.hit->point;hit.normal=result.hit->normal;
    // 00278060..00278070: *(*(result +64)) is the hit COL source record's owner, the
    // object whose +b8 gave the geometry (003c4f00): a GOB for a population model's COL.
    const auto& source=g.sources.at(result.hit->candidate_index);
    const auto owner=world.game_object(source.geometry);
    hit.object=owner?owner:source.object;
    hit.source=result.hit->source;
    return hit;
}

}
