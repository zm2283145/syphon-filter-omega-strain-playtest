#include "float_bits.hpp"
#include "character_shadows.hpp"
#include "game_libm.hpp"
#include "vu_float.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace omega {
namespace {
using V=ShadowVector;
float fb(std::uint32_t v){return std::bit_cast<float>(v);}
// Interpreter model: every float result flushed to zero when denormal. No
// operation here is contracted (the file is compiled with -ffp-contract=off).
float ftz(float v){return ::omega::float_bits::subnormal(v)?(std::signbit(v)?-0.0F:0.0F):v;}
float add(float a,float b){return ftz(a+b);}
float sub(float a,float b){return ftz(a-b);}
float mul(float a,float b){return ftz(a*b);}
// VMADD/ACC accumulation: the product is rounded, then the sum, then flushed.
float madd(float acc,float a,float b){return ftz(acc+a*b);}
float dvd(float a,float b){
    if(b==0)throw std::runtime_error("shadow: division by zero");
    return ftz(a/b);}
// MAX.S / MIN.S as the interpreter evaluates them (first operand unless the second is strictly greater/smaller).
float max_s(float s,float t){return t>s?t:s;}
float min_s(float s,float t){return t<s?t:s;}
void require(bool ok,const char* what){if(!ok)throw std::runtime_error(std::string("shadow: ")+what);}
void require_finite(const V& v,const char* what){for(float x:v)require(::omega::float_bits::finite(x),what);}
void require_finite(const ShadowMatrix& m,const char* what){for(const auto& c:m.column)require_finite(c,what);}

V add4(const V& a,const V& b){return {add(a[0],b[0]),add(a[1],b[1]),add(a[2],b[2]),add(a[3],b[3])};}
V sub4(const V& a,const V& b){return {sub(a[0],b[0]),sub(a[1],b[1]),sub(a[2],b[2]),sub(a[3],b[3])};}
V scale4(const V& a,float s){return {mul(a[0],s),mul(a[1],s),mul(a[2],s),mul(a[3],s)};}
// vmulax.xyzw ACC,vf0,vf0 / vmsubw.xyzw: 0 - v*1 in every lane (zero stays +0).
V negate4(const V& a){return {sub(0.0F,a[0]),sub(0.0F,a[1]),sub(0.0F,a[2]),sub(0.0F,a[3])};}
// vmul.xyz squares, then vmulax.w/vmadday.w/vmaddz.w with vf0.w = 1: (x2 + y2) + z2.
float sum_squares(const V& v){return add(add(mul(v[0],v[0]),mul(v[1],v[1])),mul(v[2],v[2]));}
// vmul.xyzw then (x + y) + z (00192fb0 and the inline form in 003d4810).
float dot3(const V& a,const V& b){return add(add(mul(a[0],b[0]),mul(a[1],b[1])),mul(a[2],b[2]));}
// VSQRT of the sum of squares (00183350, 003d3d50, 003d4810).
float length(const V& v){return ftz(vu::sqrt(sum_squares(v)));}
// 00132060 / 00132030: xyz *= VRSQRT(1, sum); w unchanged.
V normalized(const V& v,const char* what){
    const float s=sum_squares(v);
    require(s!=0.0F,what);
    const float q=ftz(vu::rsqrt(s));
    return {mul(v[0],q),mul(v[1],q),mul(v[2],q),v[3]};
}
// 001320f0: vopmula/vopmsub, w = vf0.w - vf0.w.
V cross(const V& a,const V& b){
    return {ftz(mul(a[1],b[2])-b[1]*a[2]),ftz(mul(a[2],b[0])-b[2]*a[0]),ftz(mul(a[0],b[1])-b[0]*a[1]),0.0F};
}
// 0013ed60 / the tail of 00133200: xyz = (c0*t.x + c1*t.y) + c2*t.z, w kept.
V rotate3(const ShadowMatrix& m,const V& t,float w){
    V r{};
    for(std::size_t k=0;k<3;++k)r[k]=madd(madd(mul(m.column[0][k],t[0]),m.column[1][k],t[1]),m.column[2][k],t[2]);
    r[3]=w;
    return r;
}
bool zero3(const V& v){return v[0]==0.0F&&v[1]==0.0F&&v[2]==0.0F;}
// cvt.w.s as the interpreter evaluates it: truncation, saturated.
std::int32_t to_int(float v){
    if(v>=2147483647.0F)return 2147483647;
    if(v<=-2147483648.0F)return -2147483647-1;
    return static_cast<std::int32_t>(v);
}

// Light candidate (light manager scratchpad +0x2000 + 8*k: light, key).
struct Candidate{std::size_t light; float key;};
// 00373ba0
int compare(const Candidate& a,const Candidate& b){
    if(a.key<b.key)return -1;
    if(a.key<=b.key)return 0;
    return 1;
}
// 001273c8: the executable's qsort (BSD form: insertion sort below 7 elements,
// median of three / ninther above 7 / 40, three-way partition, insertion sort
// when a pass swaps nothing, recursion on the left part, loop on the right).
// Only the final order matters; element indices stand in for pointers.
using Index=std::ptrdiff_t;
void insertion(std::vector<Candidate>& v,Index a,Index n){
    for(Index pm=a+1;pm<a+n;++pm)
        for(Index pl=pm;pl>a&&compare(v[static_cast<std::size_t>(pl-1)],v[static_cast<std::size_t>(pl)])>0;--pl)
            std::swap(v[static_cast<std::size_t>(pl)],v[static_cast<std::size_t>(pl-1)]);
}
void game_qsort(std::vector<Candidate>& v,Index a,Index n){
    auto at=[&](Index i)->Candidate&{return v[static_cast<std::size_t>(i)];};
    auto cmp=[&](Index i,Index j){return compare(at(i),at(j));};
    auto swp=[&](Index i,Index j){std::swap(at(i),at(j));};
    auto vecswap=[&](Index i,Index j,Index count){for(Index k=0;k<count;++k)swp(i+k,j+k);};
    auto med3=[&](Index x,Index y,Index z){
        return cmp(x,y)<0?(cmp(y,z)<0?y:(cmp(x,z)<0?z:x)):(cmp(y,z)>0?y:(cmp(x,z)<0?x:z));};
    for(;;){
        bool swapped=false;
        if(n<7){insertion(v,a,n);return;}
        Index pm=a+n/2;
        if(n>7){
            Index pl=a,pn=a+n-1;
            if(n>40){
                const Index d=n/8;
                pl=med3(pl,pl+d,pl+2*d);
                pm=med3(pm-d,pm,pm+d);
                pn=med3(pn-2*d,pn-d,pn);
            }
            pm=med3(pl,pm,pn);
        }
        swp(a,pm);
        Index pa=a+1,pb=a+1,pc=a+n-1,pd=a+n-1;
        for(;;){
            int r=0;
            while(pb<=pc&&(r=cmp(pb,a))<=0){
                if(r==0){swapped=true;swp(pa,pb);++pa;}
                ++pb;
            }
            while(pb<=pc&&(r=cmp(pc,a))>=0){
                if(r==0){swapped=true;swp(pc,pd);--pd;}
                --pc;
            }
            if(pb>pc)break;
            swp(pb,pc);swapped=true;++pb;--pc;
        }
        if(!swapped){insertion(v,a,n);return;}
        const Index pn=a+n;
        Index r=std::min(pa-a,pb-pa);
        vecswap(a,pb-r,r);
        r=std::min(pd-pc,pn-pd-1);
        vecswap(pb,pn-r,r);
        if((r=pb-pa)>1)game_qsort(v,a,r);
        if((r=pd-pc)>1){a=pn-r;n=r;continue;}
        return;
    }
}
void validate(const ShadowSettings& s){
    require(s.entry_cap>=0&&static_cast<std::size_t>(s.entry_cap)<=shadow_max_entries,"entry cap outside the 6 entries of a caster record");
    require(s.caster_cap>=0&&static_cast<std::size_t>(s.caster_cap)<=shadow_max_casters,"caster cap outside the 4 caster records");
    require(s.receiver_radius>=0&&s.receiver_radius<(1<<24),"receiver radius outside the exactly representable range");
    require(::omega::float_bits::finite(s.reflection_strength),"reflection strength not finite");
}
// 003d3d50 acceptance by the box test: rejected only on 0x80 (every corner
// outside one common clip plane); the depth bit 0x40 never reaches that test.
bool box_outside(const ShadowMatrix& clip,const V& lo,const V& hi){
    return shadow_box_clip_code(clip,lo,hi,0.0F)==0x80;
}
}

ShadowVector shadow_transform(const ShadowMatrix& m,const ShadowVector& v){
    V r{};
    for(std::size_t k=0;k<4;++k)r[k]=madd(madd(madd(mul(m.column[0][k],v[0]),m.column[1][k],v[1]),m.column[2][k],v[2]),m.column[3][k],v[3]);
    return r;
}
ShadowMatrix shadow_multiply(const ShadowMatrix& a,const ShadowMatrix& b){
    ShadowMatrix r;
    for(std::size_t k=0;k<4;++k)r.column[k]=shadow_transform(a,b.column[k]);
    return r;
}
ShadowMatrix shadow_rigid_inverse(const ShadowMatrix& m){   // 00133200
    ShadowMatrix r=m;
    std::swap(r.column[0][1],r.column[1][0]);
    std::swap(r.column[0][2],r.column[2][0]);
    std::swap(r.column[1][2],r.column[2][1]);
    const V t=m.column[3];
    if(!zero3(t))r.column[3]=rotate3(r,{sub(0.0F,t[0]),sub(0.0F,t[1]),sub(0.0F,t[2]),t[3]},t[3]);
    return r;
}
ShadowMatrix shadow_axis_matrix(){                           // 0037ec24..0037ec64
    ShadowMatrix m;
    m.column[0]={1.0F,0.0F,0.0F,0.0F};
    m.column[1]={0.0F,0.0F,1.0F,0.0F};
    m.column[2]={0.0F,1.0F,0.0F,0.0F};
    m.column[3]={0.0F,0.0F,0.0F,1.0F};
    return m;
}
ShadowMatrix shadow_look_at(const ShadowVector& eye,const ShadowVector& direction){   // 003d2b20
    // Auxiliary axis 00497f30 = (0,0,1,0) or 00497f20 = (1,0,0,0) when |dir.z| >= 0.70711 (003d2bf0).
    const V aux=std::fabs(direction[2])<fb(0x3f34fdf4U)?V{0.0F,0.0F,1.0F,0.0F}:V{1.0F,0.0F,0.0F,0.0F};
    const V forward=normalized(direction,"look-at direction of zero length");      // 00132060
    const V right=normalized(cross(direction,aux),"look-at right axis of zero length"); // 001320f0, 00132030
    const V up=cross(right,forward);
    // Rows right/forward/up (00131fd0, 00131fa0, 00131f70), translation = eye, then the 3x3 transpose 0013ee20.
    ShadowMatrix m;
    m.column[0]={right[0],forward[0],up[0],right[3]};
    m.column[1]={right[1],forward[1],up[1],forward[3]};
    m.column[2]={right[2],forward[2],up[2],up[3]};
    m.column[3]=eye;
    // 0013edb0: a zero translation is kept; otherwise 0013ed90/0013ed60 rotate its negation.
    if(!zero3(eye))m.column[3]=rotate3(m,{sub(0.0F,eye[0]),sub(0.0F,eye[1]),sub(0.0F,eye[2]),eye[3]},eye[3]);
    return m;
}
ShadowPerspective shadow_perspective(const ShadowPerspectiveInput& in){                // 003d2970
    const float t0=game_tanf(in.half_angle),t1=game_tanf(in.half_angle);  // tanf is called twice
    const float two_near=mul(2.0F,in.near_plane);
    const float v_span=ftz(std::fabs(sub(in.v1,in.v0)));
    ShadowPerspective p;
    auto& m=p.matrix.column;
    m[0]={dvd(two_near,mul(in.x_scale,mul(two_near,t0))),0.0F,0.0F,0.0F};
    m[1]={0.0F,dvd(two_near,mul(in.y_scale,mul(two_near,t1))),0.0F,0.0F};
    const float two_far_near=mul(mul(2.0F,in.far_plane),in.near_plane);
    const float depth=sub(in.far_plane,in.near_plane);
    m[2]={0.0F,0.0F,dvd(add(in.far_plane,in.near_plane),depth),1.0F};
    m[3]={0.0F,0.0F,dvd(ftz(-two_far_near),depth),0.0F};
    const float u_half=mul(0.5F,ftz(std::fabs(sub(in.u1,in.u0))));
    p.scale={u_half,mul(0.5F,ftz(-v_span)),-64.0F,1.0F};
    p.offset={u_half,mul(0.5F,v_span),64.0F,0.0F};
    if(!(in.intensity<=0.0F)){p.scale[2]=0.0F;p.offset[2]=mul(128.0F,in.intensity);}
    return p;
}
float shadow_fog_factor(const ShadowMatrix& fog_matrix,float fog_base,float fog_scale,const ShadowVector& position){   // 003d3700
    const float w=shadow_transform(fog_matrix,position)[3];
    // div.s 003d3778: a zero w (the point in the camera plane, e.g. the player's root with the camera
    // pushed against it) gives the largest float with the quotient's sign, +FLT_MAX for 0 / 0 (EE FPU as
    // the interpreter runs it; tests/shadow_fog_zero_goldens.hpp).
    const float q=w!=0.0F?dvd(fog_scale,w)
        :std::copysign(std::numeric_limits<float>::max(),fog_scale!=0.0F&&std::signbit(fog_scale)!=std::signbit(w)?-1.0F:1.0F);
    float f=add(fog_base,q);
    if(f<=0.0F)f=0.0F;
    if(!(f<255.0F))f=255.0F;
    return f;
}
std::int32_t shadow_silhouette_level(float strength,float fog){   // 003d2f98..003d2fa8, 003d32e4..003d32f8
    const float f=mul(fog,fb(0x3b808081U));
    return to_int(add(1.0F,mul(f,mul(127.0F,strength))));
}
int shadow_clip_code(const ShadowMatrix& clip,const ShadowVector& point,float depth_reference){   // 00379e20
    const V v=shadow_transform(clip,{point[0],point[1],point[2],1.0F});
    const float w=std::fabs(v[3]);
    int code=0;
    for(std::size_t k=0;k<3;++k){
        if(v[k]>w)code|=1<<(2*k);
        if(v[k]<-w)code|=2<<(2*k);
    }
    if(!(v[2]<depth_reference))code|=0x40;   // z from the VSQI copy at VU0 data 0x11004008 vs renderer +0x1b8
    return code;
}
int shadow_box_clip_code(const ShadowMatrix& clip,const ShadowVector& lo,const ShadowVector& hi,float depth_reference){   // 00379b70
    const V corners[8]={{lo[0],lo[1],lo[2],1.0F},{lo[0],hi[1],lo[2],1.0F},{hi[0],lo[1],lo[2],1.0F},{hi[0],hi[1],lo[2],1.0F},
                        {lo[0],hi[1],hi[2],1.0F},{hi[0],lo[1],hi[2],1.0F},{lo[0],lo[1],hi[2],1.0F},{hi[0],hi[1],hi[2],1.0F}};
    int any=0,all=0x3f,behind=0;
    for(const V& c:corners){
        const int code=shadow_clip_code(clip,c,depth_reference);
        any|=code;all&=code;behind+=code&0x20;
    }
    if(all!=0)return 0x80;
    int r=behind!=0?4:(any==0||any==0x40?1:2);
    if(any&0x40)r|=0x10;
    return r;
}

ShadowLightTableEntry shadow_light_table_entry(const std::vector<ShadowLight>& lights,std::size_t index){   // 00373ae8..00373b70
    require(index<lights.size(),"light index out of range");
    const ShadowLight& l=lights[index];
    return {l.type,index,l.outer,l.weight,l.position};
}

bool register_shadow_caster(ShadowFrame& frame,const ShadowSettings& settings,const ShadowCasterInput& caster,std::size_t index){
    // 0013f234..0013f2c0: limit 2 for an actor whose +0x58 object reports visible, else 1;
    // with 005721c8 set only visible actors are registered.
    const bool visible=caster.has_visibility_object&&caster.visible;
    if(settings.require_visible_casters&&!visible)return false;
    const int limit=visible?2:1;
    // 003d5260
    if((settings.mode&(shadow_mode_shadows|shadow_mode_reflections))==0)return false;
    if(!(static_cast<int>(frame.casters.size())<settings.caster_cap))return false;
    if(caster.hidden||!caster.model_ready)return false;
    frame.casters.push_back({index,limit,{}});
    return true;
}
bool register_shadow_receiver(ShadowFrame& frame,const ShadowSettings& settings,const ShadowReceiverInput& receiver,std::size_t index){
    if((receiver.model_flags&0x60U)==0||!receiver.model_loaded)return false;          // 0013f1ec..0013f208
    if((settings.mode&(shadow_mode_shadows|shadow_mode_reflections))==0)return false;   // 003d40d0 (third argument 0)
    if(frame.receivers.size()>=shadow_max_receivers||receiver.hidden)return false;
    V c=scale4(add4(receiver.bounds_min,receiver.bounds_max),0.5F);
    if(receiver.transform)c=shadow_transform(*receiver.transform,c);
    frame.receivers.push_back({index,c,{}});
    return true;
}
bool add_shadow_entry(const ShadowSettings& settings,ShadowCasterRecord& record,const ShadowEntry& entry){
    const std::uint32_t bit=entry.type==shadow_entry_ground?shadow_mode_reflections:shadow_mode_shadows;
    require(entry.type<=shadow_entry_ground,"entry type other than 0, 1, 2");
    if((settings.mode&bit)==0||entry.strength<=0.0F)return false;
    const int cap=settings.entry_cap<record.limit?settings.entry_cap:record.limit;
    require(cap>=0&&static_cast<std::size_t>(cap)<=shadow_max_entries,"caster entry limit outside the 6 entries of a record");
    if(static_cast<int>(record.entries.size())<cap){record.entries.push_back(entry);return true;}
    for(ShadowEntry& e:record.entries)
        if(e.strength<entry.strength){e=entry;return true;}
    return false;
}
void gather_shadow_lights(const ShadowSettings& settings,ShadowCasterRecord& record,const ShadowVector& position,const ShadowLightSet& set){
    if(!set.any_lists)return;                                                             // 00372c80..00372c9c
    std::vector<Candidate> candidates;
    for(const ShadowLightTableEntry& e:set.table){                                        // 00372cd8..00372d14
        if((e.flags&0x16U)==0)continue;
        require(e.light<set.lights.size(),"light table entry without a light");
        float key=0.0F;
        if(e.flags&0x14U){                                                                // 003730f0
            const float d2=sum_squares(sub4(position,e.position));                       // 00183380, 00373290
            if(!(d2<mul(e.range,e.range)))continue;
            key=mul(d2,e.weight);
            if(e.flags==0x10U){
                const ShadowLight& l=set.lights[e.light];
                if(l.cone_inner!=1.0F){
                    const V n=normalized(sub4(e.position,position),"spot light at the caster position");
                    if(!(sub(1.0F,dot3(l.direction,n))<=sub(1.0F,l.cone_outer)))continue;
                }
            }
        }else if(e.flags!=2U)continue;
        candidates.push_back({e.light,key});
    }
    game_qsort(candidates,0,static_cast<Index>(candidates.size()));                       // 001273c8 / 00373ba0
    int remaining=settings.caster_cap;                                                    // manager +0x4
    for(std::size_t i=0;i<candidates.size()&&remaining!=0;++i){                           // 00372d50..003730b8
        const ShadowLight& l=set.lights[candidates[i].light];
        if(l.intensity<=0.0F)continue;
        if(l.type==2){
            add_shadow_entry(settings,record,{shadow_entry_directional,l.intensity,{},negate4(l.direction)});   // 003d54b0
        }else{
            float f=1.0F;
            if(l.type==0x10||l.type==4){
                const V v=sub4(l.position,position);
                const float len=length(v);
                V dir{};
                if(l.type==0x10){
                    require(len!=0.0F,"spot light at the caster position");
                    dir={dvd(v[0],len),dvd(v[1],len),dvd(v[2],len),0.0F};
                }
                const float g=max_s(0.0F,sub(len,l.inner));                               // 001f1350
                const float span=sub(l.outer,l.inner);
                f=g<span?sub(1.0F,dvd(g,span)):0.0F;
                if(l.type==0x10&&l.cone_inner!=1.0F){                                       // 00372fc0..00373050
                    const float c=dot3(l.direction,dir);
                    if(c<=l.cone_outer)f=0.0F;
                    else if(c<l.cone_inner){
                        const float a=sub(sub(1.0F,c),sub(1.0F,l.cone_inner));
                        const float b=sub(sub(1.0F,l.cone_outer),sub(1.0F,l.cone_inner));
                        f=mul(f,sub(1.0F,dvd(a,b)));
                    }
                }
            }
            f=min_s(max_s(f,0.0F),1.0F);                                                   // 001f1350, 0024ffd0
            add_shadow_entry(settings,record,{shadow_entry_point,mul(f,l.intensity),l.position,{}});   // 003d5600
        }
        --remaining;
    }
}
void assign_shadow_receivers(ShadowFrame& frame,const ShadowSettings& settings,std::size_t slot_index,const ShadowVector& caster_center,
                             const std::vector<ShadowReceiverInput>& receivers){            // 003d3d50
    require(slot_index<frame.slots.size(),"slot index out of range");
    const ShadowSlot& slot=frame.slots[slot_index];
    const float radius=static_cast<float>(settings.receiver_radius);                     // cvt.s.w of manager +0xc
    for(ShadowReceiverRecord& r:frame.receivers){
        require(r.receiver<receivers.size(),"receiver record without an input");
        const ShadowReceiverInput& in=receivers[r.receiver];
        if(!in.model_loaded)continue;
        const bool shadow=slot.type==shadow_slot_shadow&&(in.model_flags&0x20U);
        const bool reflection=slot.type==shadow_slot_reflection&&(in.model_flags&0x40U);
        if(!shadow&&!reflection)continue;
        if(!(length(sub4(caster_center,r.center))<radius))continue;
        V lo=in.bounds_min,hi=in.bounds_max;
        if(in.transform){lo=shadow_transform(*in.transform,lo);hi=shadow_transform(*in.transform,hi);}
        if(box_outside(slot.clip,lo,hi))continue;
        if(r.slots.size()<shadow_receiver_slot_limit)r.slots.push_back(slot_index);
    }
}
void build_shadow_slots(ShadowFrame& frame,const ShadowSettings& settings,std::size_t record_index,const std::vector<ShadowCasterInput>& casters,
                        const std::vector<ShadowReceiverInput>& receivers,const ShadowMatrix& camera_view){   // 003d4810
    require(record_index<frame.casters.size(),"caster record index out of range");
    if(frame.casters[record_index].entries.empty())return;
    require(frame.casters[record_index].caster<casters.size(),"caster record without an input");
    const ShadowCasterInput& in=casters[frame.casters[record_index].caster];
    const V local=scale4(add4(in.bounds_min,in.bounds_max),0.5F);
    const V center=shadow_transform(in.transform,local);
    const float r=length(sub4(in.bounds_min,local));
    const ShadowMatrix axis=shadow_axis_matrix();
    for(std::size_t k=0;k<frame.casters[record_index].entries.size();++k){
        ShadowEntry& e=frame.casters[record_index].entries[k];
        std::uint8_t type=0;
        float d=0,theta=0,near_plane=0,far_plane=0,guard=0,intensity=0;
        V camera{};
        if(e.type==shadow_entry_point){                                                   // 003d4a3c..003d4b38
            const V v=sub4(center,e.position);
            d=length(v);
            if(d==0.0F){
                // VDIV by zero gives the largest float; only an exactly zero difference
                // keeps a defined (zero) direction. The slot is then skipped as d < r.
                require(zero3(v),"point light direction below the representable length");
                require(0.0F<r,"point light at the centre of a zero-size caster");
                e.direction=v;
                continue;
            }
            const float q=ftz(vu::div(1.0F,d));
            e.direction={mul(v[0],q),mul(v[1],q),mul(v[2],q),v[3]};
            theta=game_atanf(dvd(add(2.0F,r),d));
            if(!(theta<fb(0x3edf66f3U)))theta=fb(0x3edf66f3U);                            // 25 degrees
            type=shadow_slot_shadow;guard=1.5F;
            near_plane=sub(d,r);far_plane=add(25.0F,add(d,r));
        }else if(e.type==shadow_entry_directional){                                       // 003d4b40..003d4c04
            d=10.0F;
            e.position=sub4(center,scale4(e.direction,10.0F));
            theta=game_atanf(dvd(add(2.0F,r),10.0F));
            type=shadow_slot_shadow;guard=1.5F;
            near_plane=sub(10.0F,r);far_plane=add(25.0F,add(10.0F,r));
        }else{                                                                            // 003d4c08..003d4d04
            require(e.type==shadow_entry_ground,"entry type other than 0, 1, 2");
            camera=shadow_rigid_inverse(camera_view).column[3];                          // 00133300, 00133200
            d=length(sub4(camera,e.position));
            if(d==0.0F){
                require(0.0F<r,"camera at the ground point of a zero-size caster");
                continue;
            }
            theta=game_atanf(dvd(r,d));
            type=shadow_slot_reflection;guard=5.5F;intensity=e.strength;
            near_plane=sub(sub(d,mul(3.0F,r)),6.0F);far_plane=add(d,r);
        }
        if(d<r)continue;                                                                  // 003d4d08
        theta=mul(theta,fb(0x3f666666U));
        near_plane=max_s(near_plane,1.0F);
        ShadowSlot s;
        s.caster=frame.casters[record_index].caster;
        s.half_angle=theta;s.strength=e.strength;s.type=type;
        if(type==shadow_slot_reflection){                                                 // 003d4d60..003d4f6c
            const V v=sub4(e.position,camera);
            const float len=length(v);
            const float q=ftz(vu::div(1.0F,len));
            const V n={mul(v[0],q),mul(v[1],q),mul(v[2],q),v[3]};
            const float twice=mul(2.0F,dot3(n,e.direction));
            e.position=add4(e.position,scale4(sub4(scale4(e.direction,twice),n),len));
            e.direction=normalized(sub4(center,e.position),"reflection projector at the caster centre");
        }
        s.view=shadow_look_at(e.position,e.direction);
        ShadowPerspective p=shadow_perspective({theta,0.0F,1.0F,0.0F,1.0F,near_plane,far_plane,1.0F,1.0F,intensity});
        const float g=dvd(1.0F,guard);
        p.matrix.column[0][0]=mul(p.matrix.column[0][0],g);
        p.matrix.column[1][1]=mul(p.matrix.column[1][1],g);
        p.scale[0]=mul(p.scale[0],guard);
        p.scale[1]=mul(p.scale[1],guard);
        s.clip=shadow_multiply(shadow_multiply(p.matrix,axis),s.view);
        s.scale=p.scale;s.offset=p.offset;
        s.facing=type==shadow_slot_shadow?e.direction:negate4(e.direction);
        require(frame.slots.size()<shadow_max_slots,"more than 24 slots");
        frame.slots.push_back(s);
        assign_shadow_receivers(frame,settings,frame.slots.size()-1,center,receivers);
    }
}
ShadowFrame build_shadow_frame(const ShadowFrameInput& input){
    const ShadowSettings& st=input.settings;
    validate(st);
    require_finite(input.camera_view,"camera view not finite");
    for(const ShadowCasterInput& c:input.casters){
        require_finite(c.transform,"caster transform not finite");
        require_finite(c.bounds_min,"caster bounds not finite");require_finite(c.bounds_max,"caster bounds not finite");
        if(c.ground_plane)require_finite(*c.ground_plane,"ground plane not finite");
    }
    for(const ShadowReceiverInput& r:input.receivers){
        if(r.transform)require_finite(*r.transform,"receiver transform not finite");
        require_finite(r.bounds_min,"receiver bounds not finite");require_finite(r.bounds_max,"receiver bounds not finite");
    }
    for(const ShadowLight& l:input.lights.lights){
        require(::omega::float_bits::finite(l.intensity)&&::omega::float_bits::finite(l.inner)&&::omega::float_bits::finite(l.outer)&&::omega::float_bits::finite(l.weight)&&
                ::omega::float_bits::finite(l.cone_outer)&&::omega::float_bits::finite(l.cone_inner),"light parameter not finite");
        require_finite(l.position,"light position not finite");require_finite(l.direction,"light direction not finite");
    }
    for(const ShadowLightTableEntry& e:input.lights.table){
        require(::omega::float_bits::finite(e.range)&&::omega::float_bits::finite(e.weight),"light table value not finite");
        require_finite(e.position,"light table position not finite");
    }
    require(input.caster_cursor<shadow_max_casters,"caster cursor outside the 4 caster records");
    ShadowFrame f;
    f.caster_cursor=input.caster_cursor;
    for(const ShadowObjectRef& o:input.order){                                            // 0013f1a0
        if(o.caster){
            require(o.index<input.casters.size(),"caster index out of range");
            register_shadow_caster(f,st,input.casters[o.index],o.index);
        }else{
            require(o.index<input.receivers.size(),"receiver index out of range");
            register_shadow_receiver(f,st,input.receivers[o.index],o.index);
        }
    }
    // 003d37e0
    if((st.mode&(shadow_mode_shadows|shadow_mode_reflections))==0)return f;
    bool shadow_receivers=false,reflection_receivers=false;
    for(const ShadowReceiverRecord& r:f.receivers){
        const ShadowReceiverInput& in=input.receivers[r.receiver];
        if(!in.model_loaded)continue;
        if(in.model_flags&0x20U)shadow_receivers=true;
        if(in.model_flags&0x40U)reflection_receivers=true;
    }
    std::size_t cursor=input.caster_cursor;
    for(std::size_t i=0;i<f.casters.size();++i){
        const ShadowCasterInput& in=input.casters[f.casters[i].caster];
        const V position=in.transform.column[3];                                           // transform object +0x40
        if(shadow_receivers&&(st.mode&shadow_mode_shadows)){
            cursor=i;                                                                      // 003d3900
            gather_shadow_lights(st,f.casters[i],position,input.lights);
        }
        if(reflection_receivers&&(st.mode&shadow_mode_reflections)&&in.ground_plane){      // vcall +0xf8
            const V& plane=*in.ground_plane;
            float z=0.0F;
            if(plane[2]!=0.0F){                                                            // 003d39d4..003d3a3c
                const V p=V{mul(plane[0],position[0]),mul(plane[1],position[1]),mul(plane[2],0.0F),mul(plane[3],1.0F)};
                z=dvd(ftz(-add(add(add(p[0],p[1]),p[2]),p[3])),plane[2]);
            }
            // 003d5370 writes to record [cursor]; a cursor past this frame's records names a
            // stale record that no slot build reads and the next registration resets.
            if(cursor<f.casters.size())
                add_shadow_entry(st,f.casters[cursor],{shadow_entry_ground,st.reflection_strength,{position[0],position[1],z,1.0F},{0.0F,0.0F,1.0F,0.0F}});
        }
    }
    f.caster_cursor=cursor;
    for(std::size_t i=0;i<f.casters.size();++i)build_shadow_slots(f,st,i,input.casters,input.receivers,input.camera_view);
    return f;
}
int find_or_register_shadow_receiver(ShadowFrame& frame,const ShadowReceiverInput& receiver,std::size_t index){   // 003d40d0, a2 = 1
    if(frame.receivers.size()>=shadow_max_receivers||receiver.hidden||!receiver.model_loaded)return -1;
    for(std::size_t i=0;i<frame.receivers.size();++i)
        if(frame.receivers[i].receiver==index)return static_cast<int>(i);
    V c=scale4(add4(receiver.bounds_min,receiver.bounds_max),0.5F);
    if(receiver.transform)c=shadow_transform(*receiver.transform,c);
    frame.receivers.push_back({index,c,{}});
    return static_cast<int>(frame.receivers.size()-1);
}
bool add_shadow_projector(ShadowFrame& frame,std::size_t cap,const ShadowReceiverInput& receiver,std::size_t receiver_index,
                          const ShadowProjectorInput& in,const std::function<float(const ShadowVector&)>& fog){   // 003d4350
    if(!(frame.projectors<cap))return false;
    const int found=find_or_register_shadow_receiver(frame,receiver,receiver_index);
    if(found<0)return false;
    ShadowReceiverRecord& record=frame.receivers[static_cast<std::size_t>(found)];
    if(!(record.slots.size()<shadow_receiver_slot_limit))return false;
    if(!(frame.slots.size()<shadow_max_slots))return false;
    record.slots.push_back(frame.slots.size());
    const float two_r=mul(2.0F,in.size);
    const float back=add(in.size,two_r);                                                 // 003d4408
    const float theta=game_atanf(dvd(in.size,back));                                      // 001212e0
    const float far_plane=add(back,two_r);
    ShadowSlot s;
    s.type=shadow_slot_projector;
    s.half_angle=theta;                                                                   // no 0.9 factor here
    s.strength=mul(in.strength,mul(fog(in.position),fb(0x3b808081U)));                   // 003d3700, 003d445c..003d4480
    s.texture=static_cast<std::int16_t>(static_cast<std::uint16_t>(static_cast<std::uint32_t>(in.texture)&0xffffU));
    const V eye=add4(in.position,scale4(in.normal,back));                                 // vmulx.xyzw, vadd.xyzw
    s.view=shadow_look_at(eye,negate4(in.normal));                                        // 003d2b20
    ShadowPerspective p=shadow_perspective({theta,0.0F,1.0F,0.0F,1.0F,in.size,far_plane,1.0F,1.0F,0.0F});
    const float g=fb(0x3e924925U);                                                        // 1 / 3.5
    p.matrix.column[0][0]=mul(p.matrix.column[0][0],g);
    p.matrix.column[1][1]=mul(p.matrix.column[1][1],g);
    p.scale[0]=mul(p.scale[0],3.5F);
    p.scale[1]=mul(p.scale[1],3.5F);
    s.clip=shadow_multiply(shadow_multiply(p.matrix,shadow_axis_matrix()),s.view);
    s.scale=p.scale;s.offset=p.offset;
    s.facing=negate4(in.normal);
    frame.slots.push_back(s);
    ++frame.projectors;
    return true;
}
std::vector<std::size_t> shadow_light_order(const std::vector<float>& keys){
    std::vector<std::size_t> order;
    shadow_light_order(keys.data(),keys.size(),order);
    return order;
}
void shadow_light_order(const float* keys,std::size_t count,std::vector<std::size_t>& order){
    thread_local std::vector<Candidate> c;   // port-only (132vs1): kept between calls (no allocation once grown)
    c.clear();
    for(std::size_t i=0;i<count;++i)c.push_back({i,keys[i]});
    game_qsort(c,0,static_cast<Index>(c.size()));
    order.clear();
    for(const Candidate& x:c)order.push_back(x.light);
}
std::vector<ShadowBatch> shadow_receiver_batches(const ShadowReceiverRecord& receiver){
    std::vector<ShadowBatch> b;
    for(std::size_t first=0;first<receiver.slots.size();first+=shadow_batch_size)
        b.push_back({first,std::min(shadow_batch_size,receiver.slots.size()-first)});
    return b;
}
}
