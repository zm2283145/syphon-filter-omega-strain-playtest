#include "collision_collection.hpp"
#include "col_gather.hpp"
#include <stdexcept>
#include <cmath>

namespace omega {
CollisionCollection collect_live_col5(const GatherBounds& bounds,LiveGatherHost host,
    const CollisionColResolver& resolve,std::uint8_t excluded){
    if(!resolve||host.clear_output||host.emit||host.finalize_output)
        throw std::runtime_error("COL collection needs a resolver and ownership of its output callbacks");
    std::vector<CollisionTriangleDraft> drafts;CollisionCollection result;
    host.clear_output=[&]{drafts.clear();};
    host.emit=[&](const GatherEmission& emission){
        const auto bytes=resolve(emission.geometry,emission.route);
        const auto& b=emission.local_bounds;
        const Bounds local{{b.minimum[0],b.minimum[1],b.minimum[2]},{b.maximum[0],b.maximum[1],b.maximum[2]}};
        const auto selected=gather_col5_triangles(bytes,local,excluded);
        if(selected.triangle_indices.size()>1000000-drafts.size())
            throw std::runtime_error("COL collection exceeds expanded triangle bound");
        for(const auto index:selected.triangle_indices){
            drafts.push_back(expand_col5_triangle(bytes,{emission.geometry,index,emission.object},[&]()->std::optional<CollisionMatrix>{
                const auto owner=host.owner(emission.object);
                return owner?std::optional<CollisionMatrix>{host.owner_world(owner)}:std::nullopt;
            }));
        }
    };
    host.finalize_output=[&]{result.triangles=finalize_collision_triangles(drafts);};
    result.traversal=gather_live_collision(bounds,host);
    return result;
}
CollisionCandidateBatch prepare_collision_candidates(std::span<const ExpandedCollisionTriangle> triangles){
    CollisionCandidateBatch result;
    prepare_collision_candidates(triangles,result);
    return result;
}
void prepare_collision_candidates(std::span<const ExpandedCollisionTriangle> triangles,CollisionCandidateBatch& result){
    if(triangles.size()>1000000)throw std::runtime_error("Collision candidate batch exceeds bounded extent");
    result.candidates.clear();result.sources.clear();
    result.candidates.reserve(triangles.size());result.sources.reserve(triangles.size());
    for(const auto& triangle:triangles){
        if(!triangle.source.object||!triangle.source.geometry)
            throw std::runtime_error("World collision candidate lacks object or geometry identity");
        const auto xyz=[](const RootVector& value,float w){
            if(value[3]!=w)throw std::runtime_error("XYZ collision query requires homogeneous points and vectors");
            for(const auto lane:value)if(!std::isfinite(lane))
                throw std::runtime_error("Nonfinite world collision candidate");
            return Vec3{value[0],value[1],value[2]};
        };
        FaceCandidate candidate;
        for(unsigned i=0;i<3;++i)candidate.vertices[i]=xyz(triangle.vertices[i],1);
        candidate.normal=xyz(triangle.normal,0);candidate.flags=triangle.surface[0];
        candidate.source_id=static_cast<std::uint32_t>(result.sources.size());
        result.sources.push_back(triangle.source);result.candidates.push_back(candidate);
    }
}
CollisionTriangleSource resolve_collision_contact(const CollisionCandidateBatch& batch,const FaceContact& contact){
    if(batch.sources.size()!=batch.candidates.size()||contact.source_id>=batch.sources.size())
        throw std::runtime_error("Collision contact does not belong to this candidate batch");
    const auto index=contact.source_id;
    if(batch.candidates[index].source_id!=index||!batch.sources[index].object||!batch.sources[index].geometry)
        throw std::runtime_error("Collision source mapping was changed or lost");
    return batch.sources[index];
}
ExpandedCollisionTriangle refresh_collision_source_triangle(const CollisionTriangleSource& source,
    const std::function<std::span<const std::uint8_t>(std::uint64_t)>& geometry,
    const std::function<std::optional<CollisionMatrix>(std::uint64_t)>& owner){
    if(!source.object||!source.geometry||!geometry||!owner)
        throw std::runtime_error("Collision source refresh requires retained identities and live resolvers");
    const auto bytes=geometry(source.geometry);
    const auto draft=expand_col5_triangle(bytes,source,[&]{return owner(source.object);});
    return finalize_collision_triangles(std::span(&draft,1)).front();
}
}
