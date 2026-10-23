#include "float_bits.hpp"
#include "actor_response_probes.hpp"
#include <cmath>
#include <stdexcept>

namespace omega {
namespace {
float check(float value){
    if(::omega::float_bits::nonfinite_or_subnormal(value))
        throw std::runtime_error("Response probe: unsupported nonfinite/subnormal arithmetic");
    return value;
}
// The probe reference and root target are points, but a fractional edge
// weight leaves the blended reference's W an ulp off 1 (001e5700 blend). In
// the interpreter the water range 1e3d28..1e3d74 and the floor range
// 1e3e88..1e4020 (shapes 1 and 3) carry that W only into W lanes for W in
// {1 -+ 1 ulp, 1.5, 0}; every XYZ lane and the hit results are unchanged
// (LADDERS.md). The native queries read XYZ only.
void point(const RootVector& value){
    for(float lane:value)check(lane);
}
}

RootVector apply_actor_gravity(const RootVector& force,const RootVector& gravity,float mass,float primary_weight){
    if(!(check(primary_weight)>0.0F))return force;
    RootVector result{};
    for(std::size_t lane=0;lane<4;++lane)
        result[lane]=check(check(force[lane])+check(check(gravity[lane])*check(mass)));
    return result;
}

std::optional<ActorWaterProbeHit> run_actor_water_probe(const RootVector& upper_reference,
    const RootVector& target,const ActorResponseProbeHost& host){
    if(!host.candidates||!host.geometry||!host.owner)
        throw std::runtime_error("Water probe: complete collection and source refresh host required");
    point(upper_reference);point(target);
    const RootVector offset{0,0,6.0F,0}; // 40c00000 at 001e3d28
    RootVector start{},displacement{};
    for(std::size_t lane=0;lane<4;++lane)start[lane]=check(upper_reference[lane]+offset[lane]);
    for(std::size_t lane=0;lane<4;++lane)displacement[lane]=check(target[lane]-start[lane]);
    FaceQuery query;
    query.origin={start[0],start[1],start[2]};
    query.displacement={displacement[0],displacement[1],displacement[2]};
    query.radius=std::bit_cast<float>(0x3dcccccdU);
    query.min_approach_cosine=std::bit_cast<float>(0x3d4ccccdU);
    query.min_direct_cosine=query.min_approach_cosine;
    query.min_start_distance=0;
    query.max_end_distance=query.radius;
    query.required_flags=0x80;query.excluded_flags=0;
    auto* batch=host.candidates();
    if(!batch)return std::nullopt; // 3c3450: null collection returns before any test
    if(batch->candidates.size()>500000)throw std::runtime_error("Water probe: candidate bound exceeded");
    FaceResult result;
    try{result=query_faces(query,batch->candidates,true);}
    catch(const std::runtime_error& error){throw std::runtime_error(std::string("Water probe: ")+error.what());}
    if(result.status==FaceResult::Status::unsupported_edge_vertex)
        throw std::runtime_error("Water probe: incomplete collision result");
    if(!result.contact)return std::nullopt;
    const auto source=resolve_collision_contact(*batch,*result.contact);
    // 3c3450 itself reconstructs the source face for edge/vertex contacts; the
    // probe then performs its own reconstruction for the recorded triangle.
    if(result.contact->type!=1)(void)refresh_collision_source_triangle(source,host.geometry,host.owner);
    ActorWaterProbeHit hit;
    hit.triangle=refresh_collision_source_triangle(source,host.geometry,host.owner);
    hit.height=check(result.contact->point.z);
    return hit;
}

namespace {
float dot3(const Vec3& a,const Vec3& b){return check(check(check(a.x*b.x)+check(a.y*b.y))+check(a.z*b.z));}
Vec3 sub3(const Vec3& a,const Vec3& b){return {check(a.x-b.x),check(a.y-b.y),check(a.z-b.z)};}
}

FloorRayResult trace_floor_ray(const RootVector& origin,const RootVector& displacement,
    std::span<FaceCandidate> candidates,std::uint8_t required,std::uint8_t excluded){
    for(float lane:origin)check(lane);
    for(float lane:displacement)check(lane);
    FloorRayResult result;result.displacement=displacement;
    const Vec3 o{origin[0],origin[1],origin[2]};
    for(std::size_t index=0;index<candidates.size();++index){
        auto& candidate=candidates[index];
        if((candidate.flags&excluded)!=0||(candidate.flags&required)!=required)continue;
        const Vec3 d{result.displacement[0],result.displacement[1],result.displacement[2]};
        float denominator=dot3(d,candidate.normal);
        bool front=denominator<0.0F;
        if((candidate.flags&2)!=0&&!front){
            candidate.normal={-candidate.normal.x,-candidate.normal.y,-candidate.normal.z};
            front=true;denominator=-denominator;
        }
        if(!front)continue;
        const float numerator=dot3(sub3(candidate.vertices[0],o),candidate.normal);
        if(!(numerator<0.0F)||numerator<=denominator)continue;
        const float s=check(numerator/denominator);
        RootVector scaled{},point{};
        for(std::size_t lane=0;lane<4;++lane){
            scaled[lane]=check(result.displacement[lane]*s);
            point[lane]=check(origin[lane]+scaled[lane]);
        }
        // Drop the dominant normal axis with the original tie order.
        const float ax=std::fabs(candidate.normal.x),ay=std::fabs(candidate.normal.y),az=std::fabs(candidate.normal.z);
        int dropped;
        if(ax<=ay)dropped=ay<=az?2:1;else dropped=az<=ax?0:2;
        const auto e1=sub3(candidate.vertices[1],candidate.vertices[0]);
        const auto e2=sub3(candidate.vertices[2],candidate.vertices[0]);
        const auto p=sub3({point[0],point[1],point[2]},candidate.vertices[0]);
        const auto lane=[&](const Vec3& v,int axis){return axis==0?v.x:axis==1?v.y:v.z;};
        const int a=dropped==0?1:dropped==1?2:0,b=dropped==0?2:dropped==1?0:1;
        const float inverse=check(1.0F/check(check(lane(e1,a)*lane(e2,b))-check(lane(e1,b)*lane(e2,a))));
        const float u=check(inverse*check(check(lane(e2,b)*lane(p,a))-check(lane(e2,a)*lane(p,b))));
        const float v=check(inverse*check(check(lane(p,b)*lane(e1,a))-check(lane(p,a)*lane(e1,b))));
        if(u<0.0F||v<0.0F||!(check(u+v)<=1.0F))continue;
        result.hit=FloorRayHit{candidate.normal,point,index};
        result.displacement=scaled;
    }
    return result;
}

namespace {
std::optional<ActorFloorProbeHit> sphere_floor_query(const RootVector& reference,float radius,
    CollisionCandidateBatch* batch,const ActorResponseProbeHost& host){
    FaceQuery query;
    query.origin={reference[0],reference[1],reference[2]};
    query.displacement={0,0,-15.5F};
    query.radius=radius;
    query.min_approach_cosine=std::bit_cast<float>(0x3d4ccccdU);
    query.min_direct_cosine=query.min_approach_cosine;
    query.min_start_distance=0;query.max_end_distance=radius;
    query.required_flags=0;query.excluded_flags=0x80;
    if(!batch)return std::nullopt;
    if(batch->candidates.size()>500000)throw std::runtime_error("Floor probe: candidate bound exceeded");
    FaceResult result;
    try{result=query_faces(query,batch->candidates,true);}
    catch(const std::runtime_error& error){throw std::runtime_error(std::string("Floor probe: ")+error.what());}
    if(result.status==FaceResult::Status::unsupported_edge_vertex)
        throw std::runtime_error("Floor probe: incomplete collision result");
    if(!result.contact)return std::nullopt;
    const auto source=resolve_collision_contact(*batch,*result.contact);
    Vec3 face=result.contact->normal;
    if(result.contact->type!=1){
        const auto triangle=refresh_collision_source_triangle(source,host.geometry,host.owner);
        face={triangle.normal[0],triangle.normal[1],triangle.normal[2]};
    }
    return ActorFloorProbeHit{result.contact->normal,face,
        {result.contact->point.x,result.contact->point.y,result.contact->point.z,1},source};
}
}

std::optional<ActorFloorProbeHit> run_actor_floor_probe(const RootVector& reference,
    std::uint8_t shape_byte_14,const ActorResponseProbeHost& host){
    if(!host.candidates||!host.geometry||!host.owner)
        throw std::runtime_error("Floor probe: complete collection and source refresh host required");
    point(reference);
    const RootVector displacement{0,0,-15.5F,0}; // c1780000
    auto* batch=host.candidates();
    if(shape_byte_14<3){
        // 003c36c0 dereferences the collection without a null test.
        if(!batch)throw std::runtime_error("Floor ray probe requires a present collection");
        const auto ray=trace_floor_ray(reference,displacement,batch->candidates,0,0x80);
        if(!ray.hit)return std::nullopt;
        return ActorFloorProbeHit{ray.hit->normal,ray.hit->normal,ray.hit->point,batch->sources.at(ray.hit->ordinal)};
    }
    return sphere_floor_query(reference,std::bit_cast<float>(0x3dcccccdU),batch,host);
}

std::optional<ActorFloorProbeHit> run_actor_floor_reprobe(const RootVector& reference,
    const ActorResponseProbeHost& host){
    if(!host.candidates||!host.geometry||!host.owner)
        throw std::runtime_error("Floor re-probe: complete collection and source refresh host required");
    point(reference);
    return sphere_floor_query(reference,1.0F,host.candidates(),host);
}
}
