#include "shadow_scene.hpp"
#include "float_bits.hpp"
#include "morph_resources.hpp"
#include <cmath>
#include <stdexcept>
#include <string>
#include <utility>

namespace omega {
namespace {
using V=ShadowVector;
// The interpreter's flush of shadow_transform (character_shadows.cpp).
float ftz(float v){return float_bits::subnormal(v)?(std::signbit(v)?-0.0F:0.0F):v;}
void require(bool ok,const std::string& what){if(!ok)throw std::runtime_error("shadow scene: "+what);}
void require(bool ok,const char* what){if(!ok)throw std::runtime_error(std::string("shadow scene: ")+what);}   // 132vs1: no string unless it fails
}

std::uint32_t fold_shadow_model_flags(std::uint32_t flags,std::uint32_t header_flags,std::span<const MeshMaterial> materials){   // 00377120
    for(const MeshMaterial& m:materials){
        if(m.flags&0x10U)flags|=0x20U;
        if(m.flags&0x20U)flags|=0x40U;
        for(const MaterialPass& p:m.passes){
            if(p.shader==14)flags|=0x1000U;
            else if(p.shader==10)flags|=0x800U;
            else if(p.shader==11)flags|=0x400U;
        }
    }
    if(header_flags&0x10U)flags=(flags&~0x60U)|0x20000U;                                   // 00377238..00377264
    return flags;
}
std::uint32_t shadow_model_flags(const Mesh& model){
    // 00377700..00377754 (inside 00377570): header +4 bit 1 / bit 2 / neither.
    const std::uint32_t h=model.header_flags;
    const std::uint32_t flags=(h&1U)?0x400000U:((h&2U)?0x800000U:0x200000U);
    // 00377a8c: 00377120 runs when header byte +0x64 is clear, which decode_vum requires.
    return fold_shadow_model_flags(flags,h,model.materials);
}
ShadowWalkBucket shadow_walk_bucket(std::uint32_t header_flags,std::uint32_t model_flags){
    if(header_flags&8U)return ShadowWalkBucket::header_8;                                   // 0013e4b4 -> +0x4110
    if(model_flags&0x800000U)return ShadowWalkBucket::model_800000;                         // 0013e4f8 -> +0x6130
    if(model_flags&0x400000U)return ShadowWalkBucket::model_400000;                         // 0013e534 -> +0x5120
    return ShadowWalkBucket::first;                                                          // +0x8150
}
void add_terrain_shadow_receiver(ShadowReceiverTable& table,const Mesh& destination,const Mesh& source){
    require(destination.surfaces.size()+source.surfaces.size()<=0xffffffffU,"surface count out of range");
    ShadowReceiverInstance r;
    r.kind=1;
    r.header_flags=source.header_flags;
    r.model_flags=shadow_model_flags(source);
    const Bounds& b=source.declared_bounds;
    r.bounds_min={b.minimum.x,b.minimum.y,b.minimum.z,source.declared_bounds_w[0]};
    r.bounds_max={b.maximum.x,b.maximum.y,b.maximum.z,source.declared_bounds_w[1]};
    r.first_surface=static_cast<std::uint32_t>(destination.surfaces.size());
    r.surface_count=static_cast<std::uint32_t>(source.surfaces.size());
    table.instances.push_back(r);
}
std::size_t add_gob_shadow_receiver(ShadowReceiverTable& table,const Mesh& model,const ShadowMatrix& transform,
                                    std::uint32_t first_surface,std::uint32_t surface_count){
    ShadowReceiverInstance r;
    r.kind=1;                                                                              // cGOBJ_ctor a2 = 1 (00395dd0)
    r.header_flags=model.header_flags;
    r.model_flags=shadow_model_flags(model);
    const Bounds& b=model.declared_bounds;
    r.bounds_min={b.minimum.x,b.minimum.y,b.minimum.z,model.declared_bounds_w[0]};
    r.bounds_max={b.maximum.x,b.maximum.y,b.maximum.z,model.declared_bounds_w[1]};
    r.transform=transform;
    r.first_surface=first_surface;r.surface_count=surface_count;
    r.filed=false;r.walked=false;r.world_vertices=true;                                    // +0x38 = 0x80 (003cf0e0)
    table.instances.push_back(r);
    return table.instances.size()-1;
}
std::vector<ShadowReceiverSlotConstants> shadow_receiver_constants(const std::vector<ShadowSlot>& slots,const ShadowReceiverRecord& record,
                                                                   const ShadowBatch& batch,const std::optional<ShadowMatrix>& transform){   // 0037cad0..0037ce0c
    require(batch.count<=shadow_batch_size&&batch.first+batch.count<=record.slots.size(),"batch outside the receiver's slot list");
    std::vector<ShadowReceiverSlotConstants> out;
    for(std::size_t k=0;k<batch.count;++k){
        const std::size_t index=record.slots[batch.first+k];
        require(index<slots.size(),"receiver slot index out of range");
        const ShadowSlot& s=slots[index];
        out.push_back({transform?shadow_multiply(s.clip,*transform):s.clip,s.scale,s.offset,s.facing});
    }
    return out;
}

bool shadow_visibility_substitute(const ShadowMatrix& clip,const ShadowVector& lo,const ShadowVector& hi,const std::optional<ShadowMatrix>& transform){
    // NOT ORIGINAL: stands in for 003c8990 (VU0 micro routine, not ported).
    // 132vs2b: the corners through the first matrix (the transform, else the clip) are shadow_transform's arithmetic
    // with its products m.column[c][k] * lo[c] / hi[c] formed once per box (the same rounded values, added in the
    // same order and flushed the same way); m.column[3][k] * 1 is m.column[3][k] (exact, and NaN stays NaN). The
    // test stops once no plane is common to the corners seen (the rest can only clear more bits of `all`).
    const ShadowMatrix& m=transform?*transform:clip;
    float p[3][2][4];
    for(std::size_t c=0;c<3;++c)for(std::size_t k=0;k<4;++k){p[c][0][k]=m.column[c][k]*lo[c];p[c][1][k]=m.column[c][k]*hi[c];}
    int all=0x3f;
    for(std::size_t k=0;k<8&&all!=0;++k){
        const std::size_t sx=k&1U,sy=(k>>1)&1U,sz=(k>>2)&1U;
        V c;
        for(std::size_t r=0;r<4;++r)c[r]=ftz(ftz(ftz(ftz(p[0][sx][r])+p[1][sy][r])+p[2][sz][r])+m.column[3][r]);
        const V v=transform?shadow_transform(clip,c):c;
        const float w=std::fabs(v[3]);
        int code=0;
        for(std::size_t a=0;a<3;++a){
            if(v[a]>w)code|=1<<(2*a);
            if(v[a]<-w)code|=2<<(2*a);
        }
        all&=code;
    }
    return all==0;
}

ShadowCasterInput shadow_caster_input(const ShadowCasterFacts& f){
    ShadowCasterInput c;
    c.transform=f.transform;c.bounds_min=f.bounds_min;c.bounds_max=f.bounds_max;
    c.hidden=!f.visible;c.has_visibility_object=f.has_controller;c.visible=f.is_player;
    c.model_ready=f.on_flagged_floor;c.ground_plane=f.ground_plane;
    return c;
}
ShadowMatrix shadow_matrix(const std::array<float,16>& m){
    ShadowMatrix r;
    for(std::size_t c=0;c<4;++c)for(std::size_t k=0;k<4;++k)r.column[c][k]=m[c*4+k];
    return r;
}
void SoloShadowCaster::observe(const SoloActorSimulation& simulation,const SoloActorFrame& frame){
    // 001e4064..001e40ec: with a floor probe hit, +e8 = 1 and 001e1b70 rebuilds the
    // record from the hit triangle (bytes 0xb / 0xc bit 0x80, 001e1d40 / 001e1d30).
    if(!frame.trace.response||!frame.trace.response->floor)return;
    const auto t=simulation.world().refresh(frame.trace.response->floor->source);
    record_51_=t.edge_flags[1];record_52_=t.edge_flags[2];
}
ShadowCasterFacts SoloShadowCaster::facts(SoloActorSimulation& simulation) const{
    const ActorBounds b=human_actor_bounds(simulation.skeleton_node_matrices(),attachment_);   // 001af460, 003a9220, 00389fa0
    return facts(simulation,b.minimum,b.maximum);
}
ShadowCasterFacts SoloShadowCaster::facts(SoloActorSimulation& simulation,const ShadowVector& lo,const ShadowVector& hi) const{
    ShadowCasterFacts f;
    f.transform=shadow_matrix(root_placement_matrix(simulation.component_placement()));
    f.bounds_min=lo;f.bounds_max=hi;
    f.has_controller=true;f.is_player=true;                                               // player component 00177b10
    const bool e8=simulation.physical().response.physical.e8!=0;
    f.on_flagged_floor=e8&&(record_51_||record_52_);                                       // 0013e020, 003d5260
    if(e8)f.ground_plane=simulation.physical().plane_f0;                                   // 0014d3d0
    return f;
}

float shadow_scene_caster_fog(const ShadowSceneFrame& frame,std::size_t caster){
    if(caster>=frame.input.casters.size())throw std::runtime_error("Shadow fog: caster outside the frame's casters");
    if(!frame.fog)throw std::runtime_error("Shadow fog: the environment's fog terms are undefined (001339b0: equal near and far, or overflow)");
    // 003d2f60..003d2f94: transform object +0x40 (refreshed), 003d3700.
    return shadow_fog_factor(frame.camera_clip,frame.fog->base,frame.fog->scale,frame.input.casters[caster].transform.column[3]);
}
float shadow_scene_point_fog(const ShadowSceneFrame& frame,const ShadowVector& point){
    if(!frame.fog)throw std::runtime_error("Shadow fog: the environment's fog terms are undefined (001339b0: equal near and far, or overflow)");
    return shadow_fog_factor(frame.camera_clip,frame.fog->base,frame.fog->scale,point);   // 003d4350's 003d3700
}
std::int32_t shadow_scene_silhouette_level(const ShadowSceneFrame& frame,const ShadowSlot& slot){
    if(slot.type!=shadow_slot_shadow)throw std::runtime_error("Silhouette level: only type-1 slots have one (003d2f58)");
    return shadow_silhouette_level(slot.strength,shadow_scene_caster_fog(frame,slot.caster));
}

ShadowScene::ShadowScene(LevelLights level_lights,ShadowReceiverTable receiver_table,ShadowSceneSettings scene_settings)
    :lights_(std::move(level_lights)),receivers_(std::move(receiver_table)),settings_(scene_settings){}

void ShadowScene::set_receiver_walked(std::size_t instance,bool walked){
    require(instance<receivers_.instances.size()&&!receivers_.instances[instance].filed,"set_receiver_walked on a filed instance");
    receivers_.instances[instance].walked=walked;
}
void ShadowScene::set_receiver_transform(std::size_t instance,const ShadowMatrix& transform){
    require(instance<receivers_.instances.size()&&!receivers_.instances[instance].filed,"set_receiver_transform on a filed instance");
    receivers_.instances[instance].transform=transform;
}

LevelLightTable ShadowScene::light_table(const std::array<std::array<float,4>,4>& camera_world,float half_angle) const{
    ShadowMatrix world;
    for(std::size_t c=0;c<4;++c)world.column[c]=camera_world[c];
    LevelLightCamera camera;
    camera.half_angle=half_angle;camera.near_plane=settings_.near_plane;camera.far_plane=level_camera_far(lights_);camera.aspect=settings_.aspect;
    return build_level_light_table(lights_,shadow_rigid_inverse(world),camera);
}

ShadowScene::Camera ShadowScene::camera(const std::array<std::array<float,4>,4>& camera_world,float half_angle) const{
    Camera out;
    ShadowMatrix world;
    for(std::size_t c=0;c<4;++c)world.column[c]=camera_world[c];
    out.view=shadow_rigid_inverse(world);                                                  // 00133190 -> 00133200
    out.far_plane=level_camera_far(lights_);
    LevelLightCamera camera;
    camera.half_angle=half_angle;camera.near_plane=settings_.near_plane;camera.far_plane=out.far_plane;camera.aspect=settings_.aspect;
    out.projection=level_camera_projection(camera);
    out.clip=shadow_multiply(shadow_multiply(out.projection,shadow_axis_matrix()),out.view);
    return out;
}

ShadowSceneFrame ShadowScene::build(const ShadowSceneFrameInput& in){
    ShadowSceneFrame out;
    const Camera camera=this->camera(in.camera_world,in.half_angle);
    out.camera_view=camera.view;
    out.far_plane=camera.far_plane;
    try{out.fog=level_fog(lights_);}catch(const std::runtime_error&){out.fog.reset();}  // 00133574..00133648 (no environment already threw above)
    out.camera_projection=camera.projection;
    out.camera_clip=camera.clip;
    out.lights=light_table(in.camera_world,in.half_angle);                                // 0013e820 -> 00373670
    const auto& instances=receivers_.instances;
    if(in.receiver_visible)require(in.receiver_visible->size()==instances.size(),"receiver visibility does not match the table");
    ShadowFrameInput& f=out.input;
    f.settings=settings_.shadow;f.camera_view=out.camera_view;f.lights=out.lights.set;f.caster_cursor=cursor_;
    // 0013e1b0: culling, then the buckets; only visible objects are bucketed.
    std::vector<std::size_t> buckets[3];
    ShadowMatrix visibility_clip=out.camera_clip;   // port-only view_x_scale (132ws); the PS2's clip at 1
    if(in.view_x_scale!=1.0F)for(auto& c:visibility_clip.column)c[0]*=in.view_x_scale;
    if(in.view_y_scale!=1.0F)for(auto& c:visibility_clip.column)c[1]*=in.view_y_scale;   // 132ws3
    f.receivers.reserve(instances.size());
    for(std::size_t i=0;i<instances.size();++i){
        const ShadowReceiverInstance& r=instances[i];
        ShadowReceiverInput x;
        x.model_flags=r.model_flags;x.model_loaded=true;x.transform=r.transform;x.bounds_min=r.bounds_min;x.bounds_max=r.bounds_max;
        if(!r.filed){
            // A GOB sub-instance: 003d40d0 reads its +0x38 alone (copied by 0021a840); never bucketed.
            x.hidden=!r.walked;
            f.receivers.push_back(x);
            continue;
        }
        bool visible=r.active;
        if(visible)visible=in.receiver_visible?bool((*in.receiver_visible)[i]):shadow_visibility_substitute(visibility_clip,r.bounds_min,r.bounds_max,r.transform);
        x.hidden=!visible;
        f.receivers.push_back(x);
        if(!visible||r.kind!=1)continue;     // 0013f1a0 registers kind 1 only
        const ShadowWalkBucket b=shadow_walk_bucket(r.header_flags,r.model_flags);
        if(b==ShadowWalkBucket::model_800000)continue;
        auto& list=buckets[b==ShadowWalkBucket::first?0:(b==ShadowWalkBucket::model_400000?1:2)];
        if(list.size()<1024)list.push_back(i);                                              // 0013e374
    }
    for(const ShadowCasterFacts& c:in.casters){
        ShadowCasterInput x=shadow_caster_input(c);
        if(in.cull_casters&&!x.hidden)x.hidden=!shadow_visibility_substitute(visibility_clip,c.bounds_min,c.bounds_max,c.transform);
        f.casters.push_back(x);
    }
    // Walk order 0013e840..0013e864: first list, actors, 0x400000 list, header-8 list.
    for(std::size_t i:buckets[0])f.order.push_back({false,i});
    for(std::size_t i=0;i<f.casters.size();++i)if(!f.casters[i].hidden)f.order.push_back({true,i});
    for(std::size_t i:buckets[1])f.order.push_back({false,i});
    for(std::size_t i:buckets[2])f.order.push_back({false,i});
    out.frame=build_shadow_frame(f);
    cursor_=out.frame.caster_cursor;
    // The type-3 projectors (003d4350, the blood decals' 0027a020) join the
    // frame after its type-1/2 slots and before the receivers are drawn.
    if(in.projectors)in.projectors(out);
    for(std::size_t k=0;k<out.frame.receivers.size();++k){
        const ShadowReceiverRecord& rec=out.frame.receivers[k];
        ShadowSceneReceiver r;
        r.instance=rec.receiver;r.record=k;
        const ShadowReceiverInstance& inst=instances[rec.receiver];
        for(const ShadowBatch& b:shadow_receiver_batches(rec)){
            auto constants=shadow_receiver_constants(out.frame.slots,rec,b,inst.world_vertices?std::nullopt:inst.transform);
            // PORT-ONLY (world_vertices): the receiver routine (microcode set 0, 0x06d9..0x06db / 0x06f4..0x06f6)
            // dots the slot's world direction (VU 16 + 7k + 6, unchanged by the instance transform) with the
            // vertex's model-space normal (byte - 128) / 128. The merged mesh holds this instance's normals posed
            // by B (or B^-T, apply_stored_pose), so n_model = B^T n_posed and the same dot is (B dir) . n_posed.
            if(inst.world_vertices&&inst.transform)for(auto& c:constants){
                const ShadowVector d=c.facing;const auto& m=inst.transform->column;
                for(std::size_t i=0;i<3;++i)c.facing[i]=(m[0][i]*d[0]+m[1][i]*d[1])+m[2][i]*d[2];
            }
            r.batches.push_back({b,std::move(constants)});
        }
        out.receivers.push_back(std::move(r));
    }
    return out;
}
}
