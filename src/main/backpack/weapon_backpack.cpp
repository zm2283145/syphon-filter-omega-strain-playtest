#include "weapon_backpack.hpp"
#include <bit>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
[[noreturn]] void fail(const std::string& what){throw std::runtime_error("Weapon backpack: "+what);}
void check(std::size_t offset,std::size_t n){
    if(offset+n>weapon_backpack_size||offset>weapon_backpack_size)
        fail("field access outside the 0x318-byte object (the original has no bounds check)");
}
std::uint32_t add32(std::int32_t a,std::int32_t b){return static_cast<std::uint32_t>(a)+static_cast<std::uint32_t>(b);}
bool active(const WeaponBackpackOwner& o){return o.root[1]!=0&&o.root[0]!=0&&o.root[2]==0;}
}

std::uint32_t WeaponBackpack::word(std::size_t offset) const{
    check(offset,4);
    return static_cast<std::uint32_t>(bytes[offset])|static_cast<std::uint32_t>(bytes[offset+1])<<8|
        static_cast<std::uint32_t>(bytes[offset+2])<<16|static_cast<std::uint32_t>(bytes[offset+3])<<24;
}
void WeaponBackpack::set_word(std::size_t offset,std::uint32_t v){
    check(offset,4);
    for(int k=0;k<4;++k)bytes[offset+k]=static_cast<std::uint8_t>(v>>(8*k));
}
std::uint8_t WeaponBackpack::byte(std::size_t offset) const{check(offset,1);return bytes[offset];}
void WeaponBackpack::set_byte(std::size_t offset,std::uint8_t v){check(offset,1);bytes[offset]=v;}
std::size_t WeaponBackpack::entry(std::size_t k,std::int64_t j){
    const std::int64_t o=static_cast<std::int64_t>(slot(k))+4+j*0x28;
    if(o<0||o>static_cast<std::int64_t>(weapon_backpack_size))fail("entry outside the 0x318-byte object");
    return static_cast<std::size_t>(o);
}

WeaponBackpackOps::WeaponBackpackOps(WeaponBackpack& backpack,const WeaponDatabase& database,const WeaponBackpackHost& host,
    std::vector<WeaponBackpackEvent>& events):bp_(backpack),db_(database),host_(host),events_(events){}

std::int8_t WeaponBackpackOps::slot_of(std::int32_t item) const{
    // 00143d20: lb from the 00489d60 table (values 0..7).
    return static_cast<std::int8_t>(weapon_slot_of_class(db_.record(item).weapon_class_30()));
}

void WeaponBackpackOps::reset(bool keep,const WeaponInventory& player){
    for(std::size_t k=0;k<3;++k){
        const std::size_t s=WeaponBackpack::slot(k);
        bool clear=!keep;
        if(keep){
            const std::uint8_t index=bp_.byte(s);
            if(index>=player.slots.size())fail("00258330 reads a player inventory slot outside the 8 slots");
            clear=player.slots[index].item>=0;
        }
        if(clear){bp_.set_word(s+0xcc,0);bp_.set_byte(s+0xd0,0);}
    }
    if(!keep){bp_.set_word(0x2fc,0);bp_.set_word(0x30c,2);}
    bp_.set_word(0x2e0,3);bp_.set_word(0x2e4,0);bp_.set_byte(0x2f2,0);bp_.set_word(0x300,0);
    bp_.set_word(0x304,0xffffffffU);bp_.set_byte(0x2f1,0);bp_.set_byte(0x2f3,0);bp_.set_word(0x314,0xbf800000U);
    bp_.set_word(0x308,0);bp_.set_byte(0x2f4,0);
}

void WeaponBackpackOps::ensure_box(){
    if(bp_.word(0x2fc)!=0)return;
    if(!host_.create_box){
        if(host_.box_not_simulated){event({WeaponBackpackEvent::Kind::box_not_simulated});return;}
        fail("the hotbox 00170e20 is not hosted (its bounds 00253fb0 are the gearpack model's +3e0 box, "
             "computed by VU0 micro code at model load 0038d720)");
    }
    event({WeaponBackpackEvent::Kind::activate_5c});
    event({WeaponBackpackEvent::Kind::register_40c9d0});
    if(bp_.word(0x2fc)!=0)return;
    const std::uint32_t box=host_.create_box();
    event({WeaponBackpackEvent::Kind::create_box,box});
    bp_.set_word(0x2fc,box);
    event({WeaponBackpackEvent::Kind::box_word,box,0x94,bp_.word(0xc)});
}

void WeaponBackpackOps::add_armor(std::int32_t armor,std::int32_t max_armor){
    if(armor==0)return;
    const std::int32_t n=bp_.value(0x2e4);
    bp_.set_word(0x2e4,add32(n,1));
    if(n==0){bp_.set_word(0x2e8,static_cast<std::uint32_t>(armor));bp_.set_word(0x2ec,static_cast<std::uint32_t>(max_armor));}
    ensure_box();
}

bool WeaponBackpackOps::take_armor(std::int32_t& armor,std::int32_t& max_armor){
    const std::int32_t n=bp_.value(0x2e4);
    if(n==0){armor=0;max_armor=0;return false;}
    bp_.set_word(0x2e4,add32(n,-1));
    armor=bp_.value(0x2e8);max_armor=bp_.value(0x2ec);
    return true;
}

void WeaponBackpackOps::copy_last_over(std::size_t k,std::int64_t j){
    const std::size_t s=WeaponBackpack::slot(k);
    const std::int32_t last=static_cast<std::int32_t>(add32(bp_.value(s+0xcc),-1));
    bp_.set_word(s+0xcc,static_cast<std::uint32_t>(last));
    const std::size_t from=WeaponBackpack::entry(k,last),to=WeaponBackpack::entry(k,j);
    for(std::size_t w=0;w<0x28;w+=4)bp_.set_word(to+w,bp_.word(from+w));
}

void WeaponBackpackOps::add_weapon(std::int32_t item,std::int32_t spare,std::int32_t clip,std::uint8_t flag){
    const std::int8_t raw=slot_of(item);
    const std::uint8_t slot=static_cast<std::uint8_t>(raw);
    if(slot==4&&spare==0)return;
    int k=-1;
    for(int s=0;s<3;++s){
        const std::size_t o=WeaponBackpack::slot(s);
        if(bp_.word(o+0xcc)!=0||bp_.byte(o+0xd0)!=0){
            if(bp_.byte(o)==slot){k=s;break;}
        }else if(k<0)k=s;
    }
    if(k>=0){
        const std::size_t o=WeaponBackpack::slot(k);
        bool found=false;
        std::int64_t j=0;
        for(;!found&&j<bp_.value(o+0xcc);++j){
            const std::size_t e=WeaponBackpack::entry(k,j);
            if(bp_.value(e+0x20)!=item)continue;
            if(slot==4)bp_.set_word(e,add32(bp_.value(e),spare));
            else{
                const std::int64_t n=bp_.value(e+0x24);
                bp_.set_word(WeaponBackpack::entry(k,j)+static_cast<std::size_t>(4*n),static_cast<std::uint32_t>(spare));
                bp_.set_word(WeaponBackpack::entry(k,j)+static_cast<std::size_t>(4*bp_.value(e+0x24))+0x10,static_cast<std::uint32_t>(clip));
                bp_.set_word(e+0x24,add32(bp_.value(e+0x24),1));
            }
            found=true;
        }
        if(!found){
            const std::size_t e=WeaponBackpack::entry(k,j);
            bp_.set_word(e+0x20,static_cast<std::uint32_t>(item));
            bp_.set_word(e+0x10,static_cast<std::uint32_t>(clip));
            bp_.set_word(e,static_cast<std::uint32_t>(spare));
            bp_.set_word(e+0x24,1);
            bp_.set_byte(o,static_cast<std::uint8_t>(raw));
            bp_.set_word(o+0xcc,add32(bp_.value(o+0xcc),1));
            if(flag!=0)bp_.set_byte(o+0xd0,flag);
        }
    }
    ensure_box();
}

void WeaponBackpackOps::mark_slot(std::int32_t item){
    const std::int8_t raw=slot_of(item);
    bp_.set_byte(0x134,1);
    bp_.set_byte(0x64,static_cast<std::uint8_t>(raw));
    ensure_box();
    bp_.set_byte(0x2f1,1);
    bp_.set_word(0x30c,0);
}

std::vector<std::int32_t> WeaponBackpackOps::list(const WeaponInventory& inventory) const{
    std::vector<std::int32_t> out;
    std::int32_t slot6=-1;
    for(std::size_t k=0;k<3;++k){
        const std::size_t o=WeaponBackpack::slot(k);
        if(bp_.word(o+0xcc)!=0){
            const std::int32_t item=bp_.value(o+0x24);
            if(static_cast<std::uint8_t>(slot_of(item))==6)slot6=item;
            else out.push_back(item);
        }else if(bp_.byte(o+0xd0)!=0){
            const std::uint8_t index=bp_.byte(o);
            if(index>=inventory.slots.size())fail("002553e0 reads an inventory slot outside the 8 slots");
            const std::int32_t v=inventory.slots[index].item;
            if(v!=-1)out.push_back(v);
        }
    }
    if(slot6>=0&&(out.empty()||inventory.slots[6].item!=slot6))out.push_back(slot6);
    return out;
}

std::optional<WeaponBackpackAmmo> WeaponBackpackOps::ammunition(std::int32_t item) const{
    const std::uint8_t slot=static_cast<std::uint8_t>(slot_of(item));
    std::optional<WeaponBackpackAmmo> r;
    for(std::size_t k=0;k<3;++k){
        const std::size_t o=WeaponBackpack::slot(k);
        if(bp_.byte(o)!=slot)continue;
        for(std::int64_t j=0;j<bp_.value(o+0xcc);++j){
            const std::size_t e=WeaponBackpack::entry(k,j);
            if(bp_.value(e+0x20)!=item)continue;
            WeaponBackpackAmmo a;
            const std::int64_t n=bp_.value(e+0x24);
            a.spare=bp_.value(static_cast<std::size_t>(static_cast<std::int64_t>(e)+4*n-4));
            a.clip=bp_.value(static_cast<std::size_t>(static_cast<std::int64_t>(e)+4*n+0xc));
            std::uint32_t total=0;
            for(std::int64_t i=0;i<bp_.value(e+0x24);++i){
                total+=bp_.word(e+static_cast<std::size_t>(4*i));
                if(slot!=4)total+=bp_.word(e+0x10+static_cast<std::size_t>(4*i));
            }
            a.total=static_cast<std::int32_t>(total);
            r=a;
        }
    }
    return r;
}

bool WeaponBackpackOps::take(std::int32_t item,bool test_only){
    const std::uint8_t slot=static_cast<std::uint8_t>(slot_of(item));
    for(std::size_t k=0;k<3;++k){
        const std::size_t o=WeaponBackpack::slot(k);
        if(bp_.byte(o)!=slot)continue;
        for(std::int64_t j=0;j<bp_.value(o+0xcc);++j){
            const std::size_t e=WeaponBackpack::entry(k,j);
            if(bp_.value(e+0x20)!=item)continue;
            if(!test_only){
                const std::uint32_t n=add32(bp_.value(e+0x24),-1);
                bp_.set_word(e+0x24,n);
                if(n==0)copy_last_over(k,j);
            }
            return true;
        }
    }
    return false;
}

std::int32_t WeaponBackpackOps::remove_ammunition(std::int32_t item,std::int32_t rounds){
    const std::uint8_t slot=static_cast<std::uint8_t>(slot_of(item));
    for(std::size_t k=0;k<3;++k){
        const std::size_t o=WeaponBackpack::slot(k);
        if(bp_.byte(o)!=slot)continue;
        for(std::int64_t j=0;j<bp_.value(o+0xcc);++j){
            const std::size_t e=WeaponBackpack::entry(k,j);
            if(bp_.value(e+0x20)!=item)continue;
            const std::uint8_t hold=db_.record(item).hold_class_f0();
            std::int32_t removed=0;
            // The spare rounds first; hold classes 1 and 7 drop a pair whose
            // spare runs out, the others take the rest from the pair's clip.
            const auto take_from=[&](std::size_t a){
                std::int32_t take_n=bp_.value(a);
                if(rounds<take_n)take_n=rounds;
                rounds=static_cast<std::int32_t>(add32(rounds,-take_n));
                bp_.set_word(a,add32(bp_.value(a),-take_n));
                removed=static_cast<std::int32_t>(add32(removed,take_n));
            };
            for(std::int64_t t=0;rounds!=0&&t<bp_.value(e+0x24);++t){
                const std::size_t a=e+static_cast<std::size_t>(4*t);
                take_from(a);
                if(hold==1||hold==7){
                    if(bp_.value(a)==0){
                        const std::uint32_t n=add32(bp_.value(e+0x24),-1);
                        bp_.set_word(e+0x24,n);
                        if(n==0){copy_last_over(k,j);return removed;}
                    }
                }else take_from(a+0x10);
            }
            return removed;
        }
    }
    return 0;
}

bool WeaponBackpackOps::pickup_ammunition(WeaponInventoryOps& inventory){
    if(bp_.byte(0x2f0)==0)return false;
    bool any=false;
    std::optional<std::int32_t> total;   // the stack word 0x154(sp) of 00255290
    for(const std::int32_t item:list(inventory.inventory())){
        if(const auto a=ammunition(item))total=a->total;
        else if(!total)fail("00255290 reads an uninitialised total (no backpack entry for a listed item)");
        const std::int32_t added=inventory.add_ammunition(item,*total);
        if(added==0)continue;
        event({WeaponBackpackEvent::Kind::display_name,static_cast<std::uint32_t>(db_.index(item))});
        remove_ammunition(item,added);
        event({WeaponBackpackEvent::Kind::string,backpack_string_pickup_ammo});
        event({WeaponBackpackEvent::Kind::format,backpack_format_pickup});
        event({WeaponBackpackEvent::Kind::hud_message});
        any=true;
    }
    return any;
}

void WeaponBackpackOps::pickup(WeaponBackpackActor& actor){
    if(bp_.byte(0x2f0)!=0){
        if(!actor.inventory)fail("00254da0 without the actor's inventory +3524");
        WeaponInventoryOps& inv=*actor.inventory;
        const std::vector<std::int32_t> items=list(inv.inventory());
        if(!host_.position_c0)fail("00254da0: the actor virtual +c0 (position) is not hosted");
        const RootVector position=host_.position_c0();
        event({WeaponBackpackEvent::Kind::position_c0,0,0,0,position});
        bool sounded=false;
        const auto sound=[&](std::uint32_t descriptor){
            if(host_.multiplayer)fail("00254da0 sound send with 005721c8 set");
            event({WeaponBackpackEvent::Kind::sound,descriptor,0,0,position});};
        for(const std::int32_t item:items){
            if(!take(item,true))continue;
            const std::int8_t raw=slot_of(item);
            const std::uint8_t slot=static_cast<std::uint8_t>(raw);
            const std::int32_t held=inv.inventory().slots.at(slot).item;
            bool dual=false;
            if(slot==0&&inv.inventory().slots[1].item<0&&held==item){
                if(!host_.dual_unlock)fail("00254da0: the unlock query 00332f90 is not hosted");
                event({WeaponBackpackEvent::Kind::unlock_query});
                dual=host_.dual_unlock();
            }
            const auto a=ammunition(item);
            if(!a)fail("00255500 found no entry after 002558a0 did");
            event({WeaponBackpackEvent::Kind::display_name,static_cast<std::uint32_t>(db_.index(item))});
            const std::uint8_t target=dual?1:slot;
            if(held>=0&&!dual)continue;
            inv.add(item,false,false);
            inv.set_clip(target,a->clip);
            inv.add_spare(target,a->spare,false);
            take(item,false);
            event({WeaponBackpackEvent::Kind::string,backpack_string_pickup_wpn});
            event({WeaponBackpackEvent::Kind::format,backpack_format_pickup});
            event({WeaponBackpackEvent::Kind::hud_message});
            inv.share_dual();
            if(!sounded){sound(backpack_sound_weapon);sounded=true;}
        }
        if(pickup_ammunition(inv)){
            inv.share_dual();
            if(!sounded){sound(backpack_sound_ammo);sounded=true;}
        }
        const std::int32_t count=bp_.value(0x2e4);
        const std::int32_t best=count!=0?bp_.value(0x2e8):0;
        const std::int32_t old=actor.armor_33a0;
        if(old<best){
            const std::int32_t old_max=actor.max_armor_339c;
            std::int32_t armor=0,max_armor=0;
            if(count!=0){
                bp_.set_word(0x2e4,add32(bp_.value(0x2e4),-1));
                max_armor=bp_.value(0x2ec);armor=bp_.value(0x2e8);
            }
            actor.max_armor_339c=max_armor;
            actor.armor_33a0=armor;
            if(old!=0){
                const std::int32_t n=bp_.value(0x2e4);
                bp_.set_word(0x2e4,add32(n,1));
                if(n==0){bp_.set_word(0x2e8,static_cast<std::uint32_t>(old));bp_.set_word(0x2ec,static_cast<std::uint32_t>(old_max));}
                ensure_box();
            }
            if(!sounded)sound(backpack_sound_armor);
            event({WeaponBackpackEvent::Kind::string,backpack_string_pickup_armor});
            event({WeaponBackpackEvent::Kind::hud_message});
        }
    }
    if(bp_.byte(0x2f1)==0)finish();
}

std::optional<WeaponBackpackOwner> WeaponBackpackOps::valid_owner() const{
    const std::uint32_t owner=bp_.word(0x300);
    if(owner==0)return std::nullopt;
    if(!host_.owner)fail("the owner actor +300 is not hosted");
    const WeaponBackpackOwner o=host_.owner(owner);
    if(o.key_c!=bp_.value(0x304))return std::nullopt;
    return o;
}

void WeaponBackpackOps::box_priority(){
    const std::uint32_t box=bp_.word(0x2fc);
    if(box==0)return;
    std::uint32_t priority=0;
    if(bp_.byte(0x60)==1)priority=5;
    else if(const auto o=valid_owner()){
        if((active(*o)&&o->pickupable_3468!=0)||(active(*o)&&o->byte_3469!=0)||o->items_3528)priority=0x14;
    }
    event({WeaponBackpackEvent::Kind::box_word,box,0x9c,priority});
}

void WeaponBackpackOps::check_empty(){
    for(std::size_t k=0;k<3;++k){
        const std::size_t o=WeaponBackpack::slot(k);
        for(std::int64_t j=0;j<bp_.value(o+0xcc);++j){
            const std::size_t e=WeaponBackpack::entry(k,j);
            if(static_cast<std::uint8_t>(slot_of(bp_.value(e+0x20)))!=4)continue;
            const std::int64_t n=bp_.value(e+0x24);
            if(n!=0&&bp_.value(static_cast<std::size_t>(static_cast<std::int64_t>(e)+4*n-4))!=0)continue;
            copy_last_over(k,j);
            --j;
        }
    }
    bool empty=false;
    if(bp_.word(0x2e4)==0){
        const auto o=valid_owner();
        // 001a3730: !+3468 && !+3469 && !004704f0(+3528).
        empty=!o||(o->pickupable_3468==0&&o->byte_3469==0&&!o->items_3528);
    }
    for(std::size_t k=0;empty&&k<3;++k){
        const std::size_t o=WeaponBackpack::slot(k);
        if(bp_.byte(o+0xd0)!=0&&bp_.byte(0x2f1)==0){empty=false;continue;}
        if(bp_.word(o+0xcc)==0)continue;
        (void)db_.record(bp_.value(o+0x24));   // 00147840, result unused
        for(std::int64_t j=0;empty&&j<bp_.value(o+0xcc);++j)
            if(bp_.word(WeaponBackpack::entry(k,j)+0x24)!=0)empty=false;
    }
    if(!empty)return;
    if(bp_.byte(0x60)==1){
        const std::uint32_t box=bp_.word(0x2fc);
        if(box==0)return;
        event({WeaponBackpackEvent::Kind::remove_box,box});
        event({WeaponBackpackEvent::Kind::unregister_box,box});
        event({WeaponBackpackEvent::Kind::release_box,box});
        bp_.set_word(0x2fc,0);
    }else event({WeaponBackpackEvent::Kind::destroy});
}

void WeaponBackpackOps::finish(){
    box_priority();
    check_empty();
    bp_.set_byte(0x2f2,0);
    if(bp_.byte(0x2f1)!=0){bp_.set_byte(0x134,0);bp_.set_byte(0x2f1,0);}
}

bool WeaponBackpackOps::receive(bool pickup_message,WeaponBackpackPicker& picker,WeaponBackpackActor& actor){
    if(!pickup_message)fail("00254600: a message other than *004f5440 goes to the default receiver 003cea10 (not hosted)");
    if(!picker.present||!picker.predicate_54||picker.byte_14==0)return true;
    if(bp_.byte(0x2f3)!=0||bp_.value(0x308)==picker.index_68){
        if(std::bit_cast<float>(bp_.word(0x314))<0.0F)picker_update(picker,actor);
    }else if(host_.multiplayer)fail("002546d0: the multiplayer pickup request (005721c8) is not ported");
    return true;
}

void WeaponBackpackOps::picker_update(WeaponBackpackPicker& picker,WeaponBackpackActor& actor){
    if(picker.pickup_1bc!=0){
        picker.pickup_1bc=0;
        pickup(actor);
    }
    if(picker.confirm_17c!=0&&picker.confirm_178==0){
        if(bp_.byte(0x2f2)==0)bp_.set_byte(0x2f2,1);
        if(!host_.open_menu)
            fail("001762c0 creates the Triangle confirm menu 00253070 (HUD *004ffc2c screen +54) and no menu host is set");
        picker.confirm_178=host_.open_menu(picker.word_184>0?0U:1U);
    }
}

std::int32_t WeaponBackpackOps::cycle(std::int32_t item,bool previous) const{
    const std::uint8_t slot=static_cast<std::uint8_t>(slot_of(item));
    for(std::size_t k=0;k<3;++k){
        const std::size_t o=WeaponBackpack::slot(k);
        if(bp_.byte(o)!=slot)continue;
        const std::int32_t count=bp_.value(o+0xcc);                  // read once (0x00254ce4)
        for(std::int32_t j=0;j<count;++j){
            if(bp_.value(WeaponBackpack::entry(k,j)+0x20)!=item)continue;
            std::int32_t n=j+(previous?-1:1);
            if(n<0)n=count-1;
            else if(n>=count)n=0;
            return bp_.value(WeaponBackpack::entry(k,n)+0x20);
        }
    }
    return item;
}

bool WeaponBackpackOps::owner_state(bool d90) const{
    const auto o=valid_owner();
    return o&&active(*o)&&(d90?o->byte_3469:o->pickupable_3468)!=0;
}

std::uint8_t WeaponBackpackOps::slot_flag(std::int32_t item) const{
    const std::uint8_t slot=static_cast<std::uint8_t>(slot_of(item));
    for(std::size_t k=0;k<3;++k){
        const std::size_t o=WeaponBackpack::slot(k);
        if(bp_.byte(o)==slot)return bp_.byte(o+0xd0);
    }
    return 0;
}

}
