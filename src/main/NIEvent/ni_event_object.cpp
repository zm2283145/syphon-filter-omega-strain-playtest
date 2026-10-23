#include "ni_event_object.hpp"
#include "game_libm.hpp"
#include "hotbox_world.hpp"
#include <bit>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace omega {
namespace {
[[noreturn]] void fail(const std::string& what){throw std::runtime_error("NI event object: "+what);}
float check(float v){
    if(!std::isfinite(v))fail("nonfinite arithmetic");
    return std::fpclassify(v)==FP_SUBNORMAL?(std::signbit(v)?-0.0F:0.0F):v;
}
float mul(float a,float b){return check(a*b);}
float add(float a,float b){return check(a+b);}
float sub(float a,float b){return check(a-b);}
float fdiv(float a,float b){
    if(b==0.0F)fail("division by zero");
    return check(a/b);
}
// acc + s * t / acc - s * t: the product is rounded, the sum written back.
float madd(float acc,float s,float t){return check(acc+s*t);}
float msub(float acc,float s,float t){return check(acc-s*t);}
constexpr float degree=std::bit_cast<float>(0x3c8efa35U);   // pi / 180
// cvt.w.s: truncation, saturating outside the int range; NaN throws.
std::int32_t cvt_w(float v){
    if(std::isnan(v))fail("cvt.w.s of NaN");
    if(v>=2147483648.0F)return std::numeric_limits<std::int32_t>::max();
    if(v<-2147483648.0F)return std::numeric_limits<std::int32_t>::min();
    return static_cast<std::int32_t>(v);
}
// The key search the three track steps share: the first i < count - 1 with
// !(time < t_i) && time < t_(i+1) (c.olt.s).
template<class K> std::optional<std::size_t> segment(const std::vector<K>& keys,float time){
    for(std::size_t i=0;i+1<keys.size();++i)
        if(!(time<keys[i].t)&&time<keys[i+1].t)return i;
    return std::nullopt;
}
template<class K> std::size_t loop_key(const std::vector<K>& keys,std::int32_t loop,const char* where){
    if(loop<0||static_cast<std::size_t>(loop)+1>=keys.size())fail(std::string(where)+": the loop key is outside the track");
    return static_cast<std::size_t>(loop);
}
}

bool ni_position_track_step(NiPositionTrack& tr,float dt){
    const auto& k=tr.keys;
    if(k.empty())fail("0023d290 steps a position track without keys (reads key -1)");
    if(k.size()==1)return false;
    tr.time=add(tr.time,dt);
    std::size_t s{};
    float dur{},u{};
    if(const auto i=segment(k,tr.time)){
        s=*i;
        dur=sub(k[s+1].t,k[s].t);
        u=fdiv(sub(tr.time,k[s].t),dur);
    }else{
        if(tr.loop==-1){
            tr.time=k.back().t;
            tr.out=k.back().pos;                                         // lq of the key's +10 (w 1.0 from 002386a0)
            return true;
        }
        s=loop_key(k,tr.loop,"0023d290");
        tr.time=k[s].t;
        dur=sub(k[s+1].t,k[s].t);
        u=fdiv(tr.time,dur);
    }
    const float f7=mul(u,u),f6=mul(f7,u);
    const float h00=add(1.0F,sub(mul(2.0F,f6),mul(3.0F,f7)));
    const float h10=add(u,sub(f6,mul(2.0F,f7)));
    const float h01=add(mul(-2.0F,f6),mul(3.0F,f7));
    const float h11=sub(f6,f7);
    for(std::size_t j=0;j<3;++j){
        const float t1=mul(sub(0.0F,k[s+1].in[j]),dur);          // vmulax ACC 0, vmsubw; vmulx f21
        const float t0=mul(k[s].out[j],dur);
        float r=add(mul(k[s].pos[j],h00),mul(k[s+1].pos[j],h01)); // 0017dc00 / 00134080
        r=add(r,mul(t0,h10));
        tr.out[j]=add(r,mul(t1,h11));
    }
    tr.out[3]=1.0F;
    return true;
}

bool ni_switch_track_step(NiSwitchTrack& tr,float dt){
    const auto& k=tr.keys;
    if(k.empty())fail("0023d6d0 steps a switch track without keys (reads key -1)");
    std::size_t a3=k.size()-1;
    if(a3==0)return false;
    tr.time=add(tr.time,dt);
    if(const auto i=segment(k,tr.time))a3=*i;
    else{
        if(tr.loop==-1){
            tr.time=k.back().t;
            tr.on=k.back().on;
            return true;
        }
        const std::size_t l=loop_key(k,tr.loop,"0023d6d0");
        tr.time=add(sub(tr.time,k[a3].t),k[l].t);
        a3=l;
    }
    const bool old=tr.on;
    tr.on=k[a3].on;
    return old!=tr.on;
}

bool ni_rotation_track_step(NiRotationTrack& tr,float dt){
    const auto& k=tr.keys;
    if(!tr.active)return false;
    if(k.empty())fail("00239a90 steps an active rotation track without keys (reads key -1)");
    if(k.size()==1)return false;
    tr.time=add(tr.time,dt);
    std::size_t s{};
    float f4{},u{};
    if(const auto i=segment(k,tr.time)){
        s=*i;
        f4=sub(k[s+1].t,k[s].t);
        u=fdiv(sub(tr.time,k[s].t),f4);
    }else{
        if(tr.loop==-1){
            tr.time=k.back().t;
            tr.value=k.back().v;
            tr.active=false;
            return true;
        }
        s=loop_key(k,tr.loop,"00239a90");
        tr.time=k[s].t;
        f4=sub(k[s+1].t,k[s].t);
        u=fdiv(tr.time,f4);
    }
    const float f3=mul(f4,-k[s+1].in);
    const float f4o=mul(f4,k[s].out);
    const float f11=mul(u,u),f10=mul(f11,u),f9=mul(3.0F,f11);
    const float h00=add(1.0F,sub(mul(2.0F,f10),f9));
    const float h01=add(mul(-2.0F,f10),f9);
    const float a=madd(mul(h00,k[s].v),h01,k[s+1].v);
    const float h10=add(u,sub(f10,mul(2.0F,f11)));
    tr.value=madd(madd(a,h10,f4o),sub(f10,f11),f3);
    return true;
}

void ni_end_state_tracks(NiPositionTrack& position,NiSwitchTrack& sw,std::array<NiRotationTrack,3>& rotation){
    if(position.keys.empty())fail("0023b4c0 reads position key -1 (+b0 = 0)");
    ni_position_track_step(position,sub(position.keys.back().t,position.time));        // 0023b4e8..0023b510
    if(sw.keys.empty())fail("0023b4c0 reads switch key -1 (+c8 = 0)");
    ni_switch_track_step(sw,sub(sw.keys.back().t,sw.time));                            // 0023b514..0023b540
    for(auto& r:rotation){                                                             // 0023b544..0023b604
        r.active=true;
        if(r.loop!=-1)continue;
        if(r.keys.empty())fail("0023b4c0 reads rotation key -1");
        ni_rotation_track_step(r,sub(r.keys.back().t,r.time));
    }
}

std::vector<std::size_t> ni_event_list_step(NiEventList& list,float dt){
    const float next=add(list.time,dt);
    std::vector<std::size_t> fired;
    if(!list.active)return fired;
    for(std::size_t i=0;i<list.times.size();++i){
        const float t=list.times[i];
        if(t<=next&&!(t<=list.time))fired.push_back(i);
    }
    list.time=next;
    return fired;
}

NiMatrix ni_axis_angle(float x,float y,float z,float angle){
    const float s=game_sinf(angle);
    const float c=game_cosf(angle);
    const float f3=sub(1.0F,c);
    const float f9=mul(y,mul(f3,x));
    const float f8=mul(z,mul(f3,y));
    const float f7=mul(x,mul(f3,z));
    NiMatrix m{};
    m[0][0]=add(c,mul(f3,mul(x,x)));
    m[1][1]=add(c,mul(f3,mul(y,y)));
    m[2][2]=add(c,mul(f3,mul(z,z)));
    const float sz=mul(s,z),sy=mul(s,y),sx=mul(s,x);
    m[0][1]=add(f9,sz);
    m[1][0]=sub(f9,sz);
    m[0][2]=sub(f7,sy);
    m[2][0]=add(f7,sy);
    m[1][2]=add(f8,sx);
    m[2][1]=sub(f8,sx);
    m[3]={0.0F,0.0F,0.0F,1.0F};
    return m;
}

void ni_rotate_rows(NiMatrix& a,const NiMatrix& b){
    for(auto& r:a){
        RootVector o=r;
        for(std::size_t j=0;j<3;++j)o[j]=madd(madd(mul(b[0][j],r[0]),b[1][j],r[1]),b[2][j],r[2]);
        r=o;
    }
}

NiMatrix ni_matrix_product(const NiMatrix& a,const NiMatrix& b){
    NiMatrix out{};
    for(std::size_t i=0;i<4;++i)
        for(std::size_t j=0;j<4;++j)
            out[i][j]=madd(madd(madd(mul(a[0][j],b[i][0]),a[1][j],b[i][1]),a[2][j],b[i][2]),a[3][j],b[i][3]);
    return out;
}

RootVector ni_point_product(const NiMatrix& w,const RootVector& p){
    RootVector out{};
    for(std::size_t j=0;j<4;++j)out[j]=madd(madd(madd(mul(w[0][j],p[0]),w[1][j],p[1]),w[2][j],p[2]),w[3][j],p[3]);
    return out;
}

NiBeam ni_beam_segment(const std::array<RootVector,2>& bounds,const NiMatrix& world){
    const auto& lo=bounds[0];
    const auto& hi=bounds[1];
    RootVector mid{0.0F,0.0F,0.0F,1.0F};                                     // sp+90 (+9c = 1.0)
    float extent[3]{};
    for(std::size_t i=0;i<3;++i){
        extent[i]=std::fabs(sub(hi[i],lo[i]));                               // sub.s, abs.s
        mid[i]=add(lo[i],fdiv(sub(hi[i],lo[i]),2.0F));                       // sub.s, div.s 2.0, add.s
    }
    RootVector start=mid,end=mid;                                            // sp+90, sp+80
    std::size_t axis=2;
    if(!(extent[0]<extent[1])&&!(extent[0]<extent[2]))axis=0;                // c.olt.s
    else if(!(extent[1]<extent[0])&&!(extent[1]<extent[2]))axis=1;
    start[axis]=lo[axis];
    end[axis]=hi[axis];
    return {ni_point_product(world,start),ni_point_product(world,end)};      // 0023a1f0, 0023a21c
}

namespace {
// A vsub's MAC flags where they are read: zero / sign per lane, a denormal or
// -0 difference (flags not established) throwing.
float flagged(float a,float b,const char* where){
    if(!std::isfinite(a)||!std::isfinite(b))fail(std::string(where)+": nonfinite beam operand");
    const float d=a-b;
    if(std::fpclassify(d)==FP_SUBNORMAL)fail(std::string(where)+": denormal difference (VU zero flag not established)");
    if(d==0.0F&&std::signbit(d))fail(std::string(where)+": -0 difference (VU sign flag not established)");
    return d;
}
// MAC sign bits x 0x80, y 0x40, z 0x20 of a - b (vsub.xyz, cfc2 vi17 & 0xe0).
unsigned sign_bits(const RootVector& a,const RootVector& b,const char* where){
    unsigned s=0;
    for(std::size_t i=0;i<3;++i)
        if(flagged(a[i],b[i],where)<0.0F)s|=0x80U>>i;
    return s;
}
}

bool ni_beam_box_hit(const std::array<RootVector,2>& box,const RootVector& p0,const RootVector& p1){
    const auto& lo=box[0];
    const auto& hi=box[1];
    unsigned c0=sign_bits(p0,lo,"0023a930");                                 // vf6 = p0 - min
    unsigned c1=sign_bits(p1,lo,"0023a950");                                 // vf8 = p1 - min
    if(c0&c1)return false;
    c0=(c0>>4)|sign_bits(hi,p0,"0023a980");                                  // vf7 = max - p0
    c1=(c1>>4)|sign_bits(hi,p1,"0023a9a8");                                  // vf9 = max - p1
    if(c0&c1)return false;
    if((c0|c1)==0)return true;
    const unsigned straddle=c0^c1;                                           // 0023aa08
    RootVector d{};
    bool zero[3]{};
    for(std::size_t i=0;i<3;++i){                                            // vsub.xyzw vf5 = p1 - p0
        d[i]=flagged(p1[i],p0[i],"0023aa0c");
        zero[i]=d[i]==0.0F;
    }
    RootVector to_lo{},to_hi{};
    for(std::size_t i=0;i<3;++i){
        to_lo[i]=-sub(p0[i],lo[i]);                                          // vsub.xyz vf6 = 0 - (p0 - min)
        to_hi[i]=sub(hi[i],p0[i]);                                           // vf7 = max - p0
    }
    // The crossing p0 + (d * plane.axis) * Q (vmulx; vmulaw p0 * 1, vmaddq)
    // inside min <= . <= max on the two other lanes (prot3w, c.ole.s).
    const auto inside=[&](std::size_t axis,const RootVector& plane,float q){
        for(std::size_t k=1;k<3;++k){
            const std::size_t j=(axis+k)%3;
            const float p=madd(mul(p0[j],1.0F),mul(d[j],plane[axis]),q);
            if(!(lo[j]<=p)||!(p<=hi[j]))return false;
        }
        return true;
    };
    for(std::size_t axis=0;axis<3;++axis){
        const unsigned near_bit=0x8U>>axis,far_bit=0x80U>>axis;              // x 8 / 0x80, y 4 / 0x40, z 2 / 0x20
        if(zero[axis]||!(straddle&(near_bit|far_bit)))continue;
        const float q=fdiv(1.0F,d[axis]);                                    // vdiv Q, vf0.w, vf5.axis
        if((straddle&near_bit)&&inside(axis,to_lo,q))return true;
        if((straddle&far_bit)&&inside(axis,to_hi,q))return true;
    }
    return false;
}

NiMatrix ni_billboard_world(const RootVector& camera,const RootVector& position){
    RootVector dir{};
    for(std::size_t j=0;j<4;++j)dir[j]=sub(camera[j],position[j]);            // vsub.xyzw
    const float s=madd(madd(mul(1.0F,mul(dir[0],dir[0])),1.0F,mul(dir[1],dir[1])),1.0F,mul(dir[2],dir[2]));
    if(s==0.0F)fail("0023e180: the camera at the billboard's position (vrsqrt of 0)");
    const float d=static_cast<float>(std::sqrt(std::fabs(static_cast<double>(s))));
    const float q=check(1.0F/d);                                             // vrsqrt Q = 1 / sqrt
    for(std::size_t j=0;j<3;++j)dir[j]=mul(dir[j],q);                        // vmulq.xyz (w kept)
    const auto cross=[](const RootVector& a,const RootVector& b){            // vopmula a, b; vopmsub b, a; w = 0
        return RootVector{msub(mul(a[1],b[2]),b[1],a[2]),msub(mul(a[2],b[0]),b[2],a[0]),msub(mul(a[0],b[1]),b[0],a[1]),0.0F};
    };
    const RootVector up{0.0F,0.0F,1.0F,0.0F};
    const RootVector x0=cross(dir,up);
    return {x0,dir,cross(x0,dir),position};                                  // 00131f00
}

NiMatrix ni_rotation_compose(float x,float y,float z,const RootVector& position){
    NiMatrix m=ni_axis_angle(1.0F,0.0F,0.0F,mul(degree,x));
    ni_rotate_rows(m,ni_axis_angle(0.0F,1.0F,0.0F,mul(degree,y)));
    ni_rotate_rows(m,ni_axis_angle(0.0F,0.0F,1.0F,mul(degree,z)));
    m[3]=position;
    return m;
}

bool ni_identity(const NiMatrix& m){
    for(std::size_t r=0;r<4;++r)
        for(std::size_t c=0;c<4;++c)
            if(!(m[r][c]==(r==c?1.0F:0.0F)))return false;                // c.eq.s: -0 equals 0
    return true;
}

namespace {
NiPositionTrack position_track(const PopulationTrack& t,const char* where){
    if(t.width!=10)fail(std::string(where)+": keyframes are not 10 floats");
    NiPositionTrack r;
    for(std::size_t i=0;i<t.keys();++i){
        const float* v=&t.values[i*10];
        r.keys.push_back({v[0],{v[1],v[2],v[3],1.0F},{v[4],v[5],v[6],0.0F},{v[7],v[8],v[9],0.0F}});
    }
    if(r.keys.empty())fail(std::string(where)+" reads key 0 of an empty keyframe track");
    r.loop=cvt_w(sub(t.length,1.0F));
    r.out=r.keys[0].pos;
    return r;
}
// An empty switch track becomes one key {0, on} with loop -1 (00238610).
NiSwitchTrack switch_track(const PopulationTrack& t){
    NiSwitchTrack r;
    if(t.keys()==0){
        r.keys.push_back({0.0F,true});
        r.loop=-1;
    }else{
        if(t.width!=2)fail("switch keys are not 2 floats");
        for(std::size_t i=0;i<t.keys();++i)r.keys.push_back({t.values[i*2],t.values[i*2+1]!=0.0F});
        r.loop=cvt_w(sub(t.length,1.0F));
    }
    r.on=r.keys[0].on;
    return r;
}
// 00239df0: loop = cvt.w.s(length - 1) when there are keys; value = key 0's v.
NiRotationTrack rotation_track(const PopulationTrack& t){
    NiRotationTrack r;
    if(t.keys()==0)return r;
    if(t.width!=4)fail("rotation keys are not 4 floats");
    for(std::size_t i=0;i<t.keys();++i){
        const float* v=&t.values[i*4];
        r.keys.push_back({v[0],v[1],v[2],v[3]});
    }
    r.loop=cvt_w(sub(t.length,1.0F));
    r.value=r.keys[0].v;
    return r;
}
std::uint32_t pair_id(const std::array<std::uint32_t,2>& p){
    if(p[0]>=0x100||p[1]>=0x10000)fail("a child reference outside 0040c480's category / index range");
    return (p[0]<<16)|p[1];
}
std::string hex(std::uint32_t v){
    char b[16];
    std::snprintf(b,sizeof b,"%x",v);
    return b;
}
}

NiEventWorld::NiEventWorld(const Population& pop,NiEventWorldHost host):host_(std::move(host)){
    const auto add_node=[&](Node&& n){
        if(index_.count(n.id))fail("duplicate object id "+hex(n.id));
        index_[n.id]=static_cast<int>(nodes_.size());
        nodes_.push_back(std::move(n));
    };
    for(const auto& l:pop.event_lights){                                 // 00237f80
        Node n;
        n.light=true;
        n.id=l.id;
        n.kind=7;
        n.position=position_track(l.keyframes,"002386a0");
        n.sw=switch_track(l.switches);
        n.rotation[0]=rotation_track(l.tracks[0]);
        n.rotation[1]=rotation_track(l.tracks[1]);
        n.colour={l.values_144[0],l.values_144[1],l.values_144[2]};
        n.byte_150=l.byte_150!=0;
        n.byte_151=l.byte_151!=0;
        n.local_rows={false,false,false,true};                           // vt +90 with stack rows 0..2
        n.local[3]=n.position.out;
        n.node=n.local;
        n.node_rows=n.local_rows;
        n.node_reason="00238600 leaves the NIEventLight's local rows 0..2 uninitialised";
        add_node(std::move(n));
        step(static_cast<int>(nodes_.size()-1),0.0F);                    // 00238740: vt +134(0)
    }
    const auto& names=pop.sound_names;
    for(const auto& e:pop.event_objects){                                // 0023c610
        Node n;
        n.id=e.id;
        n.kind=e.kind;
        if(n.kind>6)fail("0023c610: kind "+std::to_string(n.kind)+" outside the jump table 004a6fc0");
        n.asset=n.kind!=0&&n.kind!=4;
        n.position=position_track(e.keyframes,"0023c9e8");
        n.sw=switch_track(e.switches);
        for(std::size_t i=0;i<3;++i)n.rotation[i]=rotation_track(e.tracks[i]);
        for(const auto& ev:e.events){                                    // 00238a50
            Event x;
            x.type=ev.type;
            x.values=ev.values;
            const std::size_t need_values=ev.type==1?5:ev.type==2||ev.type==4?3:ev.type==0||ev.type==3?1:0;
            if(need_values==0)fail("00238a50: event type "+std::to_string(ev.type)+" has no time");
            if(x.values.size()!=need_values)fail("an event with the wrong value count");
            if(ev.type==2||ev.type==4){                                  // 002397f0 / 00239610 at load (the same code but the vtable)
                std::int32_t index=cvt_w(x.values[1]);
                if(static_cast<std::int32_t>((n.id>>16)&0xffU)!=names.first_category)index+=static_cast<std::int32_t>(names.base);
                if(index<0)fail("002397f0: a negative sound index");
                if(static_cast<std::size_t>(index)<names.names.size()){
                    x.sound=names.names[static_cast<std::size_t>(index)];
                    x.named=true;
                }
                x.param=cvt_w(x.values[2]);
            }
            n.list.times.push_back(x.values[0]);
            n.events.push_back(std::move(x));
        }
        n.local=ni_rotation_compose(n.rotation[0].value,n.rotation[1].value,n.rotation[2].value,n.position.out);
        n.local_rows={true,true,true,true};
        n.node=n.local;                                                  // vt +90
        n.node_rows=n.local_rows;
        if(n.asset&&(n.kind==1||n.kind==3||n.kind==6))                   // 0023d07c: asset vt +90 = 00394850
            n.asset_matrix=!ni_identity(n.local);                        // 003cdff0, else 003cdf20 (none)
        const int self=static_cast<int>(nodes_.size());
        std::vector<int> children;
        for(const auto& c:e.children){                                   // 0023ce4c: 0040c480
            const std::uint32_t cid=pair_id(c);
            const int ci=find(cid);
            if(ci<0)fail("0023ce4c: child "+hex(cid)+" of "+hex(n.id)+" is not a loaded NIE:/NIL: object");
            children.push_back(ci);
        }
        n.children=children;
        add_node(std::move(n));
        for(const int ci:children)nodes_[static_cast<std::size_t>(ci)].parent=self;
        if(e.byte_130){                                                  // Play (queued)
            queue_shuttle(self,0,false);
            ++autoplay_;
        }
        const std::function<void(int)> refresh=[&](int k){               // 0023b120
            node_update(k);
            for(const int c:nodes_[static_cast<std::size_t>(k)].children)
                if(!nodes_[static_cast<std::size_t>(c)].light)refresh(c);
        };
        refresh(self);
    }
}

int NiEventWorld::find(std::uint32_t id) const{
    const auto it=index_.find(id);
    return it==index_.end()?-1:it->second;
}
int NiEventWorld::need(std::uint32_t id,const char* what) const{
    const int n=find(id);
    if(n<0)fail(std::string(what)+": no NIE:/NIL: object "+hex(id));
    return n;
}

void NiEventWorld::queue_shuttle(int node,int mode,bool byte_20){queue_.push_back({node,false,mode,byte_20,0});}
void NiEventWorld::queue_notice(int node,std::int32_t value){queue_.push_back({node,true,0,false,value});}

void NiEventWorld::play(std::uint32_t id){queue_shuttle(need(id,"Play 00237960"),0,false);}
void NiEventWorld::stop(std::uint32_t id){queue_shuttle(need(id,"Stop 00237930"),1,false);}
void NiEventWorld::reset(std::uint32_t id){queue_shuttle(need(id,"00237900"),2,false);}
void NiEventWorld::end_state(std::uint32_t id){end_state_node(need(id,"SetEndState 00237870"));}
void NiEventWorld::end_state_node(int n){
    auto& o=nodes_[static_cast<std::size_t>(n)];
    if(o.light)fail("00237a20: the NIEventLight end state (virtual +128 of 004dbed0) is not ported");
    o.playing_130=true;                                                  // 0023b4e4
    ni_end_state_tracks(o.position,o.sw,o.rotation);
    if(o.kind!=3){                                                       // 0023b608..0023b754
        o.local=ni_rotation_compose(o.rotation[0].value,o.rotation[1].value,o.rotation[2].value,o.position.out);
        o.local_rows={true,true,true,true};
    }
    node_update(n);                                                      // 0023b758: vt +120
    const auto children=nodes_[static_cast<std::size_t>(n)].children;
    for(const int c:children)end_state_node(c);                          // 0023b770..0023b7a0: vt +128
}

std::size_t NiEventWorld::take_pending(){
    if(!delivering_.empty())fail("take_pending() before the previous snapshot was delivered");
    delivering_.swap(queue_);
    queue_.clear();
    return delivering_.size();
}

void NiEventWorld::deliver(){
    auto batch=std::move(delivering_);
    delivering_.clear();
    for(const auto& m:batch)receive(m);
}

void NiEventWorld::receive(const Message& m){
    auto& o=nodes_[static_cast<std::size_t>(m.node)];
    if(m.beam){                                                          // 0023b7d0 -> 0023e890 -> 003cea10
        const bool handled=host_.beam?host_.beam(o.id,m.actor):false;
        beams_.push_back({o.id,m.actor,m.flags,handled});
        return;
    }
    if(m.notice){
        const bool handled=host_.notice?host_.notice(o.id,m.value_24):false;
        notices_.push_back({o.id,m.value_24,handled});
        return;
    }
    if(o.light){                                                         // 00237eb0
        if(m.mode==0){
            o.rotation[0].active=true;                                   // +18c
            o.rotation[1].active=true;                                   // +1ac
            o.playing_130=true;                                          // 0023e890
        }else if(m.mode==1)o.playing_130=false;
        else if(m.mode==2){                                              // 00237f18, then 0023e890 -> 0023e900
            for(int k=0;k<2;++k){                                        // +188 / +170, +1a8 / +190
                auto& r=o.rotation[static_cast<std::size_t>(k)];
                r.time=0.0F;
                if(!r.keys.empty())r.value=r.keys[0].v;
            }
            if(o.position.keys.empty()||o.sw.keys.empty())
                fail("0023e900: the NIEventLight reset reads the first position / switch key of "+hex(o.id)+", which has none");
            o.position.time=0.0F;                                        // +c0
            o.sw.time=0.0F;                                              // +d8
            o.position.out=o.position.keys[0].pos;                       // +e0 = keys +10
            o.local_rows={false,false,false,true};                       // +f0: stack rows 0..2, row 3 +e0
            o.local[3]=o.position.out;
            o.sw.on=o.sw.keys[0].on;                                     // +dc = *(+cc) +4
            step(m.node,0.0F);                                           // vt +134(0) = 0023d1d0
            nodes_[static_cast<std::size_t>(m.node)].playing_130=false;
        }
        return;
    }
    if(m.mode==0){                                                       // 0023b7d0
        for(auto& r:o.rotation)r.active=true;                            // +160 / +180 / +1a0
        o.list.active=true;                                              // +1a8
        for(const int c:o.children)queue_shuttle(c,0,true);
        o.playing_130=true;                                              // 0023e890
    }else if(m.mode==1){
        for(const int c:o.children)queue_shuttle(c,1,true);
        o.playing_130=false;
    }else if(m.mode==2){                                                 // 0023b960
        for(auto& r:o.rotation){
            r.time=0.0F;
            if(!r.keys.empty())r.value=r.keys[0].v;
        }
        o.list.time=-0.0F;                                               // +1ac = 0x80000000
        o.list.active=false;
        o.position.time=0.0F;
        o.sw.time=0.0F;
        o.position.out=o.position.keys[0].pos;
        o.position.out[3]=1.0F;
        o.local=ni_rotation_compose(o.rotation[0].value,o.rotation[1].value,o.rotation[2].value,o.position.out);
        o.local_rows={true,true,true,true};
        if(o.kind==2&&o.asset){                                          // 0023bb40: +60 == 2, +140 set
            if(!host_.particle_reset)fail("0023bb58: the particle asset's reset (virtuals +60 / +5c) is not hosted");
            host_.particle_reset(o.id);
        }
        step(m.node,0.0F);                                               // vt +134(0)
        nodes_[static_cast<std::size_t>(m.node)].playing_130=false;
        for(const int c:nodes_[static_cast<std::size_t>(m.node)].children)queue_shuttle(c,2,true);
    }else fail("0023b7d0: SHUTTLE mode "+std::to_string(m.mode));
}

void NiEventWorld::step(int n,float dt){
    auto& o=nodes_[static_cast<std::size_t>(n)];
    if(o.light){                                                         // 0023d1d0
        const bool p=ni_position_track_step(o.position,dt);
        const bool s=ni_switch_track_step(o.sw,dt);
        if((p||s)&&o.kind!=3){
            o.local_rows={false,false,false,true};
            o.local[3]=o.position.out;
            node_update(n);
        }
        return;
    }
    const bool s2=ni_position_track_step(o.position,dt);                // 0023b1c0: vt +12c
    bool s1=false;
    if(o.kind==3)s1=true;
    else for(auto& r:o.rotation)s1=ni_rotation_track_step(r,dt)||s1;
    const bool s0=ni_switch_track_step(o.sw,dt);                         // vt +130
    for(const auto i:ni_event_list_step(o.list,dt))fire(n,i);            // 00238990 -> event vt +8
    auto& p=nodes_[static_cast<std::size_t>(n)];
    if(s2||s1||s0){
        if(p.kind!=3){
            p.local=ni_rotation_compose(p.rotation[0].value,p.rotation[1].value,p.rotation[2].value,p.position.out);
            p.local_rows={true,true,true,true};
        }
        node_update(n);                                                  // vt +120
    }
}

void NiEventWorld::fire(int n,std::size_t i){
    const auto& o=nodes_[static_cast<std::size_t>(n)];
    const auto& e=o.events[i];
    switch(e.type){
    case 0:                                                              // Stop(owner, 0), NOTICE +24 = 1
        queue_shuttle(n,1,true);
        queue_notice(n,1);
        return;
    case 3:
        queue_notice(n,0);
        return;
    case 1:{                                                             // 00239960 -> 0021a200
        if(!host_.blast)fail("00239960: the blast 0021a200 is not hosted");
        NiBlastRecord b{o.id,cvt_w(e.values[1]),e.values[2],e.values[3],e.values[4],position(o.id)};
        blasts_.push_back(b);
        host_.blast(b);
        return;
    }
    case 2:{                                                             // 00239780 -> 0020f990, 003c9420(msg, 004ffb50, 1)
        if(!e.named)fail("002397f0/001ca010: a sound index outside the name table copies the name from address 0 (not hosted)");
        if(!host_.sound)fail("00239780: the sound manager 004ffb50 is not hosted");
        NiSoundRecord s{o.id,e.sound,e.named,e.param};
        sounds_.push_back(s);
        host_.sound(s);
        return;
    }
    case 4:{                                                             // 002395a0 -> 0020eba0, 003c9420(msg, 004ffb50, 1)
        if(!e.named)fail("00239610/001ca010: a sound index outside the name table copies the name from address 0 (not hosted)");
        if(!host_.sound)fail("002395a0: the sound manager 004ffb50 is not hosted");
        NiSoundRecord s{o.id,e.sound,e.named,e.param,true};
        sounds_.push_back(s);
        host_.sound(s);
        return;
    }
    default:
        fail("event type "+std::to_string(e.type));
    }
}

namespace { const char* const absent_kind3_world="0023de10 leaves an absent kind-3 object's world uninitialised"; }

void NiEventWorld::world(int n,NiMatrix& out,std::array<bool,4>& rows,std::string& reason) const{
    const auto& o=nodes_[static_cast<std::size_t>(n)];
    if(o.kind==3){                                                       // 0023de10
        rows={false,false,false,false};
        if(!o.present_2d){reason=absent_kind3_world;return;}
        if(!host_.camera_position){reason="0023e180: the camera object of 004ffc14 is not hosted";return;}
        const auto camera=host_.camera_position();                       // 00159570 / 00159560: +50 transform +40
        if(!camera){reason="0023e180 writes no world without a camera object (00159570)";return;}
        RootVector p=o.position.out;                                     // +e0
        if(o.parent>=0){                                                 // +94: 0023de10(parent) x +e0
            NiMatrix pw{};
            std::array<bool,4> pr{};
            std::string why;
            world(o.parent,pw,pr,why);
            if(!(pr[0]&&pr[1]&&pr[2]&&pr[3])){reason=why;return;}
            p=ni_point_product(pw,p);
        }
        out=ni_billboard_world(*camera,p);
        rows={true,true,true,true};
        reason.clear();
        return;
    }
    if(o.parent>=0&&!nodes_[static_cast<std::size_t>(o.parent)].light&&nodes_[static_cast<std::size_t>(o.parent)].kind==3){
        rows={false,false,false,false};                                  // 0023ded4: 001830d0 then 0023e180
        reason="0023ded4: the world under a kind-3 parent (001830d0) is not native";
        return;
    }
    if(o.parent<0){
        out=o.local;
        rows=o.local_rows;
        reason=o.light?"00238600 / 0023d1d0 leave the NIEventLight's local rows 0..2 uninitialised":"";
        return;
    }
    NiMatrix pw{};
    std::array<bool,4> pr{};
    std::string why;
    world(o.parent,pw,pr,why);
    const bool parent_valid=pr[0]&&pr[1]&&pr[2]&&pr[3];
    out={};
    for(std::size_t i=0;i<4;++i){
        rows[i]=parent_valid&&o.local_rows[i];
        if(!rows[i])continue;
        const NiMatrix one={pw[0],pw[1],pw[2],pw[3]};
        NiMatrix b{};
        b[i]=o.local[i];
        out[i]=ni_matrix_product(one,b)[i];
    }
    reason=!parent_valid?why:"00238600 / 0023d1d0 leave the NIEventLight's local rows 0..2 uninitialised";
}

void NiEventWorld::node_update(int n){
    auto& o=nodes_[static_cast<std::size_t>(n)];
    NiMatrix w{};
    std::array<bool,4> rows{};
    std::string reason;
    world(n,w,rows,reason);
    o.node=w;                                                            // vt +94 / +ac
    o.node_rows=rows;
    o.node_reason=reason;
    if(rows[0]&&rows[1]&&rows[2]&&rows[3])o.node_written=w;
}

void NiEventWorld::update(float dt){
    for(std::size_t n=0;n<nodes_.size();++n)
        if(nodes_[n].parent<0)update_node(static_cast<int>(n),dt);       // 0023a560 / 00237b50
}

void NiEventWorld::update_node(int n,float dt){
    auto* o=&nodes_[static_cast<std::size_t>(n)];
    if(!o->light&&o->kind==2&&o->asset&&o->sw.on&&host_.particle_update){
        const bool valid=o->node_rows[0]&&o->node_rows[1]&&o->node_rows[2]&&o->node_rows[3];
        const NiMatrix node=o->node;
        host_.particle_update(o->id,dt,valid?&node:nullptr);             // 0023a5cc: asset vt +48(dt), node+10
    }
    o=&nodes_[static_cast<std::size_t>(n)];
    const bool was=o->playing_130;
    if(was||(o->kind==3&&o->sw.on&&o->present_2d&&o->enabled_2c)){      // 0023a604..0023a64c: +dc, +2d, +2c, !+2e
        step(n,was?mul(dt,o->speed_134):0.0F);
        o=&nodes_[static_cast<std::size_t>(n)];
        if(was&&!o->playing_130)queue_notice(n,0);                       // NOTICE +24 = 0
    }
    if(o->light){                                                        // 00237b90
        if(o->playing_130){
            ni_rotation_track_step(o->rotation[0],mul(dt,o->speed_134));
            ni_rotation_track_step(o->rotation[1],mul(dt,o->speed_134));
        }
        if(o->present_2d&&o->sw.on)update_light(n);
        else release_light(n);
        return;
    }
    if(o->present_2d&&o->enabled_2c&&o->kind==6&&o->sw.on&&o->asset)update_beam(n);   // 0023a6f4 (+2d, +2c, !+2e)
    const auto children=o->children;
    for(const int c:children)update_node(c,dt);                          // child vt +110
}

void NiEventWorld::update_beam(int n){
    const auto& o=nodes_[static_cast<std::size_t>(n)];
    const std::uint32_t id=o.id;
    if(!host_.beam_asset_bounds||!host_.beam_player||!host_.beam_hit||!host_.beam)
        fail("0023a6f4: the kind-6 beam of "+hex(id)+" without hosted asset bounds 0023a250 / player slots 003fb8d0 / hit volumes 00188140 / BeamMsg 003c9420");
    if(!(o.node_rows[0]&&o.node_rows[1]&&o.node_rows[2]&&o.node_rows[3]))
        fail("00239ff0: the beam of "+hex(id)+" reads its node world ("+o.node_reason+")");
    const NiBeam segment=ni_beam_segment(host_.beam_asset_bounds(id),o.node);   // sp+1c0, sp+1b0
    for(int slot=0;slot<4;++slot){                                       // 0023a77c..0023afc4
        const auto player=host_.beam_player(slot);
        if(!player)continue;
        std::array<float,16> node{};
        for(std::size_t r=0;r<4;++r)
            for(std::size_t c=0;c<4;++c)node[r*4+c]=player->node[r][c];
        const auto inverse=hotbox_rigid_inverse(node);                  // 0013eca0(sp+150, node+10)
        NiMatrix inv{};
        for(std::size_t r=0;r<4;++r)
            for(std::size_t c=0;c<4;++c)inv[r][c]=inverse[r*4+c];
        const RootVector p0=ni_point_product(inv,segment.start);
        const RootVector p1=ni_point_product(inv,segment.end);
        if(!ni_beam_box_hit(player->box,p0,p1))continue;
        RootVector direction{};
        for(std::size_t j=0;j<4;++j)direction[j]=sub(segment.end[j],segment.start[j]);   // vsub.xyzw
        const std::uint32_t flags=host_.beam_hit(player->actor,segment.start,direction);
        if(flags==0)continue;                                            // 0023af10
        Message m{};
        m.node=n;
        m.beam=true;
        m.actor=player->actor;
        m.flags=flags;
        m.byte_20=true;                                                  // 0023af4c: *005721c8 == 0 (single player)
        queue_.push_back(m);                                             // 003c9420(msg, nie, 0)
    }
}

namespace {
// Light vcall +28 (00144b10): vsub.xyzw and the MAC zero flags.
bool moved(const ShadowVector& from,const RootVector& to){
    bool any=false;
    for(std::size_t i=0;i<4;++i){
        if(!std::isfinite(from[i])||!std::isfinite(to[i]))fail("light position not finite");
        const float d=from[i]-to[i];
        if(std::fpclassify(d)==FP_SUBNORMAL)fail("denormal position difference (VU zero flag not established)");
        if(d==0.0F&&std::signbit(d))fail("-0 position difference (VU sign flag not established)");
        if(d!=0.0F)any=true;
    }
    return any;
}
}

void NiEventWorld::update_light(int n){
    auto& o=nodes_[static_cast<std::size_t>(n)];
    if(!o.node_rows[3])fail("the light position: "+o.node_reason);
    const RootVector pos=o.node[3];                                      // the node object's +40
    if(!host_.lights)fail("the light manager 00535d20 (00373e70) is not hosted");
    auto& pool=host_.lights->point;
    if(!o.light_140){                                                    // 00237d10
        for(std::size_t i=0;i<pool.size();++i){                          // 00373e70(00535d20, 4)
            if(pool[i].id!=-1)continue;
            pool[i].id=static_cast<std::int32_t>(i+1);
            o.light_140=i;
            break;
        }
        if(!o.light_140)return;                                          // the pool is full: no light
        auto& l=pool[*o.light_140];
        l.intensity=0.0F;                                                // 00179540(light, 4, colour, pos, 3)
        l.enabled=false;
        l.aux=3;
        l.type=4;
        l.colour={o.colour[0],o.colour[1],o.colour[2],0.0F};             // 00178b80(.., 0.0, +144)
        l.position=pos;
        const float sum=add(add(mul(l.colour[0],l.colour[0]),mul(l.colour[1],l.colour[1])),mul(l.colour[2],l.colour[2]));
        l.weight=fdiv(1.0F,sum);
        l.vtable=level_light_vtable_point;                               // 004d9200
        l.inner=o.rotation[0].value;                                     // 00179530: +10 = +170
        l.outer=o.rotation[1].value;                                     // 00179520: +14 = +190
        l.enabled=true;                                                  // vcall +2c: 00144b80
        host_.lights->dirty_42=1;                                        // (and 00535d62)
        if(o.byte_151)l.aux|=8;
        if(o.byte_150)l.aux|=0x10;
        lights_.push_back({o.id,true,*o.light_140,pos,l.inner,l.outer});
        return;
    }
    if(*o.light_140>=pool.size())fail("light index outside the point pool");
    auto& l=pool[*o.light_140];                                          // 00237e00
    if(moved(l.position,pos)){l.position=pos;host_.lights->dirty_42=1;}   // vcall +28: 00144b10
    l.inner=o.rotation[0].value;
    l.outer=o.rotation[1].value;
}

void NiEventWorld::release_light(int n){
    auto& o=nodes_[static_cast<std::size_t>(n)];
    if(!o.light_140)return;                                              // 00237e50
    if(!host_.lights||*o.light_140>=host_.lights->point.size())fail("the released light is outside the point pool");
    auto& l=host_.lights->point[*o.light_140];
    if(l.enabled)host_.lights->dirty_42=1;                               // vcall +30: 00144980 (00535d62)
    l.enabled=false;
    l.id=-1;                                                             // vcall +8(1): 00144a60
    l.vtable=level_light_vtable_released;
    lights_.push_back({o.id,false,*o.light_140,l.position,l.inner,l.outer});
    o.light_140.reset();
}

bool NiEventWorld::set_present(std::uint32_t id,bool present){
    int n=find(id);
    if(n<0)return false;
    while(nodes_[static_cast<std::size_t>(n)].parent>=0)n=nodes_[static_cast<std::size_t>(n)].parent;   // 0023c100
    const std::function<void(int)> walk=[&](int k){                      // 0023c010 / 0023bee0
        const auto& self=nodes_[static_cast<std::size_t>(k)];
        if(!self.light&&self.kind==2&&self.asset&&host_.particle_present)
            host_.particle_present(self.id,present);                     // asset vt +5c / +60
        for(const int c:nodes_[static_cast<std::size_t>(k)].children)walk(c);
        nodes_[static_cast<std::size_t>(k)].present_2d=present;          // 003ce980
    };
    walk(n);
    return true;
}

void NiEventWorld::set_enabled(std::uint32_t id,bool enable){
    const int n=need(id,enable?"Enable 002378d0":"Disable 002378a0");
    const std::function<void(int)> walk=[&](int k){                      // 0023bd40 / 0023bcc0
        auto& o=nodes_[static_cast<std::size_t>(k)];
        if(o.light)return;                                               // NIL vt +28 / +2c: 0012ed30 / 0012ed40 (jr ra)
        o.enabled_2c=enable;                                             // sb +2c
        const auto children=o.children;                                  // +a4[0..+a0)
        for(const int c:children)walk(c);                                // child vt +28 / +2c (-1)
    };
    if(nodes_[static_cast<std::size_t>(n)].light)fail("Enable / Disable(NIEventOBJ) on NIEventLight "+hex(id));
    walk(n);
}

bool NiEventWorld::enabled(std::uint32_t id) const{return nodes_[static_cast<std::size_t>(need(id,"+2c"))].enabled_2c;}

bool NiEventWorld::contains(std::uint32_t id) const{return find(id)>=0;}
bool NiEventWorld::playing(std::uint32_t id) const{return nodes_[static_cast<std::size_t>(need(id,"playing"))].playing_130;}
bool NiEventWorld::present(std::uint32_t id) const{return nodes_[static_cast<std::size_t>(need(id,"present"))].present_2d;}
bool NiEventWorld::switched_on(std::uint32_t id) const{return nodes_[static_cast<std::size_t>(need(id,"switched_on"))].sw.on;}
float NiEventWorld::event_time(std::uint32_t id) const{return nodes_[static_cast<std::size_t>(need(id,"event_time"))].list.time;}
std::optional<std::size_t> NiEventWorld::light_index(std::uint32_t id) const{return nodes_[static_cast<std::size_t>(need(id,"light_index"))].light_140;}

RootVector NiEventWorld::node_position(std::uint32_t id) const{
    const auto& o=nodes_[static_cast<std::size_t>(need(id,"node_position"))];
    if(!o.node_rows[3])fail("the position of "+hex(id)+": "+o.node_reason);
    return o.node[3];
}

std::optional<NiMatrix> NiEventWorld::node_world(std::uint32_t id) const{
    const auto& o=nodes_[static_cast<std::size_t>(need(id,"node_world"))];
    for(std::size_t r=0;r<4;++r)
        if(!o.node_rows[r])return std::nullopt;
    return o.node;
}

std::optional<std::uint32_t> NiEventWorld::parent_id(std::uint32_t id) const{
    const auto& o=nodes_[static_cast<std::size_t>(need(id,"+94"))];
    if(o.parent<0)return std::nullopt;
    return nodes_[static_cast<std::size_t>(o.parent)].id;
}

float NiEventWorld::rotation_value(std::uint32_t id,int axis) const{
    const auto& o=nodes_[static_cast<std::size_t>(need(id,"+144"))];
    if(o.light&&axis>1)fail("the rotation track "+std::to_string(axis)+" of NIEventLight "+hex(id)+" (+170 / +190 only)");
    if(axis<0||axis>2)fail("rotation axis "+std::to_string(axis));
    return o.rotation[static_cast<std::size_t>(axis)].value;
}

void NiEventWorld::set_speed(std::uint32_t id,float speed){
    const int n=need(id,"0023e3d0");
    std::vector<int> stack{n};
    while(!stack.empty()){                                               // +134, then each +9c child (0023e444 ..)
        const int k=stack.back();stack.pop_back();
        auto& o=nodes_[static_cast<std::size_t>(k)];
        o.speed_134=speed;
        for(auto c=o.children.rbegin();c!=o.children.rend();++c)stack.push_back(*c);
    }
}

void NiEventWorld::set_local_rows(std::uint32_t id,const std::array<RootVector,3>& rows){
    const int n=need(id,"the +f0 rows (00286eb0)");
    auto& o=nodes_[static_cast<std::size_t>(n)];
    if(o.light)fail("the tank writes the +f0 rows of NIEventLight "+hex(id)+" (the NIL vtable 004dbed0's +120 is not ported)");
    for(std::size_t r=0;r<3;++r){o.local[r]=rows[r];o.local_rows[r]=true;}
    node_update(n);                                                      // vt +120 = 0023a2d0 -> 0023dda0
}

std::string NiEventWorld::node_reason(std::uint32_t id) const{return nodes_[static_cast<std::size_t>(need(id,"node_reason"))].node_reason;}

std::uint32_t NiEventWorld::kind(std::uint32_t id) const{return nodes_[static_cast<std::size_t>(need(id,"kind"))].kind;}

std::optional<NiMatrix> NiEventWorld::particle_world(std::uint32_t id) const{
    const auto& o=nodes_[static_cast<std::size_t>(need(id,"particle_world"))];
    if(o.light||o.kind!=2||!o.asset||!o.present_2d||!o.enabled_2c||!o.sw.on)return std::nullopt;   // 0013e250 / 00239fa0
    for(std::size_t r=0;r<4;++r)
        if(!o.node_rows[r])fail("the particle world of "+hex(id)+": "+o.node_reason);
    return o.node;
}

std::optional<NiModelDraw> NiEventWorld::model_draw(std::uint32_t id) const{
    const auto& o=nodes_[static_cast<std::size_t>(need(id,"model_draw"))];
    if(o.light||(o.kind!=1&&o.kind!=3&&o.kind!=6))fail("model_draw: "+hex(id)+" is not a kind-1 / kind-3 / kind-6 NIE (0023c180)");
    if(!o.asset||!o.present_2d||!o.enabled_2c||!o.sw.on)return std::nullopt;   // 0013e264 +2d / +2c; 0023a250 +dc / +140
    NiModelDraw d;
    if(o.kind==3&&!o.node_rows[0]&&o.node_reason==absent_kind3_world){
        d.stale=true;
        return d;
    }
    for(std::size_t r=0;r<4;++r)
        if(!o.node_rows[r])fail("the model world of "+hex(id)+": "+o.node_reason);
    d.cull=o.node;                                                       // 0013e2e4: the NIE's +50, matrix +10
    if(o.asset_matrix)d.matrix=o.node;                                   // 0023a2d0: asset vt +94 / +ac (NIE +50 +10)
    return d;
}

bool NiEventWorld::has_asset(std::uint32_t id) const{
    const auto& o=nodes_[static_cast<std::size_t>(need(id,"0023c180"))];
    if(o.light||!ni_kind_has_part(o.kind))fail("0023c180: "+hex(id)+" is not a kind-1 / kind-3 / kind-6 NIE");
    return o.asset;
}

std::optional<NiMatrix> NiEventWorld::asset_world(std::uint32_t id) const{
    const auto& o=nodes_[static_cast<std::size_t>(need(id,"0027a370"))];
    if(o.light||!ni_kind_has_part(o.kind)||!o.asset)fail("0027a370: "+hex(id)+" has no asset +140");
    if(!o.asset_matrix)return std::nullopt;                              // 003cdf20: no component (world space)
    // 0023de10 does not write an absent kind-3 NIE's world, so the component still holds the last one it wrote.
    if(o.kind==3&&!o.node_rows[0]&&o.node_reason==absent_kind3_world&&o.node_written)return *o.node_written;
    for(std::size_t r=0;r<4;++r)
        if(!o.node_rows[r])fail("0027a370: the asset world of "+hex(id)+": "+o.node_reason);
    return o.node;                                                       // 0023a2d0: asset vt +94 / +ac
}

RootVector NiEventWorld::position(std::uint32_t id) const{
    const auto& o=nodes_[static_cast<std::size_t>(need(id,"003cd260"))];
    if(!o.light&&o.sw.on&&o.asset&&o.present_2d&&o.enabled_2c){
        // 003cd260: +88 (0023a250 -> the asset's vt+88; +dc, +140, +2d, +2c, !+2e); with bounds, their centre
        // through the +50 world.
        if((o.kind!=1&&o.kind!=3)||!host_.asset_bounds)
            fail("the position of "+hex(id)+": the asset bounds 0023a250 are not hosted");
        if(const auto b=host_.asset_bounds(o.id)){
            for(std::size_t r=0;r<4;++r)
                if(!o.node_rows[r])fail("the position of "+hex(id)+": "+o.node_reason);
            return object_position_e4(&o.node,b);
        }
    }
    return node_position(id);
}

std::vector<std::uint32_t> NiEventWorld::ids() const{
    std::vector<std::uint32_t> r;
    for(const auto& n:nodes_)r.push_back(n.id);
    return r;
}

std::vector<NiSoundRecord> NiEventWorld::take_sounds(){return std::exchange(sounds_,{});}
std::vector<NiNoticeRecord> NiEventWorld::take_notices(){return std::exchange(notices_,{});}
std::vector<NiLightRecord> NiEventWorld::take_lights(){return std::exchange(lights_,{});}
std::vector<NiBlastRecord> NiEventWorld::take_blasts(){return std::exchange(blasts_,{});}
std::vector<NiBeamRecord> NiEventWorld::take_beams(){return std::exchange(beams_,{});}

}
