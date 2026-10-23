#include "los_provider.hpp"
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace omega {
namespace {
[[noreturn]] void fail(const char* reason) {
    throw std::runtime_error(reason);
}
float finite(float value) {
    if (!std::isfinite(value) || std::fpclassify(value) == FP_SUBNORMAL)
        fail("LOS provider: unsupported nonfinite/subnormal value");
    return value;
}
float add(float a,float b) { return finite(a+b); }
float sub(float a,float b) { return finite(a-b); }
float mul(float a,float b) { return finite(a*b); }
void vector(const RootVector& value) { for (float x:value) finite(x); }
const LosRegionBounds& bounds(const LosSpatialObject& object) {
    if (!object.bounds) fail("LOS provider: selected object has no observed bounds");
    const auto& box=*object.bounds;
    vector(box.minimum); vector(box.maximum);
    for (std::size_t i=0;i<3;++i)
        if (box.minimum[i]>box.maximum[i]) fail("LOS provider: inverted bounds unsupported");
    return box;
}
bool contains(const LosRegionBounds& box,const RootVector& point) {
    // The original tests XYZ MAC sign bits after each vector subtraction.
    bool lower=true,upper=true;
    for (std::size_t i=0;i<3;++i) if (sub(point[i],box.minimum[i])<0) lower=false;
    if (!lower) return false;
    for (std::size_t i=0;i<3;++i) if (sub(box.maximum[i],point[i])<0) upper=false;
    return upper;
}
float diagonal_squared(const LosRegionBounds& box) {
    RootVector squares{};
    for (std::size_t i=0;i<4;++i) {
        const float extent=sub(box.maximum[i],box.minimum[i]);
        squares[i]=mul(extent,extent);
    }
    return add(add(squares[0],squares[1]),squares[2]);
}
float distance_squared(const LosRegionBounds& box,const RootVector& point) {
    RootVector lower{},upper{},squares{};
    for (std::size_t i=0;i<3;++i) lower[i]=sub(box.minimum[i],point[i]);
    for (std::size_t i=0;i<3;++i) upper[i]=sub(point[i],box.maximum[i]);
    for (std::size_t i=0;i<3;++i) {
        const float outside=lower[i]>=0 ? add(lower[i],0.0f) :
                            upper[i]>=0 ? add(upper[i],0.0f) : 0.0f;
        squares[i]=mul(outside,outside);
    }
    return add(add(squares[0],squares[1]),squares[2]);
}
}
LosCollectionSelection select_los_collection(LosProviderState& state,const RootVector& origin,
                                              const LosSpatialSnapshot& snapshot) {
    constexpr std::size_t max_objects=100000,max_members=1000000;
    vector(origin);
    if (snapshot.objects.size()>max_objects || snapshot.global_regions.size()>max_objects)
        fail("LOS provider: snapshot extent unsupported");
    std::unordered_map<LosObjectId,const LosSpatialObject*> objects;
    std::size_t total_members=0;
    for (const auto& object:snapshot.objects) {
        if (!object.id || !objects.emplace(object.id,&object).second)
            fail("LOS provider: zero/duplicate object identity");
        if (object.members.size()>max_members-total_members)
            fail("LOS provider: collection extent unsupported");
        total_members+=object.members.size();
    }
    auto resolve=[&](LosObjectId id)->const LosSpatialObject& {
        const auto it=objects.find(id);
        if (it==objects.end()) fail("LOS provider: unresolved snapshot identity");
        return *it->second;
    };
    for (const auto& object:snapshot.objects)
        for (auto id:object.members) if (id) (void)resolve(id);
    for (auto id:snapshot.global_regions) (void)resolve(id);

    const LosSpatialObject* cached=nullptr;
    if (const auto it=objects.find(state.region);it!=objects.end() &&
        it->second->generation==state.generation) cached=it->second;
    const LosSpatialObject* selected=nullptr;
    LosCollectionSelection result;
    if (cached && contains(bounds(*cached),origin)) {
        selected=cached;
        result.path=LosSelectionPath::retained;
    } else {
        float largest=0;
        if (cached) for (auto id:cached->members) {
            if (!id) continue;
            const auto& candidate=resolve(id);
            if (candidate.type!=1 || candidate.descriptor!=snapshot.region_descriptor) continue;
            const auto& box=bounds(candidate);
            if (!contains(box,origin)) continue;
            const float size=diagonal_squared(box);
            if (size>largest) { largest=size; selected=&candidate; }
        }
        if (selected) result.path=LosSelectionPath::local;
        else {
            float nearest=std::numeric_limits<float>::max();
            for (auto id:snapshot.global_regions) {
                const auto& candidate=resolve(id);
                const float distance=distance_squared(bounds(candidate),origin);
                if (distance<nearest) {
                    nearest=distance;selected=&candidate;
                    if (distance==0) break;
                }
            }
            if (selected) result.path=LosSelectionPath::global;
        }
    }
    LosProviderState draft{origin,0,0xffffffffu};
    if (selected) {
        result.region=selected->id;
        draft.region=selected->id;draft.generation=selected->generation;
        result.object_order.reserve(selected->members.size());
        for (auto it=selected->members.rbegin();it!=selected->members.rend();++it)
            if (*it) result.object_order.push_back(*it);
    }
    state=draft;
    return result;
}
}
