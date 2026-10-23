#include "level_lights.hpp"
#include "game_libm.hpp"
#include "vu_float.hpp"
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
using V=ShadowVector;
// Interpreter model (as character_shadows.cpp): binary32, round to nearest,
// denormal results flushed to zero; compiled with -ffp-contract=off.
float ftz(float v){return std::fpclassify(v)==FP_SUBNORMAL?(std::signbit(v)?-0.0F:0.0F):v;}
float add(float a,float b){return ftz(a+b);}
float sub(float a,float b){return ftz(a-b);}
float mul(float a,float b){return ftz(a*b);}
// div.s: a zero divisor gives the largest float with the quotient's sign (EE FPU).
float fdiv(float a,float b){
    if(b==0.0F)return std::copysign(std::numeric_limits<float>::max(),std::signbit(a)!=std::signbit(b)?-1.0F:1.0F);
    return ftz(a/b);
}
void require(bool ok,const std::string& what){if(!ok)throw std::runtime_error("level lights: "+what);}
std::string hex(std::uint32_t v){
    static const char* digits="0123456789abcdef";
    std::string s(8,'0');
    for(int i=7;i>=0;--i){s[static_cast<std::size_t>(i)]=digits[v&15U];v>>=4;}
    return s;
}

// 00179540 / 00178aa0 weight: vmul.xyz squares, ACC = 1*r2, +1*g2, +1*b2, then div.s 1/sum.
float colour_weight(const V& c){
    const float sum=add(add(mul(c[0],c[0]),mul(c[1],c[1])),mul(c[2],c[2]));
    return fdiv(1.0F,sum);
}
// 001799a4..00179a00: cosf(k*(0.5*angle)), k = 0x3c8efa35 (pi/180).
float cone_cosine(float degrees){
    return game_cosf(mul(std::bit_cast<float>(0x3c8efa35U),mul(0.5F,degrees)));
}
// 00374270: count entries, zeroed, every id -1.
std::vector<LevelLight> allocate_pool(std::size_t count){return std::vector<LevelLight>(count);}
// 00373e70: the first entry whose id is -1 gets id = index + 1.
std::optional<std::size_t> take(std::vector<LevelLight>& pool){
    for(std::size_t i=0;i<pool.size();++i)
        if(pool[i].id==-1){pool[i].id=static_cast<std::int32_t>(i+1);return i;}
    return std::nullopt;
}
// 00178aa0 (directional / ambient base init): type, colour (w as passed), position (0,0,0,1), weight.
void base_init(LevelLight& l,std::uint8_t type,const V& colour,const V& position){
    l.vtable=level_light_vtable_base;l.intensity=0.0F;l.enabled=false;l.aux=0;l.type=type;
    l.colour=colour;l.position=position;l.weight=colour_weight(colour);
}
// 00179240 for one DIR record: the light configured and left disabled.
void configure_directional(LevelLight& l,const PopulationDirectionalLight& r){
    base_init(l,2,{r.colour[0],r.colour[1],r.colour[2],0.0F},{0.0F,0.0F,0.0F,1.0F});
    l.vtable=level_light_vtable_directional;
    l.direction={r.direction[0],r.direction[1],r.direction[2],0.0F};   // 00132020
    l.intensity=r.enable?r.intensity:0.0F;                              // 00179418..00179434
    l.enabled=false;                                                    // vcall +0x30 (00144980)
}
// The objects 0040c480(category, index) can name among those this module registers.
struct Registered{bool environment; std::size_t index;};
std::vector<Registered> find_objects(const LevelLights& lights,std::uint32_t category,std::uint32_t index,std::size_t environments){
    std::vector<Registered> hits;
    if(category>=0x100||index>=0x10000)return hits;                     // 0040c530 / 0040c520
    const std::uint32_t key=(category<<16)|index;                        // 0040c500 with a3 = -1
    for(std::size_t i=0;i<lights.objects.size();++i)if(lights.objects[i].id==key)hits.push_back({false,i});
    for(std::size_t i=0;i<environments;++i)if(lights.environments[i].id()==key)hits.push_back({true,i});
    return hits;
}
const LevelLightObject& light_object(const LevelLights& lights,std::uint32_t id){
    const LevelLightObject* found=nullptr;
    for(const auto& o:lights.objects)if(o.id==id){
        require(found==nullptr,"two light objects with id "+hex(id));
        found=&o;
    }
    require(found!=nullptr,"no light object with id "+hex(id));
    return *found;
}
// Light vcall +0x2c / +0x30 on a light object's light: PNT 00371b00 / 00371a60 (the patch slots, then
// +0x1c), DIR 00144b80 / 00144980 (+0x1c); 00535d62 on a +0x1c change.
void set_light_object(LevelLights& lights,const LevelLightObject& object,bool on){
    switch_level_light(lights,object.light,on,object.id);
}
}

std::vector<LevelLight>& LevelLights::pool(LevelLightPool p){
    switch(p){
    case LevelLightPool::ambient:return ambient;
    case LevelLightPool::directional:return directional;
    case LevelLightPool::point:return point;
    case LevelLightPool::spot:return spot;
    }
    throw std::runtime_error("level lights: unknown pool");
}
const std::vector<LevelLight>& LevelLights::pool(LevelLightPool p) const{return const_cast<LevelLights*>(this)->pool(p);}
LevelLight& LevelLights::light(const LevelLightRef& r){
    auto& p=pool(r.pool);
    require(r.index<p.size(),"light reference outside its pool");
    return p[r.index];
}
const LevelLight& LevelLights::light(const LevelLightRef& r) const{return const_cast<LevelLights*>(this)->light(r);}

LevelLightSources population_light_sources(const Population& population){
    LevelLightSources s;
    for(const auto& section:population.sections){
        if(section.tag=="PNT")s.point=population.point_lights;
        else if(section.tag=="DIR")s.directional=population.directional_lights;
        else if(section.tag=="ENV")s.environments=population.environments;
    }
    return s;
}

LevelLights load_level_lights(const LevelLightSources& src,const std::vector<LevelLightPatchModel>& patch_models){
    LevelLights out;
    // 001c1a20: spot pool = PNT count; with DIR: directional = DIR count, point 24 (001c1b34), ambient 1 (001c1b48).
    if(src.point){
        out.spot=allocate_pool(src.point->size());
        if(src.directional){
            out.directional=allocate_pool(src.directional->size());
            out.point=allocate_pool(24);
            out.ambient=allocate_pool(1);
        }
    }
    // 001c1770: returns at once without PNT:.
    if(src.point){
        for(const auto& r:*src.point){                                         // 00179620
            const auto index=take(out.spot);
            require(index.has_value(),"spot pool exhausted");
            LevelLight& l=out.spot[*index];
            base_init(l,0x10,{r.colour[0],r.colour[1],r.colour[2],0.0F},{r.position[0],r.position[1],r.position[2],1.0F});   // 00179540
            l.vtable=level_light_vtable_spot;
            l.enabled=true;                                                    // 001794a0
            l.id=static_cast<std::int32_t>(r.id());                            // 00179490
            l.inner=r.inner;l.outer=r.outer;
            l.cone_outer=cone_cosine(r.cone_angle_outer);
            l.cone_inner=cone_cosine(r.cone_angle_inner);
            l.direction={r.direction[0],r.direction[1],r.direction[2],0.0F};
            l.intensity=r.enable?r.intensity:0.0F;
            if(src.version>=0x3b){
                if(r.aux_08)l.aux|=0x08;
                if(r.aux_10)l.aux|=0x10;
            }
            out.objects.push_back({r.id(),{LevelLightPool::spot,*index}});     // 0040c9d0
        }
        if(src.directional){
            for(std::size_t k=0;k<src.directional->size();++k){                // 001c1920: the first four become objects
                const auto& r=(*src.directional)[k];
                const auto index=take(out.directional);
                // 00179240 stores the null light at +0x60 and then writes through it.
                require(index.has_value(),"directional pool exhausted");
                LevelLight& l=out.directional[*index];
                configure_directional(l,r);
                if(k<4){out.objects.push_back({r.id(),{LevelLightPool::directional,*index}});continue;}
                // 00179c20 -> light vcall +8 (00179cc0 -> 00144a60): the entry is released.
                l.vtable=level_light_vtable_released;l.id=-1;l.enabled=false;
            }
        }
    }
    // 001c3940 (after 001c1e40): the TER: (001c2cb0) and GOB: model loads, each 00377920 registering its sets.
    for(const auto& m:patch_models)
        for(std::size_t i=0;i<m.keys.size();++i)register_level_light_patch(out,m.keys[i],m.model,static_cast<std::uint32_t>(i));
    // 001c1470 (after 001c3940, before 001c12a0): each PNT record with neither link word -1 deactivates its own
    // object (0040c480 of its id words, virtual +60 with 1).
    if(src.point_links&&src.point)for(const auto& r:*src.point){
        if(r.skipped[0]==0xffffffffU||r.skipped[1]==0xffffffffU)continue;
        const auto hits=find_objects(out,r.id_category,r.id_index,0);
        require(hits.size()<=1,"PNT link pass: the record's id names more than one object");
        if(!hits.empty())set_light_object(out,out.objects[hits[0].index],false);
    }
    // 001c12a0
    require(src.environments.has_value(),"ENV: section missing (001c12a0 reads its count past the end of the file)");
    for(const auto& r:*src.environments){                                      // 00178630
        // References resolve against the objects registered so far (0040c480 at 00178918).
        for(std::uint32_t ref:r.references){
            const auto hits=find_objects(out,r.id_category,ref,out.environments.size());
            require(hits.size()<=1,"ENV reference names more than one object");
            require(hits.empty()||!hits[0].environment,"ENV reference names an ENV object (activation not modelled)");
        }
        if(const auto index=take(out.ambient)){
            LevelLight& l=out.ambient[*index];
            base_init(l,1,{r.values[7],r.values[8],r.values[9],0.0F},{0.0F,0.0F,0.0F,1.0F});
            l.vtable=level_light_vtable_ambient;
            l.position={0.0F,0.0F,0.0F,0.0F};                                  // vcall +0x28 (00144b10) with a zero qword
            l.enabled=true;                                                    // vcall +0x2c (00144b80)
        }
        out.environments.push_back(r);                                          // 001c1440, 0040c9d0
    }
    // 001c13f0..001c1414 -> 001781a0: the first ENV becomes current.
    if(!out.environments.empty()){
        out.current_environment=0;
        const auto& env=out.environments[0];
        for(std::uint32_t ref:env.references){                                 // 001782f0..00178320, vcall +0x5c
            const auto hits=find_objects(out,env.id_category,ref,0);
            if(hits.empty())continue;
            set_light_object(out,out.objects[hits[0].index],true);
            out.environment_activated.push_back(out.objects[hits[0].index].id);
        }
        // 00178328..001783b4: the ambient light with id 1 takes the current colour.
        for(auto& l:out.ambient)if(l.id==1&&l.type==1){
            l.colour={env.values[7],env.values[8],env.values[9],0.0F};
            l.weight=colour_weight(l.colour);
            break;
        }
    }
    return out;
}

bool register_level_light_patch(LevelLights& lights,std::uint32_t key,std::uint32_t model,std::uint32_t index){
    require(model!=0,"00371c30 with a null model");
    for(auto& l:lights.spot){
        if(static_cast<std::uint32_t>(l.id)!=key)continue;
        for(auto& s:l.patch_slots){
            if(s.model==model)return false;
            if(s.model==0){s={model,index};++l.patch_models_5c;return true;}
        }
        return false;
    }
    return false;
}
void switch_level_light(LevelLights& lights,const LevelLightRef& ref,bool on,std::uint32_t object_id){
    auto& l=lights.light(ref);
    if(l.vtable==level_light_vtable_spot&&(l.patches_on_58!=0)!=on&&l.patch_models_5c!=0){
        for(const auto& s:l.patch_slots)if(s.model)lights.patch_calls.push_back({s.model,s.index,on,object_id});   // 003767e0
        l.patches_on_58=on?1:0;
    }
    if(l.enabled!=on){l.enabled=on;lights.dirty_42=1;}
}
bool is_level_light_object(const LevelLights& lights,std::uint32_t id){
    for(const auto& o:lights.objects)if(o.id==id)return true;
    return false;
}
void activate_level_light_object(LevelLights& lights,std::uint32_t id){
    set_light_object(lights,light_object(lights,id),true);                   // 00179bb0 / 00179200 -> vcall +0x2c
}
void deactivate_level_light_object(LevelLights& lights,std::uint32_t id){
    set_light_object(lights,light_object(lights,id),false);                  // 00179b70 / 001791c0 -> vcall +0x30
}
float level_camera_far(const LevelLights& lights){
    require(lights.current_environment.has_value()&&*lights.current_environment<lights.environments.size(),
        "no current environment for the camera far distance");
    return add(12.0F,lights.environments[*lights.current_environment].far_term());   // 00132ee0, 00133004
}

float level_far_distance(const LevelLights& lights){
    require(lights.current_environment.has_value()&&*lights.current_environment<lights.environments.size(),
        "no current environment for the far distance");
    return lights.environments[*lights.current_environment].far_term();   // 00133648
}

std::uint32_t level_frame_mode(const LevelLights& lights){
    require(lights.current_environment.has_value()&&*lights.current_environment<lights.environments.size(),
        "no current environment for the frame mode");
    const PopulationEnvironment& env=lights.environments[*lights.current_environment];
    std::uint32_t mode=0x75;                                                    // 00132e6c
    if(!(env.values[4]<=0.0F))mode|=env.fog_mode?0x100U:0x80U;                  // +0x6c (00132e94..00132ec8)
    return mode;
}

LevelFog renderer_fog(float n,float f,float d0,float d1){                       // 001339b0
    require(std::isfinite(n)&&std::isfinite(f)&&std::isfinite(d0)&&std::isfinite(d1),"fog values not finite");
    require(f!=n,"fog near and far distances are equal (001339b0 divides by far - near)");
    const float a=mul(255.0F,sub(1.0F,d0)),b=mul(255.0F,sub(1.0F,d1));            // f9, f8
    LevelFog fog;
    fog.base=fdiv(add(add(a,b),fdiv(mul(sub(b,a),add(f,n)),sub(f,n))),2.0F);        // +0x180
    fog.scale=fdiv(mul(mul(f,n),sub(a,b)),sub(f,n));                                // +0x190
    require(std::isfinite(fog.base)&&std::isfinite(fog.scale),"fog terms not finite");
    return fog;
}
LevelFog level_fog(const LevelLights& lights){
    require(lights.current_environment.has_value()&&*lights.current_environment<lights.environments.size(),
        "no current environment for the fog terms");
    const PopulationEnvironment& env=lights.environments[*lights.current_environment];
    // values: +0x70 +0x74 +0x78 +0x68 +0x6c +0x60 +0x64 ...; 001335d8..00133648.
    return renderer_fog(env.values[5],env.values[6],env.values[3],env.values[4]);
}
float renderer_aspect(int width,int height){                                      // 0037f740
    static constexpr std::array<std::array<float,2>,5> screens{{{1280,720},{640,512},{640,480},{640,360},{320,240}}};   // 004bc1d0
    require(width>0&&height>0,"frame buffer size must be positive");
    const float w=static_cast<float>(width),h=static_cast<float>(height);          // cvt.s.w
    std::size_t k=0;
    while(k<screens.size()&&w<=screens[k][0]&&h<=screens[k][1])++k;
    require(k>0,"frame buffer larger than the first virtual screen (0037f740 would read before 004bc1d0)");
    return fdiv(screens[k-1][0],screens[k-1][1]);                                   // +0xf88 / +0xf8c
}

ShadowMatrix level_camera_projection(const LevelLightCamera& c){                // 003754a0
    require(!c.orthographic,"orthographic camera records (00375390) are not supported");
    require(std::isfinite(c.half_angle)&&std::isfinite(c.near_plane)&&std::isfinite(c.far_plane)&&std::isfinite(c.aspect),
        "camera values not finite");
    const float t=game_tanf(c.half_angle);
    require(t!=0.0F&&c.aspect!=0.0F&&c.far_plane!=c.near_plane,"degenerate camera projection");
    ShadowMatrix p;
    p.column[0]={fdiv(1.0F,t),0.0F,0.0F,0.0F};
    p.column[1]={0.0F,fdiv(1.0F,mul(t,fdiv(1.0F,c.aspect))),0.0F,0.0F};
    const float depth=sub(c.far_plane,c.near_plane);
    p.column[2]={0.0F,0.0F,fdiv(add(c.far_plane,c.near_plane),depth),1.0F};
    p.column[3]={0.0F,0.0F,fdiv(ftz(-mul(mul(2.0F,c.far_plane),c.near_plane)),depth),0.0F};
    return p;
}

std::vector<LevelLightTableRow> rebuild_level_light_table(const LevelLights& lights,const ShadowMatrix& view,const LevelLightCamera& camera){
    std::vector<LevelLightTableRow> rows;
    // 00373670: the clip matrix of the camera test is (P*A)*I (identity view pushed), D = far for perspective.
    const ShadowMatrix p=level_camera_projection(camera);
    ShadowMatrix identity;
    for(std::size_t k=0;k<4;++k)identity.column[k][k]=1.0F;
    const ShadowMatrix clip=shadow_multiply(shadow_multiply(p,shadow_axis_matrix()),identity);
    const float half=mul(0.5F,camera.far_plane);                                // 00373718
    const float radius=fdiv(half,game_cosf(camera.half_angle));
    const V centre{0.0F,half,0.0F,1.0F};
    const LevelLightPool order[3]={LevelLightPool::point,LevelLightPool::spot,LevelLightPool::directional};
    for(const LevelLightPool pool:order){
        const auto& entries=lights.pool(pool);
        for(std::size_t i=0;i<entries.size();++i){
            const LevelLight& l=entries[i];
            if(!l.enabled)continue;                                              // 00373940: +0x1c
            std::uint8_t cls=0;
            if(l.type==2)cls=1;
            else{
                for(float x:l.position)require(std::isfinite(x),"light position not finite");
                const V v=shadow_transform(view,l.position);
                // 00379e20 on v (VCLIPw bits against |w|); its depth bit 0x40 is masked off.
                const V c=shadow_transform(clip,v);
                const float w=std::fabs(c[3]);
                int code=0;
                for(std::size_t a=0;a<3;++a){
                    if(c[a]>w)code|=1<<(2*a);
                    if(c[a]<-w)code|=2<<(2*a);
                }
                if(code==0)cls=3;
                else{
                    float range=0.0F;                                            // light vcall +0x34
                    if(l.vtable==level_light_vtable_flashlight)range=lights.flash_dazzle_size;   // 0045bc00 (00497a20)
                    else if(l.vtable==level_light_vtable_base||l.vtable==level_light_vtable_spot||l.vtable==level_light_vtable_directional||
                            l.vtable==level_light_vtable_point||l.vtable==level_light_vtable_ambient)range=l.outer;   // 001479f0
                    else require(false,"enabled light with an unsupported range method (vtable "+hex(l.vtable)+")");
                    const float threshold=add(radius,mul(0.5F,range));           // 00373a30
                    const V d{sub(centre[0],v[0]),sub(centre[1],v[1]),sub(centre[2],v[2]),0.0F};
                    const float dist=ftz(vu::sqrt(add(add(mul(d[0],d[0]),mul(d[1],d[1])),mul(d[2],d[2]))));
                    if(dist<threshold)cls=2;
                }
            }
            if(cls!=0)rows.push_back({{pool,i},cls});
        }
    }
    return rows;
}

namespace {
// The pools flattened as LevelLightTable::set.lights; base[pool] is the pool's first index there.
void flatten_level_light_pools(const LevelLights& lights,LevelLightTable& out,std::size_t (&base)[4]){
    out.set.any_lists=!lights.directional.empty()||!lights.point.empty()||!lights.spot.empty();   // manager +0x14/+0x18/+0x1c
    const LevelLightPool order[3]={LevelLightPool::point,LevelLightPool::spot,LevelLightPool::directional};
    for(const LevelLightPool pool:order){
        base[static_cast<std::size_t>(pool)]=out.set.lights.size();
        const auto& entries=lights.pool(pool);
        for(std::size_t i=0;i<entries.size();++i){
            const LevelLight& l=entries[i];
            ShadowLight s;
            s.type=l.type;s.intensity=l.intensity;s.inner=l.inner;s.outer=l.outer;s.weight=l.weight;
            s.position=l.position;s.direction=l.direction;s.cone_outer=l.cone_outer;s.cone_inner=l.cone_inner;
            out.set.lights.push_back(s);
            out.lights.push_back({pool,i});
        }
    }
}
}

LevelLightTable level_start_light_table(const LevelLights& lights){
    LevelLightTable out;
    std::size_t base[4]{};
    flatten_level_light_pools(lights,out,base);
    return out;
}

LevelLightTable build_level_light_table(const LevelLights& lights,const ShadowMatrix& view,const LevelLightCamera& camera){
    LevelLightTable out;
    out.rows=rebuild_level_light_table(lights,view,camera);
    std::size_t base[4]{};
    flatten_level_light_pools(lights,out,base);
    for(const LevelLightTableRow& row:out.rows){
        const LevelLight& l=lights.light(row.light);
        if(l.type==0x10)require(row.light.pool==LevelLightPool::spot,"spot-typed light outside the spot pool in the table");
        if(l.type==2)require(row.light.pool!=LevelLightPool::point,"directional light in the point pool in the table");
        ShadowLightTableEntry e=shadow_light_table_entry(out.set.lights,base[static_cast<std::size_t>(row.light.pool)]+row.light.index);   // 00373ae8..00373b70
        out.set.table.push_back(e);
    }
    return out;
}
}
