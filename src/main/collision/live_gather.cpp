#include "float_bits.hpp"
#include "live_gather.hpp"
#include "projectile_candidates.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

namespace omega {
namespace {
float finite(float x) {
    if (::omega::float_bits::nonfinite_or_subnormal(x))
        throw std::runtime_error("Live gather: unsupported nonfinite/subnormal arithmetic");
    return x;
}
float add(float a,float b){return finite(a+b);}
float sub(float a,float b){return finite(a-b);}
float mul(float a,float b){return finite(a*b);}
void validate(const RootVector& v){for(float x:v)finite(x);}
void validate(const GatherBounds& b){
    validate(b.minimum);validate(b.maximum);
    for(std::size_t i=0;i<3;++i)if(b.minimum[i]>b.maximum[i])
        throw std::runtime_error("Live gather: inverted query bounds");
}
float squared_xyz(const RootVector& v){
    const auto x=mul(v[0],v[0]),y=mul(v[1],v[1]),z=mul(v[2],v[2]);
    return add(add(x,y),z);
}
RootVector transform(const GatherMatrix& matrix,const RootVector& value){
    for(float x:matrix)finite(x);
    validate(value);RootVector result{};
    for(std::size_t row=0;row<4;++row){
        float x=mul(matrix[row],value[0]);
        x=add(x,mul(matrix[4+row],value[1]));
        x=add(x,mul(matrix[8+row],value[2]));
        result[row]=add(x,mul(matrix[12+row],value[3]));
    }
    return result;
}
GatherBounds transformed_bounds(const GatherBounds& bounds,const GatherMatrix& matrix){
    const auto largest=std::numeric_limits<float>::max();
    GatherBounds result{{largest,largest,largest,1},{-largest,-largest,-largest,1}};
    for(unsigned bits=0;bits<8;++bits){
        RootVector point{bits&4?bounds.maximum[0]:bounds.minimum[0],
                         bits&2?bounds.maximum[1]:bounds.minimum[1],
                         bits&1?bounds.maximum[2]:bounds.minimum[2],1};
        point=transform(matrix,point);
        for(std::size_t i=0;i<3;++i){
            result.minimum[i]=std::min(result.minimum[i],point[i]);
            result.maximum[i]=std::max(result.maximum[i],point[i]);
        }
    }
    return result;
}
GatherSphere query_sphere(const GatherBounds& bounds){
    RootVector extent{},center{};
    for(std::size_t i=0;i<4;++i){
        extent[i]=sub(bounds.maximum[i],bounds.minimum[i]);
        center[i]=mul(add(bounds.minimum[i],bounds.maximum[i]),.5f);
    }
    const auto length=finite(static_cast<float>(std::sqrt(static_cast<double>(squared_xyz(extent)))));
    return {mul(.5f,length),center};
}
bool overlaps(const GatherSphere& object,const GatherSphere& query){
    finite(object.radius);validate(object.center);
    if(object.radius<0)throw std::runtime_error("Live gather: negative sphere radius unsupported");
    RootVector delta{};
    for(std::size_t i=0;i<4;++i)delta[i]=sub(query.center[i],object.center[i]);
    const auto radius=add(object.radius,query.radius);
    const auto radius_squared=mul(radius,radius);
    return sub(squared_xyz(delta),radius_squared)<=0;
}
class Traversal {
public:
    Traversal(const GatherBounds& bounds,const LiveGatherHost& host)
        :bounds_(bounds),sphere_(query_sphere(bounds)),host_(host){}
    LiveGatherStats run(){
        host_.clear_output();host_.update_provider();
        const auto range=host_.provider_collection();check_range(range);
        for(std::size_t i=range.count;i>0;--i){
            const auto object=host_.member_at(range,i-1);
            if(object)visit(object,GatherRoute::direct_col,0);
        }
        host_.finalize_output();return stats_;
    }
private:
    void check_range(GatherRange range){
        if(range.count>100000 || (!range.identity&&range.count))
            throw std::runtime_error("Live gather: invalid collection extent");
    }
    GatherBounds local_query(GatherIdentity object,GatherIdentity owner){
        const auto history=host_.enabled_history(object);
        if(!history)return transformed_bounds(bounds_,host_.owner_inverse(owner));
        // Refresh current owner before observing the retained prior transform.
        const auto current=host_.owner_world(owner);
        const auto previous=host_.history_world(history);
        const auto old_bounds=transformed_bounds(bounds_,prepare_projectile_query_frame(previous));
        auto result=transformed_bounds(bounds_,prepare_projectile_query_frame(current));
        // 182290 includes both corners in XYZ; bounds W remains initialized one.
        for(const auto& point:{old_bounds.minimum,old_bounds.maximum})
            for(std::size_t i=0;i<3;++i){
                result.minimum[i]=std::min(result.minimum[i],point[i]);
                result.maximum[i]=std::max(result.maximum[i],point[i]);
            }
        return result;
    }
    void visit(GatherIdentity object,GatherRoute route,std::size_t depth){
        if(!object)throw std::runtime_error("Live gather: null child object");
        if(depth>=128 || stats_.objects>=100000 ||
           std::find(stack_.begin(),stack_.end(),object)!=stack_.end())
            throw std::runtime_error("Live gather: cyclic or excessive object traversal");
        stack_.push_back(object);++stats_.objects;
        const auto geometry=host_.geometry(object);
        if(geometry){
            const auto owner=host_.owner(object);
            auto sphere=host_.geometry_sphere(geometry);
            if(owner)sphere.center=transform(host_.owner_world(owner),sphere.center);
            if(!overlaps(sphere,sphere_))++stats_.sphere_rejected;
            else if(!host_.eligible(object))++stats_.ineligible;
            else if(host_.excluded(object))++stats_.excluded;
            else{
                const auto local=owner?local_query(object,owner):bounds_;
                host_.emit({object,geometry,route,local});++stats_.emissions;
            }
        }
        // Each virtual child lookup occurs after its own direct branch, even
        // when that branch was absent, culled, disabled or excluded.
        const auto children=host_.children(object);check_range(children);
        const auto child_route=route==GatherRoute::direct_col?GatherRoute::node_col:GatherRoute::direct_col;
        for(std::size_t i=0;i<children.count;++i)
            visit(host_.member_at(children,i),child_route,depth+1);
        stack_.pop_back();
    }
    const GatherBounds& bounds_;
    GatherSphere sphere_;
    const LiveGatherHost& host_;
    LiveGatherStats stats_;
    std::vector<GatherIdentity> stack_;
};
}
LiveGatherStats gather_live_collision(const GatherBounds& bounds,const LiveGatherHost& host){
    validate(bounds);
    if(!host.clear_output||!host.update_provider||!host.finalize_output||!host.provider_collection||
       !host.member_at||!host.geometry||!host.geometry_sphere||!host.owner||!host.owner_world||
       !host.owner_inverse||!host.enabled_history||!host.history_world||!host.eligible||
       !host.excluded||!host.children||!host.emit)
        throw std::runtime_error("Live gather: complete synchronous host required");
    return Traversal(bounds,host).run();
}
}
