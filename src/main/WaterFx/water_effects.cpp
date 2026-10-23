#include "water_effects.hpp"
#include <bit>
#include <cmath>
#include <stdexcept>

namespace omega {
namespace {
constexpr float spawn_interval=0.0375F;      // 3d19999a
constexpr float height_offset=0.2F;          // 3e4ccccd
constexpr float splash_threshold=1.5F;       // 3fc00000
constexpr float ripple_lifetime=std::bit_cast<float>(0x3f99999aU); // 1.2
constexpr std::array<std::uint32_t,3> ripple_variants{0,2,3}; // 004abfa0
// 004abf60..9c, written by static initializer 004d33d0.
constexpr std::array<RootVector,4> ripple_corners{{{-1,-1,0,0},{-1,1,0,0},{1,-1,0,0},{1,1,0,0}}};
constexpr float random_scale=2147483648.0F;  // 4f000000

void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
float clamp_nonnegative(float value){return value<0.0F?0.0F:value;} // max.S 0
std::int32_t required_generation(WaterEffectActorHost& host,std::uint32_t actor){
    const auto value=host.generation(actor);
    require(value.has_value(),"Water effect read a destroyed actor through a live pointer");
    return *value;
}
float uniform(std::uint64_t& state){
    return game_int_to_float(static_cast<std::int32_t>(next_game_random(state)))/random_scale;
}
// 00132130 rotation about a unit axis, then the 0023b450 product with identity:
// rows R0,R1,R2 and a zero fourth row (the product omits the W term).
std::array<RootVector,4> ripple_basis(float angle){
    constexpr float x=0,y=0,z=1;
    const float s=std::sin(angle),c=std::cos(angle),k=1.0F-c;
    const float xy=y*k*x,yz=z*k*y,zx=x*k*z;
    const std::array<RootVector,4> r{{
        {c+k*x*x,xy+s*z,zx-s*y,0},{xy-s*z,c+k*y*y,yz+s*x,0},{zx+s*y,yz-s*x,c+k*z*z,0},{0,0,0,1}}};
    std::array<RootVector,4> identity{{{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}}},m{};
    for(std::size_t row=0;row<4;++row)for(std::size_t lane=0;lane<4;++lane)
        m[row][lane]=r[0][lane]*identity[row][0]+r[1][lane]*identity[row][1]+r[2][lane]*identity[row][2];
    return m;
}
RootVector transform_corner(const std::array<RootVector,4>& m,const RootVector& v){
    RootVector out{};
    for(std::size_t lane=0;lane<4;++lane)
        out[lane]=m[0][lane]*v[0]+m[1][lane]*v[1]+m[2][lane]*v[2]+m[3][lane]*v[3];
    const float length2=out[0]*out[0]+out[1]*out[1]+out[2]*out[2];
    const float q=1.0F/std::sqrt(length2); // VU RSQRT (002b3b94..002b3d2c): float sqrt, float 1/x
    out[0]*=q;out[1]*=q;out[2]*=q;
    return out;
}
// 002b4710.
void initialize_splash(WaterEffectRecord& record,float radius,float height,
                       const RootVector& source,WaterEffectSink& sink){
    record.splash_timer=5.0F;
    record.splash_position=source;
    record.splash_position[2]=height;
    const float outer=2.5F*radius;
    record.splash_inner=0.15F*radius;
    record.splash_outer=outer-record.splash_inner;
    const RootVector point{record.splash_position[0],record.splash_position[1],
        record.splash_position[2]-0.25F*radius,1.0F};
    sink.splash(radius<2.0F?std::uint8_t{0x12}:std::uint8_t{0x11},point);
}
void initialize_ripple_shape(WaterEffectRecord& record){
    // mula 3.8*r, mul 0.6*r, msub: retained as separate binary32 operations.
    const float r=record.radius,large=3.8F*r;
    record.ripple_inner=0.6F*r;
    record.ripple_outer=large-0.6F*r;
    record.ripple_height=record.height;
}
}

std::uint32_t next_game_random(std::uint64_t& state){
    state=state*0x5851f42d4c957f2dULL+1ULL;
    return static_cast<std::uint32_t>(state>>32)&0x7fffffffU;
}
float game_int_to_float(std::int32_t value){
    if(value==0)return 0.0F;
    const bool negative=value<0;
    std::uint32_t magnitude=negative?0U-static_cast<std::uint32_t>(value):static_cast<std::uint32_t>(value);
    const int width=std::bit_width(magnitude);
    if(width>24)magnitude&=~((1U<<(width-24))-1U); // truncate to 24 significant bits
    const float result=static_cast<float>(magnitude); // now exact
    return negative?-result:result;
}

WaterEffectManager::WaterEffectManager(std::uint64_t& random_state):random_(&random_state){
    // 002b2d30: 4x4 grid; 0.33333334 steps accumulate in binary32.
    constexpr float step=0.33333334F;
    float v=0.5F,row_t=0.0F;std::size_t index=0;
    for(int row=0;row<4;++row){
        float u=-0.5F,column_t=0.0F;
        for(int column=0;column<4;++column,++index){
            surface_.positions[index]={u,v,0,1};
            surface_.texcoords[index]={column_t,row_t,1,0};
            const float first=uniform(random_state),second=uniform(random_state),third=uniform(random_state);
            surface_.colors[index]={1.0F*third+0.0F,1.0F*second+0.0F,first*1.0F+0.0F,1.0F};
            u+=step;column_t+=step;
        }
        v-=step;row_t+=step;
    }
    float angle=-3.1415927F;
    for(float& value:surface_.sine){value=std::sin(angle);angle+=0.09817477F;}
}

const WaterEffectRecord* WaterEffectManager::slot(std::size_t index) const{
    require(index<slot_count,"Water effect slot out of range");
    return slots_[index].get();
}

std::uint32_t WaterEffectManager::resolve(const WaterEffectRecord& record,WaterEffectActorHost& host) const{
    if(record.owner==0)return 0;
    const auto current=host.generation(record.owner);
    return current&&*current==record.generation?record.owner:0;
}

int WaterEffectManager::lookup(std::uint32_t actor,WaterEffectActorHost& host) const{
    if(actor==0)return -1;
    for(std::size_t i=0;i<slot_count;++i){
        const auto* record=slots_[i].get();
        if(record&&resolve(*record,host)==actor)return static_cast<int>(i);
    }
    return -1;
}

WaterEffectRecord& WaterEffectManager::allocate(std::size_t index,WaterEffectSink& sink){
    // 002b2a40 constructor. Allocation failure is not modeled: the original
    // installs null, increments +450 and then dereferences it.
    auto record=std::make_unique<WaterEffectRecord>();
    const auto resources=sink.record_resources();
    record->splash_resource=resources.splash_resource;
    record->ripple_resource=resources.ripple_resource;
    record->ripple_frame_count=resources.ripple_frame_count;
    slots_[index]=std::move(record);
    ++allocated_;
    return *slots_[index];
}

void WaterEffectManager::ensure(float height,std::uint32_t actor,WaterEffectActorHost& host,WaterEffectSink& sink){
    const float offset_height=height_offset+height;
    if(!(allocated_<20&&active_<8)||actor==0)return;
    const std::int32_t captured=required_generation(host,actor);
    const auto recheck=[&]{require(host.generation(actor)==captured,
        "Water effect actor generation changed during eligibility (original null dereference)");};
    recheck();
    if(!host.has_transform_component(actor))return;
    recheck();
    if(!host.bounds(actor))return;
    recheck();
    if(host.byte_49(actor)!=2)return;
    if(lookup(actor,host)>=0)return; // a found index always holds a record
    std::size_t index=slot_count;
    for(std::size_t i=0;i<slot_count;++i)if(!slots_[i]){index=i;break;}
    require(index<slot_count,"Water effect allocation count disagrees with occupied slots");
    auto& record=allocate(index,sink);
    const auto bounds=host.bounds(actor);
    require(bounds.has_value(),"Water effect bounds call returned null after eligibility");
    record.owner=actor;
    record.generation=required_generation(host,actor);
    const float dx=bounds->maximum[0]-bounds->minimum[0],dy=bounds->maximum[1]-bounds->minimum[1];
    record.radius=0.5F*std::sqrt(dx*dx+dy*dy+0.0F);
    record.position=host.refreshed_translation(actor);
    record.displacement={};
    record.height=offset_height;
    if(!(record.position[2]<=splash_threshold+height))
        initialize_splash(record,record.radius,record.height,record.position,sink);
    initialize_ripple_shape(record);
    sink.request({WaterEffectRequestKind::start,index,actor,record.position});
    ++active_;
}

void WaterEffectManager::update_height(float height,std::uint32_t actor,WaterEffectActorHost& host){
    const float offset_height=height_offset+height;
    if(actor==0)return;
    const int index=lookup(actor,host);
    if(index<0)return;
    auto* record=slots_[static_cast<std::size_t>(index)].get();
    if(!record)return;
    record->height=offset_height;
    record->ripple_height=offset_height;
}

void WaterEffectManager::remove(std::uint32_t actor,WaterEffectActorHost& host,WaterEffectSink& sink){
    if(actor==0)return;
    const int index=lookup(actor,host);
    if(index<0)return;
    const auto slot_index=static_cast<std::size_t>(index);
    sink.request({WaterEffectRequestKind::stop,slot_index,actor,{}});
    auto* record=slots_[slot_index].get(); // reread after synchronous delivery
    require(record!=nullptr,"Water effect slot vanished during stop delivery (original null store)");
    record->owner=0;
    record->generation=-1;
    --active_;
}

void WaterEffectManager::add_point_splash(float radius,float height,const RootVector& position,WaterEffectSink& sink){
    const float offset_height=height_offset+height;
    std::size_t index=slot_count;
    for(std::size_t i=0;i<slot_count;++i)if(!slots_[i]){index=i;break;}
    if(index==slot_count)return;
    auto& record=allocate(index,sink);
    record.owner=0;
    record.generation=-1;
    record.radius=radius;
    record.position=position;
    record.displacement={};
    record.height=offset_height;
    initialize_splash(record,record.radius,record.height,record.position,sink);
    initialize_ripple_shape(record);
}

void age_water_ripples(float elapsed,WaterEffectRecord& record){
    for(auto it=record.ripples.begin();it!=record.ripples.end();){
        it->lifetime=clamp_nonnegative(it->lifetime-elapsed);
        if(it->lifetime==0.0F)it=record.ripples.erase(it);else ++it;
    }
}

void WaterEffectManager::advance_ripples(float elapsed,WaterEffectRecord& record,std::uint32_t owner,
                                         WaterEffectActorHost& host,const RootVector& motion){
    const float speed=std::sqrt(motion[0]*motion[0]+motion[1]*motion[1]+0.0F);
    const float half=0.5F*elapsed;
    if(!(record.ripple_accumulator<spawn_interval)){
        record.ripple_accumulator=record.ripple_accumulator-spawn_interval;
        if(record.ripples.size()>=ripple_capacity)record.ripples.pop_back(); // oldest
        const float angle=std::atan2(-motion[0],motion[1]);
        const auto basis=ripple_basis(angle);
        WaterRippleNode node;
        const RootVector translation=host.current_translation(owner);
        node.position=translation;
        node.position[2]=record.ripple_height;
        for(std::size_t i=0;i<4;++i)node.corners[i]=transform_corner(basis,ripple_corners[i]);
        node.sinking=speed<=-motion[2]?1.0F:0.0F;
        const float pick=uniform(*random_)*3.0F;
        node.variant=ripple_variants.at(static_cast<std::size_t>(static_cast<std::int32_t>(pick)));
        node.lifetime=ripple_lifetime;
        const std::int32_t last=static_cast<std::int32_t>(record.ripple_frame_count)-2+1;
        node.frame=static_cast<std::int32_t>(game_int_to_float(last)*uniform(*random_));
        record.ripples.push_front(node);
    }
    age_water_ripples(elapsed,record);
    if(!(speed<=half)||speed< -motion[2])record.ripple_accumulator=record.ripple_accumulator+elapsed;
}

void WaterEffectManager::update(float elapsed,WaterEffectActorHost& host){
    require(std::isfinite(elapsed)&&elapsed>=0.0F,"Water effect update requires finite nonnegative time");
    for(std::size_t i=0;i<slot_count;++i){
        auto* record=slots_[i].get();
        if(!record)continue;
        const std::uint32_t owner=resolve(*record,host);
        if(owner==0){
            record->splash_timer=clamp_nonnegative(record->splash_timer-elapsed);
            age_water_ripples(elapsed,*record);
        }else{
            const RootVector translation=host.refreshed_translation(owner);
            require(resolve(*record,host)==owner,
                "Water effect owner invalidated by transform refresh (original null dereference)");
            const auto bounds=host.bounds(owner);
            require(bounds.has_value(),"Water effect owner bounds call returned null");
            if(translation[2]+bounds->minimum[2]<=record->height){
                RootVector motion{};
                for(std::size_t lane=0;lane<4;++lane)motion[lane]=translation[lane]-record->position[lane];
                record->displacement={motion[0],motion[1],0,0};
                record->position=translation;
                record->splash_timer=clamp_nonnegative(record->splash_timer-elapsed);
                advance_ripples(elapsed,*record,owner,host,motion);
            }else{
                record->splash_timer=clamp_nonnegative(record->splash_timer-elapsed);
                age_water_ripples(elapsed,*record);
            }
        }
        if(resolve(*record,host)==0&&record->splash_timer==0.0F&&record->ripples.empty()){
            slots_[i].reset(); // 002b20e0 frees ripple nodes and the record
            --allocated_;
        }
    }
}

void WaterEffectManager::clear(){
    for(auto& record:slots_)if(record){record.reset();--allocated_;}
}
}
