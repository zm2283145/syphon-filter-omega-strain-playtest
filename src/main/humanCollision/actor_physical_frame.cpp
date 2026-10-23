#include "float_bits.hpp"
#include "actor_physical_frame.hpp"
#include "actor_footstep.hpp"
#include <cmath>
#include <stdexcept>

namespace omega {
namespace {
float check(float v){
    if(::omega::float_bits::nonfinite_or_subnormal(v))
        throw std::runtime_error("Physical frame: unsupported nonfinite/subnormal arithmetic");
    return v;
}
void to_response(ActorPhysicalFrameState& s){
    auto& p=s.response.physical;
    p.mode_20=s.history.current.value;p.flags_34=s.history.current_flags;p.radius_370=s.history.radius;
    p.predicted_2f0=s.integrator.position;p.position_300=s.integrator.previous_position;
    p.velocity_310=s.integrator.velocity;p.acceleration_320=s.integrator.acceleration;p.force_330=s.integrator.force;
    p.contact_motion_1e0=s.tail.contact_motion;
    p.use_new_offset_11=s.tail.reset_requested; // +11 is both the reset request and 001e2180's offset selector
    s.response.mass_2e0=s.integrator.mass;
    p.byte_3d4=s.auxiliary.second_hit;          // +3d4: second probe result of the previous 001ec580
}
void from_response(ActorPhysicalFrameState& s){
    const auto& p=s.response.physical;
    s.integrator.position=p.predicted_2f0;s.integrator.previous_position=p.position_300;
    s.integrator.velocity=p.velocity_310;s.integrator.acceleration=p.acceleration_320;s.integrator.force=p.force_330;
    s.history.radius=p.radius_370;
    s.tail.contact_motion=p.contact_motion_1e0;
}
float water_normal_z(const ActorPhysicalFrameState& s){
    if(!s.response.water)throw std::runtime_error("Physical frame: water height without retained water triangle");
    return s.response.water->triangle.normal[2]; // +1a8
}
}

void initialize_actor_physical_frame(ActorPhysicalFrameState& s,const ActorPhysicalInitializationHost& host){
    auto& p=s.response.physical;
    ActorPhysicalInitializationState init;
    init.integrator=s.integrator;
    init.contacts={ActorPhysicalInitialContact{p.contact_40,p.contact_50,p.contact_60},
        ActorPhysicalInitialContact{p.contact_70,p.contact_80,p.contact_90}};
    init.flag_dc=p.byte_dc;init.flag_dd=p.byte_dd;init.primary_support_enabled=p.e8;
    init.base_height=p.e0;init.contact_height=p.e4;init.plane=s.plane_f0;
    init.scalar_2c4=p.floor_2c4;init.scalar_394=p.height_394;
    initialize_actor_physical_state(init,host);
    s.integrator=init.integrator;
    p.contact_40=init.contacts[0].position;p.contact_50=init.contacts[0].direction;p.contact_60=init.contacts[0].scalar;
    p.contact_70=init.contacts[1].position;p.contact_80=init.contacts[1].direction;p.contact_90=init.contacts[1].scalar;
    p.byte_dc=init.flag_dc;p.byte_dd=init.flag_dd;p.e8=init.primary_support_enabled;
    p.e0=init.base_height;p.e4=init.contact_height;s.plane_f0=init.plane;
    p.floor_2c4=init.scalar_2c4;p.height_394=init.scalar_394;
}

ActorPhysicalFrameTrace run_actor_physical_frame(ActorPhysicalFrameState& s,float elapsed,
    const ActorPhysicalFrameHost& host,const ActorResponseTuning& tuning,const RootVector& gravity){
    if(!host.initialize||!host.edge_presets||!host.component_translation||!host.water_support)
        throw std::runtime_error("Physical frame: complete host required");
    check(elapsed);
    if(elapsed<=0)throw std::runtime_error("Physical frame: positive elapsed time required");
    ActorPhysicalFrameTrace trace;
    const float inverse=check(1.0F/elapsed);
    // 1e99c8..: entry capture of water support (+e4, +1a8).
    auto& p=s.response.physical;
    const float height=p.e4;
    trace.entry_capture=height==actor_unavailable_height?capture_actor_support_height(height,actor_unavailable_height,0)
        :capture_actor_support_height(height,actor_unavailable_height,water_normal_z(s));
    if(s.tail.reset_requested)host.initialize(s);                        // +11 -> 001e9380
    s.history=advance_actor_physical_history(s.history,elapsed);         // 001ea1b0 x2, flags, +370
    trace.prediction=prepare_actor_physical_prediction(s.integrator,elapsed);
    s.integrator=trace.prediction.state;                                  // includes +330 clear
    to_response(s);
    host.edge_presets(s);                                                 // 001dfb80
    to_response(s);
    trace.support=consume_actor_support_motion(inverse,host.root_keys,host.support_motion); // 001e7110
    if(actor_physical_mode_runs_response(s.history.current.value)){
        ActorPhysicalResponseInput in;
        in.support=trace.support.displacement[0];in.support_scaled=trace.support.scaled_displacement[0];
        in.elapsed=elapsed;in.inverse_elapsed=inverse;in.gravity=gravity;in.tuning=tuning;
        trace.response=run_actor_physical_response(s.response,in,host.response);
        from_response(s);
        // 1e4064..408c: a floor-probe hit (+e8 = 1, in the floor response) publishes the plane
        // 001e5430(face normal +30, point +50) at +f0 (001e5420).
        if(trace.response->floor){
            const auto& f=*trace.response->floor;
            s.plane_f0=actor_floor_plane({f.face_normal.x,f.face_normal.y,f.face_normal.z,0},f.point);
            // 1e40a0..40ec: 001e1b70 stores the hit's source (sp+1280 word 4) at record +64.
            s.floor_contact_object_164=f.source.object;
        }
    }
    capture_actor_support_history(s.support_history,host.support_history); // 001e6300
    clear_actor_physical_override(s.integrator);                           // +9d0
    // Auxiliary origin: +2f0 + +380 once initialized, else component translation + +380.
    const RootVector base=s.tail.initialized?s.integrator.position:host.component_translation();
    for(std::size_t i=0;i<4;++i)trace.auxiliary_origin[i]=check(base[i]+p.offset_380[i]);
    trace.auxiliary=run_actor_auxiliary_probe(s.auxiliary,trace.auxiliary_origin,p.e0,host.auxiliary);
    const float after=p.e4;
    host.water_support(trace.entry_capture,after,after==actor_unavailable_height?0.0F:water_normal_z(s)); // 001e5cf0
    finish_actor_physical_step(s.integrator,s.tail,elapsed);              // +1e0, +2d0, +10, +11
    p.contact_motion_1e0=s.tail.contact_motion;p.use_new_offset_11=s.tail.reset_requested;
    return trace;
}
}
