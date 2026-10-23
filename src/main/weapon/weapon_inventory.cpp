#include "weapon_inventory.hpp"
#include <bit>
#include "water_effects.hpp"
#include <stdexcept>

namespace omega {
namespace {
constexpr std::array<std::uint8_t,8> holster_points{3,4,2,5,8,9,6,7};   // 00489d70
constexpr std::array<std::uint8_t,8> hand_points{0,1,0,0,0,0,6,0};      // 00489d78
std::uint8_t checked_slot(std::uint8_t slot){
    if(slot>=8)throw std::runtime_error("Weapon inventory: slot outside the 8-slot array");
    return slot;
}
// The EE's wrapping 32-bit multiply (mult rd: the low word).
std::int32_t wrap_mul(std::int32_t a,std::int32_t b){
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a)*static_cast<std::uint32_t>(b));
}
}

std::uint8_t weapon_holster_point(std::uint8_t slot){return holster_points[checked_slot(slot)];}
std::uint8_t weapon_hand_point(std::uint8_t slot){return hand_points[checked_slot(slot)];}

WeaponInventoryOps::WeaponInventoryOps(WeaponInventory& inventory,const WeaponDatabase& database,const WeaponInventoryHost& host,
    std::vector<WeaponInventoryEvent>& events):inv_(inventory),db_(database),host_(host),events_(events){}

WeaponInventory WeaponInventoryOps::construct(std::uint64_t actor,bool with_object,const WeaponDatabase& database,
    const WeaponInventoryHost& host,std::vector<WeaponInventoryEvent>& events){
    WeaponInventory inv;
    for(std::size_t k=0;k<8;++k)inv.slots[k].byte_0=static_cast<std::uint8_t>(holster_points[k]<8?1:0);
    inv.actor_88=actor;
    if(with_object){
        inv.object_0=WeaponInventoryObject{};
        inv.object_0->owner_4=actor;
        events.push_back({WeaponInventoryEvent::Kind::object_created,0,0,0,0,{}});
        WeaponInventoryOps(inv,database,host,events).add(weapon_default_item,false,false);
    }
    return inv;
}

std::int32_t WeaponInventoryOps::clip_size(std::uint8_t slot) const{
    if(slot==weapon_no_slot)slot=inv_.current_84;
    if(slot==weapon_no_slot)return 0;
    return db_.record(inv_.slots[checked_slot(slot)].item).clip_70();
}

bool WeaponInventoryOps::full_auto() const{
    if(inv_.current_84==weapon_no_slot)return false;
    return db_.record(inv_.slots[inv_.current_84].item).full_auto_78()==1;
}

void WeaponInventoryOps::randomize_dropped_ammunition(const std::function<std::uint32_t()>& random){
    if(!random)throw std::runtime_error("Weapon inventory 00142170: the game RNG is required");
    const float two31=std::bit_cast<float>(0x4f000000U);                   // 2^31
    const float lo=std::bit_cast<float>(0x3d4ccccdU),hi=std::bit_cast<float>(0x3e99999aU);   // 00489c18 / 00489c20
    auto trunc_word=[](float v){
        if(!(v>-2147483648.0F&&v<2147483648.0F))throw std::runtime_error("Weapon inventory 00142170: an amount outside the cvt.w.s range");
        return static_cast<std::int32_t>(v);
    };
    for(std::size_t k=0;k<6;++k){
        auto& slot=inv_.slots[k];
        if(slot.item==-1)continue;
        const auto& record=db_.record(slot.item);
        const std::int32_t clips=clips_for_actor(record.clips_74());
        const std::int32_t total=static_cast<std::int32_t>(static_cast<std::uint32_t>(clips)*static_cast<std::uint32_t>(record.clip_70()));   // mult (lo)
        const float r=game_int_to_float(static_cast<std::int32_t>(random()))/two31;
        float fraction=r*(hi-lo);
        fraction=fraction+lo;
        if(total<10)fraction=fraction*2.0F;
        std::int32_t amount=trunc_word(game_int_to_float(total)*fraction);
        if(record.hold_class_f0()==1){slot.spare=amount;slot.clip=0;continue;}
        const std::int32_t size=record.clip_70();
        if(size<amount){slot.spare=amount-size;amount=amount-slot.spare;}
        else slot.spare=0;
        slot.clip=amount;
        if(slot.spare>0){
            const float r2=game_int_to_float(static_cast<std::int32_t>(random()))/two31;
            const std::int32_t moved=trunc_word(game_int_to_float(size)*r2);
            slot.clip=slot.clip-moved;
            slot.spare=slot.spare+moved;
        }
    }
}

std::int32_t WeaponInventoryOps::clips_for_actor(std::int32_t clips){
    if(!host_.player)return clips;
    event({WeaponInventoryEvent::Kind::agent_unlock_query,5,0,0,0,{}});
    if(!host_.increased_ammo)return clips;
    // cvt.s.w, mul.s by 1.2 (0048a2d0), cvt.w.s (truncation).
    const float scaled=game_int_to_float(clips)*1.2F;
    const auto t=static_cast<std::int32_t>(scaled);
    return t==clips?t+1:t;
}

void WeaponInventoryOps::add(std::int32_t item,bool give_ammo,bool preload){
    const auto& record=db_.record(item);
    std::uint8_t slot=weapon_slot_of_class(record.weapon_class_30());
    if(preload)event({WeaponInventoryEvent::Kind::preload,item,0,0,0,{}});
    // The dual rule: a second copy of the slot-0 item goes to slot 1.
    if(inv_.slots[slot].item!=-1&&slot==0&&inv_.slots[0].item==item&&inv_.slots[1].item==-1)slot=1;
    auto& s=inv_.slots[slot];
    s.item=item;
    if(give_ammo){
        s.clip=record.clip_70();
        const auto clips=clips_for_actor(record.clips_74());
        s.spare=wrap_mul(record.clip_70(),clips);
    }else{s.clip=0;s.spare=0;}
    std::uint8_t point=slot==inv_.current_84?hand_points[slot]:holster_points[slot];
    if(slot==1&&inv_.current_84==0)point=1;
    if(record.model_13c()!=0)event({WeaponInventoryEvent::Kind::attach,item,point,0,record.model_13c(),{}});
    if(slot==6||slot==7)event({WeaponInventoryEvent::Kind::clear_attach_24a2,item,point,0,0,{}});
    if(record.hold_class_f0()==5&&inv_.object_0)inv_.object_0->record_0=item;
}

bool WeaponInventoryOps::switch_allowed() const{
    if(inv_.busy_ef!=0||inv_.reloading_a0||inv_.burst_bc[0]!=0||inv_.burst_bc[1]!=0)return false;
    return !inv_.object_0||inv_.object_0->state_8==0;
}

void WeaponInventoryOps::holster(std::uint8_t slot){
    checked_slot(slot);
    if(slot==0&&inv_.slots[1].item!=-1)holster(1);
    event({WeaponInventoryEvent::Kind::move_attach,0,hand_points[slot],holster_points[slot],0,{}});
    inv_.current_84=weapon_no_slot;inv_.previous_85=slot;inv_.selection_a4=-1;
}

void WeaponInventoryOps::holster_current(){
    if(!switch_allowed())return;
    if(inv_.current_84!=weapon_no_slot)holster(inv_.current_84);
    else inv_.previous_85=weapon_no_slot;
}

std::int32_t weapon_active_item(const WeaponInventory& inventory){
    if(inventory.current_84==weapon_no_slot)return -1;                     // 001855f8
    if(inventory.current_84>=inventory.slots.size())
        throw std::runtime_error("GetActiveWeapon 001855c0: held slot "+std::to_string(inventory.current_84)+" past the slot array");
    return inventory.slots[inventory.current_84].item;                      // 00185600: lw +8 + 16 * slot
}

void WeaponInventoryOps::force_holster(){
    if(inv_.current_84!=weapon_no_slot)holster_current();
}

bool WeaponInventoryOps::equip(std::uint8_t slot){
    checked_slot(slot);
    if(inv_.current_84==slot)return false;
    holster_current();
    if(slot==0&&inv_.slots[1].item!=-1)equip(1);
    if(host_.player&&host_.byte_14&&slot!=1){
        if(!host_.position_c0)throw std::runtime_error("Weapon inventory: the switch sound needs the actor's +c0 position");
        event({WeaponInventoryEvent::Kind::switch_sound,0,0,0,0,host_.position_c0()});
    }
    inv_.current_84=slot;
    const auto from=holster_points[slot],to=hand_points[slot];
    if(!host_.attached)throw std::runtime_error("Weapon inventory: equip needs the actor's model attachment table");
    const bool moved=host_.attached(from);
    if(moved)event({WeaponInventoryEvent::Kind::move_attach,0,from,to,0,{}});
    inv_.selection_a4=-1;
    return moved;
}

void WeaponInventoryOps::equip_first(){
    for(std::uint8_t k=0;k<6;++k)
        if(inv_.slots[k].item!=-1){equip(k);return;}
}

void WeaponInventoryOps::cycle(bool holster_at_end){
    if(!switch_allowed())return;
    const std::uint8_t cur=inv_.current_84;
    if(cur==weapon_no_slot){equip_first();return;}
    const int stop=holster_at_end?6:cur;                                       // t0
    int k=cur+1;                                                               // t1
    for(;;){
        if(k==stop)break;
        const bool occupied=k<8&&inv_.slots[static_cast<std::size_t>(k)].item!=-1;
        if(k!=1&&occupied&&k!=6)break;
        ++k;
        if(!holster_at_end&&k>=6)k=0;
    }
    if(k!=cur&&k!=6){equip(static_cast<std::uint8_t>(k));return;}
    if(holster_at_end)holster(cur);
}

bool WeaponInventoryOps::equip_item(std::int32_t item){
    if(!switch_allowed())return false;
    if(item==-1){holster_current();return true;}
    const std::uint8_t slot=slot_of_item(item);
    if(inv_.slots[checked_slot(slot)].item!=item)return false;
    equip(slot);
    return inv_.slots[slot].item==item;
}

void WeaponInventoryOps::remove(std::uint8_t slot,bool unload){
    checked_slot(slot);
    auto& s=inv_.slots[slot];
    s.spare=0;s.clip=0;
    std::uint8_t point;
    if(slot==inv_.current_84||(slot==1&&inv_.current_84==0)){
        point=hand_points[slot];
        if(slot==inv_.current_84)inv_.current_84=weapon_no_slot;
    }else point=holster_points[slot];
    event({WeaponInventoryEvent::Kind::detach,s.item,point,0,0,{}});
    if(unload&&db_.record(s.item).keep_loaded_139()==0)event({WeaponInventoryEvent::Kind::unload,s.item,0,0,0,{}});
    s.item=-1;
}

void WeaponInventoryOps::clear(){
    for(std::uint8_t k=0;k<6;++k)
        if(inv_.slots[k].item!=-1)remove(k,true);
}

std::optional<std::size_t> WeaponInventoryOps::held_record() const{
    if(inv_.current_84==weapon_no_slot)return std::nullopt;
    return db_.index(inv_.slots[checked_slot(inv_.current_84)].item);
}

std::int32_t WeaponInventoryOps::spare_room(std::uint8_t slot){
    const auto& s=inv_.slots[checked_slot(slot)];
    const auto& r=db_.record(s.item);
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(wrap_mul(r.clip_70(),clips_for_actor(r.clips_74())))-
        static_cast<std::uint32_t>(s.spare));
}

std::int32_t WeaponInventoryOps::set_clip(std::uint8_t slot,std::int32_t rounds){
    inv_.slots[checked_slot(slot)].clip=rounds;
    return 1;
}

std::int32_t WeaponInventoryOps::add_spare(std::uint8_t slot,std::int32_t rounds,bool no_limit){
    const std::int32_t room=spare_room(slot);
    const std::int32_t added=(rounds<room||no_limit)?rounds:room;
    auto& s=inv_.slots[checked_slot(slot)];
    s.spare=static_cast<std::int32_t>(static_cast<std::uint32_t>(s.spare)+static_cast<std::uint32_t>(added));
    return added;
}

bool WeaponInventoryOps::same_ammunition(std::size_t a,std::size_t b) const{
    if(a==b)return true;
    const auto& ra=db_.record(static_cast<std::int32_t>(a));
    const auto& rb=db_.record(static_cast<std::int32_t>(b));
    return ra.ammo_type_10c()==rb.ammo_type_10c()&&ra.hold_class_f0()!=1&&rb.hold_class_f0()!=1;
}

std::int32_t WeaponInventoryOps::add_ammunition(std::int32_t item,std::int32_t rounds){
    const std::size_t record=db_.index(item);
    std::int32_t total=0;
    if(const auto held=held_record();held&&same_ammunition(*held,record)){
        // lb +84: the held slot as a signed byte, masked again by 001429d0.
        total=add_spare(inv_.current_84,rounds,false);
        rounds-=total;
    }
    for(std::uint8_t k=0;rounds!=0&&k<6;++k){
        if(k==inv_.current_84||inv_.slots[k].item==-1)continue;
        if(!same_ammunition(db_.index(inv_.slots[k].item),record))continue;
        const std::int32_t added=add_spare(k,rounds,false);
        total+=added;rounds-=added;
    }
    share_dual();
    return total;
}

void WeaponInventoryOps::share_dual(){
    auto& a=inv_.slots[0];auto& b=inv_.slots[1];
    if(b.item==-1||a.item==-1)return;
    const auto half=[](std::int32_t x,std::int32_t y){
        return static_cast<std::int32_t>(static_cast<std::uint32_t>(x)+static_cast<std::uint32_t>(y)+1U)/2;};
    const std::int32_t spare=half(b.spare,a.spare),clip=half(b.clip,a.clip);
    a.clip=clip;b.clip=clip;a.spare=spare;b.spare=spare;
}

std::uint8_t WeaponInventoryOps::slot_of_item(std::int32_t item) const{
    return weapon_slot_of_class(db_.record(item).weapon_class_30());
}

void WeaponInventoryOps::set_player_weapons(std::int32_t first,std::int32_t second){
    clear();
    if(first>=0)add(first,true,true);
    if(second>=0){
        add(second,true,true);
        if(first<0)first=second;
    }
    if(first>=0)equip(slot_of_item(first));
}

std::int32_t WeaponInventoryOps::spare(std::uint8_t slot) const{
    if(slot==weapon_no_slot)slot=inv_.current_84;
    if(slot==weapon_no_slot)return 0;
    checked_slot(slot);
    auto value=inv_.slots[slot].spare;
    if(slot==0&&inv_.slots[1].item!=-1)value=static_cast<std::int32_t>(static_cast<std::uint32_t>(value)+static_cast<std::uint32_t>(inv_.slots[1].spare));
    return value;
}

std::int32_t WeaponInventoryOps::clip(std::uint8_t slot) const{
    if(slot==weapon_no_slot)slot=inv_.current_84;
    if(slot==weapon_no_slot)return 0;
    checked_slot(slot);
    auto value=inv_.slots[slot].clip;
    if(slot==0&&inv_.slots[1].item!=-1)value=static_cast<std::int32_t>(static_cast<std::uint32_t>(value)+static_cast<std::uint32_t>(inv_.slots[1].clip));
    return value;
}

bool WeaponInventoryOps::spare_of_ammo_type(std::int32_t type) const{
    for(std::size_t k=0;k<6;++k){
        const auto& s=inv_.slots[k];
        if(s.item==-1)continue;
        if(db_.record(s.item).ammo_type_10c()!=type)continue;
        if(s.spare>0)return true;
    }
    return false;
}

bool WeaponInventoryOps::reload_allowed() const{
    const auto cur=inv_.current_84;
    if(cur==weapon_no_slot)return false;
    if(inv_.reloading_a0||inv_.burst_bc[0]!=0||inv_.burst_bc[1]!=0)return false;
    const auto item=inv_.slots[checked_slot(cur)].item;
    if(item==-1)return false;
    const auto& record=db_.record(item);
    std::int32_t rounds=clip(weapon_no_slot);
    if(inv_.slots[1].item!=-1&&cur==0)rounds/=2;                               // sra with the rounding of a signed division
    if(record.full_auto_78()==2)return false;
    if(!(rounds<record.clip_70()))return false;
    return spare_of_ammo_type(record.ammo_type_10c());
}

bool WeaponInventoryOps::ready(bool continuation) const{
    const auto hand=inv_.hand_a9;
    if(hand>1)throw std::runtime_error("Weapon inventory: active hand outside 0/1");
    if(inv_.reloading_a0)return false;
    if(!(inv_.cooldown_ac[hand]<=0.0F))return false;         // c.ole.s: NaN is not ready
    if(!(clip(weapon_no_slot)>0))return false;
    if(!continuation&&inv_.burst_bc[hand]!=0)return false;
    return inv_.spinup_cc[hand]==0.0F;
}

}
