#include "target_visibility.hpp"
#include "vu_float.hpp"
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
[[noreturn]] void fail(const std::string& what){throw std::runtime_error("Target visibility: "+what);}
float fin(float x){
    if(!std::isfinite(x))fail("nonfinite arithmetic");
    if(x!=0.0F&&std::fabs(x)<std::bit_cast<float>(0x00800000U))return std::copysign(0.0F,x);
    return x;
}
float add(float a,float b){return fin(a+b);}
float sub(float a,float b){return fin(a-b);}
float mul(float a,float b){return fin(a*b);}
template<class F> const F& need(const F& f,const char* what){if(!f)fail(std::string("host function not supplied: ")+what);return f;}
// vmul.xyz then (1*x2 + 1*y2) + 1*z2 (0022d048..0022d05c).
float distance2(const RootVector& a,const RootVector& b){
    const RootVector d{sub(a[0],b[0]),sub(a[1],b[1]),sub(a[2],b[2]),sub(a[3],b[3])};
    return add(add(mul(1.0F,mul(d[0],d[0])),mul(1.0F,mul(d[1],d[1]))),mul(1.0F,mul(d[2],d[2])));
}
}

std::optional<std::size_t> TargetVisibility::slot(std::uint64_t object) const{
    for(std::size_t k=0;k<slot_count;++k)if(slots_[k]==object)return k;
    return std::nullopt;
}

VisibilityEntry& TargetVisibility::at(std::size_t lo,std::size_t hi){
    if(!(lo<hi&&hi<slot_count))fail("pair outside the table");
    // Row lo holds the entries of hi = lo+1 .. 29.
    std::size_t base=0;
    for(std::size_t r=0;r<lo;++r)base+=slot_count-1-r;
    return entries_[base+(hi-lo-1)];
}
const VisibilityEntry& TargetVisibility::entry(std::size_t lo,std::size_t hi) const{
    return const_cast<TargetVisibility*>(this)->at(lo,hi);
}

bool TargetVisibility::register_object(std::uint64_t object){
    for(const auto s:slots_)if(s==object)return false;      // also object 0 when a slot is free? see below
    for(auto& s:slots_)if(s==0){s=object;return true;}
    return false;
}

void TargetVisibility::unregister_object(std::uint64_t object){
    const auto k=slot(object);
    if(!k)return;
    for(std::size_t hi=*k+1;hi<slot_count;++hi){auto& e=at(*k,hi);e.visible_until_4=0;e.expiry_0=0;e.pending_d=0;}
    for(std::size_t lo=0;lo<*k;++lo){auto& e=at(lo,*k);e.visible_until_4=0;e.expiry_0=0;e.pending_d=0;}
    slots_[*k]=0;
}

LosServiceRequest pair_los_request(std::uint64_t viewer,const RootVector& viewer_eye,std::uint64_t target,const RootVector& target_eye){
    RootVector direction{sub(target_eye[0],viewer_eye[0]),sub(target_eye[1],viewer_eye[1]),
                         sub(target_eye[2],viewer_eye[2]),sub(target_eye[3],viewer_eye[3])};
    // 0022cf20..0022cf6c: normalize (VRSQRT), then times 250.
    const float s=add(add(mul(direction[0],direction[0]),mul(direction[1],direction[1])),mul(direction[2],direction[2]));
    // Checkpoint 132zf12: coincident eyes (BELARUS2 frame 3: the follower 526364 and its leader 526363) give a zero sum; the
    // interpreter's VRSQRT by zero is +FLT_MAX (VU_ROUNDING.md), so the direction stays zero (0 * FLT_MAX).
    const float r=s==0.0F?std::numeric_limits<float>::max():fin(vu::rsqrt(s));
    for(std::size_t k=0;k<3;++k)direction[k]=mul(direction[k],r);
    for(auto& v:direction)v=mul(v,250.0F);
    LosServiceRequest request;
    request.origin_30=viewer_eye;request.displacement_40=direction;
    request.object_50=viewer;request.object_54=target;request.mask_58=4;request.mode_5c=0;
    request.value_3c=1.0F;request.value_4c=0.0F;
    return request;
}

bool pair_los_blocks(const LosServiceResult& result,const RootVector& viewer_eye,const RootVector& target_eye){
    // A hit beyond the target's eye does not count.
    return result.hit&&distance2(result.point,viewer_eye)<=distance2(target_eye,viewer_eye);
}

std::uint64_t TargetVisibility::test(std::uint64_t a,std::uint64_t b,std::int32_t max_age,bool force,std::int32_t fp,VisibilityHost& host){
    const auto ia=slot(a),ib=slot(b);
    if(!ia||!ib||*ia==*ib)return 0;
    const auto& player=need(host.player_controlled,"player_controlled");
    if(player(a)&&player(b))return 0;
    std::size_t lo=*ia,hi=*ib;
    if(hi<lo)std::swap(lo,hi);
    VisibilityEntry& e=at(lo,hi);
    auto answer=[&]()->std::uint64_t{
        if(fp-max_age<e.visible_until_4)return 0;
        if(fp<e.expiry_0)return e.blocker_8;
        return 0;
    };
    if(!force&&(e.pending_d!=0||!(e.expiry_0-20<fp)))return answer();
    // The viewer becomes the locally owned player.
    std::uint64_t viewer=a,target=b;
    const auto& local=need(host.byte_14,"byte_14");
    if((player(target)&&local(target))||(player(viewer)&&!local(viewer)))std::swap(viewer,target);
    const auto& eye=need(host.eye_bc,"eye_bc");
    const RootVector target_eye=eye(target),viewer_eye=eye(viewer);
    if(need(host.actor_class,"actor_class")(viewer)){
        const auto region=need(host.actor_region,"actor_region")(viewer);
        if(region)need(host.seed_los_region,"seed_los_region")(*region);
    }
    if(fp-max_age<e.visible_until_4)return answer();
    if(const auto smoke=need(host.smoke_blocker,"smoke_blocker")(viewer_eye,target_eye)){
        e.blocker_8=smoke;e.visible_c=0;e.pending_d=0;e.expiry_0=fp+40;
        return answer();
    }
    LosServiceResult result=need(host.los,"los")(pair_los_request(viewer,viewer_eye,target,target_eye));
    if(!pair_los_blocks(result,viewer_eye,target_eye))result.hit=false;
    // 0022d1f0.
    e.expiry_0=fp+40;
    e.visible_c=result.hit?0:1;
    e.pending_d=0;
    e.blocker_8=e.visible_c?0:result.object;
    if(e.visible_c)e.visible_until_4=e.expiry_0;
    else if(fp<e.visible_until_4)e.visible_until_4=fp;
    return answer();
}

}
