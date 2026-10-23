#include "float_bits.hpp"
#include "actor_auxiliary_probe.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace omega {
namespace {
float check(float x){if(::omega::float_bits::nonfinite_or_subnormal(x))
    throw std::runtime_error("Auxiliary probe: unsupported nonfinite/subnormal input or arithmetic");return x;}
void valid(const RootVector& v){for(auto x:v)check(x);}
Vec3 xyz(const RootVector& v){valid(v);if(v[3]!=1)throw std::runtime_error("Auxiliary probe: point W must be one");return {v[0],v[1],v[2]};}
FaceResult execute(const RootVector& origin,const ActorAuxiliaryProbeHost& host){
    const float extent=check(host.tuning());
    FaceQuery query;query.origin=xyz(origin);query.displacement={0,0,extent};
    query.radius=std::bit_cast<float>(std::uint32_t{0x3dcccccd});
    query.min_approach_cosine=std::bit_cast<float>(std::uint32_t{0x3d4ccccd});
    query.min_direct_cosine=query.min_approach_cosine;query.max_end_distance=query.radius;
    query.required_flags=0;query.excluded_flags=128;
    // Constructor work precedes even a null collection's execution early-out.
    check(extent*extent);
    const float endpoint=check(query.origin.z+extent);
    check(query.origin.x-query.radius);check(query.origin.x+query.radius);
    check(query.origin.y-query.radius);check(query.origin.y+query.radius);
    check(std::min(query.origin.z,endpoint)-query.radius);
    check(std::max(query.origin.z,endpoint)+query.radius);
    auto candidates=host.candidates();
    if(!candidates){FaceResult result;result.displacement=query.displacement;return result;}
    if(candidates->size()>500000)throw std::runtime_error("Auxiliary probe: candidate bound exceeded");
    FaceResult result;
    try{result=query_faces(query,*candidates,true);}
    catch(const std::runtime_error& error){throw std::runtime_error(std::string("Auxiliary probe: ")+error.what());}
    if(result.status==FaceResult::Status::unsupported_edge_vertex)
        throw std::runtime_error("Auxiliary probe: incomplete collision result");
    if(result.contact&&result.contact->type!=1){
        const auto normal=host.source_face_normal(*result.contact);
        check(normal.x);check(normal.y);check(normal.z);
    }
    return result;
}
bool accepted(const FaceResult& result,float reference,const ActorAuxiliaryProbeHost& host){
    if(!result.contact)return false;
    // Original loads live tuning before subtracting/inspecting the contact Z.
    const auto threshold=check(host.tuning());
    const auto delta=check(check(result.contact->point.z)-check(reference));
    return delta<=threshold;
}
}
ActorAuxiliaryProbeOutput run_actor_auxiliary_probe(ActorAuxiliaryProbeState& state,
    const RootVector& first_origin,float first_reference_z,const ActorAuxiliaryProbeHost& host){
    if(!host.tuning||!host.candidates||!host.root_world||!host.source_face_normal)
        throw std::runtime_error("Auxiliary probe: complete real query/root host required");
    ActorAuxiliaryProbeOutput output;
    state.first_hit=0;
    if(state.first_requested){
        output.queries[0]=execute(first_origin,host);
        if(accepted(*output.queries[0],first_reference_z,host))state.first_hit=1;
    }
    state.first_requested=0;
    const auto placement=host.root_world();valid(placement.orientation);xyz(placement.position);
    const RootVector offset{0,std::bit_cast<float>(std::uint32_t{0x3eb33333}),
        std::bit_cast<float>(std::uint32_t{0x3f333333}),1};
    output.second_origin=transform_root_vector(placement,offset);xyz(output.second_origin);
    // The second lookup is not elided, even if the following request is zero.
    output.second_reference_z=check(host.root_world().position[2]);
    state.second_hit=0;
    if(state.second_requested){
        output.queries[1]=execute(output.second_origin,host);
        if(accepted(*output.queries[1],output.second_reference_z,host))state.second_hit=1;
    }
    state.second_requested=0;
    return output;
}
}
