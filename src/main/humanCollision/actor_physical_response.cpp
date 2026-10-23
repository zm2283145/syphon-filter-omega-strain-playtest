#include "actor_physical_response.hpp"
#include <stdexcept>

namespace omega {
ActorPhysicalResponseTrace run_actor_physical_response(ActorPhysicalResponseState& state,
    const ActorPhysicalResponseInput& in,const ActorPhysicalResponseHost& host){
    if(!host.body_query||!host.contact_events||!host.refresh_skeleton_nodes||!host.floor_root_orientation||
        !host.anchor_world||!host.placement)
        throw std::runtime_error("Physical response: complete driver host required");
    auto& p=state.physical;
    ActorPhysicalResponseTrace trace;
    trace.setup=prepare_actor_response(p,in.support,in.elapsed,in.tuning,host.setup);
    const auto& s=trace.setup;
    if(!s.proceed){
        skip_actor_response(p,host.anchor_world());
        finish_actor_response(p,s,in.support_scaled[2]);
        return trace;
    }
    // 1e29fc..306c: body query and contact events.
    ActorBodyQueryInput query;
    query.resolved_origin={s.origin[0],s.origin[1],s.origin[2]};
    query.resolved_displacement={s.displacement[0],s.displacement[1],s.displacement[2]};
    if(p.shape_14>=2&&!s.specialized)query.primary_position_z=host.setup.floor_root_world_z();
    query.radius_370=p.radius_370;query.parameter_390=p.scale_390;
    query.height_offset_unflagged=in.tuning.height_offset_unflagged;query.height_offset_flag1=in.tuning.height_offset_flag1;
    query.mode_14=p.shape_14;query.flags_34=p.flags_34;
    query.supported_dd=p.byte_dd!=0;query.override_9c0=p.keep_contact_9c0!=0;
    query.blend_a20=state.contact_blend_a20;
    if(s.specialized){
        query.secondary_weight=host.setup.root_weight(ActorRoot::edge);
        query.tertiary_weight=host.setup.root_weight(ActorRoot::ladder);
    }
    trace.body=host.body_query(query,s.specialized);
    host.contact_events(*trace.body);
    const RootVector displacement{trace.body->world_displacement.x,trace.body->world_displacement.y,
        trace.body->world_displacement.z,0};
    const bool body_hit=trace.body->world_contact.has_value();
    // 1e3074..3cd0: root resolution, blocked fraction and publication.
    trace.position=resolve_actor_response_position(p,s,in.support,displacement,body_hit,host.resolve);
    const auto& position=*trace.position;
    // 1e3cd4..3d24: gravity.
    p.force_330=apply_actor_gravity(p.force_330,in.gravity,state.mass_2e0,host.setup.root_weight(ActorRoot::floor));
    // 1e3d28..3d70: water probe.
    state.water=run_actor_water_probe(position.probe_reference,position.root_target,host.collision);
    if(state.water)p.e4=state.water->height;
    // 1e3d74..3e84: node refreshes whose results are unused.
    host.refresh_skeleton_nodes();
    // 1e3e88..401c: floor probe.
    trace.floor=run_actor_floor_probe(position.probe_reference,p.shape_14,host.collision);
    // 1e4020..43a8: floor response.
    ActorFloorResponseState floor_state{p.force_330,p.byte_dc!=0,p.byte_dd!=0,p.e8!=0,p.floor_2c4,state.contact_blend_a20};
    ActorFloorResponseInput floor_in;
    floor_in.secondary_weight=host.setup.root_weight(ActorRoot::edge);
    floor_in.tertiary_weight=host.setup.root_weight(ActorRoot::ladder);
    floor_in.primary_weight=host.setup.root_weight(ActorRoot::floor);
    floor_in.previously_supported=s.supported;
    floor_in.flags34=p.flags_34;floor_in.flag13=p.byte_13!=0;floor_in.override_9c0=p.keep_contact_9c0!=0;
    floor_in.reference_height=position.root_target[2];
    floor_in.velocity_z=p.velocity_310[2];floor_in.relative_velocity_z=in.support_scaled[2];
    floor_in.force_limit_scale=in.gravity[2];floor_in.mass=state.mass_2e0;
    if(trace.floor)floor_in.contact=ActorFloorContact{trace.floor->point[2],trace.floor->face_normal.z};
    trace.floor_response=apply_actor_floor_response(floor_state,floor_in);
    const auto& fr=trace.floor_response->state;
    p.force_330=fr.force;p.byte_dc=fr.flag_dc?1:0;p.byte_dd=fr.flag_dd?1:0;p.e8=fr.contact_e8?1:0;
    p.floor_2c4=fr.height_distance;state.contact_blend_a20=fr.contact_blend;
    // 1e43a8..4428: optional wider re-probe into the same result.
    std::optional<ActorFloorProbeHit> sample=trace.floor;
    if(p.shape_14>=3&&p.byte_dc&&!s.specialized){
        trace.reprobe=run_actor_floor_reprobe(position.probe_reference,host.collision);
        sample=trace.reprobe;
    }
    // 1e4430..4be0: floor-height filter and published floor plane.
    std::optional<ActorFloorSample> filter_sample;
    if(sample)filter_sample=ActorFloorSample{sample->point,
        {sample->face_normal.x,sample->face_normal.y,sample->face_normal.z,0}};
    trace.filter=update_actor_floor_filter(p,s,position,in.support,filter_sample,p.byte_dc!=0,
        in.elapsed,in.inverse_elapsed,host.floor_root_orientation);
    // 1e4be4..4dcc: root placement; floor hit/height come from the floor probe.
    ActorResponsePlacementState placement_state{p.e0,p.height_394};
    ActorResponsePlacementInput placement_in;
    placement_in.resolved_position=position.root_target;
    placement_in.floor_hit=trace.floor.has_value();
    placement_in.flag_dc=p.byte_dc!=0;
    if(trace.floor)placement_in.floor_height=trace.floor->point[2];
    placement_in.flags34=p.flags_34;
    host.placement(placement_state,placement_in);
    p.e0=placement_state.support_height;p.height_394=placement_state.child_offset;
    finish_actor_response(p,s,in.support_scaled[2]);
    return trace;
}
}
