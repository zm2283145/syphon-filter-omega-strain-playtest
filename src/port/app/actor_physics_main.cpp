// Diagnostic harness: one authored actor driven by the native physical update
// (prediction + 001e2180) over a real, locally extracted COL5 resource.
// Authored inputs (root weights, model anchor, shape, start point) are explicit
// command-line choices, not recovered actor data. Missing original stages are
// listed in the JSON header; this is not a playable character controller.
#include "actor_contact_events.hpp"
#include "actor_physical_prediction.hpp"
#include "actor_physical_response.hpp"
#include "actor_physical_state.hpp"
#include "actor_gather_bounds.hpp"
#include "actor_animation.hpp"
#include "actor_heading_state.hpp"
#include "player_pad_control.hpp"
#include "animation_model.hpp"
#include <memory>
#include "binary.hpp"
#include "disc.hpp"
#include "native_paths.hpp"
#include "population.hpp"
#ifdef OMEGA_HAS_VERIFIED_ISO
#include "windows_hash.hpp"
#endif
#include "actor_physical_frame.hpp"
#include "actor_water_support.hpp"
#include "animation_root_clip.hpp"
#include "col_gather.hpp"
#include "root_attachments.hpp"
#include "solo_actor_simulation.hpp"
#include "float_environment.hpp"
#include <algorithm>
#include <array>
#include <sstream>
#include <cmath>
#include <optional>
#include <span>
#include <vector>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
using namespace omega;

std::vector<std::uint8_t> read_file(const std::string& path){
    std::ifstream in(path,std::ios::binary);
    if(!in)throw std::runtime_error("Cannot open "+path);
    return {std::istreambuf_iterator<char>(in),{}};
}

// AnimationModel (CoreMotions graph) as the simulation's animation model.
struct ModelAnimation final:SoloActorAnimation {
    AnimationModel& model;const AnimationGraph& graph;const SkeletonDefinition& skeleton;const AnimationClipBank& clips;
    ModelAnimation(AnimationModel& m,const AnimationGraph& g,const SkeletonDefinition& s,const AnimationClipBank& c)
        :model(m),graph(g),skeleton(s),clips(c){}
    std::size_t channel_count() const override{return model.channel_count();}
    const AnimationChannelState& channel(std::size_t i) const override{return model.channel(i);}
    void apply_commands(std::span<const ActorChannelCommand> c) override{model.apply_commands(c);}
    void set_anchor_world_orientation(const RootAnchorKey& k,const RootVector& q,bool reset) override{model.set_anchor_world_orientation(k,q,reset);}
    void set_anchor_world_position(const RootAnchorKey& k,const RootVector& v) override{model.set_anchor_world_position(k,v);}
    void set_anchor_world_placement(const RootAnchorKey& k,const RootParentTransform& t,bool reset) override{model.set_anchor_world_placement(k,t,reset);}
    void set_anchor_state(const RootAnchorKey& k,const RootAnimationState& s) override{model.set_anchor_state(k,s);}
    void set_anchor_parent(const RootAnchorKey& k,std::optional<RootParentTransform> p) override{model.set_anchor_parent(k,std::move(p));}
    void set_anchor_constraint(const RootAnchorKey& k,std::optional<RootMotionConstraint> c) override{model.set_anchor_constraint(k,c);}
    void step(float dt) override{model.step(graph,skeleton,clips,dt);}
    RootAnimationState anchor_state(const RootAnchorKey& k) const override{
        for(const auto& anchor:model.player().anchors)if(anchor.key==k)return anchor.state;
        throw std::runtime_error("Animation model has no such root anchor");
    }
};
}

int main(int argc,char** argv){
    try{
        enable_ps2_denormal_flush();
        std::vector<std::string> col_paths;std::string col_path,out_path,iso_path,mission="TORONTO1";bool drop_given=false,with_objects=false;std::size_t object_count=0;int frames=120;float dt=1.0F/60.0F,drop=3.0F;
        int shape=3;float radius=1.74F;bool keep_velocity=true;float walk_x=0,walk_y=0,pose_z=0,yaw_degrees=0;std::string skel_path,clip_path;
        std::string graph_path,tuning_path,motion_name="CoreMotions",skeleton_name="MALE.SKEL";
        float stick_x=0,stick_y=0,turn_rate=0;std::optional<float> stick_speed;
        std::optional<std::array<float,2>> pad;float pad_10=0,pad_11=0;
        for(int i=1;i<argc;++i){
            const std::string a=argv[i];
            if(a=="--col"&&i+1<argc){col_path=argv[++i];col_paths.push_back(col_path);}
            else if(a=="--iso"&&i+1<argc)iso_path=argv[++i];
            else if(a=="--mission"&&i+1<argc)mission=argv[++i];
            else if(a=="--objects")with_objects=true;
            else if(a=="--out"&&i+1<argc)out_path=argv[++i];
            else if(a=="--frames"&&i+1<argc)frames=std::stoi(argv[++i]);
            else if(a=="--dt"&&i+1<argc)dt=std::stof(argv[++i]);
            else if(a=="--drop"&&i+1<argc){drop=std::stof(argv[++i]);drop_given=true;}
            else if(a=="--shape"&&i+1<argc)shape=std::stoi(argv[++i]);
            else if(a=="--radius"&&i+1<argc)radius=std::stof(argv[++i]);
            else if(a=="--reset-velocity")keep_velocity=false;
            else if(a=="--walk"&&i+2<argc){walk_x=std::stof(argv[++i]);walk_y=std::stof(argv[++i]);}
            else if(a=="--pose-z"&&i+1<argc)pose_z=std::stof(argv[++i]);
            else if(a=="--skel"&&i+1<argc)skel_path=argv[++i];
            else if(a=="--clip"&&i+1<argc)clip_path=argv[++i];
            else if(a=="--yaw"&&i+1<argc)yaw_degrees=std::stof(argv[++i]);
            else if(a=="--graph"&&i+1<argc)graph_path=argv[++i];
            else if(a=="--tuning"&&i+1<argc)tuning_path=argv[++i];
            else if(a=="--motion"&&i+1<argc)motion_name=argv[++i];
            else if(a=="--skeleton"&&i+1<argc)skeleton_name=argv[++i];
            else if(a=="--stick"&&i+2<argc){stick_x=std::stof(argv[++i]);stick_y=std::stof(argv[++i]);}
            else if(a=="--speed"&&i+1<argc)stick_speed=std::stof(argv[++i]);
            else if(a=="--turn"&&i+1<argc)turn_rate=std::stof(argv[++i]);
            else if(a=="--pad"&&i+2<argc){const float x=std::stof(argv[i+1]),y=std::stof(argv[i+2]);i+=2;pad=std::array<float,2>{x,y};}
            else if(a=="--pad-strafe"&&i+2<argc){pad_10=std::stof(argv[++i]);pad_11=std::stof(argv[++i]);}
            else throw std::runtime_error("Usage: omega_actor_physics (--col <local COL5>... | --iso <verified ISO> [--mission TORONTO1|BELARUS1] [--objects]) [--out json] [--frames N]"
                " [--dt s] [--drop ft] [--shape byte14] [--radius ft] [--reset-velocity] [--walk vx vy] [--pose-z ft] [--skel SKEL --clip SKA] [--yaw degrees]"
                " [--graph <local manifest> --tuning <local tuning> [--motion CoreMotions] [--stick x y] [--speed s] [--turn rad/s] | [--pad x y] [--pad-strafe p10 p11]]");
        }
        if(col_paths.empty()==iso_path.empty())throw std::runtime_error("Give either --col files or --iso");
        SoloCollisionWorld world;
        std::optional<RootVector> spawn;float spawn_yaw=0;
        // Graph mode: the native animation model (CoreMotions by default) driven
        // by the ordinary channel producer (00199f30) supplies the "flor" root.
        std::unique_ptr<AnimationGraphManifest> manifest;std::optional<SkeletonDefinition> graph_skeleton;
        AnimationClipBank clip_bank;std::optional<ActorAnimationTuning> tuning;
        if(!graph_path.empty()){
            if(tuning_path.empty())throw std::runtime_error("--graph needs --tuning (local actor animation tuning)");
            std::ifstream tuning_stream(tuning_path);
            if(!tuning_stream)throw std::runtime_error("Cannot open "+tuning_path);
            tuning=read_actor_animation_tuning(tuning_stream);
            manifest=std::make_unique<AnimationGraphManifest>(load_animation_graph_manifest(graph_path));
        }
        for(const auto& path:col_paths)world.add(read_file(path),path);
        std::optional<std::array<std::uint32_t,2>> record_parameters;
        if(!iso_path.empty()){
#ifdef OMEGA_HAS_VERIFIED_ISO
            // Terrain COL5 resources and the first NPC's loader pose (the local
            // player, CARTHAGE_PLAYER.md) straight from the verified disc.
            if(mission!="TORONTO1"&&mission!="BELARUS1")throw std::runtime_error("Supported missions: TORONTO1, BELARUS1");
            auto stream=open_verified_iso(iso_path,OMEGA_ISO_SIZE,OMEGA_ISO_SHA256);
            DiscImage disc(stream,OMEGA_ISO_SIZE);
            const auto population=decode_population(disc.read(disc.file({"GAMEDATA",mission,"DATA.POP"})));
            for(const auto& item:population.terrain){
                const auto name=uppercase_asset(item.name.substr(0,item.name.size()-4));
                const auto data=disc.archive_member({"GAMEDATA",mission,"DATA.HOG"},name+".HOG");
                const auto extent=hog_member(data,data.size(),name+".COL");
                const auto bytes=checked_bytes(data,static_cast<std::size_t>(extent.offset),static_cast<std::size_t>(extent.size));
                world.add({bytes.begin(),bytes.end()},name);
            }
            // Game objects at their stored (population) poses; scripts and path
            // motion are not run, so movable objects stay at their initial pose.
            if(with_objects)for(const auto& object:population.objects){
                if(object.asset=="no_vum")continue;
                const auto name=uppercase_asset(object.asset.substr(0,object.asset.size()-4));
                const auto data=disc.archive_member({"GAMEDATA",mission,"OBJECTS.HOG"},name+".HOG");
                const auto extent=hog_member(data,data.size(),name+".COL");
                const auto bytes=checked_bytes(data,static_cast<std::size_t>(extent.offset),static_cast<std::size_t>(extent.size));
                const auto& b=object.basis;const auto& t=object.position;
                world.add({bytes.begin(),bytes.end()},name,CollisionMatrix{b[0],b[1],b[2],0,b[3],b[4],b[5],0,b[6],b[7],b[8],0,t.x,t.y,t.z,1});
                ++object_count;
            }
            if(manifest){
                // Same provenance checks and asset sources as omega_actor.
                if(manifest->iso_sha256!=OMEGA_ISO_SHA256)throw std::runtime_error("Graph and ISO provenance differ");
                const auto skas=disc.archive_member({"GAMEDATA","COMMON","SKAS.HOG"},manifest->source_skas);
                std::istringstream hash_input(std::string(skas.begin(),skas.end()));
                if(sha256_stream(hash_input)!=manifest->skas_sha256)throw std::runtime_error("Graph animation-set provenance differs");
                graph_skeleton=decode_skeleton(disc.archive_member({"GAMEDATA","COMMON","SKEL.HOG"},skeleton_name));
                for(const auto& node:manifest->graph.nodes())
                    if((node.kind==AnimationGraphKind::Leaf||node.kind==AnimationGraphKind::ControlledLeaf)&&!clip_bank.contains(node.leaf.clip_key))
                        clip_bank.emplace(node.leaf.clip_key,decode_animation_clip(disc.archive_member({"GAMEDATA","COMMON","SKA.HOG"},
                            node.leaf.clip_key),static_cast<std::uint32_t>(graph_skeleton->bones.size())));
            }
            const auto pose=npc_spawn_pose(population);
            record_parameters=population.npcs.at(0).parameters;           // health/armor words of the player record
            spawn=RootVector{pose.position.x,pose.position.y,pose.position.z,1};
            // Heading of normalized basis column 1 (model +Y, the clips' forward axis).
            spawn_yaw=std::atan2(-pose.basis[3],pose.basis[4])*180.0F/3.14159265F;
            col_path=iso_path+":"+mission;
#else
            throw std::runtime_error("--iso needs the Windows build with verified-ISO support");
#endif
        }
        if(manifest&&!graph_skeleton)throw std::runtime_error("--graph needs --iso (clips and skeleton come from the verified disc)");
        // Optional real root clip ("flor" anchor), sampled each frame with the
        // native default sampler and single-anchor accumulation.
        if(skel_path.empty()!=clip_path.empty())throw std::runtime_error("--skel and --clip go together");
        std::optional<SkeletonDefinition> skeleton;std::optional<AnimationClip> clip;float clip_duration=0,phase=0;
        if(!clip_path.empty()){
            skeleton=decode_skeleton(read_file(skel_path));
            clip=decode_animation_clip(read_file(clip_path),static_cast<std::uint32_t>(skeleton->bones.size()));
            clip_duration=static_cast<float>(clip->frame_intervals)*clip->frame_period;
            if(!(clip_duration>0))throw std::runtime_error("Clip has no duration");
            const auto first=sample_default_root_clip(*clip,*skeleton,0,0);
            if(first.pose_translation)pose_z=(*first.pose_translation)[2];
        }
        RootVector start;
        if(spawn){start=*spawn;start[2]+=drop_given?drop:0.0F;yaw_degrees+=spawn_yaw;}
        else start=world.pick_start(drop);
        const std::size_t resource_count=world.cols.size();

        std::unique_ptr<AnimationModel> model;std::unique_ptr<ModelAnimation> animation;
        if(manifest){
            model=std::make_unique<AnimationModel>(*graph_skeleton,*manifest);
            animation=std::make_unique<ModelAnimation>(*model,manifest->graph,*graph_skeleton,clip_bank);
        }
        // Retained control fields: desired vector +31c0, speed +31d0, turn +31d4.
        // Pad mode starts from zero fields; the controller fills them each frame.
        SoloActorControlFields initial;
        if(!pad){initial.desired={stick_x,stick_y,0,0};
            initial.speed=stick_speed?*stick_speed:std::sqrt(stick_x*stick_x+stick_y*stick_y);initial.turn=turn_rate;}
        SoloActorConfig config;config.start=start;config.yaw_degrees=yaw_degrees;config.radius=radius;
        config.population_parameters=record_parameters;
        config.shape=shape;config.keep_velocity=keep_velocity;config.dt=dt;
        SoloActorSimulation sim(std::move(world),config,animation.get(),tuning,initial);
        if(model)model->play(motion_name,MotionInstallMode::reset);
        const auto& frame_state=sim.physical();
        const auto& p=frame_state.response.physical;

        std::ostream* out=&std::cout;std::ofstream file;
        if(!out_path.empty()){file.open(out_path);if(!file)throw std::runtime_error("Cannot write "+out_path);out=&file;}
        std::string col_json;for(char c:col_path){if(c=='\\'||c=='"')col_json+='\\';col_json+=c;}
        *out<<std::setprecision(9)<<"{\"col\":\""<<col_json<<"\",\"start\":["<<start[0]<<','<<start[1]<<','<<start[2]
            <<"],\"dt\":"<<dt<<",\"pose_z\":"<<pose_z<<",\"clip\":\""<<(clip?"yes":"no")<<"\",\"yaw\":"<<yaw_degrees<<",\"turn\":"<<turn_rate
            <<",\"resources\":"<<resource_count<<",\"objects\":"<<object_count<<",\"mass\":"<<frame_state.integrator.mass<<",\"shape\":"<<shape<<",\"radius\":"<<radius
            <<",\"missing\":[\"001dfb80 edge detection body\",\"skeleton nodes\",\"actor list\",\"animation graph\"],\"frames\":[";
        for(int frame=0;frame<frames;++frame){
            SoloActorFrameInput input;
            if(model){
                if(pad){
                    // Physical deflection to centered pad bytes (up = byte 0), then 0024b5d0.
                    PlayerMoveInput move;move.left=player_pad_stick((*pad)[0]*127.5F,-(*pad)[1]*127.5F);
                    move.pressure_10=pad_10;move.pressure_11=pad_11;
                    input.pad=move;
                }
            }else{
                // Root frame for "flor": a real clip when given, else authored
                // (weight 1, identity pose, constant motion). Stands in for 003ab510.
                RootFrame root_frame;root_frame.weight=1;root_frame.pose_quaternion={0,0,0,1};
                root_frame.pose_translation={0,0,pose_z,0};
                root_frame.motion_buckets[0]={walk_x*dt,walk_y*dt,0,0};
                if(clip){
                    // Looping playback: advance normalized phase by dt/duration.
                    const float advance=dt/clip_duration;
                    phase+=advance;if(phase>=1.0F)phase-=1.0F;
                    const auto sample=sample_default_root_clip(*clip,*skeleton,phase,advance);
                    const std::array anchors{RootAnchorDefinition{sample.anchor_key,{}}};
                    const std::array contributions{WeightedRootClipSample{1,sample,0,false}};
                    root_frame=accumulate_root_clip_contributions(anchors,contributions).anchors.at(0).frame;
                }
                input.root_frame=root_frame;
            }
            const auto result=sim.step(input);
            const auto& frame_trace=result.trace;
            const auto root=sim.floor_root_placement();
            const auto& position=frame_state.integrator.position;
            *out<<(frame?",":"")<<"{\"frame\":"<<frame<<",\"position\":["<<position[0]<<','<<position[1]
                <<','<<position[2]<<"],\"root\":["<<root.position[0]<<','
                <<root.position[1]<<','<<root.position[2]
                <<"],\"heading\":"<<2.0F*std::atan2(root.orientation[2],root.orientation[3])*57.29578F
                <<",\"published_z\":"<<(sim.published()?std::to_string(sim.published_position()[2]):std::string("null"));
            if(frame_trace.response){
                const auto& trace=*frame_trace.response;
                *out<<",\"floor\":"<<(trace.floor?std::to_string(trace.floor->point[2]):std::string("null"))
                    <<",\"body_hit\":"<<(trace.body&&trace.body->world_contact?"true":"false");
                if(trace.body&&trace.body->world_contact){const auto& c=*trace.body->world_contact;
                    *out<<",\"contact_normal\":["<<c.normal.x<<','<<c.normal.y<<','<<c.normal.z<<"],\"contact_point\":["
                        <<c.point.x<<','<<c.point.y<<','<<c.point.z<<"],\"contact_type\":"<<c.type;}
            }
            *out<<",\"dc\":"<<int(p.byte_dc)<<",\"dd\":"<<int(p.byte_dd)<<",\"force_z\":"<<p.force_330[2]
                <<",\"filter\":"<<p.height_filter.value<<",\"offset_394\":"<<p.height_394
<<",\"water_events\":"<<result.water_events;
            if(pad){const auto& c=sim.controls();
                *out<<",\"pad\":{\"direction\":["<<c.desired[0]<<','<<c.desired[1]<<"],\"speed\":"<<c.speed
                <<",\"turn\":"<<c.turn<<",\"backward\":"<<(sim.pad_state().backward_2c?"true":"false")<<'}';}
            if(model){const auto& w=sim.state_words();
                *out<<",\"words\":["<<w.previous_3200<<','<<w.current_3204<<','<<w.pending_3208<<"],\"events\":["
                    <<result.stance_events<<','<<result.running_events<<']';}
            if(model){*out<<",\"channels\":[";for(std::size_t i=0;i<18;++i)*out<<(i?",":"")<<result.channels[i];*out<<']';}
            *out<<'}';
        }
        *out<<"]}\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<"omega_actor_physics: "<<e.what()<<'\n';return 1;}
}
