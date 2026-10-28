#include "backpack_object.hpp"
#include "game_libm.hpp"
#include "hotbox_world.hpp"
#include <bit>
#include <cmath>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
[[noreturn]] void fail(const std::string& what){throw std::runtime_error("Backpack object: "+what);}
// The EE FPU as the interpreter models it: binary32 round to nearest, a
// denormal result flushed to a zero of the same sign; nonfinite values (which
// the EE FPU cannot hold) throw.
float finite(float v){
    if(!std::isfinite(v))fail("a nonfinite float (the EE FPU has no infinities or NaNs)");
    return std::fpclassify(v)==FP_SUBNORMAL?(std::signbit(v)?-0.0F:0.0F):v;
}
float add(float a,float b){return finite(finite(a)+finite(b));}
float sub(float a,float b){return finite(finite(a)-finite(b));}
float mul(float a,float b){return finite(finite(a)*finite(b));}
float bits_float(std::uint32_t v){return std::bit_cast<float>(v);}
std::uint32_t float_bits(float v){return std::bit_cast<std::uint32_t>(v);}
bool active(const std::array<std::uint8_t,3>& root){return root[1]!=0&&root[0]!=0&&root[2]==0;}
void event(std::vector<BackpackObjectEvent>& events,BackpackObjectEvent::Kind kind,std::uint32_t a=0,std::uint32_t b=0,
    std::uint32_t c=0){
    BackpackObjectEvent e;e.kind=kind;e.a=a;e.b=b;e.c=c;events.push_back(e);
}
// The valid owner of +300 / +304: +300 non-null and its +c equal to +304.
std::optional<WeaponBackpackOwner> valid_owner(const WeaponBackpack& bp,const BackpackObjectHost& host){
    const std::uint32_t a=bp.word(0x300);
    if(a==0)return std::nullopt;
    if(!host.owner)fail("the owner actor's view is not hosted");
    const auto o=host.owner(a);
    if(static_cast<std::uint32_t>(o.key_c)!=bp.word(0x304))return std::nullopt;
    return o;
}
// 001a3730: !+3468 && !+3469 && !004704f0(+3528).
bool no_pickup_items(const WeaponBackpackOwner& o){
    if(o.pickupable_3468!=0||o.byte_3469!=0)return false;
    return !o.items_3528;
}
WeaponBackpack& object_at(const BackpackObjectHost& host,std::uint32_t address){
    if(!host.object)fail("the list's objects are not hosted");
    return host.object(address);
}
// Multiplayer (132fx5): *00494178, made by 002ebaf8 on first use.
std::uint32_t local_index(const BackpackObjectHost& host){
    if(!host.local_index)fail("the local client index (*00494178 / 002ebaf8) is not hosted");
    return host.local_index();
}
}

std::uint32_t BackpackTransform::word(std::size_t offset) const{
    if(offset+4>bytes.size())fail("transform access outside the 0x60-byte component");
    return static_cast<std::uint32_t>(bytes[offset])|static_cast<std::uint32_t>(bytes[offset+1])<<8|
        static_cast<std::uint32_t>(bytes[offset+2])<<16|static_cast<std::uint32_t>(bytes[offset+3])<<24;
}
void BackpackTransform::set_word(std::size_t offset,std::uint32_t v){
    if(offset+4>bytes.size())fail("transform access outside the 0x60-byte component");
    for(int k=0;k<4;++k)bytes[offset+k]=static_cast<std::uint8_t>(v>>(8*k));
}
void BackpackTransform::set_byte(std::size_t offset,std::uint8_t v){
    if(offset>=bytes.size())fail("transform access outside the 0x60-byte component");
    bytes[offset]=v;
}

void backpack_object_cap_check(const BackpackObjectList& list,const BackpackObjectHost& host,
    std::vector<BackpackObjectEvent>& events){
    std::int32_t count=0;
    for(const auto address:list.data){
        const auto& e=object_at(host,address);
        if(e.byte(0x60)!=0)continue;
        if(e.value(0x30c)<2)continue;   // slti 2
        const auto owner=valid_owner(e,host);
        if(!owner||no_pickup_items(*owner))++count;
    }
    if(static_cast<std::int32_t>(backpack_object_cap)>=count)return;
    // 00257b7c: retire one unattached backpack that is not already fading. In a network game (132fx5) only the
    // local client's own (+308 = *00494178, read once here).
    const std::uint32_t me=host.multiplayer?local_index(host):0;
    // Unlike the count, both loops skip the +60 test.
    auto eligible=[&](const WeaponBackpack& e){
        if(host.multiplayer&&e.word(0x308)!=me)return false;
        const auto owner=valid_owner(e,host);
        return (!owner||no_pickup_items(*owner))&&e.byte(0x2f2)==0&&finite(bits_float(e.word(0x314)))<0.0F;
    };
    std::int32_t s5=0;   // the highest +30c (slt s5, +30c)
    for(const auto address:list.data){
        const auto& e=object_at(host,address);
        if(eligible(e)&&s5<e.value(0x30c))s5=e.value(0x30c);
    }
    if(s5<2)return;
    // Among those with that +30c, the oldest by its creation clock +310
    // (the first strictly below the running minimum, from 1e7 = 0x4b189680).
    float f20=bits_float(0x4b189680U);
    std::optional<std::uint32_t> s3;
    for(const auto address:list.data){
        const auto& e=object_at(host,address);
        if(!eligible(e)||e.value(0x30c)!=s5)continue;
        const float t=finite(bits_float(e.word(0x310)));
        if(t<f20){f20=t;s3=address;}
    }
    if(!s3)fail("the eviction 00257b7c found no backpack created before 1e7 (it stores through a null s3)");
    auto& bp=object_at(host,*s3);
    bp.set_byte(0x2f2,1);
    bp.set_word(0x314,0);   // the tick 002568f0 now ages it; the draw fades it out
    if(bp.word(0x2fc)!=0){
        const std::uint32_t box=bp.word(0x2fc);
        event(events,BackpackObjectEvent::Kind::remove_box,box);
        event(events,BackpackObjectEvent::Kind::unregister,box);
        event(events,BackpackObjectEvent::Kind::release,box,backpack_release_delay);
        bp.set_word(0x2fc,0);
    }
}

std::array<float,16> backpack_drop_matrix(const BackpackDropActor& actor){
    // First-use statics 004eeda4 = 0, 004eeda0 = pi/2 (0x3fc90fdb), 004eeda8 = 0.
    const float a=0.0F,b=bits_float(0x3fc90fdbU),c=0.0F;
    const float f24=game_cosf(a),f23=game_sinf(a);   // 00140e80 / 00140e70
    const float f22=game_cosf(b),f21=game_sinf(b);
    const float f20=game_cosf(c),f0=game_sinf(c);
    const float s0=mul(f22,f24);
    const float f2=mul(f0,f24);
    const float f1=mul(f0,f23);
    const float f13=mul(-f0,f22);
    const float f18=mul(-f22,f23);
    const float f4=mul(f20,f24);
    const float f12=sub(f4,mul(f1,f21));
    const float f17=sub(f1,mul(f4,f21));
    const float f3=mul(f20,f23);
    const float f14=add(f3,mul(f2,f21));
    const float f16=mul(f20,f22);
    const float f15=add(f2,mul(f3,f21));
    const float f19=f21;
    // 00140e20: the rows (f12 f15 f18 0) (f13 f16 f19 0) (f14 f17 s0 0) (0 0 0 1).
    std::array<float,16> m{f12,f15,f18,0.0F, f13,f16,f19,0.0F, f14,f17,s0,0.0F, 0.0F,0.0F,0.0F,1.0F};
    // The actor's virtual +f4 matrix: row 3 is taken (the qword at +30),
    // then the active test +2d / +2c / !+2e.
    for(std::size_t k=0;k<4;++k)m[12+k]=actor.matrix_f4[12+k];
    if(!active(actor.view.root)&&actor.spawned_3598&&actor.controller_5c.value_or(false)&&actor.block_3584_44)
        for(std::size_t k=0;k<3;++k)m[12+k]=(*actor.block_3584_44)[k];
    return m;
}

void backpack_object_set_owner(WeaponBackpack& bp,std::uint32_t bp_address,std::uint32_t owner,
    const std::optional<WeaponBackpackOwner>& owner_view,const BackpackObjectHost& host,
    std::vector<BackpackObjectEvent>& events){
    if(owner!=0&&!owner_view)fail("002566b0 without the owner's view");
    bp.set_word(0x300,owner);
    bp.set_word(0x304,owner?static_cast<std::uint32_t>(owner_view->key_c):0xffffffffU);
    const bool valid=bp.word(0x300)!=0&&static_cast<std::uint32_t>(owner_view->key_c)==bp.word(0x304);
    bool need=valid&&active(owner_view->root)&&owner_view->pickupable_3468!=0;
    if(!need)need=valid&&active(owner_view->root)&&owner_view->byte_3469!=0;
    if(!need)need=owner!=0&&owner_view->items_3528;   // 004704f0(a1 +3528)
    if(!need)return;
    if(bp.word(0x2fc)!=0)return;
    // vtable +5c = 003ce980(bp, 1).
    bp.set_byte(0x2d,1);
    if(bp.word(0x58)!=0&&bp.byte(0x2c)!=0)fail("003ce980 with a +58 component (its virtual +48) is not ported");
    event(events,BackpackObjectEvent::Kind::register_object,bp_address);
    if(bp.word(0x2fc)!=0)return;
    if(!host.create_box)fail("the hotbox 00170e20 is not hosted");
    const std::uint32_t box=host.create_box(bp_address);
    event(events,BackpackObjectEvent::Kind::create_box,box);
    bp.set_word(0x2fc,box);
    event(events,BackpackObjectEvent::Kind::box_word,box,0x94,bp.word(0xc));
}

namespace {
// 00171050(*004ffbd0, box, bp, 0): 001710a0(out, bp, 0) -- the transform's
// vt+58 00132900 when its byte +50 is set, the translation +40, the bounds of
// vt+88 00253fb0, every lane halved -- then box +70 / +80.
void update_box(BackpackTransform& t,std::uint32_t box,const BackpackObjectHost& host,std::vector<BackpackObjectEvent>& events){
    if(!host.model_bounds||!host.set_box_bounds)fail("the box update 00171050 (vt+88 00253fb0, box +70 / +80) is not hosted");
    if(t.bytes[0x50]!=0)t.set_byte(0x50,0);
    const RootVector p{bits_float(t.word(0x40)),bits_float(t.word(0x44)),bits_float(t.word(0x48)),bits_float(t.word(0x4c))};
    const auto box_88=host.model_bounds();
    const auto b=npc_interactable_bounds(p,box_88[0],box_88[1],false);
    BackpackObjectEvent e;e.kind=BackpackObjectEvent::Kind::box_bounds;e.a=box;e.origin=b.minimum;e.displacement=b.maximum;
    events.push_back(e);
    host.set_box_bounds(box,b.minimum,b.maximum);
}
}

// 002561d0(bp, m, 1).
void backpack_object_place(WeaponBackpack& bp,std::uint32_t bp_address,BackpackTransform& t,const std::array<float,16>& m,
    const BackpackObjectHost& host,std::vector<BackpackObjectEvent>& events){
    for(std::size_t k=0;k<16;++k)t.set_word(0x10+4*k,float_bits(m[k]));   // T virtual +c = 00131c10
    const RootVector p{m[12],m[13],m[14],m[15]};
    if(!host.containing_region||!host.nearest_region)fail("the regions 00224ac0 / 002248a0 are not hosted");
    BackpackObjectEvent contain;contain.kind=BackpackObjectEvent::Kind::contain;contain.origin=p;events.push_back(contain);
    auto region=host.containing_region(p);
    bp.set_word(0x2f8,region?region->handle:0);
    if(bp.word(0x2f8)==0){
        BackpackObjectEvent near;near.kind=BackpackObjectEvent::Kind::nearest;near.origin=p;events.push_back(near);
        region=host.nearest_region(p);
        if(!region||region->handle==0)fail("002561d0 reads the root bytes of a null region (002248a0 found none)");
        bp.set_word(0x2f8,region->handle);
    }
    bp.set_byte(0x2f4,active(region->root)?0:1);
    // The downward line of sight: 003c9e30(request, 005435a0), vtable
    // 004d9180, origin = m row 3 with w = 1, displacement (0, 0, -3, 0), +50 = bp.
    event(events,BackpackObjectEvent::Kind::los_begin);
    BackpackObjectEvent los;los.kind=BackpackObjectEvent::Kind::los;
    los.origin={m[12],m[13],m[14],1.0F};
    los.displacement={0.0F,0.0F,bits_float(0xc0400000U),0.0F};
    events.push_back(los);
    if(!host.line_of_sight)fail("the placement's line of sight (003caa50, virtual +c) is not hosted");
    const auto hit=host.line_of_sight(los.origin,los.displacement);
    if(hit&&hit->class_4c==host.game_class_5468&&hit->type_68==3){
        // 002563f8..00256444: the handle 00256630(bp) = (bp, bp +c) into the lift's +84 unless found there.
        if(!host.attach_rider)fail("a backpack dropped onto a lift: its rider vector (00256630 .. 002564a0) is not hosted");
        event(events,BackpackObjectEvent::Kind::attach_rider,hit->object,bp_address,bp.word(0xc));
        host.attach_rider(hit->object,bp_address,bp.word(0xc));
    }
    event(events,BackpackObjectEvent::Kind::los_end);
    if(bp.word(0x2fc)!=0)update_box(t,bp.word(0x2fc),host,events);   // 00256460..00256474
}

void backpack_object_move_a8(WeaponBackpack& bp,BackpackTransform& t,const RootVector& v,
    const BackpackObjectHost& host,std::vector<BackpackObjectEvent>& events){
    if(bp.word(0x50)!=0){
        // 001bc330: vadd.xyz on the component's +40 (its matrix row 3).
        for(std::size_t k=0;k<3;++k)t.set_word(0x40+4*k,float_bits(add(bits_float(t.word(0x40+4*k)),v[k])));
    }
    if(bp.word(0x2fc)!=0)update_box(t,bp.word(0x2fc),host,events);   // 00256684..00256698
}

std::uint32_t backpack_object_receive_transform(WeaponBackpack& bp,BackpackTransform& t,std::uint8_t mode_24,
    const std::array<float,16>& matrix_30){
    // 003cea68: no transform component answers 0.
    if(bp.word(0x50)==0)return 0;
    if(mode_24!=3)fail("003cea10: a transform message (*0055d4e0) of mode "+std::to_string(mode_24)+" is not ported");
    // 003ceb88..003cebfc: refresh (vt+58 = 00132900 clears byte +50), +40 + row 3 (vadd.xyzw), vt+14 = 001b4960.
    if(t.bytes[0x50]!=0)t.set_byte(0x50,0);
    for(std::size_t k=0;k<4;++k)t.set_word(0x40+4*k,float_bits(add(bits_float(t.word(0x40+4*k)),matrix_30[12+k])));
    // 003cece0..: the +54 component copy and the +58 component's vt+50.
    if(bp.word(0x54)!=0)fail("003cea10: the +54 component copy (003ced30) is not ported");
    if(bp.word(0x58)!=0)fail("003cea10: the +58 component's virtual +50 (003cedac) is not ported");
    return 1;
}

std::uint32_t backpack_object_construct(const BackpackObjectCreate& create,const BackpackDropActor& actor,
    BackpackObjectList& list,const BackpackObjectHost& host,std::vector<BackpackObjectEvent>& events,
    const WeaponDatabase& database){
    if(!create.object||!create.transform)fail("00257640 without its allocations");
    backpack_object_cap_check(list,host,events);
    event(events,BackpackObjectEvent::Kind::alloc_object,create.address);
    auto& bp=*create.object;
    // 0040c240(&id, 0xf5), then id |= (local + 1) << 24: the local client in a network game (132fx5), else 0.
    const std::uint32_t id=create.network_id.value_or(
        create.id_40c240|((host.multiplayer?local_index(host):0U)+1U)<<24);
    // 003cb090(bp, &id).
    bp.set_word(0x0,0x004dfde0);bp.set_word(0x4,0xbebaafde);bp.set_word(0x8,0x00543650);bp.set_word(0xc,id);
    bp.set_word(0x10,0xffffffffU);bp.set_byte(0x14,1);bp.set_word(0x18,0);bp.set_word(0x0,0x004dfda0);
    bp.set_word(0x20,0);bp.set_word(0x24,create.address+0x24);bp.set_word(0x28,create.address+0x24);   // 00139070(bp + 20)
    // 003cf0e0(bp, &id, 3, &0).
    bp.set_word(0x0,0x004dff50);
    bp.set_byte(0x2c,1);bp.set_byte(0x2d,0);bp.set_byte(0x2e,0);bp.set_byte(0x2f,0);
    bp.set_word(0x30,0xffffffffU);bp.set_word(0x38,0x80);bp.set_word(0x3c,0);bp.set_word(0x40,0);bp.set_word(0x44,0);
    bp.set_byte(0x48,0);bp.set_byte(0x49,3);bp.set_word(0x4c,0);
    bp.set_word(0x50,0);bp.set_word(0x54,0);bp.set_word(0x58,0);bp.set_word(0x5c,0);
    bp.set_word(0x10,id);
    bp.set_word(0x0,backpack_object_vtable);
    bp.set_word(0x304,0xfffffffeU);bp.set_word(0x300,0);bp.set_byte(0x2f,1);
    std::vector<WeaponBackpackEvent> reset_events;
    const WeaponBackpackHost reset_host;
    WeaponBackpackOps(bp,database,reset_host,reset_events).reset(false,WeaponInventory{});   // 00258330(bp, 0)
    // Virtual +90 = 003cdff0(bp, 0055c9f0, 0): +50 is 0, so the 0x60-byte
    // component (vtable 004df6a0, +10..+4f a copy of *0055c9f0, dirty +50)
    // is made. The copied matrix is overwritten by the placement's 00131c10
    // before anything reads it (an actor is required below), so it is not kept.
    event(events,BackpackObjectEvent::Kind::alloc_transform,create.transform_address);
    auto& t=*create.transform;
    t.set_word(0x0,backpack_transform_vtable);
    t.set_byte(0x50,1);
    bp.set_word(0x50,create.transform_address);
    bp.set_byte(0x48,1);
    bp.set_byte(0x2f0,1);bp.set_byte(0x60,0);bp.set_word(0x310,create.clock_4f7f70);
    list.data.push_back(create.address);   // 00257620
    if(actor.address==0)fail("00257640 without an actor (the identity placement 00257820) is not ported");
    const auto m=backpack_drop_matrix(actor);
    backpack_object_place(bp,create.address,t,m,host,events);
    backpack_object_set_owner(bp,create.address,actor.address,actor.view,host,events);
    if(host.multiplayer){
        // 00257878..002579ac (132fx5): +308 = the local client, +10 = +c, +14 = 0 unless this console is the
        // session host (*005721c0), then NetMap_BindReceiver(+10, the owner: the local client on the session
        // host, else 002ebba0's session host, bp).
        bp.set_word(0x308,create.network_owner.value_or(local_index(host)));
        bp.set_word(0x10,bp.word(0xc));
        if(!host.hosting)bp.set_byte(0x14,0);
        std::uint32_t owner=0;
        if(create.network_owner)owner=*create.network_owner;
        else if(host.hosting)owner=local_index(host);
        else{
            if(!host.session_host)fail("002ebba0 (the session host's index) is not hosted");
            owner=host.session_host();
        }
        event(events,BackpackObjectEvent::Kind::net_bind,bp.word(0x10),owner,create.address);
    }
    return create.address;
}

void backpack_object_hide(WeaponBackpack& bp,std::vector<BackpackObjectEvent>& events){
    const std::uint32_t box=bp.word(0x2fc);
    if(box==0)return;
    event(events,BackpackObjectEvent::Kind::remove_box,box);              // 00170db0(*004ffbd0, box)
    event(events,BackpackObjectEvent::Kind::unregister,box);              // 0040c9a0
    event(events,BackpackObjectEvent::Kind::release,box,backpack_release_delay);   // 003cb230(box, 0.1)
    bp.set_word(0x2fc,0);
}

void backpack_object_turn_on(WeaponBackpack& bp,std::uint32_t bp_address,BackpackTransform& transform,
    const BackpackDropActor& actor,const BackpackObjectHost& host,std::vector<BackpackObjectEvent>& events){
    const auto m=backpack_drop_matrix(actor);                             // 001a3268 001a32a0
    backpack_object_place(bp,bp_address,transform,m,host,events);         // 001a327c 002561d0(bp, m, 1)
    if(bp.word(0x2fc)!=0)return;                                          // 00256180
    if(!host.create_box)fail("the hotbox 00170e20 is not hosted");
    const std::uint32_t box=host.create_box(bp_address);
    event(events,BackpackObjectEvent::Kind::create_box,box);
    bp.set_word(0x2fc,box);
    event(events,BackpackObjectEvent::Kind::box_word,box,0x94,bp.word(0xc));
}

std::uint32_t backpack_inv_record_size(std::uint32_t version){return version>=0x39?0x54U:0x48U;}

namespace {
// Virtual +5c = 003ce980(bp, 1).
void activate(WeaponBackpack& bp){
    bp.set_byte(0x2d,1);
    if(bp.word(0x58)!=0&&bp.byte(0x2c)!=0)fail("003ce980 with a +58 component (its virtual +48) is not ported");
}
// The box block of 00257f30: virtual +5c, 0040c9d0, then 00170e20 when
// the registration made no box.
void inv_box_block(WeaponBackpack& bp,std::uint32_t address,const BackpackInvHost& host,
    std::vector<BackpackObjectEvent>& events){
    activate(bp);
    event(events,BackpackObjectEvent::Kind::register_object,address);
    if(bp.word(0x2fc)!=0)return;
    if(!host.create_box)fail("the hotbox 00170e20 is not hosted");
    const std::uint32_t box=host.create_box(address);
    event(events,BackpackObjectEvent::Kind::create_box,box);
    bp.set_word(0x2fc,box);
    event(events,BackpackObjectEvent::Kind::box_word,box,0x94,bp.word(0xc));
}
}

std::uint32_t backpack_object_construct_inv(const BackpackObjectCreate& create,const BackpackInvRecord& record,
    const BackpackInvHost& host,std::vector<BackpackObjectEvent>& events,const WeaponDatabase& database){
    if(!create.object||!create.transform)fail("00257f30 without its allocations");
    auto& bp=*create.object;
    // cGOBJ_ctor 003cf0e0(bp, &(-1), 0, 004f7f68) with 003cb090(bp, &(-1)).
    bp.set_word(0x0,0x004dfde0);bp.set_word(0x4,0xbebaafde);bp.set_word(0x8,0x00543650);bp.set_word(0xc,0xffffffffU);
    bp.set_word(0x10,0xffffffffU);bp.set_byte(0x14,1);bp.set_word(0x18,0);bp.set_word(0x0,0x004dfda0);
    bp.set_word(0x20,0);bp.set_word(0x24,create.address+0x24);bp.set_word(0x28,create.address+0x24);
    bp.set_word(0x0,0x004dff50);
    bp.set_byte(0x2c,1);bp.set_byte(0x2d,0);bp.set_byte(0x2e,0);bp.set_byte(0x2f,0);
    bp.set_word(0x30,0xffffffffU);bp.set_word(0x38,0x80);bp.set_word(0x3c,0);bp.set_word(0x40,0);bp.set_word(0x44,0);
    bp.set_byte(0x48,0);bp.set_byte(0x49,0);bp.set_word(0x4c,backpack_gobj_class_4f7f68);
    bp.set_word(0x50,0);bp.set_word(0x54,0);bp.set_word(0x58,0);bp.set_word(0x5c,0);
    bp.set_word(0x10,0xffffffffU);
    bp.set_word(0x0,backpack_object_vtable);
    bp.set_word(0x304,0xfffffffeU);bp.set_word(0x300,0);
    std::vector<WeaponBackpackEvent> reset_events;
    const WeaponBackpackHost reset_host;
    WeaponBackpackOps(bp,database,reset_host,reset_events).reset(false,WeaponInventory{});   // 00258330(bp, 0)
    // Virtual +90 = 003cdff0(bp, 0055c9f0, 0): the 0x60-byte component
    // (vtable 004df6a0) with +10..+4f a copy of the identity *0055c9f0 and
    // the dirty byte +50.
    event(events,BackpackObjectEvent::Kind::alloc_transform,create.transform_address);
    auto& t=*create.transform;
    t.set_word(0x0,backpack_transform_vtable);
    for(std::size_t k=0;k<16;++k)t.set_word(0x10+4*k,k%5==0?0x3f800000U:0U);
    t.set_byte(0x50,1);
    bp.set_word(0x50,create.transform_address);
    bp.set_byte(0x48,1);
    bp.set_word(0xc,record.id[0]<<16|record.id[1]);
    // T virtual +14 = 001b4960: the quadword (x, y, z, 1.0) to +40.
    for(std::size_t k=0;k<3;++k)t.set_word(0x40+4*k,record.position[k]);
    t.set_word(0x4c,0x3f800000U);
    for(const auto& slot:record.slots){
        if(slot.item<0)continue;   // bltz s7
        const auto& rec=database.record(slot.item);   // WeaponDb_Get 00147840
        std::int32_t amount=slot.amount,copies=slot.copies;
        if(!(amount>0)&&rec.clip_70()!=0)continue;
        const std::uint8_t hold=rec.hold_class_f0();
        if(hold==1||hold==7){
            amount=static_cast<std::int32_t>(static_cast<std::uint32_t>(amount)*static_cast<std::uint32_t>(copies));   // mult
            copies=1;
        }
        for(std::int32_t i=0;i<copies;++i){
            if(record.version<0x39)
                fail("00257f30 below version 0x39 passes the caller's stale $fp as the slot flag to 00255af0");
            const std::int32_t clip=rec.clip_70();
            const std::int32_t spare=static_cast<std::int32_t>(static_cast<std::uint32_t>(amount-1)*static_cast<std::uint32_t>(clip));
            BackpackObjectEvent e;e.kind=BackpackObjectEvent::Kind::add_weapon;
            e.a=static_cast<std::uint32_t>(slot.item);e.b=static_cast<std::uint32_t>(spare);
            e.c=static_cast<std::uint32_t>(clip);e.d=slot.flag;events.push_back(e);
            if(!host.add_weapon)fail("00255af0 (the crate's slot add) is not hosted");
            host.add_weapon(bp,slot.item,spare,clip,slot.flag);
        }
    }
    const std::int32_t armor=static_cast<std::int32_t>(record.armor[0]),armor_count=static_cast<std::int32_t>(record.armor[1]);
    if(armor_count>0&&armor!=0){
        const std::uint32_t n=bp.word(0x2e4);
        bp.set_word(0x2e4,n+1);
        if(n==0){bp.set_word(0x2e8,record.armor[0]);bp.set_word(0x2ec,record.armor[0]);}
        if(bp.word(0x2fc)==0)inv_box_block(bp,create.address,host,events);
    }
    const std::uint32_t o0=record.owner[0],o1=record.owner[1];
    if(o1!=0xffffffffU&&o0!=0xffffffffU){
        event(events,BackpackObjectEvent::Kind::find_collision,o0,o1);
        if(!host.find_collision)fail("IdMgr_FindCollision 0040c480 is not hosted");
        const std::uint32_t zone=host.find_collision(o0,o1);
        if(zone!=0){
            BackpackObjectEvent e;e.kind=BackpackObjectEvent::Kind::zone_associate;
            e.a=zone;e.b=create.address;e.c=1;e.d=0;events.push_back(e);
        }else{
            activate(bp);
        }
    }
    bp.set_word(0x2f8,0);
    bp.set_byte(0x2f0,0);
    bp.set_byte(0x60,1);
    if(bp.word(0x2fc)==0)inv_box_block(bp,create.address,host,events);
    bp.set_word(0x30c,0);
    bp.set_word(0x310,0);
    return create.address;
}

void backpack_object_destroy(std::uint32_t bp_address,BackpackObjectList& list,const BackpackObjectHost& host,
    std::vector<BackpackObjectEvent>& events){
    auto& bp=object_at(host,bp_address);
    if(valid_owner(bp,host))event(events,BackpackObjectEvent::Kind::clear_owner_3464,bp.word(0x300));
    if(bp.byte(0x2f3)==0&&host.multiplayer&&bp.word(0x308)==local_index(host)){
        // 002547d4..00254834 (132fx5): the local client's own drop sends InventoryMsg {+20 4, +24 1, +28 bp,
        // +2c -1} (Event_Send(msg, bp, 0)).
        BackpackObjectEvent e;e.kind=BackpackObjectEvent::Kind::inventory_send;
        e.a=1;e.b=bp_address;e.c=0xffffffffU;e.d=bp_address;events.push_back(e);
    }
    if(bp.word(0x2fc)!=0){
        const std::uint32_t box=bp.word(0x2fc);
        event(events,BackpackObjectEvent::Kind::remove_box,box);
        event(events,BackpackObjectEvent::Kind::unregister,box);
        event(events,BackpackObjectEvent::Kind::release,box,backpack_release_delay);
        bp.set_word(0x2fc,0);
    }
    event(events,BackpackObjectEvent::Kind::unregister,bp_address);
    std::int64_t last=static_cast<std::int64_t>(list.data.size())-1;
    for(std::int64_t i=0;i<=last;++i){
        if(list.data[static_cast<std::size_t>(i)]!=bp_address)continue;
        list.data[static_cast<std::size_t>(i)]=list.data[static_cast<std::size_t>(last)];
        --last;
    }
    list.data.resize(static_cast<std::size_t>(last+1));   // 001bac40(list, a2 + 1, &0)
    event(events,BackpackObjectEvent::Kind::release,bp_address,backpack_release_delay);
}

void backpack_object_tick(float dt,std::uint32_t& clock_4f7f70,BackpackObjectList& list,const BackpackObjectHost& host,
    std::vector<BackpackObjectEvent>& events){
    clock_4f7f70=float_bits(add(bits_float(clock_4f7f70),dt));
    for(std::int64_t i=0;i<static_cast<std::int64_t>(list.data.size());++i){
        const std::uint32_t address=list.data[static_cast<std::size_t>(i)];
        auto& e=object_at(host,address);
        const float age=bits_float(e.word(0x314));
        if(finite(age)<0.0F)continue;   // c.olt.s
        e.set_word(0x314,float_bits(add(age,dt)));
        if(bits_float(e.word(0x314))<=bits_float(backpack_object_lifetime))continue;   // c.ole.s *0048af18
        backpack_object_destroy(address,list,host,events);
        --i;
    }
}

std::uint32_t BackpackCrateSnapshot::word(std::size_t offset) const{
    if(offset+4>bytes.size())throw std::out_of_range("crate snapshot word past 0x290");
    return static_cast<std::uint32_t>(bytes[offset])|static_cast<std::uint32_t>(bytes[offset+1])<<8|
           static_cast<std::uint32_t>(bytes[offset+2])<<16|static_cast<std::uint32_t>(bytes[offset+3])<<24;
}

void backpack_crate_snapshot(const BackpackObjectList& list,const BackpackObjectHost& host,
    std::vector<BackpackCrateSnapshot>& snapshots){
    snapshots.clear();   // 002569c0
    for(const std::uint32_t address:list.data){   // 00256b68..00256d30 (the count +4 reread per entry)
        const auto& bp=object_at(host,address);
        if(bp.byte(0x60)!=1)continue;
        BackpackCrateSnapshot s;
        const std::uint32_t id=bp.word(0xc);   // 00256c38
        for(std::size_t k=0;k<4;++k)s.bytes[k]=static_cast<std::uint8_t>(id>>(8*k));
        // +280..+28c from +2e0..+2ec (00256c50..00256ca0), +4..+280 from +64..+2e0 (00256cac..00256d0c).
        for(std::size_t o=0;o<0x27c+0x10;++o)s.bytes[4+o]=bp.byte(0x64+o);
        snapshots.push_back(s);   // 00256d50(004f7f48, &obj)
    }
}

void backpack_crate_restore(const BackpackObjectList& list,const BackpackObjectHost& host,
    const std::vector<BackpackCrateSnapshot>& snapshots,std::vector<BackpackObjectEvent>& events){
    for(const auto& s:snapshots){   // the 004f7f48 walk (00256db8 .. 00256f88)
        for(std::size_t i=0;i<list.data.size();++i){   // 00256de0 .. 00256f58 (the count +4 reread)
            const std::uint32_t address=list.data[i];
            auto& bp=object_at(host,address);
            if(bp.word(0xc)!=s.word(0))continue;   // 00256e00
            for(std::size_t o=0;o<0x10;++o)bp.set_byte(0x2e0+o,s.bytes[0x280+o]);   // 00256e10..00256e7c
            for(std::size_t o=0;o<0x27c;++o)bp.set_byte(0x64+o,s.bytes[4+o]);       // 00256e94..00256eec
            if(bp.word(0x2fc)!=0)continue;   // 00256f04 / 00256f1c
            if(!host.create_box)fail("the hotbox 00170e20 (00256f30) is not hosted");
            const std::uint32_t box=host.create_box(address);   // 00170e20(*004ffbd0, bp, 0, 1)
            event(events,BackpackObjectEvent::Kind::create_box,box);
            bp.set_word(0x2fc,box);
            event(events,BackpackObjectEvent::Kind::box_word,box,0x94,bp.word(0xc));
        }
    }
}

void backpack_remove_drops(BackpackObjectList& list,const BackpackObjectHost& host,
    std::vector<BackpackObjectEvent>& events){
    for(std::int64_t i=0;i<static_cast<std::int64_t>(list.data.size());++i){   // 002570d8..0025711c
        const std::uint32_t address=list.data[static_cast<std::size_t>(i)];
        if(object_at(host,address).byte(0x60)!=0)continue;
        backpack_object_destroy(address,list,host,events);
        --i;
    }
}

}
