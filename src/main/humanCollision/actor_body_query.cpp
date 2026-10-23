#include "actor_body_query.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace omega {
namespace {
float check(float v){if(!std::isfinite(v))throw std::runtime_error("Nonfinite actor body-query arithmetic");return v;}
float add(float a,float b){return check(check(a)+check(b));}
float sub(float a,float b){return check(check(a)-check(b));}
float mul(float a,float b){return check(check(a)*check(b));}
float divide(float a,float b){check(a);check(b);if(b==0)throw std::runtime_error("Actor body-query division by zero");return check(a/b);}
Vec3 valid(Vec3 v){check(v.x);check(v.y);check(v.z);return v;}
CollisionMatrix identity(){return {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};}
RootVector transform(const CollisionMatrix& matrix,RootVector vector){
    RootVector result{};
    for(unsigned row=0;row<4;++row){
        float v=mul(matrix[row],vector[0]);
        for(unsigned column=1;column<4;++column)v=add(v,mul(matrix[column*4+row],vector[column]));
        result[row]=v;
    }
    return result;
}
CollisionMatrix compose(const CollisionMatrix& left,const CollisionMatrix& right){
    CollisionMatrix result{};
    for(unsigned column=0;column<4;++column){
        const auto v=transform(left,{right[column*4],right[column*4+1],right[column*4+2],right[column*4+3]});
        std::copy(v.begin(),v.end(),result.begin()+column*4);
    }
    return result;
}
Vec3 transform3(const CollisionMatrix& matrix,Vec3 v,float w){
    const auto r=transform(matrix,{v.x,v.y,v.z,w});return {r[0],r[1],r[2]};
}
// VU RSQRT Q = vf0.w/sqrt(|v|^2), then vmulq.xyz (contact normal back to world
// at 003c358c): float square root, then float reciprocal, as the interpreter
// models it. RSQRT of 0 gives Q = FLT_MAX, so a zero vector (a collinear
// candidate's normal) stays zero.
Vec3 normalize(Vec3 v){
    if(v.x==0&&v.y==0&&v.z==0)return v;
    const float squared=add(add(mul(v.x,v.x),mul(v.y,v.y)),mul(v.z,v.z));
    if(squared<=0)throw std::runtime_error("Undefined actor body-query normal");
    const float reciprocal=divide(1.0F,check(std::sqrt(squared)));
    return {mul(v.x,reciprocal),mul(v.y,reciprocal),mul(v.z,reciprocal)};
}
}
namespace {
ActorBodyQueryPlan finish_plan(const ActorBodyQueryInput& input,ActorBodyQueryPlan plan){
    valid(plan.center);
    valid(input.resolved_displacement);check(input.radius_370);
    if(input.radius_370<=0)throw std::runtime_error("Actor body query requires positive radius");
    plan.query_to_world=plan.world_to_query=plan.candidate_normal_to_query=plan.face_normal_to_world=identity();
    if(plan.transformed){
        valid(plan.scale);
        if(plan.scale.x<=0||plan.scale.y<=0||plan.scale.z<=0)
            throw std::runtime_error("Actor body-query scale must be positive");
        auto placement=identity();placement[12]=plan.center.x;placement[13]=plan.center.y;placement[14]=plan.center.z;
        auto inverse_placement=identity();
        // 133200 preserves zero translation; otherwise rotates its negation by
        // the transposed identity basis with three ordered products.
        if(plan.center.x!=0||plan.center.y!=0||plan.center.z!=0){
            const RootVector negated{sub(0,plan.center.x),sub(0,plan.center.y),sub(0,plan.center.z),1};
            for(unsigned row=0;row<3;++row)
                inverse_placement[12+row]=add(add(mul(inverse_placement[row],negated[0]),
                    mul(inverse_placement[4+row],negated[1])),mul(inverse_placement[8+row],negated[2]));
        }
        auto scale=identity(),inverse_scale=identity();
        scale[0]=plan.scale.x;scale[5]=plan.scale.y;scale[10]=plan.scale.z;
        inverse_scale[0]=divide(1,plan.scale.x);inverse_scale[5]=divide(1,plan.scale.y);
        inverse_scale[10]=divide(1,plan.scale.z);
        plan.query_to_world=compose(placement,scale);
        plan.world_to_query=compose(inverse_scale,inverse_placement);
        plan.candidate_normal_to_query=compose(scale,inverse_placement);
        plan.face_normal_to_world=compose(placement,inverse_scale);
    }
    FaceQuery query;
    query.origin=plan.transformed?Vec3{}:plan.center;
    query.displacement=plan.transformed?transform3(plan.world_to_query,input.resolved_displacement,0):input.resolved_displacement;
    query.radius=input.radius_370;query.min_start_distance=0;query.min_approach_cosine=-.1F;
    query.min_direct_cosine=2;query.max_end_distance=input.radius_370;
    query.required_flags=0;query.excluded_flags=0x80;
    const bool supported=input.supported_dd&&!input.override_9c0;
    const bool vertical=supported||(input.flags_34&2)!=0;
    const float blend=std::clamp(sub(1,sub(1,input.blend_a20)),0.F,1.F);
    plan.passes=input.mode_14<3?1:2;
    plan.initial_query=make_slide_query(query,blend,vertical&&input.mode_14>=3,
        vertical?SlideQueryMode::vertical_projection:SlideQueryMode::projection);
    return plan;
}
}
ActorBodyQueryPlan make_actor_body_query(const ActorBodyQueryInput& input){
    check(input.secondary_weight);check(input.tertiary_weight);
    if(input.secondary_weight!=0||input.tertiary_weight!=0)
        throw std::runtime_error("Ordinary body query requires zero secondary/tertiary weights; use the specialized builder");
    ActorBodyQueryPlan plan;plan.center=valid(input.resolved_origin);
    valid(input.resolved_displacement);check(input.radius_370);
    if(input.radius_370<=0)throw std::runtime_error("Actor body query requires positive radius");
    plan.transformed=input.mode_14>=2;
    if(plan.transformed){
        const float top=add(plan.center.z,input.radius_370);
        const float height_delta=sub(sub(top,input.primary_position_z),
            (input.flags_34&2)!=0?input.height_offset_flag1:input.height_offset_unflagged);
        const float half_height=std::max(mul(.5F,height_delta),mul(input.radius_370,input.parameter_390));
        plan.center.z=sub(top,half_height);
        plan.scale.z=half_height==input.radius_370?1.F:divide(half_height,input.radius_370);
    }
    return finish_plan(input,plan);
}
ActorBodyQueryPlan make_transformed_actor_body_query(const ActorBodyQueryInput& input,Vec3 center,Vec3 scale,float center_w){
    ActorBodyQueryPlan plan;plan.transformed=true;plan.center=center;plan.scale=scale;plan.center_w=check(center_w);
    return finish_plan(input,plan);
}
namespace {
template<class Executor>
ActorBodyQueryResult run_query(const ActorBodyQueryPlan& plan,std::span<FaceCandidate> world_candidates,
    const ActorBodySourceFaceNormal& source_face_normal,Executor execute){
    if(plan.passes!=1&&plan.passes!=2)throw std::runtime_error("Actor body-query pass count must be one or two");
    if(world_candidates.size()>500000)throw std::runtime_error("Actor body-query candidate limit exceeded");
    auto candidates=std::vector<FaceCandidate>(world_candidates.begin(),world_candidates.end());
    if(plan.transformed)for(auto& candidate:candidates){
        validate_face_candidate(candidate);
        for(auto& vertex:candidate.vertices)vertex=transform3(plan.world_to_query,vertex,1);
        candidate.normal=normalize(transform3(plan.candidate_normal_to_query,candidate.normal,0));
        candidate.query_transformed=true;
    }
    ActorBodyQueryResult result;result.query_state=plan.initial_query;
    for(unsigned pass=0;pass<plan.passes;++pass){
        execute(result.query_state,std::span<FaceCandidate>(candidates));
        result.world_displacement=plan.transformed?
            transform3(plan.query_to_world,result.query_state.displacement,0):result.query_state.displacement;
        if(result.query_state.contact){
            result.world_contact=result.query_state.contact;
            if(plan.transformed){
                auto& contact=*result.world_contact;
                contact.point=transform3(plan.query_to_world,contact.point,1);
                contact.normal=normalize(transform3(contact.type==1?plan.face_normal_to_world:plan.query_to_world,contact.normal,0));
            }
            if(result.world_contact->type==1)result.source_face_normal=result.world_contact->normal;
            else if(source_face_normal)result.source_face_normal=valid(source_face_normal(result.world_contact->source_id));
            else result.source_face_normal.reset();
        }
    }
    if(!plan.transformed)std::copy(candidates.begin(),candidates.end(),world_candidates.begin());
    return result;
}
}
ActorBodyQueryResult run_static_actor_body_query(const ActorBodyQueryPlan& plan,std::span<FaceCandidate> world_candidates,
    const ActorBodySourceFaceNormal& source_face_normal){
    return run_query(plan,world_candidates,source_face_normal,[](SlideQueryState& state,std::span<FaceCandidate> candidates){
        consume_slide_candidates(state,candidates);
    });
}
ActorBodyQueryResult run_actor_body_query(const ActorBodyQueryPlan& plan,std::span<FaceCandidate> world_candidates,
    const MovingSlideHost& host,const ActorBodySourceFaceNormal& source_face_normal){
    std::optional<MovingSlideShape> shape;
    if(plan.transformed)shape=MovingSlideShape{{plan.center.x,plan.center.y,plan.center.z,plan.center_w},plan.world_to_query};
    return run_query(plan,world_candidates,source_face_normal,[&](SlideQueryState& state,std::span<FaceCandidate> candidates){
        consume_moving_slide_candidates(state,candidates,host,shape);
    });
}
}
