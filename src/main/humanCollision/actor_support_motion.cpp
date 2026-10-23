#include "float_bits.hpp"
#include "actor_support_motion.hpp"
#include "hotbox_world.hpp"
#include "vu_float.hpp"
#include <cmath>
#include <stdexcept>

namespace omega {
namespace {
float check(float x){
    if(::omega::float_bits::nonfinite_or_subnormal(x))
        throw std::runtime_error("Actor support: unsupported nonfinite/subnormal arithmetic");
    return x;
}
float add(float a,float b){return check(check(a)+check(b));}
float sub(float a,float b){return check(check(a)-check(b));}
float mul(float a,float b){return check(check(a)*check(b));}
void valid(const RootVector& v){for(auto x:v)check(x);}
void valid(const RootParentTransform& p){valid(p.orientation);valid(p.position);
    if(p.position[3]!=1)throw std::runtime_error("Actor support: placement W must be one");}
RootVector product(const RootVector& a,const RootVector& b){
    valid(a);valid(b);RootVector r;
    for(unsigned i=0;i<3;++i){const unsigned j=(i+1)%3,k=(i+2)%3;
        r[i]=sub(add(add(mul(a[j],b[k]),mul(a[i],b[3])),mul(b[i],a[3])),mul(b[j],a[k]));}
    r[3]=sub(sub(sub(mul(a[3],b[3]),mul(a[2],b[2])),mul(a[1],b[1])),mul(a[0],b[0]));return r;
}
RootVector conjugate(const RootVector& q){valid(q);return {sub(0,q[0]),sub(0,q[1]),sub(0,q[2]),q[3]};}
RootVector rotate(const RootVector& q,const RootVector& p){
    valid(q);valid(p);RootVector first;
    for(unsigned i=0;i<3;++i){const unsigned j=(i+1)%3,k=(i+2)%3;
        first[i]=sub(add(mul(q[j],p[k]),mul(p[i],q[3])),mul(p[j],q[k]));}
    first[3]=sub(sub(sub(mul(q[3],0),mul(q[2],p[2])),mul(q[1],p[1])),mul(q[0],p[0]));
    const auto c=conjugate(q);RootVector r;
    for(unsigned i=0;i<3;++i){const unsigned j=(i+1)%3,k=(i+2)%3;
        r[i]=sub(add(add(mul(first[j],c[k]),mul(first[i],c[3])),mul(c[i],first[3])),mul(c[j],first[k]));}
    r[3]=p[3];return r;
}
RootParentTransform inverse(const RootParentTransform& p){
    valid(p);const auto q=conjugate(p.orientation);
    return {q,rotate(q,{sub(0,p.position[0]),sub(0,p.position[1]),sub(0,p.position[2]),p.position[3]})};
}
RootVector normalize(RootVector q){
    valid(q);float n=mul(q[0],q[0]);for(unsigned i=1;i<4;++i)n=add(n,mul(q[i],q[i]));
    if(n<=0)throw std::runtime_error("Actor support: undefined quaternion normalization");
    // 001e7a8c..001e7aac: VU RSQRT Q = vf0.w/sqrt(|q|^2) over XYZW, then vmulq.xyzw.
    const float r=check(vu::rsqrt(n));
    for(auto& x:q)x=mul(x,r);
    return q;
}
RootVector yaw_only(const RootVector& q){
    // Only the two basis columns/entries consumed by1e75e4..761c are formed.
    valid(q);const float xx=mul(q[0],add(q[0],q[0])),yy=mul(q[1],add(q[1],q[1]));
    const float zz=mul(q[2],add(q[2],q[2]));
    const float xy=mul(q[1],add(q[0],q[0]));
    const float yx=mul(q[0],add(q[1],q[1]));
    const float zw=mul(q[3],add(q[2],q[2]));
    const float xz=mul(q[2],add(q[0],q[0])),yw=mul(q[3],add(q[1],q[1]));
    const float yz=mul(q[2],add(q[1],q[1])),xw=mul(q[3],add(q[0],q[0]));
    const float x_z=sub(xz,yw),y_z=add(yz,xw);
    const float angle=x_z<y_z?
        check(static_cast<float>(std::atan2(static_cast<double>(add(xy,zw)),static_cast<double>(sub(sub(1,yy),zz))))):
        check(static_cast<float>(std::atan2(static_cast<double>(sub(0,sub(yx,zw))),static_cast<double>(sub(sub(1,xx),zz)))));
    const float half=mul(.5F,angle);
    const float s=check(static_cast<float>(std::sin(static_cast<double>(half))));
    const float c=check(static_cast<float>(std::cos(static_cast<double>(half))));
    return {mul(s,0),mul(s,0),mul(s,1),c};
}
}
RootParentTransform actor_support_matrix_placement(const std::array<float,16>& m){
    for(auto x:m)check(x);
    if(m[3]!=0||m[7]!=0||m[11]!=0||m[15]!=1)
        throw std::runtime_error("Actor support: owner matrix must be affine");
    const float trace=add(add(m[0],m[5]),m[10]);
    RootVector q{};float radicand;unsigned dominant;
    if(trace>0){
        radicand=add(trace,1);dominant=3;
        q={sub(m[6],m[9]),sub(m[8],m[2]),sub(m[1],m[4]),0};
    }else{
        const float x_y=sub(m[0],m[5]),y_z=sub(m[5],m[10]),z_x=sub(m[10],m[0]);
        if(x_y>=0&&z_x<0){
            dominant=0;radicand=add(sub(x_y,m[10]),1);
            q={0,add(m[4],m[1]),add(m[2],m[8]),sub(m[6],m[9])};
        }else if(x_y<0&&y_z>=0){
            dominant=1;radicand=add(sub(y_z,m[0]),1);
            q={add(m[4],m[1]),0,add(m[9],m[6]),sub(m[8],m[2])};
        }else{
            dominant=2;radicand=add(sub(z_x,m[5]),1);
            q={add(m[2],m[8]),add(m[9],m[6]),0,sub(m[1],m[4])};
        }
    }
    if(radicand<=0)throw std::runtime_error("Actor support: invalid owner rotation square root");
    // 001e7258 (and the three dominant-axis branches): VU SQRT of the radicand,
    // then VU DIV 1/root; both binary32.
    const float root=check(vu::sqrt(radicand));
    if(root==0)throw std::runtime_error("Actor support: zero owner rotation divisor");
    const float reciprocal=check(vu::div(1.F,root));
    q[dominant]=root;for(auto& x:q)x=mul(x,.5F);
    for(unsigned i=0;i<4;++i)if(i!=dominant)q[i]=mul(q[i],reciprocal);
    return {q,{m[12],m[13],m[14],m[15]}};
}
RootParentTransform compose_actor_support_placement(const RootParentTransform& local,const RootParentTransform& parent){
    valid(local);valid(parent);RootParentTransform r{product(parent.orientation,local.orientation),rotate(parent.orientation,local.position)};
    for(unsigned i=0;i<3;++i)r.position[i]=add(r.position[i],parent.position[i]);
    return r;
}
RootParentTransform actor_owner_local_placement(const RootParentTransform& world,const std::array<float,16>& owner){
    return compose_actor_support_placement(world,actor_support_matrix_placement(hotbox_rigid_inverse(owner)));
}
ActorSupportMotionOutput consume_actor_support_motion(float scale,
    const std::array<std::array<std::uint8_t,4>,3>& keys,const ActorSupportMotionHost& host){
    if(!host.primary_enabled||!host.object||!host.owner||!host.local||!host.previous_owner||
       !host.owner_dirty||!host.refresh_owner||!host.owner_matrix||!host.root||!host.root_world||
       !host.root_parented||!host.set_orientation_and_reset||!host.set_placement_and_reset)
        throw std::runtime_error("Actor support: complete live host required");
    ActorSupportMotionOutput output;
    for(std::size_t slot=0;slot<3;++slot){
        if(slot==0&&!host.primary_enabled())continue;
        const auto object=host.object(slot);if(!object)continue;
        const auto owner=host.owner(object);if(!owner)continue;
        const auto old_local=host.local(slot);
        const auto old_owner=host.previous_owner(slot);
        const auto old_world=compose_actor_support_placement(old_local,old_owner);
        if(host.owner_dirty(owner))host.refresh_owner(owner);
        const auto owner_world=actor_support_matrix_placement(host.owner_matrix(owner));
        const auto current=compose_actor_support_placement(host.local(slot),owner_world);
        for(unsigned i=0;i<4;++i){
            output.displacement[slot][i]=sub(current.position[i],old_world.position[i]);
            output.scaled_displacement[slot][i]=mul(output.displacement[slot][i],scale);
        }
        const auto root=host.root(keys[slot]);
        if(!root)throw std::runtime_error("Actor support: unresolved required model root");
        if(slot==0){
            const auto delta=product(current.orientation,conjugate(old_world.orientation));
            const auto world=host.root_world(root);valid(world.orientation);
            const auto rotation=product(delta,world.orientation);
            if(!host.root_parented(root))host.set_orientation_and_reset(root,yaw_only(rotation));
        }else{
            output.delta[slot]=compose_actor_support_placement(inverse(old_world),current);
            const auto world=host.root_world(root);valid(world);
            auto placement=compose_actor_support_placement(world,output.delta[slot]);
            placement.orientation=normalize(placement.orientation);
            if(!host.root_parented(root))host.set_placement_and_reset(root,placement);
        }
    }
    return output;
}
}
